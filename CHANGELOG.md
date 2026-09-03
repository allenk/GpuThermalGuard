# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

## [0.8.0-beta.1] - 2026-09-03

### Added

- 200 ms NVML temperature, power, VRAM, GPU-utilization, and CPU-utilization monitoring.
- Hard-limit and confirmed predictive thermal protection with verified power-limit writes.
- Safe-power latch with manual restore and optional strictly monitored automatic restore.
- Persistent trip count, structured rotating logs, and trigger/manual dashboard snapshots.
- One-hour history with a draggable five-minute view and a compact 30-second OSD.
- Tray UI, Explorer restart recovery, single-instance activation, and SCM service mode.
- Standalone embedded English and Traditional Chinese UI with per-user language persistence.
- Static MSVC runtime and vendored WTL/NVML headers for a single-EXE deployment.

### Safety

- Automatic restore keeps the trip count, immediately rearms monitoring, and can re-trip on the first dangerous sample.
- A power-limit write is never reported as successful until the value is read back.
- Release automation requires Authenticode signing before a GitHub Release can be published.

[Unreleased]: https://github.com/allenk/GpuThermalGuard/compare/v0.8.0-beta.1...HEAD
[0.8.0-beta.1]: https://github.com/allenk/GpuThermalGuard/releases/tag/v0.8.0-beta.1
