// gtg_fake_hosts: paired hosts for testing Hosts on one machine.
//
// One process plays up to 16 hosts on GTG's own peer module -- the code, the
// cryptography and the link thread are the ones GTG runs -- so pairing and
// every state a host can be in can be checked without other PCs.
//
//   gtg_fake_hosts --state FILE join CODE [--name NAME]
//       a new fake host redeems a code GTG created
//   gtg_fake_hosts --state FILE issue [--name NAME] [--copy]
//       a new fake host shows a code for GTG to join, and waits for it
//   gtg_fake_hosts --state FILE run [--modes idle,load,safe,...]
//       every stored fake host sends its state once a second; keys switch them
//   gtg_fake_hosts --state FILE attack [--host N]
//       bad traffic at the GTG host N is paired with; GTG must show no change
//   gtg_fake_hosts --state FILE list | forget all|N
//   gtg_fake_hosts selftest
//       two fake hosts pair over loopback, exchange state, take bad traffic
//       and say goodbye; the exit code says whether all of it held
//
// Common options: --port P (the fixed port issuing hosts start from, plus one
// plus their index, so they never take GTG's), --bind A.B.C.D (bind and name
// that address; loopback for tests).
//
// Pair keys are kept in the state file under DPAPI for the current user. The
// tool never reads or writes GTG's registry. It is built with the tests and is
// never part of a release.

#include <winsock2.h>
#include <windows.h>
#include <conio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "peer/crypto.hpp"
#include "peer/hosts_view.hpp"
#include "peer/keys.hpp"
#include "peer/link.hpp"
#include "peer/pairing_code.hpp"
#include "peer/protocol.hpp"
#include "tray/hosts_bridge.hpp"

#ifdef GTG_FEATURE_UPDATE
#include "update/hosts_wire.hpp"
#endif

namespace {

using namespace gtg;

constexpr std::size_t kMaxFakeHosts = peer::kMaxPairs;

// ----------------------------------------------------------------------------
// Output

std::string Utf8(const std::wstring_view text) {
    return peer::wire::EncodeUtf8(text, 1 << 20);
}

std::wstring Wide(const std::string_view text) {
    return peer::wire::DecodeUtf8(text).value_or(std::wstring(text.begin(), text.end()));
}

std::mutex print_mutex;

void Print(const std::wstring& line) {
    std::lock_guard lock(print_mutex);
    std::cout << Utf8(line) << '\n' << std::flush;
}

std::wstring EndpointText(const peer::Endpoint& endpoint) {
    return std::format(L"{}.{}.{}.{}:{}", endpoint.address[0], endpoint.address[1],
                       endpoint.address[2], endpoint.address[3], endpoint.port);
}

// ----------------------------------------------------------------------------
// The state file: one line per fake host and per pair, names last so they may
// hold spaces.
//
//   host <id> <name>
//   pair <host id> <peer id> <issued 0|1> <a.b.c.d:port|-> <DPAPI key, hex> <name>

struct FakeHost {
    std::uint64_t id{};
    std::wstring name;
    std::vector<peer::Pair> pairs;
};

std::string Hex(const std::span<const std::uint8_t> bytes) {
    std::string text;
    for (const std::uint8_t b : bytes)
        text += std::format("{:02x}", b);
    return text;
}

std::optional<std::vector<std::uint8_t>> FromHex(const std::string& text) {
    if (text.size() % 2 != 0) return std::nullopt;
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned value = 0;
        const auto [end, error] = std::from_chars(text.data() + i, text.data() + i + 2, value, 16);
        if (error != std::errc() || end != text.data() + i + 2) return std::nullopt;
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

// a.b.c.d:port
std::optional<peer::Endpoint> ParseEndpoint(const std::string& text) {
    std::array<unsigned, 5> parts{};
    const char* at = text.data();
    const char* const end = text.data() + text.size();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto [next, error] = std::from_chars(at, end, parts[i]);
        if (error != std::errc()) return std::nullopt;
        at = next;
        const char separator = i < 3 ? '.' : ':';
        if (i < 4) {
            if (at == end || *at != separator) return std::nullopt;
            ++at;
        }
    }
    if (at != end) return std::nullopt;
    for (std::size_t i = 0; i < 4; ++i) {
        if (parts[i] > 255) return std::nullopt;
    }
    if (parts[4] == 0 || parts[4] > 65535) return std::nullopt;
    return peer::Endpoint{
        {static_cast<std::uint8_t>(parts[0]), static_cast<std::uint8_t>(parts[1]),
         static_cast<std::uint8_t>(parts[2]), static_cast<std::uint8_t>(parts[3])},
        static_cast<std::uint16_t>(parts[4])};
}

class StateFile {
public:
    StateFile(std::filesystem::path path, const peer::Crypto& crypto)
        : path_(std::move(path)), crypto_(crypto) {}

    bool Load() {
        std::lock_guard lock(mutex_);
        hosts_.clear();
        std::ifstream in(path_, std::ios::binary);
        if (!in) return true;   // a new file
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::istringstream fields(line);
            std::string kind;
            fields >> kind;
            if (kind == "host") {
                std::string id;
                fields >> id;
                std::string name;
                std::getline(fields >> std::ws, name);
                hosts_.push_back({std::stoull(id, nullptr, 16), Wide(name), {}});
            } else if (kind == "pair") {
                std::string host, peer_id, issued, endpoint, key;
                fields >> host >> peer_id >> issued >> endpoint >> key;
                std::string name;
                std::getline(fields >> std::ws, name);
                const std::uint64_t owner = std::stoull(host, nullptr, 16);
                const auto blob = FromHex(key);
                const auto clear = blob ? crypto_.Unprotect(*blob) : std::nullopt;
                if (!clear || clear->size() != peer::kKeyBytes) {
                    Print(
                        std::format(L"a pair of host {:016x} has a key this user cannot read; "
                                    L"skipped",
                                    owner));
                    continue;
                }
                peer::Pair pair;
                pair.peer = std::stoull(peer_id, nullptr, 16);
                pair.issued = issued == "1";
                pair.endpoint = ParseEndpoint(endpoint);
                std::memcpy(pair.key.data(), clear->data(), peer::kKeyBytes);
                pair.name = Wide(name);
                for (FakeHost& fake : hosts_) {
                    if (fake.id == owner) fake.pairs.push_back(pair);
                }
            }
        }
        return true;
    }

