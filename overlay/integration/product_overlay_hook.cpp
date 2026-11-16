#include "backend_selection.hpp"
#include "initial_refusal.hpp"
#include "d3d12_bitmap_renderer.hpp"
#include "d3d12_color_transition.hpp"
#include "product_color_contract.hpp"
#include "color_transition_scope.hpp"
#include "fresh_output.hpp"
#include "../injector/hook_protocol.hpp"
#include "../injector/draw_diagnostics.hpp"
#include "../injector/queue_binding.hpp"
#include "../injector/staged_attach.hpp"
// Included directly now. It used to arrive through install_thread_ops.hpp,
// which is gone -- a transitive include is not a dependency you declared.
#include "../injector/install_diagnostics.hpp"
#include "../injector/install_policy.hpp"
#include "slot_admission.hpp"
#include <splice/live.h>
#include <memory>
#include <array>
#include <dxgi1_6.h>
bool PrepareDrawDiagnostics() noexcept;
void CloseDrawDiagnostics() noexcept;
gtg::research::DrawDiagnostics* ProductDrawDiagnostics() noexcept;
void RenderAndCheck(IDXGISwapChain*, gtg::research::HookSection&, bool);

namespace {
using namespace gtg::research;
using namespace gtg::overlay::integration;
std::array<std::atomic<void*>, 7> originals{};

// The seven sites keep their numbers everywhere a reader sees them -- the
// section's fields, the diagnostic report, `originals`, `entry_shape` -- and
// the two the install treats differently are named. They live in
// `entry_admission.hpp` because the preflight in the executable decides the
// same thing about the same sites and must not hold its own copy of either the
// numbers or the rule.
using gtg::research::kPresentSite;
using gtg::research::kSignalSite;
using gtg::research::kSiteCount;
using gtg::research::PatchedInline;

// Which report index each site of the inline group is. These used to be the
// same numbers and are not any more: Signal is report index one and is not in
// that group at all, so the group's ordinal two is the report's index three. A
// table, because the alternative is the same arithmetic written out at each
// use and silently wrong at one of them -- and what it would be wrong about is
// which site a refusal names.
constexpr std::size_t kInlineSites[] = {0, 2, 3, 4, 5, 6};

// A refused vtable cell, in the same negative space as the other refusals
// `product_ready` carries. -300 is unused: -10..-16 is a prologue that would
// not decode, -100..-106 the one-instruction rule, -2xx the runtime refusals.
constexpr LONG kSignalCellRefused = -300;
std::atomic<bool> active{false};
HookSection* counters{};
std::atomic<bool> failed{false};
std::atomic<std::uintptr_t> selected_d12_chain{};
std::atomic<LONG> selected_backend{0};
std::atomic<bool> observed_sdr{false};
ColorPolicy session_color_policy{
    ColorPolicy::Strict}; // Worker writes before active release; never mutable thereafter.
QueueBinding binding;
D3d12BitmapRenderer* renderer{}; // Intentionally resident, never destroyed in DllMain.
D3d12LifecycleGate owner;
bool initialized{}; // Access only under owner admission.

struct Clock {
    UINT64 Now() noexcept { return GetTickCount64(); }

    void Pause() noexcept { Sleep(1); }
};

struct Context {
    IUnknown* chain;
    IUnknown* device;
};

thread_local Context* context{};
thread_local unsigned recursion{};
thread_local IUnknown* resizing{};

template <class T>
T Original(unsigned index) noexcept {
    return reinterpret_cast<T>(originals[index].load(std::memory_order_acquire));
}

void Failure(LONG reason = -299) noexcept {
    failed.store(true, std::memory_order_release);
    if (counters) {
        StopColorProvenance(counters->product_color_provenance);
        InterlockedIncrement(&counters->control_errors);
        RecordRuntimeRefusal(counters->product_ready, reason);
    }
}

bool MatchesMethods(IDXGISwapChain3* chain) noexcept {
    auto** table = *reinterpret_cast<void***>(chain);
    constexpr unsigned indices[] = {13, 39, 10, 22, 38};
    if (reinterpret_cast<std::uint64_t>(table[8]) != counters->present_address) {
        RecordInitialRefusal(counters->product_ready, InitialRefusal::PresentMethod);
        return false;
    }
    for (unsigned i = 0; i < 5; ++i)
        if (reinterpret_cast<std::uint64_t>(table[indices[i]]) != counters->product_methods[i]) {
            RecordInitialRefusal(counters->product_ready,
                                 static_cast<InitialRefusal>(-241 - static_cast<LONG>(i)));
            return false;
        }
    return true;
}

LONG QualifySdrOutput(IDXGISwapChain* chain) noexcept {
    // Query our own current output snapshot, never the game's stale parent.
    // No game factory/swapchain is recreated or assigned a new display mode.
    struct Ops {
        IDXGISwapChain* chain;
        HWND window{};
        HMONITOR monitor{};
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
        DXGI_OUTPUT_DESC basic{};
        DXGI_OUTPUT_DESC1 description{};

        bool Window() noexcept {
            DXGI_SWAP_CHAIN_DESC desc{};
            if (FAILED(chain->GetDesc(&desc)) || !desc.OutputWindow || !IsWindow(desc.OutputWindow))
                return false;
            window = desc.OutputWindow;
            monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
            return monitor != nullptr;
        }

        bool Create() noexcept { return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); }

        bool Current() noexcept { return factory->IsCurrent() != FALSE; }

        HRESULT Adapter(UINT index) noexcept {
            return factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf());
        }

