#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "update/plan.hpp"

// Handing the update to the script.
//
// The Tray writes the plan and the script, then starts Windows PowerShell with
// the supervisor as its parent: the script then holds the supervisor's elevated
// token and stands outside the kill-on-close job that would end it with GTG.
// It never goes through the shell, so neither SmartScreen nor UAC can stop the
// chain. The Tray exits only once the script says it has read the plan.
namespace gtg::update {

// apply-update.ps1 as compiled into the module, or nothing.
[[nodiscard]] std::optional<std::string_view> EmbeddedScript(HMODULE module) noexcept;

struct HandOffResult {
    bool ok{};
    DWORD script_pid{};
    // Whether the script ended up inside this process's own job, the one that
    // ends with it (it must not).
    bool in_job{};
    std::wstring problem;
};

// Writes <work>\plan.json and <work>\apply-update.ps1, starts the script with
// `parent` as its parent process, and waits at most loaded_ms for
// <work>\loaded.json to name it. Without that, it deletes the plan -- a script
// that starts late then finds none and stops -- and reports the failure;
// nothing has been changed.
[[nodiscard]] HandOffResult HandOff(const std::filesystem::path& work, const Plan& plan,
                                    std::string_view script, DWORD parent,
                                    DWORD loaded_ms = 10'000);

}  // namespace gtg::update
