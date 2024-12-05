#pragma once
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/slot.hpp>
#include <stdplus/fd/intf.hpp>

namespace kcsbridge
{
/**
 * @brief KCS Input request
 * @param netfn Network Function
 * @param lun Logical Unit Number
 * @param cmd Command
 * @param data Data
 */
using KCSIn = std::tuple<uint8_t, uint8_t, uint8_t, std::vector<uint8_t>>;

void write(stdplus::Fd& kcs, sdbusplus::message_t&& m, const KCSIn& kcsIn);
void read(stdplus::Fd& kcs, sdbusplus::bus_t& bus,
          sdbusplus::slot_t& outstanding, KCSIn& kcsIn, uint64_t timeout);

} // namespace kcsbridge
