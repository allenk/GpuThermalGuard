# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

## [0.9.0-beta.2] - 2026-09-17

### Added

- Equal-width compact OSD with a fixed-position header toggle, five live metric cards, and 30-second miniature trends. Protection alerts and telemetry warnings remain visible without interrupting monitoring.
- Compact OSD screenshot and English/Traditional Chinese usage documentation.

## [0.9.0-beta.1] - 2026-09-15

### Added

- Dedicated high-priority protection scheduling, independent of the desktop message loop, with delay and telemetry-loss safety handling.
- Same-EXE supervisor for unexpected child failure, bounded restart backoff, and conservative driver-recovery checks.
- Persistent lifetime trip totals and an explicit per-run Reset action that does not interrupt monitoring.
- VBIOS display, pending-settings indication, and safety-checked application of the working power limit.
- OSD protection alerts, delayed-data visualization, and persisted, monitor-validated window positions.

### Improved

- Bounded automatic restore retries, power-write/readback diagnostics, and verified safe-power fallback after restore failure.
- Deferred bounded logging and OSD drawing/resource handling for long-running sessions.
- Non-activating OSD visibility repair after session/display changes and sustained ordinary-window obstruction.
- Updated English/Traditional Chinese documentation and screenshots, with a clearly separated preview of the unreleased private System Monitor integration.

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
- Release automation signs and verifies executables when an Authenticode certificate is configured; otherwise it publishes only a clearly labeled unsigned prerelease with checksums and build provenance.

[Unreleased]: https://github.com/allenk/GpuThermalGuard/compare/v0.9.0-beta.2...HEAD
[0.9.0-beta.2]: https://github.com/allenk/GpuThermalGuard/compare/v0.9.0-beta.1...v0.9.0-beta.2
[0.9.0-beta.1]: https://github.com/allenk/GpuThermalGuard/compare/v0.8.0-beta.1...v0.9.0-beta.1
[0.8.0-beta.1]: https://github.com/allenk/GpuThermalGuard/releases/tag/v0.8.0-beta.1
