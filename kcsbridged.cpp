/* Copyright 2017 - 2019 Intel
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *	Unless required by applicable law or agreed to in writing, software
 *	distributed under the License is distributed on an "AS IS" BASIS,
 *	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *	See the License for the specific language governing permissions and
 *	limitations under the License.
 */

#include <getopt.h>
#include <linux/ipmi_bmc.h>

#include <CLI/CLI.hpp>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <utility>
#include <vector>

using namespace phosphor::logging;
using namespace std::chrono;

// KY: global data structure for a histogram
unsigned long long num_requests = 0;
unsigned long long num_responses = 0;
time_point<high_resolution_clock> start, end;

// histogram: for each {netfn, lun, cmd}, keep an array of counts of response
// time
// [0]: error
// [1]: 0-10ms
// [2]: 10-100ms
// [3]: -1s
// [4]: -2s
// [5]: -5s
// [6]: >5s (should not happen)
using hist_t = std::map<uint32_t, std::vector<unsigned long long>>;
hist_t histogram;

static inline void prettyPrint(const hist_t& h)
{
    std::cout << " error, 10ms, 50ms, 250ms, 1s, 5s, >5s" << std::endl;
    for (auto const& [k, v] : h)
    {
        // k is {netfn, lun, cmd}, each a byte
        std::cout << "NetFn 0x" << std::hex << (int)((k >> 16) & 0xf)
                  << " LUN 0x" << (int)((k >> 8) & 0xf) << " CMD 0x"
                  << (int)(k & 0xf) << std::dec;
        for (const auto& i : v)
        {
            std::cout << " " << i << std::endl;
        }
        std::cout << std::endl;
    }
}

static inline void incrementHist(hist_t& h, uint8_t netfn, uint8_t lun,
                                 uint8_t cmd, int num_ms)
{
    uint32_t k = netfn << 16 | lun << 8 | cmd;
    if (histogram.find(k) == histogram.end())
    {
        histogram.insert(std::pair<uint32_t, std::vector<unsigned long long>>(
            k, std::vector<unsigned long long>(7)));
    }

    auto& arr = histogram[k];

    if (num_ms < 0)
    {
        ++arr[0];
    }
    else if (num_ms < 10)
    {
        ++arr[1];
    }
    else if (num_ms < 50)
    {
        ++arr[2];
    }
    else if (num_ms < 250)
    {
        ++arr[3];
    }
    else if (num_ms < 1000)
    {
        ++arr[4];
    }
    else if (num_ms < 5000)
    {
        ++arr[5];
    }
    else
    {
        ++arr[6];
    }
}

namespace
{
namespace io_control
{
struct ClearSmsAttention
{
    // Get the name of the IO control command.
    int name() const
    {
        return static_cast<int>(IPMI_BMC_IOCTL_CLEAR_SMS_ATN);
    }

    // Get the address of the command data.
    boost::asio::detail::ioctl_arg_type* data()
    {
        return nullptr;
    }
};

struct SetSmsAttention
{
    // Get the name of the IO control command.
    int name() const
    {
        return static_cast<int>(IPMI_BMC_IOCTL_SET_SMS_ATN);
    }

    // Get the address of the command data.
    boost::asio::detail::ioctl_arg_type* data()
    {
        return nullptr;
    }
};

struct ForceAbort
{
    // Get the name of the IO control command.
    int name() const
    {
        return static_cast<int>(IPMI_BMC_IOCTL_FORCE_ABORT);
    }

    // Get the address of the command data.
    boost::asio::detail::ioctl_arg_type* data()
    {
        return nullptr;
    }
};
} // namespace io_control

class SmsChannel
{
  public:
    static constexpr size_t kcsMessageSize = 256;
    static constexpr uint8_t netFnShift = 2;
    static constexpr uint8_t lunMask = (1 << netFnShift) - 1;

