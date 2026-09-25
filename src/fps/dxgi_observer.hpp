#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "fps/fps_rate.hpp"

namespace gtg::fps {

// Display-outcome ETW observer. Calls from the Tray UI only signal the
// controller; no ETW control/consume API runs on the UI or protection thread.
class DxgiObserver final {
public:
    DxgiObserver();
    ~DxgiObserver();
    DxgiObserver(const DxgiObserver&) = delete;
    DxgiObserver& operator=(const DxgiObserver&) = delete;

    void SetTarget(std::optional<Identity> target) noexcept;
    [[nodiscard]] Snapshot TryRead() const noexcept;
    void Stop() noexcept;

    // What the last finished session actually received, one field per side of
    // the measurement.
    //
    // It exists to be written to the journal. These ETW event ids are not a
    // documented contract, and if a Windows update moves them the only signal
    // today is a reader noticing a dash -- which is not a way to find out.
    // Reported here rather than logged here so that the FPS module keeps no
    // logging dependency and still builds as an isolated target.
    //
    // Taking it clears it, so one session is never journalled twice.
    struct SessionCounts {
        bool valid{};                       // a session ran and stopped
        std::uint64_t displayed_frames{};   // the DXGI/Win32k/DWM chain
        std::uint64_t kernel_presents{};    // DxgKrnl, every API
    };
    [[nodiscard]] SessionCounts TakeLastSessionCounts() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// A foreground window is an eligible candidate only if its process identity
// can be verified. Having a candidate does not claim the app uses DXGI.
[[nodiscard]] std::optional<Identity> ForegroundCandidate() noexcept;
[[nodiscard]] std::optional<Identity> ProcessIdentity(std::uint32_t pid) noexcept;

}  // namespace gtg::fps
