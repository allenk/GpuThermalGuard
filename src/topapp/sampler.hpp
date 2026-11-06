#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "topapp/board.hpp"

namespace gtg::topapp {

// What the sampler publishes: per record, the name to show, if any.
struct Result {
    std::array<std::optional<std::wstring>, kRecordCount> names;
};

// Finds the app behind each record in alert, on its own thread.
//
// The thread exists only while the reader's setting is on, and sleeps on an
// event until some record is in alert. Each sweep runs only the half its
// records need: the GPU counters for GPU and VRAM, the process sweep for CPU
// and RAM. It runs in background mode -- lowest CPU, I/O and memory priority
// -- holds no NVML session, and shares no lock, queue or handle with
// protection. The UI thread reaches it through SetMask and TryRead, neither
// of which waits.
//
// Measured on the owner's machine with 1008 processes: about 7 ms a sweep for
// the GPU half, 10 ms for the process half, once a second.
class Sampler {
public:
    Sampler() = default;

    ~Sampler() { Stop(); }

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    // Creates the thread, asleep. False if it could not be created.
    [[nodiscard]] bool Start() noexcept;
    // Wakes the thread and joins it. A sweep in progress finishes first, so
    // the join is as long as one sweep.
    void Stop() noexcept;

    [[nodiscard]] bool Running() const noexcept { return thread_.joinable(); }

    // Which records are in alert, as Bit() flags. Zero puts the thread back to
    // sleep and forgets what it had measured.
    void SetMask(std::uint32_t mask) noexcept;

    // The language names should be read in first, as a Windows LANGID.
    void SetLanguage(std::uint16_t language) noexcept {
        language_.store(language, std::memory_order_relaxed);
    }

    // The last published result, or nothing if the sampler is publishing at
    // this instant. Never waits.
    [[nodiscard]] std::optional<Result> TryRead() const;

private:
    void Run() noexcept;
    void Publish(Result result);

    std::thread thread_;
    void* wake_{};   // auto-reset event
    std::atomic<bool> stop_{false};
    std::atomic<std::uint32_t> mask_{0};
    std::atomic<std::uint16_t> language_{0x0409};
    mutable std::mutex published_mutex_;
    Result published_;
};

}  // namespace gtg::topapp
