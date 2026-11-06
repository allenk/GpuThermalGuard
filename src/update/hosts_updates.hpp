#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "update/digest.hpp"
#include "update/hosts_wire.hpp"
#include "update/manifest.hpp"

// The asking side of updates over Hosts: which paired hosts can be asked, the
// triggers this GTG sent and what came of them, and "update all", one host at
// a time. Pure: the Tray hands in the hosts as the link's table has them and
// this machine's source, and hands the triggers this returns to the link. It
// never sends anything the reader did not ask for.
namespace gtg::update {

// A host that has not echoed a trigger in its status by then is reported as
// not answering. The link repeats a trigger for the first few seconds of it.
inline constexpr std::uint64_t kTriggerAnswerMs = 15'000;
// During "update all", a host that stays away this long stops it.
inline constexpr std::uint64_t kRolloutAwayMs = 10 * 60'000;

struct HostView {
    std::uint64_t id{};
    std::wstring name;
    bool online{};
    std::array<std::uint8_t, 3> version{};   // from its state; zeros while unknown
    // None: a GTG that sends no update status (1.2.0, or no updater).
    std::optional<hosts::Status> status;
};

struct SourceView {
    Version version;
    Digest release{};
};

class HostsUpdates {
public:
    enum class Ask { Sent, NoAnswer, Answered };

    struct Asked {
        std::uint64_t trigger{};
        bool force{};
        // The release the trigger named.
        Digest release{};
        std::uint64_t sent_ms{};
        Ask ask{Ask::Sent};
        // As of the last status that echoed the trigger with an answer that
        // may replace the one before.
        hosts::Answer answer{hosts::Answer::None};
        // The last answer that may not, journalled once.
        hosts::Answer ignored{hosts::Answer::None};
    };

    enum class Run { Idle, Running, Done, Stopped };

    // "Update all".
    struct Rollout {
        Run run{Run::Idle};
        std::vector<std::uint64_t> pending;   // still to go, in order
        std::optional<std::uint64_t> current;
        std::vector<std::uint64_t> done;
        // Why it stopped: a host and its answer, or the reader.
        std::optional<std::uint64_t> stopped_at;
        hosts::Answer stopped_answer{hosts::Answer::None};
        bool stopped_by_reader{};
        bool stopped_no_answer{};
        bool stopped_away{};
        std::uint64_t away_since_ms{};
    };

    // `random` makes trigger ids; tests pass their own.
    explicit HostsUpdates(std::function<std::uint64_t()> random = {});

    void Tick(std::uint64_t now_ms, std::vector<HostView> hosts, std::optional<SourceView> source);

    // The reader's requests. False when it cannot be done now.
    bool Ask(std::uint64_t host, bool force, std::uint64_t now_ms);
    bool UpdateAll(std::uint64_t now_ms);
    // Stops "update all"; a host already asked carries on.
    void Stop();

    [[nodiscard]] bool CanAsk(std::uint64_t host) const;
    // The hosts "update all" would ask, in order.
    [[nodiscard]] std::vector<std::uint64_t> Candidates() const;

    // The triggers to send, each taken once: the host and the body.
    [[nodiscard]] std::vector<std::pair<std::uint64_t, hosts::TriggerBody>> TakeOutbox();
    // Lines for the journal, each taken once: what was asked, every answer,
    // and where and why "update all" ended.
    [[nodiscard]] std::vector<std::wstring> TakeJournal();

    // Copies, never references: every tick replaces the hosts and the source,
    // and a modal dialog runs ticks inside it, so nothing outside may hold on
    // to them.
    [[nodiscard]] std::optional<HostView> Find(std::uint64_t host) const;

    [[nodiscard]] std::optional<SourceView> Source() const { return source_; }

    [[nodiscard]] std::optional<Asked> AskedOf(std::uint64_t host) const;

    [[nodiscard]] const Rollout& Progress() const noexcept { return rollout_; }

    // The host runs the release this machine's source names: by its status,
    // or, before its own first check after the restart, because it answered
    // "updated" to a request from here for that release.
    [[nodiscard]] bool AtRelease(const HostView& host) const noexcept;

private:
    [[nodiscard]] const HostView* Lookup(std::uint64_t host) const noexcept;
    [[nodiscard]] bool Askable(const HostView& host) const noexcept;
    void Send(std::uint64_t host, bool force, std::uint64_t now_ms);
    void Advance(std::uint64_t now_ms);
    void StopAt(std::uint64_t host, std::wstring_view why);
    [[nodiscard]] std::wstring NameOf(std::uint64_t host) const;

    std::function<std::uint64_t()> random_;
    std::vector<HostView> hosts_;
    std::optional<SourceView> source_;
    std::map<std::uint64_t, Asked> asked_;
    std::vector<std::pair<std::uint64_t, hosts::TriggerBody>> outbox_;
    std::vector<std::wstring> journal_;
    Rollout rollout_;
};

}  // namespace gtg::update
