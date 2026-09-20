#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace gtg::fps {

// The first product milestone has only been validated with a D3D11 DXGI
// target. DXGI Present events alone do not identify the rendering API.
[[nodiscard]] constexpr bool FirstStageD3D11Candidate(
    const bool has_d3d11, const bool has_d3d12) noexcept {
    return has_d3d11 && !has_d3d12;
}

// Module presence is only a DXGI-capability hint, not evidence of the active
// renderer. A bounded mixed-module D3D12/WARP ledger matched the same DXGI
// Present surface/rate. Numeric FPS still requires matched events and the
// RateTracker's dominant-surface, freshness and loss confidence gates.
[[nodiscard]] constexpr bool ValidatedDxgiCandidate(
    const bool has_d3d11, const bool has_d3d12) noexcept {
    return has_d3d11 || has_d3d12;
}

struct RawDxgiEvent {
    std::uint16_t id{};
    std::uint8_t version{};
    std::uint32_t thread_id{};
    const std::uint8_t* payload{};
    std::size_t payload_size{};
    std::uint64_t timestamp_us{};
};

struct MatchedPresent {
    std::uint64_t surface{};
    std::uint64_t timestamp_us{};
};

// Matches the version-0 Microsoft-Windows-DXGI Present Start(42) and Stop(43)
// schema. Rejecting unknown versions is safer than guessing payload offsets.
// The payload layout is validated against the ETW manifest/trace in the FPS
// research change; only successful, non-test calls become samples.
class DxgiPresentMatcher {
public:
    [[nodiscard]] std::optional<MatchedPresent> OnEvent(
        const RawDxgiEvent& event) noexcept {
        if (event.version != 0 || event.payload == nullptr) return std::nullopt;
        if (event.id == 42) {
            if (event.payload_size < sizeof(std::uint64_t) + sizeof(std::uint32_t))
                return std::nullopt;
            std::uint64_t surface{};
            std::uint32_t flags{};
            std::memcpy(&surface, event.payload, sizeof(surface));
            std::memcpy(&flags, event.payload + sizeof(surface), sizeof(flags));
            Pending* slot = nullptr;
            for (auto& item : pending_) {
                if (item.thread_id == event.thread_id) {
                    slot = &item;
                    break;
                }
                if (item.thread_id == 0 && slot == nullptr) slot = &item;
            }
            if (slot == nullptr) {
                overflow_ = true;
                return std::nullopt;
            }
            slot->thread_id = event.thread_id;
            slot->surface = surface;
            slot->timestamp_us = event.timestamp_us;
            slot->eligible = surface != 0 && (flags & 1U) == 0;
            return std::nullopt;
        }
        if (event.id == 43) {
            if (event.payload_size < sizeof(std::int32_t)) return std::nullopt;
            std::int32_t result{};
            std::memcpy(&result, event.payload, sizeof(result));
            for (auto& item : pending_) {
                if (item.thread_id != event.thread_id) continue;
                const auto matched = item;
                item = {};
                if (matched.eligible && result == 0 &&
                    event.timestamp_us >= matched.timestamp_us) {
                    return MatchedPresent{matched.surface, matched.timestamp_us};
                }
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool TakeOverflow() noexcept {
        const bool result = overflow_;
        overflow_ = false;
        return result;
    }

    void Reset() noexcept {
        pending_ = {};
        overflow_ = false;
    }

private:
    struct Pending {
        std::uint32_t thread_id{};
        std::uint64_t surface{};
        std::uint64_t timestamp_us{};
        bool eligible{};
    };
    std::array<Pending, 32> pending_{};
    bool overflow_{};
};

}  // namespace gtg::fps
