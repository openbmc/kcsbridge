#pragma once
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/slot.hpp>
#include <stdplus/fd/intf.hpp>

namespace kcsbridge
{

void write(stdplus::Fd& kcs, sdbusplus::message_t&& m);
void read(stdplus::Fd& kcs, sdbusplus::bus_t& bus,
          sdbusplus::slot_t& outstanding);

} // namespace kcsbridge
