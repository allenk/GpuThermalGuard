// The SDK version is read from an image this process never loads, through
// bounded requests. Every case here is a way that image can be wrong, plus the
// one that matters in the field: pages that are legitimately unreadable.
#include "runtime_export.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++failures;
    }
}

using Image = std::array<std::byte, 4096>;

template <typename T>
void Put(Image& image, std::size_t offset, const T& value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

// A minimal PE64 that exports one data value, which is all this reader needs.
Image Fixture() {
    Image image{};
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 128;
    Put(image, 0, dos);
    IMAGE_NT_HEADERS64 nt{};
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(image.size());
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {512, 128};
    Put(image, 128, nt);
    IMAGE_EXPORT_DIRECTORY exports{};
    exports.NumberOfNames = exports.NumberOfFunctions = 1;
    exports.AddressOfNames = 768;
    exports.AddressOfFunctions = 800;
    exports.AddressOfNameOrdinals = 832;
    Put(image, 512, exports);
    Put(image, 768, DWORD{1024});
    Put(image, 800, DWORD{1280});
    Put(image, 832, WORD{0});
    constexpr char kName[] = "D3D12SDKVersion";
    std::memcpy(image.data() + 1024, kName, sizeof(kName));
    Put(image, 1280, DWORD{619});
    return image;
}

// Pages this test owns, one of which is made unreadable on purpose.
struct OwnedPages {
    void* value = VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    ~OwnedPages() {
        if (value) VirtualFree(value, 0, MEM_RELEASE);
    }
};
}  // namespace

int main() {
    using gtg::overlay::integration::CoreSdkVersion;

    auto image = Fixture();
    Check(CoreSdkVersion(image) == 619U, "the exported value is what the runtime declares");

    const auto sparse_read = [&](std::size_t offset, void* output, std::size_t bytes) {
        if (offset > image.size() || bytes > image.size() - offset || offset + bytes > 2048)
            return false;
        std::memcpy(output, image.data() + offset, bytes);
        return true;
    };
    Check(CoreSdkVersion(image.size(), sparse_read) == 619U,
          "ranges this value does not need are never asked for");
    Check(!CoreSdkVersion(image.size(),
                          [&](std::size_t offset, void* output, std::size_t bytes) {
                              return offset != 1280 && sparse_read(offset, output, bytes);
                          }),
          "a value that cannot be read refuses");

    OwnedPages pages;
    Check(pages.value != nullptr, "owned pages");
    if (pages.value) {
        std::memcpy(pages.value, image.data(), image.size());
        auto* base = static_cast<std::byte*>(pages.value);
        const DWORD sparse_size = 8192;
        std::memcpy(base + 128 + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                        offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage),
                    &sparse_size, sizeof(sparse_size));
        DWORD old_protection{};
        Check(VirtualProtect(base + 4096, 4096, PAGE_NOACCESS, &old_protection) != FALSE,
              "an unreadable page, as a real image has");
        const auto remote_read = [&](std::size_t offset, void* output, std::size_t bytes) {
            SIZE_T read{};
            return ReadProcessMemory(GetCurrentProcess(), base + offset, output, bytes, &read) &&
                   read == bytes;
        };
        Check(CoreSdkVersion(sparse_size, remote_read) == 619U,
              "an unrelated PAGE_NOACCESS range does not refuse the discovery");
        const DWORD unreadable_sdk = 4096;
        std::memcpy(base + 800, &unreadable_sdk, sizeof(unreadable_sdk));
        Check(!CoreSdkVersion(sparse_size, remote_read), "an unreadable SDK value does refuse it");
    }

    Check(!CoreSdkVersion(std::span(image).first(32)), "a truncated image refuses");
    Put(image, 60, LONG{-1});
    Check(!CoreSdkVersion(image), "a negative PE offset refuses");
    image = Fixture();
    Put(image, 832, WORD{1});
    Check(!CoreSdkVersion(image), "an ordinal outside the table refuses");
    image = Fixture();
    Put(image, 800, DWORD{4094});
    Check(!CoreSdkVersion(image), "a value outside the image refuses");
    image = Fixture();
    // Point the entry at NumberOfFunctions inside the export directory, which
    // is a nonzero DWORD: without the forwarder rule this reads 1 and reports
    // it as a version, so the case cannot pass for any other reason.
    Put(image, 800, DWORD{512 + offsetof(IMAGE_EXPORT_DIRECTORY, NumberOfFunctions)});
    Check(!CoreSdkVersion(image), "a forwarded export is a name, not a value");
    image = Fixture();
    Put(image, 1280, DWORD{0});
    Check(!CoreSdkVersion(image), "a zero version is no version");
    image = Fixture();
    image[1024] = std::byte{'X'};
    Check(!CoreSdkVersion(image), "an image without the export refuses");

    if (failures == 0) std::puts("PASS bounded Core SDK export parsing");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
