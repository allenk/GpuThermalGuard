// Design §10.1's yardstick: what the overlay costs the host's render thread,
// per frame and per upload, measured with QPC and kept as a distribution.
//
// Shared by both probes so D3D11 and D3D12 are measured the same way, with the
// same segment numbers, and read by the same kind of harness. A step an API does
// not have stays zero: D3D11 has no allocator to reset and no separate submit.
//
// Research instrumentation, not part of any shipped overlay: it costs two QPC
// reads per segment and a few stores per frame, and the numbers it produces are
// what decided the cache layers.
#pragma once

#include <windows.h>

#include "published_bitmap.hpp"   // QpcNs, ConsumerStats

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gtg::overlay::probe::cost {

constexpr std::size_t kSamples = 8192;
constexpr int kSegments = 8;

// Where a frame's time goes. Indices are part of the export contract: the
// generators print them by these names, in this order.
enum Segment : int {
    kPre = 0,   // swapchain description, back-buffer index, slot checks
    kReset,     // allocator and list reset (D3D12, fallback or upload frames)
    kImGui,     // NewFrame, AddImage, Render -- only where L2 rebuilds
    kReport,    // ReportTarget, through L0's cache
    kAcquire,   // Acquire, and any upload it records
    kRecord,    // render-target binding, RenderDrawData, state restore
    kSubmit,    // ExecuteCommandLists + Signal (D3D12)
    kRemoved,   // GetDeviceRemovedReason
};

inline std::atomic<std::uint64_t> g_frame_ns_total{0};
inline std::atomic<std::uint64_t> g_frame_ns_max{0};
inline std::atomic<std::uint64_t> g_frames{0};
inline std::uint32_t g_frame_ns[kSamples]{};
inline std::uint32_t g_segment_ns[kSegments][kSamples]{};
inline std::atomic<std::uint32_t> g_samples{0};
inline thread_local std::uint32_t t_segment[kSegments]{};
inline thread_local std::uint64_t t_mark = 0;

// Start of the measured part of a frame.
inline void Begin() {
    t_mark = QpcNs();
}

// Charge the time since the previous mark to `segment`. Accumulates, so a
// segment that happens in two pieces (D3D11's ImGui calls) is counted whole.
inline void Mark(Segment segment) {
    const std::uint64_t now = QpcNs();
    t_segment[segment] += static_cast<std::uint32_t>(now - t_mark);
    t_mark = now;
}

// Called once per Present with the whole overlay's time for that frame.
inline void Record(std::uint64_t overlay_ns) {
    g_frame_ns_total.fetch_add(overlay_ns, std::memory_order_relaxed);
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (overlay_ns > g_frame_ns_max.load(std::memory_order_relaxed)) {
        g_frame_ns_max.store(overlay_ns, std::memory_order_relaxed);
    }
    const std::uint32_t at = g_samples.load(std::memory_order_relaxed);
    if (at < kSamples) {
        g_frame_ns[at] = static_cast<std::uint32_t>(overlay_ns);
        for (int i = 0; i < kSegments; ++i)
            g_segment_ns[i][at] = t_segment[i];
        g_samples.store(at + 1, std::memory_order_relaxed);
    }
    for (auto& segment : t_segment)
        segment = 0;
}

}  // namespace gtg::overlay::probe::cost

// The exports, defined once per probe DLL. `stats` is the probe's
// ConsumerStats (published.stats()).
#define GTG_FRAME_COST_EXPORTS(stats)                                                             \
    extern "C" __declspec(dllexport) void WINAPI GtgOverlayResearchCost(                          \
        std::uint64_t* upload_total, std::uint64_t* upload_max, std::uint64_t* uploads,           \
        std::uint64_t* frame_total, std::uint64_t* frame_max, std::uint64_t* frames) {            \
        namespace c = gtg::overlay::probe::cost;                                                  \
        auto& st = (stats);                                                                       \
        if (upload_total) *upload_total = st.upload_ns_total.load();                              \
        if (upload_max) *upload_max = st.upload_ns_max.load();                                    \
        if (uploads) *uploads = st.uploads.load();                                                \
        if (frame_total) *frame_total = c::g_frame_ns_total.load();                               \
        if (frame_max) *frame_max = c::g_frame_ns_max.load();                                     \
        if (frames) *frames = c::g_frames.load();                                                 \
    }                                                                                             \
    extern "C" __declspec(dllexport) void WINAPI GtgOverlayResearchUploadSteady(                  \
        std::uint64_t* total_ns, std::uint64_t* count) {                                          \
        auto& st = (stats);                                                                       \
        if (total_ns) *total_ns = st.upload_ns_steady_total.load();                               \
        if (count) *count = st.uploads_steady.load();                                             \
    }                                                                                             \
    extern "C" __declspec(dllexport) std::uint32_t WINAPI GtgOverlayResearchCostSamples(          \
        const std::uint32_t** frame_ns, const std::uint32_t** segments, int* segment_count,       \
        std::uint32_t* stride) {                                                                  \
        namespace c = gtg::overlay::probe::cost;                                                  \
        if (frame_ns) *frame_ns = c::g_frame_ns;                                                  \
        if (segments) *segments = &c::g_segment_ns[0][0];                                         \
        if (segment_count) *segment_count = c::kSegments;                                         \
        if (stride) *stride = static_cast<std::uint32_t>(c::kSamples);                            \
        return c::g_samples.load();                                                               \
    }                                                                                             \
    extern "C" __declspec(dllexport) void WINAPI GtgOverlayResearchPlacement(                     \
        std::uint64_t* applied, std::uint64_t* rejected, std::uint64_t* latency_total_us,         \
        std::uint64_t* latency_max_us, std::uint64_t* uploads) {                                  \
        auto& st = (stats);                                                                       \
        if (applied) *applied = st.placements_applied.load();                                     \
        if (rejected) *rejected = st.placements_rejected.load();                                  \
        if (latency_total_us) *latency_total_us = st.placement_latency_us_total.load();           \
        if (latency_max_us) *latency_max_us = st.placement_latency_us_max.load();                 \
        if (uploads) *uploads = st.uploads.load();                                                \
    }                                                                                             \
    extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchAwaitingFrameSize() { \
        return (stats).awaiting_frame_size.load();                                                \
    }