        HRESULT Output(UINT index) noexcept {
            return adapter->EnumOutputs(index, output.ReleaseAndGetAddressOf());
        }

        bool OutputDescription() noexcept { return SUCCEEDED(output->GetDesc(&basic)); }

        bool MatchesMonitor() noexcept {
            return basic.AttachedToDesktop && basic.Monitor == monitor;
        }

        LONG Find() noexcept { return FindFreshMonitorOutput(*this); }

        bool Output6() noexcept { return SUCCEEDED(output.As(&output6)); }

        bool Description() noexcept { return SUCCEEDED(output6->GetDesc1(&description)); }

        bool Sdr() noexcept {
            return description.AttachedToDesktop && description.Monitor == monitor &&
                   description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        }

        bool SameMonitor() noexcept {
            return IsWindow(window) && MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == monitor;
        }
    } ops{chain};

    return QualifyFreshOutput(ops);
}

HRESULT STDMETHODCALLTYPE Signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
    InterlockedIncrement64(&counters->signal_calls);
    if (active.load(std::memory_order_acquire) && context && recursion == 1) {
        Microsoft::WRL::ComPtr<IUnknown> queue_id, device_id;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> public_queue;
        // The intercepted runtime implementation can receive an inner queue
        // interface. Submit through the public interface so debug/runtime
        // command-list translation and workload tracking are not bypassed.
        // Canonical identity still qualifies the object; original Signal below
        // must receive its unchanged raw this pointer, not this normalized one.
        if (SUCCEEDED(queue->QueryInterface(IID_PPV_ARGS(&public_queue))) &&
            SUCCEEDED(public_queue.As(&queue_id)) &&
            SUCCEEDED(public_queue->GetDevice(IID_PPV_ARGS(&device))) &&
            SUCCEEDED(device.As(&device_id))) {
            struct RetainOps {
                void Retain(void* queue, void* chain) noexcept {
                    static_cast<ID3D12CommandQueue*>(queue)->AddRef();
                    static_cast<IUnknown*>(chain)->AddRef();
                }
            } retain;

            const QueueObservation observation{
                public_queue.Get(),
                context->chain,
                reinterpret_cast<std::uintptr_t>(queue_id.Get()),
                reinterpret_cast<std::uintptr_t>(context->chain),
                reinterpret_cast<std::uintptr_t>(device_id.Get()),
                reinterpret_cast<std::uintptr_t>(context->device),
                recursion,
                public_queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT};
            const auto result = binding.Offer(observation, retain);
            if (result == BindingResult::kBound) {
                InterlockedExchange64(&counters->queue_identity,
                                      static_cast<LONG64>(observation.queue_id));
                InterlockedIncrement64(&counters->nested_signal_calls);
            }
        } else
            binding.MarkAmbiguous();
        if (binding.Ambiguous()) InterlockedExchange(&counters->queue_ambiguous, 1);
    }
    const HRESULT result = Original<Fn>(1)(queue, fence, value);
    if (FAILED(result)) InterlockedIncrement(&counters->failures);
    return result;
}

