#pragma once

#include <windows.h>

#include <optional>

// What must hold on this machine before an update is staged.
namespace gtg::update {

// Held by the update script for its whole run. The Tray only asks whether it
// exists: one update at a time, and a plan left behind while nobody holds it
// is an update that was interrupted.
inline constexpr wchar_t kUpdateMutexName[] = L"Global\\GpuThermalGuard.Update.v1";

// The Service would keep running the old build through a swap; phase 0 does
// not replace it, so it refuses.
[[nodiscard]] bool ServiceInstalled() noexcept;

// Smart App Control: 0 off, 1 on, 2 evaluation; nothing when unreadable. On,
// it blocks a new build that has no reputation -- and the rollback with it.
[[nodiscard]] std::optional<DWORD> SmartAppControlState() noexcept;

// The name is the plan's: tests, which run without elevation, use a Local one.
[[nodiscard]] bool UpdateRunning(const wchar_t* mutex_name = kUpdateMutexName) noexcept;

}  // namespace gtg::update
