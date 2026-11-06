#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update/digest.hpp"

// The updater's settings, in HKLM\SOFTWARE\GpuThermalGuard\Update: a machine
// setting that only Administrators and SYSTEM can write. A source a non-admin
// program could change would let it point an elevated GTG at its own files.
namespace gtg::update {

struct UpdatePreference {
    // The UNC folder updates come from; empty: GTG's public releases on GitHub.
    std::wstring source;
    // A stored source that cannot be used: no update activity at all, rather
    // than updates from somewhere the administrator did not choose.
    bool source_invalid{};
    // That source as stored, to show and correct; empty when it is not text.
    std::wstring source_text;
    // Absent means on: checking is a read of a small file, and nothing is
    // ever installed without the reader's consent.
    bool auto_check{true};
    // The SHA-256 of the one certificate staged files must be signed with.
    std::optional<Digest> pin;
    bool pin_invalid{};
    // The release the last notification was shown for, as hex.
    std::string notified_release;
    // Paired hosts may ask this machine to update (Hosts). Absent means on;
    // off is how a machine refuses them.
    bool hosts_trigger{true};
    // When a paired host's request was last acted on, by the wall clock (UTC
    // ms): at most one per 30 minutes, across the restarts updates cause.
    std::optional<std::int64_t> last_trigger_unix_ms;
    // The request whose update is with the script, so that its outcome is
    // reported against it after the restart; 0 for none.
    std::uint64_t pending_trigger{};
    // The last requests acted on, oldest first: a repeat or a replay of one is
    // not answered again, also by the build its update starts.
    std::vector<std::uint64_t> recent_triggers;
};

struct StoredSource {
    std::wstring source;
    bool invalid{};
    std::wstring text;
};

// What a stored Source value means. Absent, or empty text: the public releases.
// A valid UNC path: that folder. Anything else -- not text, too long, not a UNC
// path -- is invalid, never a source somewhere else.
[[nodiscard]] StoredSource ClassifySource(bool present, const std::optional<std::wstring>& text);

[[nodiscard]] UpdatePreference LoadUpdatePreference() noexcept;
// Empty removes the value.
[[nodiscard]] bool SaveUpdateSource(std::wstring_view source, std::wstring& error);
[[nodiscard]] bool SaveUpdateAutoCheck(bool on, std::wstring& error);
// Empty removes the pin.
[[nodiscard]] bool SaveUpdatePin(std::wstring_view hex, std::wstring& error);
[[nodiscard]] bool SaveNotifiedRelease(std::string_view release, std::wstring& error);
[[nodiscard]] bool SaveHostsTrigger(bool on, std::wstring& error);
[[nodiscard]] bool SaveLastTrigger(std::int64_t unix_ms, std::wstring& error);
// 0 removes the value.
[[nodiscard]] bool SavePendingTrigger(std::uint64_t trigger, std::wstring& error);
[[nodiscard]] bool SaveRecentTriggers(const std::vector<std::uint64_t>& triggers,
                                      std::wstring& error);

}  // namespace gtg::update
