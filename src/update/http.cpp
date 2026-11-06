#include "update/http.hpp"

#include <winhttp.h>

#include <algorithm>
#include <cwctype>

namespace gtg::update {
namespace {

constexpr std::size_t kMaxUrlLength = 4096;
// Each step of a read; a stalled share or proxy cannot hold the check longer.
constexpr int kResolveMs = 15'000;
constexpr int kConnectMs = 15'000;
constexpr int kSendMs = 30'000;
constexpr int kReceiveMs = 30'000;

bool IsHostChar(const wchar_t c) noexcept {
    return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'.';
}

// The WinHTTP functions a read uses. The DLL is loaded from System32 on the
// first read and never unloaded: WinHTTP resolves names on threads of its own,
// which can outlive a call that timed out.
struct WinHttp {
    [[nodiscard]] bool Ready() const noexcept {
        return open && connect && open_request && set_option && set_timeouts && send && receive &&
               query && read && close;
    }

    decltype(&WinHttpOpen) open{};
    decltype(&WinHttpConnect) connect{};
    decltype(&WinHttpOpenRequest) open_request{};
    decltype(&WinHttpSetOption) set_option{};
    decltype(&WinHttpSetTimeouts) set_timeouts{};
    decltype(&WinHttpSendRequest) send{};
    decltype(&WinHttpReceiveResponse) receive{};
    decltype(&WinHttpQueryHeaders) query{};
    decltype(&WinHttpReadData) read{};
    decltype(&WinHttpCloseHandle) close{};
};

const WinHttp& LoadWinHttp() {
    static const WinHttp loaded = [] {
        WinHttp http;
        const HMODULE module =
            LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) return http;
        const auto function = [module](const char* name) {
            return GetProcAddress(module, name);
        };
        http.open = reinterpret_cast<decltype(&WinHttpOpen)>(function("WinHttpOpen"));
        http.connect = reinterpret_cast<decltype(&WinHttpConnect)>(function("WinHttpConnect"));
        http.open_request =
            reinterpret_cast<decltype(&WinHttpOpenRequest)>(function("WinHttpOpenRequest"));
        http.set_option =
            reinterpret_cast<decltype(&WinHttpSetOption)>(function("WinHttpSetOption"));
        http.set_timeouts =
            reinterpret_cast<decltype(&WinHttpSetTimeouts)>(function("WinHttpSetTimeouts"));
        http.send = reinterpret_cast<decltype(&WinHttpSendRequest)>(function("WinHttpSendRequest"));
        http.receive =
            reinterpret_cast<decltype(&WinHttpReceiveResponse)>(function("WinHttpReceiveResponse"));
        http.query =
            reinterpret_cast<decltype(&WinHttpQueryHeaders)>(function("WinHttpQueryHeaders"));
        http.read = reinterpret_cast<decltype(&WinHttpReadData)>(function("WinHttpReadData"));
        http.close =
            reinterpret_cast<decltype(&WinHttpCloseHandle)>(function("WinHttpCloseHandle"));
        return http;
    }();
    return loaded;
}

class Handle {
public:
    Handle(const WinHttp& http, const HINTERNET value) noexcept : http_(http), value_(value) {}

