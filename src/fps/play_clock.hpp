#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace gtg::fps {

// How long the reader has been playing, shown low-key in the FPS record
// (owner, 2026-10-07). Time counts only while a program has a stable FPS
// reading, so ALT+TAB, a loading screen or a dropped reading pause it rather
// than reset it.
//
// One counter. The program it counts is identified by its executable, so a
// game that is restarted carries on. The count ends when the program it is
// counting has had no FPS for three minutes -- whether the reader went to
// another game or stopped playing -- and the program that has FPS now takes
// over. Time that program earned during those three minutes is added back, so
// switching games loses nothing. Kept in memory only.
class PlayClock {
public:
    static constexpr std::uint64_t kHandoverMs = 3ULL * 60ULL * 1000ULL;
    // A tick longer than this is not play time: the machine slept, or the
    // caller stalled. Twice GTG's 500 ms OSD cadence is generous.
    static constexpr std::uint64_t kMaxTickMs = 1'000;

    // Called on every UI tick. `program` is the executable of the program
    // whose FPS reading is Ready right now, or nothing.
    void Tick(const std::uint64_t now_ms, const std::optional<std::uint64_t> program) noexcept {
        const std::uint64_t step =
            has_tick_ && now_ms > last_tick_ms_ ? now_ms - last_tick_ms_ : 0;
        last_tick_ms_ = now_ms;
        has_tick_ = true;
        const std::uint64_t counted = step <= kMaxTickMs ? step : 0;

        if (program) {
            if (!current_) {
                Start(*program, now_ms, counted);
            } else if (*program == *current_) {
                elapsed_ms_ += counted;
                last_seen_ms_ = now_ms;
                waiting_.reset();   // back on the program being counted
                waiting_ms_ = 0;
            } else {
                if (waiting_ != program) {
                    waiting_ = program;
                    waiting_ms_ = 0;
                }
                waiting_ms_ += counted;
                waiting_seen_ms_ = now_ms;
            }
        }

        if (current_ && now_ms >= last_seen_ms_ && now_ms - last_seen_ms_ >= kHandoverMs) {
            if (waiting_) {
                current_ = waiting_;
                elapsed_ms_ = waiting_ms_;
                last_seen_ms_ = waiting_seen_ms_;
            } else {
                current_.reset();
                elapsed_ms_ = 0;
            }
            waiting_.reset();
            waiting_ms_ = 0;
        }
    }

    [[nodiscard]] std::uint64_t ElapsedMs() const noexcept { return elapsed_ms_; }
    [[nodiscard]] std::optional<std::uint64_t> Program() const noexcept { return current_; }

private:
    void Start(const std::uint64_t program, const std::uint64_t now_ms,
               const std::uint64_t counted) noexcept {
        current_ = program;
        elapsed_ms_ = counted;
        last_seen_ms_ = now_ms;
        waiting_.reset();
        waiting_ms_ = 0;
    }

    std::optional<std::uint64_t> current_;
    std::uint64_t elapsed_ms_{};
    std::uint64_t last_seen_ms_{};
    std::optional<std::uint64_t> waiting_;
    std::uint64_t waiting_ms_{};
    std::uint64_t waiting_seen_ms_{};
    std::uint64_t last_tick_ms_{};
    bool has_tick_{};
};

// "42 min" below an hour, "7.2 hr" from there, held at "99.9 hr". Nothing
// under a minute: a counter that reads "0 min" is noise beside the FPS.
[[nodiscard]] constexpr std::array<wchar_t, 12> FormatPlayTime(const std::uint64_t elapsed_ms) noexcept {
    std::array<wchar_t, 12> text{};
    const std::uint64_t minutes = elapsed_ms / 60'000;
    if (minutes == 0) return text;
    std::size_t i = 0;
    const auto put = [&](const std::uint64_t value) {
        wchar_t digits[8]{};
        std::size_t n = 0;
        std::uint64_t v = value;
        do { digits[n++] = static_cast<wchar_t>(L'0' + v % 10); v /= 10; } while (v != 0 && n < 8);
        while (n > 0) text[i++] = digits[--n];
    };
    const auto append = [&](const wchar_t* suffix) {
        for (; *suffix != L'\0' && i + 1 < text.size(); ++suffix) text[i++] = *suffix;
    };
    if (minutes < 60) {
        put(minutes);
        append(L" min");
        return text;
    }
    // Tenths of an hour, rounded down so the display never runs ahead.
    std::uint64_t tenths = elapsed_ms / 360'000;
    if (tenths > 999) tenths = 999;
    put(tenths / 10);
    text[i++] = L'.';
    text[i++] = static_cast<wchar_t>(L'0' + tenths % 10);
    append(L" hr");
    return text;
}

}  // namespace gtg::fps
