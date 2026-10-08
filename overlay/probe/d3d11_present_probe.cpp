// Spike 1 T-03 + T-04 — the Direct3D 11 present hook, and ImGui drawing on a
// device we do not own.
//
// T-03: an earlier research prototype proved the hook shape already, on WARP, in a
// hidden window, for one frame, counting calls and drawing nothing. This
// reproduces it in our own tree against a hardware swapchain presenting at
// rate, which is the part that was never covered.
//
// T-04: initialise ImGui's Direct3D 11 backend with the device obtained from
// that swapchain and render a frame through it. The question is not whether
// ImGui works -- it obviously does -- but whether it works on a device, context
// and back buffer that belong to somebody else, without disturbing them.
//
// Deliberately NOT the production shape. ReShade and Special K both wrap the
// swapchain in a proxy object rather than patching its vtable, because two
// inline patches on one slot fight while two wrappers stack -- and a player's
// machine may already have Steam or Discord in that slot. Wrapping requires
// intercepting creation, which requires the proxy DLL, which is not this
// spike. See docs/game-overlay-design-20260927.md section 9.1.
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>   // IDXGISwapChain1::GetDesc1, for DXGI_SCALING
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <splice/splice.h>

#include "d3d11_texture.hpp"
#include "published_bitmap.hpp"
#include "frame_cost.hpp"
#include "probe_print.hpp"
#include "../integration/sdr_target_format.hpp"
#ifdef GTG_DIRECT_DRAW
#include "../injector/draw_diagnostics.hpp"
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

namespace probe = gtg::overlay::probe;
#ifdef GTG_DIRECT_DRAW
gtg::research::DrawDiagnostics* g_draw_diagnostics{};
#endif

using PresentFunction = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

// IUnknown (3) + IDXGIObject (4) + IDXGIDeviceSubObject::GetDevice (1) = 8.
// An ABI assumption that holds for this controlled test; production selection
// must handle swapchain versions and ownership on its own.
constexpr std::size_t kPresentVtableSlot = 8;

std::atomic<void*> g_present_target{};

// The entry outlives Attach because the macro declares it static. Keeping a
// pointer is what lets HitCount ask splice how many times the hook was
// entered instead of maintaining a counter of its own -- see .observe().
using PresentEntry = splice::InterceptorEntry<PresentFunction>;
std::atomic<PresentEntry*> g_entry{};

// ─── T-05: the object a screen capture has to find ─────────────────────────
//
// Acceptance criterion 1 of this spike is "pixels reach the screen", and that
// is not satisfiable by a return code -- T-04 already returns success while
// proving only that the calls did not fail. So one rectangle, at coordinates
// and in a colour chosen to be checkable by a machine rather than by an
// impression:
//
//   * a fixed top-left position, so the checker knows where to look without
//     searching, and a wrong position fails rather than passes by luck
//   * fully opaque, so the blend leaves the source colour untouched
//   * a colour nothing else on the screen uses -- not the generator's dark blue
//     clear (roughly 10..36, 20, 36), not the panel above, not any Windows
//     chrome
//
// The swapchain is DXGI_FORMAT_B8G8R8A8_UNORM, which is NOT sRGB, so no gamma
// conversion sits between this constant and the back buffer. The captured
// pixel should equal it exactly; "close enough" would mean something is
// converting, and that is worth failing over.
constexpr int kQuadLeft = 32;
constexpr int kQuadTop = 32;
constexpr int kQuadRight = 232;
constexpr int kQuadBottom = 132;
constexpr int kQuadR = 255;
constexpr int kQuadG = 0;
constexpr int kQuadB = 170;

// ─── T-04 state ────────────────────────────────────────────────────────────
//
// All of it global because the hook callback must be capture-less, and all of
// it touched only from whichever thread calls Present. A game that presents
// from two threads would need more than this; g_overlay_busy is here so that
// case degrades to "draws nothing" instead of to "corrupts ImGui".

