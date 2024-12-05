#include "args.hpp"
#include "cmd.hpp"
#include "server.hpp"

#include <systemd/sd-daemon.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/slot.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>
#include <sdeventplus/source/signal.hpp>
#include <stdplus/exception.hpp>
#include <stdplus/fd/create.hpp>
#include <stdplus/print.hpp>
#include <stdplus/signal.hpp>

#include <algorithm>
#include <cstdio>
#include <format>
#include <stdexcept>
#include <string>

namespace kcsbridge
{

using sdeventplus::source::IO;
using sdeventplus::source::Signal;
using stdplus::fd::OpenAccess;
using stdplus::fd::OpenFlag;
using stdplus::fd::OpenFlags;

int execute(const char* channel, uint64_t timeout = 0)
{
    // Set up our DBus and event loop
    auto event = sdeventplus::Event::get_default();
    auto bus = sdbusplus::bus::new_default();
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);

    // Configure basic signal handling
    auto exit_handler = [&event](Signal&, const struct signalfd_siginfo*) {
        stdplus::print(stderr, "Interrupted, Exiting\n");
        event.exit(0);
    };
    stdplus::signal::block(SIGINT);
    Signal sig_int(event, SIGINT, exit_handler);
    stdplus::signal::block(SIGTERM);
    Signal sig_term(event, SIGTERM, exit_handler);

    // Open an FD for the KCS channel
    stdplus::ManagedFd kcs = stdplus::fd::open(
        std::format("/dev/{}", channel),
        OpenFlags(OpenAccess::ReadWrite).set(OpenFlag::NonBlock));
    sdbusplus::slot_t slot(nullptr);
    KCSIn kcsIn{};

    // Add a reader to the bus for handling inbound IPMI
    IO ioSource(event, kcs.get(), EPOLLIN | EPOLLET,
                stdplus::exception::ignore(
                    [&kcs, &bus, &slot, &kcsIn, timeout](IO&, int, uint32_t) {
                        read(kcs, bus, slot, kcsIn, timeout);
                    }));

    // Allow processes to affect the state machine
    std::string dbusChannel = channel;
    std::replace(dbusChannel.begin(), dbusChannel.end(), '-', '_');
    auto obj = "/xyz/openbmc_project/Ipmi/Channel/" + dbusChannel;
    auto srv = "xyz.openbmc_project.Ipmi.Channel." + dbusChannel;
    auto intf = createSMSHandler(bus, obj.c_str(), kcs);
    bus.request_name(srv.c_str());

    sd_notify(0, "READY=1");
    return event.loop();
}

} // namespace kcsbridge

int main(int argc, char* argv[])
{
    try
    {
        kcsbridge::Args args(argc, argv);
        return kcsbridge::execute(args.channel, args.timeout);
    }
    catch (const std::exception& e)
    {
        stdplus::print(stderr, "FAILED: {}\n", e.what());
        return 1;
    }
}