// Return only after releasing renderer admission. Original Present owns TLS
// evidence but never our drawing gate, including recursively hooked Signal.
void Render(IDXGISwapChain* chain, UINT flags, Context& captured,
            Microsoft::WRL::ComPtr<IUnknown>& chain_id,
            Microsoft::WRL::ComPtr<IUnknown>& device_id) noexcept {
    if (!active.load(std::memory_order_acquire) || failed.load(std::memory_order_acquire) ||
        flags & DXGI_PRESENT_TEST || recursion)
        return;
    const auto backend = InspectBackend(chain);
    if (backend == Backend::D3d11) {
        // Auxiliary D11 windows must never change a selected D12 owner's
        // backend and accidentally bypass its resize/reference drain.
        LONG expected = 0;
        selected_backend.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
        if (selected_backend.load(std::memory_order_acquire) != 1) return;
        try {
            RenderAndCheck(chain, *counters, false);
        } catch (...) {
            Failure();
            return;
        }
        auto* data = ProductDrawDiagnostics();
        if (data && data->identity_state == 2 && data->on12_result == E_NOINTERFACE) {
            InterlockedExchange(&counters->product_backend, 1);
            RecordInitialReady(counters->product_ready);
            RecordColorProvenance(counters->product_color_provenance, ColorProvenance::Legacy8);
        }
        return;
    }
    const auto refuse = [](InitialRefusal reason) noexcept {
        RecordInitialRefusal(counters->product_ready, reason);
        Failure();
    };
    if (backend != Backend::D3d12) {
        refuse(InitialRefusal::Backend);
        return;
    }
    if (!renderer) {
        refuse(InitialRefusal::Renderer);
        return;
    }
    if (!counters->signal_address) {
        refuse(InitialRefusal::Signal);
        return;
    }
    if (selected_backend.load(std::memory_order_acquire) == 1) return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> chain3;
    DXGI_SWAP_CHAIN_DESC desc{};
    DWORD pid{};
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) {
        refuse(InitialRefusal::Chain3);
        return;
    }
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain_id)))) {
        refuse(InitialRefusal::ChainIdentity);
        return;
    }
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device)))) {
        refuse(InitialRefusal::Device);
        return;
    }
    if (FAILED(device.As(&device_id))) {
        refuse(InitialRefusal::DeviceIdentity);
        return;
    }
    const HRESULT description_result = chain->GetDesc(&desc);
    // Publish initial facts before qualification, including rejected formats.
    // product_ready remains the acceptance oracle, not identity_state alone.
    auto* initial_data = ProductDrawDiagnostics();
    if (initial_data && InterlockedCompareExchange(&initial_data->identity_state, 1, 0) == 0) {
        initial_data->chain = reinterpret_cast<std::uintptr_t>(chain_id.Get());
        initial_data->device = reinterpret_cast<std::uintptr_t>(device_id.Get());
        initial_data->output_window = reinterpret_cast<std::uintptr_t>(desc.OutputWindow);
        initial_data->width = desc.BufferDesc.Width;
        initial_data->height = desc.BufferDesc.Height;
        initial_data->format = desc.BufferDesc.Format;
        initial_data->swap_effect = desc.SwapEffect;
        initial_data->desc_result = description_result;
        initial_data->on12_result = E_NOINTERFACE;
        initial_data->thread = GetCurrentThreadId();
        InterlockedExchange(&initial_data->identity_state, 2);
    }
    if (FAILED(description_result)) {
        refuse(InitialRefusal::Description);
        return;
    }
    if (!MatchesMethods(chain3.Get())) {
        Failure();
        return;
    }
    if (!GetWindowThreadProcessId(desc.OutputWindow, &pid)) {
        refuse(InitialRefusal::Window);
        return;
    }
    if (pid != GetCurrentProcessId()) {
        refuse(InitialRefusal::WindowPid);
        return;
    }
    if (device->GetNodeCount() != 1) {
        refuse(InitialRefusal::NodeCount);
        return;
    }
    if (!IsSupportedSdrTarget(desc.BufferDesc.Format)) {
        refuse(InitialRefusal::Format);
        return;
    }
    captured = {chain_id.Get(), device_id.Get()};
    if (!binding.Ready()) return; // Original Present may produce first evidence.
    if (binding.ChainId() != reinterpret_cast<std::uintptr_t>(chain_id.Get())) {
        binding.MarkAmbiguous();
        Failure(-253);
        return;
    }
    LONG expected = 0;
    selected_backend.compare_exchange_strong(expected, 2, std::memory_order_acq_rel);
    if (selected_backend.load(std::memory_order_acquire) != 2) return;
    if (!owner.TryDraw()) return;

    struct Scope {
        ~Scope() { owner.EndDraw(); }
    } scope;

    selected_d12_chain.store(reinterpret_cast<std::uintptr_t>(chain_id.Get()),
                             std::memory_order_release);
    auto* data = ProductDrawDiagnostics();
    if (data && InterlockedCompareExchange(&data->identity_state, 1, 0) == 0) {
        data->chain = reinterpret_cast<std::uintptr_t>(chain_id.Get());
        data->device = reinterpret_cast<std::uintptr_t>(device_id.Get());
        data->output_window = reinterpret_cast<std::uintptr_t>(desc.OutputWindow);
        data->width = desc.BufferDesc.Width;
        data->height = desc.BufferDesc.Height;
        data->format = desc.BufferDesc.Format;
        data->desc_result = S_OK;
        data->on12_result = E_NOINTERFACE;
        data->thread = GetCurrentThreadId();
        InterlockedExchange(&data->identity_state, 2);
    }
    InterlockedExchange(&counters->product_backend, 2);
    RecordInitialReady(counters->product_ready);
    if (InterlockedCompareExchange(&counters->header.release, 0, 0)) return;
    if (!initialized) {
        // No swapchain color-space getter exists. Conservatively refuse an
        // HDR/unknown output rather than assume 8-bit alone proves SDR. Future
        // non-SDR SetColorSpace1 on this chain also permanently stops drawing.
        const LONG output_reason = QualifySdrOutput(chain);
        if (output_reason != 1) {
            Failure(output_reason);
            return;
        }
        initialized =
            renderer->Initialize(chain3.Get(), static_cast<ID3D12CommandQueue*>(binding.Queue()));
        if (!initialized) {
            RecordRuntimeRefusal(counters->product_ready, -201);
            Failure();
            return;
        }
    }
    // Use the actual renderer generation under owner admission, not a discovery
    // description that may predate resize. Unknown SDR10 is a pending no-draw
    // state, not permission to assume the monitor describes the chain.
    if (failed.load(std::memory_order_acquire)) return;
    const auto color =
        DecideSdrAdmission(renderer->TargetFormat(), observed_sdr.load(std::memory_order_acquire),
                           session_color_policy);
    RecordColorProvenance(counters->product_color_provenance, color);
    if (color == ColorProvenance::WaitingUnknown) {
#ifndef GTG_RESEARCH_SOLID_RECT
        renderer->ReportTargetMetadata();
#endif
        RecordColorWait(counters->product_ready);
        return;
    }
    if (color == ColorProvenance::None) {
        RecordRuntimeRefusal(counters->product_ready, -202);
        Failure();
        return;
    }
    ClearColorWait(counters->product_ready);
#ifdef GTG_RESEARCH_SOLID_RECT
    if (!renderer->DrawSolidRectangle()) {
        if (renderer->Failed()) Failure(-254);
        return;
    }
#else
    if (!renderer->Draw()) {
        if (renderer->Failed()) Failure(-254);
        return;
    }