enum class OverlayState : int {
    Untouched = 0,       // no attempt yet
    Ready,               // ImGui initialised, drawing
    DeviceUnavailable,   // GetDevice or GetImmediateContext failed
    BackendInitFailed,   // ImGui_ImplDX11_Init returned false
    DeviceRemoved,       // the device died; we stopped drawing
    // Off on purpose, which is NOT the same as Untouched. Untouched means the
    // overlay was expected to start and never did -- a failure. Disabled means
    // the gate is closed and nothing was ever meant to happen, which is the
    // shipping behaviour design section 9.5 requires for a game we decline.
    // Collapsing the two would make a real failure indistinguishable from
    // working as asked.
    Disabled,
    // Initialised, gate open, and nothing to draw because nothing is published.
    // Distinct from Ready for the same reason Disabled is distinct from
    // Untouched: this is the state T-08 requires, and a host that could not tell
    // it from Ready would have to treat the safety floor as a fault. See
    // published_bitmap.hpp for which conditions put us here.
    Idle,
};

// ─── The gate (T-06, and a shipping requirement) ───────────────────────────
//
// Design section 9.5 makes "turn the overlay off" load-bearing rather than
// convenient: when a game cannot be supported, declining it IS the handling.
// So the off switch has to be a real thing that can be measured, not a claim.
//
// This is that switch in its simplest form -- one relaxed atomic load per
// Present -- and it is deliberately a gate rather than an unhook. Splice can
// now disable an inline patch (the prologue really is restored), but a gate
// costs one load and never rewrites instructions underneath a running render
// thread, and the trampoline could not be reclaimed either way. Section 11.
//
// Read once from the environment at attach, because a spike has no IPC channel
// yet; the shipped version reads the shared `enabled` flag instead. What the
// mechanism is fed does not change what it costs.
std::atomic<bool> g_draw_enabled{true};

std::atomic<OverlayState> g_overlay_state{OverlayState::Untouched};
std::atomic<bool> g_overlay_attempted{false};
std::atomic<bool> g_overlay_busy{false};

std::atomic<std::uint32_t> g_frames_rendered{0};
std::atomic<std::uint32_t> g_frames_skipped{0};
// Every intercepted Present, drawn or not. The consumer's summary is printed on
// this cadence rather than on the rendered-frame one, so a run where nothing is
// ever drawn -- T-07's "publisher never started" -- still reports its counters.
std::atomic<std::uint64_t> g_presents_seen{0};
std::atomic<int> g_last_draw_lists{0};
std::atomic<int> g_last_vertices{0};

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
LARGE_INTEGER g_qpc_freq{};
LARGE_INTEGER g_qpc_last{};

// ─── Spike 2 T-04: what is actually drawn ──────────────────────────────────
//
// The published dashboard, read from the section and uploaded as a texture. See
// published_bitmap.hpp -- the transport lives there because it fails for
// entirely different reasons than the hook does.
probe::PublishedBitmapT<probe::D3D11Texture> g_published{};
#ifdef GTG_REMOTE_RENDER_RESEARCH
bool g_remote_visible = false;
#endif
probe::TargetFactsCache g_target_facts;

// L2's key: everything the draw data depends on (design §10.1).
struct DrawKey {
    ImTextureID texture{};
    float left = 0, top = 0, right = 0, bottom = 0;
    UINT width = 0, height = 0;
    bool drawing = false;
    bool operator==(const DrawKey&) const = default;
};

DrawKey g_draw_key;
bool g_draw_key_valid = false;
std::atomic<std::uint64_t> g_draw_rebuilds{0};

// Spike 1's synthetic quad, kept but no longer the default.
//
// With a publisher present it would draw on top of the thing under test; with
// none present it would break T-08, which requires a game with no publisher to
// be indistinguishable from a game with no probe at all. So it is off unless
// GTG_OVERLAY_SYNTHETIC=1 asks for it, which is how Spike 1's T-05 capture
// stays reproducible.
std::atomic<bool> g_draw_synthetic{false};

std::uint64_t NowUs() {
    LARGE_INTEGER n{};
    ::QueryPerformanceCounter(&n);
    return static_cast<std::uint64_t>(n.QuadPart) * 1'000'000ULL /
           static_cast<std::uint64_t>(g_qpc_freq.QuadPart);
}

