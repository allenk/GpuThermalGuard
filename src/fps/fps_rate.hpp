#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gtg::fps {

struct Identity {
    std::uint32_t pid{};
    std::uint64_t creation_time{};

    [[nodiscard]] constexpr bool operator==(const Identity&) const noexcept = default;
};

enum class Status { Unavailable, Warmup, Ready };

// Which quantity the current window is counting.
//
// `Displayed` is a frame Windows confirmed reached the screen, correlated
// across DXGI, Win32k and DWM. `Presented` is a call the program made to put a
// frame up, counted in the graphics kernel, with no confirmation that it was
// shown.
//
// The second exists because the first is impossible for Vulkan and OpenGL:
// they produce no DXGI Present and no Win32k composition token, and the chain
// from a window to a composition surface was chased to the end and does not
// close -- see docs/fps-and-windows-composition.md. The owner's ruling is that
// this record is a reference rather than a guarantee, so a presentation rate
// is worth more than a permanent dash.
//
// The two are never mixed in one window: a reading of the other kind restarts
// it.
enum class Measure { Displayed, Presented };

// Which graphics API the reading came from, as far as it can be established.
//
// `Unknown` is a real answer and the default: the precise name is not in any
// ETW event. D3DKMT_CLIENTHINT names every one of these, but it is set when the
// program creates its kernel context, long before an observer that follows the
// foreground can be listening. What remains is the module list, which can be
// refused -- so the name is a label, never a precondition, and its absence
// never affects the number.
enum class Backend { Unknown, D3D9, D3D11, D3D12, DXGI, Vulkan, OpenGL };

[[nodiscard]] constexpr const wchar_t* BackendLabel(const Backend backend) noexcept {
    switch (backend) {
        case Backend::D3D9: return L"D9";
        case Backend::D3D11: return L"D11";
        case Backend::D3D12: return L"D12";
        // The events proved a DXGI runtime but the modules could not say which.
        case Backend::DXGI: return L"DX";
        case Backend::Vulkan: return L"VK";
        case Backend::OpenGL: return L"GL";
        case Backend::Unknown: break;
    }
    return L"";
}

// Where a resolution came from, because the two are not the same claim.
//
// `Presented` is the swapchain the program actually put up -- measured: giving
// a generator buffers a quarter of its window made Win32k 201 report the
// quarter, not the window. `Output` is the window or the monitor, used only
// when the presented size could not be established.
//
// Neither is the engine's internal render target. A game upscaling with DLSS or
// FSR resolves into its own full-size swapchain and that texture never leaves
// the process, so no provider can see it; such a game correctly reports its
// full presented size. Establishing the internal one needs in-process code --
// the overlay research, not this.
enum class ResolutionSource { None, Presented, Output };

struct Resolution {
    std::uint32_t width{};
    std::uint32_t height{};
    ResolutionSource source{ResolutionSource::None};

    [[nodiscard]] constexpr bool Known() const noexcept {
        return source != ResolutionSource::None && width != 0 && height != 0;
    }
};

struct Snapshot {
    Status status{Status::Unavailable};
    double displayed_fps{};
    Identity identity{};
    std::uint64_t surface{};
    std::uint64_t sampled_at_us{};
    // Last, so that existing brace initialisers keep compiling: these say
    // what `displayed_fps` is and what produced it, not what it is.
    Measure measure{Measure::Displayed};
    Backend backend{Backend::Unknown};
    Resolution resolution{};
};

// Display-outcome-only, single-writer rate state. Neither this type nor its
// consumers are permitted in the protection deadline path.
class RateTracker {
public:
    void SetTarget(Identity identity, std::uint64_t now_us) noexcept {
        if (has_target_ && target_ == identity) return;
        target_ = identity;
        has_target_ = identity.pid != 0 && identity.creation_time != 0;
        measure_ = Measure::Displayed;
        last_displayed_us_ = 0;
        ResetWindow(now_us);
    }

    void ClearTarget() noexcept {
        target_ = {};
        has_target_ = false;
        measure_ = Measure::Displayed;
        last_displayed_us_ = 0;
        ResetWindow(0);
    }

    void MarkLost(std::uint64_t now_us) noexcept { ResetWindow(now_us); }

    // A frame Windows confirmed on screen. The DXGI path.
    void RecordDisplayed(Identity identity, std::uint64_t surface,
                         std::uint64_t timestamp_us) noexcept {
        Record(identity, surface, timestamp_us, Measure::Displayed);
    }

