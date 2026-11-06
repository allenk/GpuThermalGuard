#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gtg::topapp {

// How wide a name may be, in half-units: a Latin letter is 2, a CJK character
// 3 -- one and a half letters, which is how wide it draws in the OSD's face
// (owner, 2026-10-16: a cap of 20 with CJK counted 1.5). Longer names are cut
// and end in an ellipsis.
inline constexpr int kMaxNameWidth = 40;
inline constexpr wchar_t kEllipsis = L'\u2026';

// The width of one UTF-16 unit in half-units. A surrogate pair is a
// supplementary character, nearly always CJK or a symbol drawn as wide: the
// high half carries the width and the low half none.
[[nodiscard]] constexpr int UnitWidth(const wchar_t c) noexcept {
    if (c >= 0xDC00 && c <= 0xDFFF) return 0;
    if (c >= 0xD800 && c <= 0xDBFF) return 3;
    const bool wide = (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) ||
                      (c >= 0x3041 && c <= 0x33FF) || (c >= 0x3400 && c <= 0x4DBF) ||
                      (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
                      (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
                      (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
                      (c >= 0xFFE0 && c <= 0xFFE6);
    return wide ? 3 : 2;
}

[[nodiscard]] constexpr int NameWidth(const std::wstring_view text) noexcept {
    int width = 0;
    for (const wchar_t c : text)
        width += UnitWidth(c);
    return width;
}

// The executable's name without its directory or extension:
// `C:\x\vllm-server.exe` reads `vllm-server`.
[[nodiscard]] inline std::wstring_view ExecutableStem(const std::wstring_view image_path) noexcept {
    const std::size_t slash = image_path.find_last_of(L"\\/");
    std::wstring_view name =
        slash == std::wstring_view::npos ? image_path : image_path.substr(slash + 1);
    const std::size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring_view::npos && dot != 0) name = name.substr(0, dot);
    return name;
}

[[nodiscard]] constexpr bool EqualsIgnoringAsciiCase(const std::wstring_view a,
                                                     const std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto fold = [](wchar_t c) {
            return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
        };
        if (fold(a[i]) != fold(b[i])) return false;
    }
    return true;
}

// The executable's name, without the build decorations games ship with:
// `GameName-Win64-Shipping` reads `GameName`, `Tool_x64` reads `Tool`.
[[nodiscard]] inline std::wstring_view ExecutableName(const std::wstring_view image_path) noexcept {
    std::wstring_view stem = ExecutableStem(image_path);
    for (const std::wstring_view suffix :
         {L"-Win64-Shipping", L"-Win64-Test", L"-Win64-Development", L"-Win64-Debug",
          L"-WinGDK-Shipping", L"-WinGDK-Test", L"_x64", L"-x64", L"_win64", L"-win64", L"_dx11",
          L"-dx11", L"_dx12", L"-dx12"}) {
        if (stem.size() > suffix.size() &&
            EqualsIgnoringAsciiCase(stem.substr(stem.size() - suffix.size()), suffix)) {
            return stem.substr(0, stem.size() - suffix.size());
        }
    }
    return stem;
}

[[nodiscard]] inline bool IsServiceHost(const std::wstring_view image_path) noexcept {
    return EqualsIgnoringAsciiCase(ExecutableStem(image_path), L"svchost");
}

// What groups processes into one app: the image path, with ASCII letters
// folded. Two processes of one app share a path; the file system ignores
// case, so the key must too. Non-ASCII letters are left as they are, which
// at worst splits an app whose path differs only in the case of a non-ASCII
// letter -- an app that names itself two ways.
[[nodiscard]] inline std::wstring GroupKey(const std::wstring_view image_path) {
    std::wstring key(image_path);
    for (wchar_t& c : key) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (c == L'/') c = L'\\';
    }
    return key;
}