// Initialise once, on the render thread, inside the first intercepted Present.
//
// This is the earliest point at which a device certainly exists, which is the
// whole reason the design puts it here rather than in DllMain -- and DllMain
// could not do it anyway, under loader lock. See design section 9.3b.
bool InitialiseOverlay(IDXGISwapChain* swap_chain) {
    if (FAILED(
            swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) ||
        g_device == nullptr) {
        probe::Print("[T-04] GetDevice failed -- not a D3D11 swapchain?\n");
        g_overlay_state.store(OverlayState::DeviceUnavailable);
        return false;
    }
    g_device->GetImmediateContext(&g_context);
    if (g_context == nullptr) {
        probe::Print("[T-04] GetImmediateContext returned null\n");
        g_device->Release();
        g_device = nullptr;
        g_overlay_state.store(OverlayState::DeviceUnavailable);
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // Never write into the game's directory. ImGui defaults to creating
    // imgui.ini next to the executable, and an overlay that litters a player's
    // game folder is a shipping defect, not a detail -- so this is set here in
    // the spike to establish the habit.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // No platform backend: there is no window we own and no input to read, so
    // DisplaySize and DeltaTime are supplied by hand every frame. Naming the
    // backend anyway makes the absence deliberate rather than an oversight.
    io.BackendPlatformName = "gtg-overlay-probe (none)";

    // 1.92 lets the renderer backend create textures on demand, so the atlas
    // does not have to be built before the first frame. Asking for the default
    // font explicitly removes any doubt about whether one exists.
    io.Fonts->AddFontDefault();

    if (!ImGui_ImplDX11_Init(g_device, g_context)) {
        probe::Print("[T-04] ImGui_ImplDX11_Init failed\n");
        ImGui::DestroyContext();
        g_context->Release();
        g_context = nullptr;
        g_device->Release();
        g_device = nullptr;
        g_overlay_state.store(OverlayState::BackendInitFailed);
        return false;
    }

    ::QueryPerformanceFrequency(&g_qpc_freq);
    ::QueryPerformanceCounter(&g_qpc_last);

    g_published.texture().Bind(g_device, g_context);

    DXGI_SWAP_CHAIN_DESC desc{};
    swap_chain->GetDesc(&desc);
    probe::Print(
        "[T-04] ImGui %s on someone else's device: %ux%u, "
        "device=%p context=%p, RendererHasTextures=%d\n",
        IMGUI_VERSION, desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<void*>(g_device),
        static_cast<void*>(g_context),
        (io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) ? 1 : 0);

    // Printed rather than assumed: the capture checker reads these numbers off
    // the run it is checking, so a change to the constants cannot silently
    // leave the check pointing at the old coordinates.
    probe::Print("[T-05] quad rect=%d,%d,%d,%d rgb=%d,%d,%d\n", kQuadLeft, kQuadTop, kQuadRight,
                 kQuadBottom, kQuadR, kQuadG, kQuadB);

    g_overlay_state.store(OverlayState::Ready);
    return true;
}

// Build and submit one frame. Returns false if it declined to draw.
bool DrawOverlayFrame(IDXGISwapChain* swap_chain) {
    namespace cost = probe::cost;
    cost::Begin();
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap_chain->GetDesc(&desc))) return false;
    if (desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0) return false;
    g_published.texture().SetTargetFormat(desc.BufferDesc.Format);
    cost::Mark(cost::kPre);

    // ─── Spike 2 T-04: the published dashboard ─────────────────────────────
    //
    // T-06: tell the publisher what this swapchain is, before asking it for a
    // bitmap. Does nothing on a frame where the answer has not changed.
    //
    // The panel size is what the swapchain will be scaled to on screen, and it is
    // asked of the monitor rather than of this process, because a DPI-virtualised
    // process is told the scaled number by GetSystemMetrics -- which is exactly
    // the case T-05 found and exactly the case the publisher needs to know about.
    // EnumDisplaySettings, not GetSystemMetrics and not MONITORINFO: both of
    // those are virtualised for an unaware process and would report the same
    // scaled number the swapchain was already created at, making the ratio one
    // and the whole field useless in precisely the case it exists for. The
    // display MODE is a hardware property and is not scaled.
    //
    // The scaling mode lives on IDXGISwapChain1, not on the legacy desc, and it
    // is half of what decides whether a swapchain smaller than its window is
    // stretched or merely placed. T-06 got that wrong without it.
    //
    // Both through L0's cache (design §10.1): asked when the swapchain or its
    // size changes, otherwise at most once a second, not every frame.
    const std::uint64_t now_us = NowUs();
    const probe::TargetFacts& facts =
        g_target_facts.Get(swap_chain, desc.BufferDesc.Width, desc.BufferDesc.Height, now_us);
    g_published.ReportTarget(desc.BufferDesc.Width, desc.BufferDesc.Height, facts.display_width,
                             facts.display_height, facts.scaling, now_us);
    cost::Mark(cost::kReport);

    const bool have_published =
        g_published.Acquire(desc.BufferDesc.Width, desc.BufferDesc.Height, NowUs());
    cost::Mark(cost::kAcquire);

    // L2 (design §10.1): rebuild the draw data only when what it draws
    // changed -- which texture, where, on what size of target. Otherwise the
    // previous ImDrawData is drawn again; it stays valid until the next
    // NewFrame. A new upload into the same texture does not invalidate it: Map
    // writes the same texture, so the SRV, and the key, are unchanged. Spike 1's
    // synthetic geometry draws a frame counter, so it rebuilds every frame.
    const bool drawing = have_published && g_published.HasTexture();
