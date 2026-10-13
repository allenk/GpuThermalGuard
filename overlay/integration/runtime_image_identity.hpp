// Which file a discovered method came from, and whether that is the file the
// target has loaded. Two paths can name one image, and one name can mean two.
#pragma once
#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace gtg::overlay::integration {
// A method report writes module paths as UTF-8 with forward slashes. Decoding
// one through the active code page would corrupt every path outside it, so the
// decoding is explicit and malformed input is refused, never substituted: a
// replacement character would name a different file that might exist.
inline std::optional<std::filesystem::path> RuntimePath(std::string_view utf8) {
    constexpr std::size_t kMaxPathBytes = 32767U * 4U;
    if (utf8.empty() || utf8.size() > kMaxPathBytes) return std::nullopt;
    if (utf8.find('\0') != std::string_view::npos) return std::nullopt;
    const auto bytes = static_cast<int>(utf8.size());
    const auto count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), bytes, nullptr, 0);
    if (!count) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), bytes, wide.data(),
                            count) != count)
        return std::nullopt;
    return std::filesystem::path(wide);
}

// The same path narrow again, which is what the report wrote and what the
// runtime selection APIs take. char8_t exists to put the encoding in the type
// system, and then it has to be spelled back out to reach a C interface.
inline std::string RuntimeNarrowPath(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
}

// One backing file can be presented under more than one path: a packaged game's
// runtime is reachable through both the package root and the library folder the
// title was installed into. Comparing the two strings refuses an alias of the
// very file the discovery read, so the filesystem is asked instead — and an
// answer it cannot give is a refusal, never a match.
inline bool SameRuntimeFile(const std::filesystem::path& local,
                            const std::filesystem::path& reported) noexcept {
    std::error_code error;
    const bool same = std::filesystem::equivalent(local, reported, error);
    return !error && same;
}

// The image that owns a discovered method. A device factory is asked for a
// directory; what it selected is a question only the image that answered can
// settle, and the caller needs a file to compare against the target's own.
inline std::optional<std::filesystem::path> MethodOwnerPath(const void* method) {
    HMODULE owner{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(method), &owner) ||
        !owner)
        return std::nullopt;
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(owner, path, 32768);
    if (!length || length >= 32768) return std::nullopt;
    return std::filesystem::path(path);
}
}  // namespace gtg::overlay::integration