#endif
    auto& stats = renderer->Stats();
    InterlockedExchange64(&counters->bitmap_uploads, static_cast<LONG64>(stats.uploads.load()));
    InterlockedExchange64(&counters->bitmap_copies,
                          static_cast<LONG64>(stats.copy_attempts.load()));
    InterlockedExchange(&counters->motion_sampled_placement,
                        static_cast<LONG>(renderer->PlacementSerial()));
    InterlockedIncrement(&counters->control_samples[0]);
}

template <class Call>
HRESULT WithPresent(IDXGISwapChain* chain, UINT flags, Call&& call) noexcept {
    if (recursion) return call(); // A Present1 implementation may call Present.
    Context captured{};
    Microsoft::WRL::ComPtr<IUnknown> chain_id, device_id;
    InterlockedIncrement64(&counters->hook_calls);
    Render(chain, flags, captured, chain_id, device_id);
    auto* previous = context;
    context = captured.chain ? &captured : nullptr;
    ++recursion;
    const HRESULT result = call();
    --recursion;
    context = previous;
    if (FAILED(result)) InterlockedIncrement(&counters->failures);
    return result;
}

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* chain, UINT interval, UINT flags) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    return WithPresent(chain, flags, [&] { return Original<Fn>(0)(chain, interval, flags); });
}

HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* chain, UINT interval, UINT flags,
                                   const DXGI_PRESENT_PARAMETERS* parameters) {
    using Fn =
        HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    const bool partial = parameters && (parameters->DirtyRectsCount || parameters->pScrollRect ||
                                        parameters->pScrollOffset);
    if (partial) Failure(-250);
    return WithPresent(chain, flags,
                       [&] { return Original<Fn>(5)(chain, interval, flags, parameters); });
}

template <class Call>
HRESULT WithResize(IDXGISwapChain* chain, Call&& call) noexcept {
    if (!active.load(std::memory_order_acquire) ||
        !selected_d12_chain.load(std::memory_order_acquire))
        return call();
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&identity))) ||
        selected_d12_chain.load(std::memory_order_acquire) !=
            reinterpret_cast<std::uintptr_t>(identity.Get()))
        return call();
    // Native ResizeBuffers/Fullscreen can delegate to another hooked method.
    // The outer callback already drained/released this exact chain generation;
    // nested original forwarding must not steal or close its admission.
    if (resizing == identity.Get()) return call();
    Clock clock;
    const bool owned = owner.BeginTransition(clock);
    // Only this callback's successful BeforeResize may authorize AfterResize.
    const bool drained = owned && (!initialized || renderer->BeforeResize(owner.Remaining(clock)));
    if (!drained) Failure(-251);
    auto* previous = resizing;
    resizing = identity.Get();
    const HRESULT result = call(); // Always unchanged, even timeout/refusal.
    resizing = previous;
    if (owned) {
        const LONG reason = DiagnoseOutputGeneration(
            drained, initialized, SUCCEEDED(result),
            binding.Ambiguous() || owner.Disabled() || failed.load(std::memory_order_acquire),
            [&]() noexcept { return QualifySdrOutput(chain); },
            [&](bool valid) noexcept { return renderer->AfterResize(valid ? result : E_ABORT); });
        owner.EndTransition(reason == 1);
        if (reason != 1) Failure(reason);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* chain, UINT count, UINT w, UINT h,
                                 DXGI_FORMAT format, UINT flags) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    return WithResize(chain, [&] { return Original<Fn>(2)(chain, count, w, h, format, flags); });
}

HRESULT STDMETHODCALLTYPE Resize1(IDXGISwapChain3* chain, UINT count, UINT w, UINT h,
                                  DXGI_FORMAT format, UINT flags, const UINT* masks,
                                  IUnknown* const* queues) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                           const UINT*, IUnknown* const*);
    return WithResize(chain, [&] {
        if (queues || masks) {
            // Drain the old qualified generation before refusing reassociation.
            binding.MarkAmbiguous();
            InterlockedExchange(&counters->queue_ambiguous, 1);
        }
        return Original<Fn>(3)(chain, count, w, h, format, flags, masks, queues);
    });
}

HRESULT STDMETHODCALLTYPE Fullscreen(IDXGISwapChain* chain, BOOL fullscreen, IDXGIOutput* output) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
    return WithResize(chain, [&] { return Original<Fn>(4)(chain, fullscreen, output); });
}