    // A present the program submitted, counted in the graphics kernel. The key
    // is the window rather than a composition surface, because that is what
    // DxgKrnl carries and because the dominant-surface rules below work the
    // same on either: they need a stable identity per presenting thing, not a
    // particular kind of identity.
    void RecordPresented(Identity identity, std::uint64_t window,
                         std::uint64_t timestamp_us) noexcept {
        Record(identity, window, timestamp_us, Measure::Presented);
    }

    void Record(Identity identity, std::uint64_t surface,
                std::uint64_t timestamp_us, Measure measure) noexcept {
        if (!has_target_ || identity != target_ || surface == 0 ||
            timestamp_us < window_started_us_) return;
        // A DXGI program produces BOTH kinds, continuously: every frame is a
        // DXGI Present that becomes a displayed frame, and also a kernel flip.
        // An earlier draft let the newer kind win, which reset the window on
        // every alternation and left D3D11 and D3D12 permanently in Warmup --
        // measured, not reasoned about.
        //
        // Displayed wins, because it is the stronger claim: Windows confirmed
        // the frame reached the screen. Presented is ignored while a displayed
        // frame is recent.
        //
        // "Recent" rather than "ever", so the fallback is automatic: a program
        // that stops producing displayed frames -- true exclusive fullscreen
        // takes the composition token away -- drops to its presentation rate
        // after the same staleness window the surface rules use, instead of
        // going dark.
        if (measure == Measure::Presented) {
            if (last_displayed_us_ != 0 && timestamp_us >= last_displayed_us_ &&
                timestamp_us - last_displayed_us_ <= kSurfaceStaleUs) return;
        } else {
            last_displayed_us_ = timestamp_us;
        }
        if (measure != measure_) {
            ResetWindow(timestamp_us);
            measure_ = measure;
        }
        if (too_many_surfaces_ && timestamp_us >= capacity_lost_at_us_ &&
            timestamp_us - capacity_lost_at_us_ > kSurfaceStaleUs) {
            bool another_recent_surface = false;
            for (const auto& entry : surfaces_) {
                if (entry.key != 0 && entry.key != surface &&
                    timestamp_us >= entry.last_us &&
                    timestamp_us - entry.last_us <= kSurfaceStaleUs) {
                    another_recent_surface = true;
                    break;
                }
            }
            if (!another_recent_surface) ResetWindow(timestamp_us);
        }
        Surface* selected = nullptr;
        Surface* oldest_stale = nullptr;
        for (auto& entry : surfaces_) {
            if (entry.key == surface) {
                selected = &entry;
                break;
            }
            if (entry.key == 0 && selected == nullptr) selected = &entry;
            if (entry.key != 0 && timestamp_us >= entry.last_us &&
                timestamp_us - entry.last_us > kSurfaceStaleUs &&
                (oldest_stale == nullptr || entry.last_us < oldest_stale->last_us))
                oldest_stale = &entry;
        }
        if (selected == nullptr && oldest_stale != nullptr) {
            *oldest_stale = {};
            selected = oldest_stale;
        }
        if (selected == nullptr) {
            too_many_surfaces_ = true;
            capacity_lost_at_us_ = timestamp_us;
            return;
        }
        if (selected->key == 0) selected->key = surface;
        if (selected->count != 0 && timestamp_us < selected->last_us) return;
        selected->last_us = timestamp_us;
        selected->times[selected->next] = timestamp_us;
        selected->next = (selected->next + 1) % kFrameCapacity;
        if (selected->count < kFrameCapacity) {
            ++selected->count;
        } else {
            selected->wrapped = true;
        }
    }

