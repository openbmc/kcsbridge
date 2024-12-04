#include "args.hpp"

#include <getopt.h>

#include <format>
#include <stdexcept>

namespace kcsbridge
{

Args::Args(int argc, char* argv[])
{
    static const char opts[] = ":c:t:";
    static const struct option longopts[] = {
        {"channel", required_argument, nullptr, 'c'},
        {"timeout", required_argument, nullptr, 't'},
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
            case 't':
                timeout = std::stoi(optarg);
                break;
            case ':':
                throw std::runtime_error(
                    std::format("Missing argument for `{}`", argv[optind - 1]));
                break;
            default:
                throw std::runtime_error(std::format(
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