HRESULT STDMETHODCALLTYPE ColorSpace(IDXGISwapChain3* chain, DXGI_COLOR_SPACE_TYPE color) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
    const auto original = [&]() noexcept {
        return Original<Fn>(6)(chain, color);
    };
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (!active.load(std::memory_order_acquire) || failed.load(std::memory_order_acquire) ||
        selected_backend.load(std::memory_order_acquire) == 1)
        return original();
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&identity)))) {
        const HRESULT result = original();
        Failure(-202);
        return result;
    }
    if (!TrackColorSetter(selected_d12_chain.load(std::memory_order_acquire),
                          reinterpret_cast<std::uintptr_t>(identity.Get())))
        return original();
    if (resizing == identity.Get()) {
        // The outer resize/color callback already holds transition admission.
        // Do not recursively wait or infer evidence from a nested setter.
        return ForwardNestedColor(color == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, original,
                                  []() noexcept { Failure(-202); });
    }

    struct Ops {
        DXGI_COLOR_SPACE_TYPE color;
        IDXGISwapChain3* chain;
        std::uintptr_t identity;
        bool tracked{}, selected{};
        LONG reason{-202};

        void RefuseColor(LONG value) noexcept {
            if (reason == -202) reason = value;
        }

        bool BeforeColor(DWORD budget) noexcept {
            // Recheck only while owning transition admission: selection may
            // have occurred while this setter was waiting for an earlier draw.
            const auto current = selected_d12_chain.load(std::memory_order_acquire);
            tracked = TrackColorSetter(current, identity);
            selected = current && current == identity;
            if (!tracked || !selected) return true;
            observed_sdr.store(false, std::memory_order_release);
            return !initialized || renderer->BeforeResize(budget);
        }

        bool AfterColor(HRESULT result) noexcept {
            if (!tracked) return true;
            if (color != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) return false;
            if (failed.load(std::memory_order_acquire) || binding.Ambiguous()) {
                RefuseColor(-271);
                return false;
            }
            // Preselection SDR is not published as evidence for another chain.
            // Preselection PQ/failure, however, conservatively disables overlay.
            if (!selected) return true;
            reason = DiagnoseOutputGeneration(
                true, initialized, SUCCEEDED(result), false,
                [&]() noexcept { return QualifySdrOutput(chain); },
                [&](bool valid) noexcept {
                    return renderer->AfterResize(valid ? result : E_ABORT);
                });
            if (reason == 1) observed_sdr.store(true, std::memory_order_release);
            return reason == 1;
        }

        void AbortColor() noexcept {
            if (!selected) return;
            observed_sdr.store(false, std::memory_order_release);
            if (initialized) renderer->AfterResize(E_ABORT);
        }
    } ops{color, chain, reinterpret_cast<std::uintptr_t>(identity.Get())};

    Clock clock;
    const auto result = ForwardColorTransition(owner, clock, ops, [&]() noexcept {
        auto* previous = resizing;
        resizing = identity.Get();
        const HRESULT value = original();
        resizing = previous;
        return value;
    });
    if (!result.qualified) {
        observed_sdr.store(false, std::memory_order_release);
        Failure(ops.reason); // Close admissions before publishing against pending state.
    }
    return result.original_result;
}

// ─── The seven sites, as one transaction ──────────────────────────────────
//
// This used to be seven transactions. `Sites::Install(i)` was called once per
// site by a staged-attach policy, and each call collected its own thread
// snapshot, took its own page protection and ran its own enlisted-thread
// commit -- so a seven-site attach suspended every thread in the game seven
// times, at roughly 118 ms per thread enumeration, and a refusal at the fifth
// left four installed with nothing tying them together. That last state is
// `PartialResident`, and it is what the install-diagnostic sidecar was built
// to explain.
//
// It is one group now: one enumeration, one suspension, all seven written or
// none. What each piece of the old shape became is written beside it below,
// because this is product code that patches a live render thread and "it still
// compiles" is not the standard.
//
//   seven snapshots + CollectStableSnapshot   -> the engine's prepare phase
//   VirtualProtect / restore around each      -> the engine's prepare/release
//   PreparedStrictPatch + expected bytes      -> .expect_prologue(snapshot)
//   OneInstructionEntry(plan)                 -> OneInstructionEntry(facts, addr)
//   our replacement + originals[i]            -> .replace_with(fn, originals[i])
//   commit_enlisted(plan, slots, pub, ops)    -> .install(splice::live)
//   InstallThreadOps's wrapped Ops seam       -> splice::Failure
//   RecoverEnlisted + its Sleep(1) loop       -> State::recovered
//   threads.Slots().size()                    -> outcome.threads()
//
// The gates are deliberately absent. Our replacements are the hooks and they
// are in the call path themselves, so `.replace_with()` refuses a group that
// also asks for one -- the admission these functions enforce lives in their
// own bodies, where it always did.
struct Sites {
    HookSection& section;
    InstallReport& report;
    std::array<std::uint64_t, 7> addresses;

