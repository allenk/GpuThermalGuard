#pragma once
#include "chain_handoff.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace gtg::research {
// Dedicated byte-data key, not an interface/debug-name key. Cooperative marker.
inline constexpr GUID kChainStampGuid{
    0x8f97da2b, 0xba21, 0x4375, {0x90, 0x48, 0x59, 0x81, 0xd3, 0x74, 0xe8, 0x25}};

struct ChainReferences {
    IUnknown* chain{};
    IUnknown* device{};
    IUnknown* pending{};

    void ClearPending() noexcept {
        auto* old = pending;
        pending = nullptr;
        if (old) old->Release();
    }
};

template <typename DrawFn, typename InvalidateFn>
struct D3D11ChainCandidate {
    IDXGISwapChain* chain;
    ID3D11Device* device;
    Microsoft::WRL::ComPtr<IUnknown>& chain_id;
    Microsoft::WRL::ComPtr<IUnknown>& device_id;
    ChainReferences& retained;
    DrawFn& draw;
    InvalidateFn& invalidate;
    void (*record)(const HandoffDecision&) noexcept {};

    ChainFacts Inspect() noexcept {
        DXGI_SWAP_CHAIN_DESC desc{};
        BOOL fullscreen{};
        DWORD pid{};
        if (FAILED(chain->GetDesc(&desc)) || !desc.OutputWindow || !desc.BufferDesc.Width ||
            !desc.BufferDesc.Height || FAILED(chain->GetFullscreenState(&fullscreen, nullptr)) ||
            FAILED(device->GetDeviceRemovedReason()))
            return {};
        if (!GetWindowThreadProcessId(desc.OutputWindow, &pid) || pid != GetCurrentProcessId())
            return {};
        return {reinterpret_cast<std::uintptr_t>(desc.OutputWindow),
                true,
                fullscreen != FALSE,
                desc.BufferDesc.Width,
                desc.BufferDesc.Height,
                static_cast<std::uint32_t>(desc.BufferDesc.Format)};
    }

    StampRead ReadStamp() noexcept {
        ChainStamp stamp{};
        UINT size = sizeof(stamp);
        const HRESULT hr = chain->GetPrivateData(kChainStampGuid, &size, &stamp);
        if (hr == DXGI_ERROR_NOT_FOUND) return {StampState::kAbsent, {}, hr, size};
        if (hr != S_OK || size != sizeof(stamp)) return {StampState::kError, {}, hr, size};
        return {StampState::kPresent, stamp, hr, size};
    }

    bool WriteStamp(ChainStamp stamp) noexcept {
        return chain->SetPrivateData(kChainStampGuid, sizeof(stamp), &stamp) == S_OK;
    }

    void Invalidate() noexcept { invalidate(); }

    void ClearPending() noexcept { retained.ClearPending(); }

    void HoldPending() noexcept {
        retained.pending = chain_id.Get();
        retained.pending->AddRef();
    }

    void RecordDecision(const HandoffDecision& value) noexcept {
        if (record) record(value);
    }

    void CommitOwner(bool) noexcept {
        auto* previous = retained.chain;
        retained.chain = chain_id.Detach();
        if (!retained.device) retained.device = device_id.Detach();
        Invalidate();
        // No retained old-chain list; this release may destroy driver resources.
        // The policy's busy guard is still held so reentrant drawing skips.
        if (previous) previous->Release();
    }

    void Draw() noexcept { draw(); }
};
} // namespace gtg::research
