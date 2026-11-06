#include "update/tray_updates.hpp"

#include <shellapi.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <system_error>
#include <variant>

#include "localization/localization.hpp"
#include "logging/logger.hpp"
#include "update/files.hpp"
#include "update/handoff.hpp"
#include "update/hosts_trigger.hpp"
#include "update/json.hpp"
#include "update/plan.hpp"
#include "update/preconditions.hpp"
#include "update/settings_dialog.hpp"
#include "update/tray_badge.hpp"
#include "update/trust.hpp"
#include "update/update_resource.h"
#include "update/work_folder.hpp"

namespace gtg::update {
namespace {

constexpr std::uint64_t kFirstCheckMs = 2 * 60'000;
// How long a build started by an update script waits for the script's outcome.
constexpr std::uint64_t kSettleMs = 5 * 60'000;
// After start, so the tray icon exists before a notice is shown.
constexpr std::uint64_t kNoticeDelayMs = 3'000;
constexpr DWORD kStopWaitMs = 2'000;
constexpr std::uint64_t kMaxExecutableBytes = 256ULL * 1024 * 1024;

struct CheckDone {
    bool manual{};
    SourceManifest source;
    std::optional<Digest> build;
};

struct StageDone {
    std::optional<StageResult> staged;
    std::optional<Manifest> manifest;
    std::wstring problem;
    // The problem as a paired host's request is answered.
    hosts::Answer answer{hosts::Answer::StageFailed};
};

struct HandOffDone {
    HandOffResult result;
};

// monostate: the job ended on an exception, which only running out of memory
// can raise there.
using Result = std::variant<std::monostate, CheckDone, StageDone, HandOffDone>;

std::wstring Title() {
    return std::wstring(
        localization::Select(L"GPU Thermal Guard 更新", L"GPU Thermal Guard update"));
}

std::wstring Wide(const std::string_view ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

std::wstring FromUtf8(const std::string_view utf8) {
    if (utf8.empty()) return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
}

std::wstring VersionText(const Version& version) {
    return Wide(FormatVersion(version));
}

std::wstring Short(const Digest& digest) {
    return Wide(ToHex(digest).substr(0, 8));
}

const wchar_t* CompareName(const Decision decision) {
    if (decision.below_floor) return L"below-floor";
    switch (decision.compare) {
        case Compare::SameBuild:
            return L"same-build";
        case Compare::Newer:
            return L"newer";
        case Compare::Older:
            return L"older";
    }
    return L"?";
}

std::wstring VerdictText(const SignerVerdict verdict) {
    switch (verdict) {
        case SignerVerdict::Pinned:
            return {};
        case SignerVerdict::Unsigned:
            return std::wstring(localization::Select(L"沒有簽章", L"not signed"));
        case SignerVerdict::Invalid:
            return std::wstring(localization::Select(L"簽章無效，檔案可能被改過",
                                                     L"the signature does not verify"));
        case SignerVerdict::OtherSigner:
            return std::wstring(localization::Select(
                L"簽署者不是釘選的憑證", L"signed by a certificate other than the pin"));
        case SignerVerdict::Unavailable:
            return std::wstring(
                localization::Select(L"無法驗證簽章", L"signatures cannot be checked"));
    }
    return {};
}

std::filesystem::path ExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// The PE time stamp of the running EXE: when it was linked.
std::int64_t LinkedUnixMs() {
    const auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return static_cast<std::int64_t>(headers->FileHeader.TimeDateStamp) * 1000;
}

std::int64_t NowUnixMs() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    // 100 ns intervals since 1601.
    return static_cast<std::int64_t>((ticks.QuadPart - 116'444'736'000'000'000ULL) / 10'000);
}

std::array<std::uint8_t, 3> WireVersion(const Version& version) {
    const auto part = [](const int value) {
        return static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
    };
    return {part(version.major), part(version.minor), part(version.patch)};
}

}  // namespace

struct TrayUpdates::Shared {
    std::mutex mutex;
    std::optional<Result> result;
};

void TrayUpdates::Launch(std::function<void(Shared&)> job, const bool background) {
    // The previous job delivered its result before this one could be asked
    // for, so its thread is ending.
    if (worker_.joinable()) worker_.join();
    if (!shared_) shared_ = std::make_shared<Shared>();
    worker_ = std::thread([shared = shared_, job = std::move(job), background] {
        if (background) (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        try {
            job(*shared);
        } catch (...) {
            std::scoped_lock lock(shared->mutex);
            shared->result = Result{};
        }
        if (background) (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    });
}

void TrayUpdates::Start(TrayHost host) {
    host_ = std::move(host);
    executable_ = ExecutablePath();
    install_ = executable_.parent_path();
    work_ = install_ / kWorkFolderName;
    linked_unix_ms_ = LinkedUnixMs();
    preference_ = LoadUpdatePreference();
    // The request whose update started this build is still repeated for a few
    // seconds, and a request recorded on the network can be replayed for two
    // minutes: none acted on before is answered again.
    for (const std::uint64_t id : preference_.recent_triggers)
        (void)seen_.First(id);
    (void)seen_.First(preference_.pending_trigger);
    if (preference_.source_invalid) {
        logging::Warning(
            L"update: the source in the registry is not a valid UNC path; nothing is checked "
            L"until it is corrected");
    } else {
        logging::Info(std::format(L"update: source {}", Describe(Source())));
    }
    if (preference_.pin_invalid) {
        logging::Warning(
            L"update: the signer pin in the registry is not 64 hex digits; "
            L"updates are refused until it is corrected");
    }
    folder_writable_ = InspectInstallFolder(install_).user_writable;
    const std::uint64_t now = GetTickCount64();
    state_ = HasSource() ? State::Idle : State::Off;
    next_check_ms_ = now + kFirstCheckMs;
    std::error_code error;
    if (std::filesystem::exists(work_ / kPlanFileName, error)) {
        if (UpdateRunning()) {
            // This build was started by the script, which is still waiting
            // for it to report ready; its outcome follows.
            state_ = State::Settling;
            settle_until_ms_ = now + kSettleMs;
            next_poll_ms_ = now;
            if (preference_.pending_trigger != 0) {
                Answer(preference_.pending_trigger, hosts::Answer::Accepted);
            }
            logging::Info(L"update: started by the update script; waiting for its outcome");
        } else {
            SettleRequest(hosts::Answer::Interrupted);
            logging::Warning(
                L"update: a plan was left by an update that did not finish; "
                L"the .old files beside the EXE are kept");
            (void)DeleteFileW((work_ / kPlanFileName).c_str());
            Notice(std::wstring(localization::Select(
                       L"上次的更新沒有完成。可以從 tray 選單「從來源重新安裝」再試一次。",
                       L"The last update did not finish. Reinstall from the source in the tray "
                       L"menu to try again.")),
                   NIIF_WARNING);
        }
    } else if (std::filesystem::exists(work_ / kOutcomeFileName, error) && !UpdateRunning()) {
        if (ReportOutcome()) next_check_ms_ = now;
    } else {
        if (!UpdateRunning()) SettleRequest(hosts::Answer::Interrupted);
        RemoveStagedExcept(work_, L"");
    }
}

// The paired host whose request started the update that just ended is told
// how it ended, once.
void TrayUpdates::SettleRequest(const hosts::Answer answer) {
    if (preference_.pending_trigger == 0) return;
    Answer(preference_.pending_trigger, answer);
    std::wstring problem;
    if (!SavePendingTrigger(0, problem)) logging::Warning(problem);
    preference_.pending_trigger = 0;
}

void TrayUpdates::Notice(std::wstring text, const DWORD icon) {
    pending_notice_ = std::move(text);
    pending_icon_ = icon;
    pending_after_ms_ = GetTickCount64() + kNoticeDelayMs;
}

void TrayUpdates::Balloon(const std::wstring& text, const DWORD icon) const {
    if (host_.balloon) host_.balloon(Title(), text, icon);
}

bool TrayUpdates::ReportOutcome() {
    const std::filesystem::path path = work_ / kOutcomeFileName;
    const ReadResult read = ReadBounded(path, 4096);
    (void)DeleteFileW(path.c_str());
    RemoveStagedExcept(work_, L"");
    if (read.status != ReadResult::Status::Ok) {
        SettleRequest(hosts::Answer::Interrupted);
        return false;
    }
    const json::ParseResult parsed = json::Parse(
        std::string_view(reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size()));
    if (!parsed.value || !parsed.value->IsObject()) {
        logging::Warning(L"update: the script's outcome could not be read");
        SettleRequest(hosts::Answer::Interrupted);
        return false;
    }
    if (const json::Value* value = parsed.value->Find("outcome"); value != nullptr) {
        SettleRequest(AnswerOfOutcome(value->IsString() ? std::string_view(value->text) : ""));
    } else {
        SettleRequest(hosts::Answer::Failed);
    }
    const json::Value& root = *parsed.value;
    auto text = [&](const char* key) {
        const json::Value* value = root.Find(key);
        return value != nullptr && value->IsString() ? FromUtf8(value->text) : std::wstring();
    };
    const std::wstring outcome = text("outcome");
    const std::wstring version = text("version");
    const std::wstring reason = text("reason");
    const json::Value* reinstall = root.Find("reinstall");
    const bool reinstalled = reinstall != nullptr && reinstall->boolean;
    logging::Info(std::format(L"update outcome={} version={} reinstall={} reason={}", outcome,
                              version, reinstalled, reason));
    if (outcome == L"updated") {
        Notice(reinstalled ? localization::Format(L"已從來源重新安裝 {}。",
                                                  L"Reinstalled {} from the source.", version)
                           : localization::Format(L"已更新到 {}。", L"Updated to {}.", version),
               NIIF_INFO);
    } else if (outcome == L"rolled_back") {
        Notice(
            localization::Format(L"新版沒有正常啟動，已回到 {}。詳情在 _gtg_update\\update.log。",
                                 L"The new build did not come up; back on {}. Details are in "
                                 L"_gtg_update\\update.log.",
                                 version),
            NIIF_WARNING);
    } else if (outcome == L"aborted") {
        Notice(localization::Format(L"更新沒有進行，仍是 {}：{}",
                                    L"Nothing was updated; still {}: {}", version, reason),
               NIIF_WARNING);
    } else {
        Notice(localization::Format(L"更新失敗：{}", L"The update failed: {}", reason), NIIF_ERROR);
    }
    return outcome == L"updated";
}

void TrayUpdates::Tick(const std::uint64_t now_ms, const SafeInputs& safe) {
    if (!pending_notice_.empty() && now_ms >= pending_after_ms_) {
        Balloon(pending_notice_, pending_icon_);
        pending_notice_.clear();
    }
    TakeResult(now_ms);
    switch (state_) {
        case State::Settling:
            if (now_ms >= next_poll_ms_) {
                next_poll_ms_ = now_ms + 1'000;
                std::error_code error;
                const bool done =
                    std::filesystem::exists(work_ / kOutcomeFileName, error) && !UpdateRunning();
                if (done || now_ms >= settle_until_ms_) {
                    bool updated = false;
                    if (done) {
                        updated = ReportOutcome();
                    } else {
                        logging::Warning(L"update: no outcome from the script within 5 minutes");
                        SettleRequest(hosts::Answer::Interrupted);
                    }
                    state_ = HasSource() ? State::Idle : State::Off;
                    // After an update, at once: paired hosts learn from the
                    // status that this machine runs the source's release.
                    next_check_ms_ = updated ? now_ms : now_ms + kFirstCheckMs;
                }
            }
            break;
        case State::Idle:
            if (preference_.auto_check && now_ms >= next_check_ms_) StartCheck(false);
            break;
        case State::Waiting:
            if (request_ && now_ms - request_waiting_since_ms_ >= kTriggerWaitMs) {
                state_ = State::Idle;
                staged_.reset();
                logging::Info(L"update: no safe moment within 30 minutes; the request is given up");
                Balloon(localization::Format(
                            L"{} 要求的更新等了 30 分鐘都沒有安全時機，已取消。",
                            L"The update {} asked for found no safe moment in 30 minutes and was "
                            L"given up.",
                            request_->name),
                        NIIF_INFO);
                EndRequest(hosts::Answer::GaveUp);
                break;
            }
            wait_ = SafeMoment(safe);
            if (wait_ == Wait::None) StartHandOff();
            break;
        default:
            break;
    }
}

void TrayUpdates::TakeResult(const std::uint64_t now_ms) {
    if (!shared_) return;
    std::optional<Result> result;
    {
        std::unique_lock lock(shared_->mutex, std::try_to_lock);
        if (!lock.owns_lock() || !shared_->result) return;
        result.swap(shared_->result);
    }
    if (worker_.joinable()) worker_.join();

    if (std::holds_alternative<std::monostate>(*result)) {
        logging::Warning(L"update: a background step ended unexpectedly");
        state_ = HasSource() ? State::Idle : State::Off;
        if (request_) EndRequest(hosts::Answer::StageFailed);
        return;
    }

    if (auto* check = std::get_if<CheckDone>(&*result)) {
        [&] {
            state_ = State::Idle;
            next_check_ms_ = now_ms + NextCheckMs(Source().kind, !check->source.manifest);
            if (check->build) running_build_ = check->build;
            if (!check->source.manifest) {
                manifest_.reset();
                problem_ = check->source.problem;
                if (!check_failing_) {
                    logging::Warning(std::format(L"update check failed: {} {}", problem_,
                                                 Wide(check->source.detail)));
                }
                check_failing_ = true;
                if (check->manual) Balloon(problem_, NIIF_WARNING);
                return;
            }
            if (check_failing_) logging::Info(L"update check: the source can be read again");
            check_failing_ = false;
            const bool changed =
                !manifest_ || manifest_->release != check->source.manifest->release;
            manifest_ = std::move(check->source.manifest);
            problem_.clear();
            const std::wstring version = VersionText(manifest_->version);
            const ManifestVariant* variant = Variant();
            const auto decision = Current();
            if (variant == nullptr || !decision) {
                problem_ = variant == nullptr ? localization::Format(
                                                    L"來源沒有這台需要的 {} 版本",
                                                    L"The source has no {} build for this machine",
                                                    Wide(host_.variant))
                                              : std::wstring(localization::Select(
                                                    L"無法讀取這台 GTG 自己的 build",
                                                    L"This GTG's own build cannot be read"));
                if (check->manual) Balloon(problem_, NIIF_WARNING);
                return;
            }
            if (changed || check->manual) {
                logging::Info(std::format(L"update check: source {} release={} build={} is {}",
                                          version, Short(manifest_->release), Short(variant->build),
                                          CompareName(*decision)));
            }
            if (OffersUpdate(*decision)) {
                const std::string release = ToHex(manifest_->release);
                if (check->manual || preference_.notified_release != release) {
                    Balloon(
                        localization::Format(L"有新版本 {}。從 tray 選單的「更新到 {}...」開始。",
                                             L"Version {} is available. Start it from \"Update to "
                                             L"{}...\" in the tray menu.",
                                             version, version),
                        NIIF_INFO);
                }
                if (!check->manual && preference_.notified_release != release) {
                    preference_.notified_release = release;
                    std::wstring error;
                    if (!SaveNotifiedRelease(release, error)) logging::Warning(error);
                }
            } else if (check->manual) {
                if (decision->below_floor) {
                    Balloon(
                        localization::Format(L"來源上的 {} 早於 1.2.0，不能安裝。",
                                             L"The source's {} is older than 1.2.0 and cannot be "
                                             L"installed.",
                                             version),
                        NIIF_WARNING);
                } else if (decision->compare == Compare::SameBuild) {
                    Balloon(localization::Format(L"已是最新（{}）。", L"Up to date ({}).", version),
                            NIIF_INFO);
                } else {
                    Balloon(
                        localization::Format(L"來源上的 {} 比這台舊；可以從 tray 選單重新安裝。",
                                             L"The source's {} is older than this build; it can "
                                             L"be reinstalled from the tray menu.",
                                             version),
                        NIIF_INFO);
                }
            }
        }();
        if (request_ && state_ == State::Idle) ContinueRequest();
        return;
    }

    if (auto* stage = std::get_if<StageDone>(&*result)) {
        if (!stage->problem.empty() || !stage->staged || !stage->manifest) {
            state_ = State::Idle;
            logging::Warning(std::format(L"update not staged: {}", stage->problem));
            Balloon(stage->problem, NIIF_WARNING);
            if (request_) EndRequest(stage->answer);
            return;
        }
        staged_ = std::move(stage->staged);
        manifest_ = std::move(stage->manifest);
        state_ = State::Waiting;
        wait_ = Wait::None;
        request_waiting_since_ms_ = now_ms;
        logging::Info(std::format(
            L"update staged: {} release={} files={}; waiting for a safe moment",
            VersionText(manifest_->version), Short(manifest_->release), staged_->files.size()));
        Balloon(localization::Format(
                    L"{} 已準備好。GPU 冷卻、沒有遊戲使用 overlay 時，GTG 會重新啟動完成更新。",
                    L"{} is ready. Once the GPU has cooled and no game uses the overlay, GTG "
                    L"restarts to finish the update.",
                    VersionText(manifest_->version)),
                NIIF_INFO);
        return;
    }

    if (auto* hand = std::get_if<HandOffDone>(&*result)) {
        if (!hand->result.ok) {
            state_ = State::Idle;
            staged_.reset();
            logging::Warning(std::format(L"update hand-off failed: {}", hand->result.problem));
            Balloon(hand->result.problem, NIIF_WARNING);
            if (request_) {
                std::wstring problem;
                if (!SavePendingTrigger(0, problem)) logging::Warning(problem);
                preference_.pending_trigger = 0;
                EndRequest(hosts::Answer::HandOffFailed);
            }
            return;
        }
        logging::Info(std::format(L"update handed to the script pid={} in_job={}; GTG exits for it",
                                  hand->result.script_pid, hand->result.in_job));
        state_ = State::Exiting;
        if (host_.exit) host_.exit();
    }
}

const ManifestVariant* TrayUpdates::Variant() const noexcept {
    return manifest_ ? manifest_->Find(host_.variant) : nullptr;
}

UpdateSource TrayUpdates::Source() const {
    return SourceOf(preference_.source, L"GpuThermalGuard/" + VersionText(host_.version));
}

std::optional<Decision> TrayUpdates::Current() const noexcept {
    const ManifestVariant* variant = Variant();
    if (variant == nullptr || !running_build_) return std::nullopt;
    Running running;
    running.version = host_.version;
    running.build = *running_build_;
    running.linked_unix_ms = linked_unix_ms_;
    return Decide(running, *manifest_, *variant);
}

void TrayUpdates::StartCheck(const bool manual) {
    state_ = State::Checking;
    Launch(
        [manual, source = Source(), executable = executable_,
         need_build = !running_build_.has_value()](Shared& shared) {
            CheckDone done;
            done.manual = manual;
            const Sha256 sha;
            if (need_build) {
                const ReadResult self = ReadBounded(executable, kMaxExecutableBytes);
                if (self.status == ReadResult::Status::Ok) done.build = sha.Of(self.bytes);
            }
            done.source = ReadManifest(source, sha);
            std::scoped_lock lock(shared.mutex);
            shared.result = std::move(done);
        },
        true);
}

void TrayUpdates::StartStage(const Consent consent) {
    state_ = State::Staging;
    consent_ = consent;
    staged_.reset();
    Launch(
        [consent, source = Source(), variant = host_.variant, pin = preference_.pin,
         work = work_](Shared& shared) {
            StageDone done;
            const Sha256 sha;
            DWORD error = 0;
            SourceManifest read;
            const ManifestVariant* found = nullptr;
            if (ServiceInstalled()) {
                done.answer = hosts::Answer::ServiceInstalled;
                done.problem = localization::Select(
                    L"這台安裝了 GTG Service；目前的更新不替換 Service，所以不進行。",
                    L"The GTG Service is installed here, and updates do not replace it yet.");
            } else if (SmartAppControlState() == 1) {
                done.answer = hosts::Answer::SmartAppControl;
                done.problem = localization::Select(
                    L"Smart App Control 已開啟：它會擋下新版，連回到舊版也會被擋，所以不進行。",
                    L"Smart App Control is on: it would block the new build, and the way back "
                    L"too.");
            } else if (UpdateRunning()) {
                done.answer = hosts::Answer::Busy;
                done.problem = localization::Select(L"已有另一個更新正在進行。",
                                                    L"Another update is running.");
            } else if (!SecureFolder(work, kWorkFolderSddl, error)) {
                done.problem =
                    localization::Format(L"無法建立更新資料夾（Win32 {}）",
                                         L"The update folder cannot be made (Win32 {})", error);
            } else if (read = ReadManifest(source, sha); !read.manifest) {
                done.answer = hosts::Answer::Unreadable;
                done.problem = read.problem;
            } else if (read.manifest->release != consent.release) {
                done.answer = hosts::Answer::OtherRelease;
                done.problem = localization::Select(
                    L"來源的版本在你同意之後變了；請重新檢查後再試。",
                    L"The source changed after you agreed; check again and retry.");
            } else if (found = read.manifest->Find(variant); found == nullptr) {
                done.answer = hosts::Answer::NoVariant;
                done.problem = localization::Format(L"來源沒有這台需要的 {} 版本",
                                                    L"The source has no {} build for this machine",
                                                    Wide(variant));
            } else if (read.manifest->version < kFloor) {
                done.answer = hosts::Answer::BelowFloor;
                done.problem = localization::Select(L"來源上的版本早於 1.2.0，不能安裝。",
                                                    L"The source's version is older than 1.2.0.");
            }
            if (done.problem.empty()) {
                StageResult staged = Stage(source, *read.manifest, *found, work, sha);
                if (!staged.Ok()) done.problem = staged.problem;
                for (const StagedFile& file : staged.files) {
                    if (!done.problem.empty() || !pin) break;
                    const SignerResult signer = VerifySigner(file.path, *pin, sha);
                    if (signer.verdict != SignerVerdict::Pinned) {
                        done.answer = hosts::Answer::SignatureRefused;
                        done.problem = localization::Format(L"{}：{}", L"{}: {}", Wide(file.name),
                                                            VerdictText(signer.verdict));
                    }
                }
                if (done.problem.empty()) {
                    done.staged = std::move(staged);
                    done.manifest = std::move(read.manifest);
                }
            }
            std::scoped_lock lock(shared.mutex);
            shared.result = std::move(done);
        },
        true);
}

void TrayUpdates::StartHandOff() {
    const ManifestVariant* variant = Variant();
    const auto script = EmbeddedScript(GetModuleHandleW(nullptr));
    if (!staged_ || variant == nullptr || !running_build_ || !script || host_.supervisor_pid == 0) {
        state_ = State::Idle;
        staged_.reset();
        logging::Warning(L"update: the hand-off cannot be prepared");
        Balloon(std::wstring(localization::Select(L"無法準備更新；請重新檢查後再試。",
                                                  L"The update cannot be prepared; check again.")),
                NIIF_WARNING);
        if (request_) EndRequest(hosts::Answer::HandOffFailed);
        return;
    }
    Plan plan;
    plan.install = install_;
    plan.entry = manifest_->entry;
    plan.old_version = FormatVersion(host_.version);
    plan.old_build = ToHex(*running_build_);
    plan.new_version = FormatVersion(manifest_->version);
    plan.new_build = ToHex(variant->build);
    plan.release = ToHex(manifest_->release);
    plan.reinstall = consent_.reinstall;
    for (const StagedFile& file : staged_->files) {
        plan.files.push_back({file.name, file.path, file.size, file.sha256});
    }
    const HANDLE supervisor =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, host_.supervisor_pid);
    plan.wait.push_back({host_.supervisor_pid, ProcessCreated(supervisor)});
    if (supervisor != nullptr) CloseHandle(supervisor);
    plan.wait.push_back({GetCurrentProcessId(), ProcessCreated(GetCurrentProcess())});
    state_ = State::HandingOff;
    if (request_) {
        // Read by the build the script starts, to report the outcome against it.
        std::wstring problem;
        if (!SavePendingTrigger(request_->trigger.id, problem)) logging::Warning(problem);
        preference_.pending_trigger = request_->trigger.id;
    }
    logging::Info(std::format(L"update: safe moment reached; handing {} to the script",
                              VersionText(manifest_->version)));
    Launch(
        [work = work_, plan = std::move(plan), text = std::string(*script),
         parent = host_.supervisor_pid](Shared& shared) {
            HandOffDone done;
            done.result = HandOff(work, plan, text, parent);
            std::scoped_lock lock(shared.mutex);
            shared.result = std::move(done);
        },
        false);
}

bool TrayUpdates::Confirm(const bool reinstall) {
    const ManifestVariant* variant = Variant();
    const auto decision = Current();
    if (variant == nullptr || !decision) return false;
    if (const auto refusal = Refusal()) {
        MessageBoxW(host_.window, refusal->second.c_str(), Title().c_str(), MB_OK | MB_ICONWARNING);
        return false;
    }
    const InstallFolder folder = InspectInstallFolder(install_);
    const std::wstring version = VersionText(manifest_->version);
    const bool downgrade = reinstall && decision->compare == Compare::Older;
    std::wstring text = !reinstall ? localization::Format(L"更新到 {}？", L"Update to {}?", version)
                        : downgrade
                            ? localization::Format(L"降版到 {}？", L"Downgrade to {}?", version)
                            : localization::Format(L"從來源重新安裝 {}？",
                                                   L"Reinstall {} from the source?", version);
    const UpdateSource source = Source();
    text += localization::Format(L"\n\n來源：{}\n發佈時間：{}", L"\n\nSource: {}\nPublished: {}",
                                 Describe(source), Wide(manifest_->published));
    if (!manifest_->notes.empty()) text += L"\n\n" + manifest_->notes;
    text += localization::Select(
        L"\n\nGTG 會等到 GPU 比觸發溫度低 15 °C，而且沒有遊戲使用 overlay 時才更新。屆時 GTG "
        L"會重新啟動，保護暫停幾秒；新版沒有正常啟動時，會自動回到目前的版本。",
        L"\n\nGTG waits until the GPU is 15 °C below its trigger and no game uses the overlay. "
        L"It then restarts, so protection pauses for a few seconds; if the new build does not "
        L"come up, the current one is put back.");
    if (downgrade) {
        text += localization::Select(
            L"\n\n這是降版：舊版不認得新版才加入的設定，會改用預設值；如果舊版的 Hosts 協定較舊，"
            L"這台可能會從其他主機的列表上消失。",
            L"\n\nThis is a downgrade: settings added after that version are not read and their "
            L"defaults apply, and with an older Hosts protocol this machine may drop off other "
            L"hosts' lists.");
    }
    if (folder.user_writable) {
        text += localization::Select(
            L"\n\n注意：GTG 所在的資料夾，一般程式也能寫入；不論是否更新，任何程式都能換掉這裡的 "
            L"GTG。建議放在 C:\\Program Files 之類的位置。",
            L"\n\nNote: programs without administrator rights can write to GTG's folder, so any "
            L"of them could replace GTG there, update or not. A place such as C:\\Program Files "
            L"is safer.");
    }
    if (!preference_.pin && source.kind == UpdateSource::Kind::Releases) {
        text += localization::Select(
            L"\n\n公開版沒有程式碼簽章：這台信任 GitHub 上這個專案的發佈，透過 HTTPS 下載，並逐一"
            L"比對發佈清單裡的 SHA-256，跟自己下載 zip 一樣。",
            L"\n\nPublic releases are not code-signed: this machine trusts the project's releases "
            L"on GitHub, downloaded over HTTPS and checked against the SHA-256 of each file in the "
            L"release's manifest, as when you download the zip yourself.");
    } else if (!preference_.pin) {
        text += localization::Select(
            L"\n\n沒有設定簽章釘選：任何能寫入更新來源的人，都能發佈這台會安裝的版本。",
            L"\n\nNo signer pin is set: anyone who can write to the update source can publish "
            L"what this machine installs.");
    }
    return MessageBoxW(host_.window, text.c_str(), Title().c_str(), MB_YESNO | MB_ICONQUESTION) ==
           IDYES;
}

void TrayUpdates::OpenSettings() {
    const std::wstring this_build = localization::Format(
        L"這台：{}（{}），build {}", L"This machine: {} ({}), build {}", VersionText(host_.version),
        Wide(host_.variant), running_build_ ? Short(*running_build_) : std::wstring(L"?"));
    const std::wstring source_release =
        manifest_ ? localization::Format(L"來源：{}，release {}", L"Source: {}, release {}",
                                         VersionText(manifest_->version), Short(manifest_->release))
        : problem_.empty()
            ? std::wstring(localization::Select(L"來源：尚未檢查", L"Source: not checked yet"))
            : localization::Format(L"來源：{}", L"Source: {}", problem_);
    UpdateSettingsDialog dialog(preference_, this_build, source_release);
    if (dialog.DoModal(host_.window) != IDOK) return;
    const std::wstring old_source = preference_.source;
    preference_ = LoadUpdatePreference();
    logging::Info(std::format(L"update settings saved; source={} auto_check={} pin={}",
                              HasSource() ? Describe(Source()) : std::wstring(L"(invalid)"),
                              preference_.auto_check, preference_.pin.has_value()));
    if (preference_.source != old_source) {
        manifest_.reset();
        problem_.clear();
        check_failing_ = false;
    }
    if (state_ == State::Off || state_ == State::Idle) {
        state_ = HasSource() ? State::Idle : State::Off;
        if (state_ == State::Idle) StartCheck(true);
    }
}

std::wstring TrayUpdates::StatusLine() const {
    const std::wstring version = manifest_ ? VersionText(manifest_->version) : std::wstring();
    switch (state_) {
        case State::Checking:
            return std::wstring(
                localization::Select(L"正在檢查更新...", L"Checking for updates..."));
        case State::Staging:
            return localization::Format(L"正在準備 {}...", L"Preparing {}...", version);
        case State::Waiting:
            switch (wait_) {
                case Wait::Protection:
                    return localization::Format(L"{}：等待保護回到待命",
                                                L"{}: waiting for protection to be armed", version);
                case Wait::Temperature:
                    return localization::Format(L"{}：等待 GPU 冷卻",
                                                L"{}: waiting for the GPU to cool", version);
                case Wait::Overlay:
                    return localization::Format(L"{}：等待遊戲 overlay 結束",
                                                L"{}: waiting for the game overlay to end",
                                                version);
                case Wait::None:
                    return localization::Format(L"{}：即將開始", L"{}: starting", version);
            }
            return {};
        case State::HandingOff:
        case State::Exiting:
            return std::wstring(localization::Select(L"正在交給更新程式...", L"Handing over..."));
        case State::Settling:
            return std::wstring(
                localization::Select(L"正在確認更新結果...", L"Confirming the update..."));
        case State::Idle: {
            if (!problem_.empty())
                return localization::Format(L"更新：{}", L"Update: {}", problem_);
            const auto decision = Current();
            if (!decision) return {};
            if (OffersUpdate(*decision)) {
                return localization::Format(L"有新版本 {}", L"Version {} is available", version);
            }
            if (decision->compare == Compare::SameBuild) {
                return localization::Format(L"已是最新（{}）", L"Up to date ({})", version);
            }
            return localization::Format(L"來源上是 {}", L"The source has {}", version);
        }
        case State::Off:
            return {};
    }
    return {};
}

std::wstring TrayUpdates::BadgeLine() const {
    if (!manifest_) return {};
    BadgeState badge;
    switch (state_) {
        case State::Idle:
        // A scheduled check keeps what the last one found until it ends.
        case State::Checking:
            badge.decision = Current();
            badge.problem = !problem_.empty();
            break;
        case State::Staging:
        case State::Waiting:
        case State::HandingOff:
        case State::Exiting:
            badge.updating = true;
            badge.reinstall = consent_.reinstall;
            badge.requested = request_.has_value();
            break;
        case State::Off:
        case State::Settling:
            return {};
    }
    if (!ShowsBadge(badge)) return {};
    return localization::Format(L"有新版本 {}", L"Version {} is available",
                                VersionText(manifest_->version));
}

void TrayUpdates::AddMenuItems(const HMENU menu) const {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (const std::wstring status = StatusLine(); !status.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
    }
    const bool idle = state_ == State::Idle;
    if (state_ != State::Off) {
        AppendMenuW(menu, MF_STRING | (idle ? MF_ENABLED : MF_GRAYED), IDM_TRAY_UPDATE_CHECK,
                    localization::Select(L"檢查更新", L"Check for Updates").data());
    }
    if (const auto decision = Current(); idle && decision) {
        const std::wstring version = VersionText(manifest_->version);
        if (OffersUpdate(*decision)) {
            const std::wstring label =
                localization::Format(L"更新到 {}...", L"Update to {}...", version);
            AppendMenuW(menu, MF_STRING, IDM_TRAY_UPDATE_APPLY, label.c_str());
        } else if (OffersReinstall(*decision)) {
            const std::wstring label = localization::Format(
                L"從來源重新安裝 {}...", L"Reinstall {} from Source...", version);
            AppendMenuW(menu, MF_STRING, IDM_TRAY_UPDATE_REINSTALL, label.c_str());
        }
    }
    if (state_ == State::Waiting) {
        AppendMenuW(menu, MF_STRING, IDM_TRAY_UPDATE_CANCEL,
                    localization::Select(L"取消更新", L"Cancel Update").data());
    }
    const bool busy =
        state_ == State::Staging || state_ == State::HandingOff || state_ == State::Exiting;
    AppendMenuW(menu, MF_STRING | (busy ? MF_GRAYED : MF_ENABLED), IDM_TRAY_UPDATE_SETTINGS,
                localization::Select(L"更新設定...", L"Update Settings...").data());
}

bool TrayUpdates::OnCommand(const UINT id) {
    switch (id) {
        case IDM_TRAY_UPDATE_CHECK:
            if (state_ == State::Idle) StartCheck(true);
            return true;
        case IDM_TRAY_UPDATE_APPLY:
        case IDM_TRAY_UPDATE_REINSTALL: {
            if (state_ != State::Idle || !manifest_) return true;
            const bool reinstall = id == IDM_TRAY_UPDATE_REINSTALL;
            const Digest release = manifest_->release;
            if (Confirm(reinstall)) {
                // The dialog ran a message loop of its own, in which a check
                // or a paired host's request may have started.
                if (state_ != State::Idle || !manifest_ || manifest_->release != release) {
                    Balloon(std::wstring(localization::Select(
                                L"確認的時候更新的狀態變了；請重新檢查後再試。",
                                L"The update changed while you were confirming; check again and "
                                L"retry.")),
                            NIIF_WARNING);
                    return true;
                }
                logging::Info(std::format(L"update: the reader agreed to {} {}",
                                          reinstall ? L"reinstall" : L"update to",
                                          VersionText(manifest_->version)));
                StartStage({release, reinstall});
            }
            return true;
        }
        case IDM_TRAY_UPDATE_CANCEL:
            if (state_ == State::Waiting) {
                state_ = State::Idle;
                staged_.reset();
                logging::Info(L"update: cancelled by the reader while waiting");
                if (request_) EndRequest(hosts::Answer::Cancelled);
            }
            return true;
        case IDM_TRAY_UPDATE_SETTINGS:
            OpenSettings();
            return true;
        default:
            return false;
    }
}

std::optional<std::pair<hosts::Answer, std::wstring>> TrayUpdates::Refusal() const {
    const InstallFolder folder = InspectInstallFolder(install_);
    if (folder.remote) {
        return std::pair{
            hosts::Answer::FolderRefused,
            std::wstring(localization::Select(
                L"GTG 是從網路位置執行的。請把 GTG 複製到這台電腦上再更新。",
                L"GTG runs from a network location. Copy it to this computer before updating."))};
    }
    if (!folder.acls) {
        return std::pair{
            hosts::Answer::FolderRefused,
            std::wstring(localization::Select(
                L"GTG 所在的磁碟不支援權限設定，無法安全地更新。",
                L"GTG's drive keeps no access lists, so it cannot be updated safely."))};
    }
    if (manifest_ &&
        _wcsicmp(executable_.filename().c_str(), Wide(manifest_->entry).c_str()) != 0) {
        return std::pair{
            hosts::Answer::EntryMismatch,
            localization::Format(L"GTG 的檔名不是 {}，無法更新。",
                                 L"GTG's file name is not {}, so it cannot be updated.",
                                 Wide(manifest_->entry))};
    }
    if (preference_.pin_invalid) {
        return std::pair{hosts::Answer::PinInvalid,
                         std::wstring(localization::Select(
                             L"更新設定裡的簽章釘選無效。",
                             L"The signer pin in the update settings is not valid."))};
    }
    return std::nullopt;
}

void TrayUpdates::Answer(const std::uint64_t trigger, const hosts::Answer answer) {
    answered_ = trigger;
    answer_ = answer;
}

void TrayUpdates::EndRequest(const hosts::Answer answer) {
    if (!request_) return;
    logging::Info(
        std::format(L"update: the request from {} ended: {}", request_->name, AnswerName(answer)));
    Answer(request_->trigger.id, answer);
    request_.reset();
}

void TrayUpdates::OnHostsRequest(const HostsRequest& request) {
    const std::uint64_t id = request.trigger.id;
    if (!seen_.First(id)) return;
    const bool busy = state_ == State::Staging || state_ == State::Waiting ||
                      state_ == State::HandingOff || state_ == State::Exiting ||
                      state_ == State::Settling;
    hosts::Answer answer = hosts::Answer::Accepted;
    if (preference_.hosts_trigger && busy && state_ != State::Settling &&
        consent_.release == request.trigger.release) {
        // Already on its way to that release.
        answer = hosts::Answer::Accepted;
    } else {
        TriggerGate gate;
        gate.allowed = preference_.hosts_trigger;
        gate.has_source = state_ != State::Off;
        // A check in flight is the one the request waits on.
        gate.idle = state_ == State::Idle || state_ == State::Checking;
        gate.last_unix_ms = preference_.last_trigger_unix_ms;
        answer = AdmitTrigger(gate, NowUnixMs(), request.trigger.force);
    }
    logging::Info(std::format(L"update: {} asked for release {}{}; {}",
                              request.name.empty() ? L"a paired host" : request.name,
                              Short(request.trigger.release),
                              request.trigger.force ? L" (forced)" : L"", AnswerName(answer)));
    Answer(id, answer);
    if (answer != hosts::Answer::Accepted || busy) return;
    request_ = request;
    if (request_->name.empty()) request_->name = L"?";
    if (state_ == State::Idle) StartCheck(false);
}

void TrayUpdates::ContinueRequest() {
    const HostsRequest request = *request_;
    if (!manifest_) {
        EndRequest(hosts::Answer::Unreadable);
        return;
    }
    if (Variant() == nullptr) {
        EndRequest(hosts::Answer::NoVariant);
        return;
    }
    const auto decision = Current();
    if (!decision) {
        EndRequest(hosts::Answer::Unreadable);
        return;
    }
    hosts::Answer answer = DecideTrigger(request.trigger, manifest_->release, *decision);
    if (answer == hosts::Answer::Accepted) {
        if (const auto refusal = Refusal()) answer = refusal->first;
    }
    if (answer != hosts::Answer::Accepted) {
        EndRequest(answer);
        return;
    }
    const std::int64_t now = NowUnixMs();
    std::wstring problem;
    if (!SaveLastTrigger(now, problem)) logging::Warning(problem);
    preference_.last_trigger_unix_ms = now;
    preference_.recent_triggers =
        KeepTrigger(std::move(preference_.recent_triggers), request.trigger.id);
    if (!SaveRecentTriggers(preference_.recent_triggers, problem)) logging::Warning(problem);
    const bool reinstall = decision->compare != Compare::Newer;
    const std::wstring version = VersionText(manifest_->version);
    logging::Info(std::format(L"update: carrying out the request from {}: {} {}", request.name,
                              reinstall ? L"reinstall" : L"update to", version));
    Balloon(reinstall ? localization::Format(
                            L"{} 要求這台從來源重新安裝 {}。GPU 冷卻、沒有遊戲使用 overlay 時，"
                            L"GTG 會重新啟動完成。",
                            L"{} asked this machine to reinstall {} from the source. GTG restarts "
                            L"to do it once the GPU has cooled and no game uses the overlay.",
                            request.name, version)
                      : localization::Format(
                            L"{} 要求這台更新到 {}。GPU 冷卻、沒有遊戲使用 overlay 時，GTG "
                            L"會重新啟動完成。",
                            L"{} asked this machine to update to {}. GTG restarts to do it once "
                            L"the GPU has cooled and no game uses the overlay.",
                            request.name, version),
            NIIF_INFO);
    StartStage({manifest_->release, reinstall});
}

hosts::Status TrayUpdates::HostsStatus() const {
    hosts::Status status;
    status.has_source = HasSource();
    status.accepts_triggers = preference_.hosts_trigger;
    status.pin_set = preference_.pin.has_value();
    status.folder_writable = folder_writable_;
    switch (state_) {
        case State::Off:
            status.phase = hosts::Phase::Off;
            break;
        case State::Idle:
            status.phase = hosts::Phase::Idle;
            break;
        case State::Checking:
            status.phase = hosts::Phase::Checking;
            break;
        case State::Staging:
            status.phase = hosts::Phase::Staging;
            break;
        case State::Waiting:
            status.phase = hosts::Phase::Waiting;
            break;
        case State::HandingOff:
            status.phase = hosts::Phase::HandingOff;
            break;
        case State::Exiting:
            status.phase = hosts::Phase::Exiting;
            break;
        case State::Settling:
            status.phase = hosts::Phase::Settling;
            break;
    }
    if (state_ == State::Waiting) {
        switch (wait_) {
            case Wait::None:
                status.wait = hosts::WaitFor::None;
                break;
            case Wait::Protection:
                status.wait = hosts::WaitFor::Protection;
                break;
            case Wait::Temperature:
                status.wait = hosts::WaitFor::Temperature;
                break;
            case Wait::Overlay:
                status.wait = hosts::WaitFor::Overlay;
                break;
        }
    }
    status.source = SourceIsOf(Current(), !manifest_ && !problem_.empty());
    if (manifest_) {
        status.source_version = WireVersion(manifest_->version);
        status.source_release = hosts::ShortOf(manifest_->release);
    }
    if (running_build_) status.running_build = hosts::ShortOf(*running_build_);
    status.trigger = answered_;
    status.answer = answer_;
    return status;
}

bool TrayUpdates::AllowHostsRequests(const bool on, std::wstring& error) {
    if (!SaveHostsTrigger(on, error)) return false;
    preference_.hosts_trigger = on;
    logging::Info(std::format(L"update: paired hosts may ask this machine to update: {}", on));
    return true;
}

std::optional<SourceView> TrayUpdates::SourceRelease() const {
    if (!manifest_) return std::nullopt;
    return SourceView{manifest_->version, manifest_->release};
}

void TrayUpdates::CheckQuietly() {
    if (state_ == State::Idle) StartCheck(false);
}

void TrayUpdates::Shutdown() noexcept {
    if (worker_.joinable()) {
        if (WaitForSingleObject(worker_.native_handle(), kStopWaitMs) == WAIT_OBJECT_0) {
            worker_.join();
        } else {
            worker_.detach();
        }
    }
    // Exiting for some other reason while a hand-off is unanswered: without its
    // plan, a script that starts late stops instead of updating a GTG the
    // reader closed. One that has already answered keeps the update going.
    if (state_ == State::HandingOff && shared_) {
        bool answered = false;
        {
            std::scoped_lock lock(shared_->mutex);
            const auto* done =
                shared_->result ? std::get_if<HandOffDone>(&*shared_->result) : nullptr;
            answered = done != nullptr && done->result.ok;
        }
        if (!answered) (void)DeleteFileW((work_ / kPlanFileName).c_str());
    }
    state_ = State::Off;
}

}  // namespace gtg::update
