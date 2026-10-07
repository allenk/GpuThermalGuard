#include "d3d12_frame_resources.hpp"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <d3d12sdklayers.h>
#include <vector>
#include <thread>

using namespace gtg::overlay::integration;
using Microsoft::WRL::ComPtr;
void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::fflush(stderr);
        ExitProcess(1);  // Owned fixture only; do not unwind possibly pending GPU resources.
    }
}
void Hr(HRESULT result, const char* message) {
    if (FAILED(result)) {
        std::fprintf(stderr, "HRESULT=0x%08lX ", static_cast<unsigned long>(result));
        Check(false, message);
    }
}
void WaitOwned(ID3D12CommandQueue* queue, ID3D12Device* device) {
    // Independent oracle only. Frame policy has no waits; the frame owner now
    // permits a bounded wait solely through DrainForTransition.
    ComPtr<ID3D12Fence> done;
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "oracle fence");
    struct Event {
        HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ~Event() { if (handle) CloseHandle(handle); }
    } event;
    Check(event.handle != nullptr, "oracle event");
    const HRESULT signal = queue->Signal(done.Get(), 1);
    const HRESULT notify = SUCCEEDED(signal) ? done->SetEventOnCompletion(1, event.handle) : E_FAIL;
    const bool completed = SUCCEEDED(signal) && SUCCEEDED(notify) &&
        WaitForSingleObject(event.handle, 5000) == WAIT_OBJECT_0 &&
        done->GetCompletedValue() == 1;
    if (!completed) {
        // Do not destroy a registered event/fence whose GPU lifetime is unknown.
        // This failing owned fixture exits rather than continuing to allocate.
        done.Detach(); queue->AddRef(); device->AddRef(); event.handle = nullptr;
        Check(false, "owned GPU work completes normally");
    }
}
struct Gate {
    ComPtr<ID3D12Fence> fence;
    ~Gate() { if (fence) fence->Signal(1); }
    void Open() { Hr(fence->Signal(1), "release owned queue gate"); }
};
int main() try {
    ComPtr<ID3D12Debug> debug;
    Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "owned debug layer available");
    debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter> warp;
    Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
    ComPtr<ID3D12Device> device;
    Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    D3d12Admission admission(flag);
    Check(static_cast<bool>(admission), "entire owner operation is admitted");
    {
        D3d12FrameResources frames;
        Check(frames.Initialize(queue.Get()), "native private frame resources initialized");
        Gate gate;
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "gate fence");
        // Gate only this owned WARP queue; unblock immediately after assertions.
        // Never inject a stall into hardware, a game or a GTG graphics context.
        Hr(queue->Wait(gate.fence.Get(), 1), "owned deterministic GPU-busy stimulus");
        for (int i = 0; i < D3d12Slots::kSlots; ++i) {
            Check(frames.TryBegin() != nullptr, "idle native allocator/list available");
            Check(frames.Submit(), "empty native command list submits with private fence");
        }
        Check(frames.TryBegin() == nullptr, "busy native allocators are not reset");
        frames.Invalidate();
        Check(!frames.Resume(), "generation cannot retire pending native work");
        gate.Open();
        WaitOwned(queue.Get(), device.Get());
        Check(frames.Resume(), "generation resumes only after private GPU completion");
        Check(frames.TryBegin() != nullptr, "new generation reservation");
        frames.Invalidate();
        Check(!frames.Submit(), "invalidated unexecuted list cannot be submitted");
        Check(frames.Resume(), "canceled old CPU reservation allows resume");
        Check(frames.TryBegin() != nullptr, "completed native allocator reused");
        Check(frames.Cancel(), "unexecuted command list cancellation");
        Check(frames.TryBegin() != nullptr && frames.Submit(), "reusable private resources submit again");
        WaitOwned(queue.Get(), device.Get());
        Check(frames.Close() == Retirement::Released, "proven-complete native resources released");
        Check(frames.TryBegin() == nullptr, "closed owner cannot reinitialize through begin");
    }
    {
        D3d12FrameResources frames;
        Check(frames.Initialize(queue.Get()), "failure fixture initialization");
        Check(frames.TryBegin() != nullptr, "failure fixture private list");
        struct Failure {
            HRESULT operator()(ID3D12CommandQueue*, ID3D12Fence*, UINT64) const noexcept {
                return E_FAIL;
            }
        } failure;
        Check(!frames.Submit(failure), "post-Execute failed Signal is terminal");
        Check(frames.Failed() && frames.Quarantined() == 1, "uncertain native slot quarantined");
        Check(frames.TryBegin() == nullptr, "no submissions after failure");
        Check(frames.DrainForTransition() == DrainResult::Unproven,
              "failed Signal cannot be repaired by a transition wait");
        WaitOwned(queue.Get(), device.Get());
        frames.Invalidate();
        Check(!frames.Resume(), "independent queue completion cannot repair private Signal proof");
        Check(frames.Close() == Retirement::Retained, "uncertain resources retained until process exit");
        Check(frames.Close() == Retirement::Retained, "retirement is idempotent");
    }
    {
        D3d12FrameResources frames;
        Check(frames.Initialize(queue.Get()), "pending-close fixture initialization");
        Gate gate;
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "close gate");
        Hr(queue->Wait(gate.fence.Get(), 1), "pending-close GPU gate");
        Check(frames.TryBegin() != nullptr && frames.Submit(), "successful pending submission");
        Check(frames.Close() == Retirement::Retained, "successful pending work is also retained");
        Check(!frames.Initialize(queue.Get()) && frames.TryBegin() == nullptr,
              "closed retained owner cannot allocate a replacement");
        gate.Open();
        WaitOwned(queue.Get(), device.Get());
        Check(frames.Close() == Retirement::Retained, "retained close never changes after detach");
    }
    {
        D3d12FrameResources frames;
        Check(frames.Initialize(queue.Get()), "transition fixture initialization");
        Check(frames.DrainForTransition(0) == DrainResult::Complete,
              "unsubmitted transition requires no event wait");
        Check(frames.TryBegin() == nullptr && frames.Resume(),
              "transition disables reservations until explicit requalification");
        Gate gate;
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "transition gate");
        Hr(queue->Wait(gate.fence.Get(), 1), "transition pending GPU stimulus");
        Check(frames.TryBegin() != nullptr && frames.Submit(), "transition pending submission");
        std::jthread release([&gate] { Sleep(10); gate.Open(); });
        Check(frames.DrainForTransition() == DrainResult::Complete,
              "transition drains real private GPU fence");
        release.join();
        Check(frames.TryBegin() == nullptr && frames.Resume(), "new generation explicit resume");
        Check(frames.Close() == Retirement::Released, "transition-completed resources released");
    }
    {
        D3d12FrameResources frames;
        Check(frames.Initialize(queue.Get()), "transition timeout fixture initialization");
        Gate gate;
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "timeout gate");
        Hr(queue->Wait(gate.fence.Get(), 1), "timeout owned GPU gate");
        Check(frames.TryBegin() != nullptr && frames.Submit(), "timeout pending submission");
        Check(frames.DrainForTransition(20) == DrainResult::Timeout,
              "real private fence timeout is bounded");
        Check(frames.Failed() && frames.TryBegin() == nullptr && !frames.Resume(),
              "timeout permanently disables new submissions");
        Check(frames.Close() == Retirement::Retained, "pending event and GPU resources retained");
        gate.Open();
        WaitOwned(queue.Get(), device.Get());
        Check(frames.Close() == Retirement::Retained, "later completion does not revive failed owner");
    }
    {
        D3D12_COMMAND_QUEUE_DESC compute_desc{};
        compute_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        ComPtr<ID3D12CommandQueue> compute;
        Hr(device->CreateCommandQueue(&compute_desc, IID_PPV_ARGS(&compute)), "compute refusal fixture");
        D3d12FrameResources frames;
        Check(!frames.Initialize(compute.Get()) && frames.Failed(), "non-DIRECT queue refused");
        Check(frames.Close() == Retirement::Released, "unsubmitted refusal releases safe resources");
    }
    ComPtr<ID3D12InfoQueue> messages;
    Hr(device.As(&messages), "independent debug message oracle");
    for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        Hr(messages->GetMessage(i, nullptr, &size), "debug message size");
        std::vector<unsigned char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        Hr(messages->GetMessage(i, message, &size), "debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            std::fprintf(stderr, "debug: %s\n", message->pDescription);
            Check(false, "zero GPU debug warnings/errors");
        }
    }
    std::puts("PASS native WARP private frame resources: busy, reuse, generations and Signal failure");
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
