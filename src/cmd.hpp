#pragma once
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/slot.hpp>
#include <stdplus/fd/intf.hpp>

namespace kcsbridge
{

void write(stdplus::Fd& kcs, sdbusplus::message::message&& m);
void read(stdplus::Fd& kcs, sdbusplus::bus::bus& bus,
          sdbusplus::slot::slot& outstanding);

} // namespace kcsbridge
