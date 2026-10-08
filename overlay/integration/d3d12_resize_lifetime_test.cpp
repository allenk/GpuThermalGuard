#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
#include "d3d12_frame_resources.hpp"

using Microsoft::WRL::ComPtr;
using namespace gtg::overlay::integration;

namespace {
void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::fflush(stderr);
        // A failed post-submit experiment may still have pending GPU commands.
        // Exit only this owned process without unwinding resource owners; OS
        // process teardown, not premature Release, ends unknown GPU lifetimes.
        ExitProcess(1);
    }
}

void Hr(HRESULT value, const char* message) {
    if (FAILED(value)) {
        std::fprintf(stderr, "HRESULT=0x%08lX ", static_cast<unsigned long>(value));
        Check(false, message);
    }
}

struct Window {
    HWND handle = CreateWindowExW(0, L"STATIC", L"GTG owned resize lifetime test", WS_POPUP, 0, 0,
                                  64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    ~Window() {
        if (handle) DestroyWindow(handle);
    }
};

struct Event {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ~Event() {
        if (handle) CloseHandle(handle);
    }
};

void WaitOwned(ID3D12Fence* fence, UINT64 value) {
    Event event;
    Check(event.handle != nullptr, "owned completion event");
    const HRESULT notification = fence->SetEventOnCompletion(value, event.handle);
    if (FAILED(notification) || WaitForSingleObject(event.handle, 5000) != WAIT_OBJECT_0 ||
        fence->GetCompletedValue() != value) {
        // The failing fixture exits. Never close a potentially registered event.
        fence->AddRef();
        event.handle = nullptr;
        Check(false, "owned completion proof");
    }
}

struct Gate {
    ComPtr<ID3D12Fence> fence;

    ~Gate() {
        if (fence) fence->Signal(1);
    }

    void Open() { Hr(fence->Signal(1), "open owned WARP queue gate"); }
};

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* buffer, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return barrier;
}

void NoD3d12Warnings(ID3D12Device* device) {
    ComPtr<ID3D12InfoQueue> messages;
    Hr(device->QueryInterface(IID_PPV_ARGS(&messages)), "debug oracle");
    for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        Hr(messages->GetMessage(i, nullptr, &size), "debug size");
        std::vector<unsigned char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        Hr(messages->GetMessage(i, message, &size), "debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            std::fprintf(stderr, "%s\n", message->pDescription);
            Check(false, "no D3D12 lifetime warnings/errors");
        }
    }
}
}  // namespace

int main() {
    // This is an API-contract experiment, not a product resize implementation.
    // All GPU gates, waits and expected invalid calls belong to this owned WARP
    // process. No game, GTG core context or hardware queue is touched.
    ComPtr<ID3D12Debug> debug;
    Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug layer required");
    debug->EnableDebugLayer();
    Window window;
    Check(window.handle != nullptr, "owned hidden window");
    ComPtr<IDXGIFactory4> factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    ComPtr<IDXGIAdapter> warp;
    Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
    ComPtr<ID3D12Device> device;
    Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Hr(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "queue");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 64;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferCount = 2;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain;
    Hr(factory->CreateSwapChainForHwnd(queue.Get(), window.handle, &desc, nullptr, nullptr, &chain),
       "owned chain");
    Hr(chain->ResizeBuffers(0, 64, 64, DXGI_FORMAT_UNKNOWN, 0),
       "baseline resize with no external backbuffer references");

    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    D3d12Admission admission(flag);
    Check(static_cast<bool>(admission), "test holds external owner admission");
    D3d12FrameResources frames;
    Check(frames.Initialize(queue.Get()), "real private frame owner initialized");
    auto* list = frames.TryBegin();
    Check(list != nullptr, "native backbuffer list reserved without waiting");
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv;
    Hr(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv)), "RTV heap");
    ComPtr<ID3D12Resource> backbuffer;
    Hr(chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "retained overlay backbuffer");
    const auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(backbuffer.Get(), nullptr, handle);
    auto barrier = Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &barrier);
    constexpr float color[] = {0.25f, 0.5f, 0.75f, 1.0f};
    list->ClearRenderTargetView(handle, color, 0, nullptr);
    barrier = Transition(backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(1, &barrier);

    ComPtr<ID3D12Fence> game_done, overlay_done;
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&game_done)), "game fence");
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&overlay_done)), "overlay fence");
    Gate gate;
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "gate");
    Hr(queue->Signal(game_done.Get(), 1), "game's earlier queue marker");
    WaitOwned(game_done.Get(), 1);
    Hr(queue->Wait(gate.fence.Get(), 1), "gate only owned WARP queue");
    Check(frames.Submit(), "actual backbuffer commands use private allocator/fence owner");
    Hr(queue->Signal(overlay_done.Get(), 1), "overlay's later GPU marker");
    Check(game_done->GetCompletedValue() == 1 && overlay_done->GetCompletedValue() == 0,
          "earlier game marker does not prove later overlay completion");

    const HRESULT pending_resize = chain->ResizeBuffers(0, 80, 80, DXGI_FORMAT_UNKNOWN, 0);
    std::printf("pending retained-reference ResizeBuffers=0x%08lX\n",
                static_cast<unsigned long>(pending_resize));
    Check(pending_resize == DXGI_ERROR_INVALID_CALL,
          "retained overlay backbuffer obstructs native resize");
    gate.Open();
    Check(frames.DrainForTransition() == DrainResult::Complete,
          "approved transition proves private backbuffer work complete");
    Check(frames.TryBegin() == nullptr, "old generation cannot submit after transition drain");
    WaitOwned(overlay_done.Get(), 1);
    const HRESULT completed_resize = chain->ResizeBuffers(0, 80, 80, DXGI_FORMAT_UNKNOWN, 0);
    Check(completed_resize == DXGI_ERROR_INVALID_CALL,
          "completed GPU work alone does not release external backbuffer reference");
    // Safe reference release: independent fence proves actual clear completed.
    backbuffer.Reset();
    Check(frames.Close() == Retirement::Released, "completed native frame resources retired");
    rtv.Reset();
    Hr(chain->ResizeBuffers(0, 80, 80, DXGI_FORMAT_UNKNOWN, 0),
       "native resize succeeds after proven completion and reference release");
    DXGI_SWAP_CHAIN_DESC1 after{};
    Hr(chain->GetDesc1(&after), "independent resized descriptor");
    Check(after.Width == 80 && after.Height == 80, "real backbuffer generation changed");
    NoD3d12Warnings(device.Get());
    std::puts("PASS owned WARP resize lifetime experiment; no product policy inferred");
}
