#pragma once

#include <windows.h>

#include <filesystem>

#include "update/files.hpp"

// Where an update is prepared, and what the folder GTG runs from allows.
//
// The work folder sits beside the EXE, under a name nobody else would use, and
// only SYSTEM and Administrators may touch it: the script that swaps the files
// runs as admin from there, so anything a non-admin program could change in it
// would be a way to run code as admin.
namespace gtg::update {

// SYSTEM and Administrators, full control, inherited by everything inside;
// nothing inherited from the parent.
inline constexpr wchar_t kWorkFolderSddl[] = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

// Creates the folder or, when it exists, replaces its access list with this one.
[[nodiscard]] bool SecureFolder(const std::filesystem::path& folder, const wchar_t* sddl,
                                DWORD& error) noexcept;

struct InstallFolder {
    // On a network drive or share: an update would be writing to someone
    // else's machine.
    bool remote{};
    // The volume keeps access lists (NTFS, ReFS). Without them the work folder
    // cannot be admin-only.
    bool acls{};
    // The user's non-elevated token may add or remove files here, so any
    // program the user runs could replace GTG, update or not.
    bool user_writable{};
};

[[nodiscard]] InstallFolder InspectInstallFolder(const std::filesystem::path& folder) noexcept;

}  // namespace gtg::update
