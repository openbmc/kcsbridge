#include "cmd.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/slot.hpp>
#include <stdplus/exception.hpp>
#include <stdplus/fd/ops.hpp>
#include <stdplus/print.hpp>

#include <array>
#include <cstdio>
#include <format>
#include <map>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace kcsbridge
{

using sdbusplus::bus_t;
using sdbusplus::message_t;
using sdbusplus::slot_t;
using SdBusDuration =
    std::chrono::duration<uint64_t, std::chrono::microseconds::period>;

static std::string to_string(const KCSIn& kcsIn)
{
    const auto& [netfn, lun, cmd, data] = kcsIn;

    // security: data is not printed (it could contain passwords)
    return std::format("netfn: 0x{:02x}, lun: 0x{:02x}, cmd: 0x{:02x}, "
                       "data size: {}",
                       netfn, lun, cmd, data.size());
}

void write(stdplus::Fd& kcs, message_t&& m, const KCSIn& kcsIn)
{
    std::array<uint8_t, 1024> buffer;
    std::span<uint8_t> out(buffer.begin(), 3);
    try
    {
        if (m.is_method_error())
        {
            // Extra copy to workaround lack of `const sd_bus_error` constructor
            auto error = *m.get_error();
            throw sdbusplus::exception::SdBusError(&error, "ipmid response");
        }
        std::tuple<uint8_t, uint8_t, uint8_t, uint8_t, std::vector<uint8_t>>
            ret;
        m.read(ret);
        const auto& [netfn, lun, cmd, cc, data] = ret;
        // Based on the IPMI KCS spec Figure 9-2
        // netfn needs to be changed to odd in KCS responses
        if (data.size() + 3 > buffer.size())
        {
            throw std::runtime_error(std::format(
                "too large {} > {}", data.size() + 3, buffer.size()));
        }
        buffer[0] = (netfn | 1) << 2;
        buffer[0] |= lun;
        buffer[1] = cmd;
        buffer[2] = cc;
        memcpy(&buffer[3], data.data(), data.size());
        out = std::span<uint8_t>(buffer.begin(), data.size() + 3);
    }
    catch (const std::exception& e)
    {
        stdplus::print(stderr, "Req {}: IPMI response failure: {}\n",
                       to_string(kcsIn), e.what());
        buffer[0] |= 1 << 2;
        buffer[2] = 0xff;
    }
    stdplus::fd::writeExact(kcs, out);
}

void read(stdplus::Fd& kcs, bus_t& bus, slot_t& outstanding, KCSIn& kcsIn,
          uint64_t timeout)
{
    std::array<uint8_t, 1024> buffer;
    auto in = stdplus::fd::read(kcs, buffer);
    if (in.empty())
    {
        return;
    }
    if (outstanding)
    {
        stdplus::print(stderr, "Canceling outstanding request {}\n",
                       to_string(kcsIn));
        outstanding = slot_t(nullptr);
    }
    if (in.size() < 2)
    {
        stdplus::print(stderr, "Read too small, ignoring\n");
        return;
    }
    auto m = bus.new_method_call("xyz.openbmc_project.Ipmi.Host",
                                 "/xyz/openbmc_project/Ipmi",
                                 "xyz.openbmc_project.Ipmi.Server", "execute");
    std::map<std::string, std::variant<int>> options;
    // Based on the IPMI KCS spec Figure 9-1
    uint8_t netfn = in[0] >> 2, lun = in[0] & 3, cmd = in[1];
    m.append(netfn, lun, cmd, in.subspan(2), options);
    kcsIn = {netfn, lun, cmd, std::vector<uint8_t>(in.begin() + 2, in.end())};
    outstanding = m.call_async(
        stdplus::exception::ignore([&outstanding, &kcs, kcsIn](message_t&& m) {
            outstanding = slot_t(nullptr);
            write(kcs, std::move(m), kcsIn);
        }),
        SdBusDuration{timeout});
    if (!outstanding)
    {
        stdplus::print(stderr, "Failed to send request {}\n", to_string(kcsIn));
        kcsIn = {};
    }
}

} // namespace kcsbridge
