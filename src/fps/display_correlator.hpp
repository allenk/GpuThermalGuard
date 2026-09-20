#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace gtg::fps {

struct TokenKey {
    std::uint64_t luid{};
    std::uint64_t present_count{};
    std::uint64_t bind_id{};
    [[nodiscard]] constexpr bool operator==(const TokenKey&) const noexcept = default;
};

struct DisplayedFrame {
    std::uint64_t surface{};
    std::uint64_t timestamp_us{};
};

// Single-consumer, fixed-capacity Composed-Flip correlation. It does not
// infer display from a Present call, DWM schedule, or global VSync alone.
// The Tray observer owns this class; no protection component may import it.
class DisplayCorrelator final {
public:
    void Reset() noexcept {
        candidates_ = {};
        tokens_ = {};
        dwm_frames_ = {};
        threads_ = {};
        waiting_ = {};
        output_ = {};
        waiting_count_ = 0;
        output_read_ = output_write_ = 0;
        dwm_thread_ = 0;
        next_serial_ = 1;
        healthy_ = true;
    }

    void MarkLost() noexcept {
        Reset();
        healthy_ = false;
    }

    [[nodiscard]] bool Healthy() const noexcept { return healthy_; }

    void OnPresentStart(std::uint32_t thread, std::uint64_t surface,
                        std::uint32_t flags, std::uint64_t time_us) noexcept {
        if (!healthy_ || (flags & 1U) != 0 || surface == 0) return;
        Expire(time_us);
        if (FindThread(thread) != nullptr) { MarkLost(); return; }
        auto* pending = FreeThread();
        auto* candidate = FreeCandidate();
        if (pending == nullptr || candidate == nullptr) { MarkLost(); return; }
        *candidate = Candidate{surface, time_us, next_serial_++, thread, true};
        *pending = ThreadPresent{thread, candidate->serial, true};
    }

    void OnPresentStop(std::uint32_t thread, std::int32_t result) noexcept {
        if (!healthy_) return;
        auto* pending = FindThread(thread);
        if (pending == nullptr) return;
        auto* candidate = FindCandidate(pending->serial);
        *pending = {};
        if (candidate == nullptr) { MarkLost(); return; }
        candidate->completed = true;
        candidate->successful = result == 0;
        if (candidate->claimed) {
            if (auto* token = FindToken(candidate->token_serial))
                token->accepted = candidate->successful;
            *candidate = {};
        } else if (!candidate->successful) {
            *candidate = {};
        }
    }

    void OnToken(std::uint32_t /*driver_thread*/, TokenKey key,
                 std::uint64_t time_us) noexcept {
        if (!healthy_) return;
        Expire(time_us);
        auto* token = FreeToken();
        if (token == nullptr) { MarkLost(); return; }
        *token = Token{key, 0, time_us, next_serial_++, true};
        Candidate* oldest = nullptr;
        std::uint64_t pending_surface{};
        for (auto& candidate : candidates_) {
            if (!candidate.used || candidate.claimed) continue;
            if (pending_surface != 0 && pending_surface != candidate.surface) {
                MarkLost();
                return;
            }
            pending_surface = candidate.surface;
            if (oldest == nullptr || candidate.time_us < oldest->time_us)
                oldest = &candidate;
        }
        if (oldest == nullptr) return;  // Trace may have begun mid-Present.
        token->surface = oldest->surface;
        token->accepted = oldest->completed && oldest->successful;
        if (oldest->completed) {
            *oldest = {};
        } else {
            oldest->claimed = true;
            oldest->token_serial = token->serial;
        }
    }

    void OnInFrame(TokenKey key) noexcept {
        if (healthy_) if (auto* token = FindToken(key)) token->in_frame = true;
    }

    void OnDiscard(TokenKey key) noexcept {
        if (healthy_) if (auto* token = FindToken(key)) token->discarded = true;
    }

    void OnSurfaceUpdate(TokenKey key) noexcept {
        if (!healthy_) return;
        auto* token = FindToken(key);
        if (token == nullptr || !token->in_frame || token->scheduled) return;
        if (waiting_count_ == waiting_.size()) { MarkLost(); return; }
        token->scheduled = true;
        waiting_[waiting_count_++] = token->serial;
    }

    void SetDwmThread(std::uint32_t thread) noexcept {
        if (!healthy_) return;
        if (dwm_thread_ != 0 && dwm_thread_ != thread) {
            if (waiting_count_ != 0) { MarkLost(); return; }
            for (const auto& frame : dwm_frames_)
                if (frame.used) { MarkLost(); return; }
        }
        dwm_thread_ = thread;
    }

    void OnDwmFlip(std::uint32_t thread, std::uint64_t time_us) noexcept {
        if (!healthy_ || thread != dwm_thread_ || thread == 0) return;
        Expire(time_us);
        auto* frame = FreeDwmFrame();
        if (frame == nullptr) { MarkLost(); return; }
        frame->used = true;
        frame->time_us = time_us;
        frame->count = waiting_count_;
        for (std::size_t i = 0; i < waiting_count_; ++i)
            frame->token_serials[i] = waiting_[i];
        waiting_count_ = 0;
    }

