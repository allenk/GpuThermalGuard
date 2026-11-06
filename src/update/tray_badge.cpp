#include "update/tray_badge.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

namespace gtg::update {
namespace {

// Samples per pixel along each axis: the dot's edge gets 64 levels.
constexpr int kSamples = 8;
// Larger than any tray icon; LoadImage's frames stop at 256.
constexpr int kLargestIcon = 256;

struct OwnedBitmap {
    HBITMAP handle{};

    ~OwnedBitmap() {
        if (handle != nullptr) DeleteObject(handle);
    }
};

double Channel(const std::uint32_t pixel, const int shift) noexcept {
    return static_cast<double>((pixel >> shift) & 0xFF);
}

std::uint32_t Pack(const double value, const int shift) noexcept {
    const long rounded = std::lround(std::clamp(value, 0.0, 255.0));
    return static_cast<std::uint32_t>(rounded) << shift;
}

BITMAPINFO TopDown32(const int size) noexcept {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return info;
}

bool ReadPixels(const HBITMAP bitmap, const int size, std::vector<std::uint32_t>& pixels) {
    pixels.assign(static_cast<std::size_t>(size) * size, 0);
    BITMAPINFO info = TopDown32(size);
    const HDC screen = GetDC(nullptr);
    if (screen == nullptr) return false;
    const int rows =
        GetDIBits(screen, bitmap, 0, static_cast<UINT>(size), pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    return rows == size;
}

HICON Compose(const HICON icon) {
    ICONINFO info{};
    if (icon == nullptr || !GetIconInfo(icon, &info)) return nullptr;
    // GetIconInfo hands over copies of both bitmaps.
    const OwnedBitmap source_color{info.hbmColor};
    const OwnedBitmap source_mask{info.hbmMask};
    BITMAP shape{};
    if (source_color.handle == nullptr ||
        GetObjectW(source_color.handle, sizeof(shape), &shape) != sizeof(shape)) {
        return nullptr;
    }
    const int size = shape.bmWidth;
    if (size <= 0 || size > kLargestIcon || shape.bmHeight != size) return nullptr;
    std::vector<std::uint32_t> pixels;
    if (!ReadPixels(source_color.handle, size, pixels)) return nullptr;
    // An icon without an alpha channel keeps its transparency in the mask,
    // which reads back white where the icon is transparent.
    const bool has_alpha =
        std::any_of(pixels.begin(), pixels.end(), [](std::uint32_t p) { return (p >> 24) != 0; });
    if (!has_alpha) {
        std::vector<std::uint32_t> mask;
        if (source_mask.handle == nullptr || !ReadPixels(source_mask.handle, size, mask)) {
            return nullptr;
        }
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = (mask[i] & 0x00FFFFFF) != 0 ? 0 : pixels[i] | 0xFF000000;
        }
    }
    PaintBadge(pixels, size);

    BITMAPINFO header = TopDown32(size);
    void* bits = nullptr;
    const OwnedBitmap color{CreateDIBSection(nullptr, &header, DIB_RGB_COLORS, &bits, nullptr, 0)};
    if (color.handle == nullptr || bits == nullptr) return nullptr;
    std::memcpy(bits, pixels.data(), pixels.size() * sizeof(pixels[0]));
    // 1 where the icon is transparent; rows padded to 16 bits.
    const int stride = (size + 15) / 16 * 2;
    std::vector<std::uint8_t> mask_bits(static_cast<std::size_t>(stride) * size, 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if ((pixels[static_cast<std::size_t>(y) * size + x] >> 24) != 0) continue;
            mask_bits[static_cast<std::size_t>(y) * stride + x / 8] |=
                static_cast<std::uint8_t>(0x80 >> (x % 8));
        }
    }
    const OwnedBitmap mask{CreateBitmap(size, size, 1, 1, mask_bits.data())};
    if (mask.handle == nullptr) return nullptr;
    ICONINFO composed{};
    composed.fIcon = TRUE;
    composed.hbmMask = mask.handle;
    composed.hbmColor = color.handle;
    return CreateIconIndirect(&composed);
}

}  // namespace

void PaintBadge(const std::span<std::uint32_t> pixels, const int size) noexcept {
    if (size <= 0 || pixels.size() < static_cast<std::size_t>(size) * size) return;
    const BadgeBox box = BadgeBoxFor(size);
    const double radius = box.diameter / 2.0;
    const double inner = std::max(0.0, radius - box.rim);
    const double center_x = box.left + radius;
    const double center_y = radius;
    constexpr double kTotal = kSamples * kSamples;
    for (int y = 0; y < box.diameter; ++y) {
        for (int x = box.left; x < size; ++x) {
            int in_dot = 0;
            int in_fill = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const double dx = x + (sx + 0.5) / kSamples - center_x;
                    const double dy = y + (sy + 0.5) / kSamples - center_y;
                    const double distance = dx * dx + dy * dy;
                    if (distance <= radius * radius) ++in_dot;
                    if (distance <= inner * inner) ++in_fill;
                }
            }
            if (in_dot == 0) continue;
            // The dot over the icon, both in straight alpha.
            const double fill = in_fill / kTotal;
            const double rim = (in_dot - in_fill) / kTotal;
            const double cover = in_dot / kTotal;
            std::uint32_t& pixel = pixels[static_cast<std::size_t>(y) * size + x];
            const double below = Channel(pixel, 24) / 255.0 * (1.0 - cover);
            const double alpha = cover + below;
            std::uint32_t result = Pack(alpha * 255.0, 24);
            for (const int shift : {16, 8, 0}) {
                const double over =
                    Channel(kBadgeFill, shift) * fill + Channel(kBadgeRim, shift) * rim;
                result |= Pack((over + Channel(pixel, shift) * below) / alpha, shift);
            }
            pixel = result;
        }
    }
}

HICON WithBadge(const HICON icon) noexcept {
    try {
        return Compose(icon);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

}  // namespace gtg::update
