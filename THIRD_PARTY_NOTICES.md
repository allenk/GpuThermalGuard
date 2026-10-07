# Third-party notices

GpuThermalGuard itself is licensed under the MIT License. The following components retain their own terms.

## Windows Template Library (WTL) 10.01

- Vendored as header files under `third_party/wtl`.
- Licensed under the Microsoft Public License.
- The license text is included at `third_party/wtl/MS-PL.txt`.

## NVIDIA Management Library header

- `third_party/nvml/include/nvml.h` is redistributed with NVIDIA's copyright, license, disclaimer, and U.S. Government End Users notice intact.
- GpuThermalGuard dynamically loads `nvml.dll` supplied by the installed NVIDIA display driver. The project does not redistribute `nvml.dll`.

## darkmodelib

- Git submodule at `third_party/darkmodelib` (https://github.com/ozone10/darkmodelib), used as published.
- Compiled into `GpuThermalGuard.exe` for the dark theme.
- Licensed under the Mozilla Public License 2.0, with some code under the MIT License. See `third_party/darkmodelib/LICENSE.md` and `third_party/darkmodelib/LICENSE-MIT.md`. MPL-2.0 is file-level copyleft: the source of every darkmodelib file is available in that submodule.

## Dear ImGui

- Obtained through vcpkg (`vcpkg.json`, version 1.92.9) and compiled into `gtg_overlay.dll`.
- Licensed under the MIT License, Copyright (c) 2014-2026 Omar Cornut.

## Splice

- Git submodule at `third_party/splice` (https://github.com/allenk/splice), compiled into `gtg_overlay.dll`.
- Licensed under the MIT License. See `third_party/splice/LICENSE`.