    ~Handle() {
        if (value_ != nullptr) http_.close(value_);
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    [[nodiscard]] HINTERNET Get() const noexcept { return value_; }

private:
    const WinHttp& http_;
    HINTERNET value_;
};

bool IsRedirect(const std::uint32_t status) noexcept {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::optional<std::wstring> QueryText(const WinHttp& http, const HINTERNET request,
                                      const DWORD info) {
    DWORD bytes = 0;
    (void)http.query(request, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                     WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 ||
        bytes > (kMaxUrlLength + 1) * sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (!http.query(request, info, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &bytes,
                    WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    text.resize(bytes / sizeof(wchar_t));
    return text;
}

std::optional<DWORD> QueryNumber(const WinHttp& http, const HINTERNET request, const DWORD info) {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (!http.query(request, info | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &value,
                    &bytes, WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    return value;
}

FetchResult Fail(const FetchResult::Status status, const DWORD error = 0) {
    FetchResult result;
    result.status = status;
    result.error = error;
    return result;
}

}  // namespace

std::optional<HttpUrl> ParseHttpUrl(const std::wstring_view url) {
    if (url.size() > kMaxUrlLength) return std::nullopt;
    for (const wchar_t c : url) {
        if (c <= 0x20 || c >= 0x7F) return std::nullopt;
    }
    HttpUrl parsed;
    std::wstring_view rest;
    const auto starts = [&](const std::wstring_view prefix) {
        if (url.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            if (std::towlower(url[i]) != prefix[i]) return false;
        }
        return true;
    };
    if (starts(L"https://")) {
        parsed.secure = true;
        rest = url.substr(8);
    } else if (starts(L"http://")) {
        rest = url.substr(7);
    } else {
        return std::nullopt;
    }
    const std::size_t end = rest.find_first_of(L"/?#");
    std::wstring_view authority = rest.substr(0, end);
    std::wstring_view tail =
        end == std::wstring_view::npos ? std::wstring_view() : rest.substr(end);
    parsed.port = parsed.secure ? 443 : 80;
    if (const std::size_t colon = authority.find(L':'); colon != std::wstring_view::npos) {
        const std::wstring_view digits = authority.substr(colon + 1);
        if (digits.empty() || digits.size() > 5) return std::nullopt;
        unsigned value = 0;
        for (const wchar_t c : digits) {
            if (c < L'0' || c > L'9') return std::nullopt;
            value = value * 10 + static_cast<unsigned>(c - L'0');
        }
        if (value == 0 || value > 65535) return std::nullopt;
        parsed.port = static_cast<std::uint16_t>(value);
        authority = authority.substr(0, colon);
    }
    if (authority.empty() || authority.front() == L'.' || authority.back() == L'.') {
        return std::nullopt;
    }
    for (const wchar_t c : authority) {
        const wchar_t lower = static_cast<wchar_t>(std::towlower(c));
        if (!IsHostChar(lower)) return std::nullopt;
        parsed.host.push_back(lower);
    }
    if (const std::size_t hash = tail.find(L'#'); hash != std::wstring_view::npos) {
        tail = tail.substr(0, hash);
    }
    if (tail.empty() || tail.front() != L'/') parsed.path = L"/";
    parsed.path += tail;
    return parsed;
}

bool Allowed(const HttpUrl& url, const FetchPolicy& policy) {
    if (!url.secure) return policy.loopback_http && url.host == L"127.0.0.1";
    if (url.port != 443) return false;
    return std::any_of(policy.hosts.begin(), policy.hosts.end(), [&](const std::wstring& host) {
        if (host.empty()) return false;
        if (host.front() != L'.') return url.host == host;
        return url.host.size() > host.size() && url.host.ends_with(host);
    });
}

std::optional<std::wstring> RedirectTarget(const HttpUrl& from, const std::wstring_view location) {
    if (ParseHttpUrl(location)) return std::wstring(location);
    // A path on the same host; "//host" would be another host, so not that.
    if (location.size() < 1 || location.front() != L'/' ||
        (location.size() > 1 && location[1] == L'/')) {
        return std::nullopt;
    }
    std::wstring url = from.secure ? L"https://" : L"http://";
    url += from.host;
    if (from.port != (from.secure ? 443 : 80)) url += L":" + std::to_wstring(from.port);
    url += location;
    if (!ParseHttpUrl(url)) return std::nullopt;
    return url;
}

FetchResult FetchBounded(const std::wstring_view url, const std::uint64_t max_bytes,
                         const FetchPolicy& policy, const std::wstring_view agent) {
    const WinHttp& http = LoadWinHttp();
    if (!http.Ready()) return Fail(FetchResult::Status::Failed, ERROR_MOD_NOT_FOUND);
    const std::wstring agent_text(agent);
    HINTERNET raw = http.open(agent_text.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    // Before Windows 8.1 the automatic proxy is not available.
    if (raw == nullptr) {
        raw = http.open(agent_text.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    const Handle session(http, raw);
    if (session.Get() == nullptr) return Fail(FetchResult::Status::Failed, GetLastError());
    if (!http.set_timeouts(session.Get(), kResolveMs, kConnectMs, kSendMs, kReceiveMs)) {
        return Fail(FetchResult::Status::Failed, GetLastError());
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!http.set_option(session.Get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                         sizeof(protocols))) {
        // A system without TLS 1.3 refuses the pair.
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if (!http.set_option(session.Get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                             sizeof(protocols))) {
            return Fail(FetchResult::Status::Failed, GetLastError());
        }
    }
    const std::uint64_t started = GetTickCount64();
    const auto late = [&] {
        return GetTickCount64() - started > policy.deadline_ms;
    };

    std::wstring current(url);
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        if (late()) return Fail(FetchResult::Status::Failed, ERROR_TIMEOUT);
        const auto target = ParseHttpUrl(current);
        if (!target || !Allowed(*target, policy)) return Fail(FetchResult::Status::Refused);
        const Handle connection(http,
                                http.connect(session.Get(), target->host.c_str(), target->port, 0));
        if (connection.Get() == nullptr) return Fail(FetchResult::Status::Failed, GetLastError());
        const Handle request(
            http, http.open_request(connection.Get(), L"GET", target->path.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    target->secure ? WINHTTP_FLAG_SECURE : 0));
        if (request.Get() == nullptr) return Fail(FetchResult::Status::Failed, GetLastError());
        // Redirects are followed here, after the policy has seen them.
        DWORD features =
            WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        if (!http.set_option(request.Get(), WINHTTP_OPTION_DISABLE_FEATURE, &features,
                             sizeof(features))) {
            return Fail(FetchResult::Status::Failed, GetLastError());
        }
        // Not on by default: a revoked certificate would otherwise still pass.
        DWORD revocation = WINHTTP_ENABLE_SSL_REVOCATION;
        if (target->secure && !http.set_option(request.Get(), WINHTTP_OPTION_ENABLE_FEATURE,
                                               &revocation, sizeof(revocation))) {
            return Fail(FetchResult::Status::Failed, GetLastError());
        }
        if (!http.send(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                       0, 0) ||
            !http.receive(request.Get(), nullptr)) {
            return Fail(FetchResult::Status::Failed, GetLastError());
        }
        if (late()) return Fail(FetchResult::Status::Failed, ERROR_TIMEOUT);
        const auto status = QueryNumber(http, request.Get(), WINHTTP_QUERY_STATUS_CODE);
        if (!status) return Fail(FetchResult::Status::Failed, GetLastError());
        if (IsRedirect(*status)) {
            const auto location = QueryText(http, request.Get(), WINHTTP_QUERY_LOCATION);
            const auto next = location ? RedirectTarget(*target, *location) : std::nullopt;
            if (!next) return Fail(FetchResult::Status::Refused);
            current = *next;
            continue;
        }
        if (*status != 200) {
            FetchResult result =
                Fail(*status == 404 || *status == 410 ? FetchResult::Status::Missing
                                                      : FetchResult::Status::Failed);
            result.http_status = *status;
            return result;
        }
        if (const auto length = QueryNumber(http, request.Get(), WINHTTP_QUERY_CONTENT_LENGTH);
            length && *length > max_bytes) {
            return Fail(FetchResult::Status::TooLarge);
        }
        // One byte past the bound: a body longer than it said is caught.
        FetchResult result;
        result.http_status = *status;
        result.bytes.resize(static_cast<std::size_t>(max_bytes) + 1);
        std::size_t done = 0;
        while (done < result.bytes.size()) {
            const DWORD chunk =
                static_cast<DWORD>(std::min<std::size_t>(result.bytes.size() - done, 1 << 16));
            DWORD got = 0;
            if (!http.read(request.Get(), result.bytes.data() + done, chunk, &got)) {
                return Fail(FetchResult::Status::Failed, GetLastError());
            }
            if (got == 0) break;
            done += got;
            if (late()) return Fail(FetchResult::Status::Failed, ERROR_TIMEOUT);
        }
        if (done > max_bytes) return Fail(FetchResult::Status::TooLarge);
        result.bytes.resize(done);
        result.status = FetchResult::Status::Ok;
        return result;
    }
    return Fail(FetchResult::Status::Refused);
}

}  // namespace gtg::update
