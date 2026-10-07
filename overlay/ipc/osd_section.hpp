// Spike 2 T-03 — the shared layout from design section 8.
//
// Header plus two buffers, an atomic published index, and no lock. The reason
// it is double-buffered rather than a seqlock is written in section 8 and is
// worth repeating where the code is: the reader runs on the GAME'S RENDER
// THREAD, and a seqlock that fails has to retry. A retry loop there steals
// frame time from the thing the overlay exists to sit politely on top of.
// Double buffering makes the reader succeed on its first attempt, always, and
// the price is one more buffer.
//
// Atomics are std::atomic_ref over plain integers rather than std::atomic
// members. Two processes map this struct and must agree on its layout byte for
// byte; a plain uint32_t has one obvious representation, while embedding
// std::atomic invites a question about whether both compilations produced the
// same thing. Same reasoning splice uses for patching an instruction stream.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gtg::overlay::ipc {

// The real one. The protocol probe uses a name of its own so a T-03 stress run
// cannot collide with a publisher somebody left running.
inline constexpr wchar_t kSectionName[] = L"Local\\GpuThermalGuard.Osd.v1";

inline constexpr std::uint32_t kMagic = 0x4F475447;   // "GTGO"
// 2: Spike 4 added the placement record. A reader checks the version and the
// header size, so a v1 reader refuses a v2 writer rather than misreading it.
inline constexpr std::uint32_t kVersion = 2;

// Generous enough for the expanded dashboard at the largest offered scale
// (x2.50 of 388x330 is 970x825) with room above it. Two of them is the whole
// cost of not needing a lock.
inline constexpr std::uint32_t kMaxWidth = 1280;
inline constexpr std::uint32_t kMaxHeight = 1024;
inline constexpr std::size_t kMaxPixelBytes =
    static_cast<std::size_t>(kMaxWidth) * kMaxHeight * 4;

enum class PixelFormat : std::uint32_t { Bgra8 = 0 };
enum class AlphaMode : std::uint32_t { Straight = 0, Premultiplied = 1 };
enum class AnchorCorner : std::uint32_t {
    TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3
};

// Everything about one frame except the pixels, kept beside them so a reader
// that has chosen a buffer needs no second lookup and no second acquire.
struct FrameHeader {
    std::uint32_t serial;        // advances only when the IMAGE changed
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    std::uint32_t format;        // PixelFormat
    std::uint32_t alpha_mode;    // AlphaMode
    std::uint32_t anchor_corner; // AnchorCorner
    std::int32_t  offset_dip_x;
    std::int32_t  offset_dip_y;
    std::uint32_t mode;          // 0 compact, 1 expanded
    std::uint32_t byte_count;    // height * stride, <= kMaxPixelBytes

    // Written BEFORE the pixels; `serial` above is written AFTER them. A reader
    // that sees the two disagree was copying a buffer the writer had started to
    // refill. See the validity note below -- this is what T-03 had to add.
    std::uint32_t serial_begin;

    // v2 (Spike 4): the frame size this image was sized and placed for, or 0
    // when it was placed by anchor and offset alone. Until the reverse request
    // has reached the writer, it can only guess the frame; a reader whose frame
    // differs draws nothing rather than draw a dashboard placed for another
    // size -- rule 4, never outside the frame. T-05 found the guess on screen
    // for 7 frames.
    std::uint32_t frame_width;
    std::uint32_t frame_height;
};

struct Frame {
    FrameHeader header;
    std::uint8_t pixels[kMaxPixelBytes];
};

// Spike 4: where the dashboard goes, published apart from what it looks like.
//
// The bitmap changes at 2 Hz; the desktop OSD can be dragged far faster, and
// the overlay should follow it (owner, 2026-10-02). So placement has its own
// record and its own serial -- the "dirty counter" -- and the reader checks that
// serial once per frame: unchanged, nothing happens; changed, the rectangle
// moves and the texture is not uploaded again.
//
// The rectangle is in frame pixels, computed by the writer from the rule
// (overlay/publisher/osd_geometry.hpp) for the frame size the reverse request
// reported. A reader applies it only when its size equals the texture it has:
// a placement computed for the next bitmap (the OSD dragged onto a monitor of
// another scale) must not stretch the current one for the moment between.
//
// Same begin/end serial guard as a frame (T-03), for the same reason, with the
// same failure path: keep what you had.
struct Placement {
    std::uint32_t serial_begin;
    std::int32_t  left;
    std::int32_t  top;
    std::int32_t  width;
    std::int32_t  height;
    std::uint32_t frame_width;    // the frame it was computed for
    std::uint32_t frame_height;
    std::uint32_t reserved;
    std::uint64_t written_us;     // QPC microseconds, for the latency measurement
    std::uint32_t serial;
    std::uint32_t reserved2;
};

