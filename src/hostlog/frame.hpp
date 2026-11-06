#pragma once

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "peer/hosts_view.hpp"
#include "peer/protocol.hpp"

// One line of the Hosts log: the hosts table as this machine's dashboard holds
// it, written for a program outside GTG to read.
//
// This file is that program's contract. A line is one JSON object with its
// keys in a fixed order, ended by '\n'; a frame is whole exactly when its line
// is, so a reader assembles nothing. What a line can carry is fixed by Frame:
// it has no field for an endpoint, a port, a key, a pairing code or a clock,
// so none of them can reach the file.
namespace gtg::hostlog {

inline constexpr int kFormatVersion = 1;

enum class Status {
    Disabled,      // Hosts is off
    Starting,      // on, and no table yet
    Running,       // the link runs: the table is the whole pair list
    Unavailable,   // on, and the link is not running: no cryptography, no network
};

struct Host {
    std::uint64_t id{};
    std::wstring name;
    peer::Heard heard;
    std::optional<peer::HostState> state;   // the last one received
};

struct Frame {
    std::uint64_t seq{};
    std::chrono::sys_time<std::chrono::milliseconds> captured_at{};
    // The monotonic clock the heard times use, at the same instant.
    std::uint64_t now_ms{};
    std::uint64_t observer_id{};   // 0: none made yet
    std::wstring observer_name;
    std::array<std::uint8_t, 3> version{};
    bool enabled{};
    Status status{Status::Disabled};
    // `hosts` is the whole pair list, in the reader's order. Only while running.
    bool table_complete{};
    // Frames not written before this one, in this run.
    std::uint64_t dropped{};
    std::vector<Host> hosts;
};

namespace frame_detail {

[[nodiscard]] inline std::string Hex16(const std::uint64_t value) {
    return std::format("{:016x}", value);
}

// A JSON string. Names reach here cleaned, so the control branch is only a
// guarantee that no input can break the line.
inline void AppendString(std::string& out, const std::string_view utf8) {
    constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char c : utf8) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (byte < 0x20) {
            out.push_back('\\');
            out.push_back('u');
            out.push_back('0');
            out.push_back('0');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0F]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

// The name the dashboard shows, in at most 48 UTF-8 bytes; never empty.
inline void AppendName(std::string& out, const std::wstring_view name, const std::uint64_t id) {
    std::string utf8 = peer::wire::EncodeUtf8(peer::CleanName(name), peer::kNameBytes);
    if (utf8.empty()) utf8 = id != 0 ? "Host-" + Hex16(id) : "GTG";
    AppendString(out, utf8);
}

// The shortest fixed-point text that reads back as the same float: 500000,
// not 5e+05, and 0.0001, not 1e-04. 128 characters hold any float.
inline void AppendNumber(std::string& out, const std::optional<float> value) {
    if (!value) {
        out += "null";
        return;
    }
    char text[128];
    const auto result = std::to_chars(text, text + sizeof(text), *value, std::chars_format::fixed);
    if (result.ec == std::errc{}) {
        out.append(text, result.ptr);
    } else {
        std::format_to(std::back_inserter(out), "{}", *value);
    }
}

inline void AppendVersion(std::string& out, const std::array<std::uint8_t, 3>& version) {
    std::format_to(std::back_inserter(out), "\"{}.{}.{}\"", version[0], version[1], version[2]);
}

[[nodiscard]] constexpr std::string_view StatusText(const Status status) noexcept {
    switch (status) {
        case Status::Disabled:
            return "disabled";
        case Status::Starting:
            return "starting";
        case Status::Running:
            return "running";
        case Status::Unavailable:
            return "unavailable";
    }
    return "unavailable";
}

[[nodiscard]] constexpr std::string_view PresenceText(const peer::Presence presence) noexcept {
    switch (presence) {
        case peer::Presence::Online:
            return "online";
        case peer::Presence::Dim:
            return "dim";
        case peer::Presence::Lost:
            return "lost";
        case peer::Presence::Offline:
            return "offline";
        case peer::Presence::NotHeard:
            return "not_heard";
    }
    return "not_heard";
}

[[nodiscard]] constexpr std::string_view ProtectionText(
    const peer::Protection protection) noexcept {
    switch (protection) {
        case peer::Protection::Normal:
            return "normal";
        case peer::Protection::Warning:
            return "warning";
        case peer::Protection::SafePower:
            return "safe_power";
        case peer::Protection::Fault:
            return "fault";
        case peer::Protection::Unknown:
            return "unknown";
    }
    return "unknown";
}

// In wire order.
inline constexpr std::array<std::string_view, peer::kRecords> kRecordKeys{
    "temperature", "power", "vram", "gpu", "cpu", "ram", "fps", "network"};

inline void AppendHost(std::string& out, const Host& host, const std::uint64_t now_ms) {
    const peer::HostState* state = host.state ? &*host.state : nullptr;
    out += "{\"id\":\"";
    out += Hex16(host.id);
    out += "\",\"name\":";
    AppendName(out, host.name, host.id);
    out += ",\"presence\":\"";
    out += PresenceText(peer::PresenceAt(host.heard, now_ms));
    out += "\",\"age_ms\":";
    if (host.heard.state_ms) {
        const std::uint64_t at = *host.heard.state_ms;
        std::format_to(std::back_inserter(out), "{}", now_ms > at ? now_ms - at : 0);
    } else {
        out += "null";
    }
    out += ",\"version\":";
    if (state != nullptr) {
        AppendVersion(out, state->version);
    } else {
        out += "null";
    }
    out += ",\"protection\":\"";
    out += ProtectionText(state != nullptr ? state->protection : peer::Protection::Unknown);
    // Nothing received is as old as it gets.
    out += state == nullptr || state->stale ? "\",\"stale\":true" : "\",\"stale\":false";
    out += ",\"records\":{";
    for (std::size_t i = 0; i < peer::kRecords; ++i) {
        const peer::RecordState record = state != nullptr ? state->records[i] : peer::RecordState{};
        if (i != 0) out.push_back(',');
        out.push_back('"');
        out += kRecordKeys[i];
        out += "\":[";
        AppendNumber(out, record.value);
        out.push_back(',');
        AppendNumber(out, record.value2);
        out.push_back(',');
        AppendNumber(out, record.level);
        out.push_back(',');
        AppendNumber(out, record.tick);
        out += record.raised ? ",true" : ",false";
        out += record.alarm ? ",true]" : ",false]";
    }
    out += "}}";
}

}  // namespace frame_detail

// The frame as one line, '\n' included.
[[nodiscard]] inline std::string FormatLine(const Frame& frame, const std::string_view run_id) {
    using namespace frame_detail;
    std::string out;
    out.reserve(512 + frame.hosts.size() * 768);
    std::format_to(std::back_inserter(out), "{{\"v\":{},\"run_id\":\"{}\",\"seq\":{}",
                   kFormatVersion, run_id, frame.seq);
    std::format_to(std::back_inserter(out), ",\"captured_at\":\"{:%FT%T}Z\"", frame.captured_at);
    out += ",\"observer_id\":";
    out += frame.observer_id != 0 ? "\"" + Hex16(frame.observer_id) + "\"" : "null";
    out += ",\"observer_name\":";
    AppendName(out, frame.observer_name, frame.observer_id);
    out += ",\"gtg_version\":";
    AppendVersion(out, frame.version);
    out += frame.enabled ? ",\"enabled\":true" : ",\"enabled\":false";
    out += ",\"status\":\"";
    out += StatusText(frame.status);
    out += frame.table_complete ? "\",\"table_complete\":true" : "\",\"table_complete\":false";
    std::format_to(std::back_inserter(out), ",\"export_dropped_total\":{},\"hosts\":[",
                   frame.dropped);
    if (frame.table_complete) {
        for (std::size_t i = 0; i < frame.hosts.size(); ++i) {
            if (i != 0) out.push_back(',');
            AppendHost(out, frame.hosts[i], frame.now_ms);
        }
    }
    out += "]}\n";
    return out;
}

// A run's id: 16 random bytes as a version 4 UUID.
[[nodiscard]] inline std::string FormatRunId(std::array<std::uint8_t, 16> bytes) {
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        std::format_to(std::back_inserter(out), "{:02x}", bytes[i]);
    }
    return out;
}

}  // namespace gtg::hostlog
