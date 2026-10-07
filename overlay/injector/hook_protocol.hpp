#pragma once
#include "load_protocol.hpp"
#include <cstddef>

namespace gtg::research {
constexpr std::uint32_t kHookMagic = 0x35485447;
constexpr DWORD kProductVersion = 2;
enum class HookState : LONG { kWaiting, kInstalled, kRefused, kRecoveryRequired, kPartialResident };
struct alignas(8) CallerSample {
    volatile LONG tid;
    volatile LONG calls;
    volatile LONG64 first_queue;
};
struct alignas(8) HookSection {
    LoadSection header;
    DWORD target_tid;
    DWORD reserved;
    std::uint64_t present_address;
    unsigned char expected[16];
    volatile LONG64 hook_calls;
    volatile LONG64 first_hook_qpc;
    volatile LONG failures;
    volatile LONG wrong_thread;
    LONG transaction_stage;
    volatile LONG pixel_samples;
    volatile LONG fixture_pixels;
    volatile LONG outside_changed;
    volatile LONG readback_errors;
    volatile LONG inside_changed;
    std::uint64_t signal_address;
    unsigned char signal_expected[16];
    LONG force_second_refusal;
    volatile LONG64 signal_calls;
    volatile LONG64 nested_signal_calls;
    volatile LONG64 queue_identity;
    volatile LONG queue_ambiguous;
    LONG motion_mode;  // 1: fixed bitmap, 2: 2 Hz content; research only.
    volatile LONG motion_left_samples;
    volatile LONG motion_right_samples;
    volatile LONG motion_same_bitmap;
    volatile LONG motion_errors;
    volatile LONG motion_last_left;
    volatile LONG motion_sampled_placement;
    volatile LONG64 bitmap_copies;
    volatile LONG64 bitmap_uploads;
    LONG control_mode;  // 1: toggle, 2: resize, 3: bitmap scale transition.
    volatile LONG control_samples[3];
    volatile LONG control_errors;
    volatile LONG observed_width;
    volatile LONG observed_height;
    volatile LONG control_epoch;
    volatile LONG leading_large_samples;
    volatile LONG leading_small_samples;
    // Research-only attribution; legacy wrong_thread remains the aggregate.
    volatile LONG present_other_thread;
    volatile LONG nested_signal_other_thread;
    volatile LONG outside_signal_other_thread;
    volatile LONG outside_signal_calls;
    volatile LONG first_present_other_tid;
    volatile LONG first_nested_signal_other_tid;
    volatile LONG first_outside_signal_other_tid;
    volatile LONG64 first_outside_signal_other_queue;
    CallerSample outside_callers[16];
    volatile LONG outside_caller_overflow;
    // Product-only contract, appended so research counters keep their offsets.
    DWORD product_version;
    volatile LONG product_backend; // 1: D11, 2: D12; actual-chain evidence only.
    volatile LONG product_ready;
    std::uint64_t product_methods[5]; // Resize, Resize1, Fullscreen, Present1, ColorSpace.
    unsigned char product_expected[5][16];
    volatile LONG product_patch_copy_size;
    volatile LONG product_patch_boundaries;
    // Product v2: immutable worker-copied policy/target identity; provenance
    // is diagnostic only and can never grant admission.
    DWORD product_color_policy;
    volatile LONG product_color_provenance;
    std::uint64_t product_target_created;
};
static_assert(offsetof(HookSection, header) == 0);
static_assert(offsetof(HookSection, hook_calls) % 8 == 0);
static_assert(offsetof(HookSection, first_hook_qpc) % 8 == 0);
}  // namespace gtg::research
