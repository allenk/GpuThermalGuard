#pragma once
#include "hook_protocol.hpp"
#include <cstdint>

extern "C" std::uint64_t WINAPI GtgOverlayResearchUploads();
extern "C" std::uint64_t WINAPI GtgOverlayRemoteCopies();
extern "C" std::uint32_t WINAPI GtgOverlayRemotePlacementSerial();
extern "C" std::uint32_t WINAPI GtgOverlayResearchLastSerial();
extern "C" void WINAPI GtgOverlayResearchDestRect(std::int32_t*, std::int32_t*, std::int32_t*, std::int32_t*);

namespace gtg::research {
struct SamplePixels { std::uint32_t inside{}, outside{}, left{}, edge_left{}, edge_right{}; };

inline void SnapshotMotionCounters(HookSection& counters) {
    InterlockedExchange64(&counters.bitmap_copies, static_cast<LONG64>(GtgOverlayRemoteCopies()));
    InterlockedExchange64(&counters.bitmap_uploads, static_cast<LONG64>(GtgOverlayResearchUploads()));
}

// Called on the owned host's single render thread after drawing. Use the
// renderer's accepted serial/rectangle, not concurrently changing publisher data.
inline void CheckMotion(HookSection& counters, const SamplePixels& before, const SamplePixels& after) {
    std::int32_t left{}, top{}, width{}, height{};
    GtgOverlayResearchDestRect(&left, &top, &width, &height);
    const auto serial = GtgOverlayResearchLastSerial();
    const auto color = serial % 2 ? 0xffff00aaU : 0xffff0055U;
    const bool valid = (left == 0 || left == 32) && top == 32 && width == 16 && height == 16 && serial;
    const auto selected_before = left == 0 ? before.left : before.inside;
    const auto selected_after = left == 0 ? after.left : after.inside;
    const bool unchanged_other = left == 0 ? before.inside == after.inside : before.left == after.left;
    if (!valid || selected_before == color || selected_after != color ||
        !unchanged_other || before.outside != after.outside) {
        InterlockedIncrement(&counters.motion_errors);
        return;
    }
    InterlockedIncrement(left == 0 ? &counters.motion_left_samples : &counters.motion_right_samples);
    static std::uint32_t previous_serial{};
    static std::int32_t previous_left = -1;
    if (serial == previous_serial && left != previous_left) InterlockedIncrement(&counters.motion_same_bitmap);
    previous_serial = serial;
    previous_left = left;
    InterlockedExchange(&counters.motion_last_left, left);
    InterlockedExchange(&counters.motion_sampled_placement, static_cast<LONG>(GtgOverlayRemotePlacementSerial()));
}
}  // namespace gtg::research
