#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "peer/protocol.hpp"
#include "tray/osd_compact.hpp"
#include "tray/osd_tiny.hpp"

// Between the dashboard and the link: what this machine sends, built from
// what its own Tiny draws, and where a host's records sit among the reader's.
namespace gtg::tray::hosts {

// The wire's records are its own list; the dashboard's enum may change.
[[nodiscard]] constexpr peer::Record ToWire(const compact::Metric metric) noexcept {
    switch (metric) {
        case compact::Metric::Temperature:
            return peer::Record::Temperature;
        case compact::Metric::Power:
            return peer::Record::Power;
        case compact::Metric::Vram:
            return peer::Record::Vram;
        case compact::Metric::Gpu:
            return peer::Record::Gpu;
        case compact::Metric::Cpu:
            return peer::Record::Cpu;
        case compact::Metric::Ram:
            return peer::Record::Ram;
        case compact::Metric::Fps:
            return peer::Record::Fps;
        case compact::Metric::Network:
            return peer::Record::Network;
        case compact::Metric::Hosts:
            break;
    }
    return peer::Record::Temperature;
}

// What protection reads, in the dashboard's words.
enum class Visual { Neutral, Armed, Warning, Protected, Fault };

[[nodiscard]] constexpr peer::Protection ToWire(const Visual visual) noexcept {
    switch (visual) {
        case Visual::Armed:
            return peer::Protection::Normal;
        case Visual::Warning:
            return peer::Protection::Warning;
        case Visual::Protected:
            return peer::Protection::SafePower;
        case Visual::Fault:
            return peer::Protection::Fault;
        case Visual::Neutral:
            break;
    }
    return peer::Protection::Unknown;
}

struct LocalState {
    tiny::BarInputs bars;
    Visual visual{Visual::Neutral};
    bool stale{};
    std::optional<double> network_received;   // bytes per second
    std::optional<double> network_sent;
    std::array<std::uint8_t, 3> version{};
};

// This machine's state as its paired hosts receive it: every record as its
// own Tiny bar draws it, and the readings. Sent whether or not the reader has
// a record switched on here: the receiver shows the records its own reader
// has on.
[[nodiscard]] inline peer::HostState Build(const LocalState& local) {
    peer::HostState state;
    state.version = local.version;
    state.protection = ToWire(local.visual);
    state.stale = local.stale;
    // Protection is read once, from the visual, so the bars and the
    // protection state sent beside them cannot disagree.
    tiny::BarInputs in = local.bars;
    in.warning = local.visual == Visual::Warning;
    in.safe_power = local.visual == Visual::Protected;
    const auto as_float = [](const std::optional<double> value) -> std::optional<float> {
        if (!value) return std::nullopt;
        return static_cast<float>(*value);
    };
    for (int i = 0; i < compact::kRecordCount; ++i) {
        const auto metric = static_cast<compact::Metric>(i);
        // The hosts are this machine's view of others; they are not sent on.
        if (metric == compact::Metric::Hosts) continue;
        const tiny::BarState bar = tiny::DescribeBar(metric, in);
        peer::RecordState& out = state.records[static_cast<std::size_t>(ToWire(metric))];
        out.level = as_float(bar.level);
        out.tick = as_float(bar.tick);
        out.raised = bar.raised;
        out.alarm = bar.alarm;
        switch (metric) {
            case compact::Metric::Temperature:
                out.value = as_float(in.temperature_c);
                break;
            case compact::Metric::Power:
                out.value = as_float(in.power_w);
                break;
            case compact::Metric::Vram:
                out.value = as_float(in.vram_percent);
                break;
            case compact::Metric::Gpu:
                out.value = as_float(in.gpu_percent);
                break;
            case compact::Metric::Cpu:
                out.value = as_float(in.cpu_percent);
                break;
            case compact::Metric::Ram:
                out.value = as_float(in.ram_percent);
                break;
            case compact::Metric::Fps:
                if (in.fps) out.value = static_cast<float>(in.fps->current);
                break;
            case compact::Metric::Network:
                out.value = as_float(local.network_received);
                out.value2 = as_float(local.network_sent);
                break;
            case compact::Metric::Hosts:
                break;
        }
    }
    return state;
}

}  // namespace gtg::tray::hosts
