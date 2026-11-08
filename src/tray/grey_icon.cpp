#include "tray/grey_icon.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

namespace gtg::tray {
namespace {

// Larger than any tray icon; LoadImage's frames stop at 256.
constexpr int kLargestIcon = 256;

struct OwnedBitmap {
    HBITMAP handle{};

    ~OwnedBitmap() {
        if (handle != nullptr) DeleteObject(handle);
    }
};

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
    GreyPixels(pixels);

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

HICON GreyIcon(const HICON icon) noexcept {
    try {
        return Compose(icon);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

}  // namespace gtg::tray
