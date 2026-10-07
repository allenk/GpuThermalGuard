#pragma once
#include <cstdio>
namespace gtg::overlay::probe {
template <typename... Args>
void Print(const char* format, Args... args) {
#ifndef GTG_DIRECT_DRAW
    std::printf(format, args...);
#else
    (void)format;
    ((void)args, ...);
#endif
}
}  // namespace gtg::overlay::probe
