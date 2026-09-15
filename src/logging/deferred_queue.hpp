#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gtg::logging {

enum class DeferredLevel : std::uint8_t {
    Info,
    Warning,
    Error,
};

template <std::size_t MaxChars>
struct DeferredRecord {
    static_assert(MaxChars > 1);

    DeferredLevel level{DeferredLevel::Info};
    std::array<wchar_t, MaxChars> text{};
    std::size_t length{};

    [[nodiscard]] std::wstring_view View() const noexcept {
        return {text.data(), length};
    }
};

// Fixed-storage multiple-producer/single-consumer queue. Producer admission
// uses one try-only atomic operation: contention drops a diagnostic, never
// spins or waits. The admitted producer owns the SPSC publication sequence.
template <std::size_t Capacity, std::size_t MaxChars>
class DeferredQueue final {
    static_assert(Capacity > 0);

public:
    [[nodiscard]] bool TryPush(const DeferredLevel level,
                               const std::wstring_view message) noexcept {
        if (producer_busy_.test_and_set(std::memory_order_acquire)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        struct ReleaseAdmission {
            std::atomic_flag& flag;
            ~ReleaseAdmission() { flag.clear(std::memory_order_release); }
        } admission{producer_busy_};
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        auto& record = records_[static_cast<std::size_t>(head % Capacity)];
        record.level = level;
        record.length = std::min(message.size(), MaxChars - 1);
        std::copy_n(message.data(), record.length, record.text.data());
        record.text[record.length] = L'\0';
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool TryPop(DeferredRecord<MaxChars>& record) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (tail == head) return false;

        record = records_[static_cast<std::size_t>(tail % Capacity)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint64_t Dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool Empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

private:
    std::atomic_flag producer_busy_ = ATOMIC_FLAG_INIT;
    std::array<DeferredRecord<MaxChars>, Capacity> records_{};
    std::atomic<std::uint64_t> head_{0};
    std::atomic<std::uint64_t> tail_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace gtg::logging