    bool Save() {
        std::lock_guard lock(mutex_);
        return SaveLocked();
    }

    // A host's new or changed pair, from its link thread.
    void Remember(const std::uint64_t host, const peer::Pair& pair) {
        std::lock_guard lock(mutex_);
        for (FakeHost& fake : hosts_) {
            if (fake.id != host) continue;
            const auto found =
                std::find_if(fake.pairs.begin(), fake.pairs.end(),
                             [&](const peer::Pair& p) { return p.peer == pair.peer; });
            if (found != fake.pairs.end()) {
                *found = pair;
            } else {
                fake.pairs.push_back(pair);
            }
        }
        (void)SaveLocked();
    }

    void Forget(const std::uint64_t host, const std::uint64_t peer_id) {
        std::lock_guard lock(mutex_);
        for (FakeHost& fake : hosts_) {
            if (fake.id != host) continue;
            std::erase_if(fake.pairs, [&](const peer::Pair& p) { return p.peer == peer_id; });
        }
        (void)SaveLocked();
    }

    FakeHost& Add(const std::uint64_t id, std::wstring name) {
        std::lock_guard lock(mutex_);
        hosts_.push_back({id, std::move(name), {}});
        return hosts_.back();
    }

    std::vector<FakeHost> Hosts() const {
        std::lock_guard lock(mutex_);
        return hosts_;
    }

    void RemoveHost(const std::size_t index) {
        std::lock_guard lock(mutex_);
        if (index < hosts_.size())
            hosts_.erase(hosts_.begin() + static_cast<std::ptrdiff_t>(index));
    }

    void Clear() {
        std::lock_guard lock(mutex_);
        hosts_.clear();
    }

private:
    bool SaveLocked() {
        std::ostringstream out;
        for (const FakeHost& fake : hosts_) {
            out << "host " << std::format("{:016x}", fake.id) << ' ' << Utf8(fake.name) << '\n';
            for (const peer::Pair& pair : fake.pairs) {
                const auto blob = crypto_.Protect(pair.key);
                if (!blob) return false;
                out << "pair " << std::format("{:016x}", fake.id) << ' '
                    << std::format("{:016x}", pair.peer) << ' ' << (pair.issued ? '1' : '0') << ' '
                    << (pair.endpoint ? Utf8(EndpointText(*pair.endpoint)) : std::string("-"))
                    << ' ' << Hex(*blob) << ' ' << Utf8(pair.name) << '\n';
            }
        }
        std::ofstream file(path_, std::ios::binary | std::ios::trunc);
        file << out.str();
        return static_cast<bool>(file);
    }

    std::filesystem::path path_;
    const peer::Crypto& crypto_;
    mutable std::mutex mutex_;
    std::vector<FakeHost> hosts_;
};

// ----------------------------------------------------------------------------
// What a fake host reads like.

enum class Mode { Idle, Load, Warning, SafePower, Fault, NoGpu };

const wchar_t* ModeName(const Mode mode) {
    switch (mode) {
        case Mode::Idle:
            return L"idle";
        case Mode::Load:
            return L"load";
        case Mode::Warning:
            return L"warning";
        case Mode::SafePower:
            return L"safe power";
        case Mode::Fault:
            return L"fault";
        case Mode::NoGpu:
            return L"no GPU";
    }
    return L"?";
}

std::optional<Mode> ParseMode(const std::string& text) {
    if (text == "idle") return Mode::Idle;
    if (text == "load") return Mode::Load;
    if (text == "warning") return Mode::Warning;
    if (text == "safe") return Mode::SafePower;
    if (text == "fault") return Mode::Fault;
    if (text == "nogpu") return Mode::NoGpu;
    return std::nullopt;
}

struct Behaviour {
    Mode mode{Mode::Idle};
    bool fps{};
    bool fps_unsteady{};
};

