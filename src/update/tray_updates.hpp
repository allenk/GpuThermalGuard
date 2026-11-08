#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "update/decision.hpp"
#include "update/hosts_trigger.hpp"
#include "update/hosts_updates.hpp"
#include "update/hosts_wire.hpp"
#include "update/manifest.hpp"
#include "update/preference.hpp"
#include "update/safe_moment.hpp"
#include "update/source.hpp"

// The updater as the Tray runs it: checks on a schedule, the tray menu's
// entries, consent, staging, the wait for a safe moment, the hand-off to the
// script and, after the restart, what came of it.
//
// Everything that reads the source or the disk runs on a short-lived thread
// in background mode, one at a time; the UI takes its result with a try-lock on
// the 200 ms tick and never waits. Nothing here touches NVML or protection: the
// Tray hands in what it already knows (SafeInputs) and, once the script has
// the update, is asked to exit the ordinary way.
namespace gtg::update {

struct TrayHost {
    HWND window{};
    DWORD supervisor_pid{};
    Version version;
    // "overlay" or "no-overlay", as this build was compiled.
    std::string variant;
    std::function<void(const std::wstring& title, const std::wstring& text, DWORD icon)> balloon;
    // The ordinary Exit: hosts told, protection stopped, positions saved.
    std::function<void()> exit;
};

// A paired host's request that this GTG update, as the Hosts table had it.
struct HostsRequest {
    std::uint64_t sender{};
    std::wstring name;
    hosts::Trigger trigger;
};

class TrayUpdates {
public:
    TrayUpdates() = default;

    ~TrayUpdates() { Shutdown(); }

    TrayUpdates(const TrayUpdates&) = delete;
    TrayUpdates& operator=(const TrayUpdates&) = delete;

    void Start(TrayHost host);
    void Tick(std::uint64_t now_ms, const SafeInputs& safe);
    // The update entries, under a separator, for the tray menu.
    void AddMenuItems(HMENU menu) const;
    // True when the command was one of the update entries.
    bool OnCommand(UINT id);
    // At most 2 s: a thread still reading a stalled share is left to finish
    // on its own.
    void Shutdown() noexcept;

    // A paired host asks this GTG to update to the release its own source
    // names. Repeats of a request, and requests already answered, are ignored;
    // the answer goes out in the status.
    void OnHostsRequest(const HostsRequest& request);
    // This GTG's update status, which paired hosts are sent with its state.
    [[nodiscard]] hosts::Status HostsStatus() const;

    // Whether paired hosts may ask this machine to update.
    [[nodiscard]] bool HostsRequestsAllowed() const noexcept { return preference_.hosts_trigger; }

    // Kept in HKLM: it decides whether others may make this machine run new
    // code as admin.
    [[nodiscard]] bool AllowHostsRequests(bool on, std::wstring& error);
    // The release this machine's source named at its last check.
    [[nodiscard]] std::optional<SourceView> SourceRelease() const;
    // A check without notices, for the Hosts dialog; only while idle.
    void CheckQuietly();
    // The tray tooltip's line while the tray icon carries the update badge:
    // the source offers a newer release, or the reader's update to it is on
    // its way. Empty when there is no badge.
    [[nodiscard]] std::wstring BadgeLine() const;

    // What the worker thread hands back, defined with it.
    struct Shared;

private:
    enum class State {
        Off,
        Idle,
        Checking,
        Staging,
        Waiting,
        HandingOff,
        Exiting,
        // This build was started by an update script; its outcome is pending.
        Settling,
    };

    struct Consent {
        Digest release{};
        bool reinstall{};
    };

    void Launch(std::function<void(Shared&)> job, bool background);
    void TakeResult(std::uint64_t now_ms);
    void StartCheck(bool manual);
    void StartStage(Consent consent);
    void StartHandOff();
    bool Confirm(bool reinstall);
    // Why this machine cannot be updated at all, if it cannot: the same for
    // the reader's consent and a paired host's request.
    [[nodiscard]] std::optional<std::pair<hosts::Answer, std::wstring>> Refusal() const;
    void ContinueRequest();
    void Answer(std::uint64_t trigger, hosts::Answer answer);
    void EndRequest(hosts::Answer answer);
    void SettleRequest(hosts::Answer answer);
    void OpenSettings();
    // True when the update succeeded.
    bool ReportOutcome();
    void Balloon(const std::wstring& text, DWORD icon) const;
    void Notice(std::wstring text, DWORD icon);
    [[nodiscard]] const ManifestVariant* Variant() const noexcept;
    // Where this machine's updates come from: the administrator's folder, or
    // GTG's public releases when none is set.
    [[nodiscard]] UpdateSource Source() const;

    // Off only when the administrator set a source that is not valid: then
    // nothing is read rather than reading somewhere else.
    [[nodiscard]] bool HasSource() const noexcept { return !preference_.source_invalid; }
    [[nodiscard]] std::optional<Decision> Current() const noexcept;
    [[nodiscard]] std::wstring StatusLine() const;

    TrayHost host_;
    UpdatePreference preference_;
    State state_{State::Off};
    Wait wait_{Wait::None};
    // From the last safe-moment inputs: told to the next build through the plan.
    bool no_gpu_{};
    std::filesystem::path executable_;
    std::filesystem::path install_;
    std::filesystem::path work_;
    std::int64_t linked_unix_ms_{};
    std::optional<Digest> running_build_;
    std::optional<Manifest> manifest_;
    std::wstring problem_;
    bool check_failing_{};
    std::uint64_t next_check_ms_{};
    std::uint64_t settle_until_ms_{};
    std::uint64_t next_poll_ms_{};
    Consent consent_;
    std::optional<StageResult> staged_;
    // A notice found at start, shown once the tray icon exists.
    std::wstring pending_notice_;
    DWORD pending_icon_{};
    std::uint64_t pending_after_ms_{};
    std::shared_ptr<Shared> shared_;
    std::thread worker_;
    // A paired host's request being carried out, and since when its update
    // has waited for a safe moment.
    std::optional<HostsRequest> request_;
    std::uint64_t request_waiting_since_ms_{};
    // The last request answered, as the status says.
    std::uint64_t answered_{};
    hosts::Answer answer_{hosts::Answer::None};
    // Requests already seen: each is repeated for a few seconds and stays in
    // the hosts table after that. Starts with the ones acted on before.
    SeenTriggers seen_;
    bool folder_writable_{};
};

}  // namespace gtg::update
