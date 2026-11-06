#include "update/hosts_updates.hpp"

#include <algorithm>
#include <format>
#include <random>

#include "update/hosts_trigger.hpp"

namespace gtg::update {
namespace {

std::array<std::uint8_t, 3> WireVersion(const Version& version) noexcept {
    const auto part = [](const int value) {
        return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
    };
    return {part(version.major), part(version.minor), part(version.patch)};
}

// Answers after which a host is where "update all" wants it.
bool Arrived(const hosts::Answer answer) noexcept {
    return answer == hosts::Answer::Updated || answer == hosts::Answer::AlreadyCurrent;
}

// Answers only admission gives, before anything is read.
bool Admission(const hosts::Answer answer) noexcept {
    return answer == hosts::Answer::NotAllowed || answer == hosts::Answer::NoSource ||
           answer == hosts::Answer::Busy || answer == hosts::Answer::TooSoon;
}

// Whether a status's answer to a request replaces the one recorded. An outcome
// is final. Once the request was accepted, an answer only admission gives can
// come only from a build that did not know it had seen the request: the one
// the update started, getting the request's last repeats.
bool Replaces(const HostsUpdates::Asked& asked, const hosts::Answer answer) noexcept {
    if (answer == hosts::Answer::None || answer == asked.answer) return false;
    if (asked.ask != HostsUpdates::Ask::Answered) return true;
    return asked.answer == hosts::Answer::Accepted && !Admission(answer);
}

std::wstring Short(const Digest& digest) {
    const std::string hex = ToHex(digest).substr(0, 8);
    return std::wstring(hex.begin(), hex.end());
}

}  // namespace

HostsUpdates::HostsUpdates(std::function<std::uint64_t()> random) : random_(std::move(random)) {
    if (!random_) {
        random_ = [] {
            // Ids only tell one trigger from another; the link seals them.
            static std::random_device device;
            return (static_cast<std::uint64_t>(device()) << 32) | device();
        };
    }
}

const HostView* HostsUpdates::Lookup(const std::uint64_t host) const noexcept {
    const auto found =
        std::find_if(hosts_.begin(), hosts_.end(), [&](const HostView& h) { return h.id == host; });
    return found == hosts_.end() ? nullptr : &*found;
}

std::optional<HostView> HostsUpdates::Find(const std::uint64_t host) const {
    const HostView* view = Lookup(host);
    if (view == nullptr) return std::nullopt;
    return *view;
}

std::wstring HostsUpdates::NameOf(const std::uint64_t host) const {
    const HostView* view = Lookup(host);
    return view == nullptr || view->name.empty() ? std::wstring(L"?") : view->name;
}

bool HostsUpdates::AtRelease(const HostView& host) const noexcept {
    if (!source_ || !host.status) return false;
    if (host.status->source == hosts::SourceIs::SameBuild &&
        host.status->source_release == hosts::ShortOf(source_->release)) {
        return true;
    }
    const auto asked = asked_.find(host.id);
    return asked != asked_.end() && asked->second.answer == hosts::Answer::Updated &&
           asked->second.release == source_->release &&
           host.version == WireVersion(source_->version);
}

bool HostsUpdates::Askable(const HostView& host) const noexcept {
    return source_ && host.online && host.status && host.status->accepts_triggers;
}

bool HostsUpdates::CanAsk(const std::uint64_t host) const {
    if (rollout_.run == Run::Running) return false;
    const HostView* view = Lookup(host);
    if (view == nullptr || !Askable(*view)) return false;
    const auto asked = AskedOf(host);
    return !asked || asked->ask != Ask::Sent;
}

std::vector<std::uint64_t> HostsUpdates::Candidates() const {
    std::vector<std::uint64_t> out;
    for (const HostView& host : hosts_) {
        if (Askable(host) && !AtRelease(host)) out.push_back(host.id);
    }
    return out;
}

std::optional<HostsUpdates::Asked> HostsUpdates::AskedOf(const std::uint64_t host) const {
    const auto found = asked_.find(host);
    if (found == asked_.end()) return std::nullopt;
    return found->second;
}

void HostsUpdates::Send(const std::uint64_t host, const bool force, const std::uint64_t now_ms) {
    std::uint64_t id = 0;
    while (id == 0)
        id = random_();
    hosts::Trigger trigger;
    trigger.id = id;
    trigger.release = source_->release;
    trigger.force = force;
    Asked asked;
    asked.trigger = id;
    asked.force = force;
    asked.release = source_->release;
    asked.sent_ms = now_ms;
    asked_[host] = asked;
    outbox_.emplace_back(host, hosts::WriteTrigger(trigger));
    journal_.push_back(std::format(L"update: asked {} to {} release {}", NameOf(host),
                                   force ? L"force" : L"update to", Short(source_->release)));
}

bool HostsUpdates::Ask(const std::uint64_t host, const bool force, const std::uint64_t now_ms) {
    if (!CanAsk(host)) return false;
    Send(host, force, now_ms);
    return true;
}

bool HostsUpdates::UpdateAll(const std::uint64_t now_ms) {
    if (rollout_.run == Run::Running || !source_) return false;
    std::vector<std::uint64_t> order = Candidates();
    if (order.empty()) return false;
    rollout_ = {};
    rollout_.run = Run::Running;
    rollout_.pending = std::move(order);
    Advance(now_ms);
    return true;
}

void HostsUpdates::Stop() {
    if (rollout_.run != Run::Running) return;
    rollout_.run = Run::Stopped;
    rollout_.stopped_by_reader = true;
}

void HostsUpdates::StopAt(const std::uint64_t host, const std::wstring_view why) {
    rollout_.run = Run::Stopped;
    rollout_.stopped_at = host;
    journal_.push_back(std::format(L"update: update all stopped at {}: {}", NameOf(host), why));
}

// The next host that can still be asked, or done.
void HostsUpdates::Advance(const std::uint64_t now_ms) {
    rollout_.current.reset();
    rollout_.away_since_ms = 0;
    while (!rollout_.pending.empty()) {
        const std::uint64_t next = rollout_.pending.front();
        rollout_.pending.erase(rollout_.pending.begin());
        const HostView* host = Lookup(next);
        // Gone, unwilling or already there since the list was made: not asked.
        if (host == nullptr || !Askable(*host) || AtRelease(*host)) continue;
        rollout_.current = next;
        Send(next, false, now_ms);
        return;
    }
    rollout_.run = Run::Done;
    journal_.push_back(
        std::format(L"update: update all done; {} host(s) updated", rollout_.done.size()));
}

void HostsUpdates::Tick(const std::uint64_t now_ms, std::vector<HostView> hosts,
                        std::optional<SourceView> source) {
    hosts_ = std::move(hosts);
    source_ = std::move(source);
    for (auto& [id, asked] : asked_) {
        const HostView* host = Lookup(id);
        if (host != nullptr && host->status && host->status->trigger == asked.trigger) {
            const hosts::Answer answer = host->status->answer;
            if (Replaces(asked, answer)) {
                asked.ask = Ask::Answered;
                asked.answer = answer;
                journal_.push_back(
                    std::format(L"update: {} answered {}", NameOf(id), AnswerName(answer)));
            } else if (answer != asked.answer && answer != hosts::Answer::None &&
                       answer != asked.ignored) {
                asked.ignored = answer;
                journal_.push_back(
                    std::format(L"update: {} answered {} to a request it had answered {}; ignored",
                                NameOf(id), AnswerName(answer), AnswerName(asked.answer)));
            }
        } else if (asked.ask == Ask::Sent && now_ms - asked.sent_ms >= kTriggerAnswerMs) {
            asked.ask = Ask::NoAnswer;
            journal_.push_back(std::format(L"update: {} did not answer within {} s", NameOf(id),
                                           kTriggerAnswerMs / 1'000));
        }
    }
    if (rollout_.run != Run::Running || !rollout_.current) return;
    const std::uint64_t current = *rollout_.current;
    const HostView* host = Lookup(current);
    const auto asked = AskedOf(current);
    if (!asked) {
        StopAt(current, L"not asked");
        return;
    }
    if (asked->ask == Ask::NoAnswer) {
        rollout_.stopped_no_answer = true;
        StopAt(current, L"no answer");
        return;
    }
    if (host == nullptr || !host->online) {
        if (rollout_.away_since_ms == 0) rollout_.away_since_ms = now_ms;
        if (now_ms - rollout_.away_since_ms >= kRolloutAwayMs) {
            rollout_.stopped_away = true;
            StopAt(current, std::format(L"away for {} minutes", kRolloutAwayMs / 60'000));
        }
        return;
    }
    rollout_.away_since_ms = 0;
    if (asked->ask != Ask::Answered || asked->answer == hosts::Answer::Accepted) return;
    if (!Arrived(asked->answer)) {
        rollout_.stopped_answer = asked->answer;
        StopAt(current, AnswerName(asked->answer));
        return;
    }
    // Updated is reported by the new build itself; it counts once the host is
    // back with the source's version.
    if (asked->answer == hosts::Answer::Updated && source_ &&
        host->version != WireVersion(source_->version)) {
        return;
    }
    rollout_.done.push_back(current);
    Advance(now_ms);
}

std::vector<std::pair<std::uint64_t, hosts::TriggerBody>> HostsUpdates::TakeOutbox() {
    std::vector<std::pair<std::uint64_t, hosts::TriggerBody>> out;
    out.swap(outbox_);
    return out;
}

std::vector<std::wstring> HostsUpdates::TakeJournal() {
    std::vector<std::wstring> out;
    out.swap(journal_);
    return out;
}

}  // namespace gtg::update
