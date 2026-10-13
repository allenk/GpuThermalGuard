#pragma once
#include "../offsets/method_report.hpp"
#include "runtime_core.hpp"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <memory>
#include <stdexcept>

namespace gtg::overlay::integration {
inline constexpr const char* kProductRoles[] = {"ResizeBuffers", "ResizeBuffers1",
                                                "SetFullscreenState", "Present1", "SetColorSpace1"};

// All objects/modules remain alive through the helper's attachment call. For a
// target with its own runtime that includes the matching Core: the factory and
// device keep it loaded, and the attachment resolves the method's owner by the
// exact path this discovery reported.
struct ProductMethods {
    Microsoft::WRL::ComPtr<IDXGISwapChain> chain;
    Microsoft::WRL::ComPtr<ID3D11Device> device11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> chain3;
    Microsoft::WRL::ComPtr<ID3D12Device> device12;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12SDKConfiguration1> sdk_configuration;
    Microsoft::WRL::ComPtr<ID3D12DeviceFactory> device_factory;
    std::unique_ptr<CoreRuntime> core_runtime;
    HMODULE d11{}, d12{};
    HWND window{};

    ~ProductMethods() {
        queue.Reset();
        device12.Reset();
        device_factory.Reset();
        sdk_configuration.Reset();
        chain3.Reset();
        chain.Reset();
        context.Reset();
        device11.Reset();
        if (window) DestroyWindow(window);
        if (d12) FreeLibrary(d12);
        if (d11) FreeLibrary(d11);
    }

    // True when the target runs a D3D12 runtime of its own. Every other target,
    // including one on the system Core, takes the route it always took.
    bool MatchingRuntime() const { return core_runtime && !core_runtime->IsSystem(); }

    bool Discover(const wchar_t* report, Identity target) {
        core_runtime = CoreRuntime::Open(target);
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
        if (MatchingRuntime()) {
            CreateMatchingDevice();
        } else {
            const auto create12 = d12 ? reinterpret_cast<decltype(&D3D12CreateDevice)>(
                                            GetProcAddress(d12, "D3D12CreateDevice"))
                                      : nullptr;
            if (create12) (void)create12(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12));
        }
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        if (device12 &&
            SUCCEEDED(device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) {
            // An address-discovery object only: this queue never receives a
            // command, here or in the game.
            auto** queue_table = *reinterpret_cast<void***>(queue.Get());
            if (MatchingRuntime()) CheckMethodOwner(queue_table[14]);
            methods.push_back({"Signal", queue_table[14]});
        } else if (MatchingRuntime()) {
            // For the system route a missing queue means D3D12 is unavailable,
            // and Signal is left out of the report. A target that runs its own
            // runtime has already proved otherwise, so this is a refusal.
            throw std::runtime_error("matching runtime helper queue unavailable");
        }
        return research::WriteMethodReport(report, methods);
    }

    // Select the target's runtime side by side, for this helper only: no
    // SetSDKVersion, no global factory state, no debug or experimental layers,
    // and no change to what the game selected for itself.
    void CreateMatchingDevice() {
        const auto get_interface = d12 ? reinterpret_cast<decltype(&D3D12GetInterface)>(
                                             GetProcAddress(d12, "D3D12GetInterface"))
                                       : nullptr;
        RequireRuntime(get_interface && SUCCEEDED(get_interface(CLSID_D3D12SDKConfiguration,
                                                                IID_PPV_ARGS(&sdk_configuration))),
                       "matching runtime device factory capability unavailable");
        const auto directory = RuntimeNarrowPath(core_runtime->Path().parent_path()) + "/";
        RequireRuntime(
            SUCCEEDED(sdk_configuration->CreateDeviceFactory(
                core_runtime->SdkVersion(), directory.c_str(), IID_PPV_ARGS(&device_factory))),
            "matching runtime device factory refused");
        RequireRuntime(SUCCEEDED(device_factory->SetFlags(
                           D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON)),
                       "matching runtime independent device policy refused");
        RequireRuntime(SUCCEEDED(device_factory->CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                                              IID_PPV_ARGS(&device12))),
                       "matching runtime helper device refused");
    }

    // The factory was asked for a directory; the image that answered settles
    // what was selected. Requiring it to be the file the target loaded is what
    // makes this address evidence about the target rather than about us.
    void CheckMethodOwner(const void* method) const {
        const auto owner = MethodOwnerPath(method);
        RequireRuntime(owner.has_value(), "matching runtime method owner unresolved");
        RequireRuntime(SameRuntimeFile(*owner, core_runtime->Path()),
                       "matching runtime device factory selected a different Core");
    }
};
}  // namespace gtg::overlay::integration