// Control characters become spaces, runs of spaces become one, the ends are
// trimmed, and the trademark marks go: they add width and say nothing.
[[nodiscard]] inline std::wstring CleanName(const std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    bool space = false;
    for (wchar_t c : text) {
        if (c == 0x00AE || c == 0x2122 || c == 0x00A9) continue;
        if (c < 0x20 || c == 0x7F) c = L' ';
        if (c == L' ' || c == 0x3000) {
            space = !out.empty();
            continue;
        }
        if (space) out.push_back(L' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

// `text` without a trailing version note: `NVIDIA cicc, Version 12.4.131`
// reads `NVIDIA cicc`. The whole CUDA toolchain describes itself this way, and
// at the cap the version is what would be left in place of the name. Only the
// word "Version" followed by a number is taken; a bare number may be part of
// the name.
[[nodiscard]] constexpr std::wstring_view WithoutVersionNote(
    const std::wstring_view text) noexcept {
    std::size_t end = text.size();
    while (end > 0 && ((text[end - 1] >= L'0' && text[end - 1] <= L'9') || text[end - 1] == L'.'))
        --end;
    if (end == text.size() || text[end] < L'0' || text[end] > L'9') return text;
    constexpr std::wstring_view kWord = L" version ";
    if (end < kWord.size() ||
        !EqualsIgnoringAsciiCase(text.substr(end - kWord.size(), kWord.size()), kWord)) {
        return text;
    }
    end -= kWord.size();
    if (end > 0 && text[end - 1] == L',') --end;
    return end == 0 ? text : text.substr(0, end);
}

// The app's own name out of its description. Descriptions are often a name
// and a tagline: `qBittorrent - A Bittorrent Client` names `qBittorrent`. So
// the text before a spaced dash, a bar or a colon is kept, then a trailing
// version note goes, then a trailing bracketed note: `Foo Tool (x64)` names
// `Foo Tool`. No rule may leave nothing behind. `text` is already clean.
[[nodiscard]] inline std::wstring ShortAppName(std::wstring text) {
    for (const std::wstring_view separator :
         {L" - ", L" \u2013 ", L" \u2014 ", L" | ", L": ", L"\uFF1A"}) {
        const std::size_t at = text.find(separator);
        if (at != std::wstring::npos && at > 0) text.resize(at);
    }
    text.resize(WithoutVersionNote(text).size());
    for (int pass = 0; pass < 2; ++pass) {
        if (text.empty()) break;
        const wchar_t last = text.back();
        const wchar_t open = last == L')'    ? L'('
                             : last == L']'  ? L'['
                             : last == L'）' ? L'（'
                                             : L'\0';
        if (open == L'\0') break;
        const std::size_t at = text.rfind(open);
        if (at == std::wstring::npos || at == 0) break;
        text.resize(at);
        while (!text.empty() && text.back() == L' ')
            text.pop_back();
    }
    return text;
}

// Cuts `text` to `max_width` half-units, ending in an ellipsis. At a word
// boundary when one falls in the last third, so a word is not cut in half
// when it need not be. Never splits a surrogate pair.
[[nodiscard]] inline std::wstring CutToWidth(std::wstring text, const int max_width) {
    if (NameWidth(text) <= max_width) return text;
    const int room = max_width - UnitWidth(kEllipsis);
    int width = 0;
    std::size_t cut = 0;
    std::size_t last_space = std::wstring::npos;
    while (cut < text.size()) {
        const bool pair = text[cut] >= 0xD800 && text[cut] <= 0xDBFF && cut + 1 < text.size();
        const int step = UnitWidth(text[cut]);
        if (width + step > room) break;
        if (text[cut] == L' ') last_space = cut;
        width += step;
        cut += pair ? 2 : 1;
    }
    if (last_space != std::wstring::npos && last_space > 0 &&
        NameWidth(std::wstring_view(text).substr(0, last_space)) * 3 >= room * 2) {
        cut = last_space;
    }
    text.resize(cut);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'-' || text.back() == L',')) {
        text.pop_back();
    }
    text.push_back(kEllipsis);
    return text;
}

// The name a reader sees: the app's own short name when its description has
// one, its executable's name when it does not (owner, 2026-10-16: the app
// name first -- many services and games have executable names a reader cannot
// place -- but short). Empty only when both inputs are.
[[nodiscard]] inline std::wstring DisplayName(const std::wstring_view description,
                                              const std::wstring_view image_path) {
    std::wstring name = ShortAppName(CleanName(description));
    if (name.empty()) name = CleanName(ExecutableName(image_path));
    return CutToWidth(std::move(name), kMaxNameWidth);
}

// A service host is named for what it hosts: the first service's display
// name, and how many more share the process. The count is kept whole, so the
// name gives way to it.
[[nodiscard]] inline std::wstring ServiceHostName(const std::vector<std::wstring>& display_names) {
    std::wstring first;
    for (const std::wstring& name : display_names) {
        first = ShortAppName(CleanName(name));
        if (!first.empty()) break;
    }
    if (first.empty()) return {};
    if (display_names.size() == 1) return CutToWidth(std::move(first), kMaxNameWidth);
    const std::wstring more = L" +" + std::to_wstring(display_names.size() - 1);
    return CutToWidth(std::move(first), kMaxNameWidth - NameWidth(more)) + more;
}

}  // namespace gtg::topapp
