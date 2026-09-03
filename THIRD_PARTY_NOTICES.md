# Third-party notices

GpuThermalGuard itself is licensed under the MIT License. The following vendored components retain their own terms.

## Windows Template Library (WTL) 10.01

- Vendored as header files under `third_party/wtl`.
- Licensed under the Microsoft Public License.
- The license text is included at `third_party/wtl/MS-PL.txt`.

## NVIDIA Management Library header

- `third_party/nvml/include/nvml.h` is redistributed with NVIDIA's copyright, license, disclaimer, and U.S. Government End Users notice intact.
- GpuThermalGuard dynamically loads `nvml.dll` supplied by the installed NVIDIA display driver. The project does not redistribute `nvml.dll`.
