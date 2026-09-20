#pragma once

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

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// A foreground window is an eligible candidate only if its process identity
// can be verified. Having a candidate does not claim the app uses DXGI.
[[nodiscard]] std::optional<Identity> ForegroundCandidate() noexcept;
[[nodiscard]] std::optional<Identity> ProcessIdentity(std::uint32_t pid) noexcept;

}  // namespace gtg::fps
