#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Reading one file over HTTPS for the updater: a GET whose redirects are
// followed by hand, only to hosts the caller names, with the body bounded and
// every step under a time limit. WinHTTP is loaded from System32 for each
// read, so a GTG that never reads from the network never loads it, and the
// executable's import table does not change.
namespace gtg::update {

struct HttpUrl {
    bool secure{};
    // Lower case.
    std::wstring host;
    std::uint16_t port{};
    // Starts with '/'; the query is kept, a fragment is not.
    std::wstring path;
};

// An absolute http or https URL of printable ASCII, at most 4096 characters:
// a host of letters, digits, '-' and '.' (so no user name or password), an
// optional port. Anything else is refused rather than guessed at.
[[nodiscard]] std::optional<HttpUrl> ParseHttpUrl(std::wstring_view url);

struct FetchPolicy {
    // The hosts a read may reach over https on port 443, its redirects
    // included: an exact name, or ".example.com" for any name below example.com.
    std::vector<std::wstring> hosts;
    // Plain http to 127.0.0.1, for tests; https is required otherwise.
    bool loopback_http{};
    // The whole read, redirects included. Each step also has a limit of its
    // own, so the read ends at most one step past this.
    std::uint64_t deadline_ms{10 * 60'000};
};

[[nodiscard]] bool Allowed(const HttpUrl& url, const FetchPolicy& policy);

// Where a redirect leads: an absolute URL, or a path on the same host and
// scheme. Nothing for anything else.
[[nodiscard]] std::optional<std::wstring> RedirectTarget(const HttpUrl& from,
                                                         std::wstring_view location);

inline constexpr int kMaxRedirects = 5;

struct FetchResult {
    enum class Status {
        Ok,
        // 404 or 410.
        Missing,
        TooLarge,
        // A URL or a redirect the policy does not allow, or too many redirects.
        Refused,
        Failed,
    };

    Status status{Status::Failed};
    // A Win32 or WinHTTP error.
    DWORD error{};
    std::uint32_t http_status{};
    std::vector<std::uint8_t> bytes;
};

// At most max_bytes of body; a longer one is TooLarge and nothing of it is
// kept. The request carries `agent` as its user agent, no cookies and no
// credentials. Over https the certificate is checked by Windows, revocation
// included, and TLS is 1.2 or later; a read that cannot be set up that way is
// not made. One past its deadline fails with ERROR_TIMEOUT.
[[nodiscard]] FetchResult FetchBounded(std::wstring_view url, std::uint64_t max_bytes,
                                       const FetchPolicy& policy, std::wstring_view agent);

}  // namespace gtg::update
