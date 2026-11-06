#include "update/source.hpp"

#include <algorithm>
#include <format>
#include <system_error>

#include "localization/localization.hpp"

namespace gtg::update {
namespace {

bool IsForbidden(const wchar_t c) noexcept {
    return c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'|' ||
           c == L'?' || c == L'*';
}

class File {
public:
    explicit File(const HANDLE handle) noexcept : handle_(handle) {}

    ~File() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }

    [[nodiscard]] bool Valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

std::filesystem::path SourceFile(const std::wstring& source, const std::string& dir,
                                 const std::string& name) {
    std::filesystem::path path(source);
    // Validated as safe names, so plain ASCII.
    std::size_t begin = 0;
    while (!dir.empty()) {
        const std::size_t end = dir.find('/', begin);
        path /= std::wstring(
            dir.begin() + static_cast<std::ptrdiff_t>(begin),
            end == std::string::npos ? dir.end() : dir.begin() + static_cast<std::ptrdiff_t>(end));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return path / std::wstring(name.begin(), name.end());
}

bool WriteNew(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
              DWORD& error) {
    File file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.Valid()) {
        error = GetLastError();
        return false;
    }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1 << 20));
        DWORD written = 0;
        if (!WriteFile(file.Get(), bytes.data() + done, chunk, &written, nullptr) || written == 0) {
            error = GetLastError();
            return false;
        }
        done += written;
    }
    if (!FlushFileBuffers(file.Get())) {
        error = GetLastError();
        return false;
    }
    return true;
}

std::wstring Wide(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

std::wstring Wide(const std::string_view ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

// What a failed read from GitHub tells the reader.
std::wstring FetchProblem(const FetchResult& fetched, const std::wstring_view what,
                          const bool latest) {
    switch (fetched.status) {
        case FetchResult::Status::Ok:
            return {};
        case FetchResult::Status::Missing:
            return latest ? localization::Format(L"GitHub 上最新的版本沒有 {}",
                                                 L"The latest release on GitHub has no {}", what)
                          : localization::Format(L"GitHub 上的這個版本沒有 {}",
                                                 L"That release on GitHub has no {}", what);
        case FetchResult::Status::TooLarge:
            return localization::Format(L"GitHub 上的 {} 太大", L"{} on GitHub is too large", what);
        case FetchResult::Status::Refused:
            return localization::Format(L"GitHub 把 {} 導向不允許的位置",
                                        L"GitHub redirected {} somewhere not allowed", what);
        case FetchResult::Status::Failed:
            break;
    }
    if (fetched.http_status != 0) {
        return localization::Format(L"無法從 GitHub 讀取 {}（HTTP {}）",
                                    L"{} cannot be read from GitHub (HTTP {})", what,
                                    fetched.http_status);
    }
    return localization::Format(L"無法連到 GitHub 讀取 {}（錯誤 {}）",
                                L"GitHub cannot be reached for {} (error {})", what, fetched.error);
}

std::string FetchDetail(const FetchResult& fetched) {
    return std::format("status={} http={} error={}", static_cast<int>(fetched.status),
                       fetched.http_status, fetched.error);
}

SourceManifest ParseFetched(const std::vector<std::uint8_t>& bytes, const Sha256& sha,
                            const std::wstring_view where) {
    SourceManifest result;
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ManifestResult parsed = ParseManifest(text, sha);
    if (!parsed.manifest) {
        result.problem = localization::Format(L"{} 的 gtg-update.json 無效",
                                              L"The gtg-update.json on {} is not valid", where);
        result.detail = std::move(parsed.error);
        return result;
    }
    result.manifest = std::move(parsed.manifest);
    return result;
}

}  // namespace

UpdateSource ReleasesSource(const std::wstring_view agent) {
    UpdateSource source;
    source.kind = UpdateSource::Kind::Releases;
    source.location = kPublicRepository;
    // Release downloads redirect from github.com to GitHub's content hosts.
    source.policy.hosts = {L"github.com", L".githubusercontent.com"};
    source.agent = std::wstring(agent);
    return source;
}

UpdateSource FolderSource(std::wstring folder) {
    UpdateSource source;
    source.kind = UpdateSource::Kind::Folder;
    source.location = std::move(folder);
    source.policy = {};
    return source;
}

UpdateSource SourceOf(const std::wstring& configured, const std::wstring_view agent) {
    return configured.empty() ? ReleasesSource(agent) : FolderSource(configured);
}

std::wstring Describe(const UpdateSource& source) {
    if (source.kind == UpdateSource::Kind::Folder) return source.location;
    std::wstring_view name(source.location);
    if (const std::size_t at = name.find(L"github.com/"); at != std::wstring_view::npos) {
        name = name.substr(at + 11);
    }
    return L"GitHub (" + std::wstring(name) + L")";
}

std::wstring ManifestUrl(const UpdateSource& source) {
    return source.location + L"/releases/latest/download/" + kManifestFileName;
}

std::wstring ReleaseFileUrl(const UpdateSource& source, const Version& version,
                            const std::string_view name) {
    return source.location + L"/releases/download/v" + Wide(FormatVersion(version)) + L"/" +
           Wide(name);
}

bool IsValidSource(const std::wstring_view source) noexcept {
    if (source.size() < 5 || source.size() > MAX_PATH || !source.starts_with(L"\\\\")) {
        return false;
    }
    std::wstring_view rest = source.substr(2);
    if (rest.ends_with(L'\\')) rest.remove_suffix(1);
    std::size_t segments = 0;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = rest.find(L'\\', begin);
        const std::wstring_view segment = rest.substr(begin, end - begin);
        if (segment.empty() || segment == L"." || segment == L"..") return false;
        for (const wchar_t c : segment) {
            if (IsForbidden(c)) return false;
        }
        // The first segment is the host: \\?\ and \\.\ are device paths.
        ++segments;
        if (end == std::wstring_view::npos) break;
        begin = end + 1;
    }
    // A host and a share at least.
    return segments >= 2;
}