struct Section {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t header_size;   // so a newer reader can refuse an older writer
    std::uint32_t writer_pid;    // the reader confirms the publisher is alive

    std::uint64_t heartbeat_us;  // stops advancing when the publisher dies
    std::uint32_t enabled;       // the last of the four gates
    std::uint32_t published_index;   // ATOMIC: 0 or 1

    std::uint32_t protection_state;
    std::uint32_t reserved0;
    std::uint64_t transition_us;

    Placement placement;          // v2
    Frame buffer[2];
};

static_assert(sizeof(Frame) == sizeof(FrameHeader) + kMaxPixelBytes,
              "Frame must have no padding between its header and its pixels: "
              "two processes map this and a compiler-inserted gap in one of "
              "them would silently offset every pixel");

inline constexpr std::size_t kSectionBytes = sizeof(Section);

// ─── the published index, and why it is not the whole protocol ─────────────
//
// Writer:  fill buffer[1 - published], then publish(1 - published)
// Reader:  i = published(), then read buffer[i]
//
// Release on the store and acquire on the load is what makes the pixels written
// before the store visible to a reader that saw it. That pairing is necessary
// and it is NOT sufficient, which T-03 measured rather than argued:
//
//     writer flat out, reader copying 544 KB and checking every byte
//     -> thousands of torn frames in a 30 s run
//
// Section 8 says "double buffering makes the reader succeed the first time,
// always". The first half is true; `always` is not. With two buffers the
// reader's safe window is ONE publish interval, not two: the moment the writer
// finishes publishing 1-i it begins filling i, which is the buffer the reader
// holds. If the reader's copy outlasts a publish, the writer is writing into it.
//
// At the shipping cadence the margin is enormous -- a 2 Hz publisher against a
// sub-millisecond copy -- but a margin is not a protocol, and the reader lives
// on the game's render thread, which is exactly the thread that gets preempted,
// page-faults on 544 KB, or waits behind another overlay.
//
// So the reader gets a validity check whose failure path is NOT a retry:
//
//     i       = PublishedIndex()            acquire
//     serial  = FrameSerial(buffer[i])      acquire   -- written AFTER pixels
//     copy the pixels out
//     begun   = FrameSerialBegun(buffer[i]) acquire   -- written BEFORE pixels
//     accept the copy iff begun == serial
//
// A rejected copy means "keep the texture you already have", which costs one
// comparison and one stale frame at 2 Hz. Section 8's real requirement was that
// the render thread never LOOPS, and this honours it exactly: the reader still
// makes one attempt and never more.
//
// The two serials are a seqlock used only for detection. That is deliberate --
// a seqlock's cost is its retry, and there is no retry here.

inline std::uint32_t PublishedIndex(const Section& s) noexcept {
    return std::atomic_ref<const std::uint32_t>(s.published_index)
        .load(std::memory_order_acquire);
}

inline void Publish(Section& s, std::uint32_t index) noexcept {
    std::atomic_ref<std::uint32_t>(s.published_index)
        .store(index, std::memory_order_release);
}

// `serial` is stored after the pixels and `serial_begin` before them, each with
// release, and both are read with acquire. That ordering -- not the platform's
// store ordering -- is what makes the check above sound, so it holds on ARM64
// as well as on x86 and does not need a comment about TSO to be believed.
inline std::uint32_t FrameSerial(const Frame& f) noexcept {
    return std::atomic_ref<const std::uint32_t>(f.header.serial)
        .load(std::memory_order_acquire);
}

inline std::uint32_t FrameSerialBegun(const Frame& f) noexcept {
    return std::atomic_ref<const std::uint32_t>(f.header.serial_begin)
        .load(std::memory_order_acquire);
}

// Order matters and is the whole point: Begin, then pixels, then Finish.
inline void BeginFrame(Frame& f, std::uint32_t serial) noexcept {
    std::atomic_ref<std::uint32_t>(f.header.serial_begin)
        .store(serial, std::memory_order_release);
}

inline void FinishFrame(Frame& f, std::uint32_t serial) noexcept {
    std::atomic_ref<std::uint32_t>(f.header.serial)
        .store(serial, std::memory_order_release);
}

