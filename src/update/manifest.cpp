#include "update/manifest.hpp"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>

#include "update/json.hpp"

namespace gtg::update {
namespace {

bool IsDigit(const char c) noexcept {
    return c >= '0' && c <= '9';
}

bool IsAlnum(const char c) noexcept {
    return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char Lower(const char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoringCase(const std::string_view a, const std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](const char x, const char y) { return Lower(x) == Lower(y); });
}

// Windows opens a device for these names whatever the folder or extension.
bool IsReservedDevice(const std::string_view name) noexcept {
    const std::string_view base = name.substr(0, name.find('.'));
    for (const std::string_view device : {"con", "prn", "aux", "nul"}) {
        if (EqualsIgnoringCase(base, device)) return true;
    }
    if (base.size() == 4 && IsDigit(base[3])) {
        const std::string_view stem = base.substr(0, 3);
        if (EqualsIgnoringCase(stem, "com") || EqualsIgnoringCase(stem, "lpt")) return true;
    }
    return false;
}

std::optional<int> Component(const std::string_view part) noexcept {
    if (part.empty() || part.size() > 5 || (part.size() > 1 && part[0] == '0')) return std::nullopt;
    // from_chars would take a sign.
    if (!std::all_of(part.begin(), part.end(), IsDigit)) return std::nullopt;
    int value = 0;
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
    if (error != std::errc{} || end != part.data() + part.size() || value > 65535) {
        return std::nullopt;
    }
    return value;
}

bool Fixed(const std::string_view text, const std::size_t at, const std::size_t count, int& out) {
    if (at + count > text.size()) return false;
    out = 0;
    for (std::size_t i = at; i < at + count; ++i) {
        if (!IsDigit(text[i])) return false;
        out = out * 10 + (text[i] - '0');
    }
    return true;
}

// Text for a dialog: no control characters but line breaks, and none of the
// characters that reorder what follows them, which could make a note read as
// something it does not say.
std::wstring CleanNotes(const std::string_view utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), length);
    std::wstring out;
    out.reserve(wide.size());
    for (const wchar_t c : wide) {
        const bool reorders = (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) ||
                              c == 0x200E || c == 0x200F || c == 0x061C;
        if (c == L'\t') {
            out.push_back(L' ');
        } else if (c == L'\n' || (c >= 0x20 && c != 0x7F && !reorders)) {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<Digest> DigestOf(const json::Value* value) noexcept {
    if (value == nullptr || !value->IsString()) return std::nullopt;
    return ParseDigest(value->text);
}

const std::string* TextOf(const json::Value* value) noexcept {
    return value != nullptr && value->IsString() ? &value->text : nullptr;
}

ManifestResult Refuse(std::string why) {
    ManifestResult result;
    result.error = std::move(why);
    return result;
}

std::optional<ManifestFile> ParseFile(const json::Value& value, std::string& error) {
    if (!value.IsObject()) {
        error = "a file is not an object";
        return std::nullopt;
    }
    ManifestFile file;
    const std::string* name = TextOf(value.Find("name"));
    if (name == nullptr || !IsSafeName(*name)) {
        error = "a file name is missing or not a safe name";
        return std::nullopt;
    }
    file.name = *name;
    const json::Value* size = value.Find("size");
    if (size == nullptr || !size->integer || *size->integer < 1 ||
        static_cast<std::uint64_t>(*size->integer) > kMaxFileBytes) {
        error = std::format("{}: size is missing or outside 1 byte to 64 MiB", file.name);
        return std::nullopt;
    }
    file.size = static_cast<std::uint64_t>(*size->integer);
    const auto sha = DigestOf(value.Find("sha256"));
    if (!sha) {
        error = std::format("{}: sha256 is missing or not 64 hex digits", file.name);
        return std::nullopt;
    }
    file.sha256 = *sha;
    return file;
}

std::optional<ManifestVariant> ParseVariant(const std::string& name, const json::Value& value,
                                            const std::string& entry, std::string& error) {
    if (!IsSafeName(name) || !value.IsObject()) {
        error = "a variant name is not a safe name, or its value not an object";
        return std::nullopt;
    }
    ManifestVariant variant;
    variant.name = name;
    const auto build = DigestOf(value.Find("build"));
    if (!build) {
        error = std::format("{}: build is missing or not 64 hex digits", name);
        return std::nullopt;
    }
    variant.build = *build;
    if (const json::Value* dir = value.Find("dir"); dir != nullptr) {
        if (!dir->IsString() || (!dir->text.empty() && !IsSafeDir(dir->text))) {
            error = std::format("{}: dir is not a safe relative folder", name);
            return std::nullopt;
        }
        variant.dir = dir->text;
    }
    const json::Value* files = value.Find("files");
    if (files == nullptr || !files->IsArray() || files->items.empty() ||
        files->items.size() > kMaxFilesPerVariant) {
        error = std::format("{}: files must list 1 to {} files", name, kMaxFilesPerVariant);
        return std::nullopt;
    }
    for (const json::Value& item : files->items) {
        auto file = ParseFile(item, error);
        if (!file) return std::nullopt;
        // Windows names ignore case: two of these would be one file.
        for (const ManifestFile& seen : variant.files) {
            if (EqualsIgnoringCase(seen.name, file->name)) {
                error = std::format("{}: {} is listed twice", name, file->name);
                return std::nullopt;
            }
        }
        variant.files.push_back(std::move(*file));
    }
    const ManifestFile* executable = variant.Find(entry);
    if (executable == nullptr) {
        error = std::format("{}: the entry {} is not among its files", name, entry);
        return std::nullopt;
    }
    if (executable->sha256 != variant.build) {
        error = std::format("{}: build is not the entry's sha256", name);
        return std::nullopt;
    }
    return variant;
}

}  // namespace

std::optional<Version> ParseVersion(const std::string_view text) noexcept {
    const std::size_t first = text.find('.');
    if (first == std::string_view::npos) return std::nullopt;
    const std::size_t second = text.find('.', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    const auto major = Component(text.substr(0, first));
    const auto minor = Component(text.substr(first + 1, second - first - 1));
    const auto patch = Component(text.substr(second + 1));
    if (!major || !minor || !patch) return std::nullopt;
    return Version{*major, *minor, *patch};
}

std::string FormatVersion(const Version& version) {
    return std::format("{}.{}.{}", version.major, version.minor, version.patch);
}

bool IsSafeName(const std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxNameLength || !IsAlnum(name.front()) ||
        name.back() == '.') {
        return false;
    }
    for (const char c : name) {
        if (!IsAlnum(c) && c != '.' && c != '_' && c != '-') return false;
    }
    return !IsReservedDevice(name);
}

bool IsSafeDir(const std::string_view dir) noexcept {
    if (dir.empty() || dir.size() > kMaxDirLength) return false;
    std::size_t segments = 0;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = dir.find('/', begin);
        const std::string_view segment = dir.substr(begin, end - begin);
        if (!IsSafeName(segment) || ++segments > 8) return false;
        if (end == std::string_view::npos) return true;
        begin = end + 1;
    }
}

std::optional<std::int64_t> ParseUtcMs(const std::string_view text) noexcept {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (text.size() < 20 || !Fixed(text, 0, 4, year) || text[4] != '-' ||
        !Fixed(text, 5, 2, month) || text[7] != '-' || !Fixed(text, 8, 2, day) || text[10] != 'T' ||
        !Fixed(text, 11, 2, hour) || text[13] != ':' || !Fixed(text, 14, 2, minute) ||
        text[16] != ':' || !Fixed(text, 17, 2, second)) {
        return std::nullopt;
    }
    std::size_t at = 19;
    int milliseconds = 0;
    if (text[at] == '.') {
        ++at;
        const std::size_t begin = at;
        while (at < text.size() && IsDigit(text[at]))
            ++at;
        const std::size_t digits = at - begin;
        if (digits < 1 || digits > 7) return std::nullopt;
        for (std::size_t i = 0; i < 3; ++i) {
            milliseconds = milliseconds * 10 + (i < digits ? text[begin + i] - '0' : 0);
        }
    }
    if (at + 1 != text.size() || text[at] != 'Z') return std::nullopt;
    if (hour > 23 || minute > 59 || second > 59) return std::nullopt;
    const std::chrono::year_month_day date{std::chrono::year{year},
                                           std::chrono::month{static_cast<unsigned>(month)},
                                           std::chrono::day{static_cast<unsigned>(day)}};
    if (!date.ok()) return std::nullopt;
    const std::int64_t days = std::chrono::sys_days{date}.time_since_epoch().count();
    return ((days * 24 + hour) * 60 + minute) * 60'000LL + second * 1'000LL + milliseconds;
}

const ManifestFile* ManifestVariant::Find(const std::string_view file) const noexcept {
    for (const ManifestFile& candidate : files) {
        if (EqualsIgnoringCase(candidate.name, file)) return &candidate;
    }
    return nullptr;
}

const ManifestVariant* Manifest::Find(const std::string_view variant) const noexcept {
    for (const ManifestVariant& candidate : variants) {
        if (candidate.name == variant) return &candidate;
    }
    return nullptr;
}

std::optional<Digest> ReleaseOf(const std::vector<ManifestVariant>& variants, const Sha256& sha) {
    std::vector<const ManifestVariant*> sorted;
    for (const ManifestVariant& variant : variants)
        sorted.push_back(&variant);
    std::sort(sorted.begin(), sorted.end(),
              [](const ManifestVariant* a, const ManifestVariant* b) { return a->name < b->name; });
    std::string identity;
    for (const ManifestVariant* variant : sorted) {
        if (!identity.empty()) identity.push_back('\n');
        identity += variant->name;
        identity.push_back('=');
        identity += ToHex(variant->build);
    }
    return sha.Of(identity);
}

ManifestResult ParseManifest(const std::string_view bytes, const Sha256& sha) {
    json::Limits limits;
    limits.max_bytes = kMaxManifestBytes;
    const json::ParseResult parsed = json::Parse(bytes, limits);
    if (!parsed.value) return Refuse("not JSON: " + parsed.error);
    const json::Value& root = *parsed.value;
    if (!root.IsObject()) return Refuse("not a JSON object");

    const json::Value* format = root.Find("format");
    if (format == nullptr || format->integer != 1) return Refuse("format is not 1");
    Manifest manifest;
    const std::string* version = TextOf(root.Find("version"));
    const auto parsed_version = version ? ParseVersion(*version) : std::nullopt;
    if (!parsed_version) return Refuse("version is not MAJOR.MINOR.PATCH");
    manifest.version = *parsed_version;
    const auto release = DigestOf(root.Find("release"));
    if (!release) return Refuse("release is missing or not 64 hex digits");
    manifest.release = *release;
    const std::string* published = TextOf(root.Find("published"));
    const auto published_ms = published ? ParseUtcMs(*published) : std::nullopt;
    if (!published_ms) return Refuse("published is not an ISO-8601 UTC time");
    manifest.published = *published;
    manifest.published_unix_ms = *published_ms;
    if (const json::Value* notes = root.Find("notes"); notes != nullptr) {
        if (!notes->IsString()) return Refuse("notes is not a string");
        manifest.notes = CleanNotes(notes->text);
    }
    const std::string* entry = TextOf(root.Find("entry"));
    if (entry == nullptr || !IsSafeName(*entry))
        return Refuse("entry is missing or not a safe name");
    manifest.entry = *entry;

    const json::Value* variants = root.Find("variants");
    if (variants == nullptr || !variants->IsObject() || variants->items.empty() ||
        variants->items.size() > kMaxVariants) {
        return Refuse(std::format("variants must hold 1 to {} variants", kMaxVariants));
    }
    for (std::size_t i = 0; i < variants->items.size(); ++i) {
        std::string error;
        auto variant = ParseVariant(variants->keys[i], variants->items[i], manifest.entry, error);
        if (!variant) return Refuse(std::move(error));
        manifest.variants.push_back(std::move(*variant));
    }
    std::sort(manifest.variants.begin(), manifest.variants.end(),
              [](const ManifestVariant& a, const ManifestVariant& b) { return a.name < b.name; });

    const auto expected = ReleaseOf(manifest.variants, sha);
    if (!expected) return Refuse("SHA-256 is unavailable");
    if (*expected != manifest.release) return Refuse("release does not match its variants");
    ManifestResult result;
    result.manifest = std::move(manifest);
    return result;
}

}  // namespace gtg::update
