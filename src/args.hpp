#pragma once
#include <cstddef>
#include <cstdint>

namespace kcsbridge
{

struct Args
{
    const char* channel = nullptr;
    uint64_t timeout = 0; // in milliseconds, 0 means default timeout

    Args(int argc, char* argv[]);
};

} // namespace kcsbridge
