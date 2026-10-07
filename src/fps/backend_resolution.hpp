#pragma once

#include <cstdint>
#include <unordered_set>

#include "fps/fps_rate.hpp"

namespace gtg::fps {

// Which graphics runtimes a target process has loaded, read from its module
// list. A capability hint, never proof of the active renderer: a program can
// hold a runtime it does not present with (an interop layer, an overlay, a
// launcher), and the Vulkan ICD drags opengl32 in as well.
struct LoadedRuntimes {
    bool d3d9{}, d3d11{}, d3d12{}, vulkan{}, opengl{};
    bool readable{};   // false when the module list was refused
};

// The D3D devices alive in the target, from the D3D11 and D3D12 runtimes' own
// ETW providers (AF-20261004-fps-backend-runtime-evidence). Unlike a module
// list, a live device is something the program made: a measured DX11 game loads d3d12.dll and
// its own Agility SDK yet never creates a D3D12 device.
struct RuntimeDevices {
    bool observed{};           // both providers were enabled for this target
    std::uint32_t d3d11{};
    std::uint32_t d3d12{};
};

enum class D3DRuntime { D3D11, D3D12 };

// Live devices per runtime, from create (3), report (5) and destroy (4)
// events. A device created while the observer was attaching can arrive both as
// created and as reported, so devices are keyed by identity when the event
// carries one. Without an identity the ledger can only count, and a destroy
// never takes a count below zero.
class DeviceLedger {
public:
    void Live(D3DRuntime runtime, std::uint64_t identity) {
        Books& books = For(runtime);
        if (identity == 0) ++books.anonymous;
        else books.known.insert(identity);
    }
    void Destroyed(D3DRuntime runtime, std::uint64_t identity) {
        Books& books = For(runtime);
        if (identity == 0) {
            if (books.anonymous > 0) --books.anonymous;
        } else {
            books.known.erase(identity);
        }
    }
    [[nodiscard]] std::uint32_t Count(D3DRuntime runtime) const noexcept {
        const Books& books = runtime == D3DRuntime::D3D11 ? d3d11_ : d3d12_;
        return static_cast<std::uint32_t>(books.known.size()) + books.anonymous;
    }
    void Clear() noexcept {
        d3d11_ = {};
        d3d12_ = {};
    }

private:
    struct Books {
        std::unordered_set<std::uint64_t> known;
        std::uint32_t anonymous{};
    };
    Books& For(D3DRuntime runtime) noexcept {
        return runtime == D3DRuntime::D3D11 ? d3d11_ : d3d12_;
    }
    Books d3d11_;
    Books d3d12_;
};

// The backend label for a measured reading. The events decide the family -- a
// composition token means a DXGI runtime presented -- and inside that family
// the live devices name it when they can, the module list when they cannot.
[[nodiscard]] constexpr Backend ResolveBackend(const LoadedRuntimes& loaded,
                                               const bool saw_composition_token,
                                               const RuntimeDevices& devices = {}) noexcept {
    // What the program made outranks what it loaded. Only when both providers
    // were listening can "D3D11 only" mean only; a device of each runtime is
    // the case a D3D11 presenter doing D3D12 work also produces, so it is DX.
    if (saw_composition_token && devices.observed && (devices.d3d11 > 0 || devices.d3d12 > 0)) {
        if (devices.d3d11 > 0 && devices.d3d12 > 0) return Backend::DXGI;
        return devices.d3d12 > 0 ? Backend::D3D12 : Backend::D3D11;
    }
    if (!loaded.readable) {
        // The list was refused. The events still know the family, and saying
        // that much is better than saying nothing.
        return saw_composition_token ? Backend::DXGI : Backend::Unknown;
    }
    if (saw_composition_token) {
        // A composition token means a DXGI runtime presented. One D3D runtime
        // loaded names it. Both loaded names neither: a D3D12 program keeps
        // d3d11 for interop or an overlay, and a D3D11 program can hold d3d12
        // just as well -- a measured game presents through D3D11 with d3d12.dll loaded
        // (GetDevice on its swap chain: ID3D11Device 295/295, ID3D12Device
        // 0/295). The module list cannot break that tie, so the label stays at
        // what the events proved, DX -- the AF's weaker claim, not a guess.
        if (loaded.d3d11 && loaded.d3d12) return Backend::DXGI;
        if (loaded.d3d12) return Backend::D3D12;
        if (loaded.d3d11) return Backend::D3D11;
        return Backend::DXGI;
    }
    // No composition token, so the DXGI modules are not the renderer --
    // whatever dragged them in, it was not what put this frame up. Vulkan
    // first: its ICD loads opengl32 as well, measured.
    if (loaded.vulkan) return Backend::Vulkan;
    if (loaded.opengl) return Backend::OpenGL;
    if (loaded.d3d9) return Backend::D3D9;
    return Backend::Unknown;
}

}  // namespace gtg::fps
