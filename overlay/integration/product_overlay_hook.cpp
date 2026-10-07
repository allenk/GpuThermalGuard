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
#include "../injector/snapshot_slots.hpp"
#include "../injector/snapshot_retry.hpp"
#include "../injector/install_thread_ops.hpp"
#include "../injector/install_policy.hpp"
#include "os/win32/thread_transaction.h"
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
using namespace splice::os::win32;
std::array<std::atomic<void*>, 7> originals{};
std::atomic<bool> active{false};
HookSection* counters{};
std::atomic<bool> failed{false};
std::atomic<std::uintptr_t> selected_d12_chain{};
std::atomic<LONG> selected_backend{0};
std::atomic<bool> observed_sdr{false};
ColorPolicy session_color_policy{ColorPolicy::Strict}; // Worker writes before active release; never mutable thereafter.
QueueBinding binding;
D3d12BitmapRenderer* renderer{}; // Intentionally resident, never destroyed in DllMain.
D3d12LifecycleGate owner;
bool initialized{}; // Access only under owner admission.
struct Clock { UINT64 Now() noexcept { return GetTickCount64(); } void Pause() noexcept { Sleep(1); } };
struct Context { IUnknown* chain; IUnknown* device; };
thread_local Context* context{};
thread_local unsigned recursion{};
thread_local IUnknown* resizing{};
template<class T> T Original(unsigned index) noexcept { return reinterpret_cast<T>(originals[index].load(std::memory_order_acquire)); }
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
        RecordInitialRefusal(counters->product_ready, InitialRefusal::PresentMethod); return false;
    }
    for (unsigned i = 0; i < 5; ++i) if (reinterpret_cast<std::uint64_t>(table[indices[i]]) != counters->product_methods[i]) {
        RecordInitialRefusal(counters->product_ready, static_cast<InitialRefusal>(-241 - static_cast<LONG>(i))); return false;
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
            if (FAILED(chain->GetDesc(&desc)) || !desc.OutputWindow || !IsWindow(desc.OutputWindow)) return false;
            window = desc.OutputWindow; monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
            return monitor != nullptr;
        }
        bool Create() noexcept { return SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); }
        bool Current() noexcept { return factory->IsCurrent() != FALSE; }
        HRESULT Adapter(UINT index) noexcept { return factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()); }
        HRESULT Output(UINT index) noexcept { return adapter->EnumOutputs(index, output.ReleaseAndGetAddressOf()); }
        bool OutputDescription() noexcept { return SUCCEEDED(output->GetDesc(&basic)); }
        bool MatchesMonitor() noexcept { return basic.AttachedToDesktop && basic.Monitor == monitor; }
        LONG Find() noexcept { return FindFreshMonitorOutput(*this); }
        bool Output6() noexcept { return SUCCEEDED(output.As(&output6)); }
        bool Description() noexcept { return SUCCEEDED(output6->GetDesc1(&description)); }
        bool Sdr() noexcept { return description.AttachedToDesktop && description.Monitor == monitor &&
            description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709; }
        bool SameMonitor() noexcept { return IsWindow(window) && MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == monitor; }
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
            SUCCEEDED(public_queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device.As(&device_id))) {
            struct RetainOps { void Retain(void* queue, void* chain) noexcept {
                static_cast<ID3D12CommandQueue*>(queue)->AddRef(); static_cast<IUnknown*>(chain)->AddRef();
            } } retain;
            const QueueObservation observation{public_queue.Get(), context->chain, reinterpret_cast<std::uintptr_t>(queue_id.Get()),
                reinterpret_cast<std::uintptr_t>(context->chain), reinterpret_cast<std::uintptr_t>(device_id.Get()),
                reinterpret_cast<std::uintptr_t>(context->device), recursion, public_queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT};
            const auto result = binding.Offer(observation, retain);
            if (result == BindingResult::kBound) {
                InterlockedExchange64(&counters->queue_identity, static_cast<LONG64>(observation.queue_id));
                InterlockedIncrement64(&counters->nested_signal_calls);
            }
        } else binding.MarkAmbiguous();
        if (binding.Ambiguous()) InterlockedExchange(&counters->queue_ambiguous, 1);
    }
    const HRESULT result = Original<Fn>(1)(queue, fence, value);
    if (FAILED(result)) InterlockedIncrement(&counters->failures);
    return result;
}
// Return only after releasing renderer admission. Original Present owns TLS
// evidence but never our drawing gate, including recursively hooked Signal.
void Render(IDXGISwapChain* chain, UINT flags, Context& captured,
    Microsoft::WRL::ComPtr<IUnknown>& chain_id, Microsoft::WRL::ComPtr<IUnknown>& device_id) noexcept {
    if (!active.load(std::memory_order_acquire) || failed.load(std::memory_order_acquire) || flags & DXGI_PRESENT_TEST || recursion) return;
    const auto backend = InspectBackend(chain);
    if (backend == Backend::D3d11) {
        // Auxiliary D11 windows must never change a selected D12 owner's
        // backend and accidentally bypass its resize/reference drain.
        LONG expected = 0;
        selected_backend.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
        if (selected_backend.load(std::memory_order_acquire) != 1) return;
        try { RenderAndCheck(chain, *counters, false); } catch (...) { Failure(); return; }
        auto* data = ProductDrawDiagnostics();
        if (data && data->identity_state == 2 && data->on12_result == E_NOINTERFACE) {
            InterlockedExchange(&counters->product_backend, 1); RecordInitialReady(counters->product_ready);
            RecordColorProvenance(counters->product_color_provenance, ColorProvenance::Legacy8);
        }
        return;
    }
    const auto refuse = [](InitialRefusal reason) noexcept {
        RecordInitialRefusal(counters->product_ready, reason); Failure();
    };
    if (backend != Backend::D3d12) { refuse(InitialRefusal::Backend); return; }
    if (!renderer) { refuse(InitialRefusal::Renderer); return; }
    if (!counters->signal_address) { refuse(InitialRefusal::Signal); return; }
    if (selected_backend.load(std::memory_order_acquire) == 1) return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> chain3;
    DXGI_SWAP_CHAIN_DESC desc{}; DWORD pid{};
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain3)))) { refuse(InitialRefusal::Chain3); return; }
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&chain_id)))) { refuse(InitialRefusal::ChainIdentity); return; }
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device)))) { refuse(InitialRefusal::Device); return; }
    if (FAILED(device.As(&device_id))) { refuse(InitialRefusal::DeviceIdentity); return; }
    const HRESULT description_result = chain->GetDesc(&desc);
    // Publish initial facts before qualification, including rejected formats.
    // product_ready remains the acceptance oracle, not identity_state alone.
    auto* initial_data = ProductDrawDiagnostics();
    if (initial_data && InterlockedCompareExchange(&initial_data->identity_state, 1, 0) == 0) {
        initial_data->chain = reinterpret_cast<std::uintptr_t>(chain_id.Get());
        initial_data->device = reinterpret_cast<std::uintptr_t>(device_id.Get());
        initial_data->output_window = reinterpret_cast<std::uintptr_t>(desc.OutputWindow);
        initial_data->width = desc.BufferDesc.Width; initial_data->height = desc.BufferDesc.Height;
        initial_data->format = desc.BufferDesc.Format; initial_data->swap_effect = desc.SwapEffect;
        initial_data->desc_result = description_result; initial_data->on12_result = E_NOINTERFACE;
        initial_data->thread = GetCurrentThreadId();
        InterlockedExchange(&initial_data->identity_state, 2);
    }
    if (FAILED(description_result)) { refuse(InitialRefusal::Description); return; }
    if (!MatchesMethods(chain3.Get())) { Failure(); return; }
    if (!GetWindowThreadProcessId(desc.OutputWindow, &pid)) { refuse(InitialRefusal::Window); return; }
    if (pid != GetCurrentProcessId()) { refuse(InitialRefusal::WindowPid); return; }
    if (device->GetNodeCount() != 1) { refuse(InitialRefusal::NodeCount); return; }
    if (!IsSupportedSdrTarget(desc.BufferDesc.Format)) {
        refuse(InitialRefusal::Format); return;
    }
    captured = {chain_id.Get(), device_id.Get()};
    if (!binding.Ready()) return; // Original Present may produce first evidence.
    if (binding.ChainId() != reinterpret_cast<std::uintptr_t>(chain_id.Get())) { binding.MarkAmbiguous(); Failure(-253); return; }
    LONG expected = 0;
    selected_backend.compare_exchange_strong(expected, 2, std::memory_order_acq_rel);
    if (selected_backend.load(std::memory_order_acquire) != 2) return;
    if (!owner.TryDraw()) return;
    struct Scope { ~Scope() { owner.EndDraw(); } } scope;
    selected_d12_chain.store(reinterpret_cast<std::uintptr_t>(chain_id.Get()), std::memory_order_release);
    auto* data = ProductDrawDiagnostics();
    if (data && InterlockedCompareExchange(&data->identity_state, 1, 0) == 0) {
        data->chain = reinterpret_cast<std::uintptr_t>(chain_id.Get()); data->device = reinterpret_cast<std::uintptr_t>(device_id.Get());
        data->output_window = reinterpret_cast<std::uintptr_t>(desc.OutputWindow); data->width = desc.BufferDesc.Width;
        data->height = desc.BufferDesc.Height; data->format = desc.BufferDesc.Format; data->desc_result = S_OK;
        data->on12_result = E_NOINTERFACE; data->thread = GetCurrentThreadId();
        InterlockedExchange(&data->identity_state, 2);
    }
    InterlockedExchange(&counters->product_backend, 2); RecordInitialReady(counters->product_ready);
    if (InterlockedCompareExchange(&counters->header.release, 0, 0)) return;
    if (!initialized) {
        // No swapchain color-space getter exists. Conservatively refuse an
        // HDR/unknown output rather than assume 8-bit alone proves SDR. Future
        // non-SDR SetColorSpace1 on this chain also permanently stops drawing.
        const LONG output_reason = QualifySdrOutput(chain);
        if (output_reason != 1) {
            Failure(output_reason); return;
        }
        initialized = renderer->Initialize(chain3.Get(), static_cast<ID3D12CommandQueue*>(binding.Queue()));
        if (!initialized) { RecordRuntimeRefusal(counters->product_ready, -201); Failure(); return; }
    }
    // Use the actual renderer generation under owner admission, not a discovery
    // description that may predate resize. Unknown SDR10 is a pending no-draw
    // state, not permission to assume the monitor describes the chain.
    if (failed.load(std::memory_order_acquire)) return;
    const auto color = DecideSdrAdmission(renderer->TargetFormat(), observed_sdr.load(std::memory_order_acquire), session_color_policy);
    RecordColorProvenance(counters->product_color_provenance, color);
    if (color == ColorProvenance::WaitingUnknown) {
#ifndef GTG_RESEARCH_SOLID_RECT
        renderer->ReportTargetMetadata();
#endif
        RecordColorWait(counters->product_ready);
        return;
    }
    if (color == ColorProvenance::None) { RecordRuntimeRefusal(counters->product_ready, -202); Failure(); return; }
    ClearColorWait(counters->product_ready);
