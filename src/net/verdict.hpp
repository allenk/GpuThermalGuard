#pragma once

#include <cstdint>

// What the TCP stack's own measurements say about a connection, as a verdict.
//
// Pure arithmetic over readings the stack already took. Nothing here sends,
// opens or resolves anything, so the whole judgement is testable without a
// network.
namespace gtg::net {

// One connection's path measurements, differenced over the window the action
// watched. Counters are cumulative, so these are deltas -- the same discipline
// the octet counters need.
struct Readings {
    std::uint32_t smoothed_rtt_ms{};
    std::uint32_t rtt_var_ms{};
    std::uint32_t retransmitted{};
    std::uint32_t duplicate_acks{};
    std::uint32_t timeouts{};
    // How many RTT samples the stack took in the window. Zero means the
    // connection was idle and measured nothing, which is not the same as
    // measuring that it is fine.
    std::uint32_t sampled{};
};

enum class Grade {
    Unknown,   // nothing was measured; say so rather than guess
    Good,
    Fair,
    Poor,
};

// Thresholds.
//
// Provisional, and marked as such: they are the shape of the answer rather
// than the final numbers, which will be chosen the way the cell's text widths
// were -- from measurement, recorded with the readings that justified them.
//
// What is not provisional is the ordering. For a player, jitter and loss
// matter more than distance: a steady 80 ms is playable and a 40 ms that
// swings by 100 is not. So variance is graded harder than latency, and a
// timeout is Poor at any latency.
//
// The starting points come from the connections measured during research: a
// CDN at 1 ms with no variance, regional hosts at 33-99 ms with variance under
// 25, and distant peers at 140-350 ms with variance up to 164 and timeouts in
// double figures.
inline constexpr std::uint32_t kGoodRttMs = 60;
inline constexpr std::uint32_t kFairRttMs = 120;
inline constexpr std::uint32_t kGoodVarMs = 15;
inline constexpr std::uint32_t kFairVarMs = 40;

[[nodiscard]] constexpr Grade GradeLatency(const std::uint32_t rtt_ms) noexcept {
    if (rtt_ms <= kGoodRttMs) return Grade::Good;
    if (rtt_ms <= kFairRttMs) return Grade::Fair;
    return Grade::Poor;
}

[[nodiscard]] constexpr Grade GradeJitter(const std::uint32_t var_ms) noexcept {
    if (var_ms <= kGoodVarMs) return Grade::Good;
    if (var_ms <= kFairVarMs) return Grade::Fair;
    return Grade::Poor;
}

// A timeout means the path lost an entire window and the stack waited out its
// retransmission timer. Nothing about the latency redeems that, so it is Poor
// on its own. Retransmissions without a timeout are recoverable loss, which is
// Fair rather than Poor.
[[nodiscard]] constexpr Grade GradeLoss(const Readings& readings) noexcept {
    if (readings.timeouts > 0) return Grade::Poor;
    if (readings.retransmitted > 0 || readings.duplicate_acks > 0)
        return Grade::Fair;
    return Grade::Good;
}

// The worst of the three, never the average.
//
// Averaging would let a short distance hide heavy loss, which is precisely the
// combination a player notices and a mean does not.
[[nodiscard]] constexpr Grade Worst(const Grade a, const Grade b) noexcept {
    if (a == Grade::Unknown) return b;
    if (b == Grade::Unknown) return a;
    return static_cast<int>(a) > static_cast<int>(b) ? a : b;
}

[[nodiscard]] constexpr Grade Judge(const Readings& readings) noexcept {
    // An idle connection measured nothing. Reporting Good for it would be
    // reporting the absence of evidence as evidence of absence.
    if (readings.sampled == 0) return Grade::Unknown;
    return Worst(Worst(GradeLatency(readings.smoothed_rtt_ms),
                       GradeJitter(readings.rtt_var_ms)),
                 GradeLoss(readings));
}

// The verdict over every connection the foreground program had.
//
// Also the worst, for the same reason: a game with one healthy connection and
// one that is timing out is not half fine. `rtt_ms` reports the connection the
// verdict came from, so the figure and the colour describe the same thing
// rather than two different ones.
struct Verdict {
    Grade grade{Grade::Unknown};
    std::uint32_t rtt_ms{};
    std::uint32_t connections{};   // how many were measured, not how many existed
};

class VerdictBuilder final {
public:
    constexpr void Add(const Readings& readings) noexcept {
        const Grade grade = Judge(readings);
        if (grade == Grade::Unknown) return;
        ++verdict_.connections;
        // The reported figure follows the verdict: when a connection is worse
        // than anything seen so far, its latency is the one worth showing.
        if (static_cast<int>(grade) > static_cast<int>(verdict_.grade) ||
            verdict_.grade == Grade::Unknown) {
            verdict_.grade = grade;
            verdict_.rtt_ms = readings.smoothed_rtt_ms;
        }
    }

    [[nodiscard]] constexpr Verdict Result() const noexcept { return verdict_; }

private:
    Verdict verdict_{};
};

}  // namespace gtg::net
