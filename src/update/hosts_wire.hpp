#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// What two GTGs with the updater tell each other about updates, byte by byte:
// the status each sends with its state once a second, and the trigger one sends
// to ask another to update. The peer link seals and opens these bodies without
// reading them; this is the only code that does.
//
// A trigger names a release and nothing else. The GTG that receives it looks
// for that release on its own source, so no code, file, path or source ever
// crosses the network.
//
// Every reserved byte must be zero and every value one this version knows, or
// the body is refused whole, as the peer protocol does with a state. Integers
// are little-endian, written byte by byte.
namespace gtg::update::hosts {

inline constexpr std::size_t kStatusBodyBytes = 40;
inline constexpr std::size_t kTriggerBodyBytes = 48;
// A release or a build as a host shows it: its first 8 bytes.
inline constexpr std::size_t kShortBytes = 8;
inline constexpr std::size_t kReleaseBytes = 32;

using StatusBody = std::array<std::uint8_t, kStatusBodyBytes>;
using TriggerBody = std::array<std::uint8_t, kTriggerBodyBytes>;
using Short = std::array<std::uint8_t, kShortBytes>;

// The sender's update state.
enum class Phase : std::uint8_t {
    Off,   // no source
    Idle,
    Checking,
    Staging,
    Waiting,
    HandingOff,
    Exiting,
    Settling,   // started by the update script, its outcome pending
};
inline constexpr std::uint8_t kPhaseCount = 8;

// What a waiting update waits for.
enum class WaitFor : std::uint8_t { None, Protection, Temperature, Overlay };
inline constexpr std::uint8_t kWaitForCount = 4;

// The sender's source compared with the build it runs.
enum class SourceIs : std::uint8_t { Unknown, Newer, SameBuild, Older, BelowFloor, Unreadable };
inline constexpr std::uint8_t kSourceIsCount = 6;

// What became of a trigger, as its sender reads it in the status.
enum class Answer : std::uint8_t {
    None,
    Accepted,           // being carried out: the phase says how far
    NotAllowed,         // this machine does not let other hosts update it
    TooSoon,            // a trigger was acted on less than 30 minutes ago
    Busy,               // another update is in progress
    NoSource,
    Unreadable,         // the source, or this GTG's own EXE, could not be read
    OtherRelease,       // the source names another release
    NoVariant,          // the source has no build for this machine's variant
    AlreadyCurrent,     // the release is the build that runs
    NotNewer,
    BelowFloor,
    ServiceInstalled,
    SmartAppControl,
    FolderRefused,      // GTG runs from a network folder, or from one without access lists
    EntryMismatch,      // GTG's file name is not the manifest's entry
    PinInvalid,         // the signer pin setting here is not valid
    SignatureRefused,   // a staged file is not signed by the pinned certificate
    StageFailed,
    HandOffFailed,
    GaveUp,             // no safe moment within 30 minutes
    Cancelled,          // cancelled on that machine
    Updated,
    RolledBack,
    Aborted,            // the script installed nothing
    Failed,             // the script reported a failure
    Interrupted,        // the update did not finish
};
inline constexpr std::uint8_t kAnswerCount = 27;

namespace status_flags {
inline constexpr std::uint8_t kHasSource = 1u << 0;
inline constexpr std::uint8_t kAcceptsTriggers = 1u << 1;
inline constexpr std::uint8_t kPinSet = 1u << 2;
// Programs without administrator rights can write to GTG's folder there.
inline constexpr std::uint8_t kFolderWritable = 1u << 3;
inline constexpr std::uint8_t kKnown = 0x0F;
}  // namespace status_flags

struct Status {
    bool has_source{};
    bool accepts_triggers{};
    bool pin_set{};
    bool folder_writable{};
    Phase phase{Phase::Off};
    WaitFor wait{WaitFor::None};
    SourceIs source{SourceIs::Unknown};
    std::array<std::uint8_t, 3> source_version{};
    Short source_release{};
    Short running_build{};
    // The last trigger answered, and the answer; 0 and None before any.
    std::uint64_t trigger{};
    Answer answer{Answer::None};

    friend bool operator==(const Status&, const Status&) = default;
};

struct Trigger {
    std::uint64_t id{};   // random, never 0; repeats of one trigger carry the same
    std::array<std::uint8_t, kReleaseBytes> release{};
    // Install the release even if it is not newer, or is the build that runs.
    bool force{};

    friend bool operator==(const Trigger&, const Trigger&) = default;
};

namespace wire_detail {

inline constexpr std::uint8_t kTriggerForce = 1u << 0;

inline void PutU64(std::uint8_t* out, const std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i)
        out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

[[nodiscard]] inline std::uint64_t GetU64(const std::uint8_t* in) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    return value;
}

[[nodiscard]] inline bool AllZero(const std::uint8_t* in, const std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        if (in[i] != 0) return false;
    }
    return true;
}

}  // namespace wire_detail

