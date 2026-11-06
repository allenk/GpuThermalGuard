#include "update/settings_dialog.hpp"

#include <cwctype>

#include "localization/localization.hpp"
#include "update/source.hpp"

namespace gtg::update {
namespace {

std::wstring Trim(std::wstring text) {
    while (!text.empty() && std::iswspace(text.back()))
        text.pop_back();
    std::size_t begin = 0;
    while (begin < text.size() && std::iswspace(text[begin]))
        ++begin;
    return text.substr(begin);
}

std::wstring PinText(const UpdatePreference& preference) {
    if (!preference.pin) return {};
    const std::string hex = ToHex(*preference.pin);
    return std::wstring(hex.begin(), hex.end());
}

}  // namespace

LRESULT UpdateSettingsDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
    SetWindowTextW(localization::Select(L"更新設定", L"Update Settings").data());
    SetDlgItemTextW(
        IDC_UPDATE_SOURCE_LABEL,
        localization::Select(L"更新來源：留空就是 GitHub 上的公開版本；區域網路可填 UNC 路徑，例如 "
                             L"\\\\server\\share\\GTG",
                             L"Update source: leave it empty for the public releases on GitHub, or "
                             L"enter a UNC path on your network, such as \\\\server\\share\\GTG")
            .data());
    SetDlgItemTextW(
        IDC_UPDATE_AUTO_CHECK,
        localization::Select(L"自動檢查更新（只提示，不會自動安裝）",
                             L"Check automatically (it only tells you; nothing installs "
                             L"on its own)")
            .data());
    SetDlgItemTextW(
        IDC_UPDATE_PIN_LABEL,
        localization::Select(L"簽章釘選：簽署憑證的 SHA-256（選填）",
                             L"Signer pin: the signing certificate's SHA-256 (optional)")
            .data());
    SetDlgItemTextW(IDOK, localization::Select(L"儲存", L"Save").data());
    SetDlgItemTextW(IDCANCEL, localization::Select(L"取消", L"Cancel").data());
    shown_source_ = current_.source_invalid ? current_.source_text : current_.source;
    SetDlgItemTextW(IDC_UPDATE_SOURCE, shown_source_.c_str());
    SetDlgItemTextW(IDC_UPDATE_PIN, PinText(current_).c_str());
    CheckDlgButton(IDC_UPDATE_AUTO_CHECK, current_.auto_check ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(IDC_UPDATE_THIS_BUILD, this_build_.c_str());
    SetDlgItemTextW(IDC_UPDATE_SOURCE_RELEASE, source_release_.c_str());
    std::wstring note;
    if (current_.source_invalid) {
        note = localization::Select(
            L"儲存的更新來源無法使用，這台不會更新。請改正，或清空改用 GitHub。",
            L"The stored source cannot be used, so nothing is updated. "
            L"Correct it, or clear it for GitHub.");
        note += L"\n";
    }
    note += localization::Select(
        L"設定釘選後，只安裝由那張憑證簽署的檔案；GitHub 上的公開版目前沒有簽章，設了釘選就不會從 "
        L"GitHub 更新。沒有釘選時，GitHub 來源信任這個專案在 GitHub 的發佈；UNC 來源則是任何能寫入"
        L"它的人，都能發佈這台會安裝的版本。",
        L"With a pin, only files signed with that certificate are installed; the public releases "
        L"on GitHub are not signed yet, so a pin stops updates from GitHub. Without a pin, GitHub "
        L"means trusting the project's releases there, and a UNC source means trusting anyone who "
        L"can write to it.");
    Note(note);
    return TRUE;
}

std::wstring UpdateSettingsDialog::Text(const int id) const {
    wchar_t buffer[1024]{};
    ::GetDlgItemTextW(m_hWnd, id, buffer, 1024);
    return Trim(buffer);
}

void UpdateSettingsDialog::Note(const std::wstring& text) {
    SetDlgItemTextW(IDC_UPDATE_NOTE, text.c_str());
}

LRESULT UpdateSettingsDialog::OnSave(WORD, WORD, HWND, BOOL&) {
    const std::wstring source = Text(IDC_UPDATE_SOURCE);
    std::wstring pin = Text(IDC_UPDATE_PIN);
    // Saving the other settings never turns an unusable source into GitHub.
    const bool keep_source = current_.source_invalid && source == shown_source_;
    if (!keep_source && !source.empty() && !IsValidSource(source)) {
        Note(std::wstring(localization::Select(
            L"來源請留空（GitHub），或填 UNC 路徑，例如 \\\\server\\share\\GTG。提權的 GTG 看不到磁碟機"
            L"代號。",
            L"Leave the source empty for GitHub, or enter a UNC path such as "
            L"\\\\server\\share\\GTG. An elevated GTG does not see mapped drive letters.")));
        ::SetFocus(GetDlgItem(IDC_UPDATE_SOURCE));
        return 0;
    }
    for (wchar_t& c : pin)
        c = static_cast<wchar_t>(std::towupper(c));
    if (!pin.empty() && !ParseDigest(std::wstring_view(pin))) {
        Note(std::wstring(localization::Select(L"簽章釘選必須是 64 個十六進位數字。",
                                               L"The signer pin must be 64 hex digits.")));
        ::SetFocus(GetDlgItem(IDC_UPDATE_PIN));
        return 0;
    }
    std::wstring error;
    if ((!keep_source && !SaveUpdateSource(source, error)) || !SaveUpdatePin(pin, error) ||
        !SaveUpdateAutoCheck(IsDlgButtonChecked(IDC_UPDATE_AUTO_CHECK) == BST_CHECKED, error)) {
        Note(error);
        return 0;
    }
    EndDialog(IDOK);
    return 0;
}

LRESULT UpdateSettingsDialog::OnCancel(WORD, WORD, HWND, BOOL&) {
    EndDialog(IDCANCEL);
    return 0;
}

}  // namespace gtg::update
