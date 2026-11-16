#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "topapp/cpu_ledger.hpp"

// A process's working directory: where it was started, or where it moved
// itself. For an interpreter it says which project is running when the
// executable's own path cannot -- two Pythons of one install share that.
namespace gtg::topapp {

// The longest directory read: the longest path a UNICODE_STRING can hold.
inline constexpr std::size_t kMaxDirectoryChars = 32'767;

// Two paths compared as the file system does for ASCII letters, with either
// separator.
[[nodiscard]] inline wchar_t FoldPathChar(const wchar_t c) noexcept {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
    return c == L'/' ? L'\\' : c;
}

// Whether `path` is `folder` or inside it. "C:\WindowsApps" is not inside
// "C:\Windows": the folder must end where a separator begins.
[[nodiscard]] inline bool IsWithin(const std::wstring_view path,
                                   const std::wstring_view folder) noexcept {
    std::size_t length = folder.size();
    while (length > 0 && (folder[length - 1] == L'\\' || folder[length - 1] == L'/'))
        --length;
    if (length == 0 || path.size() < length) return false;
    for (std::size_t i = 0; i < length; ++i) {
        if (FoldPathChar(path[i]) != FoldPathChar(folder[i])) return false;
    }
    return path.size() == length || path[length] == L'\\' || path[length] == L'/';
}

// The directory as a process holds it ("C:\work\") made one way: no trailing
// separator but a drive root's ("C:\"). Nothing unless it is a drive path
// ("X:\...") or a UNC path ("\\server\share..."), with no control characters:
// a device path, a relative one or a torn read is not a place to show.
[[nodiscard]] inline std::optional<std::wstring> NormalDirectory(std::wstring_view text) {
    if (text.empty() || text.size() > kMaxDirectoryChars) return std::nullopt;
    for (const wchar_t c : text) {
        if (c < 0x20 || c == 0x7F) return std::nullopt;
    }
    const bool drive =
        text.size() >= 3 &&
        ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')) &&
        text[1] == L':' && text[2] == L'\\';
    const bool unc = text.size() >= 5 && text[0] == L'\\' && text[1] == L'\\' && text[2] != L'\\' &&
                     text[2] != L'?' && text[2] != L'.';
    if (!drive && !unc) return std::nullopt;
    const std::size_t root = drive ? 3 : 2;
    while (text.size() > root && text.back() == L'\\')
        text.remove_suffix(1);
    return std::wstring(text);
}

// The directory's last folder, or nothing where the directory says nothing
// about the app: Windows' own folder or anything in it (where a program
// started by the shell is left), the folder its executable is in or anything
// in it (where an installed app works, Edge in its version folder), a drive's
// root.
[[nodiscard]] inline std::wstring FolderOf(const std::wstring_view directory,
                                           const std::wstring_view image_path,
                                           const std::wstring_view windows_directory) {
    if (directory.empty()) return {};
    if (!windows_directory.empty() && IsWithin(directory, windows_directory)) return {};
    const std::size_t slash = image_path.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos && slash > 0 &&
        IsWithin(directory, image_path.substr(0, slash))) {
        return {};
    }
    const std::size_t last = directory.find_last_of(L"\\/");
    const std::wstring_view folder =
        last == std::wstring_view::npos ? directory : directory.substr(last + 1);
    if (folder.empty() || folder.back() == L':') return {};
    return std::wstring(folder);
}

// The working directory of the process `identity` names, read from its own
// process parameters, or nothing: it has exited or its id was reused, it is a
// 32-bit process, the caller may not read it, or what was read is not a
// directory (NormalDirectory). Opens the process for limited query and memory
// reads with whatever token the calling thread has: see ProcessTokenScope.
[[nodiscard]] std::optional<std::wstring> ReadWorkingDirectory(const Identity& identity) noexcept;

// The process token on this thread, for as long as this lives. The sampler
// thread runs on a copy with the debug privilege enabled, which is for limited
// queries only: another process's memory is asked for on the process token,
// so a process whose security refuses Administrators is not read. The copy is
// put back at the end. Not ready -- read nothing -- when the thread could not
// step out of it; a thread with no copy is already on the process token.
class ProcessTokenScope {
public:
    ProcessTokenScope() noexcept;
    ~ProcessTokenScope();
    ProcessTokenScope(const ProcessTokenScope&) = delete;
    ProcessTokenScope& operator=(const ProcessTokenScope&) = delete;

    [[nodiscard]] bool Ready() const noexcept { return ready_; }

private:
    // The thread's own token, a HANDLE, while the thread is off it.
    void* saved_{};
    bool ready_{};
};

}  // namespace gtg::topapp
