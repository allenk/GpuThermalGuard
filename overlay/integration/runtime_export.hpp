// The Agility SDK version a D3D12Core.dll implements, read from its export
// data without loading or executing the image.
#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace gtg::overlay::integration {
// One exported data value identifies the runtime a target selected. It is the
// same value the game itself declares to the loader.
inline constexpr std::string_view kCoreSdkVersionExport = "D3D12SDKVersion";

// A malformed or hostile export table must not turn into a long scan, so the
// table is bounded well above what a runtime declares and refused beyond it.
inline constexpr DWORD kMaxCoreExports = 4096;

// Reads are bounded to the bytes consumed — four small requests, not the image.
// A ReadProcessMemory call that spans an unreadable page fails as a whole, and
// an image legitimately holds pages that have nothing to do with this value.
template <typename Reader>
inline std::optional<std::uint32_t> CoreSdkVersion(std::size_t image_size, Reader&& read) {
    const auto read_at = [&](std::size_t offset, auto& output) {
        return offset <= image_size && sizeof(output) <= image_size - offset &&
               read(offset, &output, sizeof(output));
    };
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read_at(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        !read_at(static_cast<std::size_t>(dos.e_lfanew), nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT ||
        nt.OptionalHeader.SizeOfImage != image_size)
        return std::nullopt;
    const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
        directory.VirtualAddress > image_size ||
        directory.Size > image_size - directory.VirtualAddress)
        return std::nullopt;
    IMAGE_EXPORT_DIRECTORY exports{};
    if (!read_at(directory.VirtualAddress, exports) || !exports.NumberOfFunctions ||
        exports.NumberOfFunctions > kMaxCoreExports || exports.NumberOfNames > kMaxCoreExports ||
        !exports.AddressOfFunctions || !exports.AddressOfNames || !exports.AddressOfNameOrdinals)
        return std::nullopt;
    for (std::size_t i = 0; i < exports.NumberOfNames; ++i) {
        DWORD name_rva{};
        WORD ordinal{};
        if (!read_at(static_cast<std::size_t>(exports.AddressOfNames) + i * sizeof(DWORD),
                     name_rva) ||
            !read_at(static_cast<std::size_t>(exports.AddressOfNameOrdinals) + i * sizeof(WORD),
                     ordinal) ||
            ordinal >= exports.NumberOfFunctions || !name_rva)
            return std::nullopt;
        std::array<char, kCoreSdkVersionExport.size() + 1> name{};
        if (!read_at(name_rva, name)) return std::nullopt;
        if (std::memcmp(name.data(), kCoreSdkVersionExport.data(), kCoreSdkVersionExport.size()) !=
                0 ||
            name[kCoreSdkVersionExport.size()] != '\0')
            continue;
        // An export whose address lands inside the export directory is a
        // forwarder: the bytes there are a target name, not the value.
        DWORD value_rva{};
        DWORD version{};
        if (!read_at(static_cast<std::size_t>(exports.AddressOfFunctions) + ordinal * sizeof(DWORD),
                     value_rva) ||
            !value_rva ||
            (value_rva >= directory.VirtualAddress &&
             value_rva - directory.VirtualAddress < directory.Size) ||
            !read_at(value_rva, version) || !version)
            return std::nullopt;
        return version;
    }
    return std::nullopt;
}

inline std::optional<std::uint32_t> CoreSdkVersion(std::span<const std::byte> image) {
    return CoreSdkVersion(image.size(), [&](std::size_t offset, void* output, std::size_t bytes) {
        std::memcpy(output, image.data() + offset, bytes);
        return true;
    });
}
}  // namespace gtg::overlay::integration
