// Spike 2 T-04 — the published bitmap, from the section to a GPU texture.
//
// The probe stops inventing geometry and draws what the publisher published.
// Everything here runs on the game's render thread, so every decision is made
// with that in mind: no lock, no retry, no allocation after the first frame, and
// no syscall on a frame that has nothing to do.
//
// The guard is T-03's, and it is the reason the bytes are staged before they
// reach the texture rather than written into it straight out of the section. If
// the copy turns out to have been torn there must still be a good texture to fall
// back on, and a texture already written cannot be un-written. On D3D11 the stage
// is a CPU buffer (544 KB, about 6 us, T-03). On D3D12 it is the frame slot's own
// upload buffer, which nothing reads until a copy is recorded -- so the stage
// costs no extra copy at all (design §10.1).
//
// Its own header rather than more lines in d3d11_present_probe.cpp: the probe is
// about the hook and the present path, and this is about a transport. They fail
// for different reasons and should be readable apart.
//
// Spike 3 split the GPU half out. Everything that reads the section -- the
// gates, the T-03 guard, the reverse request, the placement -- is the same for
// every graphics API and stays here, once. What differs is how bytes become a
// texture, and that is the Texture parameter: d3d11_texture.hpp maps and
// writes, d3d12_texture.hpp records a copy into the overlay's command list.
// A Texture provides:
//
//   bool Bound() const                     ready to accept an upload
//   bool Has() const                       a texture exists and may be drawn
//   bool Matches(w, h) const               the existing texture has this size
//   bool Stage(pixels, header, stats)      copy the section's bytes somewhere
//                                          private; may be torn, may be dropped
//   bool Commit(header, stats)             the stage was good: create or
//                                          recreate as needed, and fill
//   ImTextureID Id() const
//   void Release()
#pragma once

#include <windows.h>

#include <dxgi1_2.h>   // IDXGISwapChain1::GetDesc1, for DXGI_SCALING

#include <imgui.h>

