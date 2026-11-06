#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gtg::topapp {

// One app's share of a record in one sweep. `key` identifies the app (its
// image path, folded; see app_name.hpp), never a process: an app made of
// seventy processes is one candidate.
struct Candidate {
    std::wstring key;
    double value{};
};

// What "dominant" is measured against. GPU uses an absolute figure because
// its per-app number is the busiest engine's utilisation, and those do not
// add up to a total that means anything; every other record compares an app
// with the sum over all apps.
enum class Dominance { ShareOfTotal, Absolute };

// Which app a record in alert names, chosen so the name does not flicker
// (owner, 2026-10-16: the alert must not keep flickering).
//
// - Every app's value is smoothed (EMA, tau 3 s), so one busy sweep cannot
//   win on its own.
// - The first name needs two agreeing sweeps.
// - A challenger replaces the shown name only after leading it by a quarter
//   for three sweeps in a row, and never before the shown name has been up
//   five seconds.
// - Only a dominant app is named: load spread over many apps names nothing
//   rather than an arbitrary one. A shown app that stops being dominant goes
//   with the same patience a challenger needs.
// - An app missing from a sweep has exited or is no longer measurable: it is
//   dropped at once, and a name on screen goes with it.
class Leader {
public:
    static constexpr double kTauMs = 3'000.0;
    static constexpr int kFirstSweeps = 2;
    static constexpr double kSwitchRatio = 1.25;
    static constexpr int kSwitchSweeps = 3;
    static constexpr std::uint64_t kMinimumShownMs = 5'000;
    static constexpr double kDominantShare = 0.25;
    static constexpr double kDominantAbsolute = 25.0;

    explicit Leader(const Dominance dominance) noexcept : dominance_(dominance) {}

    // `candidates` is every app measured this sweep. Returns the key to name,
    // or nothing.
    const std::optional<std::wstring>& Update(const std::uint64_t now_ms,
                                              const std::vector<Candidate>& candidates) {
        const double alpha = has_last_ && now_ms > last_ms_
                                 ? 1.0 - std::exp(-static_cast<double>(now_ms - last_ms_) / kTauMs)
                                 : 1.0;
        std::map<std::wstring, double> raw;
        for (const Candidate& c : candidates)
            raw[c.key] += c.value;
        std::map<std::wstring, double> next;
        for (const auto& [key, value] : raw) {
            const auto before = smoothed_.find(key);
            next[key] = before == smoothed_.end()
                            ? value
                            : before->second + alpha * (value - before->second);
        }
        smoothed_ = std::move(next);
        last_ms_ = now_ms;
        has_last_ = true;

        if (shown_ && smoothed_.find(*shown_) == smoothed_.end()) ClearShown();

        // Strict greater-than over an ordered map: a tie goes to the first
        // key, so the answer does not depend on hash order.
        const std::wstring* best = nullptr;
        double best_value = 0.0;
        double total = 0.0;
        for (const auto& [key, value] : smoothed_) {
            total += value;
            if (best == nullptr || value > best_value) {
                best = &key;
                best_value = value;
            }
        }
        const auto dominant = [&](const double value) {
            return dominance_ == Dominance::Absolute
                       ? value >= kDominantAbsolute
                       : total > 0.0 && value >= kDominantShare * total;
        };

        if (!shown_) {
            if (best != nullptr && dominant(best_value)) {
                Count(*best);
                if (streak_ >= kFirstSweeps) Show(*best, now_ms);
            } else {
                challenger_.reset();
                streak_ = 0;
            }
            return shown_;
        }

        const double shown_value = smoothed_.at(*shown_);
        const bool held = now_ms >= shown_since_ms_ + kMinimumShownMs;
        if (best != nullptr && *best != *shown_ && dominant(best_value) &&
            best_value >= kSwitchRatio * shown_value) {
            Count(*best);
            if (streak_ >= kSwitchSweeps && held) {
                Show(*best, now_ms);
                return shown_;
            }
        } else {
            challenger_.reset();
            streak_ = 0;
        }
        if (dominant(shown_value)) {
            weak_sweeps_ = 0;
        } else if (++weak_sweeps_ >= kSwitchSweeps && held) {
            ClearShown();
        }
        return shown_;
    }

    void Reset() noexcept {
        smoothed_.clear();
        has_last_ = false;
        last_ms_ = 0;
        ClearShown();
    }

    [[nodiscard]] const std::optional<std::wstring>& Shown() const noexcept { return shown_; }

private:
    void Count(const std::wstring& key) {
        if (challenger_ && *challenger_ == key) {
            ++streak_;
        } else {
            challenger_ = key;
            streak_ = 1;
        }
    }

    void Show(const std::wstring& key, const std::uint64_t now_ms) {
        shown_ = key;
        shown_since_ms_ = now_ms;
        challenger_.reset();
        streak_ = 0;
        weak_sweeps_ = 0;
    }

    void ClearShown() noexcept {
        shown_.reset();
        challenger_.reset();
        streak_ = 0;
        weak_sweeps_ = 0;
        shown_since_ms_ = 0;
    }

    Dominance dominance_;
    std::map<std::wstring, double> smoothed_;
    std::uint64_t last_ms_{};
    bool has_last_{};
    std::optional<std::wstring> shown_;
    std::uint64_t shown_since_ms_{};
    std::optional<std::wstring> challenger_;
    int streak_{};
    int weak_sweeps_{};
};

}  // namespace gtg::topapp