// The placement record's guard, as the frame's: Begin, fields, Finish.
inline std::uint32_t PlacementSerial(const Section& s) noexcept {
    return std::atomic_ref<const std::uint32_t>(s.placement.serial)
        .load(std::memory_order_acquire);
}

inline std::uint32_t PlacementSerialBegun(const Section& s) noexcept {
    return std::atomic_ref<const std::uint32_t>(s.placement.serial_begin)
        .load(std::memory_order_acquire);
}

inline void BeginPlacement(Section& s, std::uint32_t serial) noexcept {
    std::atomic_ref<std::uint32_t>(s.placement.serial_begin)
        .store(serial, std::memory_order_release);
}

inline void FinishPlacement(Section& s, std::uint32_t serial) noexcept {
    std::atomic_ref<std::uint32_t>(s.placement.serial)
        .store(serial, std::memory_order_release);
}

inline std::uint64_t Heartbeat(const Section& s) noexcept {
    return std::atomic_ref<const std::uint64_t>(s.heartbeat_us)
        .load(std::memory_order_relaxed);
}

inline void Beat(Section& s, std::uint64_t now_us) noexcept {
    std::atomic_ref<std::uint64_t>(s.heartbeat_us)
        .store(now_us, std::memory_order_relaxed);
}

inline bool Enabled(const Section& s) noexcept {
    return std::atomic_ref<const std::uint32_t>(s.enabled)
        .load(std::memory_order_relaxed) != 0;
}

inline void SetEnabled(Section& s, bool on) noexcept {
    std::atomic_ref<std::uint32_t>(s.enabled)
        .store(on ? 1U : 0U, std::memory_order_relaxed);
}

// ─── the reverse section: what the game needs ──────────────────────────────
//
// Section 8 calls this Local\GpuThermalGuard.OsdReq.v1 and describes it as the
// game reporting the size it wants. T-04 and T-05 turned it from a convenience
// into a requirement, twice over, and both times by measurement:
//
//   * T-04's first run passed every stated check with the dashboard drawn
//     entirely off-screen. The publisher had been told 1920x1080; the
//     generator's swapchain was 320x180. Nothing in the protocol carried the
//     size, so nothing could be wrong about it.
//
//   * T-05's DPI-unaware run found the swapchain at 2194x1234 behind a
//     3840x2160 panel. The overlay lands correctly in the swapchain and is then
//     stretched by DWM with everything else. So the size alone is not enough:
//     the RATIO has to come across too, or the publisher cannot know what
//     physical size its bitmap will end up being.
//
// Written by the game and read by us, which is the opposite direction to
// everything above and the reason it is a separate section: keeping them apart
// is what lets the important one stay READ-ONLY to the game (section 8's
// 「重要的那一段對遊戲是唯讀的」). Everything read out of here is therefore
// untrusted input and must be range-checked, however much we trust the process.
//
// Both sections are created by GpuThermalGuard, as section 8 requires, so there
// is no name for anything else to squat on.
inline constexpr wchar_t kRequestName[] =
    L"Local\\GpuThermalGuard.OsdReq.v1";

inline constexpr std::uint32_t kRequestMagic = 0x52475447;   // "GTGR"
inline constexpr std::uint32_t kRequestVersion = 1;

struct Request {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t writer_pid;       // the GAME, here -- it writes this one

    std::uint64_t heartbeat_us;

    // Bracketing the payload, same shape and same reason as a Frame: written
    // before and after the fields between them, so a reader can tell it read a
    // request that was being rewritten under it. The fields are small enough
    // that a tear is unlikely and that is not the same as impossible.
    std::uint32_t serial_begin;

    // What to rasterise for. The overlay is drawn INTO this, so it is the only
    // number the anchor arithmetic uses, and it is exact.
    std::uint32_t swapchain_width;
    std::uint32_t swapchain_height;