ReadResult ReadBounded(const std::filesystem::path& file, const std::uint64_t max_bytes) {
    ReadResult result;
    File handle(CreateFileW(file.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle.Valid()) {
        result.error = GetLastError();
        result.status = result.error == ERROR_FILE_NOT_FOUND || result.error == ERROR_PATH_NOT_FOUND
                            ? ReadResult::Status::Missing
                            : ReadResult::Status::Failed;
        return result;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.Get(), &size)) {
        result.error = GetLastError();
        return result;
    }
    if (static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
        result.status = ReadResult::Status::TooLarge;
        return result;
    }
    // Read one byte past the bound: a file that grew after the size was taken
    // is caught here rather than cut short.
    result.bytes.resize(static_cast<std::size_t>(max_bytes) + 1);
    std::size_t done = 0;
    for (;;) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(result.bytes.size() - done, 1 << 20));
        if (chunk == 0) break;
        DWORD read = 0;
        if (!ReadFile(handle.Get(), result.bytes.data() + done, chunk, &read, nullptr)) {
            result.error = GetLastError();
            result.bytes.clear();
            return result;
        }
        if (read == 0) break;
        done += read;
    }
    if (done > max_bytes) {
        result.bytes.clear();
        result.status = ReadResult::Status::TooLarge;
        return result;
    }
    result.bytes.resize(done);
    result.status = ReadResult::Status::Ok;
    return result;
}

SourceManifest ReadManifest(const UpdateSource& source, const Sha256& sha) {
    SourceManifest result;
    if (source.kind == UpdateSource::Kind::Releases) {
        const FetchResult fetched =
            FetchBounded(ManifestUrl(source), kMaxManifestBytes, source.policy, source.agent);
        if (fetched.status != FetchResult::Status::Ok) {
            result.problem = FetchProblem(fetched, kManifestFileName, true);
            result.detail = FetchDetail(fetched);
            return result;
        }
        return ParseFetched(fetched.bytes, sha, L"GitHub");
    }
    if (!IsValidSource(source.location)) {
        result.problem = localization::Select(L"更新來源不是有效的 UNC 路徑",
                                              L"The update source is not a valid UNC path");
        return result;
    }
    return LoadManifest(source.location, sha);
}

