#include "backend_selection.hpp"
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>

using namespace gtg::overlay::integration;
using Microsoft::WRL::ComPtr;
void Check(bool result, const char* message) {
    if (!result) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void Hr(HRESULT result, const char* message) {
    if (FAILED(result)) {
        std::fprintf(stderr, "HRESULT=0x%08lX ", static_cast<unsigned long>(result));
        Check(false, message);
    }
}
struct Window {
    HWND handle = CreateWindowExW(0, L"STATIC", L"GTG backend test", WS_POPUP,
                                 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Window() { if (handle) DestroyWindow(handle); }
};
void Native() {
    Window window11, window12;
    Check(window11.handle && window12.handle, "owned hidden windows created");
    ComPtr<IDXGIFactory4> factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "owned factory");
    ComPtr<IDXGIAdapter> warp;
    Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    Hr(D3D11CreateDevice(warp.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                        D3D11_SDK_VERSION, &device11, nullptr, &context11), "WARP D3D11");
    ComPtr<ID3D12Device> device12;
    Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)),
       "WARP D3D12");
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    Hr(device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "owned DIRECT queue");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = desc.Height = 64;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain11, chain12;
    Hr(factory->CreateSwapChainForHwnd(device11.Get(), window11.handle, &desc,
                                     nullptr, nullptr, &chain11), "D3D11 chain");
    Hr(factory->CreateSwapChainForHwnd(queue.Get(), window12.handle, &desc,
                                     nullptr, nullptr, &chain12), "D3D12 chain");
    Check(GetModuleHandleW(L"d3d11.dll") && GetModuleHandleW(L"d3d12.dll"),
          "both runtime modules loaded in same owned process");
    Check(InspectBackend(chain11.Get()) == Backend::D3d11,
          "actual D3D11 chain selected with D3D12 module also loaded");
    Check(InspectBackend(chain12.Get()) == Backend::D3d12,
          "actual D3D12 chain selected with D3D11 module also loaded");
    // Cross-check canonical identity without exposing it to selection logic.
    ComPtr<ID3D12Device> observed;
    Hr(chain12->GetDevice(IID_PPV_ARGS(&observed)), "independent chain device oracle");
    ComPtr<IUnknown> expected_id, observed_id;
    Hr(device12.As(&expected_id), "original device identity");
    Hr(observed.As(&observed_id), "observed device identity");
    Check(expected_id.Get() == observed_id.Get(), "chain returns original native device");
    std::puts("PASS native WARP DX11/DX12 actual-chain selection with both modules loaded");
}
int main() {
    constexpr auto absent = Capability::Absent;
    constexpr auto present = Capability::Present;
    constexpr auto error = Capability::Error;
    Check(QueryCapability(S_OK, true) == present, "non-null successful query");
    Check(QueryCapability(S_OK, false) == error, "success without pointer is invalid");
    Check(QueryCapability(E_NOINTERFACE, false) == absent, "explicit absence accepted");
    Check(QueryCapability(E_NOINTERFACE, true) == error, "failed query returning pointer refused");
    Check(QueryCapability(E_FAIL, false) == error, "arbitrary failure is not absence");
    Check(SelectBackend({present, absent, absent}) == Backend::D3d11, "native11 capability selects11");
    Check(SelectBackend({absent, present, absent}) == Backend::D3d12, "native12 capability selects12");
    Check(SelectBackend({present, present, absent}) == Backend::Ambiguous, "both interfaces refused");
    Check(SelectBackend({absent, absent, absent}) == Backend::Unsupported, "neither API refused");
    Check(SelectBackend({present, absent, present}) == Backend::Unsupported, "D3D11On12 refused");
    Check(SelectBackend({error, present, absent}) == Backend::QueryFailed, "query error is not absence");
    Check(SelectBackend({present, error, absent}) == Backend::QueryFailed, "no fallback after12 error");
    Check(SelectBackend({present, absent, error}) == Backend::QueryFailed, "interop query error refused");
    Check(InspectBackend(nullptr) == Backend::QueryFailed, "null chain safely refused");
    Native();
}
