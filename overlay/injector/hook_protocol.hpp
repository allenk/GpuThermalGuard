#pragma once
#include "entry_admission.hpp"
#include "load_protocol.hpp"
#include <cstddef>

namespace gtg::research {
constexpr std::uint32_t kHookMagic = 0x35485447;
// 3: the product publishes `signal_slot_address` as well as
// `signal_address`, because Signal is installed by swapping its vtable cell
// rather than by patching its prologue. A reader built against 2 would see a
// zero there and could read it as "no slot" instead of "this writer does not
// publish one", so the contract version is what separates those.
constexpr DWORD kProductVersion = 3;
enum class HookState : LONG { kWaiting, kInstalled, kRefused, kRecoveryRequired, kPartialResident };

// The entry shape of a site whose admission depends on it, as splice's own
// decoder reports it. Recorded on **every** attach and not only on a refusal,
// which is the whole point: a runtime update changed `Signal`'s first
// instruction from five bytes to three, the admission rule refused correctly,
// and nothing anywhere had ever measured that number. `Present` stands on the
// same footing -- its admission still needs `dxgi.dll`'s first instruction to
// be at least the patch length, and a code-generation change there would
// refuse the Direct3D 11 and Direct3D 12 overlays together.
//
// Zero means not measured: either the site was not reached or its prologue did
// not decode. The second of those is now reachable: a site installed by
// swapping its vtable cell is admitted whether or not its prologue decodes,
// because nothing writes there, so an undecodable prologue leaves zeroes here
// instead of refusing the install. This paragraph described that before the
// code did it.
struct alignas(4) EntryShape {
    volatile LONG patch_bytes;
    volatile LONG first_instruction;
    volatile LONG interior_mask;
    volatile LONG entry_padding;
};

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
    // Product v3: the vtable cell that holds `signal_address`, rebased into
    // the target. Appended, like the rest of the product block, so no research
    // counter above it moves. Zero means the helper found no slot it could
    // vouch for, and D3D12 is then refused rather than installed by any other
    // route.
    std::uint64_t signal_slot_address;
    // One per site, indexed by report index -- see `kSiteNames` in the attach
    // log and `kSiteCount`. It held two, for the two sites whose admission the
    // shape governed; the other five are inline-patched with no rule over
    // their shape at all, which is why measuring them is worth more than
    // guessing at whether their offsets could be widened too.
    EntryShape entry_shape[kSiteCount];
};

static_assert(offsetof(HookSection, header) == 0);
static_assert(offsetof(HookSection, hook_calls) % 8 == 0);
static_assert(offsetof(HookSection, first_hook_qpc) % 8 == 0);
}  // namespace gtg::research
