# GpuThermalGuard

English | [繁體中文](README.zh-TW.md)

**A lightweight Windows GPU thermal protection and real-time monitoring utility for NVIDIA GPUs, powered by NVML.**

GpuThermalGuard watches GPU telemetry in real time and applies a preconfigured lower power limit when a dangerous temperature or a rapidly rising thermal trend is detected. It is a small native C++/Win32 application designed to remain useful precisely when GPU stability is in question.

> [!WARNING]
> This is an independent mitigation tool, not an NVIDIA product and not a firmware, driver, cooling, or hardware repair. It cannot guarantee prevention of every TDR. A defective or affected board may still require an official SKU-matched VBIOS update or RMA.

## Screenshots

| Main dashboard | 30-second OSD |
| --- | --- |
| ![GpuThermalGuard dashboard](docs/images/dashboard.png) | ![GpuThermalGuard OSD](docs/images/osd.png) |

Screenshots show one workstation's settings, not recommended limits for every GPU.

### Compact OSD

![Compact OSD with five live metrics and miniature trends](docs/images/osd-compact.png)

Use the chevron in the OSD header to switch between the full charts and compact mode. Compact mode keeps the same width and toggle position, showing temperature, power, VRAM, GPU utilization, and CPU utilization in five small cards with live values and 30-second miniature trends.

Protection alerts and telemetry warnings remain visible in the header. Switching views does not pause monitoring or change protection settings. Drag the header outside the toggle button to move the OSD; restart returns to the expanded view.

Compact mode is included starting with v0.9.0-beta.2.

## Why I built it

This project started with an RTX PRO 6000 Blackwell Workstation Edition used for sustained AI inference. Under long-running workloads, Windows would occasionally report a TDR or lose the GPU. The recurring incident pattern was difficult to ignore:

- the GPU fan suddenly went to 100%;
- telemetry around the incident commonly showed approximately 88–93 °C;
- the driver/GPU did not always recover cleanly after the reset;
- unattended inference jobs were therefore unsafe to leave running.

Reducing the board power limit from 600 W to 350 W made the machine substantially more usable, but a permanent static cap was a blunt workaround. What I wanted was a small guard that could watch temperature continuously, react before the firmware/driver protection path became unreliable, preserve evidence of the incident, and keep the safer limit latched until recovery was explicitly allowed.

The usual gaming-oriented tuning and fan-control applications did not support this workstation board or did not provide the required temperature-to-power-limit safety policy. NVIDIA App exposed useful telemetry, but not the control needed for this workflow. NVML did expose a supported power-limit path, so GpuThermalGuard was built around that narrow, verifiable mechanism.

## The VBIOS and RMA context

This project does not claim that every black screen or TDR has the same cause. However, the RTX PRO 6000 Blackwell investigation uncovered relevant reports around early VBIOS versions:

- In one [RTX PRO 6000 black-screen report](https://forums.developer.nvidia.com/t/nvidia-rtx-6000-pro-blackwell-workstation-screens-keep-going-black/351107), the affected card was replaced and returned with a newer VBIOS; the reporter stated that the problem was resolved.
- NVIDIA forum guidance for the [RTX PRO 6000 VBIOS update path](https://forums.developer.nvidia.com/t/rtx-pro-6k-bwe-vbios-how-to-obtain/366329) explains that channel partners may have SKU-specific VBIOS customization and that the correct field updater or RMA must be obtained through the reseller/distributor chain.
- A later [firmware-channel discussion](https://forums.developer.nvidia.com/t/rtx-pro-6000-blackwell-workstation-edition-vbios-too-old-for-mig-98-02-52-00-02-how-to-obtain-update/374970) reiterates that subsystem IDs alone do not identify the support channel and recommends serial-number verification with the reseller or distributor.

Do not cross-flash a ROM downloaded for a different board or channel SKU. GpuThermalGuard is intended to reduce risk while an official firmware/support resolution is pursued; it is not a substitute for that resolution.

## Design principles

- **Native and small** — C++20, Win32, WTL, GDI/GDI+; no browser runtime or GPU rendering backend.
- **Independent protection loop** — a dedicated high-priority worker targets a 200 ms cadence; UI repaint and message-loop delays do not schedule protection. Driver calls can still stall, so this is not a hard real-time guarantee.
- **Fail-safe state model** — a hard temperature crossing is never hidden by debounce or smoothing.
- **Verified writes** — a power-limit change is not reported as successful until it is read back.
- **Safe latch** — after a trip, safe power remains active until stable cooling permits manual or explicitly enabled automatic restore.
- **Immediately rearmed** — automatic restore never relaxes monitoring; a renewed over-temperature condition can trip again immediately.
- **Incident evidence** — rolling telemetry, logs, trip count, wall time, and automatic/manual PNG snapshots.
- **Standalone deployment** — static MSVC runtime and a single EXE; no sidecar runtime files. Windows system DLLs and the NVIDIA driver's `nvml.dll` are still required.
- **CPU-rendered UI** — no application GPU rendering backend; Windows desktop composition can still be delayed by a busy or recovering graphics driver.

## What it monitors

- GPU model, VBIOS version, temperature, and firmware thermal thresholds
- board power draw and current/default power limits
- VRAM usage percentage and GiB
- GPU utilization
- CPU utilization
- one hour of retained telemetry with a draggable five-minute dashboard view
- a compact, non-activating 30-second always-on-top OSD

## Protection behavior

1. Read NVML telemetry every 200 ms.
2. Trip immediately at the configured temperature, or after the confirmed predictive-rise rule indicates an imminent crossing.
3. Apply the configured safe power limit and read it back.
4. Latch the protected state, persist the trip count, record the event, and capture a dashboard snapshot.
5. Continue enforcing safe power while cooling.
6. After the stable-cooling window, offer manual restore or perform it only when **Auto Restore** was explicitly enabled.
7. Verify normal power after restore and immediately rearm the protection loop.

The lifetime trip total persists across restarts, manual/automatic restore, and **Save & Apply**. Use **Reset** beside **Run trips** to begin a new test session without interrupting protection or changing the lifetime total.

**Working Limit (W)** is the operating power ceiling. **Save & Apply** requests that limit through the protection worker only when its safety checks permit it; a latched or hot GPU is not forced back to working power. Starting the application does not submit an Apply request. Unapplied edits are highlighted; inspect the displayed current limit and status for the verified result.

Restore failures retain the latch and attempt a verified safe-power fallback. Automatic recovery is bounded to three attempts per latch, at least five seconds apart and still subject to fresh cooling checks; permanent errors stop retries early. The log records write/readback evidence. Manual restore remains available.

The OSD displays an **ALERT** while safe power is latched, including while waiting for restore. Delayed presentation samples are visually marked rather than silently presented as fresh measurements. Missing protection telemetry is handled separately by the protection core.

## Language and settings

English is the first-run default. Traditional Chinese can be selected from the bottom language list. The choice is embedded in the EXE and persisted for the current user at:

```text
HKCU\SOFTWARE\GpuThermalGuard\UiLanguage
```

Protection settings and persistent protection state are stored under:

```text
HKLM\SOFTWARE\GpuThermalGuard
```

The application requests administrator privileges because changing an NVIDIA power limit and writing machine-wide protection settings require elevation.

## Running

The default mode is the tray application:

```powershell
.\GpuThermalGuard.exe
.\GpuThermalGuard.exe --tray
```

Default/Tray launch uses an NVML-free supervisor and a monitored child, both from the same EXE. Unexpected child failure triggers bounded-backoff restart and conservative recovery checks; intentional Exit stops the application. This reduces outages but cannot guarantee uninterrupted protection during driver failure. No separate supervisor service or installer is required.

The same executable also contains an SCM service entry point:

```text
GpuThermalGuard.exe --service
```

`--service` is intended to be launched by Windows Service Control Manager, not directly from an interactive console. The current preview does not yet ship an installer; tray mode is the recommended evaluation path.

## Build from source

Requirements:

- Windows 10/11 x64
- Visual Studio 2022 with Desktop development with C++
- CMake 3.24+
- Ninja (included with Visual Studio CMake tools)
- an NVIDIA driver that provides NVML at runtime

From a Visual Studio 2022 Developer PowerShell:

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release --output-on-failure
```

Outputs:

```text
out\build\windows-x64-release\GpuThermalGuard.exe
out\build\windows-x64-release\GpuThermalGuardProbe.exe
```

The probe is read-only and never calls an NVML setter.

## Logs and snapshots

When the executable directory is not writable, files are stored in standard per-user/per-machine locations:

```text
%LOCALAPPDATA%\GpuThermalGuard\logs
%LOCALAPPDATA%\GpuThermalGuard\snapshots
%PROGRAMDATA%\GpuThermalGuard\logs     (service)
```

Snapshots can be created from the main window or tray menu and are also captured after a thermal trigger. Hidden-window capture renders the dashboard client area without forcing the window to the foreground.

## Beyond the desktop: a private System Monitor experiment

Long-running inference jobs are not always watched from the workstation itself. A separate, private **System Monitor** explores a remote browser dashboard: GPU/CPU activity, physical memory and commit usage, disk capacity, and GTG protection events in one view.

GTG stays local and standalone. The companion collector reads GTG logs as one information source, collects additional host metrics, and sends updates to an access-controlled web application. Remote connectivity is not part of GTG's thermal decision loop. Log timestamps and stale-data indicators distinguish historical evidence from live state.

<img src="docs/images/remote-system-monitor.png" alt="Private System Monitor overview with GTG protection events and resource charts" width="760">

Metric cards open a detail view with selectable time ranges and chart scaling. The VRAM example also shows a private, fixed-action WSL workflow for stopping or launching an inference model; those actions belong to the companion, not to GTG's protection controller. The screenshot includes historical failures as well as a later successful action.

<details>
<summary>View the private VRAM detail and inference-workflow example</summary>

<img src="docs/images/remote-vram-detail.png" alt="Private VRAM detail with model selection, bounded WSL actions, and action history" width="760">

</details>

**Preview only:** System Monitor, its collector, backend, credentials, and remote controls are not open source or included in this repository or GTG releases. These screenshots illustrate a possible integration, not an available GTG feature or a hosted service offered to users. GTG requires no cloud account or network connection for local protection.

## Current limitations

- Windows and NVIDIA NVML only.
- The current release protects one selected/default GPU; multi-GPU policy is not complete.
- No installer or service-management UI yet.
- No guarantee against every TDR, driver reset, sudden sensor failure, or hardware fault.
- Exclusive fullscreen overlays are not guaranteed; the OSD uses no injection or graphics hook.
- Hardware-specific safe power and temperature values must be chosen responsibly.

## Release integrity

Published releases are tag-driven. The release workflow validates version consistency, builds and tests on GitHub Actions, produces `SHA256SUMS.txt`, and publishes a build-provenance attestation. When an Authenticode certificate is configured, the executables are signed and verified before packaging. Until a certificate is available, releases are published only as clearly labeled **unsigned prereleases**, including `unsigned` in the archive name and a warning in the release notes. Release notes are taken from [CHANGELOG.md](CHANGELOG.md).

After downloading a release archive, verify that its exact bytes were produced by this repository's GitHub Actions workflow:

```powershell
gh attestation verify .\GpuThermalGuard-<version>-unsigned-windows-x64.zip -R allenk/GpuThermalGuard
```

This provenance check does not replace Authenticode publisher identity or malware scanning. Windows SmartScreen may warn about unsigned prerelease executables.

## Contributing and security

See [CONTRIBUTING.md](CONTRIBUTING.md) for development guidelines. Please report security-sensitive issues privately as described in [SECURITY.md](SECURITY.md).

## Author

**AllenK (kwyshell)** — [GitHub](https://github.com/allenk) · [Medium](https://medium.com/@allenkuo)

## License

GpuThermalGuard is released under the [MIT License](LICENSE). Vendored WTL headers use the Microsoft Public License, and the NVIDIA NVML header retains NVIDIA's license notice. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