SourceManifest LoadManifest(const std::filesystem::path& folder, const Sha256& sha) {
    SourceManifest result;
    const ReadResult read = ReadBounded(folder / kManifestFileName, kMaxManifestBytes);
    switch (read.status) {
        case ReadResult::Status::Ok:
            break;
        case ReadResult::Status::Missing:
            result.problem = localization::Select(L"來源沒有 gtg-update.json",
                                                  L"The source has no gtg-update.json");
            result.detail = std::format("Win32 {}", read.error);
            return result;
        case ReadResult::Status::TooLarge:
            result.problem = localization::Select(L"來源的 gtg-update.json 太大",
                                                  L"The source's gtg-update.json is too large");
            return result;
        case ReadResult::Status::Failed:
            result.problem =
                localization::Format(L"無法讀取更新來源（Win32 {}）",
                                     L"The update source cannot be read (Win32 {})", read.error);
            result.detail = std::format("Win32 {}", read.error);
            return result;
    }
    const std::string_view text(reinterpret_cast<const char*>(read.bytes.data()),
                                read.bytes.size());
    ManifestResult parsed = ParseManifest(text, sha);
    if (!parsed.manifest) {
        result.problem = localization::Select(L"來源的 gtg-update.json 無效",
                                              L"The source's gtg-update.json is not valid");
        result.detail = std::move(parsed.error);
        return result;
    }
    result.manifest = std::move(parsed.manifest);
    return result;
}

std::wstring ReleaseFolderName(const Digest& release) {
    return Wide(ToHex(release).substr(0, 16));
}

StageResult Stage(const UpdateSource& source, const Manifest& manifest,
                  const ManifestVariant& variant, const std::filesystem::path& work,
                  const Sha256& sha) {
    StageResult result;
    const bool releases = source.kind == UpdateSource::Kind::Releases;
    if (releases && !variant.dir.empty()) {
        result.problem = localization::Select(L"GitHub 的清單把檔案放在資料夾裡，發佈沒有資料夾",
                                              L"The GitHub manifest puts files in a folder, and a "
                                              L"release has none");
        return result;
    }
    result.folder = work / L"staged" / ReleaseFolderName(manifest.release);
    std::error_code ignored;
    std::filesystem::remove_all(result.folder, ignored);
    std::error_code created;
    std::filesystem::create_directories(result.folder, created);
    if (created) {
        result.problem = localization::Format(L"無法建立暫存資料夾（錯誤 {}）",
                                              L"The staging folder cannot be created (error {})",
                                              created.value());
        return result;
    }
    for (const ManifestFile& file : variant.files) {
        ReadResult read;
        if (releases) {
            FetchResult fetched = FetchBounded(ReleaseFileUrl(source, manifest.version, file.name),
                                               file.size, source.policy, source.agent);
            if (fetched.status != FetchResult::Status::Ok) {
                result.problem = FetchProblem(fetched, Wide(file.name), false);
                return result;
            }
            read.status = ReadResult::Status::Ok;
            read.bytes = std::move(fetched.bytes);
        } else {
            read = ReadBounded(SourceFile(source.location, variant.dir, file.name), file.size);
            if (read.status != ReadResult::Status::Ok) {
                result.problem = localization::Format(
                    L"無法從來源讀取 {}（Win32 {}）",
                    L"{} cannot be read from the source (Win32 {})", Wide(file.name), read.error);
                return result;
            }
        }
        if (read.bytes.size() != file.size) {
            result.problem = localization::Format(L"來源的 {} 大小與清單不符",
                                                  L"The source's {} does not have the listed size",
                                                  Wide(file.name));
            return result;
        }
        const auto digest = sha.Of(read.bytes);
        if (!digest || *digest != file.sha256) {
            result.problem = localization::Format(L"來源的 {} 與清單的 SHA-256 不符",
                                                  L"The source's {} does not match its SHA-256",
                                                  Wide(file.name));
            return result;
        }
        StagedFile staged;
        staged.name = file.name;
        staged.path = result.folder / Wide(file.name);
        staged.size = file.size;
        staged.sha256 = *digest;
        DWORD error = 0;
        if (!WriteNew(staged.path, read.bytes, error)) {
            result.problem = localization::Format(L"無法寫入暫存的 {}（Win32 {}）",
                                                  L"The staged {} cannot be written (Win32 {})",
                                                  Wide(file.name), error);
            return result;
        }
        result.files.push_back(std::move(staged));
    }
    return result;
}

void RemoveStagedExcept(const std::filesystem::path& work, const std::wstring_view keep) noexcept {
    std::error_code error;
    const std::filesystem::path root = work / L"staged";
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end;
         it.increment(error)) {
        if (it->path().filename().wstring() == keep) continue;
        std::error_code ignored;
        std::filesystem::remove_all(it->path(), ignored);
    }
}

}  // namespace gtg::update
