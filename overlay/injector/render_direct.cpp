// Draw-only adapter: no pixel readback or GPU waits. Named-game variant.
#include "hook_protocol.hpp"
#include "draw_admission.hpp"
#include "d3d11_chain_candidate.hpp"
#include "draw_diagnostics.hpp"
#include "motion_oracle.hpp"
#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi.h>
#include <wrl/client.h>
extern "C" void GtgOverlayRemoteDrawFrame(IDXGISwapChain*);
extern "C" std::uint32_t WINAPI GtgOverlayResearchFramesRendered();
extern "C" void GtgOverlaySetDrawDiagnostics(gtg::research::DrawDiagnostics*);
extern "C" void GtgOverlayInvalidateChain() noexcept;

namespace {
gtg::research::DrawAdmissionGate gate;
#ifdef GTG_CHAIN_HANDOFF
gtg::research::ChainHandoffGate handoff_gate{0};
gtg::research::ChainReferences handoff_refs;
#endif
// Process-lifetime ownership after binding, never back-buffer ownership.
IUnknown* retained_chain{};
IUnknown* retained_device{};
std::uint32_t last_serial{};
std::int32_t last_left = -1;
HANDLE diagnostic_mapping{};
gtg::research::DrawDiagnostics* diagnostics{};
}

// Worker-only, before installing Present. No mapping operations on the hot path.
bool PrepareDrawDiagnostics() noexcept {
    using namespace gtg::research;
#ifdef GTG_CHAIN_HANDOFF
    LARGE_INTEGER nonce{};
    QueryPerformanceCounter(&nonce);
    const auto session = static_cast<std::uint64_t>(nonce.QuadPart) ^
                         (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32);
    handoff_gate.SetSession(session ? session : 1);
#endif
    wchar_t name[64]{};
    DrawSectionName(GetCurrentProcessId(), name);
    diagnostic_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!diagnostic_mapping) return false;
    diagnostics = static_cast<DrawDiagnostics*>(
        MapViewOfFile(diagnostic_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DrawDiagnostics)));
    if (!diagnostics || diagnostics->magic != kDrawDiagnosticMagic ||
        diagnostics->size != sizeof(DrawDiagnostics) ||
        diagnostics->version != kDrawDiagnosticVersion ||
        diagnostics->pid != GetCurrentProcessId()) {
        if (diagnostics) UnmapViewOfFile(diagnostics);
        CloseHandle(diagnostic_mapping);
        diagnostic_mapping = nullptr;
        diagnostics = nullptr;
        return false;
    }
    GtgOverlaySetDrawDiagnostics(diagnostics);
    return true;
}

void CloseDrawDiagnostics() noexcept {
    GtgOverlaySetDrawDiagnostics(nullptr);
    if (diagnostics) UnmapViewOfFile(diagnostics);
    if (diagnostic_mapping) CloseHandle(diagnostic_mapping);
    diagnostics = nullptr;
    diagnostic_mapping = nullptr;
}

gtg::research::DrawDiagnostics* ProductDrawDiagnostics() noexcept {
    return diagnostics;
}

