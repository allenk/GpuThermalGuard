#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "peer/hosts_view.hpp"
#include "peer/link.hpp"
#include "peer/protocol.hpp"
#include "tray/hosts_bridge.hpp"
#include "tray/osd_compact.hpp"

// The Hosts record as the dashboard draws it: at most four hosts, chosen and
// ordered by the peer module's rule, each with what is needed to draw its
// area, its bars and its frame. Built on the UI thread from the link's table;
// the OSD draws it and decides nothing.
namespace gtg::tray::osd_hosts {

struct Host {
    std::wstring name;
    peer::Presence presence{peer::Presence::NotHeard};
    // Whole seconds since its last state, for "lost 0:42".
    std::uint64_t silent_s{};
    bool alert{};
    // What the host last sent, while it is present (online or late). Nothing
    // is drawn as a reading for a host that is not.
    std::optional<peer::Protection> protection;
    std::array<peer::RecordState, peer::kRecords> records{};

    [[nodiscard]] bool Present() const noexcept {
        return presence == peer::Presence::Online || presence == peer::Presence::Dim;
    }

    friend bool operator==(const Host&, const Host&) = default;
};

struct View {
    std::vector<Host> shown;   // at most four, in the reader's order
    std::size_t paired{};

    [[nodiscard]] bool AnyAlert() const noexcept {
        for (const Host& host : shown) {
            if (host.alert) return true;
        }
        return false;
    }

    friend bool operator==(const View&, const View&) = default;
};

[[nodiscard]] inline View MakeView(const peer::Table& table, const std::uint64_t now_ms) {
    std::vector<peer::Candidate> candidates;
    candidates.reserve(table.hosts.size());
    for (const peer::HostRow& row : table.hosts) {
        const peer::Presence presence = peer::PresenceAt(row.heard, now_ms);
        candidates.push_back(
            {presence, peer::InAlert(presence, row.state ? &*row.state : nullptr)});
    }
    const peer::Shown chosen = peer::ChooseShown(candidates);
    View view;
    view.paired = table.hosts.size();
    for (std::size_t i = 0; i < chosen.count; ++i) {
        const peer::HostRow& row = table.hosts[chosen.index[i]];
        Host host;
        host.name = row.name;
        host.presence = candidates[chosen.index[i]].presence;
        host.alert = candidates[chosen.index[i]].alert;
        if (row.heard.state_ms && now_ms > *row.heard.state_ms) {
            host.silent_s = (now_ms - *row.heard.state_ms) / 1'000;
        }
        if (host.Present() && row.state) {
            host.protection = row.state->protection;
            host.records = row.state->records;
        }
        view.shown.push_back(std::move(host));
    }
    return view;
}

// A host's area is framed the way Tiny's cell is for this machine: amber while
// it holds a warning or safe power, red for a fault -- and red when it vanished
// without a goodbye, because its protection can no longer be confirmed from
// here.
enum class Frame { Quiet, Offline, Amber, Red };

[[nodiscard]] constexpr Frame FrameFor(const Host& host) noexcept {
    if (host.presence == peer::Presence::Lost) return Frame::Red;
    if (!host.Present()) return Frame::Offline;
    if (host.protection == peer::Protection::Fault) return Frame::Red;
    if (host.protection == peer::Protection::Warning ||
        host.protection == peer::Protection::SafePower) {
        return Frame::Amber;
    }
    return Frame::Quiet;
}

// One of the host's records, where the dashboard's record has one.
[[nodiscard]] inline const peer::RecordState* RecordOf(const Host& host,
                                                       const compact::Metric metric) noexcept {
    if (metric == compact::Metric::Hosts) return nullptr;
    return &host.records[static_cast<std::size_t>(hosts::ToWire(metric))];
}

// The records a host shows in the compact cell, from the reader's placement:
// temperature, GPU, VRAM and CPU, in the reader's order.
[[nodiscard]] inline std::vector<compact::Metric> CellMetrics(const compact::Placement& placed) {
    std::vector<compact::Metric> metrics;
    for (int i = 0; i < placed.count; ++i) {
        const compact::Metric metric = placed.cells[static_cast<std::size_t>(i)];
        if (metric == compact::Metric::Temperature || metric == compact::Metric::Gpu ||
            metric == compact::Metric::Vram || metric == compact::Metric::Cpu) {
            metrics.push_back(metric);
        }
    }
    return metrics;
}

// The records a host shows in its lane: every record the reader has placed,
// except Hosts itself.
[[nodiscard]] inline std::vector<compact::Metric> LaneMetrics(const compact::Placement& placed) {
    std::vector<compact::Metric> metrics;
    for (int i = 0; i < placed.count; ++i) {
        const compact::Metric metric = placed.cells[static_cast<std::size_t>(i)];
        if (metric != compact::Metric::Hosts) metrics.push_back(metric);
    }
    return metrics;
}

}  // namespace gtg::tray::osd_hosts