#ifdef GTG_REMOTE_RENDER_RESEARCH
    g_remote_visible = drawing;
#endif
    const DrawKey key{drawing ? g_published.TextureId() : ImTextureID{},
                      g_published.DestLeft(),
                      g_published.DestTop(),
                      g_published.DestRight(),
                      g_published.DestBottom(),
                      desc.BufferDesc.Width,
                      desc.BufferDesc.Height,
                      drawing};
    const bool synthetic = g_draw_synthetic.load(std::memory_order_relaxed);
    if (synthetic || !g_draw_key_valid || !(key == g_draw_key)) {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(desc.BufferDesc.Width),
                                static_cast<float>(desc.BufferDesc.Height));

        // DeltaTime must be strictly positive or ImGui's own timing goes wrong;
        // a minimised or stalled frame can legitimately measure zero.
        LARGE_INTEGER now{};
        ::QueryPerformanceCounter(&now);
        const double elapsed = static_cast<double>(now.QuadPart - g_qpc_last.QuadPart) /
                               static_cast<double>(g_qpc_freq.QuadPart);
        g_qpc_last = now;
        io.DeltaTime = elapsed > 0.0 ? static_cast<float>(elapsed) : 1.0f / 60.0f;

        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        // The bitmap is BGRA with straight alpha, which is what ImGui's default
        // blend state (SrcAlpha, InvSrcAlpha) expects -- the reason
        // AF-20260926-single-rasteriser gave RenderToBitmap an AlphaMode at all.
        // White tint; the selected bitmap view decodes RGB for sRGB targets.
        // Alpha remains straight and is never gamma converted.
        if (drawing) {
            fg->AddImage(ImTextureRef(key.texture), ImVec2(key.left, key.top),
                         ImVec2(key.right, key.bottom), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                         IM_COL32_WHITE);
#if defined(GTG_DIRECT_DRAW) && !defined(GTG_BITMAP_ONLY)
            // Same IPC texture at a second position: distinguish spatial
            // occlusion from texture/alpha failure without changing the backend.
            const float thumbnail_height = 200.0f * (key.bottom - key.top) / (key.right - key.left);
            fg->AddImage(ImTextureRef(key.texture), ImVec2(264.0f, 32.0f),
                         ImVec2(464.0f, 32.0f + thumbnail_height), ImVec2(0.0f, 0.0f),
                         ImVec2(1.0f, 1.0f), IM_COL32_WHITE);
            // Visibility A/B: opaque geometry on the exact same chain/backend
            // as the bitmap, using ImGui's atlas rather than the IPC texture.
            // Keep the existing publisher/heartbeat gate and cached draw data.
            fg->AddRectFilled(
                ImVec2(static_cast<float>(kQuadLeft), static_cast<float>(kQuadTop)),
                ImVec2(static_cast<float>(kQuadRight), static_cast<float>(kQuadBottom)),
                IM_COL32(kQuadR, kQuadG, kQuadB, 255));
            // Cyan marker follows the main bitmap's anchor, unlike the fixed
            // magenta marker. Both use the same atlas/pipeline and stay cached.
            fg->AddRectFilled(ImVec2(key.left, key.top),
                              ImVec2(key.left + 200.0f, key.top + 100.0f),
                              IM_COL32(0, 255, 255, 255));
#endif
        }

        // Spike 1's geometry, only when asked for. See g_draw_synthetic: with a
        // publisher present this would cover the thing under test, and with none
        // present it would break T-08's floor.
        if (g_draw_synthetic.load(std::memory_order_relaxed)) {
            fg->AddRectFilled(
                ImVec2(static_cast<float>(kQuadLeft), static_cast<float>(kQuadTop)),
                ImVec2(static_cast<float>(kQuadRight), static_cast<float>(kQuadBottom)),
                IM_COL32(kQuadR, kQuadG, kQuadB, 255));

            // Translucent and full of text, which is what exercises the font atlas
            // and therefore the backend-managed texture path.
            const float panel_top = static_cast<float>(kQuadBottom) + 16.0f;
            fg->AddRectFilled(ImVec2(static_cast<float>(kQuadLeft), panel_top),
                              ImVec2(static_cast<float>(kQuadLeft) + 272.0f, panel_top + 44.0f),
                              IM_COL32(18, 22, 30, 200));
            fg->AddRect(ImVec2(static_cast<float>(kQuadLeft), panel_top),
                        ImVec2(static_cast<float>(kQuadLeft) + 272.0f, panel_top + 44.0f),
                        IM_COL32(235, 200, 232, 255));
            char line[96];
            std::snprintf(line, sizeof(line), "GTG overlay probe  %ux%u  frame %u",
                          desc.BufferDesc.Width, desc.BufferDesc.Height,
                          g_frames_rendered.load(std::memory_order_relaxed) + 1);
            fg->AddText(ImVec2(static_cast<float>(kQuadLeft) + 12.0f, panel_top + 14.0f),
                        IM_COL32(245, 248, 252, 255), line);
        }

        ImGui::Render();
        g_draw_key = key;
        g_draw_key_valid = true;
        g_draw_rebuilds.fetch_add(1, std::memory_order_relaxed);
        cost::Mark(cost::kImGui);
    }
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr) return false;

    // Nothing to submit is a legitimate outcome now, not a failure: it is what a
    // probe with no publisher looks like, and T-08 requires exactly that. But it
    // must not be counted as a rendered frame, and it must not touch the host's
    // render targets -- so return before any of that happens.
    if (draw_data->CmdListsCount == 0 || draw_data->TotalVtxCount == 0) {
        g_last_draw_lists.store(0);
        g_last_vertices.store(0);
        return false;
    }
    g_last_draw_lists.store(draw_data->CmdListsCount);
    g_last_vertices.store(draw_data->TotalVtxCount);
