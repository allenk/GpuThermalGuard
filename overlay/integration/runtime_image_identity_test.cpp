// Path decoding, file identity and method ownership. The alias case is the one
// that refused a legitimate runtime: two paths, one file.
#include <windows.h>
#include "runtime_image_identity.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int failures = 0;

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++failures;
    }
}

// Owned temporary files, removed however this test leaves.
struct OwnedFiles {
    wchar_t first[MAX_PATH]{};
    wchar_t other[MAX_PATH]{};
    std::filesystem::path alias;

    ~OwnedFiles() {
        if (!alias.empty()) DeleteFileW(alias.c_str());
        if (first[0]) DeleteFileW(first);
        if (other[0]) DeleteFileW(other);
    }
};
}  // namespace

int main() {
    using namespace gtg::overlay::integration;

    const auto unicode = RuntimePath("C:/\xE9\x81\x8A\xE6\x88\xB2/D3D12Core.dll");
    Check(unicode && unicode->generic_wstring() == L"C:/\u904A\u6232/D3D12Core.dll",
          "a reported path is UTF-8, whatever the code page holds");
    Check(unicode && RuntimeNarrowPath(*unicode) == "C:/\xE9\x81\x8A\xE6\x88\xB2/D3D12Core.dll",
          "and narrows back to the same bytes");
    Check(!RuntimePath("C:/\xC0\xAF/D3D12Core.dll"),
          "malformed UTF-8 refuses; a replacement character names another file");
    Check(!RuntimePath(std::string_view("C:/x\0/D3D12Core.dll", 19)), "an embedded NUL refuses");
    Check(!RuntimePath(""), "an empty path refuses");

    wchar_t directory[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH, directory) > 0, "temporary directory");
    OwnedFiles files;
    Check(GetTempFileNameW(directory, L"gtg", 0, files.first) != 0, "first owned file");
    Check(GetTempFileNameW(directory, L"gtg", 0, files.other) != 0, "second owned file");
    files.alias = std::filesystem::path(files.first).wstring() + L".alias";
    Check(CreateHardLinkW(files.alias.c_str(), files.first, nullptr) != FALSE, "owned hard link");
    Check(SameRuntimeFile(files.first, files.alias), "two paths, one file: a match");
    Check(!SameRuntimeFile(files.first, files.other), "two files: never a match");
    Check(!SameRuntimeFile(files.first, files.alias.wstring() + L".missing"),
          "an answer the filesystem cannot give is a refusal, not a match");

    wchar_t own[MAX_PATH]{};
    Check(GetModuleFileNameW(nullptr, own, MAX_PATH) != 0, "own image path");
    const auto owner = MethodOwnerPath(reinterpret_cast<const void*>(&Check));
    Check(owner && SameRuntimeFile(*owner, own), "a method answers with the image that owns it");
    Check(!MethodOwnerPath(nullptr), "an address in no image owns nothing");

    if (failures == 0) std::puts("PASS runtime image identity");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
