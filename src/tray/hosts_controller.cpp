#include "tray/hosts_controller.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <utility>

#include "gtg_version.h"
#include "localization/localization.hpp"
#include "logging/logger.hpp"
#include "peer/protocol.hpp"

namespace gtg::tray {

void HostsController::Initialize() {
    preference_ = settings::LoadHostsPreference();
    self_id_ = preference_.peer_id;
    if (preference_.log) (void)StartLog();
    if (preference_.enabled) (void)Start();
}

void HostsController::Shutdown() noexcept {
    link_.Stop();
    last_publish_ms_.reset();
}

bool HostsController::Start() {
    if (link_.Running()) return true;
    if (!store_loaded_) store_loaded_ = store_.Load();
    if (!store_loaded_) {
        problem_ =
            localization::Select(L"Windows 的加密元件無法使用，主機功能無法啟動。",
                                 L"Windows cryptography is unavailable; Hosts cannot start.");
        logging::Warning(L"hosts: CNG or DPAPI unavailable; not started");
        return false;
    }
    const std::uint64_t id = store_.EnsureSelfId();
    if (id == 0) {
        problem_ = localization::Select(L"無法建立本機的主機識別碼。",
                                        L"This machine's Hosts identity could not be made.");
        return false;
    }
    self_id_ = id;
    auto loaded = store_.LoadPairs();
    dropped_ = std::move(loaded.dropped);
    for (const std::wstring& name : dropped_) {
        logging::Warning(
            std::format(L"hosts: the key for \"{}\" could not be read; pair removed", name));
    }

    peer::Link::Options options;
    options.self = {id, Name()};
    options.port = static_cast<std::uint16_t>(Port());
    options.pairs = std::move(loaded.pairs);
    // On the link's thread. The store's calls are safe there: each opens its
    // own registry keys, and CNG and DPAPI may be called from any thread.
    options.save = [this](const peer::Pair& pair) {
        (void)store_.Save(pair);
    };
    options.forget = [this](const std::uint64_t peer) {
        (void)store_.Forget(peer);
    };
    if (!link_.Start(std::move(options))) {
        problem_ = localization::Select(L"主機功能的執行緒無法建立。",
                                        L"The Hosts thread could not be created.");
        return false;
    }
    problem_.clear();
    table_.reset();
    last_publish_ms_.reset();
    return true;
}

void HostsController::Suspend() {
    if (link_.Running()) link_.Suspend();
}

void HostsController::Resume() {
    if (link_.Running()) link_.Resume();
}

bool HostsController::SetOn(const bool on, std::wstring& error) {
    if (!settings::SaveHostsEnabled(on, error)) return false;
    preference_.enabled = on;
    if (on) {
        (void)Start();
    } else {
        Shutdown();
        table_.reset();
        problem_.clear();
    }
    logging::Info(on ? L"hosts: turned on" : L"hosts: turned off");
    return true;
}

bool HostsController::SetIdentity(const std::wstring& name, const int port, std::wstring& error) {
    const std::wstring cleaned = peer::CleanName(name);
    if (cleaned != preference_.name) {
        if (!settings::SaveHostsName(cleaned, error)) return false;
        preference_.name = cleaned;
        if (link_.Running()) link_.Rename(Name());
    }
    const int wanted = port == peer::kDefaultPort ? 0 : port;
    if (wanted != preference_.port) {
        if (!settings::SaveHostsPort(wanted, error)) return false;
        preference_.port = wanted;
        if (link_.Running()) {
            Shutdown();
            (void)Start();
        }
    }
    return true;
}

bool HostsController::SetLogOn(const bool on, std::wstring& error) {
    if (!settings::SaveHostsLog(on, error)) return false;
    preference_.log = on;
    if (on) {
        (void)StartLog();
    } else {
        StopLog();
    }
    logging::Info(on ? L"hosts log: turned on" : L"hosts log: turned off");
    return true;
}

void HostsController::StopLog() noexcept {
    log_.Stop();
    last_log_ms_.reset();
}

bool HostsController::StartLog() {
    if (log_.Running()) return true;
    // Beside the Tray journal, wherever that could be written.
    const std::filesystem::path journal = logging::Path();
    if (journal.empty()) {
        logging::Warning(L"hosts log: the journal has no directory; not started");
        return false;
    }
    hostlog::Writer::Options options;
    options.file = journal.parent_path() / hostlog::kFileName;
    const std::wstring file = options.file.wstring();
    if (!log_.Start(std::move(options))) {
        logging::Warning(L"hosts log: its thread could not be created; not started");
        return false;
    }
    last_log_ms_.reset();
    logging::Info(
        std::format(L"hosts log: writing {} every {} s", file, hostlog::kFrameIntervalMs / 1'000));
    return true;
}

void HostsController::LogTick(const std::uint64_t now_ms) {
    if (!log_.Running()) return;
    if (last_log_ms_ && now_ms - *last_log_ms_ < hostlog::kFrameIntervalMs) return;
    last_log_ms_ = now_ms;
    (void)log_.Submit(MakeFrame(now_ms));
}

// The table the dashboard draws from, as it stands on this tick. Only the
// fields a frame has go in: no endpoint, port or clock skew leaves the row.
hostlog::Frame HostsController::MakeFrame(const std::uint64_t now_ms) const {
    hostlog::Frame frame;
    frame.captured_at =
        std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    frame.now_ms = now_ms;
    frame.observer_id = self_id_;
    frame.observer_name = Name();
    constexpr int kVersion[] = {GTG_VERSION_COMMA};
    frame.version = {static_cast<std::uint8_t>(kVersion[0]), static_cast<std::uint8_t>(kVersion[1]),
                     static_cast<std::uint8_t>(kVersion[2])};
    frame.enabled = preference_.enabled;
    if (!preference_.enabled) {
        frame.status = hostlog::Status::Disabled;
    } else if (!link_.Running()) {
        frame.status = hostlog::Status::Unavailable;
    } else if (!table_) {
        frame.status = hostlog::Status::Starting;
    } else {
        switch (table_->status) {
            case peer::LinkStatus::Running:
            // Sending from an ephemeral port while the fixed one is taken: the
            // table is still the whole pair list.
            case peer::LinkStatus::PortInUse:
                frame.status = hostlog::Status::Running;
                break;
            case peer::LinkStatus::Starting:
                frame.status = hostlog::Status::Starting;
                break;
            default:
                frame.status = hostlog::Status::Unavailable;
                break;
        }
    }
    frame.table_complete = frame.status == hostlog::Status::Running;
    if (frame.table_complete) {
        frame.hosts.reserve(table_->hosts.size());
        for (const peer::HostRow& row : table_->hosts)
            frame.hosts.push_back({row.peer, row.name, row.heard, row.state});
    }
    return frame;
}

void HostsController::SaveOrder(const std::vector<std::uint64_t>& order) {
    if (store_loaded_) (void)store_.SaveOrder(order);
}

std::wstring HostsController::Name() const {
    return preference_.name.empty() ? ComputerName() : preference_.name;
}

int HostsController::Port() const noexcept {
    return preference_.port == 0 ? peer::kDefaultPort : preference_.port;
}

std::wstring HostsController::ComputerName() {
    wchar_t name[256]{};
    DWORD size = static_cast<DWORD>(std::size(name));
    if (GetComputerNameExW(ComputerNameDnsHostname, name, &size)) return std::wstring(name, size);
    size = static_cast<DWORD>(std::size(name));
    if (GetComputerNameW(name, &size)) return std::wstring(name, size);
    return L"GTG";
}

}  // namespace gtg::tray
