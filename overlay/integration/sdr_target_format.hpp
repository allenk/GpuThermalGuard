#pragma once
#include <dxgiformat.h>

namespace gtg::overlay::integration {
enum class SdrPacking { Unsupported, Rgba8, Bgra8, Rgb10A2 };
inline SdrPacking SelectSdrPacking(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return SdrPacking::Rgba8;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return SdrPacking::Bgra8;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return SdrPacking::Rgb10A2;
    default: return SdrPacking::Unsupported;
    }
}
inline bool IsSupportedSdrTarget(DXGI_FORMAT format) noexcept {
    return SelectSdrPacking(format) != SdrPacking::Unsupported;
}
inline bool MatchesSdrBuffer(DXGI_FORMAT expected, unsigned long long width, unsigned height,
                             DXGI_FORMAT actual, unsigned long long actual_width,
                             unsigned actual_height, unsigned samples) noexcept {
    return IsSupportedSdrTarget(expected) && expected == actual && width && height &&
        width == actual_width && height == actual_height && samples == 1;
}
// D3D11 target admission. A superset of the SDR packings above by exactly the
// 8-bit sRGB back buffers the DX11 bitmap path already draws correctly:
// D3D11Texture::SetTargetFormat recognises them and samples the source through
// an sRGB view, and the render-target view takes the back buffer's own sRGB
// format, so the encode and decode pair up. The shared whitelist is not
// widened instead: DX12's PSO, RTV and shader do not handle sRGB, and its
// callers (SelectSdrPacking, IsSupportedSdrTarget, DecideSdrAdmission) keep
// their meaning. Regression 2026-10-06: a DX11 game's
// R8G8B8A8_UNORM_SRGB swapchain hooked and published but never drew.
inline bool IsSupportedD3D11Target(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return true;
    default:
        return IsSupportedSdrTarget(format);
    }
}
// Same guards as MatchesSdrBuffer -- declared and actual format identical,
// matching nonzero size, one sample -- over the D3D11 set.
inline bool MatchesD3D11Buffer(DXGI_FORMAT expected, unsigned long long width, unsigned height,
                               DXGI_FORMAT actual, unsigned long long actual_width,
                               unsigned actual_height, unsigned samples) noexcept {
    return IsSupportedD3D11Target(expected) && expected == actual && width && height &&
        width == actual_width && height == actual_height && samples == 1;
}
inline bool HasRequiredSdrEvidence(DXGI_FORMAT format, bool observed_sdr) noexcept {
    // Preserve legacy 8-bit admission. New 10-bit support must not infer a
    // swapchain's color space from its numeric format or the monitor state.
    return IsSupportedSdrTarget(format) &&
        (format != DXGI_FORMAT_R10G10B10A2_UNORM || observed_sdr);
}
}
