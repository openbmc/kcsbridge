#include "server.hpp"

#include "print.hpp"

#include <linux/ipmi_bmc.h>

#include <sdbusplus/exception.hpp>
#include <sdbusplus/server/interface.hpp>
#include <sdbusplus/vtable.hpp>
#include <stdplus/fd/ops.hpp>

#include <cstdio>
#include <stdexcept>

namespace kcsbridge
{

void setAttention(sdbusplus::message_t& m, stdplus::Fd& kcs)
{
    stdplus::fd::ioctl(kcs, IPMI_BMC_IOCTL_SET_SMS_ATN, nullptr);
    m.new_method_return().method_return();
}

void clearAttention(sdbusplus::message_t& m, stdplus::Fd& kcs)
{
    stdplus::fd::ioctl(kcs, IPMI_BMC_IOCTL_CLEAR_SMS_ATN, nullptr);
    m.new_method_return().method_return();
}

void forceAbort(sdbusplus::message_t& m, stdplus::Fd& kcs)
{
    stdplus::fd::ioctl(kcs, IPMI_BMC_IOCTL_FORCE_ABORT, nullptr);
    m.new_method_return().method_return();
}

template <auto func, typename Data>
int methodRsp(sd_bus_message* mptr, void* dataptr, sd_bus_error* error) noexcept
{
    sdbusplus::message_t m(mptr);
    try
    {
        func(m, *reinterpret_cast<Data*>(dataptr));
    }
    catch (const std::exception& e)
    {
        std::print(stderr, "Method response failed: {}\n", e.what());
        sd_bus_error_set(error,
                         "xyz.openbmc_project.Common.Error.InternalFailure",
                         "The operation failed internally.");
    }
    return 1;
}

template <typename Data>
constexpr sdbusplus::vtable::vtable_t dbusMethods[] = {
    sdbusplus::vtable::start(),
    sdbusplus::vtable::method("setAttention", "", "",
                              methodRsp<setAttention, Data>),
    sdbusplus::vtable::method("clearAttention", "", "",
                              methodRsp<clearAttention, Data>),
    sdbusplus::vtable::method("forceAbort", "", "",
                              methodRsp<forceAbort, Data>),
    sdbusplus::vtable::end(),
};

sdbusplus::server::interface::interface createSMSHandler(
    sdbusplus::bus_t& bus, const char* obj, stdplus::Fd& kcs)
{
    return sdbusplus::server::interface::interface(
        bus, obj, "xyz.openbmc_project.Ipmi.Channel.SMS",
        dbusMethods<stdplus::Fd>, reinterpret_cast<stdplus::Fd*>(&kcs));
}

} // namespace kcsbridge
