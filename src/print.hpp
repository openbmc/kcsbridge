#pragma once

#include <stdio.h>

#include <format>

// use this until gcc c++ lib has <print>
namespace std
{
inline void vprint(std::FILE* f, std::string_view format, std::format_args args)
{
    std::string d = std::vformat(format, args);
    fwrite(d.data(), 1, d.size(), f);
}
template <class... Args>
inline void print(std::FILE* f, std::format_string<Args...> format,
                  Args&&... args)
{
    vprint(f, format.get(), std::make_format_args(std::forward<Args>(args)...));
}
template <class... Args>
inline void print(std::format_string<Args...> format, Args&&... args)
{
    vprint(stdout, format.get(),
           std::make_format_args(std::forward<Args>(args)...));
}
} // namespace std
