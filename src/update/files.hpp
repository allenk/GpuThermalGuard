#pragma once

// The names inside an install's work folder, <install>\_gtg_update\. The
// supervisor, the Tray and the update script all use them; the script carries
// its own copies, which apply_test.ps1 checks against these.
namespace gtg::update {

inline constexpr wchar_t kWorkFolderName[] = L"_gtg_update";
// The Tray writes both before it starts the script.
inline constexpr wchar_t kPlanFileName[] = L"plan.json";
inline constexpr wchar_t kScriptFileName[] = L"apply-update.ps1";
// The script, once it has read the plan and taken the update mutex.
inline constexpr wchar_t kLoadedFileName[] = L"loaded.json";
// The supervisor, the first time it sees protection ready while a plan exists.
inline constexpr wchar_t kReadyFileName[] = L"ready.json";
// The script, when it is done, however it ended.
inline constexpr wchar_t kOutcomeFileName[] = L"outcome.json";
inline constexpr wchar_t kLogFileName[] = L"update.log";

}  // namespace gtg::update