    SmsChannel(std::shared_ptr<boost::asio::io_context>& io,
               std::shared_ptr<sdbusplus::asio::connection>& bus,
               const std::string& channel, bool verbose) :
        io(io),
        bus(bus), verbose(verbose)
    {
        static constexpr const char devBase[] = "/dev/";
        std::string devName = devBase + channel;
        // open device
        int fd = open(devName.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
        {
            log<level::ERR>("Couldn't open SMS channel O_RDWR",
                            entry("FILENAME=%s", devName.c_str()),
                            entry("ERROR=%s", strerror(errno)));
            return;
        }
        else
        {
            dev = std::make_unique<boost::asio::posix::stream_descriptor>(*io,
                                                                          fd);
        }

        async_read();

        // register interfaces...
        server = std::make_shared<sdbusplus::asio::object_server>(bus);

        static constexpr const char pathBase[] =
            "/xyz/openbmc_project/Ipmi/Channel/";
        std::shared_ptr<sdbusplus::asio::dbus_interface> iface =
            server->add_interface(pathBase + channel,
                                  "xyz.openbmc_project.Ipmi.Channel.SMS");
        iface->register_method("setAttention",
                               [this]() { return setAttention(); });
        iface->register_method("clearAttention",
                               [this]() { return clearAttention(); });
        iface->register_method("forceAbort", [this]() { return forceAbort(); });
        iface->initialize();
    }

    bool initOK() const
    {
        return !!dev;
    }

    void channelAbort(const char* msg, const boost::system::error_code& ec)
    {
        log<level::ERR>(msg, entry("ERROR=%s", ec.message().c_str()));
        // bail; maybe a restart from systemd can clear the error
        io->stop();
    }

    void async_read()
    {
        boost::asio::async_read(
            *dev, boost::asio::buffer(xferBuffer, xferBuffer.size()),
            boost::asio::transfer_at_least(2),
            [this](const boost::system::error_code& ec, size_t rlen) {
                processMessage(ec, rlen);
            });
    }

    void processMessage(const boost::system::error_code& ecRd, size_t rlen)
    {
        if (ecRd || rlen < 2)
        {
            channelAbort("Failed to read req msg", ecRd);
            return;
        }

        async_read();

        // trim raw to be only bytes returned from read
        // separate netfn/lun/cmd from payload
        auto rawIter = xferBuffer.cbegin();
        auto rawEnd = rawIter + rlen;
        uint8_t netfn = *rawIter >> netFnShift;
        uint8_t lun = *rawIter++ & lunMask;
        uint8_t cmd = *rawIter++;
        if (verbose)
        {
            log<level::INFO>("Read req msg", entry("NETFN=0x%02x", netfn),
                             entry("LUN=0x%02x", lun),
                             entry("CMD=0x%02x", cmd));
        }

        // KY: increment counter
        ++num_requests;
        start = high_resolution_clock::now();

        // copy out payload
        std::vector<uint8_t> data(rawIter, rawEnd);
        // non-session bridges still need to pass an empty options map
        std::map<std::string, sdbusplus::message::variant<int>> options;
        // the response is a tuple because dbus can only return a single value
        using IpmiDbusRspType = std::tuple<uint8_t, uint8_t, uint8_t, uint8_t,
                                           std::vector<uint8_t>>;
        static constexpr const char ipmiQueueService[] =
            "xyz.openbmc_project.Ipmi.Host";
        static constexpr const char ipmiQueuePath[] =
            "/xyz/openbmc_project/Ipmi";
        static constexpr const char ipmiQueueIntf[] =
            "xyz.openbmc_project.Ipmi.Server";
        static constexpr const char ipmiQueueMethod[] = "execute";
        bus->async_method_call(
            [this, netfnCap{netfn}, lunCap{lun}, cmdCap{cmd}, &start,
             &end](const boost::system::error_code& ec,
                   const IpmiDbusRspType& response) {
                std::vector<uint8_t> rsp;
                const auto& [netfn, lun, cmd, cc, payload] = response;

                ++num_responses;
                if (num_responses != num_requests)
                {
                    std::cout << " num_requests " << num_requests
                              << " != num_responses " << num_responses
                              << std::endl;
                }

                if (ec)
                {
                    log<level::ERR>(
                        "kcs<->ipmid bus error:", entry("NETFN=0x%02x", netfn),
                        entry("LUN=0x%02x", lun), entry("CMD=0x%02x", cmd),
                        entry("ERROR=%s", ec.message().c_str()));
                    // send unspecified error for a D-Bus error
                    constexpr uint8_t ccResponseNotAvailable = 0xce;
                    rsp.resize(sizeof(netfn) + sizeof(cmd) + sizeof(cc));
                    rsp[0] =
                        ((netfnCap + 1) << netFnShift) | (lunCap & lunMask);
                    rsp[1] = cmdCap;
                    rsp[2] = ccResponseNotAvailable;

                    incrementHist(histogram, netfn, lun, cmd, -1);
                }
                else
                {
                    rsp.resize(sizeof(netfn) + sizeof(cmd) + sizeof(cc) +
                               payload.size());

                    // write the response
                    auto rspIter = rsp.begin();
                    *rspIter++ = (netfn << netFnShift) | (lun & lunMask);
                    *rspIter++ = cmd;
                    *rspIter++ = cc;
                    if (payload.size())
                    {
                        std::copy(payload.cbegin(), payload.cend(), rspIter);
                    }

                    end = high_resolution_clock::now();
                    auto diff = end - start;
                    auto diff_ms_int =
                        duration_cast<milliseconds>(diff).count();
                    incrementHist(histogram, netfn, lun, cmd, diff_ms_int);
                }

                if (verbose)
                {
                    log<level::INFO>(
                        "Send rsp msg", entry("NETFN=0x%02x", netfn),
                        entry("LUN=0x%02x", lun), entry("CMD=0x%02x", cmd),
                        entry("CC=0x%02x", cc));

                    // KY: print evert 64 messages
                    if (num_responses % 64 == 0)
                    {
                        prettyPrint(histogram);
                    }
                }

                boost::system::error_code ecWr;
                size_t wlen =
                    boost::asio::write(*dev, boost::asio::buffer(rsp), ecWr);
                if (ecWr || wlen != rsp.size())
                {
                    log<level::ERR>(
                        "Failed to send rsp msg", entry("SIZE=%d", wlen),
                        entry("EXPECT=%d", rsp.size()),
                        entry("ERROR=%s", ecWr.message().c_str()),
                        entry("NETFN=0x%02x", netfn), entry("LUN=0x%02x", lun),
                        entry("CMD=0x%02x", cmd), entry("CC=0x%02x", cc));
                }
            },
            ipmiQueueService, ipmiQueuePath, ipmiQueueIntf, ipmiQueueMethod,
            netfn, lun, cmd, data, options);
    }

    int64_t setAttention()
    {
        if (verbose)
        {
            log<level::INFO>("Sending SET_SMS_ATTENTION");
        }
        io_control::SetSmsAttention command;
        boost::system::error_code ec;
        dev->io_control(command, ec);
        if (ec)
        {
            log<level::ERR>("Couldn't SET_SMS_ATTENTION",
                            entry("ERROR=%s", ec.message().c_str()));
            return ec.value();
        }
        return 0;
    }

    int64_t clearAttention()
    {
        if (verbose)
        {
            log<level::INFO>("Sending CLEAR_SMS_ATTENTION");
        }
        io_control::ClearSmsAttention command;
        boost::system::error_code ec;
        dev->io_control(command, ec);
        if (ec)
        {
            log<level::ERR>("Couldn't CLEAR_SMS_ATTENTION",
                            entry("ERROR=%s", ec.message().c_str()));
            return ec.value();
        }
        return 0;
    }

    int64_t forceAbort()
    {
        if (verbose)
        {
            log<level::INFO>("Sending FORCE_ABORT");
        }
        io_control::ForceAbort command;
        boost::system::error_code ec;
        dev->io_control(command, ec);
        if (ec)
        {
            log<level::ERR>("Couldn't FORCE_ABORT",
                            entry("ERROR=%s", ec.message().c_str()));
            return ec.value();
        }
        return 0;
    }

  protected:
    std::array<uint8_t, kcsMessageSize> xferBuffer;
    std::shared_ptr<boost::asio::io_context> io;
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> server;
    std::unique_ptr<boost::asio::posix::stream_descriptor> dev = nullptr;
    bool verbose;
};

} // namespace

// this is a hack to allow the new thing run with the old service file
// it will be removed once the bb file is updated to use the new service file
#define ALLOW_OLD_ARGS 1

int main(int argc, char* argv[])
{
    CLI::App app("KCS IPMI bridge");
    std::string channel;
    app.add_option("-c,--channel", channel, "channel name. e.g., ipmi-kcs3");
    bool verbose = false;
    app.add_option("-v,--verbose", verbose, "print more verbose output");
#ifdef ALLOW_OLD_ARGS
    std::string device;
    app.add_option("--d,--device", device, "device name. e.g., /dev/ipmi-kcs3");
#endif
    CLI11_PARSE(app, argc, argv);

#ifdef ALLOW_OLD_ARGS
    if (channel.size() == 0)
    {
        size_t start = device.rfind('/');
        if (start == std::string::npos)
        {
            log<level::ERR>("bad device option",
                            entry("DEVICE=%s", device.c_str()));
            return EXIT_FAILURE;
        }
        channel = device.substr(start + 1);
    }
#endif

    // Connect to system bus
    auto io = std::make_shared<boost::asio::io_context>();
    sd_bus* dbus;
    sd_bus_default_system(&dbus);
    auto bus = std::make_shared<sdbusplus::asio::connection>(*io, dbus);

    // Create the channel, listening on D-Bus and on the SMS device
    SmsChannel smsChannel(io, bus, channel, verbose);

    if (!smsChannel.initOK())
    {
        return EXIT_FAILURE;
    }

    static constexpr const char busBase[] = "xyz.openbmc_project.Ipmi.Channel.";
    std::string busName(busBase + channel);
    bus->request_name(busName.c_str());

    io->run();

    return 0;
}
