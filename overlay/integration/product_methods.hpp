#pragma once
#include "../offsets/method_report.hpp"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <stdexcept>

namespace gtg::overlay::integration {
inline constexpr const char* kProductRoles[] = {"ResizeBuffers", "ResizeBuffers1",
                                                "SetFullscreenState", "Present1", "SetColorSpace1"};

// All objects/modules remain alive through the helper's attachment call.
struct ProductMethods {
    Microsoft::WRL::ComPtr<IDXGISwapChain> chain;
    Microsoft::WRL::ComPtr<ID3D11Device> device11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> chain3;
    Microsoft::WRL::ComPtr<ID3D12Device> device12;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    HMODULE d11{}, d12{};
    HWND window{};

    ~ProductMethods() {
        queue.Reset();
        device12.Reset();
        chain3.Reset();
        chain.Reset();
        context.Reset();
        device11.Reset();
        if (window) DestroyWindow(window);
        if (d12) FreeLibrary(d12);
        if (d11) FreeLibrary(d11);
    }

    bool Discover(const wchar_t* report) {
        d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!d11) return false;
        const auto create11 = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
            GetProcAddress(d11, "D3D11CreateDeviceAndSwapChain"));
        window = CreateWindowExW(0, L"STATIC", L"GTG overlay discovery", WS_POPUP, 0, 0, 2, 2,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window || !create11) return false;
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = desc.BufferDesc.Height = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(create11(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                            D3D11_SDK_VERSION, &desc, &chain, &device11, nullptr, &context)) ||
            FAILED(chain.As(&chain3)))
            return false;
        auto** table = *reinterpret_cast<void***>(chain3.Get());
        std::vector<research::MethodAddress> methods{
            {"Present", table[8]},         {kProductRoles[0], table[13]},
            {kProductRoles[1], table[39]}, {kProductRoles[2], table[10]},
            {kProductRoles[3], table[22]}, {kProductRoles[4], table[38]}};
        d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto create12 = d12 ? reinterpret_cast<decltype(&D3D12CreateDevice)>(
                                        GetProcAddress(d12, "D3D12CreateDevice"))
                                  : nullptr;
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        if (create12 &&
            SUCCEEDED(create12(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12))) &&
            SUCCEEDED(device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) {
            auto** queue_table = *reinterpret_cast<void***>(queue.Get());
            methods.push_back({"Signal", queue_table[14]});
        }
        return research::WriteMethodReport(report, methods);
    }
};
}
