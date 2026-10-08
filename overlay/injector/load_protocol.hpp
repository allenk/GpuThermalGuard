#pragma once
#include <windows.h>
#include <cstdint>

namespace gtg::research {
constexpr std::uint32_t kMagic = 0x354c5447;
constexpr std::uint32_t kVersion = 1;
constexpr DWORD kWorkerWaitMs = 1500;
enum class LoadState : LONG { kWaiting, kReady, kReleased, kTimedOut, kRejected };

struct alignas(8) LoadSection {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t size;
    DWORD target_pid;
    volatile LONG state;
    volatile LONG release;
    std::uint64_t module_base;
};

static_assert(sizeof(LoadSection) == 32);

// No CRT use on the DLL worker; the buffer holds the prefix plus a DWORD PID.
inline void SectionName(DWORD pid, wchar_t (&name)[64]) noexcept {
    constexpr wchar_t kPrefix[] = L"Local\\GTG.Research.RemoteLoad.";
    unsigned int n = 0;
    for (wchar_t c : kPrefix) {
        if (c) name[n++] = c;
    }
    wchar_t digits[10]{};
    unsigned int count = 0;
    do {
        digits[count++] = static_cast<wchar_t>(L'0' + pid % 10);
        pid /= 10;
    } while (pid);
    while (count)
        name[n++] = digits[--count];
    name[n] = 0;
}
}  // namespace gtg::research