    // Admission only. Nothing here writes, allocates or suspends, and nothing
    // here touches a splice private type any more: the prologue facts come
    // over the public ABI and the rule over them is still ours, because
    // relaxing a safe-admission rule is a decision recorded on this side.
    bool Prepare(std::size_t i) {
        auto* target = reinterpret_cast<void*>(addresses[i]);
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(target, &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
            memory.Protect != PAGE_EXECUTE_READ ||
            addresses[i] < reinterpret_cast<std::uintptr_t>(memory.BaseAddress) ||
            addresses[i] - reinterpret_cast<std::uintptr_t>(memory.BaseAddress) >
                memory.RegionSize ||
            memory.RegionSize -
                    (addresses[i] - reinterpret_cast<std::uintptr_t>(memory.BaseAddress)) <
                16)
            return false;

        // The profile is a fact about the site, not a precondition of
        // reaching the rule. A site whose function is written cannot be judged
        // without it and refuses; the cell-swapped site does not care whether
        // its prologue decodes, because nothing writes there, and refusing it
        // for a decoder's sake would be the code-generation dependency the
        // cell swap exists to remove.
        splice_prologue_facts facts{};
        const bool profiled = splice_prologue_profile(target, &facts) == 0;
        if (!profiled && PatchedInline(i)) {
            InterlockedExchange(&section.product_ready, -10 - static_cast<LONG>(i));
            return false;
        }

        // Record the shape before deciding anything with it, for every site and
        // not only the two whose admission turns on it. That is the detector:
        // the number that changed under this product had never been measured on
        // a successful attach, so there was nothing to compare a later one
        // against -- and the five sites with no rule over their shape are
        // exactly the ones nothing would have noticed changing. Left at zero
        // when the prologue did not decode, which the protocol defines as not
        // measured.
        if (profiled) {
            auto& shape = section.entry_shape[i];
            InterlockedExchange(&shape.patch_bytes, static_cast<LONG>(facts.patch_bytes));
            InterlockedExchange(&shape.first_instruction,
                                static_cast<LONG>(facts.first_instruction));
            InterlockedExchange(&shape.interior_mask,
                                static_cast<LONG>(facts.interior_mask));
            InterlockedExchange(&shape.entry_padding,
                                static_cast<LONG>(facts.entry_padding));
        }

        // Signal is installed by swapping its vtable cell, so its prologue is
        // not written and no rule about the shape of that prologue applies to
        // it. What it needs instead is for the cell to agree with the method,
        // and that is the whole of its admission.
        if (i == kSignalSite) return SignalCellAdmitted(memory);

        // Present keeps the stricter admission it has always had: the patch
        // window must be one instruction, so no thread can be inside the bytes
        // being replaced. The coordinated tier would map an interior program
        // counter rather than refuse it, which is why this is ours to keep
        // rather than something to drop as redundant.
        //
        // It used to cover Signal too, and Signal is the reason it exists: a
        // runtime update changed that function's register-save encoding from
        // one five-byte instruction to a three-byte one followed by a
        // four-byte one, and this rule refused the install. It still would.
        // Signal left the rule by leaving the tier the rule is about.
        if (i == kPresentSite && !OneInstructionEntry(facts, addresses[i])) {
            InterlockedExchange(&section.product_patch_copy_size,
                                static_cast<LONG>(facts.patch_bytes));
            InterlockedExchange(&section.product_patch_boundaries,
                                static_cast<LONG>(facts.interior_mask));
            InterlockedExchange(&section.product_ready, -100 - static_cast<LONG>(i));
            return false;
        }
        return true;
    }

    // Gather what the rule needs and take the verdict in slot_admission.hpp.
    // `method` is the region the caller already measured for the method
    // address; the cell is measured here.
    bool SignalCellAdmitted(const MEMORY_BASIC_INFORMATION& method) {
        VtableCell cell;
        cell.slot = section.signal_slot_address;
        cell.method = addresses[kSignalSite];
        cell.method_image_base = reinterpret_cast<std::uint64_t>(method.AllocationBase);
        if (cell.slot == 0) {
            InterlockedExchange(&section.product_ready, kSignalCellRefused);
            return false;
        }
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<void*>(cell.slot), &region, sizeof(region)) !=
            sizeof(region)) {
            InterlockedExchange(&section.product_ready, kSignalCellRefused);
            return false;
        }
        cell.cell_region_base = reinterpret_cast<std::uint64_t>(region.BaseAddress);
        cell.cell_region_size = region.RegionSize;
        cell.cell_image_base = reinterpret_cast<std::uint64_t>(region.AllocationBase);
        cell.cell_state = region.State;
        cell.cell_type = region.Type;
        cell.cell_protect = region.Protect;

        // Everything that does not need the cell's contents is decided before
        // the cell is read, because the read is what needs it: a region
        // `VirtualQuery` described is not necessarily a readable page, and a
        // slot that is reserved, guarded or eight bytes short of its region's
        // end would have faulted here, inside the target process, during the
        // install. This used to be one call *after* the load.
        if (!VtableCellMemoryAdmitted(cell)) {
            InterlockedExchange(&section.product_ready, kSignalCellRefused);
            return false;
        }

        // Read it as late as the above allows and through the cell itself, so
        // the value compared is the one a swap would compare-exchange against
        // moments later. The window between them is what the conditional store
        // closes; nothing here can close it, and nothing here pretends to.
        cell.held = *reinterpret_cast<const volatile std::uint64_t*>(cell.slot);
        if (!VtableCellAdmitted(cell)) {
            InterlockedExchange(&section.product_ready, kSignalCellRefused);
            return false;
        }
        return true;
    }

    // The chain. Six different signatures, so it is six `.at()` calls and not
    // a loop -- which is the honest shape: each site names its own type, its
    // own snapshot and its own replacement, and the compiler checks that the
    // replacement matches the site.
    //
    // Signal is not here. It is installed afterwards, by swapping its vtable
    // cell, and the order is deliberate: the failures this group takes are the
    // common ones -- a prologue that has changed, a site somebody else already
    // patched -- so taking them first means the likely refusal happens with no
    // cell written and the module cleanly unloaded. The reverse order leaves a
    // resident module behind the failure that actually occurs.
    splice::Outcome InstallInline(std::size_t count) {
        using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                     DXGI_FORMAT, UINT);
        using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT,
                                                      DXGI_FORMAT, UINT, const UINT*,
                                                      IUnknown* const*);
        using FullscreenFn =
            HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
        using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                                       const DXGI_PRESENT_PARAMETERS*);
        using ColorSpaceFn =
            HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);

        const auto at = [this](std::size_t i) noexcept {
            return reinterpret_cast<void*>(addresses[i]);
        };

        splice::Live<> group;
        group.at(at(0), PresentFn{})
            .expect_prologue(section.expected)
            .replace_with(&Present, originals[0]);
        if (count > 1) {
            group.at(at(2), ResizeFn{})
                .expect_prologue(section.product_expected[0])
                .replace_with(&Resize, originals[2]);
            group.at(at(3), Resize1Fn{})
                .expect_prologue(section.product_expected[1])
                .replace_with(&Resize1, originals[3]);
            group.at(at(4), FullscreenFn{})
                .expect_prologue(section.product_expected[2])
                .replace_with(&Fullscreen, originals[4]);
            group.at(at(5), Present1Fn{})
                .expect_prologue(section.product_expected[3])
                .replace_with(&Present1, originals[5]);
            group.at(at(6), ColorSpaceFn{})
                .expect_prologue(section.product_expected[4])
                .replace_with(&ColorSpace, originals[6]);
        }
        return group.install(splice::live);
    }

    // One cell, conditionally. `.expect` is not decoration here: it is the
    // admission rule taken again at the instant of the write, as a
    // compare-exchange, so a cell another overlay claimed between Prepare and
    // this call is a refusal rather than a silent eviction -- which would also
    // publish an original from before they arrived, and calling it would skip
    // their hook.
    //
    // No thread is suspended for this and none needs to be. One aligned
    // pointer-sized store has no interior state for a thread to observe, which
    // is the property the prologue rule exists to establish for the tier that
    // writes code.
    splice::Outcome SwapSignalCell() {
        using SignalFn =
            HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
        splice::Live<> group;
        group.at_slot(reinterpret_cast<void**>(section.signal_slot_address), SignalFn{})
            .expect(reinterpret_cast<void*>(addresses[kSignalSite]))
            .replace_with(&Signal, originals[kSignalSite]);
        return group.install(splice::slot_swap);
    }
};

