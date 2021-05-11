#include "cmd.hpp"

#include <fmt/format.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/slot.hpp>
#include <stdplus/exception.hpp>
#include <stdplus/fd/ops.hpp>
#include <stdplus/types.hpp>

#include <array>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace kcsbridge
{

using sdbusplus::bus::bus;
using sdbusplus::message::message;
using sdbusplus::slot::slot;

void write(stdplus::Fd& kcs, message&& m)
{
    std::array<uint8_t, 1024> buffer;
    stdplus::span<uint8_t> out(buffer.begin(), 3);
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
            throw std::runtime_error(fmt::format(
                "too large {} > {}", data.size() + 3, buffer.size()));
        }
        buffer[0] = (netfn | 1) << 2;
        buffer[0] |= lun;
        buffer[1] = cmd;
        buffer[2] = cc;
        memcpy(&buffer[3], data.data(), data.size());
        out = stdplus::span<uint8_t>(buffer.begin(), data.size() + 3);
    }
    catch (const std::exception& e)
    {
        fmt::print(stderr, "IPMI response failure: {}\n", e.what());
        buffer[0] |= 1 << 2;
        buffer[2] = 0xff;
    }
    stdplus::fd::writeExact(kcs, out);
}

void read(stdplus::Fd& kcs, bus& bus, slot& outstanding)
{
    std::array<uint8_t, 1024> buffer;
    auto in = stdplus::fd::read(kcs, buffer);
    if (in.empty())
    {
        return;
    }
    if (outstanding)
    {
        fmt::print(stderr, "Canceling outstanding request\n");
        outstanding = slot(nullptr);
    }
    if (in.size() < 2)
    {
        fmt::print(stderr, "Read too small, ignoring\n");
        return;
    }
    auto m = bus.new_method_call("xyz.openbmc_project.Ipmi.Host",
                                 "/xyz/openbmc_project/Ipmi",
                                 "xyz.openbmc_project.Ipmi.Server", "execute");
    std::map<std::string, std::variant<int>> options;
    // Based on the IPMI KCS spec Figure 9-1
    uint8_t netfn = in[0] >> 2, lun = in[0] & 3, cmd = in[1];
    m.append(netfn, lun, cmd, in.subspan(2), options);
    outstanding = m.call_async(stdplus::exception::ignore([&](message&& m) {
        outstanding = slot(nullptr);
        write(kcs, std::move(m));
    }));
}

} // namespace kcsbridge
