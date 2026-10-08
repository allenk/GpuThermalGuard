#pragma once
#include "color_choice_policy.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <sddl.h>

namespace gtg::overlay::integration {
constexpr DWORD kColorChoiceMagic = 0x43475447;

struct alignas(8) ColorChoice {
    DWORD magic{kColorChoiceMagic}, version{1}, size{sizeof(ColorChoice)}, target_pid{};
    std::uint64_t target_created{};
    DWORD writer_pid{}, reserved{};
    std::uint64_t writer_created{};
    DWORD policy{static_cast<DWORD>(ColorPolicy::ResearchAssumeSdr)};
    volatile LONG ready{};
};

static_assert(sizeof(ColorChoice) == 48 && offsetof(ColorChoice, ready) == 44);

inline std::uint64_t ChoiceCreationTime(HANDLE process) noexcept {
    FILETIME c{}, e{}, k{}, u{};
    return GetProcessTimes(process, &c, &e, &k, &u)
               ? (static_cast<std::uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime
               : 0;
}

inline void ColorChoiceName(DWORD pid, wchar_t (&name)[96]) noexcept {
    swprintf_s(name, L"Local\\GTG.Research.ColorChoice.%lu", pid);
}

inline bool ValidColorChoice(const ColorChoice& c, DWORD pid, std::uint64_t created) noexcept {
    return c.magic == kColorChoiceMagic && c.version == 1 && c.size == sizeof(ColorChoice) &&
           c.target_pid == pid && c.target_created == created && pid && created && c.writer_pid &&
           c.writer_created && !c.reserved &&
           c.policy == static_cast<DWORD>(ColorPolicy::ResearchAssumeSdr) && c.ready == 1;
}
enum class ChoiceRead { Missing, Chosen, Invalid };

class ColorChoiceMapping {
public:
    ColorChoiceMapping() = default;
    ColorChoiceMapping(const ColorChoiceMapping&) = delete;
    ColorChoiceMapping& operator=(const ColorChoiceMapping&) = delete;

    ~ColorChoiceMapping() {
        if (data_) UnmapViewOfFile(data_);
        if (handle_) CloseHandle(handle_);
    }

    bool Create(DWORD pid, std::uint64_t created) noexcept {
        if (handle_ || !pid || !created) return false;
        PSECURITY_DESCRIPTOR descriptor{};
        // Owner/admin/system write; other interactive readers cannot mutate it.
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)(A;;GR;;;IU)", SDDL_REVISION_1, &descriptor,
                nullptr))
            return false;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        wchar_t name[96]{};
        ColorChoiceName(pid, name);
        handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
                                     sizeof(ColorChoice), name);
        const auto error = GetLastError();
        LocalFree(descriptor);
        if (!handle_ || error == ERROR_ALREADY_EXISTS) return false;
        data_ = static_cast<ColorChoice*>(
            MapViewOfFile(handle_, FILE_MAP_WRITE, 0, 0, sizeof(ColorChoice)));
        if (!data_) return false;
        *data_ = {};
        data_->target_pid = pid;
        data_->target_created = created;
        data_->writer_pid = GetCurrentProcessId();
        data_->writer_created = ChoiceCreationTime(GetCurrentProcess());
        if (!data_->writer_created) return false;
        InterlockedExchange(&data_->ready, 1);
        return true;
    }

private:
    HANDLE handle_{};
    ColorChoice* data_{};
};

inline ChoiceRead ReadColorChoice(DWORD pid, std::uint64_t created, ColorPolicy& policy,
                                  DWORD expected_writer = 0,
                                  std::uint64_t expected_writer_created = 0) noexcept {
    policy = ColorPolicy::Strict;
    if (!pid || !created) return ChoiceRead::Invalid;
    wchar_t name[96]{};
    ColorChoiceName(pid, name);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!mapping)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? ChoiceRead::Missing : ChoiceRead::Invalid;
    const auto* data = static_cast<const ColorChoice*>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(ColorChoice)));
    ChoiceRead result = ChoiceRead::Invalid;
    if (data &&
        std::atomic_ref<LONG>(const_cast<LONG&>(data->ready)).load(std::memory_order_acquire) ==
            1) {
        const ColorChoice snapshot = *data;
        if (ValidColorChoice(snapshot, pid, created) &&
            ((!expected_writer && !expected_writer_created) ||
             (snapshot.writer_pid == expected_writer &&
              snapshot.writer_created == expected_writer_created))) {
            HANDLE writer = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                        snapshot.writer_pid);
            if (writer) {
                if (ChoiceCreationTime(writer) == snapshot.writer_created &&
                    WaitForSingleObject(writer, 0) == WAIT_TIMEOUT) {
                    policy = ColorPolicy::ResearchAssumeSdr;
                    result = ChoiceRead::Chosen;
                }
                CloseHandle(writer);
            }
        }
    }
    if (data) UnmapViewOfFile(data);
    CloseHandle(mapping);
    return result;
}
}