#ifdef GTG_RESEARCH_SOLID_RECT
    if (!renderer->DrawSolidRectangle()) { if (renderer->Failed()) Failure(-254); return; }
#else
    if (!renderer->Draw()) { if (renderer->Failed()) Failure(-254); return; }
#endif
    auto& stats = renderer->Stats();
    InterlockedExchange64(&counters->bitmap_uploads, static_cast<LONG64>(stats.uploads.load()));
    InterlockedExchange64(&counters->bitmap_copies, static_cast<LONG64>(stats.copy_attempts.load()));
    InterlockedExchange(&counters->motion_sampled_placement, static_cast<LONG>(renderer->PlacementSerial()));
    InterlockedIncrement(&counters->control_samples[0]);
}
template<class Call> HRESULT WithPresent(IDXGISwapChain* chain, UINT flags, Call&& call) noexcept {
    if (recursion) return call(); // A Present1 implementation may call Present.
    Context captured{};
    Microsoft::WRL::ComPtr<IUnknown> chain_id, device_id;
    InterlockedIncrement64(&counters->hook_calls);
    Render(chain, flags, captured, chain_id, device_id);
    auto* previous = context; context = captured.chain ? &captured : nullptr; ++recursion;
    const HRESULT result = call();
    --recursion; context = previous;
    if (FAILED(result)) InterlockedIncrement(&counters->failures);
    return result;
}
HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* chain, UINT interval, UINT flags) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    return WithPresent(chain, flags, [&] { return Original<Fn>(0)(chain, interval, flags); });
}
HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* chain, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    const bool partial = parameters && (parameters->DirtyRectsCount || parameters->pScrollRect || parameters->pScrollOffset);
    if (partial) Failure(-250);
    return WithPresent(chain, flags, [&] { return Original<Fn>(5)(chain, interval, flags, parameters); });
}
template<class Call> HRESULT WithResize(IDXGISwapChain* chain, Call&& call) noexcept {
    if (!active.load(std::memory_order_acquire) || !selected_d12_chain.load(std::memory_order_acquire)) return call();
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&identity))) ||
        selected_d12_chain.load(std::memory_order_acquire) != reinterpret_cast<std::uintptr_t>(identity.Get())) return call();
    // Native ResizeBuffers/Fullscreen can delegate to another hooked method.
    // The outer callback already drained/released this exact chain generation;
    // nested original forwarding must not steal or close its admission.
    if (resizing == identity.Get()) return call();
    Clock clock;
    const bool owned = owner.BeginTransition(clock);
    // Only this callback's successful BeforeResize may authorize AfterResize.
    const bool drained = owned && (!initialized || renderer->BeforeResize(owner.Remaining(clock)));
    if (!drained) Failure(-251);
    auto* previous = resizing; resizing = identity.Get();
    const HRESULT result = call(); // Always unchanged, even timeout/refusal.
    resizing = previous;
    if (owned) {
        const LONG reason = DiagnoseOutputGeneration(drained, initialized, SUCCEEDED(result),
            binding.Ambiguous() || owner.Disabled() || failed.load(std::memory_order_acquire),
            [&]() noexcept { return QualifySdrOutput(chain); },
            [&](bool valid) noexcept { return renderer->AfterResize(valid ? result : E_ABORT); });
        owner.EndTransition(reason == 1); if (reason != 1) Failure(reason);
    }
    return result;
}
HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* chain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    return WithResize(chain, [&] { return Original<Fn>(2)(chain, count, w, h, format, flags); });
}
HRESULT STDMETHODCALLTYPE Resize1(IDXGISwapChain3* chain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags,
    const UINT* masks, IUnknown* const* queues) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
    return WithResize(chain, [&] {
        if (queues || masks) {
            // Drain the old qualified generation before refusing reassociation.
            binding.MarkAmbiguous(); InterlockedExchange(&counters->queue_ambiguous, 1);
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
    const auto original = [&]() noexcept { return Original<Fn>(6)(chain, color); };
    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (!active.load(std::memory_order_acquire) || failed.load(std::memory_order_acquire) ||
        selected_backend.load(std::memory_order_acquire) == 1) return original();
    if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&identity)))) {
        const HRESULT result = original(); Failure(-202); return result;
    }
    if (!TrackColorSetter(selected_d12_chain.load(std::memory_order_acquire), reinterpret_cast<std::uintptr_t>(identity.Get()))) return original();
    if (resizing == identity.Get()) {
        // The outer resize/color callback already holds transition admission.
        // Do not recursively wait or infer evidence from a nested setter.
        return ForwardNestedColor(color == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, original, []() noexcept {
            Failure(-202);
        });
    }
    struct Ops {
        DXGI_COLOR_SPACE_TYPE color;
        IDXGISwapChain3* chain;
        std::uintptr_t identity;
        bool tracked{}, selected{};
        LONG reason{-202};
        void RefuseColor(LONG value) noexcept { if (reason == -202) reason = value; }
        bool BeforeColor(DWORD budget) noexcept {
            // Recheck only while owning transition admission: selection may
            // have occurred while this setter was waiting for an earlier draw.
            const auto current = selected_d12_chain.load(std::memory_order_acquire);
            tracked = TrackColorSetter(current, identity); selected = current && current == identity;
            if (!tracked || !selected) return true;
            observed_sdr.store(false, std::memory_order_release);
            return !initialized || renderer->BeforeResize(budget);
        }
        bool AfterColor(HRESULT result) noexcept {
            if (!tracked) return true;
            if (color != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) return false;
            if (failed.load(std::memory_order_acquire) || binding.Ambiguous()) {
                RefuseColor(-271); return false;
            }
            // Preselection SDR is not published as evidence for another chain.
            // Preselection PQ/failure, however, conservatively disables overlay.
            if (!selected) return true;
            reason = DiagnoseOutputGeneration(true, initialized, SUCCEEDED(result), false,
                [&]() noexcept { return QualifySdrOutput(chain); },
                [&](bool valid) noexcept { return renderer->AfterResize(valid ? result : E_ABORT); });
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
        auto* previous = resizing; resizing = identity.Get();
        const HRESULT value = original(); resizing = previous; return value;
    });
    if (!result.qualified) {
        observed_sdr.store(false, std::memory_order_release);
        Failure(ops.reason); // Close admissions before publishing against pending state.
    }
    return result.original_result;
}
struct Sites {
    HookSection& section;
    InstallReport& report;
    std::array<splice::arch::x86_64::PreparedStrictPatch, 7> plans;
    std::array<std::uint64_t, 7> addresses;
    bool Prepare(std::size_t i) {
        const std::array<void*, 7> hooks{reinterpret_cast<void*>(&Present), reinterpret_cast<void*>(&Signal),
            reinterpret_cast<void*>(&Resize), reinterpret_cast<void*>(&Resize1), reinterpret_cast<void*>(&Fullscreen),
            reinterpret_cast<void*>(&Present1), reinterpret_cast<void*>(&ColorSpace)};
        auto* target = reinterpret_cast<void*>(addresses[i]); MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(target, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT ||
            memory.Type != MEM_IMAGE || memory.Protect != PAGE_EXECUTE_READ ||
            addresses[i] < reinterpret_cast<std::uintptr_t>(memory.BaseAddress) ||
            addresses[i] - reinterpret_cast<std::uintptr_t>(memory.BaseAddress) > memory.RegionSize ||
            memory.RegionSize - (addresses[i] - reinterpret_cast<std::uintptr_t>(memory.BaseAddress)) < 16) return false;
        const auto* expected = i == 0 ? section.expected : i == 1 ? section.signal_expected : section.product_expected[i-2];
        if (!plans[i].prepare(target, hooks[i], expected, 16)) {
            InterlockedExchange(&section.product_ready, -10-static_cast<LONG>(i)); return false;
        }
        // Preserve the existing Present/Signal admission. Lifecycle methods
        // use the checked coordinator's exact instruction-boundary IP map,
        // not the older single-instruction research profile. Atomic writing
        // alone is not execution exclusion; snapshot/new-thread risk remains.
        if (i < 2 && !OneInstructionEntry(plans[i])) {
            InterlockedExchange(&section.product_patch_copy_size, static_cast<LONG>(plans[i].copy_size()));
            LONG mask = 0;
            for (unsigned offset = 1; offset < plans[i].copy_size(); ++offset) {
                std::uintptr_t mapped{};
                if (plans[i].map_ip(addresses[i]+offset, mapped)) mask |= 1<<offset;
            }
            InterlockedExchange(&section.product_patch_boundaries, mask);
            InterlockedExchange(&section.product_ready, -100-static_cast<LONG>(i)); return false;
        }
        return true;
    }
    SiteResult Install(std::size_t i) {
        auto& site = report.sites[i];
        SnapshotSlots threads;
        struct Collection {
            SnapshotSlots& threads;
            bool Collect(InstallSite& attempt) noexcept { return threads.Collect(0, &attempt); }
            bool Close(InstallSite& attempt) noexcept { return threads.Close(&attempt); }
            bool Absent(DWORD tid, InstallSite& attempt) noexcept { return ThreadAbsentFromFreshSnapshot(tid, attempt); }
        } collection{threads};
        if (!CollectStableSnapshot(collection, &site)) {
            RecordInstallError(site.primary, InstallOperation::Collect);
            threads.Close(&site); return SiteResult::kRefused;
        }
        section.reserved = static_cast<DWORD>(threads.Slots().size());
        auto& plan = plans[i]; DWORD old{};
        if (!VirtualProtect(plan.target(), 8, PAGE_EXECUTE_READWRITE, &old)) {
            RecordInstallError(site.primary, InstallOperation::Protect, 0, GetLastError(), true);
            threads.Close(&site); return SiteResult::kRefused;
        }
        InstallThreadOps ops{site, threads.Slots()};
        ops.publication = &originals[i];
        const auto result = commit_enlisted(plan, threads.Slots(), originals[i], ops);
        site.transaction_entered = 1;
        site.transaction_stage = static_cast<DWORD>(result.stage);
        site.transaction_outcome = static_cast<DWORD>(result.outcome);
        section.transaction_stage = static_cast<LONG>(result.stage);
        if (result.outcome != TransactionOutcome::installed) {
            // Splice exposes terminal recovery stage, not the initiating semantic
            // refusal after failed abort cleanup. Mark that cause as unavailable.
            const bool unknown_cause = ops.Aborting() &&
                (result.stage == TransactionStage::rollback || result.stage == TransactionStage::resume);
            RecordInstallError(site.primary, InstallOperation::Transaction,
                static_cast<DWORD>(result.stage) | (unknown_cause ? kInstallCauseUnavailable : 0U));
        }
        if (result.outcome == TransactionOutcome::recovery_required) {
            plan.retain_storage();
            bool uncertain = false;
            while (!RecoverEnlisted(threads.Slots(), result.stage, originals[i].load() != nullptr, plan.target(), ops, uncertain)) {
                if (uncertain && std::none_of(threads.Slots().begin(), threads.Slots().end(), [](const auto& slot) { return slot.suspended; })) break;
                Sleep(1);
            }
            DWORD ignored{};
            if (!VirtualProtect(plan.target(), 8, old, &ignored))
                RecordInstallError(site.cleanup, InstallOperation::RestoreProtect, 0, GetLastError(), true);
            threads.Close(&site);
            return SiteResult::kRecoveryRequired;
        }
        DWORD ignored{}; const bool protection = VirtualProtect(plan.target(), 8, old, &ignored) != FALSE;
        if (!protection) RecordInstallError(site.cleanup, InstallOperation::RestoreProtect, 0, GetLastError(), true);
        const bool closed = threads.Close(&site);
        if (!protection || !closed) { plan.retain_storage(); return SiteResult::kRecoveryRequired; }
        return result.outcome == TransactionOutcome::installed ? SiteResult::kInstalled : SiteResult::kRefused;
    }
};
HookState Install(HookSection& section, InstallReport& report) {
    BOOL critical = TRUE;
    if (!IsProcessCritical(GetCurrentProcess(), &critical) || critical ||
        !ValidProductColorContract(section, GetCurrentProcessId(), InstallCreationTime(GetCurrentProcess()))) return HookState::kRefused;
    counters = &section;
    session_color_policy = static_cast<ColorPolicy>(section.product_color_policy);
    const bool d12 = section.signal_address != 0;
    std::unique_ptr<D3d12BitmapRenderer> prepared;
    if (d12) { prepared = std::make_unique<D3d12BitmapRenderer>(); if (!prepared->Prepare()) return HookState::kRefused; renderer = prepared.get(); }
    report.site_count = d12 ? 7 : 1;
    Sites sites{section, report, {}, {section.present_address, section.signal_address, section.product_methods[0],
        section.product_methods[1], section.product_methods[2], section.product_methods[3], section.product_methods[4]}};
    for (std::size_t i = 0; i < (d12 ? 7U : 1U); ++i)
        for (std::size_t j = 0; j < i; ++j) if (sites.addresses[i] == sites.addresses[j]) { renderer = nullptr; return HookState::kRefused; }
    RecordedInstallSites recorded{sites, report};
    const auto result = AttachStaged(d12 ? 7 : 1, active, recorded);
    FinishInstallReport(report, result);
    if (result.result != AttachResult::kRefused) prepared.release(); else renderer = nullptr;
    switch (result.result) {
        case AttachResult::kActive: return HookState::kInstalled;
        case AttachResult::kPartialResident: return HookState::kPartialResident;
        case AttachResult::kRecoveryRequired: return HookState::kRecoveryRequired;
        default: return HookState::kRefused;
    }
}
DWORD WINAPI Worker(void* argument) noexcept {
    const auto self = static_cast<HMODULE>(argument);
    wchar_t name[64]{}; SectionName(GetCurrentProcessId(), name);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    auto* section = mapping ? static_cast<HookSection*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HookSection))) : nullptr;
    if (section) {
        section->header.module_base = reinterpret_cast<std::uintptr_t>(self);
        InstallDiagnosticMapping diagnostic;
        const bool diagnostic_available = diagnostic.OpenWriter(GetCurrentProcessId(), InstallCreationTime(GetCurrentProcess()));
        InstallReport report{};
        HookState result = HookState::kRefused;
        try { if (PrepareDrawDiagnostics()) result = Install(*section, report); }
        catch (...) {
            if (report.failed_site < report.site_count)
                RecordInstallError(report.sites[report.failed_site].primary, InstallOperation::Exception);
            Failure();
        }
        // Outside the enlisted-thread window; terminal evidence never activates hooks.
        if (diagnostic_available) PublishInstallReport(*diagnostic.Data(), report);
        InterlockedExchange(&section->header.state, static_cast<LONG>(result));
        if (result != HookState::kRefused) {
            if (diagnostic_available) diagnostic.RetainUntilProcessExit();
            return 0;
        }
        UnmapViewOfFile(section); CloseDrawDiagnostics();
    }
    if (mapping) CloseHandle(mapping);
    FreeLibraryAndExitThread(self, 0);
}
}
BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    HANDLE thread = CreateThread(nullptr, 0, Worker, self, 0, nullptr);
    if (!thread) return FALSE; CloseHandle(thread); return TRUE;
}
