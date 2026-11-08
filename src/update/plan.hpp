#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "update/digest.hpp"
#include "update/preconditions.hpp"

// The plan: everything the update script is told, written by the Tray as
// plan.json beside the script. It is the contract between GTG and
// apply-update.ps1, which reads exactly these fields and nothing from the
// source. The script runs as admin, so nothing in the plan is a command: it
// names files to install, the hashes they must have and the processes to wait
// for.
namespace gtg::update {

struct PlanFile {
    std::string name;
    std::filesystem::path staged;
    std::uint64_t size{};
    Digest sha256{};
};

// A process the script waits for, identified by its id and its creation time,
// so a reused id is not mistaken for it.
struct PlanProcess {
    DWORD pid{};
    std::uint64_t created{};
};

struct Plan {
    std::filesystem::path install;
    std::string entry;
    std::string old_version;
    std::string old_build;
    std::string new_version;
    std::string new_build;
    std::string release;
    // A reinstall from the source rather than an update; for the outcome only.
    bool reinstall{};
    // This build has no GPU to protect. Not in plan.json: HandOff writes it as
    // the unprotected file, which the new supervisor reads by name only.
    bool unprotected{};
    std::vector<PlanFile> files;
    std::vector<PlanProcess> wait;
    std::wstring mutex{kUpdateMutexName};
    unsigned wait_exit_ms{60'000};
    unsigned ready_ms{90'000};
};

// The creation time GetProcessTimes reports, as one number: what the script
// compares with Process.StartTime.ToFileTimeUtc().
[[nodiscard]] std::uint64_t ProcessCreated(HANDLE process) noexcept;

// UTF-8 JSON, keys in a fixed order.
[[nodiscard]] std::string FormatPlan(const Plan& plan);

[[nodiscard]] std::string Utf8(const std::wstring& text);

}  // namespace gtg::update
