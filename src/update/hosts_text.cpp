#include "update/hosts_text.hpp"

#include <array>
#include <format>

#include "localization/localization.hpp"

namespace gtg::update {
namespace {

std::wstring Text(const std::wstring_view zh, const std::wstring_view en) {
    return std::wstring(localization::Select(zh, en));
}

std::wstring Wide(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

std::wstring NameOf(const HostsUpdates& updates, const std::uint64_t host) {
    const auto view = updates.Find(host);
    return !view || view->name.empty() ? std::wstring(L"?") : view->name;
}

std::array<std::uint8_t, 3> WireVersion(const Version& version) {
    const auto part = [](const int value) {
        return static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
    };
    return {part(version.major), part(version.minor), part(version.patch)};
}

std::wstring WaitText(const hosts::WaitFor wait) {
    switch (wait) {
        case hosts::WaitFor::Protection:
            return Text(L"等待：保護回到待命", L"Waiting: protection to be armed");
        case hosts::WaitFor::Temperature:
            return Text(L"等待：GPU 冷卻", L"Waiting: the GPU to cool");
        case hosts::WaitFor::Overlay:
            return Text(L"等待：遊戲 overlay 結束", L"Waiting: the game overlay to end");
        case hosts::WaitFor::None:
            return Text(L"即將開始", L"Starting");
    }
    return {};
}

}  // namespace

std::wstring VersionCell(const HostView& host) {
    if (host.version == std::array<std::uint8_t, 3>{}) return L"—";
    std::wstring text = std::format(L"{}.{}.{}", host.version[0], host.version[1], host.version[2]);
    if (host.status && host.status->running_build != hosts::Short{}) {
        text += L" ";
        for (std::size_t i = 0; i < 4; ++i)
            text += std::format(L"{:02X}", host.status->running_build[i]);
    }
    return text;
}

std::wstring AnswerText(const hosts::Answer answer) {
    switch (answer) {
        case hosts::Answer::None:
            return {};
        case hosts::Answer::Accepted:
            return Text(L"已接受", L"Accepted");
        case hosts::Answer::NotAllowed:
            return Text(L"那台不允許其他主機要求更新", L"It does not let other hosts update it");
        case hosts::Answer::TooSoon:
            return Text(L"那台 30 分鐘內已被要求更新過",
                        L"It took a request less than 30 minutes ago");
        case hosts::Answer::Busy:
            return Text(L"那台正在進行另一個更新", L"Another update is in progress there");
        case hosts::Answer::NoSource:
            return Text(L"那台沒有可用的更新來源", L"It has no usable update source");
        case hosts::Answer::Unreadable:
            return Text(L"那台讀不到它的更新來源", L"It cannot read its update source");
        case hosts::Answer::OtherRelease:
            return Text(L"那台的來源上是另一個版本", L"Its source has another release");
        case hosts::Answer::NoVariant:
            return Text(L"那台的來源沒有它需要的版本", L"Its source has no build for it");
        case hosts::Answer::AlreadyCurrent:
            return Text(L"已是這一版", L"Already runs this release");
        case hosts::Answer::NotNewer:
            return Text(L"不比那台的版本新；可以用強制更新",
                        L"Not newer than what it runs; Force update can install it");
        case hosts::Answer::BelowFloor:
            return Text(L"這一版早於 1.2.0，不能安裝", L"Older than 1.2.0; not installed");
        case hosts::Answer::ServiceInstalled:
            return Text(L"那台裝了 GTG Service，不更新", L"The GTG Service is installed there");
        case hosts::Answer::SmartAppControl:
            return Text(L"那台開著 Smart App Control，不更新", L"Smart App Control is on there");
        case hosts::Answer::FolderRefused:
            return Text(L"那台的 GTG 在網路位置或不支援權限的磁碟上",
                        L"Its GTG runs from a network folder or a drive without access lists");
        case hosts::Answer::EntryMismatch:
            return Text(L"那台 GTG 的檔名不同", L"Its GTG has another file name");
        case hosts::Answer::PinInvalid:
            return Text(L"那台的簽章釘選設定無效", L"Its signer pin setting is not valid");
        case hosts::Answer::SignatureRefused:
            return Text(L"檔案的簽章與那台的釘選不符", L"A file is not signed as its pin requires");
        case hosts::Answer::StageFailed:
            return Text(L"那台準備檔案失敗", L"It could not prepare the files");
        case hosts::Answer::HandOffFailed:
            return Text(L"那台無法啟動更新程式", L"It could not start the update script");
        case hosts::Answer::GaveUp:
            return Text(L"30 分鐘內沒有安全時機，已放棄",
                        L"No safe moment in 30 minutes; given up");
        case hosts::Answer::Cancelled:
            return Text(L"在那台被取消", L"Cancelled there");
        case hosts::Answer::Updated:
            return Text(L"已更新", L"Updated");
        case hosts::Answer::RolledBack:
            return Text(L"新版沒有正常啟動，已回到舊版",
                        L"The new build did not come up; rolled back");
        case hosts::Answer::Aborted:
            return Text(L"更新程式沒有安裝任何檔案", L"The script installed nothing");
        case hosts::Answer::Failed:
            return Text(L"更新失敗", L"The update failed");
        case hosts::Answer::Interrupted:
            return Text(L"更新沒有完成", L"The update did not finish");
    }
    return {};
}

std::wstring UpdateCell(const HostsUpdates& updates, const HostView& host) {
    if (!host.status) return Text(L"不支援 Hosts 觸發", L"No Hosts updates");
    const hosts::Status& status = *host.status;
    const auto asked = updates.AskedOf(host.id);
    if (asked && asked->ask == HostsUpdates::Ask::Sent) return Text(L"已送出", L"Sent");
    if (asked && asked->ask == HostsUpdates::Ask::NoAnswer) return Text(L"沒有回應", L"No answer");
    switch (status.phase) {
        case hosts::Phase::Off:
            return Text(L"沒有可用的來源", L"No usable source");
        case hosts::Phase::Checking:
            return Text(L"檢查中", L"Checking");
        case hosts::Phase::Staging:
            return Text(L"準備中", L"Preparing");
        case hosts::Phase::Waiting:
            return WaitText(status.wait);
        case hosts::Phase::HandingOff:
        case hosts::Phase::Exiting:
            return Text(L"更新中", L"Updating");
        case hosts::Phase::Settling:
            return Text(L"確認中", L"Confirming");
        case hosts::Phase::Idle:
            break;
    }
    if (asked && asked->ask == HostsUpdates::Ask::Answered &&
        asked->answer != hosts::Answer::Accepted && asked->answer != hosts::Answer::None) {
        return AnswerText(asked->answer);
    }
    if (!status.accepts_triggers) return Text(L"不接受要求", L"Takes no requests");
    if (updates.AtRelease(host)) return Text(L"已是最新", L"Up to date");
    return updates.Source() ? Text(L"可更新", L"Can update") : std::wstring();
}

std::wstring SourceLine(const std::optional<SourceView>& source) {
    if (!source) return Text(L"本機來源：尚未讀取", L"This machine's source: not read yet");
    std::wstring release;
    for (std::size_t i = 0; i < 4; ++i)
        release += std::format(L"{:02X}", source->release[i]);
    return localization::Format(L"本機來源：{}（release {}）",
                                L"This machine's source: {} (release {})",
                                Wide(FormatVersion(source->version)), release);
}

std::wstring ProgressLine(const HostsUpdates& updates, const std::uint64_t selected) {
    const HostsUpdates::Rollout& rollout = updates.Progress();
    switch (rollout.run) {
        case HostsUpdates::Run::Running: {
            const std::size_t total = rollout.done.size() + rollout.pending.size() + 1;
            return localization::Format(
                L"全部更新：{}（第 {}／{} 台）", L"Update all: {} ({} of {})",
                rollout.current ? NameOf(updates, *rollout.current) : std::wstring(L"?"),
                rollout.done.size() + 1, total);
        }
        case HostsUpdates::Run::Done:
            return localization::Format(L"全部更新完成：{} 台", L"Update all finished: {} host(s)",
                                        rollout.done.size());
        case HostsUpdates::Run::Stopped: {
            if (rollout.stopped_by_reader || !rollout.stopped_at) {
                return Text(L"全部更新已停止", L"Update all stopped");
            }
            const std::wstring name = NameOf(updates, *rollout.stopped_at);
            const std::wstring why = rollout.stopped_no_answer ? Text(L"沒有回應", L"no answer")
                                     : rollout.stopped_away
                                         ? Text(L"那台沒有回來", L"it did not come back")
                                         : AnswerText(rollout.stopped_answer);
            return localization::Format(L"全部更新在 {} 停止：{}", L"Update all stopped at {}: {}",
                                        name, why);
        }
        case HostsUpdates::Run::Idle:
            break;
    }
    const auto asked = updates.AskedOf(selected);
    if (!asked || asked->ask != HostsUpdates::Ask::Answered) return {};
    return localization::Format(L"{}：{}", L"{}: {}", NameOf(updates, selected),
                                AnswerText(asked->answer));
}

std::wstring ConfirmText(const HostView& host, const SourceView& source, const bool force) {
    const std::wstring version = Wide(FormatVersion(source.version));
    const std::wstring name = host.name.empty() ? std::wstring(L"?") : host.name;
    std::wstring text =
        force ? localization::Format(L"強制更新 {} 到 {}？", L"Force {} to {}?", name, version)
              : localization::Format(L"把 {} 更新到 {}？", L"Update {} to {}?", name, version);
    text += localization::Format(
        L"\n\n{} 會從它自己的更新來源安裝這一版，等 GPU 冷卻、沒有遊戲使用 overlay 時重新啟動，"
        L"保護暫停幾秒；新版沒有正常啟動時，會自動回到原本的版本。",
        L"\n\n{} installs this release from its own update source and restarts once its GPU has "
        L"cooled and no game uses the overlay, so its protection pauses for a few seconds; if the "
        L"new build does not come up, it goes back to the one it runs now.",
        name);
    const auto target = WireVersion(source.version);
    if (force && host.version > target) {
        text += Text(L"\n\n這是降版：那台不會再認得新版才加入的設定，會改用預設值。",
                     L"\n\nThis is a downgrade: settings added after that version are not read "
                     L"there and their defaults apply.");
    } else if (force && host.version == target) {
        text += Text(L"\n\n這是同一版的重新安裝。", L"\n\nThis reinstalls the same version.");
    }
    if (host.status && host.status->folder_writable) {
        text += Text(L"\n\n注意：那台 GTG 所在的資料夾，一般程式也能寫入。",
                     L"\n\nNote: programs without administrator rights can write to GTG's folder "
                     L"there.");
    }
    if (host.status && !host.status->pin_set) {
        text += Text(
            L"\n\n那台沒有設定簽章釘選：它安裝什麼，由它的更新來源決定，也就是 GitHub "
            L"上的發佈，或能寫入它 UNC 來源的人。",
            L"\n\nNo signer pin is set there: its update source decides what it installs, "
            L"meaning the releases on GitHub or whoever can write to its UNC source.");
    }
    return text;
}

std::wstring ConfirmAllText(const HostsUpdates& updates, const SourceView& source) {
    std::wstring names;
    for (const std::uint64_t host : updates.Candidates()) {
        if (!names.empty()) names += Text(L"、", L", ");
        names += NameOf(updates, host);
    }
    return localization::Format(
        L"一台一台更新到 {}？\n\n{}\n\n每台以新版回到正常後才換下一台，遇到失敗就停止。",
        L"Update to {} one host at a time?\n\n{}\n\nEach host must come back on the new build "
        L"before the next is asked, and the first failure stops it.",
        Wide(FormatVersion(source.version)), names);
}

}  // namespace gtg::update
