#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update/digest.hpp"
#include "update/http.hpp"
#include "update/manifest.hpp"

// Reading an update source: the releases of GTG's public repository on GitHub,
// or a UNC folder the administrator set, each holding gtg-update.json and the
// files it names. Every read is bounded by what the manifest allows, and a file
// is staged from the bytes that were hashed, never copied from the source a
// second time: the source can change between a check and a copy, and a copy
// would bring a Zone.Identifier stream along.
namespace gtg::update {

// Where updates come from when the administrator has set no source.
inline constexpr wchar_t kPublicRepository[] = L"https://github.com/allenk/GpuThermalGuard";

struct UpdateSource {
    enum class Kind {
        // A repository's GitHub releases: the manifest from the latest
        // release, the files from the release it names.
        Releases,
        // A UNC folder.
        Folder,
    };

    Kind kind{Kind::Releases};
    // The repository's URL, or the folder.
    std::wstring location{kPublicRepository};
    // For Releases: GitHub and the hosts its downloads redirect to.
    FetchPolicy policy;
    // For Releases: the user agent, GpuThermalGuard/<version>.
    std::wstring agent{L"GpuThermalGuard"};
};

// The public repository; `agent` names this build.
[[nodiscard]] UpdateSource ReleasesSource(std::wstring_view agent);
[[nodiscard]] UpdateSource FolderSource(std::wstring folder);
// What the administrator set: empty is the public repository.
[[nodiscard]] UpdateSource SourceOf(const std::wstring& configured, std::wstring_view agent);
// For the reader: "GitHub (allenk/GpuThermalGuard)" or the folder.
[[nodiscard]] std::wstring Describe(const UpdateSource& source);

[[nodiscard]] std::wstring ManifestUrl(const UpdateSource& source);
// A release's files are under its tag, v<version>.
[[nodiscard]] std::wstring ReleaseFileUrl(const UpdateSource& source, const Version& version,
                                          std::string_view name);

// A folder is read every 15 minutes; GitHub once a day, which is plenty for
// releases and keeps an installed base from asking it all the time.
inline constexpr std::uint64_t kFolderCheckIntervalMs = 15 * 60'000;
inline constexpr std::uint64_t kReleasesCheckIntervalMs = 24 * 60 * 60'000;

[[nodiscard]] constexpr std::uint64_t CheckIntervalMs(const UpdateSource::Kind kind) noexcept {
    return kind == UpdateSource::Kind::Folder ? kFolderCheckIntervalMs : kReleasesCheckIntervalMs;
}

// After a failed check the next one comes within the hour: a machine that
// starts before its network is up should not wait a day for an answer.
inline constexpr std::uint64_t kRetryAfterFailureMs = 60 * 60'000;

[[nodiscard]] constexpr std::uint64_t NextCheckMs(const UpdateSource::Kind kind,
                                                  const bool failed) noexcept {
    const std::uint64_t interval = CheckIntervalMs(kind);
    return failed && interval > kRetryAfterFailureMs ? kRetryAfterFailureMs : interval;
}

// The SHA-256 of the certificate the public releases are signed with: the
// project's own, self-signed (tools/update/public-release-signer.cer). Windows
// still shows their publisher as unknown; what it buys is that a machine updating
// from GitHub installs only files that certificate signed, not whatever a
// release happens to hold.
inline constexpr Digest kPublicReleaseSigner{
    0x05, 0x0B, 0xF4, 0xD6, 0x8B, 0x72, 0xAA, 0xA8, 0x7C, 0x74, 0x30, 0xDD, 0xF1, 0xF7, 0x24, 0xCA,
    0xBF, 0x37, 0x7C, 0x13, 0xBA, 0xCB, 0x25, 0xC0, 0x7D, 0xEA, 0x05, 0x38, 0x40, 0x99, 0x2D, 0x30,
};

// The certificate staged files must be signed with: the administrator's pin
// for any source when one is set; otherwise the public releases' own for
// GitHub; none for a folder without a pin.
[[nodiscard]] constexpr std::optional<Digest> PinInEffect(const std::optional<Digest>& configured,
                                                          const UpdateSource::Kind kind) noexcept {
    if (configured) return configured;
    if (kind == UpdateSource::Kind::Releases) return kPublicReleaseSigner;
    return std::nullopt;
}

// \\host\share, optionally followed by folders: what an elevated GTG can reach
// (it does not see the user's mapped drive letters). No \\?\ or \\.\ forms, no
// "." or ".." folder, no characters Windows refuses in a name; at most 260
// characters; one trailing backslash is allowed.
[[nodiscard]] bool IsValidSource(std::wstring_view source) noexcept;

struct ReadResult {
    enum class Status {
        Ok,
        Missing,
        TooLarge,
        Failed,
    };

    Status status{Status::Failed};
    DWORD error{};
    std::vector<std::uint8_t> bytes;
};

// At most max_bytes; a longer file is TooLarge and nothing of it is kept.
[[nodiscard]] ReadResult ReadBounded(const std::filesystem::path& file, std::uint64_t max_bytes);

struct SourceManifest {
    std::optional<Manifest> manifest;
    // Empty when the manifest was read and accepted. Otherwise what to tell
    // the reader, and what to journal.
    std::wstring problem;
    std::string detail;
};

// A folder: refuses one that is not a valid UNC path, then LoadManifest.
// Releases: the latest release's gtg-update.json, then ParseManifest.
[[nodiscard]] SourceManifest ReadManifest(const UpdateSource& source, const Sha256& sha);

// Reads and checks gtg-update.json in a folder, whatever kind of path it is.
[[nodiscard]] SourceManifest LoadManifest(const std::filesystem::path& folder, const Sha256& sha);

struct StagedFile {
    std::string name;
    std::filesystem::path path;
    std::uint64_t size{};
    Digest sha256{};
};

struct StageResult {
    std::filesystem::path folder;
    std::vector<StagedFile> files;
    // Empty when every file was staged.
    std::wstring problem;

    [[nodiscard]] bool Ok() const noexcept { return problem.empty(); }
};

// The first 16 hex digits of the release: the staged folder's name.
[[nodiscard]] std::wstring ReleaseFolderName(const Digest& release);

// Reads each file of the variant from the source into memory, checks its size
// and SHA-256 against the manifest, and writes those bytes as a new file into
// <work>\staged\<release:16>\, which is emptied first. A release has no
// folders: a variant with a dir is refused there.
[[nodiscard]] StageResult Stage(const UpdateSource& source, const Manifest& manifest,
                                const ManifestVariant& variant, const std::filesystem::path& work,
                                const Sha256& sha);

// Removes every staged release but the one named.
void RemoveStagedExcept(const std::filesystem::path& work, std::wstring_view keep) noexcept;

}  // namespace gtg::update
