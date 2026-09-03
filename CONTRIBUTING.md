# Contributing

Thanks for helping improve GpuThermalGuard. Thermal protection software can affect running GPU workloads, so changes should remain small, testable, and explicit about failure behavior.

## Development setup

Use Windows x64, Visual Studio 2022 with Desktop development with C++, CMake 3.24+, and Ninja. From a VS2022 Developer PowerShell:

```powershell
cmake --preset windows-x64-dev
cmake --build --preset windows-x64-dev
ctest --preset windows-x64-dev --output-on-failure
```

## Pull requests

- Explain the user-visible problem and the safety impact.
- Add or update a deterministic test for protection-state changes.
- Keep NVML writes behind read-back verification.
- Do not add undocumented NVAPI/NVML calls, firmware modification, fan-control hacks, or GPU-rendered UI dependencies.
- Keep the Release build standalone and statically linked to the MSVC runtime.
- Update `CHANGELOG.md` for user-visible changes.

Do not intentionally create a TDR merely to test this project. Hardware stress testing and temperature/power choices remain the contributor's responsibility.