// What the transaction did, in the sidecar's own fields.
//
// The layout does not move: `transaction_stage` and `transaction_outcome` are
// still DWORDs in a 76-byte InstallSite, so every size assertion still holds.
// **The values changed domain**, which is why the diagnostics version is
// bumped in the same change: the old outcome enumerated refused/installed/
// recovery_required as 0/1/2 and the new one enumerates installed/refused/
// recovered/lost as 0/1/2/3, so the most important value is inverted. A reader
// decoding the old numbers would be silently wrong, and the version is what
// makes it refuse instead.
//
// Per-site evidence survives; per-site transactions do not. Every site of one
// group reports that group's stage and outcome, and the failing one is named
// by the group's own ordinal.
//
// `sites` is the report index of each ordinal, because the two stopped being
// the same number when Signal left the inline group. A group's ordinal `k` is
// `sites[k]`; passing the mapping in rather than computing it here is what
// lets the cell swap use the same function for its one site.
void RecordGroupOutcome(HookSection& section, InstallReport& report,
                        const std::size_t* sites, std::size_t count,
                        const splice::Outcome& outcome) noexcept {
    const auto stage = static_cast<DWORD>(outcome.failure().stage);
    const auto state = static_cast<DWORD>(outcome.state());
    const bool installed = outcome.state() == splice::State::installed;

    // The cell swap enlists nobody, so it reports zero threads and must not
    // overwrite what the suspension actually cost. The stage is written by
    // both, in order, so the last group to run is the one described -- which
    // is the one that failed, if either did.
    if (outcome.threads() != 0) section.reserved = outcome.threads();
    InterlockedExchange(&section.transaction_stage, static_cast<LONG>(stage));

    for (std::size_t k = 0; k < count; ++k) {
        auto& site = report.sites[sites[k]];
        site.threads = outcome.threads();
        site.transaction_entered = 1;
        site.transaction_stage = stage;
        site.transaction_outcome = state;
        site.state = installed ? 3 : (outcome.state() == splice::State::refused ? 4 : 5);
    }

    if (installed) {
        report.failed_site = MAXDWORD;
        return;
    }

    // One transaction, so the ordinal is the group's and it localises the
    // refusal without implying that any other site was touched.
    const auto ordinal = static_cast<std::size_t>(outcome.failure().site.ordinal);
    report.failed_site = static_cast<DWORD>(ordinal < count ? sites[ordinal] : sites[0]);
    RecordInstallError(report.sites[report.failed_site].primary,
                       InstallOperation::Transaction, stage, outcome.failure().os_error,
                       outcome.failure().os_error != 0,
                       outcome.failure().thread_id);
}

// `lost` is the one outcome a caller has to decide something about: the
// threads may still be suspended and this module must not be unloaded. It maps
// to the state the product already had for exactly that, and `recovered` maps
// to a clean refusal because the writes were undone before anything resumed --
// which is what `RecoverEnlisted` used to spin trying to achieve.
AttachResult AttachResultFor(const splice::Outcome& outcome) noexcept {
    switch (outcome.state()) {
        case splice::State::installed: return AttachResult::kActive;
        case splice::State::recovered: return AttachResult::kRefused;
        case splice::State::lost:      return AttachResult::kRecoveryRequired;
        default:                       return AttachResult::kRefused;
    }
}

