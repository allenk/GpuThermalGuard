#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hostlog/writer.hpp"
#include "peer/link.hpp"
#include "peer/store.hpp"
#include "settings/settings.hpp"
#include "tray/hosts_bridge.hpp"

// Hosts on the Tray's side: the link's lifetime, its settings, and the table
// the dashboard and the dialog read. Everything here runs on the UI thread;
// the link's own thread is reached only through Publish, TryRead and its
// requests, none of which waits on it.
namespace gtg::tray {

class HostsController {
public:
    static constexpr std::uint64_t kPublishIntervalMs = 1'000;

    HostsController() = default;

    ~HostsController() {
        Shutdown();
        StopLog();
    }

    HostsController(const HostsController&) = delete;
    HostsController& operator=(const HostsController&) = delete;

    // At start: the stored preference, and the link if Hosts is on.
    void Initialize();
    // At exit, sign-out or shutdown: a goodbye to every host, then the join.
    void Shutdown() noexcept;

    // On the dashboard's tick. `make` builds this machine's state, and is
    // called once a second at most, and only while the link runs.
    template <typename Make>
    void Tick(const std::uint64_t now_ms, Make make) {
        if (link_.Running()) {
            if (!last_publish_ms_ || now_ms - *last_publish_ms_ >= kPublishIntervalMs) {
                link_.Publish(hosts::Build(make()));
                last_publish_ms_ = now_ms;
            }
            if (auto table = link_.TryRead()) table_ = std::move(table);
        }
        LogTick(now_ms);
    }

    // The Hosts log, independent of Hosts itself: while it is on, every 5 s
    // the table read above, or the fact that Hosts is off, goes to the log's
    // own thread. Off: no thread, no file.
    [[nodiscard]] bool LogOn() const noexcept { return preference_.log; }

    // Turns the log on or off and keeps the choice.
    [[nodiscard]] bool SetLogOn(bool on, std::wstring& error);
    // At exit. Waits at most 2 s for the log's thread.
    void StopLog() noexcept;

    // Sleep and wake: a goodbye now, a new session after.
    void Suspend();
    void Resume();

    [[nodiscard]] bool On() const noexcept { return preference_.enabled; }

    [[nodiscard]] bool Running() const noexcept { return link_.Running(); }

    // Turns Hosts on or off and keeps the choice.
    [[nodiscard]] bool SetOn(bool on, std::wstring& error);
    // This machine's name (empty: the computer's) and the fixed port. A new
    // port restarts the link; a new name goes out with the next state.
    [[nodiscard]] bool SetIdentity(const std::wstring& name, int port, std::wstring& error);

    // The last table read; nothing until the link has published one.
    [[nodiscard]] const std::optional<peer::Table>& Table() const noexcept { return table_; }

    // The running link, for the dialog's requests; null while Hosts is off.
    [[nodiscard]] peer::Link* Link() noexcept { return link_.Running() ? &link_ : nullptr; }

    void SaveOrder(const std::vector<std::uint64_t>& order);

    // The name sent: the reader's, or the computer's.
    [[nodiscard]] std::wstring Name() const;

    [[nodiscard]] const std::wstring& StoredName() const noexcept { return preference_.name; }

    [[nodiscard]] int Port() const noexcept;

    [[nodiscard]] static std::wstring ComputerName();

    // Pairs whose key could not be read at the last start, by name. They were
    // removed.
    [[nodiscard]] const std::vector<std::wstring>& Dropped() const noexcept { return dropped_; }

    // Why Hosts is not running although it is on; empty when it runs.
    [[nodiscard]] const std::wstring& Problem() const noexcept { return problem_; }

private:
    bool Start();
    bool StartLog();
    void LogTick(std::uint64_t now_ms);
    [[nodiscard]] hostlog::Frame MakeFrame(std::uint64_t now_ms) const;

    settings::HostsPreference preference_;
    peer::PairStore store_;
    bool store_loaded_{};
    // After the store: destroyed first, so its thread is joined before the
    // store its callbacks use goes away.
    peer::Link link_;
    std::optional<peer::Table> table_;
    std::optional<std::uint64_t> last_publish_ms_;
    std::vector<std::wstring> dropped_;
    std::wstring problem_;
    // This GTG's id among its pairs, once one is made; 0 before.
    std::uint64_t self_id_{};
    hostlog::Writer log_;
    std::optional<std::uint64_t> last_log_ms_;
};

}  // namespace gtg::tray
