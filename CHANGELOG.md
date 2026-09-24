# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

## [0.12.0] - 2026-09-25

### Added

- Optional, default-on network record. One card carries both directions as two curves with a translucent fill, on the interface the default route actually uses, re-checked every three seconds so a VPN or an unplugged cable follows the traffic rather than a name chosen at startup. Counters come from `GetIfEntry2` on the existing presentation tick.
- A verdict on the network path, on demand. Double-clicking the NET card measures the foreground program's TCP connections and reports a latency, a colour and a three-bar mark, built entirely from counters the Windows TCP stack already keeps -- round-trip time, its variance, retransmissions, duplicate acknowledgements, timeouts. Nothing is sent, opened or resolved in order to time it.
- Releasing host memory, on demand. Double-clicking the RAM card on a locked compact dashboard trims working sets and releases the standby list, then reports how much came back. It runs on its own thread, takes no focus and pops nothing up, so it is usable with a game in front.
- Device-derived initial power settings. Working Limit starts at the limit already in force, Safe Power at the lower of the card's default and current limits, and Trigger Temp one degree below the GPU's own slowdown threshold. Both power values are clamped into the range the card reports.
- Monitor-only state. Equal working and safe limits mean there is nothing to drop to, so the tool monitors without protecting and says so, rather than starting to cut power on a machine whose owner never chose a safe limit. A message on first run explains the setting that is needed.
- Single-column compact dashboard. Any row may hold one card, so the whole dashboard can be a vertical strip against a screen edge. At one column the logo gives way and the status dot moves left.
- The compact/expanded choice is remembered between runs.

### Improved

- Dropping a card between two vertically adjacent cards inserts a new row there, instead of only at the bottom.
- Dropping a card almost exactly onto another swaps the two, instead of inserting to its right.
- A lane's value box is no longer a constant scaled by a vertical ratio, so adding a record cannot narrow it. `349.5 W (350 W)` on a 350 W card used to be cut to `349.5 W (350...`; a figure that has been cut is a different figure.
- Both network directions are shown in a single fixed unit at every magnitude. A unit that changes with the value is unreadable in a card this size.
- The lock and chevron in the compact header sit closer together, and the drag strip is centred under them.
- A dragged NET card carries its label; it was blank.

### Notes

- The README shows the network action running against a real game and ending in `--`: connections were watched, none carried enough traffic in the window to measure. That is the honest outcome for a game that plays over UDP, and it is the one published, because the gesture is worth knowing about and the result is not guaranteed to be useful.
- **The verdict does not claim the attempt helped.** The action also discards the cached path state and re-resolves the next hop; that was measured against a control and found to be below the noise, so it is offered as an attempt and never reported as an improvement. What is promised is the reading.
- The path measurement is **TCP only** -- Windows keeps no per-connection statistics for UDP, so a game whose gameplay is UDP reports `no TCP` rather than a verdict -- and **IPv4 only** in this release. Extended TCP statistics require Administrator, which this application already runs as.
- Extended statistics are enabled only on the foreground program's own connections, only while a verdict is being taken, and are never disabled again, per Microsoft's guidance that disabling them would disrupt any other monitor watching the same connection.
- The verdict is the worst of the connections measured, never the average. That is right for a game, which holds a few connections all to its server, and pessimistic for a browser holding a crowd of unrelated ones.
- A network reading is refused rather than guessed whenever it cannot be trusted: a counter that went backwards, a gap longer than five seconds, an interface that changed underneath, or a rate above what the link can carry.
- `iphlpapi.dll` is resolved at run time like `nvml.dll`, so the single `/MT` EXE gains no sidecar dependency and no link-line change.

## [0.11.0-beta.1] - 2026-09-22

### Added

- Optional, default-on host RAM record in the Main UI history and the expanded and compact OSD. Physical memory is the foreground reading; virtual commit is drawn behind it in a second accent so both are legible at a glance without competing. Turning the record off clears its history immediately.
- Arrangeable compact OSD. A lock control in the compact header unlocks the metric cells: press a cell and release it where it belongs to reorder it, or to move it between the first and second row. The arrangement is remembered, and a tray-menu entry restores the default. The overlay still moves by its top bar whether locked or not.
- The compact OSD now defaults to a single row of every enabled record, so it grows sideways with the record count rather than wrapping. A second row is something you choose.
- A DPI legibility bench (`gtg_dpi_legibility_bench`) that renders the production history chart offscreen at a chosen DPI and reports base units, font sizes and lane heights.

### Improved

- History chart axis labels no longer truncate to an ellipsis at 100 % display scaling; the label gutter is measured rather than assumed.
- The Main UI history area is enlarged within the existing dialog, so seven records do not read as cramped. The dialog size is unchanged.
- RAM and FPS plots derive their geometry from the same lane definition as the thermal records, so every curve starts at the same left edge no matter how many records are shown.

### Notes

- Host memory is read with a single `GlobalMemoryStatusEx` call on the existing presentation tick. It adds no link-time dependency, is never read on the protection path, and is not written to the journal. Measured across 79,239 protection samples with the record enabled: no missed deadlines, no wake misses, no execution overruns.
- Virtual commit is measured against the commit limit, which is larger than installed physical memory, so the commit reading normally sits below the physical one.

## [0.10.0-beta.1] - 2026-09-20

### Added

- Optional, default-on FPS display in the Main UI history and expanded/compact OSD. Turning it off clears the separate FPS history and restores the original five-metric layout.
- Foreground-game Displayed FPS observation using a bounded Windows ETW correlator for validated DXGI presentation paths. No game injection, proxy DLL, PresentMon runtime, or dependency on the thermal protection worker.

### Improved

- FPS curves use the same area-fill treatment as other metrics. Dark visual connectors bridge unavailable intervals and app/surface switches without inventing measured samples; unavailable readings remain `-`.

### Limitations

- Native Vulkan FPS and in-game overlay rendering are not included. Unsupported, ambiguous, or unverified presentation paths show `-` rather than a submitted-frame estimate.

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

[Unreleased]: https://github.com/allenk/GpuThermalGuard/compare/v0.10.0-beta.1...HEAD
[0.10.0-beta.1]: https://github.com/allenk/GpuThermalGuard/compare/v0.9.0-beta.2...v0.10.0-beta.1
[0.9.0-beta.2]: https://github.com/allenk/GpuThermalGuard/compare/v0.9.0-beta.1...v0.9.0-beta.2
[0.9.0-beta.1]: https://github.com/allenk/GpuThermalGuard/compare/v0.8.0-beta.1...v0.9.0-beta.1
[0.8.0-beta.1]: https://github.com/allenk/GpuThermalGuard/releases/tag/v0.8.0-beta.1