// The state a GTG in `behaviour` would send, built by GTG's own bridge so the
// bars, ticks and raised flags come from its rules. `t` moves the readings a
// little so a reader sees them alive.
peer::HostState Generate(const Behaviour& behaviour, const double t, const int index) {
    tray::hosts::LocalState local;
    tray::tiny::BarInputs& in = local.bars;
    const double wobble = std::sin(t * 0.7 + index) * 0.5 + 0.5;
    in.trigger_temperature_c = 85;
    in.safe_power_w = 300;
    in.maximum_power_w = 600.0;
    in.ram_percent = 35.0 + 10.0 * wobble;
    local.network_received = 2.0e5 + 3.0e5 * wobble;
    local.network_sent = 5.0e4;
    in.network_bytes_per_second = *local.network_received;
    in.network_peak = 5.0e5;
    local.visual = tray::hosts::Visual::Armed;
    switch (behaviour.mode) {
        case Mode::Idle:
            in.temperature_c = 42.0 + 4.0 * wobble;
            in.power_w = 60.0 + 20.0 * wobble;
            in.gpu_percent = 3.0 + 5.0 * wobble;
            in.vram_percent = 12.0;
            in.cpu_percent = 5.0 + 8.0 * wobble;
            break;
        case Mode::Load:
            in.temperature_c = 74.0 + 5.0 * wobble;
            in.power_w = 420.0 + 60.0 * wobble;
            in.gpu_percent = 96.0;
            in.vram_percent = 81.0;
            in.cpu_percent = 88.0;
            in.top_app_alerts = topapp::Bit(topapp::Record::Gpu) |
                                topapp::Bit(topapp::Record::Vram) |
                                topapp::Bit(topapp::Record::Cpu);
            break;
        case Mode::Warning:
            in.temperature_c = 83.0 + wobble;
            in.power_w = 480.0;
            in.gpu_percent = 99.0;
            in.vram_percent = 70.0;
            in.cpu_percent = 40.0;
            local.visual = tray::hosts::Visual::Warning;
            break;
        case Mode::SafePower:
            in.temperature_c = 80.0 + 2.0 * wobble;
            in.power_w = 298.0;
            in.gpu_percent = 99.0;
            in.vram_percent = 70.0;
            in.cpu_percent = 30.0;
            local.visual = tray::hosts::Visual::Protected;
            break;
        case Mode::Fault:
            in.temperature_c = 88.0;
            in.power_w = 300.0;
            in.gpu_percent = 99.0;
            in.vram_percent = 75.0;
            in.cpu_percent = 30.0;
            local.visual = tray::hosts::Visual::Fault;
            break;
        case Mode::NoGpu:
            in.cpu_percent = 20.0 + 10.0 * wobble;
            local.visual = tray::hosts::Visual::Neutral;
            break;
    }
    if (behaviour.fps) {
        const double deviation = behaviour.fps_unsteady ? -0.3 * wobble : 0.01 * wobble;
        in.fps = tray::tiny::FpsStability{60.0 * (1.0 + deviation), 60.0, deviation,
                                          tray::tiny::FpsLevel(deviation),
                                          tray::tiny::FpsSeverity(deviation)};
    }
    local.version = {0, 0, 0};
    return tray::hosts::Build(local);
}

#ifdef GTG_FEATURE_UPDATE
// A fake host's updater. It answers a GTG's request as GTG would, each step a
// few seconds long, so the asking GTG's Hosts dialog and "update all" can be
// watched without real machines. It installs nothing: "updated" only changes
// the version it reports.
struct FakeUpdater {
    enum class Step { Idle, Checking, Staging, Waiting, Exiting, Away };

    std::array<std::uint8_t, 3> version{1, 2, 0};
    std::array<std::uint8_t, 3> target{1, 2, 1};
    bool refuses{};      // x: this host does not let others update it
    bool rolls_back{};   // k: the next update comes back on the old version
    Step step{Step::Idle};
    std::uint64_t step_ms{};
    update::hosts::Trigger trigger;
    std::uint64_t last_seen{};
    std::uint64_t answered{};
    update::hosts::Answer answer{update::hosts::Answer::None};

    update::hosts::Status Status() const {
        using update::hosts::Phase;
        update::hosts::Status status;
        status.has_source = true;
        status.accepts_triggers = !refuses;
        status.pin_set = true;
        switch (step) {
            case Step::Idle:
                status.phase = Phase::Idle;
                break;
            case Step::Checking:
                status.phase = Phase::Checking;
                break;
            case Step::Staging:
                status.phase = Phase::Staging;
                break;
            case Step::Waiting:
                status.phase = Phase::Waiting;
                status.wait = update::hosts::WaitFor::Temperature;
                break;
            case Step::Exiting:
            case Step::Away:
                status.phase = Phase::Exiting;
                break;
        }
        status.source =
            version == target ? update::hosts::SourceIs::SameBuild : update::hosts::SourceIs::Newer;
        status.source_version = target;
        if (trigger.id != 0) status.source_release = update::hosts::ShortOf(trigger.release);
        status.running_build = {version[0], version[1], version[2], 0xFA, 0x4E, 0, 0, 0};
        status.trigger = answered;
        status.answer = answered != 0 ? answer : update::hosts::Answer::None;
        return status;
    }
};
#endif

// ----------------------------------------------------------------------------
// Running fake hosts.

struct Options {
    std::filesystem::path state;
    std::uint16_t port{peer::kDefaultPort};
    std::optional<std::array<std::uint8_t, 4>> bind;
    std::wstring name;
    std::string code;
    std::vector<Mode> modes;
    std::size_t host{};
    bool copy{};
#ifdef GTG_FEATURE_UPDATE
    // The version a fake host reports after an update.
    std::array<std::uint8_t, 3> update_to{1, 2, 1};
#endif
};

class Runner {
public:
    Runner(StateFile& state, const Options& options) : state_(state), options_(options) {}

    ~Runner() { StopAll(); }

    bool Start(const FakeHost& fake, const std::size_t index) {
        auto slot = std::make_unique<Slot>();
        slot->id = fake.id;
        slot->name = fake.name;
        slot->index = index;
        slot->behaviour.mode =
            index < options_.modes.size() ? options_.modes[index] : DefaultMode(index);
        slot->behaviour.fps = index % 2 == 0;
        slot->pairs = fake.pairs;
#ifdef GTG_FEATURE_UPDATE
        slot->updater.target = options_.update_to;
#endif
        if (!Launch(*slot)) return false;
        slots_.push_back(std::move(slot));
        return true;
    }

    void StopAll() {
        for (auto& slot : slots_)
            slot->link.Stop();
    }

