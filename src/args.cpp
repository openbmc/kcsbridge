#include "args.hpp"

#include <fmt/format.h>
#include <getopt.h>

#include <stdexcept>

namespace kcsbridge
{

Args::Args(int argc, char* argv[])
{
    static const char opts[] = ":c:";
    static const struct option longopts[] = {
        {"channel", required_argument, nullptr, 'c'},
        {nullptr, 0, nullptr, 0},
    };
    int c;
    optind = 0;
    while ((c = getopt_long(argc, argv, opts, longopts, nullptr)) > 0)
    {
        switch (c)
        {
            case 'c':
                channel = optarg;
                break;
            case ':':
                throw std::runtime_error(
                    fmt::format("Missing argument for `{}`", argv[optind - 1]));
                break;
            default:
                throw std::runtime_error(fmt::format(
                    "Invalid command line argument `{}`", argv[optind - 1]));
        }
    }
    if (optind != argc)
    {
        throw std::invalid_argument("Requires no additional arguments");
    }
    if (channel == nullptr)
    {
        throw std::invalid_argument("Missing KCS channel");
    }
}

} // namespace kcsbridge