HookState Install(HookSection& section, InstallReport& report) {
    BOOL critical = TRUE;
    if (!IsProcessCritical(GetCurrentProcess(), &critical) || critical ||
        !ValidProductColorContract(section, GetCurrentProcessId(),
                                   InstallCreationTime(GetCurrentProcess())))
        return HookState::kRefused;
    counters = &section;
    session_color_policy = static_cast<ColorPolicy>(section.product_color_policy);
    const bool d12 = section.signal_address != 0;
    std::unique_ptr<D3d12BitmapRenderer> prepared;
    if (d12) {
        prepared = std::make_unique<D3d12BitmapRenderer>();
        if (!prepared->Prepare()) return HookState::kRefused;
        renderer = prepared.get();
    }
    const std::size_t count = d12 ? 7U : 1U;
    report.site_count = static_cast<DWORD>(count);
    Sites sites{section,
                report,
                {section.present_address, section.signal_address, section.product_methods[0],
                 section.product_methods[1], section.product_methods[2], section.product_methods[3],
                 section.product_methods[4]}};
    for (std::size_t i = 0; i < count; ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (sites.addresses[i] == sites.addresses[j]) {
                renderer = nullptr;
                return HookState::kRefused;
            }

    // Admission for every site before anything is touched, which is the one
    // thing the staged policy did that a transaction still has to do: a
    // refusal here has written nothing and suspended nobody. What follows it
    // used to be a second loop over the same sites, each with its own
    // suspension. It is one call now.
    AttachOutcome result{AttachResult::kRefused, 0};
    if (active.load(std::memory_order_acquire)) {
        result = {AttachResult::kRecoveryRequired, 0};
    } else {
        bool admitted = true;
        for (std::size_t i = 0; i < count && admitted; ++i) {
            report.failed_site = static_cast<DWORD>(i);
            admitted = sites.Prepare(i);
            report.sites[i].state = admitted ? 1 : 4;
            if (!admitted) {
                RecordInstallError(report.sites[i].primary, InstallOperation::Prepare);
            } else {
                report.failed_site = MAXDWORD;
            }
        }
        if (admitted) {
            // Nothing disables this. The trampolines are leaked for the
            // process's life per FR-013 and the module is retained on every
            // non-refusal, exactly as before -- so the outcome's records are
            // dropped here rather than stored, which is what the old path did
            // by never keeping one.
            const std::size_t inline_count = d12 ? std::size(kInlineSites) : 1U;
            const splice::Outcome outcome = sites.InstallInline(inline_count);
            RecordGroupOutcome(section, report, kInlineSites, inline_count, outcome);
            AttachResult mapped = AttachResultFor(outcome);

            // The cell swap only happens if the code is already patched, which
            // is what makes the ordering matter: everything above can still
            // refuse with nothing written anywhere.
            if (mapped == AttachResult::kActive && d12) {
                const splice::Outcome cell = sites.SwapSignalCell();
                RecordGroupOutcome(section, report, &kSignalSite, 1, cell);
                if (cell.state() != splice::State::installed) {
                    // Six sites are live and the seventh never will be, so the
                    // queue never binds and D3D12 never draws. That is neither
                    // active nor a refusal: a refusal unloads this module, and
                    // patched code is executing in it. `kPartialResident` is
                    // the state the protocol already carries for exactly this,
                    // and the inspection classifier already declines to draw
                    // on it while the attach side keeps the module.
                    mapped = AttachResult::kPartialResident;
                }
            }

            const std::size_t live = mapped == AttachResult::kActive ? count
                                     : mapped == AttachResult::kPartialResident
                                         ? std::size(kInlineSites)
                                         : 0;
            result = {mapped, live};
            if (mapped == AttachResult::kActive) {
                active.store(true, std::memory_order_release);
            }
        }
    }
    FinishInstallReport(report, result);
    if (result.result != AttachResult::kRefused)
        prepared.release();
    else
        renderer = nullptr;
    switch (result.result) {
        case AttachResult::kActive:
            return HookState::kInstalled;
        case AttachResult::kPartialResident:
            return HookState::kPartialResident;
        case AttachResult::kRecoveryRequired:
            return HookState::kRecoveryRequired;
        default:
            return HookState::kRefused;
    }
}

DWORD WINAPI Worker(void* argument) noexcept {
    const auto self = static_cast<HMODULE>(argument);
    wchar_t name[64]{};
    SectionName(GetCurrentProcessId(), name);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    auto* section = mapping ? static_cast<HookSection*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS,
                                                                      0, 0, sizeof(HookSection)))
                            : nullptr;
    if (section) {
        section->header.module_base = reinterpret_cast<std::uintptr_t>(self);
        InstallDiagnosticMapping diagnostic;
        const bool diagnostic_available =
            diagnostic.OpenWriter(GetCurrentProcessId(), InstallCreationTime(GetCurrentProcess()));
        InstallReport report{};
        HookState result = HookState::kRefused;
        try {
            if (PrepareDrawDiagnostics()) result = Install(*section, report);
        } catch (...) {
            if (report.failed_site < report.site_count)
                RecordInstallError(report.sites[report.failed_site].primary,
                                   InstallOperation::Exception);
            Failure();
        }
        // Outside the enlisted-thread window; terminal evidence never activates hooks.
        if (diagnostic_available) PublishInstallReport(*diagnostic.Data(), report);
        InterlockedExchange(&section->header.state, static_cast<LONG>(result));
        if (result != HookState::kRefused) {
            if (diagnostic_available) diagnostic.RetainUntilProcessExit();
            return 0;
        }
        UnmapViewOfFile(section);
        CloseDrawDiagnostics();
    }
    if (mapping) CloseHandle(mapping);
    FreeLibraryAndExitThread(self, 0);
}
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    HANDLE thread = CreateThread(nullptr, 0, Worker, self, 0, nullptr);
    if (!thread) return FALSE;
    CloseHandle(thread);
    return TRUE;
}
