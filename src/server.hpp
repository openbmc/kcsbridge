#pragma once
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/interface.hpp>
#include <stdplus/fd/intf.hpp>

namespace kcsbridge
{

sdbusplus::server::interface::interface createSMSHandler(
    sdbusplus::bus::bus& bus, const char* obj, stdplus::Fd& kcs);

}