// Layout: flags, phase, wait, source; source version (3) and a zero; the
// source's release (8); the running build (8); the trigger id (8); the answer
// and 7 zeros.
[[nodiscard]] inline StatusBody WriteStatus(const Status& status) noexcept {
    StatusBody body{};
    std::uint8_t flags = 0;
    if (status.has_source) flags |= status_flags::kHasSource;
    if (status.accepts_triggers) flags |= status_flags::kAcceptsTriggers;
    if (status.pin_set) flags |= status_flags::kPinSet;
    if (status.folder_writable) flags |= status_flags::kFolderWritable;
    const auto phase = static_cast<std::uint8_t>(status.phase);
    const auto wait = static_cast<std::uint8_t>(status.wait);
    const auto source = static_cast<std::uint8_t>(status.source);
    const auto answer = static_cast<std::uint8_t>(status.answer);
    const bool answered = status.trigger != 0 && answer != 0 && answer < kAnswerCount;
    body[0] = flags;
    body[1] = phase < kPhaseCount ? phase : 0;
    body[2] = wait < kWaitForCount ? wait : 0;
    body[3] = source < kSourceIsCount ? source : 0;
    body[4] = status.source_version[0];
    body[5] = status.source_version[1];
    body[6] = status.source_version[2];
    for (std::size_t i = 0; i < kShortBytes; ++i) {
        body[8 + i] = status.source_release[i];
        body[16 + i] = status.running_build[i];
    }
    wire_detail::PutU64(body.data() + 24, answered ? status.trigger : 0);
    body[32] = answered ? answer : 0;
    return body;
}

[[nodiscard]] inline std::optional<Status> ReadStatus(
    const std::span<const std::uint8_t> body) noexcept {
    if (body.size() != kStatusBodyBytes) return std::nullopt;
    const std::uint8_t flags = body[0];
    if ((flags & ~status_flags::kKnown) != 0) return std::nullopt;
    if (body[1] >= kPhaseCount || body[2] >= kWaitForCount || body[3] >= kSourceIsCount) {
        return std::nullopt;
    }
    if (body[7] != 0 || !wire_detail::AllZero(body.data() + 33, 7)) return std::nullopt;
    const std::uint64_t trigger = wire_detail::GetU64(body.data() + 24);
    const std::uint8_t answer = body[32];
    if (answer >= kAnswerCount || (trigger == 0) != (answer == 0)) return std::nullopt;
    Status status;
    status.has_source = (flags & status_flags::kHasSource) != 0;
    status.accepts_triggers = (flags & status_flags::kAcceptsTriggers) != 0;
    status.pin_set = (flags & status_flags::kPinSet) != 0;
    status.folder_writable = (flags & status_flags::kFolderWritable) != 0;
    status.phase = static_cast<Phase>(body[1]);
    status.wait = static_cast<WaitFor>(body[2]);
    status.source = static_cast<SourceIs>(body[3]);
    status.source_version = {body[4], body[5], body[6]};
    for (std::size_t i = 0; i < kShortBytes; ++i) {
        status.source_release[i] = body[8 + i];
        status.running_build[i] = body[16 + i];
    }
    status.trigger = trigger;
    status.answer = static_cast<Answer>(answer);
    return status;
}

// Layout: the id (8), the release (32), flags, 7 zeros.
[[nodiscard]] inline TriggerBody WriteTrigger(const Trigger& trigger) noexcept {
    TriggerBody body{};
    wire_detail::PutU64(body.data(), trigger.id);
    for (std::size_t i = 0; i < kReleaseBytes; ++i)
        body[8 + i] = trigger.release[i];
    body[40] = trigger.force ? wire_detail::kTriggerForce : 0;
    return body;
}

[[nodiscard]] inline std::optional<Trigger> ReadTrigger(
    const std::span<const std::uint8_t> body) noexcept {
    if (body.size() != kTriggerBodyBytes) return std::nullopt;
    Trigger trigger;
    trigger.id = wire_detail::GetU64(body.data());
    if (trigger.id == 0) return std::nullopt;
    for (std::size_t i = 0; i < kReleaseBytes; ++i)
        trigger.release[i] = body[8 + i];
    if ((body[40] & ~wire_detail::kTriggerForce) != 0) return std::nullopt;
    if (!wire_detail::AllZero(body.data() + 41, 7)) return std::nullopt;
    trigger.force = (body[40] & wire_detail::kTriggerForce) != 0;
    return trigger;
}

// The first bytes of a release or build, as a status carries it.
template <std::size_t N>
[[nodiscard]] constexpr Short ShortOf(const std::array<std::uint8_t, N>& digest) noexcept {
    static_assert(N >= kShortBytes);
    Short out{};
    for (std::size_t i = 0; i < kShortBytes; ++i)
        out[i] = digest[i];
    return out;
}

}  // namespace gtg::update::hosts
