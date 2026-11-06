#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "hostlog/frame.hpp"

// The Hosts log's writer: one Tray thread, which exists only while the log is
// on, in background mode.
//
// The UI hands it a frame with Submit, which never waits. The thread formats
// the frame and appends it to a file of its own, one line per frame, kept to
// 1 MiB plus one ".1". It does not use the process journal, its queue or its
// writer thread, and it shares no lock, queue or handle with protection.
namespace gtg::hostlog {

inline constexpr std::uint64_t kRotateAtBytes = 1024 * 1024;
inline constexpr std::uint64_t kFrameIntervalMs = 5'000;
inline constexpr wchar_t kFileName[] = L"GpuThermalGuard-hosts.log";

// One value handed from one thread to another. The giver never waits: a slot
// the taker holds at that instant refuses the value. The taker waits only for
// the giver's own move.
template <typename T>
class Mailbox {
public:
    enum class Put {
        Taken,
        Replaced,   // an earlier value that was never taken is gone
        Busy,       // refused: the value is dropped
    };

    [[nodiscard]] Put TryPut(T value) {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return Put::Busy;
        const bool replaced = slot_.has_value();
        slot_ = std::move(value);
        return replaced ? Put::Replaced : Put::Taken;
    }

    [[nodiscard]] std::optional<T> Take() {
        std::optional<T> out;
        std::scoped_lock lock(mutex_);
        out.swap(slot_);
        return out;
    }

private:
    std::mutex mutex_;
    std::optional<T> slot_;
};

class Writer {
public:
    struct Options {
        std::filesystem::path file;
        std::uint64_t rotate_at_bytes{kRotateAtBytes};
    };

    Writer() = default;

    ~Writer() { Stop(); }

    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    [[nodiscard]] bool Start(Options options);
    // Waits at most 2 s. A thread still inside a write then finishes it, and
    // exits, on its own: a stalled drive never holds the UI.
    void Stop() noexcept;

    [[nodiscard]] bool Running() const noexcept { return shared_ != nullptr; }

    // Numbers the frame and hands it over. Never waits; false when the slot was
    // busy and the frame was dropped.
    bool Submit(Frame frame);

    // Frames dropped or not written, and lines written, in this run.
    [[nodiscard]] std::uint64_t Dropped() const noexcept;
    [[nodiscard]] std::uint64_t Written() const noexcept;

    [[nodiscard]] const std::string& RunId() const noexcept { return run_id_; }

    // What the thread uses, defined with it.
    struct Shared;

private:
    std::shared_ptr<Shared> shared_;
    std::thread thread_;
    std::string run_id_;
    std::uint64_t seq_{};
};

}  // namespace gtg::hostlog