    [[nodiscard]] Snapshot Read(std::uint64_t now_us,
                                bool observer_healthy) const noexcept {
        Snapshot result{};
        result.identity = target_;
        result.measure = measure_;
        result.sampled_at_us = now_us;
        if (!has_target_ || !observer_healthy || too_many_surfaces_ ||
            now_us < window_started_us_) return result;
        if (now_us - window_started_us_ < kWindowUs) {
            result.status = Status::Warmup;
            return result;
        }

        std::uint64_t latest_event_us = 0;
        for (const auto& entry : surfaces_) {
            if (entry.count != 0 && entry.last_us > latest_event_us)
                latest_event_us = entry.last_us;
        }
        if (latest_event_us > now_us) return result;
        if (latest_event_us == 0 || now_us - latest_event_us > kBufferHoldUs) {
            return result;
        }
        // Real-time ETW events arrive in buffers. Use the latest event's
        // timestamp for rate math and hold it only through a bounded gap.
        const std::uint64_t cutoff = latest_event_us > kWindowUs
            ? latest_event_us - kWindowUs : 0;
        std::size_t best_count = 0;
        std::size_t second_count = 0;
        std::uint64_t first_us = 0;
        std::uint64_t last_us = 0;
        bool best_truncated = false;
        for (const auto& entry : surfaces_) {
            if (entry.key == 0) continue;
            std::size_t count = 0;
            std::uint64_t first = 0;
            std::uint64_t last = 0;
            for (std::size_t i = 0; i < entry.count; ++i) {
                const auto time = entry.times[i];
                if (time < cutoff || time > latest_event_us) continue;
                if (count == 0 || time < first) first = time;
                if (time > last) last = time;
                ++count;
            }
            if (count > best_count) {
                second_count = best_count;
                best_count = count;
                first_us = first;
                last_us = last;
                best_truncated = entry.wrapped &&
                    entry.times[entry.next] > cutoff;
                result.surface = entry.key;
            } else if (count > second_count) {
                second_count = count;
            }
        }
        // A full fixed ring can truncate the one-second interval. Do not show
        // a confident number until a subsequent clean target/window begins.
        if (best_truncated || (second_count != 0 && best_count < second_count * 2)) {
            result.surface = 0;
            return result;
        }
        if (best_count == 0) {
            return result;
        }
        // Enough elapsed time OR enough samples. The guard exists to stop a
        // rate being computed from too FEW frames -- two of them 5 ms apart
        // implying 200 -- and duration is only a proxy for that. The proxy
        // fails precisely when a program is fast: measured on Affinity Photo
        // 2, a burst of 40 flips with a 7.8-9.8 ms spread lasts 326 ms and
        // could never reach 500, so it sat at Warmup for its whole life, went
        // Unavailable in the silence after it, and did the same again on the
        // next burst. That cycle was ours. Forty samples at a sub-2 ms spread
        // is a better-conditioned estimate than many that do span a second.
        if (best_count < 2 || last_us <= first_us ||
            (last_us - first_us < kMinMeasuredSpanUs &&
             best_count < kMinMeasuredSamples)) {
            result.status = Status::Warmup;
            return result;
        }
        result.status = Status::Ready;
        result.displayed_fps = static_cast<double>(best_count - 1) *
            static_cast<double>(kWindowUs) / static_cast<double>(last_us - first_us);
        return result;
    }

private:
    Measure measure_{Measure::Displayed};
    std::uint64_t last_displayed_us_{};

    static constexpr std::uint64_t kWindowUs = 1'000'000;
    // Session warm-up alone does not make a tiny burst representative of a
    // sustained rate. Require enough of the frame-time window to be observed.
    static constexpr std::uint64_t kMinMeasuredSpanUs = 500'000;
    // The count that can stand in for the span above. Below a dozen frames a
    // burst is under 100 ms at any rate worth reporting, which is too brief to
    // be worth a number whichever way the test is written.
    static constexpr std::size_t kMinMeasuredSamples = 12;
    static constexpr std::uint64_t kBufferHoldUs = 2'000'000;
    static constexpr std::uint64_t kSurfaceStaleUs = 2'000'000;
    static constexpr std::size_t kSurfaceCapacity = 4;
    static constexpr std::size_t kFrameCapacity = 1024;

    struct Surface {
        std::uint64_t key{};
        std::array<std::uint64_t, kFrameCapacity> times{};
        std::size_t next{};
        std::size_t count{};
        std::uint64_t last_us{};
        bool wrapped{};
    };

    // The measure is deliberately NOT reset here. ResetWindow is called from
    // Record when the kind changes, and Record sets the new one immediately
    // afterwards; clearing it here would undo that. Target changes go through
    // SetTarget and ClearTarget, which set it explicitly.
    void ResetWindow(std::uint64_t now_us) noexcept {
        surfaces_ = {};
        too_many_surfaces_ = false;
        capacity_lost_at_us_ = 0;
        window_started_us_ = now_us;
    }

    Identity target_{};
    std::array<Surface, kSurfaceCapacity> surfaces_{};
    std::uint64_t window_started_us_{};
    bool has_target_{};
    bool too_many_surfaces_{};
    std::uint64_t capacity_lost_at_us_{};
};

}  // namespace gtg::fps