    // The display mode the game's monitor is in, and the swapchain's scaling
    // mode. Together these say whether what is drawn into the swapchain will
    // reach the panel one pixel to one pixel -- and they say it as two facts
    // rather than as a conclusion, deliberately.
    //
    // The first version of this field pair was called panel_width/height and was
    // documented as "what the swapchain is scaled to on screen". That is not
    // something either side can compute from the monitor alone, and T-06 proved
    // it by printing a wrong answer: after ResizeBuffers took a 3840x2160
    // swapchain down to 1920x1080 in a window still covering a 3840x2160 panel,
    // the publisher announced "the frame will be rescaled by 2.0000". With
    // DXGI_SCALING_NONE it is not rescaled at all -- the smaller buffer is
    // presented unstretched and the rest of the window is not its business.
    //
    // So the mapping depends on the scaling mode and on the window's PHYSICAL
    // client rectangle, and the second of those is genuinely awkward to obtain
    // from a DPI-unaware process, which is the case that needs it. The spike does
    // not solve that. It carries the facts it can measure exactly and leaves the
    // conclusion to whoever has the rest of them.
    std::uint32_t display_width;
    std::uint32_t display_height;
    std::uint32_t swapchain_scaling;   // DXGI_SCALING, 0xFFFFFFFF if unknown

    std::uint32_t serial;
};

// DXGI_SCALING_STRETCH = 0, NONE = 1, ASPECT_RATIO_STRETCH = 2. Named here
// rather than pulled in from dxgi1_2.h, because the publisher has no reason to
// include a graphics header to read one integer out of a shared page.
inline constexpr std::uint32_t kScalingStretch = 0;
inline constexpr std::uint32_t kScalingNone = 1;
inline constexpr std::uint32_t kScalingAspectRatioStretch = 2;
inline constexpr std::uint32_t kScalingUnknown = 0xFFFFFFFFU;

inline const char* ScalingName(std::uint32_t scaling) noexcept {
    switch (scaling) {
        case kScalingStretch:             return "stretch";
        case kScalingNone:                return "none";
        case kScalingAspectRatioStretch:  return "aspect-ratio-stretch";
        default:                          return "unknown";
    }
}

inline constexpr std::size_t kRequestBytes = sizeof(Request);

inline std::uint32_t RequestSerial(const Request& r) noexcept {
    return std::atomic_ref<const std::uint32_t>(r.serial)
        .load(std::memory_order_acquire);
}

inline std::uint32_t RequestSerialBegun(const Request& r) noexcept {
    return std::atomic_ref<const std::uint32_t>(r.serial_begin)
        .load(std::memory_order_acquire);
}

inline void BeginRequest(Request& r, std::uint32_t serial) noexcept {
    std::atomic_ref<std::uint32_t>(r.serial_begin)
        .store(serial, std::memory_order_release);
}

inline void FinishRequest(Request& r, std::uint32_t serial) noexcept {
    std::atomic_ref<std::uint32_t>(r.serial)
        .store(serial, std::memory_order_release);
}

inline std::uint64_t RequestHeartbeat(const Request& r) noexcept {
    return std::atomic_ref<const std::uint64_t>(r.heartbeat_us)
        .load(std::memory_order_relaxed);
}

inline void BeatRequest(Request& r, std::uint64_t now_us) noexcept {
    std::atomic_ref<std::uint64_t>(r.heartbeat_us)
        .store(now_us, std::memory_order_relaxed);
}

// The game wrote this. Everything a publisher would act on is checked here, in
// one place, so no call site has to remember to.
inline bool RequestPlausible(const Request& r) noexcept {
    if (r.magic != kRequestMagic || r.version != kRequestVersion) return false;
    if (r.header_size != sizeof(Request)) return false;
    if (r.swapchain_width == 0 || r.swapchain_height == 0) return false;
    if (r.display_width == 0 || r.display_height == 0) return false;
    // 16k on a side. Not a guess about monitors -- a bound past which the
    // numbers are not a resolution, and the publisher must not size a render
    // from them.
    constexpr std::uint32_t kMaxDimension = 16384;
    if (r.swapchain_width > kMaxDimension || r.swapchain_height > kMaxDimension) {
        return false;
    }
    if (r.display_width > kMaxDimension || r.display_height > kMaxDimension) {
        return false;
    }
    return true;
}

// ─── the test pattern ──────────────────────────────────────────────────────
//
// Pixels are a pure function of the serial and the position, so a reader can
// check every byte of what it got against the header it got it with. A torn
// frame -- new header over old pixels, or pixels from two publishes -- fails
// this, and a checksum over a sample of bytes would not reliably notice.
inline std::uint8_t PatternByte(std::uint32_t serial, std::size_t offset) noexcept {
    const std::uint64_t mixed =
        (static_cast<std::uint64_t>(serial) * 2654435761ULL) +
        (static_cast<std::uint64_t>(offset) * 2246822519ULL);
    return static_cast<std::uint8_t>((mixed >> 13) & 0xFFU);
}

}  // namespace gtg::overlay::ipc