#ifdef GTG_DIRECT_DRAW
    // One representative immutable sample, before touching host render state.
    // Conditional rendering is observed, not altered; no GPU query/readback.
    if (g_draw_diagnostics && g_draw_diagnostics->draw_state == 0) {
        auto& d = *g_draw_diagnostics;
        d.draw_state = 1;
        Microsoft::WRL::ComPtr<ID3D11Predicate> predicate;
        BOOL value{};
        g_context->GetPredication(&predicate, &value);
        d.predicate_present = predicate ? 1 : 0;
        d.predicate_value = value;
        d.lists = draw_data->CmdListsCount;
        d.vertices = draw_data->TotalVtxCount;
        for (const auto* list : draw_data->CmdLists) {
            for (const auto& command : list->CmdBuffer) {
                if (d.commands++ == 0) {
                    d.clip_left = command.ClipRect.x;
                    d.clip_top = command.ClipRect.y;
                    d.clip_right = command.ClipRect.z;
                    d.clip_bottom = command.ClipRect.w;
                }
                const auto left =
                    (command.ClipRect.x - draw_data->DisplayPos.x) * draw_data->FramebufferScale.x;
                const auto top =
                    (command.ClipRect.y - draw_data->DisplayPos.y) * draw_data->FramebufferScale.y;
                const auto right =
                    (command.ClipRect.z - draw_data->DisplayPos.x) * draw_data->FramebufferScale.x;
                const auto bottom =
                    (command.ClipRect.w - draw_data->DisplayPos.y) * draw_data->FramebufferScale.y;
                if (!command.UserCallback && command.ElemCount && right > left && bottom > top &&
                    right > 0 && bottom > 0 && left < desc.BufferDesc.Width &&
                    top < desc.BufferDesc.Height)
                    ++d.visible_clips;
            }
        }
        InterlockedExchange(&d.draw_state, 2);
    }
