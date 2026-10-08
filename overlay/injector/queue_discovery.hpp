#pragma once
#include "hook_protocol.hpp"
#include <cwchar>

namespace gtg::research {
constexpr std::uint32_t kQueueMagic = 0x35515447;

inline void QueueSectionName(DWORD pid, wchar_t (&name)[64]) noexcept {
    SectionName(pid, name);
    const auto length = std::wcslen(name);
    constexpr wchar_t suffix[] = L".Queue";
    for (std::size_t i = 0; i < sizeof(suffix) / sizeof(wchar_t); ++i)
        name[length + i] = suffix[i];
}

// Load-only research reuse of HookSection. signal_address/expected describe a
// temporary queue method, queue_identity its owning module base, reserved the
// PE timestamp, transaction_stage the PE SizeOfImage. No game queue is claimed.
}
