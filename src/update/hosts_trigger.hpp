#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "update/decision.hpp"
#include "update/digest.hpp"
#include "update/hosts_wire.hpp"

// What this GTG does when a paired host asks it to update: the rules, pure, so
// every answer can be pinned by a test. The Tray runs them on the UI tick;
// nothing here reads the disk, the network or the registry.
//
// A trigger only ever leads to phase 0's own path -- this machine's source,
// its hashes, its pin, its safe moment, its rollback -- without the dialog.
namespace gtg::update {

// At most one ordinary trigger is acted on in this long, counted across
// restarts, so paired hosts cannot keep a machine restarting. A forced one is
// the reader's explicit choice on the asking machine and is not held off.
// Neither is acted on twice: repeats and replays are recognised by id.
inline constexpr std::int64_t kTriggerIntervalMs = 30 * 60'000;
// A triggered update waits this long for a safe moment, then gives up, so an
// update nobody is watching any more does not restart GTG hours later.
inline constexpr std::uint64_t kTriggerWaitMs = 30 * 60'000;
// A trigger that has sat in the hosts table longer than this is not acted on.
inline constexpr std::uint64_t kTriggerFreshMs = 30'000;

struct TriggerGate {
    bool allowed{};      // other hosts may update this machine (HostsTrigger)
    bool has_source{};
    bool idle{};         // no update in progress
    // When a trigger was last acted on, by the wall clock.
    std::optional<std::int64_t> last_unix_ms;
};

// Before the source is read. Accepted means: read the source now. Force skips
// only the interval.
[[nodiscard]] constexpr hosts::Answer AdmitTrigger(const TriggerGate& gate,
                                                   const std::int64_t now_unix_ms,
                                                   const bool force = false) noexcept {
    if (!gate.allowed) return hosts::Answer::NotAllowed;
    if (!gate.has_source) return hosts::Answer::NoSource;
    if (!gate.idle) return hosts::Answer::Busy;
    if (gate.last_unix_ms && !force) {
        const std::int64_t since = now_unix_ms - *gate.last_unix_ms;
        // A clock set back by more than the interval leaves a time in the
        // future; it may not hold triggers off for longer than the interval.
        if (since < kTriggerIntervalMs && since > -kTriggerIntervalMs) {
            return hosts::Answer::TooSoon;
        }
    }
    return hosts::Answer::Accepted;
}

// After the source was read: whether its release is the one asked for, and
// whether it may be installed. Force skips only "newer" and "already that
// build"; nothing goes below the floor.
[[nodiscard]] constexpr hosts::Answer DecideTrigger(const hosts::Trigger& trigger,
                                                    const Digest& source_release,
                                                    const Decision decision) noexcept {
    if (trigger.release != source_release) return hosts::Answer::OtherRelease;
    if (decision.below_floor) return hosts::Answer::BelowFloor;
    if (!trigger.force) {
        if (decision.compare == Compare::SameBuild) return hosts::Answer::AlreadyCurrent;
        if (decision.compare == Compare::Older) return hosts::Answer::NotNewer;
    }
    return hosts::Answer::Accepted;
}

// The source as the status reports it.
[[nodiscard]] constexpr hosts::SourceIs SourceIsOf(const std::optional<Decision> decision,
                                                   const bool unreadable) noexcept {
    if (!decision) return unreadable ? hosts::SourceIs::Unreadable : hosts::SourceIs::Unknown;
    if (decision->below_floor) return hosts::SourceIs::BelowFloor;
    switch (decision->compare) {
        case Compare::Newer:
            return hosts::SourceIs::Newer;
        case Compare::SameBuild:
            return hosts::SourceIs::SameBuild;
        case Compare::Older:
            return hosts::SourceIs::Older;
    }
    return hosts::SourceIs::Unknown;
}

// The requests a GTG has seen, so that none is answered twice. The asking
// host repeats a request for its first seconds, and an update can finish
// inside them; a request recorded on the network can be replayed for two
// minutes after the restart an update causes. So the build an update starts
// begins with the requests acted on before it (KeptTriggers, in HKLM).
class SeenTriggers {
public:
    // False for 0 and for an id already seen; otherwise remembers it.
    constexpr bool First(const std::uint64_t id) noexcept {
        if (id == 0 || std::find(ids_.begin(), ids_.end(), id) != ids_.end()) return false;
        ids_[next_++ % ids_.size()] = id;
        return true;
    }

private:
    std::array<std::uint64_t, 16> ids_{};
    std::size_t next_{};
};

// How many of the requests acted on are kept across restarts. One acted on
// restarts GTG, so more than this inside a replay's two minutes cannot happen.
inline constexpr std::size_t kKeptTriggers = 8;

// The kept requests with `id` acted on, oldest first.
[[nodiscard]] inline std::vector<std::uint64_t> KeepTrigger(std::vector<std::uint64_t> kept,
                                                            const std::uint64_t id) {
    if (id == 0 || std::find(kept.begin(), kept.end(), id) != kept.end()) return kept;
    kept.push_back(id);
    if (kept.size() > kKeptTriggers) {
        kept.erase(kept.begin(), kept.end() - static_cast<std::ptrdiff_t>(kKeptTriggers));
    }
    return kept;
}

// As stored: 8 bytes each, little-endian.
[[nodiscard]] inline std::vector<std::uint8_t> StoredTriggers(
    const std::vector<std::uint64_t>& kept) {
    std::vector<std::uint8_t> bytes;
    for (const std::uint64_t id : kept) {
        for (int i = 0; i < 8; ++i)
            bytes.push_back(static_cast<std::uint8_t>(id >> (8 * i)));
    }
    return bytes;
}

// Nothing unless the value is whole ids and no more than kKeptTriggers.
[[nodiscard]] inline std::vector<std::uint64_t> KeptTriggers(
    const std::span<const std::uint8_t> bytes) {
    std::vector<std::uint64_t> kept;
    if (bytes.size() % 8 != 0 || bytes.size() > kKeptTriggers * 8) return kept;
    for (std::size_t at = 0; at < bytes.size(); at += 8) {
        std::uint64_t id = 0;
        for (int i = 0; i < 8; ++i)
            id |= static_cast<std::uint64_t>(bytes[at + i]) << (8 * i);
        if (id != 0) kept.push_back(id);
    }
    return kept;
}

// For the journal.
[[nodiscard]] constexpr const wchar_t* AnswerName(const hosts::Answer answer) noexcept {
    switch (answer) {
        case hosts::Answer::None:
            return L"none";
        case hosts::Answer::Accepted:
            return L"accepted";
        case hosts::Answer::NotAllowed:
            return L"not-allowed";
        case hosts::Answer::TooSoon:
            return L"too-soon";
        case hosts::Answer::Busy:
            return L"busy";
        case hosts::Answer::NoSource:
            return L"no-source";
        case hosts::Answer::Unreadable:
            return L"unreadable";
        case hosts::Answer::OtherRelease:
            return L"other-release";
        case hosts::Answer::NoVariant:
            return L"no-variant";
        case hosts::Answer::AlreadyCurrent:
            return L"already-current";
        case hosts::Answer::NotNewer:
            return L"not-newer";
        case hosts::Answer::BelowFloor:
            return L"below-floor";
        case hosts::Answer::ServiceInstalled:
            return L"service-installed";
        case hosts::Answer::SmartAppControl:
            return L"smart-app-control";
        case hosts::Answer::FolderRefused:
            return L"folder-refused";
        case hosts::Answer::EntryMismatch:
            return L"entry-mismatch";
        case hosts::Answer::PinInvalid:
            return L"pin-invalid";
        case hosts::Answer::SignatureRefused:
            return L"signature-refused";
        case hosts::Answer::StageFailed:
            return L"stage-failed";
        case hosts::Answer::HandOffFailed:
            return L"hand-off-failed";
        case hosts::Answer::GaveUp:
            return L"gave-up";
        case hosts::Answer::Cancelled:
            return L"cancelled";
        case hosts::Answer::Updated:
            return L"updated";
        case hosts::Answer::RolledBack:
            return L"rolled-back";
        case hosts::Answer::Aborted:
            return L"aborted";
        case hosts::Answer::Failed:
            return L"failed";
        case hosts::Answer::Interrupted:
            return L"interrupted";
    }
    return L"?";
}

// What the script's outcome means for the trigger that started the update.
[[nodiscard]] constexpr hosts::Answer AnswerOfOutcome(const std::string_view outcome) noexcept {
    if (outcome == "updated") return hosts::Answer::Updated;
    if (outcome == "rolled_back") return hosts::Answer::RolledBack;
    if (outcome == "aborted") return hosts::Answer::Aborted;
    return hosts::Answer::Failed;
}

}  // namespace gtg::update