    void OnDwmQueue(std::uint32_t thread, std::uint32_t sequence) noexcept {
        if (!healthy_ || thread != dwm_thread_ || sequence == 0) return;
        DwmFrame* oldest = nullptr;
        for (auto& frame : dwm_frames_) {
            if (!frame.used || frame.sequence != 0) continue;
            if (oldest == nullptr || frame.time_us < oldest->time_us)
                oldest = &frame;
        }
        if (oldest != nullptr) oldest->sequence = sequence;
    }

    void OnVSync(std::uint32_t sequence, std::uint64_t time_us) noexcept {
        if (!healthy_ || sequence == 0) return;
        for (auto& frame : dwm_frames_) {
            if (!frame.used || frame.sequence != sequence) continue;
            for (std::size_t i = 0; i < frame.count; ++i) {
                auto* token = FindToken(frame.token_serials[i]);
                if (token == nullptr || !token->accepted || token->discarded ||
                    token->displayed || token->surface == 0) continue;
                // A compositor frame can carry several updates to one surface;
                // only the newest one may be the visible image.
                bool superseded = false;
                for (std::size_t j = i + 1; j < frame.count; ++j) {
                    const auto* newer = FindToken(frame.token_serials[j]);
                    if (newer && newer->surface == token->surface &&
                        newer->accepted && !newer->discarded) {
                        superseded = true;
                        break;
                    }
                }
                token->displayed = true;
                if (!superseded) Push({token->surface, time_us});
            }
            frame = {};
            return;
        }
    }

    [[nodiscard]] std::optional<DisplayedFrame> Pop() noexcept {
        if (!healthy_ || output_read_ == output_write_) return std::nullopt;
        const auto frame = output_[output_read_];
        output_read_ = (output_read_ + 1) % output_.size();
        return frame;
    }

private:
    static constexpr std::uint64_t kMaxAgeUs = 2'000'000;
    struct Candidate {
        std::uint64_t surface{};
        std::uint64_t time_us{};
        std::uint64_t serial{};
        std::uint32_t thread{};
        bool used{};
        bool completed{};
        bool successful{};
        bool claimed{};
        std::uint64_t token_serial{};
    };
    struct Token {
        TokenKey key{};
        std::uint64_t surface{};
        std::uint64_t time_us{};
        std::uint64_t serial{};
        bool used{};
        bool accepted{};
        bool in_frame{};
        bool scheduled{};
        bool discarded{};
        bool displayed{};
    };
    struct DwmFrame {
        std::array<std::uint64_t, 32> token_serials{};
        std::uint64_t time_us{};
        std::uint32_t sequence{};
        std::size_t count{};
        bool used{};
    };
    struct ThreadPresent {
        std::uint32_t thread{};
        std::uint64_t serial{};
        bool used{};
    };

    [[nodiscard]] ThreadPresent* FindThread(std::uint32_t thread) noexcept {
        for (auto& item : threads_)
            if (item.used && item.thread == thread) return &item;
        return nullptr;
    }
    [[nodiscard]] ThreadPresent* FreeThread() noexcept {
        for (auto& item : threads_) if (!item.used) return &item;
        return nullptr;
    }
    [[nodiscard]] Candidate* FreeCandidate() noexcept {
        for (auto& item : candidates_) if (!item.used) return &item;
        return nullptr;
    }
    [[nodiscard]] Candidate* FindCandidate(std::uint64_t serial) noexcept {
        for (auto& item : candidates_)
            if (item.used && item.serial == serial) return &item;
        return nullptr;
    }
    [[nodiscard]] Token* FreeToken() noexcept {
        for (auto& item : tokens_) if (!item.used) return &item;
        return nullptr;
    }
    [[nodiscard]] Token* FindToken(TokenKey key) noexcept {
        for (auto& item : tokens_)
            if (item.used && item.key == key) return &item;
        return nullptr;
    }
    [[nodiscard]] Token* FindToken(std::uint64_t serial) noexcept {
        for (auto& item : tokens_)
            if (item.used && item.serial == serial) return &item;
        return nullptr;
    }
    [[nodiscard]] DwmFrame* FreeDwmFrame() noexcept {
        for (auto& item : dwm_frames_) if (!item.used) return &item;
        return nullptr;
    }
    void Push(DisplayedFrame frame) noexcept {
        const auto next = (output_write_ + 1) % output_.size();
        if (next == output_read_) { MarkLost(); return; }
        output_[output_write_] = frame;
        output_write_ = next;
    }
    void Expire(std::uint64_t now_us) noexcept {
        for (auto& item : candidates_) {
            if (item.used && now_us >= item.time_us &&
                now_us - item.time_us > kMaxAgeUs) item = {};
        }
        for (auto& item : tokens_) {
            if (item.used && now_us >= item.time_us &&
                now_us - item.time_us > kMaxAgeUs) item = {};
        }
        for (auto& item : dwm_frames_) {
            if (item.used && now_us >= item.time_us &&
                now_us - item.time_us > kMaxAgeUs) item = {};
        }
    }

    std::array<Candidate, 2048> candidates_{};
    std::array<Token, 2048> tokens_{};
    std::array<DwmFrame, 128> dwm_frames_{};
    std::array<ThreadPresent, 32> threads_{};
    std::array<std::uint64_t, 32> waiting_{};
    std::array<DisplayedFrame, 32> output_{};
    std::size_t waiting_count_{};
    std::size_t output_read_{};
    std::size_t output_write_{};
    std::uint32_t dwm_thread_{};
    std::uint64_t next_serial_{1};
    bool healthy_{true};
};

}  // namespace gtg::fps