void RenderAndCheck(IDXGISwapChain* chain, gtg::research::HookSection& counters, bool) {
    using Microsoft::WRL::ComPtr;
    using namespace gtg::research;
    if (!chain) {
#ifdef GTG_CHAIN_HANDOFF
        handoff_gate.CancelPending(handoff_refs);
#endif
        return;
    }
#ifndef GTG_STAGED_DIRECT_DRAW
    if (InterlockedCompareExchange(&counters.header.release, 0, 0)) return;
#endif
    ComPtr<ID3D11Device> device;
    ComPtr<IUnknown> chain_id, device_id;
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || !device ||
        FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain_id))) || !chain_id ||
        FAILED(device->QueryInterface(IID_PPV_ARGS(&device_id))) || !device_id) {
        if (diagnostics) InterlockedIncrement(&diagnostics->invalid);
#ifdef GTG_CHAIN_HANDOFF
        handoff_gate.CancelPending(handoff_refs);
#endif
        return;
    }
    const DrawIdentity identity{reinterpret_cast<std::uintptr_t>(chain_id.Get()),
                                reinterpret_cast<std::uintptr_t>(device_id.Get()),
                                GetCurrentThreadId()};
    const auto draw = [&]() noexcept {
        if (diagnostics && InterlockedCompareExchange(&diagnostics->identity_state, 0, 0) == 0) {
#ifndef GTG_CHAIN_HANDOFF
            retained_chain = chain_id.Detach();
            retained_device = device_id.Detach();
#endif
            if (diagnostics) {
                diagnostics->identity_state = 1;
                diagnostics->chain = identity.chain;
                diagnostics->device = identity.device;
                diagnostics->thread = identity.thread;
                DXGI_SWAP_CHAIN_DESC desc{};
                diagnostics->desc_result = chain->GetDesc(&desc);
                if (SUCCEEDED(diagnostics->desc_result)) {
                    diagnostics->output_window =
                        reinterpret_cast<std::uintptr_t>(desc.OutputWindow);
                    diagnostics->width = desc.BufferDesc.Width;
                    diagnostics->height = desc.BufferDesc.Height;
                    diagnostics->format = desc.BufferDesc.Format;
                    diagnostics->swap_effect = desc.SwapEffect;
                }
                ComPtr<ID3D11On12Device> on12;
                diagnostics->on12_result = device->QueryInterface(IID_PPV_ARGS(&on12));
                diagnostics->fullscreen_result =
                    chain->GetFullscreenState(&diagnostics->fullscreen, nullptr);
                InterlockedExchange(&diagnostics->identity_state, 2);
            }
        }
        // Staged mode publishes COM identity without invoking the renderer.
        if (InterlockedCompareExchange(&counters.header.release, 0, 0)) return;
        const auto before = GtgOverlayResearchFramesRendered();
        try {
            GtgOverlayRemoteDrawFrame(chain);
        } catch (...) {
            InterlockedIncrement(&counters.control_errors);
            return;
        }
        SnapshotMotionCounters(counters);
        if (GtgOverlayResearchFramesRendered() == before) return;
        InterlockedIncrement(&counters.control_samples[0]);
        std::int32_t left{}, top{}, width{}, height{};
        GtgOverlayResearchDestRect(&left, &top, &width, &height);
        InterlockedExchange(&counters.observed_width, width);
        InterlockedExchange(&counters.observed_height, height);
        InterlockedExchange(&counters.motion_last_left, left);
        InterlockedExchange(&counters.leading_small_samples, top);
        InterlockedExchange(&counters.motion_sampled_placement,
                            static_cast<LONG>(GtgOverlayRemotePlacementSerial()));
        const auto serial = GtgOverlayResearchLastSerial();
        if (serial == last_serial && left != last_left)
            InterlockedIncrement(&counters.motion_same_bitmap);
        last_serial = serial;
        last_left = left;
    };
#ifdef GTG_CHAIN_HANDOFF
    const auto invalidate = []() noexcept {
        GtgOverlayInvalidateChain();
    };
    const auto record = +[](const HandoffDecision& value) noexcept {
        if (diagnostics) RecordHandoffDecision(*diagnostics, value);
    };
    D3D11ChainCandidate candidate{chain,        device.Get(), chain_id,   device_id,
                                  handoff_refs, draw,         invalidate, record};
    const auto result = handoff_gate.Run(
        true, InterlockedCompareExchange(&counters.header.release, 0, 0) == 0, identity, candidate);
    const auto admission = result == HandoffResult::kDraw   ? DrawAdmission::kExecuted
                           : result == HandoffResult::kBusy ? DrawAdmission::kBusy
                                                            : DrawAdmission::kMismatch;
    if (diagnostics) {
        // Independent post-Run observations, not a coherent identity tuple.
        // Generation is atomic; competing callers report busy only.
        if (result != HandoffResult::kBusy) {
            InterlockedExchange64(&diagnostics->generation,
                                  static_cast<LONG64>(handoff_gate.Generation()));
            InterlockedExchange(&diagnostics->handoff_result, static_cast<LONG>(result));
        }
    }
#else
    const auto admission = gate.Run(true, identity, draw);
#endif
    if (diagnostics) {
        if (admission == DrawAdmission::kBusy) InterlockedIncrement(&diagnostics->busy);
        if (admission == DrawAdmission::kMismatch) {
            InterlockedIncrement(&diagnostics->mismatch);
            if (InterlockedCompareExchange(&diagnostics->identity_state, 0, 0) == 2) {
                const DrawIdentity expected{diagnostics->chain, diagnostics->device,
                                            diagnostics->thread};
                RecordDrawMismatch(*diagnostics, expected, identity,
                                   [chain](DrawMismatchSnapshot& snapshot) noexcept {
                                       DXGI_SWAP_CHAIN_DESC desc{};
                                       snapshot.desc_result = chain->GetDesc(&desc);
                                       if (SUCCEEDED(snapshot.desc_result)) {
                                           snapshot.output_window =
                                               reinterpret_cast<std::uintptr_t>(desc.OutputWindow);
                                           snapshot.width = desc.BufferDesc.Width;
                                           snapshot.height = desc.BufferDesc.Height;
                                       }
                                       snapshot.fullscreen_result =
                                           chain->GetFullscreenState(&snapshot.fullscreen, nullptr);
                                   });
            }
        }
    }
}
