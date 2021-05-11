#pragma once
#include <cstddef>

namespace kcsbridge
{

struct Args
{
    const char* channel = nullptr;

    Args(int argc, char* argv[]);
};

} // namespace kcsbridge