    void PublishAll(const double t) {
        for (auto& slot : slots_) {
            if (slot->link.Running()) {
#ifdef GTG_FEATURE_UPDATE
                peer::HostState state = Generate(slot->behaviour, t, static_cast<int>(slot->index));
                state.version = slot->updater.version;
                slot->link.Publish(state);
                slot->link.PublishUpdate(update::hosts::WriteStatus(slot->updater.Status()));
#else
                slot->link.Publish(Generate(slot->behaviour, t, static_cast<int>(slot->index)));
#endif
            }
        }
    }

#ifdef GTG_FEATURE_UPDATE
    // A request from a paired GTG starts the steps; the last one restarts the
    // fake host's link, as an update restarts GTG.
    void TickUpdates() {
        using Step = FakeUpdater::Step;
        const std::uint64_t now = GetTickCount64();
        for (auto& slot : slots_) {
            Slot& s = *slot;
            FakeUpdater& u = s.updater;
            if (u.step == Step::Idle && s.link.Running()) {
                const auto table = s.link.TryRead();
                for (std::size_t i = 0; table && i < table->hosts.size(); ++i) {
                    const peer::HostRow& row = table->hosts[i];
                    if (!row.update_trigger || now - row.update_trigger_ms > 30'000) continue;
                    const auto trigger = update::hosts::ReadTrigger(*row.update_trigger);
                    if (!trigger || trigger->id == u.last_seen) continue;
                    u.last_seen = trigger->id;
                    u.answered = trigger->id;
                    if (u.refuses) {
                        u.answer = update::hosts::Answer::NotAllowed;
                    } else if (!trigger->force && u.version == u.target) {
                        u.answer = update::hosts::Answer::AlreadyCurrent;
                    } else {
                        u.answer = update::hosts::Answer::Accepted;
                        u.trigger = *trigger;
                        u.step = Step::Checking;
                        u.step_ms = now;
                    }
                    Print(std::format(
                        L"  {} {}: asked by {}{}; {}", s.index + 1, s.name, row.name,
                        trigger->force ? L" (forced)" : L"",
                        u.answer == update::hosts::Answer::Accepted ? L"updating" : L"refused"));
                }
            }
            const std::uint64_t in_step = now - u.step_ms;
            const auto next = [&](const Step step) {
                u.step = step;
                u.step_ms = now;
            };
            switch (u.step) {
                case Step::Checking:
                    if (in_step >= 1'000) next(Step::Staging);
                    break;
                case Step::Staging:
                    if (in_step >= 1'000) next(Step::Waiting);
                    break;
                case Step::Waiting:
                    if (in_step >= 3'000) next(Step::Exiting);
                    break;
                case Step::Exiting:
                    if (in_step >= 1'000) {
                        // A goodbye, as GTG exits for the script.
                        s.link.Stop();
                        next(Step::Away);
                    }
                    break;
                case Step::Away:
                    if (in_step >= 3'000) {
                        if (u.rolls_back) {
                            u.rolls_back = false;
                            u.answer = update::hosts::Answer::RolledBack;
                        } else {
                            u.version = u.target;
                            u.answer = update::hosts::Answer::Updated;
                        }
                        u.step = Step::Idle;
                        s.muted = false;
                        s.pairs = PairsOf(s.id);
                        (void)Launch(s);
                        Print(std::format(L"  {} {}: back, {}", s.index + 1, s.name,
                                          u.answer == update::hosts::Answer::Updated
                                              ? L"updated"
                                              : L"rolled back"));
                    }
                    break;
                case Step::Idle:
                    break;
            }
        }
    }
#endif

    // goodbye, vanish, sleep, wake, back -- on one host or all.
    void Apply(const wchar_t key, const std::optional<std::size_t> which) {
        for (auto& slot : slots_) {
            if (which && slot->index != *which) continue;
            Slot& s = *slot;
            switch (key) {
                case L'i':
                    s.behaviour.mode = Mode::Idle;
                    break;
                case L'l':
                    s.behaviour.mode = Mode::Load;
                    break;
                case L'w':
                    s.behaviour.mode = Mode::Warning;
                    break;
                case L's':
                    s.behaviour.mode = Mode::SafePower;
                    break;
                case L'f':
                    s.behaviour.mode = Mode::Fault;
                    break;
                case L'n':
                    s.behaviour.mode = Mode::NoGpu;
                    break;
                case L'p':
                    s.behaviour.fps = !s.behaviour.fps;
                    break;
                case L'u':
                    s.behaviour.fps_unsteady = !s.behaviour.fps_unsteady;
                    break;
#ifdef GTG_FEATURE_UPDATE
                case L'x':
                    s.updater.refuses = !s.updater.refuses;
                    break;
                case L'k':
                    s.updater.rolls_back = !s.updater.rolls_back;
                    break;
#endif
                case L'g':
                    // A goodbye: GTG shows the host offline, quiet.
                    s.link.Stop();
                    break;
                case L'v':
                    // Silence without a goodbye: GTG shows the host lost after 10 s.
                    if (s.link.Running()) s.link.Mute();
                    s.muted = true;
                    break;
                case L'z':
                    if (s.link.Running()) s.link.Suspend();
                    break;
                case L'r':
                    if (s.link.Running() && !s.muted) {
                        s.link.Resume();
                    } else {
                        // Back from a goodbye or a vanish: a new start, as a
                        // restarted GTG would be.
                        s.link.Stop();
                        s.muted = false;
                        s.pairs = PairsOf(s.id);
                        (void)Launch(s);
                    }
                    break;
                default:
                    break;
            }
            Print(std::format(L"  {} {}: {}{}{}", s.index + 1, s.name,
                              s.link.Running() ? ModeName(s.behaviour.mode) : L"stopped",
                              s.behaviour.fps ? L", FPS" : L"", s.muted ? L", vanished" : L""));
        }
    }

    // What each fake host hears from the GTGs it is paired with.
    void Report() {
        const std::uint64_t now = GetTickCount64();
        for (auto& slot : slots_) {
            const auto table = slot->link.TryRead();
            if (!table) continue;
            for (const peer::HostRow& row : table->hosts) {
                std::wstring line =
                    std::format(L"  {} {} <- {}: ", slot->index + 1, slot->name, row.name);
                switch (peer::PresenceAt(row.heard, now)) {
                    case peer::Presence::Online:
                        line += L"online";
                        break;
                    case peer::Presence::Dim:
                        line += L"late";
                        break;
                    case peer::Presence::Lost:
                        line += L"lost";
                        break;
                    case peer::Presence::Offline:
                        line += L"offline";
                        break;
                    case peer::Presence::NotHeard:
                        line += L"not heard";
                        break;
                }
                if (row.state) {
                    const auto value = [&](const peer::Record record) -> std::wstring {
                        const auto& r = row.state->records[static_cast<std::size_t>(record)];
                        return r.value ? std::format(L"{:.0f}{}", *r.value, r.raised ? L"!" : L"")
                                       : L"-";
                    };
                    line += std::format(
                        L"; protection {} T {} P {} G {} V {} C {} R {} F {} v{}.{}.{}",
                        static_cast<int>(row.state->protection), value(peer::Record::Temperature),
                        value(peer::Record::Power), value(peer::Record::Gpu),
                        value(peer::Record::Vram), value(peer::Record::Cpu),
                        value(peer::Record::Ram), value(peer::Record::Fps), row.state->version[0],
                        row.state->version[1], row.state->version[2]);
                }
                if (row.clock_skew_ms)
                    line += std::format(L"; clock off {} ms", *row.clock_skew_ms);
                Print(line);
            }
        }
    }

    std::size_t Count() const noexcept { return slots_.size(); }

    peer::Link* LinkOf(const std::size_t index) {
        return index < slots_.size() ? &slots_[index]->link : nullptr;
    }

private:
    struct Slot {
        std::uint64_t id{};
        std::wstring name;
        std::size_t index{};
        Behaviour behaviour;
        bool muted{};
        std::vector<peer::Pair> pairs;
        peer::Link link;
#ifdef GTG_FEATURE_UPDATE
        FakeUpdater updater;
#endif
    };

    static Mode DefaultMode(const std::size_t index) {
        constexpr Mode kCycle[] = {Mode::Idle, Mode::Load,    Mode::SafePower,
                                   Mode::Idle, Mode::Warning, Mode::Fault};
        return kCycle[index % std::size(kCycle)];
    }

    std::vector<peer::Pair> PairsOf(const std::uint64_t id) const {
        for (const FakeHost& fake : state_.Hosts()) {
            if (fake.id == id) return fake.pairs;
        }
        return {};
    }

    bool Launch(Slot& slot) {
        peer::Link::Options options;
        options.self = {slot.id, slot.name};
        // Never GTG's port: an issuing fake host listens beside it.
        options.port = static_cast<std::uint16_t>(options_.port + 1 + slot.index);
        options.pairs = slot.pairs;
        options.bind_address = options_.bind;
        const std::uint64_t id = slot.id;
        options.save = [this, id](const peer::Pair& pair) {
            state_.Remember(id, pair);
        };
        options.forget = [this, id](const std::uint64_t peer_id) {
            state_.Forget(id, peer_id);
        };
        return slot.link.Start(std::move(options));
    }

    StateFile& state_;
    const Options& options_;
    std::vector<std::unique_ptr<Slot>> slots_;
};

std::uint64_t RandomId(const peer::Crypto& crypto) {
    std::uint64_t id = 0;
    while (id == 0) {
        if (!crypto.Random({reinterpret_cast<std::uint8_t*>(&id), sizeof(id)})) return 0;
    }
    return id;
}

std::wstring NextName(const StateFile& state, const Options& options) {
    if (!options.name.empty()) return options.name;
    return std::format(L"Fake-{}", state.Hosts().size() + 1);
}

template <typename Ready>
std::optional<peer::Table> Await(peer::Link& link, Ready ready, const std::uint64_t timeout_ms) {
    const std::uint64_t deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        if (auto table = link.TryRead(); table && ready(*table)) return table;
        Sleep(50);
    }
    return std::nullopt;
}

int Join(StateFile& state, const Options& options, const peer::Crypto& crypto) {
    if (state.Hosts().size() >= kMaxFakeHosts) {
        Print(L"16 fake hosts already; forget one first");
        return 2;
    }
    const auto parsed = peer::ParseCode(std::string_view(options.code));
    if (!parsed.code) {
        Print(std::format(L"not a pairing code (error {})", static_cast<int>(parsed.error)));
        return 2;
    }
    const std::uint64_t id = RandomId(crypto);
    const std::wstring name = NextName(state, options);
    (void)state.Add(id, name);
    Runner runner(state, options);
    if (!runner.Start({id, name, {}}, state.Hosts().size() - 1)) return 1;
    peer::Link& link = *runner.LinkOf(0);
    Print(std::format(L"{} asks {} ...", name,
                      EndpointText({parsed.code->address, parsed.code->port})));
    link.Join(*parsed.code);
    const auto done = Await(
        link,
        [](const peer::Table& t) {
            return t.joining && t.joining->status != peer::JoinStatus::Asking;
        },
        15'000);
    const auto status = done ? done->joining->status : peer::JoinStatus::NoAnswer;
    switch (status) {
        case peer::JoinStatus::Paired:
            Print(std::format(L"{} paired with {}", name, done->joining->issuer));
            (void)state.Save();
            return 0;
        case peer::JoinStatus::Used:
            Print(L"the code was already used");
            break;
        case peer::JoinStatus::Expired:
            Print(L"the code has expired");
            break;
        default:
            Print(L"no answer");
            break;
    }
    state.RemoveHost(state.Hosts().size() - 1);
    (void)state.Save();
    return 1;
}

void CopyToClipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const std::wstring wide(text.begin(), text.end());
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        std::memcpy(GlobalLock(memory), wide.c_str(), bytes);
        GlobalUnlock(memory);
        if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) GlobalFree(memory);
    }
    CloseClipboard();
}

int Issue(StateFile& state, const Options& options, const peer::Crypto& crypto) {
    if (state.Hosts().size() >= kMaxFakeHosts) {
        Print(L"16 fake hosts already; forget one first");
        return 2;
    }
    const std::uint64_t id = RandomId(crypto);
    const std::wstring name = NextName(state, options);
    (void)state.Add(id, name);
    Runner runner(state, options);
    if (!runner.Start({id, name, {}}, state.Hosts().size() - 1)) return 1;
    peer::Link& link = *runner.LinkOf(0);
    link.CreateCode();
    const auto shown = Await(
        link, [](const peer::Table& t) { return t.invitation.has_value() || t.no_address; }, 5'000);
    const auto give_up = [&](const std::wstring& why) {
        Print(why);
        state.RemoveHost(state.Hosts().size() - 1);
        (void)state.Save();
        return 1;
    };
    if (!shown || !shown->invitation) {
        return give_up(L"no code: this machine has no address to put in one");
    }
    if (shown->status == peer::LinkStatus::PortInUse) {
        return give_up(std::format(L"port {} is in use; try --port",
                                   options.port + 1 + state.Hosts().size() - 1));
    }
    Print(std::format(L"{} listens on port {}. Paste this into GTG's Hosts dialog:", name,
                      shown->port));
    Print(Wide(shown->invitation->code));
    if (options.copy) {
        CopyToClipboard(shown->invitation->code);
        Print(L"(copied)");
    }
    const auto done = Await(
        link,
        [](const peer::Table& t) {
            return t.invitation && t.invitation->status != peer::Invitation::Status::Live;
        },
        peer::kCodeLifetimeMs + 2'000);
    if (done && done->invitation->status == peer::Invitation::Status::Spent) {
        Print(std::format(L"{} paired with {}", name, done->invitation->joined_by));
        (void)state.Save();
        return 0;
    }
    return give_up(L"the code was not used");
}

int Run(StateFile& state, const Options& options) {
    const auto hosts = state.Hosts();
    if (hosts.empty()) {
        Print(L"no fake hosts yet: join or issue first");
        return 2;
    }
    Runner runner(state, options);
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        if (!runner.Start(hosts[i], i)) Print(std::format(L"{} could not start", hosts[i].name));
    }
    Print(
        L"keys: 1-9 pick a host, 0 all;  i idle  l load  w warning  s safe power  f fault  "
        L"n no GPU  p FPS on/off  u FPS unsteady");
    Print(L"      g goodbye (offline)  v vanish (lost)  z sleep  r wake/back  . report  q quit");
#ifdef GTG_FEATURE_UPDATE
    Print(L"      updates: x refuse requests on/off  k roll back the next update on/off");
#endif
    runner.Apply(0, std::nullopt);
    std::optional<std::size_t> which;
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t last_report = 0;
    for (;;) {
        const double t =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        runner.PublishAll(t);
#ifdef GTG_FEATURE_UPDATE
        runner.TickUpdates();
#endif
        while (_kbhit()) {
            const wchar_t key = static_cast<wchar_t>(_getwch());
            if (key == L'q') return 0;
            if (key >= L'1' && key <= L'9') {
                which = static_cast<std::size_t>(key - L'1');
                Print(std::format(L"host {} selected", *which + 1));
            } else if (key == L'0') {
                which.reset();
                Print(L"all hosts selected");
            } else if (key == L'.') {
                runner.Report();
            } else {
                runner.Apply(key, which);
            }
        }
        const std::uint64_t now = GetTickCount64();
        if (now - last_report >= 10'000) {
            runner.Report();
            last_report = now;
        }
        Sleep(200);
    }
}

// Bad traffic at the GTG that fake host `index` is paired with. Every
// datagram here must be dropped: GTG shows the host as it was.
int Attack(StateFile& state, const Options& options, const peer::Crypto& crypto) {
    const auto hosts = state.Hosts();
    if (options.host >= hosts.size() || hosts[options.host].pairs.empty() ||
        !hosts[options.host].pairs.front().endpoint) {
        Print(L"that fake host is not paired with anything it can reach");
        return 2;
    }
    const FakeHost& fake = hosts[options.host];
    const peer::Pair& pair = fake.pairs.front();
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    const SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(pair.endpoint->port);
    std::memcpy(&to.sin_addr, pair.endpoint->address.data(), 4);
    const auto send = [&](const std::span<const std::uint8_t> bytes) {
        sendto(socket_handle, reinterpret_cast<const char*>(bytes.data()),
               static_cast<int>(bytes.size()), 0, reinterpret_cast<const sockaddr*>(&to),
               sizeof(to));
    };
    const auto now_wall = [] {
        FILETIME time{};
        GetSystemTimeAsFileTime(&time);
        return static_cast<std::int64_t>(
                   ((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime) /
                   10'000) -
               11'644'473'600'000LL;
    };
    const auto sealed = [&](const peer::Key& key, const std::uint8_t salt_fill,
                            const std::uint64_t sequence, const std::int64_t sent_ms,
                            const wchar_t* name) {
        peer::SealedHeader header;
        header.sender = fake.id;
        header.salt.fill(salt_fill);
        header.sequence = sequence;
        header.sent_ms = sent_ms;
        peer::HostState state_body = Generate({Mode::Fault}, 0.0, 0);
        state_body.name = name;
        const auto direction = peer::DirectionKey(crypto, key, header.salt, fake.id, pair.peer);
        return peer::SealState(crypto, *direction, header, state_body);
    };
    Print(std::format(L"bad traffic from {} at {} ({}):", fake.name, EndpointText(*pair.endpoint),
                      pair.name));

    std::array<std::uint8_t, 300> junk{};
    for (std::size_t i = 0; i < junk.size(); ++i)
        junk[i] = static_cast<std::uint8_t>(i * 13);
    for (int i = 0; i < 5; ++i)
        send(junk);
    Print(L"  junk: five datagrams that are not GTG's");

    auto flipped = *sealed(pair.key, 0x11, 1, now_wall(), L"FLIPPED");
    flipped[flipped.size() / 2] ^= 0x01;
    send(flipped);
    Print(L"  a state datagram with one bit changed");

    peer::Key wrong{};
    (void)crypto.Random(wrong);
    send(*sealed(wrong, 0x22, 1, now_wall(), L"WRONG KEY"));
    Print(L"  a state datagram under a key that is not the pair's");

    send(*sealed(pair.key, 0x33, 1, now_wall() - 30'000, L"OLDER SESSION"));
    Print(L"  a genuine-looking session from 30 s ago (older than the one in use)");

    send(*sealed(pair.key, 0x44, 1, now_wall() - 10 * 60'000, L"STALE"));
    Print(L"  a session ten minutes old (GTG says the clocks disagree, and shows nothing of it)");

    for (int i = 0; i < 1'000; ++i)
        send(junk);
    Print(L"  a flood: 1000 datagrams in a burst");

    closesocket(socket_handle);
    WSACleanup();
    Print(
        std::format(L"GTG must still show {} as it was, never FLIPPED, WRONG KEY, OLDER "
                    L"SESSION or STALE, and keep hearing it if it is running.",
                    fake.name));
    return 0;
}

int List(const StateFile& state) {
    std::size_t index = 0;
    for (const FakeHost& fake : state.Hosts()) {
        Print(std::format(L"{} {} ({:016x})", ++index, fake.name, fake.id));
        for (const peer::Pair& pair : fake.pairs) {
            Print(std::format(L"    paired with {} at {}{}", pair.name,
                              pair.endpoint ? EndpointText(*pair.endpoint) : L"-",
                              pair.issued ? L", its code" : L""));
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
// selftest: the tool against itself over loopback.

int SelfTest(const peer::Crypto& crypto) {
    int failures = 0;
    const auto check = [&](const bool ok, const wchar_t* what) {
        Print(std::format(L"[{}] {}", ok ? L"PASS" : L"FAIL", what));
        if (!ok) ++failures;
    };
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path path =
        std::filesystem::path(temp) / std::format(L"gtg_fake_hosts_{}.txt", GetCurrentProcessId());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    // A port nothing holds right now, for the issuing host.
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(local);
    bind(probe, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
    getsockname(probe, reinterpret_cast<sockaddr*>(&local), &size);
    closesocket(probe);
    WSACleanup();

    Options options;
    options.state = path;
    options.bind = std::array<std::uint8_t, 4>{127, 0, 0, 1};
    options.port = static_cast<std::uint16_t>(ntohs(local.sin_port) - 1);  // host 0: +1
    options.modes = {Mode::SafePower, Mode::Load};
    {
        StateFile state(path, crypto);
        (void)state.Add(RandomId(crypto), L"Self-Issuer");
        (void)state.Add(RandomId(crypto), L"工作站 自測");
        const auto hosts = state.Hosts();
        Runner runner(state, options);
        check(runner.Start(hosts[0], 0) && runner.Start(hosts[1], 1), L"two fake hosts start");
        peer::Link& issuer = *runner.LinkOf(0);
        peer::Link& joiner = *runner.LinkOf(1);
        issuer.CreateCode();
        const auto code =
            Await(issuer, [](const peer::Table& t) { return t.invitation.has_value(); }, 5'000);
        check(code && code->listening, L"the issuing host binds its port and shows a code");
        const auto parsed = peer::ParseCode(std::string_view(code->invitation->code));
        joiner.Join(*parsed.code);
        const auto joined = Await(
            joiner,
            [](const peer::Table& t) {
                return t.joining && t.joining->status == peer::JoinStatus::Paired;
            },
            10'000);
        check(joined.has_value(), L"the second host pairs with the first's code");
        const auto hears = [&](peer::Link& link, const std::wstring& name,
                               const peer::Protection protection) {
            return Await(
                       link,
                       [&](const peer::Table& t) {
                           runner.PublishAll(1.0);
                           return t.hosts.size() == 1 && t.hosts[0].state &&
                                  t.hosts[0].state->name == name &&
                                  t.hosts[0].state->protection == protection;
                       },
                       10'000)
                .has_value();
        };
        check(hears(issuer, L"工作站 自測", peer::Protection::Normal),
              L"the issuer hears the joiner, CJK name and all");
        check(hears(joiner, L"Self-Issuer", peer::Protection::SafePower),
              L"the joiner hears the issuer on safe power");
        runner.Apply(L'f', 0);
        check(hears(joiner, L"Self-Issuer", peer::Protection::Fault), L"a switched mode arrives");
        runner.StopAll();
    }
    {
        StateFile reread(path, crypto);
        reread.Load();
        const auto hosts = reread.Hosts();
        check(hosts.size() == 2 && hosts[0].pairs.size() == 1 && hosts[1].pairs.size() == 1 &&
                  hosts[0].pairs[0].key == hosts[1].pairs[0].key && hosts[0].pairs[0].issued &&
                  !hosts[1].pairs[0].issued && hosts[1].name == L"工作站 自測",
              L"the state file keeps both hosts, their names and one shared key under DPAPI");

        // Back from the file, then bad traffic at the issuer.
        Runner runner(reread, options);
        check(runner.Start(hosts[0], 0) && runner.Start(hosts[1], 1),
              L"both start again from the file");
        peer::Link& issuer = *runner.LinkOf(0);
        const auto back = Await(
            issuer,
            [&](const peer::Table& t) {
                runner.PublishAll(2.0);
                return t.hosts.size() == 1 &&
                       peer::PresenceAt(t.hosts[0].heard, GetTickCount64()) ==
                           peer::Presence::Online;
            },
            10'000);
        check(back.has_value(), L"they find each other again without pairing");

        // The attack comes from the joiner's identity, aimed at the issuer.
        Options attack = options;
        attack.host = 1;
        check(Attack(reread, attack, crypto) == 0, L"bad traffic sent");
        const std::uint64_t after = GetTickCount64();
        const auto still = Await(
            issuer,
            [&](const peer::Table& t) {
                runner.PublishAll(3.0);
                return t.hosts.size() == 1 && t.hosts[0].heard.state_ms &&
                       *t.hosts[0].heard.state_ms > after + 1'500;
            },
            10'000);
        // The stale session's clock note is cleared again by the joiner's next
        // genuine datagram, so only what is shown is checked here.
        check(still && still->hosts[0].state->name == L"工作站 自測" &&
                  still->hosts[0].state->protection == peer::Protection::Normal,
              L"after it, the issuer still shows the joiner as itself");

        runner.LinkOf(1)->Stop();
        const auto offline = Await(
            issuer, [](const peer::Table& t) { return t.hosts[0].heard.goodbye_ms.has_value(); },
            5'000);
        check(offline && peer::PresenceAt(offline->hosts[0].heard, GetTickCount64()) ==
                             peer::Presence::Offline,
              L"a goodbye reads as offline");
        runner.StopAll();
    }
    std::filesystem::remove(path, ignored);
    Print(failures == 0 ? L"selftest passed" : L"selftest FAILED");
    return failures == 0 ? 0 : 1;
}

void Usage() {
    Print(
        L"usage: gtg_fake_hosts --state FILE (join CODE | issue | run | attack | list | "
        L"forget all|N) [--name NAME] [--port P] [--bind A.B.C.D] [--modes m,m,...] "
        L"[--host N] [--copy]");
    Print(L"       gtg_fake_hosts selftest");
    Print(L"join CODE  adds the next fake host (Fake-1, Fake-2, ...) to FILE; a code alone joins");
    Print(L"run        sends the state of every host in FILE; keys 1-9 pick one");
    Print(L"attack     bad traffic from host --host N (default 1)");
    Print(L"modes: idle load warning safe fault nogpu");
#ifdef GTG_FEATURE_UPDATE
    Print(L"--update-to X.Y.Z  the version a fake host reports after an update (1.2.1)");
#endif
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(Utf8(argv[i]));
    peer::Crypto crypto;
    if (!crypto.Load()) {
        Print(L"CNG or DPAPI is unavailable");
        return 1;
    }
    if (!args.empty() && args[0] == "selftest") return SelfTest(crypto);

    Options options;
    std::string command;
    std::vector<std::string> rest;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--state" && has_value) {
            options.state = Wide(args[++i]);
        } else if (arg == "--name" && has_value) {
            options.name = Wide(args[++i]);
        } else if (arg == "--port" && has_value) {
            options.port = static_cast<std::uint16_t>(std::stoi(args[++i]));
        } else if (arg == "--host" && has_value) {
            options.host = static_cast<std::size_t>(std::stoi(args[++i]) - 1);
        } else if (arg == "--bind" && has_value) {
            const auto endpoint = ParseEndpoint(args[++i] + ":1");
            if (!endpoint) {
                Usage();
                return 2;
            }
            options.bind = endpoint->address;
        } else if (arg == "--modes" && has_value) {
            std::stringstream list(args[++i]);
            std::string item;
            while (std::getline(list, item, ',')) {
                const auto mode = ParseMode(item);
                if (!mode) {
                    Usage();
                    return 2;
                }
                options.modes.push_back(*mode);
            }
        } else if (arg == "--copy") {
            options.copy = true;
#ifdef GTG_FEATURE_UPDATE
        } else if (arg == "--update-to" && has_value) {
            const std::string& text = args[++i];
            const char* at = text.data();
            const char* const end = text.data() + text.size();
            bool ok = true;
            for (std::size_t k = 0; k < 3 && ok; ++k) {
                int part = 0;
                const auto [next, error] = std::from_chars(at, end, part);
                ok = error == std::errc() && (k == 2 ? next == end : next != end && *next == '.');
                options.update_to[k] = static_cast<std::uint8_t>(std::clamp(part, 0, 255));
                if (ok && k < 2) at = next + 1;
            }
            if (!ok) {
                Usage();
                return 2;
            }
#endif
        } else if (command.empty()) {
            command = arg;
        } else {
            rest.push_back(arg);
        }
    }
    if (options.state.empty() || command.empty()) {
        Usage();
        return 2;
    }
    // A pasted code on its own means join: that is what pasting one is for.
    if (command != "join" && peer::ParseCode(std::string_view(command)).code) {
        rest.insert(rest.begin(), command);
        command = "join";
    }
    StateFile state(options.state, crypto);
    state.Load();
    if (command == "join" && !rest.empty()) {
        for (const std::string& part : rest)
            options.code += part;
        return Join(state, options, crypto);
    }
    if (command == "issue") return Issue(state, options, crypto);
    if (command == "run") return Run(state, options);
    if (command == "attack") return Attack(state, options, crypto);
    if (command == "list") return List(state);
    if (command == "forget" && !rest.empty()) {
        if (rest[0] == "all") {
            state.Clear();
        } else {
            state.RemoveHost(static_cast<std::size_t>(std::stoi(rest[0]) - 1));
        }
        return state.Save() ? 0 : 1;
    }
    Usage();
    return 2;
}
