#pragma once
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace gtg::overlay::integration {
enum class Backend { Unsupported, D3d11, D3d12, Ambiguous, QueryFailed };
enum class Capability { Absent, Present, Error };
struct BackendEvidence {
    Capability d3d11 = Capability::Absent;
    Capability d3d12 = Capability::Absent;
    Capability interop = Capability::Absent;
};
inline Backend SelectBackend(BackendEvidence value) noexcept {
    if (value.d3d11 == Capability::Error || value.d3d12 == Capability::Error ||
        value.interop == Capability::Error) return Backend::QueryFailed;
    if (value.interop == Capability::Present) return Backend::Unsupported;
    if (value.d3d11 == Capability::Present && value.d3d12 == Capability::Present)
        return Backend::Ambiguous;
    if (value.d3d11 == Capability::Present) return Backend::D3d11;
    if (value.d3d12 == Capability::Present) return Backend::D3d12;
    return Backend::Unsupported;
}
inline Capability QueryCapability(HRESULT result, bool returned) noexcept {
    if (SUCCEEDED(result)) return returned ? Capability::Present : Capability::Error;
    // A failed query is not generally proof of an absent interface.
    return result == E_NOINTERFACE && !returned ? Capability::Absent : Capability::Error;
}
inline Backend InspectBackend(IDXGISwapChain* chain) noexcept {
    if (!chain) return Backend::QueryFailed;
    Microsoft::WRL::ComPtr<ID3D11Device> device11;
    Microsoft::WRL::ComPtr<ID3D12Device> device12;
    BackendEvidence evidence;
    const HRESULT result11 = chain->GetDevice(IID_PPV_ARGS(&device11));
    const HRESULT result12 = chain->GetDevice(IID_PPV_ARGS(&device12));
    evidence.d3d11 = QueryCapability(result11, device11.Get() != nullptr);
    evidence.d3d12 = QueryCapability(result12, device12.Get() != nullptr);
    if (evidence.d3d11 == Capability::Present) {
        Microsoft::WRL::ComPtr<ID3D11On12Device> interop;
        const HRESULT result = device11.As(&interop);
        evidence.interop = QueryCapability(result, interop.Get() != nullptr);
    }
    // No module hints, FPS labels, scratch-device identity or queue guesses.
    // The caller must still qualify the selected backend's queue/lifetime.
    return SelectBackend(evidence);
}
}  // namespace gtg::overlay::integration
