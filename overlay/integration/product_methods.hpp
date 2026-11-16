#pragma once
#include "../offsets/method_report.hpp"
#include "runtime_core.hpp"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace gtg::overlay::integration {
inline constexpr const char* kProductRoles[] = {"ResizeBuffers", "ResizeBuffers1",
                                                "SetFullscreenState", "Present1", "SetColorSpace1"};

// A refused discovery names the call and what it returned. Four distinct
// failures used to leave through one bare `return false`, and the caller's one
// generic label was all that reached the log -- so a driver refusing to create
// a device read exactly like an absent interface, and the refusal that matters
// most here is the one an HRESULT identifies.
[[noreturn]] inline void RefuseDiscovery(const char* reason, HRESULT result) {
    char text[192]{};
    std::snprintf(text, sizeof(text), "%s hr=0x%08lX", reason,
                  static_cast<unsigned long>(result));
    throw std::runtime_error(text);
}

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
        RequireRuntime(d11 != nullptr, "discovery Direct3D 11 system module unavailable");
        const auto create11 = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
            GetProcAddress(d11, "D3D11CreateDeviceAndSwapChain"));
        RequireRuntime(create11 != nullptr, "discovery Direct3D 11 device entry point unavailable");
        window = CreateWindowExW(0, L"STATIC", L"GTG overlay discovery", WS_POPUP, 0, 0, 2, 2,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        RequireRuntime(window != nullptr, "discovery window unavailable");
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = desc.BufferDesc.Height = 2;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const auto created =
            create11(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                     &desc, &chain, &device11, nullptr, &context);
        if (FAILED(created)) RefuseDiscovery("discovery device and swap chain refused", created);
        const auto upgraded = chain.As(&chain3);
        if (FAILED(upgraded))
            RefuseDiscovery("discovery swap chain interface unavailable", upgraded);
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
            // Slot fourteen is COM ABI and cannot move, which is the whole
            // reason this site is installed by swapping the cell instead of
            // patching the function: the method's prologue is code generation
            // and changed under us once already. The cell's address goes into
            // the report beside the method's so the attach side can require
            // the two to agree in the target.
            methods.push_back({"Signal", queue_table[14], nullptr, queue_table + 14});
        } else if (MatchingRuntime()) {
            // For the system route a missing queue means D3D12 is unavailable,
            // and Signal is left out of the report. A target that runs its own
            // runtime has already proved otherwise, so this is a refusal.
            throw std::runtime_error("matching runtime helper queue unavailable");
        }
        RequireRuntime(research::WriteMethodReport(report, methods),
                       "discovery method report not written");
        return true;
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