#include "../ipc/osd_section.hpp"
#include "probe_print.hpp"
#ifdef GTG_REMOTE_RENDER_RESEARCH
#include "../injector/renderer_fixture.hpp"
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gtg::overlay::probe {

namespace ipc = gtg::overlay::ipc;

// Section 8's lifecycle table, as numbers rather than prose.
//
// The retry interval is the one that would be a defect if it were wrong: a probe
// that calls OpenFileMapping every frame is a syscall per frame in a game, and
// section 8 says 每 2 秒重試(不是每幀) for exactly that reason. Getting it
// wrong would still work, which is why it is a named constant with a test.
inline constexpr std::uint64_t kOpenRetryUs = 2'000'000;
inline constexpr std::uint64_t kHeartbeatStaleUs = 3'000'000;

// What the consumer did, so T-04 and T-07 read counters rather than impressions.
// Written only from the render thread; read by exports on another one.
struct ConsumerStats {
    std::atomic<std::uint64_t> open_attempts{0};
    std::atomic<std::uint64_t> frames_with_section{0};
    std::atomic<std::uint64_t> uploads{0};        // serial moved
#ifdef GTG_REMOTE_RENDER_RESEARCH
    std::atomic<std::uint64_t> copy_attempts{0};  // Includes rejected Stage attempts.
#endif
    std::atomic<std::uint64_t> reused{0};         // serial did not: the payoff
    std::atomic<std::uint64_t> rejected{0};       // the T-03 guard fired
    std::atomic<std::uint64_t> stale_heartbeat{0};
    std::atomic<std::uint64_t> disabled_by_flag{0};
    std::atomic<std::uint64_t> bad_header{0};
    std::atomic<std::uint64_t> texture_recreates{0};
    std::atomic<std::uint32_t> last_serial{0};

    // The bitmap did not fit the swapchain it was placed against, so part or all
    // of it falls outside the frame.
    //
    // This is not a defensive counter -- T-04 hit it on the first run. The
    // publisher was told 1920x1080 and the generator's swapchain was 320x180, so
    // a top-right anchor computed left = 320 - 388 - 24 = -92 and the dashboard
    // was drawn entirely off the left edge. Every stated check still passed:
    // presents, hook hits, frames rendered and vertices all agreed, and the
    // device was healthy. Nothing in the protocol had told the publisher what
    // size to draw for.
    //
    // Which is the finding. Section 8's reverse section (OsdReq.v1) is not a
    // convenience for letting the game ask for a size; it is the only thing that
    // stops the publisher rendering for a resolution that does not exist. Until
    // it is built, this counter is how that shows up as a number instead of as
    // an absence.
    std::atomic<std::uint64_t> geometry_outside{0};

    // T-06: how many times the swapchain's size changed and we said so. One on a
    // steady run; more means the resolution moved and the publisher was told.
    std::atomic<std::uint64_t> requests_written{0};

    // What an upload costs the render thread, from the first byte copied out
    // of the section to the texture being ready to draw -- the CPU side only.
    // Measured because "it is small" was an estimate, and the owner asked.
    std::atomic<std::uint64_t> upload_ns_total{0};
    std::atomic<std::uint64_t> upload_ns_max{0};
    // The same, for uploads that created nothing -- no texture, no upload
    // buffer. The average above is dominated by the few that did, which hides
    // what a steady-state upload costs. Textures count their allocations here.
    std::atomic<std::uint64_t> allocations{0};
    std::atomic<std::uint64_t> upload_ns_steady_total{0};
    std::atomic<std::uint64_t> uploads_steady{0};

    // Spike 4: placement records taken, refused, and how long each took from
    // being written to being seen by a frame.
    std::atomic<std::uint64_t> placements_applied{0};
    std::atomic<std::uint64_t> placements_rejected{0};
    // Frames not drawn because the published image was made for another frame
    // size -- the moment before the reverse request reaches the writer.
    std::atomic<std::uint64_t> awaiting_frame_size{0};
    std::atomic<std::uint64_t> placement_latency_us_total{0};
    std::atomic<std::uint64_t> placement_latency_us_max{0};
};

inline std::uint64_t QpcNs() {
    static const std::uint64_t freq = [] {
        LARGE_INTEGER f{};
        ::QueryPerformanceFrequency(&f);
        return static_cast<std::uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER n{};
    ::QueryPerformanceCounter(&n);
    return static_cast<std::uint64_t>(n.QuadPart) * 1'000'000'000ULL / freq;
}

// Where the bitmap ended up, for the T-05 checker to compare against. Four
// separate atomics rather than a struct: a torn read of a rectangle would make
// the checker report a position that never existed on the screen.
struct DestRect {
    std::atomic<std::int32_t> left{-1};
    std::atomic<std::int32_t> top{-1};
    std::atomic<std::int32_t> width{0};
    std::atomic<std::int32_t> height{0};
};

// T-07 makes section 8's lifecycle table a measurement, and a total cannot say
// WHEN something happened. Each of these is announced once, with the number that
// matters, the first time the consumer enters it.
enum class ConsumerState : int {
    // Distinct from NoSection on purpose: without it the first entry into
    // NoSection is a no-op against the initial value and case A -- publisher
    // never started -- announces nothing at all.
    Start = -1,
    NoSection = 0,     // nothing to open, retrying on the timer
    Drawing,
    StaleHeartbeat,    // the publisher stopped beating
    DisabledByFlag,    // the desktop turned the overlay off
    BadHeader,
};

inline const char* ConsumerStateName(ConsumerState s) {
    switch (s) {
        case ConsumerState::Start:
            return "start";
        case ConsumerState::NoSection:
            return "no-section";
        case ConsumerState::Drawing:
            return "drawing";
        case ConsumerState::StaleHeartbeat:
            return "stale-heartbeat";
        case ConsumerState::DisabledByFlag:
            return "disabled-by-flag";
        case ConsumerState::BadHeader:
            return "bad-header";
    }
    return "?";
}

// ─── L0 (design §10.1): the facts about the target, cached ─────────────────
//
// The display mode and the swapchain's scaling mode go into the reverse request
// (OsdReq.v1). Both change almost never, and asking for the display mode costs a
// syscall -- EnumDisplaySettingsW measured 22-30 us at p50, every frame, the
// single largest avoidable item on the D3D12 path.
//
// Refreshed when the swapchain object or its size changes -- the moments a
// display-mode change shows up as -- and otherwise at most once a second, so a
// mode change with no size change is still noticed, a second late. Facts are
// compared, not events awaited: we own none of the events.
struct TargetFacts {
    std::uint32_t display_width = 0;
    std::uint32_t display_height = 0;
    std::uint32_t scaling = ipc::kScalingUnknown;
};

class TargetFactsCache {
public:
    static constexpr std::uint64_t kRefreshUs = 1'000'000;

    const TargetFacts& Get(IDXGISwapChain* swap_chain, std::uint32_t swap_w, std::uint32_t swap_h,
                           std::uint64_t now_us) {
        if (valid_ && swap_chain == swap_chain_ && swap_w == swap_w_ && swap_h == swap_h_ &&
            now_us < next_us_) {
            return facts_;
        }
        swap_chain_ = swap_chain;
        swap_w_ = swap_w;
        swap_h_ = swap_h;
        next_us_ = now_us + kRefreshUs;
        valid_ = true;
        ++refreshes_;

        // EnumDisplaySettings, not GetSystemMetrics and not MONITORINFO: both of
        // those are virtualised for a DPI-unaware process and would report the
        // scaled number the swapchain was already created at. See Spike 2 T-06.
        facts_.display_width = swap_w;
        facts_.display_height = swap_h;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (::EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth > 0 &&
            mode.dmPelsHeight > 0) {
            facts_.display_width = mode.dmPelsWidth;
            facts_.display_height = mode.dmPelsHeight;
        }
        // The scaling mode lives on IDXGISwapChain1, not on the legacy desc.
        facts_.scaling = ipc::kScalingUnknown;
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(swap_chain->QueryInterface(__uuidof(IDXGISwapChain1),
                                                 reinterpret_cast<void**>(&sc1))) &&
            sc1 != nullptr) {
            DXGI_SWAP_CHAIN_DESC1 d1{};
            if (SUCCEEDED(sc1->GetDesc1(&d1))) {
                facts_.scaling = static_cast<std::uint32_t>(d1.Scaling);
            }
            sc1->Release();
        }
        return facts_;
    }

    std::uint64_t refreshes() const { return refreshes_; }

private:
    TargetFacts facts_{};
    IDXGISwapChain* swap_chain_ = nullptr;
    std::uint32_t swap_w_ = 0;
    std::uint32_t swap_h_ = 0;
    std::uint64_t next_us_ = 0;
    bool valid_ = false;
    std::uint64_t refreshes_ = 0;
};

template <class Texture>
class PublishedBitmapT {
public:
    Texture& texture() { return texture_; }

    ConsumerStats& stats() { return stats_; }

    const DestRect& dest() const { return dest_; }
#ifdef GTG_REMOTE_RENDER_RESEARCH
    // Same-render-thread diagnostic; not a cross-thread/public IPC interface.
    std::uint32_t ResearchPlacementSerial() const { return placement_serial_; }
#endif
    ConsumerState state() const { return state_; }

    std::uint64_t transitions() const { return transitions_; }

    // Printed on a cadence the CALLER chooses, from the present path rather than
    // the draw path, so a run in which nothing is ever drawn still reports.
    void PrintSummary(std::uint64_t present_count) const {
        std::int32_t l = dest_.left.load(std::memory_order_relaxed);
        std::int32_t t = dest_.top.load(std::memory_order_relaxed);
        std::int32_t w = dest_.width.load(std::memory_order_relaxed);
        std::int32_t h = dest_.height.load(std::memory_order_relaxed);
        Print(
            "[T-04] present %llu, state %s | serial=%u uploads=%llu "
            "reused=%llu rejected=%llu outside=%llu | opens=%llu "
            "stale=%llu disabled=%llu bad=%llu requests=%llu | "
            "rect=%d,%d %dx%d\n",
            static_cast<unsigned long long>(present_count), ConsumerStateName(state_),
            stats_.last_serial.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(stats_.uploads.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(stats_.reused.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(stats_.rejected.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                stats_.geometry_outside.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(stats_.open_attempts.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(stats_.stale_heartbeat.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                stats_.disabled_by_flag.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(stats_.bad_header.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                stats_.requests_written.load(std::memory_order_relaxed)),
            l, t, w, h);
        std::fflush(stdout);
    }

    // True when there is a texture worth drawing, with the destination
    // rectangle computed for the swapchain it was given.
    bool Acquire(std::uint32_t target_w, std::uint32_t target_h, std::uint64_t now_us) {
        if (!texture_.Bound()) return false;
        ++frames_seen_;
        if (!EnsureOpen(now_us)) {
            Enter(ConsumerState::NoSection, 0);
            return false;
        }
        stats_.frames_with_section.fetch_add(1, std::memory_order_relaxed);

        // Section 8's gates, in the order that costs least to fail.
        if (!ipc::Enabled(*section_)) {
            stats_.disabled_by_flag.fetch_add(1, std::memory_order_relaxed);
            // The desktop cleared the flag. Drawing stops on THIS frame, which is
            // the next frame after the write -- section 8's
            // 桌面改了遊戲下一幀生效 -- because the check is the first thing the
            // consumer does and there is nothing to unwind.
            Enter(ConsumerState::DisabledByFlag, 0);
            return false;
        }
        const std::uint64_t beat = ipc::Heartbeat(*section_);
        if (now_us > beat && now_us - beat > kHeartbeatStaleUs) {
            // No frozen last frame. Section 8 is explicit: a dead publisher must
            // not leave its last bitmap sitting on the screen. The age reported
            // here IS the latency -- time since the publisher's last beat --
            // which is what "數秒後停止繪製" has to be measured against.
            stats_.stale_heartbeat.fetch_add(1, std::memory_order_relaxed);
            Enter(ConsumerState::StaleHeartbeat, now_us - beat);
            return false;
        }

        const std::uint32_t i = ipc::PublishedIndex(*section_);
        if (i > 1) {
            stats_.bad_header.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const ipc::Frame& f = section_->buffer[i];
        const std::uint32_t serial = ipc::FrameSerial(f);
        const ipc::FrameHeader h = f.header;
        if (!HeaderPlausible(h)) {
            stats_.bad_header.fetch_add(1, std::memory_order_relaxed);
            Enter(ConsumerState::BadHeader, 0);
            return false;
        }
        Enter(ConsumerState::Drawing, 0);
        ReadPlacement(now_us);

        // Spike 4, rule 4: an image sized and placed for another frame size is
        // not drawn -- not uploaded either. It is the writer's guess before our
        // request reached it, and it would be drawn partly outside this frame.
        if (h.frame_width != 0 && (h.frame_width != target_w || h.frame_height != target_h)) {
            stats_.awaiting_frame_size.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // The payoff for `serial` existing: an unchanged image is not copied,
        // not uploaded, and not even read.
        if (serial != 0 && serial == uploaded_serial_ && texture_.Has() &&
            texture_.Matches(h.width, h.height)) {
            stats_.reused.fetch_add(1, std::memory_order_relaxed);
            PlaceInto(h, target_w, target_h);
            return true;
        }

        // Copy out of the section into wherever the Texture keeps bytes that
        // nothing reads yet -- D3D11 a CPU staging buffer, D3D12 the slot's own
        // upload buffer -- then check, then commit. The copy may be torn; the
        // point of staging is that a torn copy can still be thrown away.
        const std::uint64_t upload_start = QpcNs();
        const std::uint64_t allocations_before = stats_.allocations.load(std::memory_order_relaxed);
#ifdef GTG_REMOTE_RENDER_RESEARCH
        stats_.copy_attempts.fetch_add(1, std::memory_order_relaxed);
#endif
        if (!texture_.Stage(f.pixels, h, stats_)) return false;

        // T-03's guard. A mismatch means the writer began refilling this buffer
        // under the copy; keep whatever texture is already uploaded.
        if (ipc::FrameSerialBegun(f) != serial) {
            stats_.rejected.fetch_add(1, std::memory_order_relaxed);
            if (!texture_.Has()) return false;
            PlaceInto(last_header_, target_w, target_h);
            return true;
        }

        if (!texture_.Commit(h, stats_)) return false;
        const std::uint64_t upload_ns = QpcNs() - upload_start;
        stats_.upload_ns_total.fetch_add(upload_ns, std::memory_order_relaxed);
        if (upload_ns > stats_.upload_ns_max.load(std::memory_order_relaxed)) {
            stats_.upload_ns_max.store(upload_ns, std::memory_order_relaxed);
        }
        if (stats_.allocations.load(std::memory_order_relaxed) == allocations_before) {
            stats_.upload_ns_steady_total.fetch_add(upload_ns, std::memory_order_relaxed);
            stats_.uploads_steady.fetch_add(1, std::memory_order_relaxed);
        }
        uploaded_serial_ = serial;
        last_header_ = h;
        stats_.uploads.fetch_add(1, std::memory_order_relaxed);
        stats_.last_serial.store(serial, std::memory_order_relaxed);
        PlaceInto(h, target_w, target_h);
        return true;
    }

    ImTextureID TextureId() const { return texture_.Id(); }

    bool HasTexture() const { return texture_.Has(); }

    std::uint32_t AlphaMode() const { return last_header_.alpha_mode; }

    float DestLeft() const { return dest_left_; }

    float DestTop() const { return dest_top_; }

    float DestRight() const { return dest_left_ + dest_w_; }

    float DestBottom() const { return dest_top_ + dest_h_; }

    // ─── T-06: telling the publisher what this swapchain is ────────────────
    //
    // Called every frame and does nothing on almost all of them. The request is
    // written only when the answer changes, because the game's render thread is
    // the wrong place to write 40 bytes sixty times a second for no reason.
    //
    // `display` is the monitor's current mode and `scaling` the swapchain's
    // scaling mode. Two facts, not a conclusion: see the note on Request in
    // osd_section.hpp for why the conclusion was removed after T-06 printed a
    // wrong one.
    void ReportTarget(std::uint32_t swap_w, std::uint32_t swap_h, std::uint32_t disp_w,
                      std::uint32_t disp_h, std::uint32_t scaling, std::uint64_t now_us) {
        if (swap_w == req_swap_w_ && swap_h == req_swap_h_ && disp_w == req_disp_w_ &&
            disp_h == req_disp_h_ && scaling == req_scaling_) {
            // Unchanged. Keep the heartbeat moving so the publisher can tell a
            // steady game from a departed one, and do nothing else.
            if (request_ != nullptr && (++beat_tick_ % 60) == 0) {
                ipc::BeatRequest(*request_, now_us);
            }
            return;
        }
        if (!EnsureRequestOpen(now_us)) return;

        req_swap_w_ = swap_w;
        req_swap_h_ = swap_h;
        req_disp_w_ = disp_w;
        req_disp_h_ = disp_h;
        req_scaling_ = scaling;
        ++request_serial_;

        ipc::BeginRequest(*request_, request_serial_);
        request_->swapchain_width = swap_w;
        request_->swapchain_height = swap_h;
        request_->display_width = disp_w;
        request_->display_height = disp_h;
        request_->swapchain_scaling = scaling;
        request_->writer_pid = ::GetCurrentProcessId();
        ipc::BeatRequest(*request_, now_us);
        ipc::FinishRequest(*request_, request_serial_);

        stats_.requests_written.fetch_add(1, std::memory_order_relaxed);
        Print(
            "[T-06] request %u: swapchain %ux%u, display mode %ux%u, "
            "scaling %s\n",
            request_serial_, swap_w, swap_h, disp_w, disp_h, ipc::ScalingName(scaling));
    }

    void Release() {
        texture_.Release();
        if (section_ != nullptr) {
            ::UnmapViewOfFile(const_cast<ipc::Section*>(section_));
            section_ = nullptr;
        }
        if (mapping_ != nullptr) {
            ::CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        if (request_ != nullptr) {
            ::UnmapViewOfFile(request_);
            request_ = nullptr;
        }
        if (request_mapping_ != nullptr) {
            ::CloseHandle(request_mapping_);
            request_mapping_ = nullptr;
        }
    }

private:
    // Announced once per entry, never per frame. A state that persists says so
    // with one line and then stays quiet, which is what makes a game's log
    // readable at 60 Hz.
    void Enter(ConsumerState next, std::uint64_t detail_us) {
        if (next == state_) return;
        const ConsumerState from = state_;
        state_ = next;
        ++transitions_;
        if (next == ConsumerState::StaleHeartbeat) {
            Print(
                "[T-07] %s -> %s at frame %llu: last beat %llu us ago "
                "(threshold %llu) -- drawing stopped, no frozen frame\n",
                ConsumerStateName(from), ConsumerStateName(next),
                static_cast<unsigned long long>(frames_seen_),
                static_cast<unsigned long long>(detail_us),
                static_cast<unsigned long long>(kHeartbeatStaleUs));
        } else {
            Print("[T-07] %s -> %s at frame %llu\n", ConsumerStateName(from),
                  ConsumerStateName(next), static_cast<unsigned long long>(frames_seen_));
        }
        std::fflush(stdout);
    }

    // Another process wrote this, so nothing below may use it unchecked -- even
    // though that process is the one we trust. Section 8 asks for the range
    // check on the reverse section and the same reasoning applies here.
    static bool HeaderPlausible(const ipc::FrameHeader& h) {
        if constexpr (requires { Texture::SupportsHeader(h); }) {
            if (!Texture::SupportsHeader(h)) return false;
        }
        if (h.width == 0 || h.height == 0) return false;
        if (h.width > ipc::kMaxWidth || h.height > ipc::kMaxHeight) return false;
        if (h.stride < h.width * 4u) return false;
        if (h.byte_count == 0 || h.byte_count > ipc::kMaxPixelBytes) return false;
        if (static_cast<std::uint64_t>(h.stride) * h.height != h.byte_count) {
            return false;
        }
        return true;
    }

    bool EnsureOpen(std::uint64_t now_us) {
        if (section_ != nullptr) return true;
        // Not every frame. This is the line section 8's lifecycle table asks
        // for, and the one a spike would most easily get wrong without noticing,
        // because getting it wrong still works.
        if (next_open_us_ != 0 && now_us < next_open_us_) return false;
        next_open_us_ = now_us + kOpenRetryUs;
        stats_.open_attempts.fetch_add(1, std::memory_order_relaxed);

#ifdef GTG_REMOTE_RENDER_RESEARCH
        wchar_t name[96]{};
        gtg::research::BitmapSectionName(GetCurrentProcessId(), name);
        HANDLE m = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name);
#else
        HANDLE m = ::OpenFileMappingW(FILE_MAP_READ, FALSE, ipc::kSectionName);
#endif
        if (m == nullptr) return false;
        void* view = ::MapViewOfFile(m, FILE_MAP_READ, 0, 0, ipc::kSectionBytes);
        if (view == nullptr) {
            ::CloseHandle(m);
            return false;
        }
        const auto* candidate = static_cast<const ipc::Section*>(view);
        if (candidate->magic != ipc::kMagic || candidate->version != ipc::kVersion ||
            candidate->header_size != sizeof(ipc::Section)) {
            // A publisher we do not understand is not a publisher. Refuse, and
            // keep refusing on the same 2 s timer, rather than draw its bytes.
            Print(
                "[T-04] section refused: magic=0x%08X version=%u "
                "header_size=%u\n",
                candidate->magic, candidate->version, candidate->header_size);
            ::UnmapViewOfFile(view);
            ::CloseHandle(m);
            return false;
        }
        mapping_ = m;
        section_ = candidate;
        opened_us_ = now_us;
        Print("[T-04] section opened: writer_pid=%u, %zu bytes\n", candidate->writer_pid,
              ipc::kSectionBytes);
        return true;
    }

    // Opened for WRITE: this is the one section the game is allowed to write,
    // and the reason section 8 has two of them. Same 2 s timer as the other, and
    // the same refusal to touch a section whose header we do not recognise.
    bool EnsureRequestOpen(std::uint64_t now_us) {
#if defined(GTG_REMOTE_RENDER_RESEARCH) && !defined(GTG_TARGET_OSD) && \
    !defined(GTG_PID_BITMAP_REQUEST)
        (void)now_us;
        return false;  // Never write the real desktop publisher's request section.
#else
        if (request_ != nullptr) return true;
        if (next_request_us_ != 0 && now_us < next_request_us_) return false;
        next_request_us_ = now_us + kOpenRetryUs;

#if defined(GTG_TARGET_OSD) || defined(GTG_PID_BITMAP_REQUEST)
        wchar_t name[96]{};
        gtg::research::BitmapRequestName(GetCurrentProcessId(), name);
        HANDLE m = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, name);
#else
        HANDLE m = ::OpenFileMappingW(FILE_MAP_WRITE, FALSE, ipc::kRequestName);
#endif
        if (m == nullptr) return false;
        void* view = ::MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, ipc::kRequestBytes);
        if (view == nullptr) {
            ::CloseHandle(m);
            return false;
        }
        auto* candidate = static_cast<ipc::Request*>(view);
        if (candidate->magic != ipc::kRequestMagic || candidate->version != ipc::kRequestVersion ||
            candidate->header_size != sizeof(ipc::Request)) {
            Print(
                "[T-06] request section refused: magic=0x%08X "
                "version=%u header_size=%u\n",
                candidate->magic, candidate->version, candidate->header_size);
            ::UnmapViewOfFile(view);
            ::CloseHandle(m);
            return false;
        }
        request_mapping_ = m;
        request_ = candidate;
        request_serial_ = ipc::RequestSerial(*candidate);
        Print("[T-06] request section opened, %zu bytes\n", ipc::kRequestBytes);
        return true;
#endif
    }

    // Anchor plus offset, against the swapchain's own pixels.
    //
    // The offsets are named `dip` in section 8 and are treated here as device
    // pixels, because the publisher rasterises at a scale it chose and the probe
    // has no DPI for a swapchain it does not own. That is a real gap rather than
    // a simplification: T-05 is where it gets settled, and it is written into
    // the evidence rather than left implied by this code.
    void PlaceInto(const ipc::FrameHeader& h, std::uint32_t target_w, std::uint32_t target_h) {
        const float w = static_cast<float>(h.width);
        const float ht = static_cast<float>(h.height);
        const float ox = static_cast<float>(h.offset_dip_x);
        const float oy = static_cast<float>(h.offset_dip_y);
        const float tw = static_cast<float>(target_w);
        const float th = static_cast<float>(target_h);
        // Spike 4: the placement record wins when it was computed for this
        // frame size and this bitmap's size. Otherwise -- no record yet, or one
        // computed for the next bitmap -- the header's placement, which always
        // matches the bitmap it came with.
        if (placement_valid_ && placement_.width == static_cast<std::int32_t>(h.width) &&
            placement_.height == static_cast<std::int32_t>(h.height) &&
            placement_.frame_width == target_w && placement_.frame_height == target_h) {
            dest_left_ = static_cast<float>(placement_.left);
            dest_top_ = static_cast<float>(placement_.top);
            dest_w_ = w;
            dest_h_ = ht;
            StoreDest();
            return;
        }
        switch (static_cast<ipc::AnchorCorner>(h.anchor_corner)) {
            case ipc::AnchorCorner::TopLeft:
                dest_left_ = ox;
                dest_top_ = oy;
                break;
            case ipc::AnchorCorner::TopRight:
                dest_left_ = tw - w - ox;
                dest_top_ = oy;
                break;
            case ipc::AnchorCorner::BottomLeft:
                dest_left_ = ox;
                dest_top_ = th - ht - oy;
                break;
            case ipc::AnchorCorner::BottomRight:
            default:
                dest_left_ = tw - w - ox;
                dest_top_ = th - ht - oy;
                break;
        }
        dest_w_ = w;
        dest_h_ = ht;

        // Reported, not corrected. Clamping to the nearest edge would put the
        // dashboard somewhere plausible and make a wrong anchor indistinguishable
        // from a right one, which is exactly what T-05 has to be able to tell
        // apart. So the rectangle stays where the arithmetic put it, the frame
        // clips it, and the discrepancy is counted and named once.
        if (dest_left_ < 0.0f || dest_top_ < 0.0f || dest_left_ + w > tw || dest_top_ + ht > th) {
            const std::uint64_t n = stats_.geometry_outside.fetch_add(1, std::memory_order_relaxed);
            if (n == 0) {
                Print(
                    "[T-04] GEOMETRY OUTSIDE FRAME: bitmap %.0fx%.0f at "
                    "%.0f,%.0f does not fit swapchain %.0fx%.0f -- the "
                    "publisher was not told this resolution (section 8 "
                    "reverse section, OsdReq.v1, is not built)\n",
                    w, ht, dest_left_, dest_top_, tw, th);
            }
        }

        StoreDest();
    }

    void StoreDest() {
        dest_.left.store(static_cast<std::int32_t>(dest_left_), std::memory_order_relaxed);
        dest_.top.store(static_cast<std::int32_t>(dest_top_), std::memory_order_relaxed);
        dest_.width.store(static_cast<std::int32_t>(dest_w_), std::memory_order_relaxed);
        dest_.height.store(static_cast<std::int32_t>(dest_h_), std::memory_order_relaxed);
    }

    // Spike 4: one acquire load per frame. A new serial is copied under the
    // begin/end guard and range-checked; a torn or implausible record is
    // counted and the previous one kept -- no retry, as with frames.
    void ReadPlacement(std::uint64_t now_us) {
        const std::uint32_t serial = ipc::PlacementSerial(*section_);
        if (serial == 0 || serial == placement_serial_) return;
        const ipc::Placement copy = section_->placement;
        if (ipc::PlacementSerialBegun(*section_) != serial || copy.width <= 0 || copy.height <= 0 ||
            copy.width > static_cast<std::int32_t>(ipc::kMaxWidth) ||
            copy.height > static_cast<std::int32_t>(ipc::kMaxHeight) || copy.frame_width == 0 ||
            copy.frame_height == 0) {
            stats_.placements_rejected.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        placement_ = copy;
        placement_serial_ = serial;
        placement_valid_ = true;
        stats_.placements_applied.fetch_add(1, std::memory_order_relaxed);
        // Writer and reader both count QPC microseconds; the difference is the
        // time from the write to the first frame that saw it. Only for records
        // written while this reader was attached: one written before the section
        // was opened measures the wait for the reader, not the reader.
        if (now_us > copy.written_us && copy.written_us >= opened_us_) {
            const std::uint64_t latency = now_us - copy.written_us;
            stats_.placement_latency_us_total.fetch_add(latency, std::memory_order_relaxed);
            if (latency > stats_.placement_latency_us_max.load(std::memory_order_relaxed)) {
                stats_.placement_latency_us_max.store(latency, std::memory_order_relaxed);
            }
        }
    }

    ipc::Placement placement_{};
    std::uint32_t placement_serial_ = 0;
    bool placement_valid_ = false;
    std::uint64_t opened_us_ = 0;

    Texture texture_{};

    HANDLE mapping_ = nullptr;
    const ipc::Section* section_ = nullptr;
    std::uint64_t next_open_us_ = 0;

    // The reverse section. Write access, because this is the half the game owns.
    HANDLE request_mapping_ = nullptr;
    ipc::Request* request_ = nullptr;
    std::uint64_t next_request_us_ = 0;
    std::uint32_t request_serial_ = 0;
    std::uint32_t req_swap_w_ = 0;
    std::uint32_t req_swap_h_ = 0;
    std::uint32_t req_disp_w_ = 0;
    std::uint32_t req_disp_h_ = 0;
    std::uint32_t req_scaling_ = ipc::kScalingUnknown;
    std::uint32_t beat_tick_ = 0;

    ConsumerState state_ = ConsumerState::Start;
    std::uint64_t frames_seen_ = 0;
    std::uint64_t transitions_ = 0;

    std::uint32_t uploaded_serial_ = 0;
    ipc::FrameHeader last_header_{};

    ConsumerStats stats_{};
    DestRect dest_{};

    float dest_left_ = 0.0f;
    float dest_top_ = 0.0f;
    float dest_w_ = 0.0f;
    float dest_h_ = 0.0f;
};

}  // namespace gtg::overlay::probe
