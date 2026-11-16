#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "topapp/board.hpp"
#include "topapp/details.hpp"

namespace gtg::topapp {

// What the sampler publishes: per record, the name to show, if any; and the
// Details table while Details is open.
struct Result {
    std::array<std::optional<std::wstring>, kRecordCount> names;
    std::optional<Details> details;
};

// Finds the app behind each record in alert, on its own thread, and lists the
// top apps of every record while the OSD's Details view is open.
//
// The thread exists only while the reader's setting is on or Details is open,
// and sleeps on an event until some record is in alert or Details opens. Each
// sweep runs only the half its records need -- the GPU counters for GPU and
// VRAM, the process sweep for CPU and RAM -- and both while Details is open.
// It runs in background mode -- lowest CPU, I/O and memory priority -- holds
// no NVML session, and shares no lock, queue or handle with protection. The UI
// thread reaches it through SetMask, SetDetails and TryRead, none of which
// waits.
//
// While Details is open, and only then, the thread also keeps a network trace
// (NetworkMeter): an ETW session and its consumer thread, for Details' network
// column. Without Administrator the trace is refused and the column says so.
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

    // Whether Details is open. Independent of the mask: names follow the
    // alerts alone, and Details follows the reader. Off with a zero mask puts
    // the thread back to sleep.
    void SetDetails(bool open) noexcept;

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
    std::atomic<bool> details_{false};
    std::atomic<std::uint16_t> language_{0x0409};
    mutable std::mutex published_mutex_;
    Result published_;
};

}  // namespace gtg::topapp
