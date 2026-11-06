#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update/digest.hpp"

// The release manifest, gtg-update.json, at the root of an update source.
//
// The publish script writes it last, by rename, after every file it names is in
// place. GTG believes nothing the manifest says until the rules below hold, and
// then believes only the hashes: a file is what its SHA-256 says or it is not
// used.
namespace gtg::update {

inline constexpr wchar_t kManifestFileName[] = L"gtg-update.json";
inline constexpr std::size_t kMaxManifestBytes = 64 * 1024;
inline constexpr std::uint64_t kMaxFileBytes = 64ULL * 1024 * 1024;
inline constexpr std::size_t kMaxFilesPerVariant = 16;
inline constexpr std::size_t kMaxVariants = 8;
inline constexpr std::size_t kMaxNameLength = 64;
inline constexpr std::size_t kMaxDirLength = 200;

struct Version {
    int major{};
    int minor{};
    int patch{};

    friend auto operator<=>(const Version&, const Version&) = default;
};

// MAJOR.MINOR.PATCH: digits only, no leading zeros, each at most 65535.
[[nodiscard]] std::optional<Version> ParseVersion(std::string_view text) noexcept;
[[nodiscard]] std::string FormatVersion(const Version& version);

// A file or folder name the source may use: a letter or digit, then letters,
// digits, '.', '_' or '-'; at most 64 characters; not ending in '.', which
// Windows would drop; never a reserved device name such as NUL or COM1, with or
// without an extension.
[[nodiscard]] bool IsSafeName(std::string_view name) noexcept;

// A relative folder: 1-8 safe names joined by '/'.
[[nodiscard]] bool IsSafeDir(std::string_view dir) noexcept;

// ISO-8601 UTC: YYYY-MM-DDTHH:MM:SS, then up to seven fraction digits, then Z.
[[nodiscard]] std::optional<std::int64_t> ParseUtcMs(std::string_view text) noexcept;

struct ManifestFile {
    std::string name;
    std::uint64_t size{};
    Digest sha256{};
};

struct ManifestVariant {
    std::string name;
    // The entry executable's SHA-256: the build this variant installs.
    Digest build{};
    // Where its files are, relative to the source; empty for the source root.
    std::string dir;
    std::vector<ManifestFile> files;

    [[nodiscard]] const ManifestFile* Find(std::string_view file) const noexcept;
};

struct Manifest {
    Version version;
    // Names the whole publish: the SHA-256 of every variant's build.
    Digest release{};
    std::string published;
    std::int64_t published_unix_ms{};
    // For the confirmation; control characters other than line breaks removed.
    std::wstring notes;
    std::string entry;
    // Sorted by name.
    std::vector<ManifestVariant> variants;

    [[nodiscard]] const ManifestVariant* Find(std::string_view variant) const noexcept;
};

// The SHA-256 of one line "variant=BUILD" per variant, sorted by variant name
// (ordinal), joined with '\n', BUILD in upper-case hex. The publish script
// computes the same.
[[nodiscard]] std::optional<Digest> ReleaseOf(const std::vector<ManifestVariant>& variants,
                                              const Sha256& sha);

struct ManifestResult {
    std::optional<Manifest> manifest;
    // Why it was refused, for the journal.
    std::string error;
};

[[nodiscard]] ManifestResult ParseManifest(std::string_view bytes, const Sha256& sha);

}  // namespace gtg::update