#endif

    // The back buffer is the game's, and so is the render-target binding. Take
    // a view of the buffer, save what was bound, draw, put it back.
    //
    // ImGui's backend saves and restores the state IT touches
    // (BACKUP_DX11_STATE). It does not save the render targets, because it does
    // not set them -- that is this function's doing, so it is this function's to
    // undo.
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
        if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) || !back_buffer)
            return false;
        D3D11_TEXTURE2D_DESC actual{};
        back_buffer->GetDesc(&actual);
        // D3D11's own set: SDR plus the 8-bit sRGB targets this path samples
        // for (sdr_target_format.hpp). Not the shared whitelist, which is DX12's.
        if (!gtg::overlay::integration::MatchesD3D11Buffer(
                desc.BufferDesc.Format, desc.BufferDesc.Width, desc.BufferDesc.Height,
                actual.Format, actual.Width, actual.Height, actual.SampleDesc.Count))
            return false;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(g_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &rtv)) || !rtv)
            return false;

        struct RestoreTargets {
            ID3D11DeviceContext* context;
            ID3D11RenderTargetView* saved[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            ID3D11DepthStencilView* depth{};

            ~RestoreTargets() {
                context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved, depth);
                for (auto* view : saved)
                    if (view) view->Release();
                if (depth) depth->Release();
            }
        } restore{g_context};

        g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, restore.saved,
                                      &restore.depth);
        auto* target = rtv.Get();
        g_context->OMSetRenderTargets(1, &target, nullptr);
        ImGui_ImplDX11_RenderDrawData(draw_data);
    }
    cost::Mark(cost::kRecord);

    // A device that has been removed keeps accepting calls and silently does
    // nothing, so the only way to know is to ask. Stop drawing rather than
    // spend every remaining frame on a dead device.
    const HRESULT removed = g_device->GetDeviceRemovedReason();
    cost::Mark(cost::kRemoved);
    if (FAILED(removed)) {
        probe::Print("[T-04] device removed, reason=0x%08lX -- overlay stopped\n",
                     static_cast<unsigned long>(removed));
        g_overlay_state.store(OverlayState::DeviceRemoved);
        return false;
    }
    return true;
}

