// Target-format admission, per backend. Regression for 2026-10-06: a DX11
// game's swapchain is R8G8B8A8_UNORM_SRGB, the DX11 texture path
// already samples through an sRGB view for it, but the shared SDR whitelist
// refused the format, so the overlay hooked, published and never drew.
#include "sdr_target_format.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
void Check(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++failures;
    }
}
}  // namespace

int main() {
    using namespace gtg::overlay::integration;

    // DX11: the regression, and the sRGB set D3D11Texture::SetTargetFormat handles.
    Check(MatchesD3D11Buffer(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 3840, 2160,
                             DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 3840, 2160, 1),
          "DX11: an RGBA8 sRGB back buffer is admitted");
    Check(IsSupportedD3D11Target(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB), "DX11: BGRA8 sRGB");
    Check(IsSupportedD3D11Target(DXGI_FORMAT_B8G8R8X8_UNORM_SRGB), "DX11: BGRX8 sRGB");
    Check(IsSupportedD3D11Target(DXGI_FORMAT_R8G8B8A8_UNORM) &&
              IsSupportedD3D11Target(DXGI_FORMAT_B8G8R8A8_UNORM) &&
              IsSupportedD3D11Target(DXGI_FORMAT_R10G10B10A2_UNORM),
          "DX11: the existing SDR packings are still admitted");

    // DX11: every other guard still holds for sRGB.
    Check(!MatchesD3D11Buffer(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 3840, 2160,
                              DXGI_FORMAT_R8G8B8A8_UNORM, 3840, 2160, 1),
          "DX11: declared sRGB with a UNORM buffer is refused, not treated as compatible");
    Check(!MatchesD3D11Buffer(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 3840, 2160,
                              DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1920, 2160, 1),
          "DX11: stale dimensions refused");
    Check(!MatchesD3D11Buffer(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 3840, 2160,
                              DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 3840, 2160, 4),
          "DX11: multisampled sRGB refused");
    Check(!MatchesD3D11Buffer(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0, 0,
                              DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0, 0, 1),
          "DX11: zero size refused");
    Check(!IsSupportedD3D11Target(DXGI_FORMAT_R16G16B16A16_FLOAT), "DX11: FP16 (HDR) refused");
    Check(!IsSupportedD3D11Target(DXGI_FORMAT_R8G8B8A8_TYPELESS), "DX11: typeless refused");
    Check(!IsSupportedD3D11Target(DXGI_FORMAT_UNKNOWN), "DX11: unknown refused");

    // DX12 and the shared rules: unchanged. sRGB stays out of the shared
    // whitelist, so the D3D12 renderer and color provenance mean what they did.
    Check(!IsSupportedSdrTarget(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), "shared: RGBA8 sRGB not added");
    Check(SelectSdrPacking(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) == SdrPacking::Unsupported,
          "shared: no DX12 packing for BGRA8 sRGB");
    Check(!MatchesSdrBuffer(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 3840, 2160,
                            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 3840, 2160, 1),
          "shared: DX12 buffer match still refuses sRGB");
    Check(MatchesSdrBuffer(DXGI_FORMAT_R8G8B8A8_UNORM, 640, 360,
                           DXGI_FORMAT_R8G8B8A8_UNORM, 640, 360, 1),
          "shared: UNORM still matches");

    if (failures == 0) std::puts("PASS target-format admission");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