// Everything above, wrapped so that no failure can cost the host its frame.
void TryDrawOverlay(IDXGISwapChain* swap_chain) {
    const std::uint64_t present_n = g_presents_seen.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool report = present_n == 1 || present_n % 120 == 0;
    // The gate, first and cheapest. Everything below this line -- including the
    // one-time initialisation -- is skipped while the overlay is off, so a
    // disabled overlay never creates an ImGui context, never allocates a device
    // object, and never touches the host's pipeline state.
    if (!g_draw_enabled.load(std::memory_order_relaxed)) {
        g_frames_skipped.fetch_add(1, std::memory_order_relaxed);
        if (report) g_published.PrintSummary(present_n);
        return;
    }

    const OverlayState state = g_overlay_state.load();
    if (state == OverlayState::DeviceUnavailable || state == OverlayState::BackendInitFailed ||
        state == OverlayState::DeviceRemoved) {
        g_frames_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Reentrancy guard. ImGui has one global context and no locking, so a
    // second thread inside Present must decline rather than interleave.
    bool expected = false;
    if (!g_overlay_busy.compare_exchange_strong(expected, true)) {
        g_frames_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (!g_overlay_attempted.exchange(true)) {
        InitialiseOverlay(swap_chain);
    }

    const OverlayState current = g_overlay_state.load();
    if (current == OverlayState::Ready || current == OverlayState::Idle) {
        if (DrawOverlayFrame(swap_chain)) {
            g_frames_rendered.fetch_add(1, std::memory_order_relaxed);
            // Back to Ready only from Idle: a device-removed state set inside
            // DrawOverlayFrame must not be overwritten by its own success.
            if (current == OverlayState::Idle) {
                g_overlay_state.compare_exchange_strong(const_cast<OverlayState&>(current),
                                                        OverlayState::Ready);
            }
        } else {
            g_frames_skipped.fetch_add(1, std::memory_order_relaxed);
            // Nothing was drawn. If that was because the transport had nothing
            // for us -- rather than because the device died -- say so, so a host
            // can tell the safety floor from a fault.
            OverlayState expected_state = OverlayState::Ready;
            if (g_published.state() != probe::ConsumerState::Drawing) {
                g_overlay_state.compare_exchange_strong(expected_state, OverlayState::Idle);
            }
        }
    }

    if (report) g_published.PrintSummary(present_n);
    g_overlay_busy.store(false);
}

}  // namespace

#ifdef GTG_REMOTE_RENDER_RESEARCH
extern "C" std::uint64_t WINAPI GtgOverlayRemoteCopies() {
    return g_published.stats().copy_attempts.load(std::memory_order_relaxed);
}

extern "C" std::uint32_t WINAPI GtgOverlayRemotePlacementSerial() {
    return g_published.ResearchPlacementSerial();
}

// Research adapter only: the remote transaction owns the hook, not Attach.
extern "C" void GtgOverlayRemoteDrawFrame(IDXGISwapChain* swap_chain) {
    g_remote_visible = false;
    TryDrawOverlay(swap_chain);
}
#ifdef GTG_DIRECT_DRAW
extern "C" void GtgOverlaySetDrawDiagnostics(gtg::research::DrawDiagnostics* data) {
    g_draw_diagnostics = data;
}

extern "C" void GtgOverlayInvalidateChain() noexcept {
    g_target_facts = {};
    g_draw_key_valid = false;
}
#endif
extern "C" bool GtgOverlayRemoteVisible() {
    return g_remote_visible;
}
#endif

extern "C" __declspec(dllexport) BOOL WINAPI GtgOverlayResearchAttach(IDXGISwapChain* swap_chain) {
    if (swap_chain == nullptr) return FALSE;

    // GTG_OVERLAY_DRAW=0 installs the hook and draws nothing. Separates what
    // the interception costs from what the drawing costs, which one run cannot
    // do, and exercises the off switch rather than asserting it exists.
    char env[8]{};
    if (::GetEnvironmentVariableA("GTG_OVERLAY_DRAW", env, sizeof(env)) > 0 && env[0] == '0') {
        g_draw_enabled.store(false, std::memory_order_relaxed);
        g_overlay_state.store(OverlayState::Disabled);
        probe::Print("[T-06] GTG_OVERLAY_DRAW=0 -- hooking, not drawing\n");
    }

    // GTG_OVERLAY_SYNTHETIC=1 brings back Spike 1's quad and panel. Off by
    // default because Spike 2 draws the published bitmap, and drawing both would
    // put invented geometry over the thing being verified.
    char syn[8]{};
    if (::GetEnvironmentVariableA("GTG_OVERLAY_SYNTHETIC", syn, sizeof(syn)) > 0 && syn[0] == '1') {
        g_draw_synthetic.store(true, std::memory_order_relaxed);
        probe::Print("[T-04] GTG_OVERLAY_SYNTHETIC=1 -- Spike 1 geometry too\n");
    }

    void** vtable = *reinterpret_cast<void***>(swap_chain);
    if (vtable == nullptr || vtable[kPresentVtableSlot] == nullptr) return FALSE;
    void* const present_slot = vtable[kPresentVtableSlot];

    // Attach once. A second call on the same target is success if the first
    // one took, not a second patch on top of it.
    void* expected = nullptr;
    if (!g_present_target.compare_exchange_strong(expected, present_slot)) {
        return expected == present_slot && splice_is_hooked(present_slot) ? TRUE : FALSE;
    }

    auto& entry =
        SPLICE_HOOK_ADDR_STATIC(reinterpret_cast<PresentFunction>(present_slot))
            .observe()
            .onInvoke([](auto original, IDXGISwapChain* self, UINT sync, UINT flags) -> HRESULT {
            // Draw before presenting: the frame is composed but not yet
            // submitted, which is the only moment an overlay can add to it.
                const std::uint64_t overlay_start = probe::QpcNs();
                TryDrawOverlay(self);
                probe::cost::Record(probe::QpcNs() - overlay_start);

            // Every intercepted present must still present. A hook that keeps
            // the frame is not an overlay, it is a freeze.
                return original(self, sync, flags);
            });
    splice::install_all();
    g_entry.store(&entry, std::memory_order_release);

    // Reported as installed only if it actually is. The caller treats FALSE
    // as a failed attach and stops, which is the right outcome: a probe that
    // silently did nothing would be read as evidence that hooking works.
    return splice_is_hooked(present_slot) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) std::uint32_t WINAPI GtgOverlayResearchHitCount() {
    auto* const entry = g_entry.load(std::memory_order_acquire);
    if (entry == nullptr) return 0;
    // invocations() is nullopt when nothing asked to observe; here it always
    // did, so the fallback is unreachable rather than meaningful.
    return static_cast<std::uint32_t>(entry->invocations().value_or(0));
}

// T-04's result, as numbers rather than as an absence of complaints.
extern "C" __declspec(dllexport) std::uint32_t WINAPI GtgOverlayResearchFramesRendered() {
    return g_frames_rendered.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) int WINAPI GtgOverlayResearchOverlayState() {
    return static_cast<int>(g_overlay_state.load(std::memory_order_relaxed));
}

// ─── Spike 2: the consumer's own account of itself ─────────────────────────
//
// One export per counter rather than a struct across the DLL boundary, so the
// harness reads them with GetProcAddress and no shared header. Ugly and
// completely inert -- which is the right trade for a spike whose numbers have to
// be trustworthy.
extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchUploads() {
    return g_published.stats().uploads.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchReused() {
    return g_published.stats().reused.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchRejected() {
    return g_published.stats().rejected.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchOpenAttempts() {
    return g_published.stats().open_attempts.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchStaleHeartbeat() {
    return g_published.stats().stale_heartbeat.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchDisabledByFlag() {
    return g_published.stats().disabled_by_flag.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchBadHeader() {
    return g_published.stats().bad_header.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchTextureRecreates() {
    return g_published.stats().texture_recreates.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchRequestsWritten() {
    return g_published.stats().requests_written.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchGeometryOutside() {
    return g_published.stats().geometry_outside.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) std::uint32_t WINAPI GtgOverlayResearchLastSerial() {
    return g_published.stats().last_serial.load(std::memory_order_relaxed);
}

// Where the bitmap was drawn, for the T-05 checker. -1 for left means nothing
// has been drawn, which is distinguishable from "drawn at 0".
extern "C" __declspec(dllexport) void WINAPI GtgOverlayResearchDestRect(std::int32_t* left,
                                                                        std::int32_t* top,
                                                                        std::int32_t* width,
                                                                        std::int32_t* height) {
    const auto& d = g_published.dest();
    if (left != nullptr) *left = d.left.load(std::memory_order_relaxed);
    if (top != nullptr) *top = d.top.load(std::memory_order_relaxed);
    if (width != nullptr) *width = d.width.load(std::memory_order_relaxed);
    if (height != nullptr) *height = d.height.load(std::memory_order_relaxed);
}

// Per-frame and per-upload cost (frame_cost.hpp), the same exports as D3D12.
GTG_FRAME_COST_EXPORTS(g_published.stats())

// How many frames L2 had to rebuild the draw data on (design §10.1).
extern "C" __declspec(dllexport) std::uint64_t WINAPI GtgOverlayResearchDrawRebuilds() {
    return g_draw_rebuilds.load(std::memory_order_relaxed);
}
