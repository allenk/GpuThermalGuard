#include "tray/hosts_dialog.hpp"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>

#include "qrcodegen.hpp"

#include "localization/localization.hpp"
#include "logging/logger.hpp"
#include "peer/hosts_view.hpp"
#include "tray/ui_theme.hpp"

#ifdef GTG_FEATURE_UPDATE
#include "update/hosts_text.hpp"
#endif

namespace gtg::tray {
namespace {

std::wstring Text(const std::wstring_view zh, const std::wstring_view en) {
    return std::wstring(localization::Select(zh, en));
}

std::wstring Widen(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

std::wstring EndpointText(const peer::Endpoint& endpoint) {
    return std::format(L"{}.{}.{}.{}:{}", endpoint.address[0], endpoint.address[1],
                       endpoint.address[2], endpoint.address[3], endpoint.port);
}

std::wstring MinutesSeconds(const std::uint64_t ms) {
    const std::uint64_t seconds = (ms + 999) / 1'000;
    return std::format(L"{}:{:02}", seconds / 60, seconds % 60);
}

std::wstring Ago(const std::optional<std::uint64_t> heard_ms, const std::uint64_t now_ms) {
    if (!heard_ms) return L"—";
    const std::uint64_t seconds = now_ms > *heard_ms ? (now_ms - *heard_ms) / 1'000 : 0;
    if (seconds < 2) return Text(L"剛剛", L"just now");
    if (seconds < 120) return localization::Format(L"{} 秒前", L"{} s ago", seconds);
    if (seconds < 7'200) return localization::Format(L"{} 分鐘前", L"{} min ago", seconds / 60);
    return localization::Format(L"{} 小時前", L"{} h ago", seconds / 3'600);
}

std::wstring StateText(const peer::HostRow& row, const std::uint64_t now_ms) {
    const peer::Presence presence = peer::PresenceAt(row.heard, now_ms);
    std::wstring text;
    switch (presence) {
        case peer::Presence::Online:
            text = Text(L"線上", L"Online");
            break;
        case peer::Presence::Dim:
            text = Text(L"延遲", L"Late");
            break;
        case peer::Presence::Lost: {
            const std::uint64_t silent = now_ms - *row.heard.state_ms;
            text = Text(L"失聯 ", L"Lost ") + peer::SilenceText(silent);
            break;
        }
        case peer::Presence::Offline:
            text = Text(L"離線", L"Offline");
            break;
        case peer::Presence::NotHeard:
            text = Text(L"尚未收到", L"Not heard yet");
            break;
    }
    if ((presence == peer::Presence::Online || presence == peer::Presence::Dim) && row.state) {
        switch (row.state->protection) {
            case peer::Protection::Warning:
                text += Text(L"・溫度警告", L" · temperature warning");
                break;
            case peer::Protection::SafePower:
                text += Text(L"・安全功率", L" · safe power");
                break;
            case peer::Protection::Fault:
                text += Text(L"・故障", L" · fault");
                break;
            case peer::Protection::Normal:
            case peer::Protection::Unknown:
                break;
        }
    }
    return text;
}

std::wstring ParseErrorText(const peer::CodeError error) {
    switch (error) {
        case peer::CodeError::None:
        case peer::CodeError::Empty:
            return {};
        case peer::CodeError::Character:
            return Text(L"這不是配對碼：含有配對碼不會有的字元。",
                        L"Not a pairing code: it has characters a code never has.");
        case peer::CodeError::Length:
            return Text(L"配對碼不完整，或貼上了不只一組。",
                        L"The code is incomplete, or more than one code was pasted.");
        case peer::CodeError::Checksum:
            return Text(L"配對碼有字元打錯或順序顛倒，請重新複製。",
                        L"A character is mistyped or two are swapped. Copy the code again.");
        case peer::CodeError::Format:
            return Text(L"這是較新版本 GTG 的配對碼，請先更新這台的 GTG。",
                        L"This code is from a later GTG. Update GTG here first.");
        case peer::CodeError::Address:
            return Text(L"配對碼裡的位址不是可連線的主機。",
                        L"The code's address is not one a host can have.");
    }
    return {};
}

}  // namespace

LRESULT HostsDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
    Localize();
    CheckDlgButton(IDC_HOSTS_ENABLED, hosts_.On() ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(IDC_HOSTS_NAME, hosts_.StoredName().c_str());
    const std::wstring computer = HostsController::ComputerName();
    SendDlgItemMessageW(IDC_HOSTS_NAME, EM_SETCUEBANNER, TRUE,
                        reinterpret_cast<LPARAM>(computer.c_str()));
    SendDlgItemMessageW(IDC_HOSTS_NAME, EM_LIMITTEXT, peer::kNameBytes, 0);
    SetDlgItemInt(IDC_HOSTS_PORT, static_cast<UINT>(hosts_.Port()), FALSE);
    SendDlgItemMessageW(IDC_HOSTS_PORT, EM_LIMITTEXT, 5, 0);
    SendDlgItemMessageW(IDC_HOSTS_JOIN_CODE, EM_LIMITTEXT, 256, 0);

    const HWND list = GetDlgItem(IDC_HOSTS_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    RECT client{};
    ::GetClientRect(list, &client);
    const int width = client.right - client.left - GetSystemMetrics(SM_CXVSCROLL);

    const struct {
        std::wstring title;
        int share;
#ifdef GTG_FEATURE_UPDATE
    } columns[] = {{Text(L"名稱", L"Name"), 20},    {Text(L"狀態", L"State"), 16},
                   {Text(L"位址", L"Address"), 18}, {Text(L"最後收到", L"Heard"), 11},
                   {Text(L"版本", L"Version"), 15}, {Text(L"更新", L"Update"), 20}};
#else
    } columns[] = {{Text(L"名稱", L"Name"), 30},
                   {Text(L"狀態", L"State"), 30},
                   {Text(L"位址", L"Address"), 24},
                   {Text(L"最後收到", L"Heard"), 16}};
#endif

    int index = 0;
    for (const auto& column : columns) {
        LVCOLUMNW item{};
        item.mask = LVCF_TEXT | LVCF_WIDTH;
        item.pszText = const_cast<wchar_t*>(column.title.c_str());
        item.cx = width * column.share / 100;
        ListView_InsertColumn(list, index++, &item);
    }

    theme::AttachDialog(m_hWnd);
    theme::Repaint(m_hWnd);
    SetTimer(kRefreshTimer, kRefreshIntervalMs);
    Refresh();
    return TRUE;
}

LRESULT HostsDialog::OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
    KillTimer(kRefreshTimer);
    handled = FALSE;
    return 0;
}

LRESULT HostsDialog::OnTimer(UINT, WPARAM, LPARAM, BOOL&) {
    Refresh();
    return 0;
}

void HostsDialog::Localize() {
    SetWindowTextW(Text(L"主機", L"Hosts").c_str());
    SetText(IDC_HOSTS_ENABLED,
            Text(L"主機：與配對的 GTG 交換狀態，顯示在儀表板上",
                 L"Hosts: exchange state with paired GTGs and show them on the dashboard"));
    SetText(IDC_HOSTS_NAME_LABEL, Text(L"本機名稱", L"This machine"));
    SetText(IDC_HOSTS_PORT_LABEL, Text(L"連接埠", L"Port"));
    SetText(IDC_HOSTS_LIST_LABEL,
            Text(L"配對的主機（儀表板顯示四台，警示中的優先）",
                 L"Paired hosts (the dashboard shows four; hosts in alert first)"));
    SetText(IDC_HOSTS_UP, Text(L"上移", L"Up"));
    SetText(IDC_HOSTS_DOWN, Text(L"下移", L"Down"));
    SetText(IDC_HOSTS_REMOVE, Text(L"移除", L"Remove"));
#ifdef GTG_FEATURE_UPDATE
    SetText(IDC_HOSTS_UPDATE_ALLOW,
            Text(L"允許其他主機要求這台更新", L"Let other hosts ask this machine to update"));
    SetText(IDC_HOSTS_UPDATE_CHECK, Text(L"重新檢查", L"Check again"));
    SetText(IDC_HOSTS_UPDATE_ONE, Text(L"更新", L"Update"));
    SetText(IDC_HOSTS_UPDATE_FORCE, Text(L"強制更新", L"Force update"));
    SetText(IDC_HOSTS_UPDATE_ALL, Text(L"全部更新", L"Update all"));
    SetText(IDC_HOSTS_UPDATE_STOP, Text(L"停止", L"Stop"));
#endif
    SetText(IDC_HOSTS_PAIR_GROUP, Text(L"與另一台 GTG 配對", L"Pair with another GTG"));
    SetText(IDC_HOSTS_COPY, Text(L"複製", L"Copy"));
    SetText(IDC_HOSTS_JOIN_LABEL,
            Text(L"貼上另一台 GTG 的配對碼", L"Paste a code from another GTG"));
    SetText(IDC_HOSTS_JOIN, Text(L"加入", L"Join"));
    SetText(IDOK, Text(L"關閉", L"Close"));
}

void HostsDialog::SetText(const int id, const std::wstring& text) {
    wchar_t current[512]{};
    GetDlgItemTextW(id, current, static_cast<int>(std::size(current)));
    if (text != current) SetDlgItemTextW(id, text.c_str());
}

void HostsDialog::Refresh() {
    const auto& stored = hosts_.Table();
    const peer::Table* table = stored && hosts_.Running() ? &*stored : nullptr;
    const bool running = hosts_.Running();
    for (const int id : {IDC_HOSTS_LIST, IDC_HOSTS_CREATE, IDC_HOSTS_JOIN_CODE}) {
        ::EnableWindow(GetDlgItem(id), running);
    }
    RefreshList(table);
    RefreshPairing(table);
    RefreshNote(table);
#ifdef GTG_FEATURE_UPDATE
    RefreshUpdates();
#endif
}

void HostsDialog::RefreshList(const peer::Table* table) {
    const HWND list = GetDlgItem(IDC_HOSTS_LIST);
    std::vector<std::uint64_t> peers;
    if (table != nullptr) {
        for (const peer::HostRow& row : table->hosts)
            peers.push_back(row.peer);
    }
    if (peers != rows_) {
        ListView_DeleteAllItems(list);
        for (std::size_t i = 0; i < peers.size(); ++i) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = const_cast<wchar_t*>(L"");
            ListView_InsertItem(list, &item);
        }
        rows_ = peers;
        const auto found = std::find(rows_.begin(), rows_.end(), selected_peer_);
        if (found != rows_.end()) {
            const int at = static_cast<int>(found - rows_.begin());
            ListView_SetItemState(list, at, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    const std::uint64_t now = GetTickCount64();
    for (std::size_t i = 0; table != nullptr && i < table->hosts.size(); ++i) {
        const peer::HostRow& row = table->hosts[i];
#ifdef GTG_FEATURE_UPDATE
        const auto view =
            asking_ != nullptr ? asking_->Find(row.peer) : std::optional<update::HostView>();
        const std::wstring cells[] = {row.name.empty() ? L"?" : row.name,
                                      StateText(row, now),
                                      row.endpoint ? EndpointText(*row.endpoint) : L"—",
                                      Ago(row.heard.state_ms, now),
                                      view ? update::VersionCell(*view) : L"—",
                                      view ? update::UpdateCell(*asking_, *view) : std::wstring()};
        for (int column = 0; column < 6; ++column) {
#else
        const std::wstring cells[] = {row.name.empty() ? L"?" : row.name, StateText(row, now),
                                      row.endpoint ? EndpointText(*row.endpoint) : L"—",
                                      Ago(row.heard.state_ms, now)};
        for (int column = 0; column < 4; ++column) {
#endif
            wchar_t current[256]{};
            ListView_GetItemText(list, static_cast<int>(i), column, current,
                                 static_cast<int>(std::size(current)));
            if (cells[column] != current) {
                ListView_SetItemText(list, static_cast<int>(i), column,
                                     const_cast<wchar_t*>(cells[column].c_str()));
            }
        }
    }
    const std::uint64_t selected = SelectedPeer();
    const auto at = std::find(rows_.begin(), rows_.end(), selected);
    const bool has = selected != 0 && at != rows_.end();
    ::EnableWindow(GetDlgItem(IDC_HOSTS_UP), has && at != rows_.begin());
    ::EnableWindow(GetDlgItem(IDC_HOSTS_DOWN), has && at + 1 != rows_.end());
    ::EnableWindow(GetDlgItem(IDC_HOSTS_REMOVE), has);
}

void HostsDialog::RefreshPairing(const peer::Table* table) {
    const std::uint64_t now = GetTickCount64();
    const peer::InvitationView* invitation =
        table != nullptr && table->invitation ? &*table->invitation : nullptr;
    const bool live = invitation != nullptr && invitation->status == peer::Invitation::Status::Live;
    const std::string code = invitation != nullptr ? invitation->code : std::string();
    if (code != shown_code_) {
        shown_code_ = code;
        SetDlgItemTextW(IDC_HOSTS_CODE, Widen(code).c_str());
        qr_modules_.clear();
        qr_size_ = 0;
        if (!code.empty()) {
            try {
                const auto qr =
                    qrcodegen::QrCode::encodeText(code.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
                qr_size_ = qr.getSize();
                qr_modules_.resize(static_cast<std::size_t>(qr_size_ * qr_size_));
                for (int y = 0; y < qr_size_; ++y) {
                    for (int x = 0; x < qr_size_; ++x) {
                        qr_modules_[static_cast<std::size_t>(y * qr_size_ + x)] =
                            qr.getModule(x, y);
                    }
                }
            } catch (...) {
                qr_size_ = 0;
                qr_modules_.clear();
            }
        }
        ::InvalidateRect(GetDlgItem(IDC_HOSTS_QR), nullptr, TRUE);
    }
    if (qr_live_ != live) {
        qr_live_ = live;
        ::InvalidateRect(GetDlgItem(IDC_HOSTS_QR), nullptr, TRUE);
    }
    ::EnableWindow(GetDlgItem(IDC_HOSTS_COPY), live);
    SetText(IDC_HOSTS_CREATE,
            live ? Text(L"產生新的配對碼", L"New code") : Text(L"產生配對碼", L"Create code"));

    std::wstring code_status;
    if (invitation != nullptr) {
        switch (invitation->status) {
            case peer::Invitation::Status::Live:
                code_status = localization::Format(
                    L"{} 內有效。複製到另一台 GTG，或用手機掃描。",
                    L"Valid for {}. Copy it to the other GTG, or scan it.",
                    MinutesSeconds(invitation->expires_ms > now ? invitation->expires_ms - now
                                                                : 0));
                break;
            case peer::Invitation::Status::Spent:
                code_status = localization::Format(L"已與 {} 配對。", L"Paired with {}.",
                                                   invitation->joined_by);
                break;
            case peer::Invitation::Status::Expired:
                code_status = Text(L"已過期，請產生新的配對碼。", L"Expired. Create a new code.");
                break;
            case peer::Invitation::Status::Burned:
                code_status = Text(L"錯誤嘗試已達五次，這組配對碼作廢。",
                                   L"Five wrong attempts spent this code. Create a new one.");
                break;
        }
    } else if (table != nullptr && table->no_address) {
        code_status = Text(L"這台電腦沒有其他電腦能連到的網路位址。",
                           L"This machine has no address another machine can reach.");
    }
    SetText(IDC_HOSTS_CODE_STATUS, code_status);

    std::wstring join_status;
    bool asking = false;
    if (table != nullptr && table->joining) {
        const peer::JoinView& join = *table->joining;
        const std::wstring target = EndpointText(join.target);
        switch (join.status) {
            case peer::JoinStatus::Asking:
                asking = true;
                join_status = localization::Format(L"正在連線 {}…", L"Contacting {}…", target);
                break;
            case peer::JoinStatus::Paired:
                join_status =
                    localization::Format(L"已與 {} 配對。", L"Paired with {}.", join.issuer);
                break;
            case peer::JoinStatus::Used:
                join_status = Text(L"這組配對碼已經被使用過，請向對方要一組新的。",
                                   L"This code was already used. Ask for a new one.");
                break;
            case peer::JoinStatus::Expired:
                join_status = Text(L"這組配對碼已過期，請向對方要一組新的。",
                                   L"This code has expired. Ask for a new one.");
                break;
            case peer::JoinStatus::NoAnswer:
                join_status = localization::Format(
                    L"{} 沒有回應。請確認網路，以及那台 GTG 的主機功能已開啟、配對碼仍有效。",
                    L"No answer from {}. Check the network, and that Hosts is on there and the "
                    L"code is still valid.",
                    target);
                break;
            case peer::JoinStatus::Own:
                join_status = Text(L"這是這台 GTG 自己的配對碼。", L"This is this GTG's own code.");
                break;
            case peer::JoinStatus::Full:
                join_status = Text(L"已配對 16 台主機，請先移除一台。",
                                   L"Sixteen hosts are paired. Remove one first.");
                break;
        }
    } else if (pasted_) {
        join_status = localization::Format(L"將連線到 {}。", L"Will contact {}.",
                                           EndpointText({pasted_->address, pasted_->port}));
    } else {
        join_status = ParseErrorText(paste_error_);
    }
    SetText(IDC_HOSTS_JOIN_STATUS, join_status);
    ::EnableWindow(GetDlgItem(IDC_HOSTS_JOIN), table != nullptr && pasted_.has_value() && !asking);
}

void HostsDialog::RefreshNote(const peer::Table* table) {
    std::vector<std::wstring> notes;
    if (!identity_error_.empty()) notes.push_back(identity_error_);
    if (hosts_.On() && !hosts_.Problem().empty()) notes.push_back(hosts_.Problem());
    if (table != nullptr) {
        switch (table->status) {
            case peer::LinkStatus::CryptoMissing:
                notes.push_back(Text(L"Windows 的加密元件缺少必要功能，主機功能無法使用。",
                                     L"Windows cryptography lacks a function Hosts needs."));
                break;
            case peer::LinkStatus::NetworkMissing:
                notes.push_back(Text(L"Windows 的網路元件無法啟動，主機功能無法使用。",
                                     L"Windows networking could not be started for Hosts."));
                break;
            case peer::LinkStatus::PortInUse:
                notes.push_back(localization::Format(
                    L"連接埠 {} 正被其他程式使用，其他主機連不進來。請換一個連接埠。",
                    L"Port {} is used by another program, so hosts cannot reach this one. "
                    L"Choose another port.",
                    hosts_.Port()));
                break;
            default:
                break;
        }
        const std::uint64_t now = GetTickCount64();
        for (const peer::HostRow& row : table->hosts) {
            if (row.clock_skew_ms) {
                notes.push_back(localization::Format(
                    L"{} 的時鐘與這台相差 {} 分鐘，在時鐘一致之前不採用它的資料。",
                    L"{}'s clock is {} min off this one's; its state is refused until the "
                    L"clocks agree.",
                    row.name,
                    (*row.clock_skew_ms < 0 ? -*row.clock_skew_ms : *row.clock_skew_ms) / 60'000));
            }
        }
        // Only a host never heard since start: one heard and then silent got
        // through the firewall, so it was closed or cut off, which its row
        // already says.
        for (const peer::HostRow& row : table->hosts) {
            const peer::Presence presence = peer::PresenceAt(row.heard, now);
            if (table->listening && row.issued && presence == peer::Presence::NotHeard) {
                notes.push_back(localization::Format(
                    L"沒有收到 {} 的資料。若 Windows 曾詢問 GTG 能否使用網路而被拒絕，"
                    L"請到 Windows 安全性 > 防火牆 > 允許應用程式通過防火牆，允許 GpuThermalGuard。",
                    L"Nothing heard from {}. If Windows asked whether GTG may use the network "
                    L"and the answer was no, allow GpuThermalGuard in Windows Security > "
                    L"Firewall > Allow an app through firewall.",
                    row.name));
                break;
            }
        }
    }
    if (!hosts_.Dropped().empty()) {
        std::wstring names;
        for (const std::wstring& name : hosts_.Dropped()) {
            if (!names.empty()) names += L", ";
            names += name;
        }
        notes.push_back(localization::Format(L"已移除無法讀取金鑰的配對：{}",
                                             L"Removed pairs whose key could not be read: {}",
                                             names));
    }
    // Ticking Hosts places nothing by itself; without this the reader is left
    // looking for a record that waits for its first pair.
    if (notes.empty() && hosts_.On() && table != nullptr && table->hosts.empty()) {
        notes.push_back(Text(L"主機功能已開啟。配對第一台主機後，儀表板才會出現「主機」。",
                             L"Hosts is on. The dashboard shows Hosts once a host is paired."));
    }
    std::wstring text;
    for (std::size_t i = 0; i < notes.size() && i < 2; ++i) {
        if (!text.empty()) text += L"\r\n";
        text += notes[i];
    }
    SetText(IDC_HOSTS_NOTE, text);
}

LRESULT HostsDialog::OnDrawItem(UINT, WPARAM, const LPARAM lparam, BOOL& handled) {
    const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
    if (draw == nullptr || draw->CtlID != IDC_HOSTS_QR) {
        handled = FALSE;
        return 0;
    }
    const RECT area = draw->rcItem;
    FillRect(draw->hDC, &area, theme::DialogBackgroundBrush());
    const auto& stored = hosts_.Table();
    const bool live = stored && stored->invitation &&
                      stored->invitation->status == peer::Invitation::Status::Live;
    if (!live || qr_size_ <= 0) return TRUE;
    // Four modules of quiet zone on every side, as the standard asks, and
    // whole pixels per module so a camera sees sharp edges.
    const int modules = qr_size_ + 8;
    const int side = std::min(area.right - area.left, area.bottom - area.top);
    const int pixel = side / modules;
    if (pixel < 1) return TRUE;
    const int total = pixel * modules;
    const int left = area.left + (area.right - area.left - total) / 2;
    const int top = area.top + (area.bottom - area.top - total) / 2;
    const RECT paper{left, top, left + total, top + total};
    FillRect(draw->hDC, &paper, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const auto ink = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    for (int y = 0; y < qr_size_; ++y) {
        for (int x = 0; x < qr_size_; ++x) {
            if (!qr_modules_[static_cast<std::size_t>(y * qr_size_ + x)]) continue;
            const RECT module{left + (x + 4) * pixel, top + (y + 4) * pixel, left + (x + 5) * pixel,
                              top + (y + 5) * pixel};
            FillRect(draw->hDC, &module, ink);
        }
    }
    return TRUE;
}

LRESULT HostsDialog::OnEnabled(WORD, WORD, HWND, BOOL&) {
    const bool on = IsDlgButtonChecked(IDC_HOSTS_ENABLED) == BST_CHECKED;
    // The name and port typed so far are the ones the first start uses.
    if (on && !ApplyIdentity()) {
        CheckDlgButton(IDC_HOSTS_ENABLED, BST_UNCHECKED);
        return 0;
    }
    std::wstring error;
    if (!hosts_.SetOn(on, error)) {
        CheckDlgButton(IDC_HOSTS_ENABLED, hosts_.On() ? BST_CHECKED : BST_UNCHECKED);
        identity_error_ = error;
    }
    rows_.clear();
    Refresh();
    return 0;
}

LRESULT HostsDialog::OnJoinCodeChanged(WORD, WORD, HWND, BOOL&) {
    wchar_t text[300]{};
    GetDlgItemTextW(IDC_HOSTS_JOIN_CODE, text, static_cast<int>(std::size(text)));
    const auto parsed = peer::ParseCode(std::wstring_view(text));
    pasted_ = parsed.code;
    paste_error_ = parsed.error;
    // A new code replaces whatever the last one came to.
    if (peer::Link* link = hosts_.Link()) link->CancelJoin();
    Refresh();
    return 0;
}

LRESULT HostsDialog::OnCreate(WORD, WORD, HWND, BOOL&) {
    if (peer::Link* link = hosts_.Link()) link->CreateCode();
    return 0;
}

LRESULT HostsDialog::OnCopy(WORD, WORD, HWND, BOOL&) {
    if (shown_code_.empty() || !OpenClipboard()) return 0;
    // Not marked to stay out of clipboard history or the cloud clipboard:
    // those are how the code reaches the other machine. It is single use and
    // short lived, so a copy left in a history is worth nothing.
    const std::wstring text = Widen(shown_code_);
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* data = GlobalLock(memory)) {
            std::memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return 0;
}

LRESULT HostsDialog::OnJoin(WORD, WORD, HWND, BOOL&) {
    peer::Link* link = hosts_.Link();
    if (link != nullptr && pasted_) link->Join(*pasted_);
    return 0;
}

LRESULT HostsDialog::OnMove(WORD, const WORD id, HWND, BOOL&) {
    peer::Link* link = hosts_.Link();
    const std::uint64_t selected = SelectedPeer();
    const auto at = std::find(rows_.begin(), rows_.end(), selected);
    if (link == nullptr || selected == 0 || at == rows_.end()) return 0;
    std::vector<std::uint64_t> order = rows_;
    const auto index = static_cast<std::size_t>(at - rows_.begin());
    if (id == IDC_HOSTS_UP && index > 0) {
        std::swap(order[index], order[index - 1]);
    } else if (id == IDC_HOSTS_DOWN && index + 1 < order.size()) {
        std::swap(order[index], order[index + 1]);
    } else {
        return 0;
    }
    link->Reorder(order);
    hosts_.SaveOrder(order);
    selected_peer_ = selected;
    return 0;
}

LRESULT HostsDialog::OnRemove(WORD, WORD, HWND, BOOL&) {
    peer::Link* link = hosts_.Link();
    const std::uint64_t selected = SelectedPeer();
    const auto& table = hosts_.Table();
    if (link == nullptr || selected == 0 || !table) return 0;
    std::wstring name;
    for (const peer::HostRow& row : table->hosts) {
        if (row.peer == selected) name = row.name;
    }
    const std::wstring question = localization::Format(
        L"移除 {}？對方會看到這台離線，直到它也移除這組配對。",
        L"Remove {}? It will show this machine offline until it removes the pair too.", name);
    if (MessageBoxW(question.c_str(), Text(L"主機", L"Hosts").c_str(),
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return 0;
    }
    link->Remove(selected);
    return 0;
}

LRESULT HostsDialog::OnClose(WORD, WORD, HWND, BOOL&) {
    if (!ApplyIdentity()) return 0;
    EndDialog(IDOK);
    return 0;
}

LRESULT HostsDialog::OnListChanged(int, LPNMHDR, BOOL&) {
    const std::uint64_t selected = SelectedPeer();
    if (selected != 0) selected_peer_ = selected;
    Refresh();
    return 0;
}

#ifdef GTG_FEATURE_UPDATE
void HostsDialog::RefreshUpdates() {
    const bool on = updates_ != nullptr && asking_ != nullptr;
    if (on) {
        CheckDlgButton(IDC_HOSTS_UPDATE_ALLOW,
                       updates_->HostsRequestsAllowed() ? BST_CHECKED : BST_UNCHECKED);
    }
    const std::uint64_t selected = SelectedPeer();
    const auto view =
        on && selected != 0 ? asking_->Find(selected) : std::optional<update::HostView>();
    const bool running = on && asking_->Progress().run == update::HostsUpdates::Run::Running;
    SetText(IDC_HOSTS_UPDATE_SOURCE, on ? update::SourceLine(asking_->Source()) : std::wstring());
    SetText(IDC_HOSTS_UPDATE_STATUS,
            on ? update::ProgressLine(*asking_, selected) : std::wstring());
    const bool askable = view && asking_->CanAsk(selected);
    ::EnableWindow(GetDlgItem(IDC_HOSTS_UPDATE_ALLOW), on);
    ::EnableWindow(GetDlgItem(IDC_HOSTS_UPDATE_CHECK), on && !running);
    ::EnableWindow(GetDlgItem(IDC_HOSTS_UPDATE_ONE), askable && !asking_->AtRelease(*view));
    ::EnableWindow(GetDlgItem(IDC_HOSTS_UPDATE_FORCE), askable);
    ::EnableWindow(GetDlgItem(IDC_HOSTS_UPDATE_ALL),
                   on && !running && asking_->Source() && !asking_->Candidates().empty());
    ::EnableWindow(GetDlgItem(IDC_HOSTS_UPDATE_STOP), running);
}

LRESULT HostsDialog::OnUpdateAllow(WORD, WORD, HWND, BOOL&) {
    if (updates_ == nullptr) return 0;
    const bool on = IsDlgButtonChecked(IDC_HOSTS_UPDATE_ALLOW) == BST_CHECKED;
    std::wstring error;
    if (!updates_->AllowHostsRequests(on, error)) {
        logging::Warning(error);
        MessageBoxW(error.c_str(), Text(L"主機", L"Hosts").c_str(), MB_OK | MB_ICONWARNING);
    }
    RefreshUpdates();
    return 0;
}

// A confirmation runs a message loop of its own, and the dashboard's ticks run
// inside it: they replace the hosts and may change this machine's source. So
// only copies taken before it are used after it, and what was confirmed is
// checked again against what is there now.
LRESULT HostsDialog::OnUpdateCommand(WORD, const WORD id, HWND, BOOL&) {
    if (updates_ == nullptr || asking_ == nullptr) return 0;
    const std::wstring title = Text(L"主機更新", L"Host updates");
    const auto same_source = [this](const update::SourceView& confirmed) {
        const auto source = asking_->Source();
        return source && source->release == confirmed.release;
    };
    switch (id) {
        case IDC_HOSTS_UPDATE_CHECK:
            updates_->CheckQuietly();
            break;
        case IDC_HOSTS_UPDATE_ONE:
        case IDC_HOSTS_UPDATE_FORCE: {
            const std::uint64_t selected = SelectedPeer();
            const auto view = asking_->Find(selected);
            const auto source = asking_->Source();
            if (!view || !source || !asking_->CanAsk(selected)) break;
            const bool force = id == IDC_HOSTS_UPDATE_FORCE;
            const std::wstring text = update::ConfirmText(*view, *source, force);
            if (MessageBoxW(text.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION) != IDYES) {
                break;
            }
            if (!same_source(*source) || !asking_->CanAsk(selected)) break;
            (void)asking_->Ask(selected, force, GetTickCount64());
            break;
        }
        case IDC_HOSTS_UPDATE_ALL: {
            const auto source = asking_->Source();
            if (!source || asking_->Candidates().empty()) break;
            const std::wstring text = update::ConfirmAllText(*asking_, *source);
            if (MessageBoxW(text.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION) != IDYES) {
                break;
            }
            if (!same_source(*source)) break;
            logging::Info(L"update: updating the paired hosts one at a time");
            (void)asking_->UpdateAll(GetTickCount64());
            break;
        }
        case IDC_HOSTS_UPDATE_STOP:
            asking_->Stop();
            logging::Info(L"update: update all stopped by the reader");
            break;
        default:
            break;
    }
    Refresh();
    return 0;
}
#endif

bool HostsDialog::ApplyIdentity() {
    wchar_t name[128]{};
    GetDlgItemTextW(IDC_HOSTS_NAME, name, static_cast<int>(std::size(name)));
    BOOL ok = FALSE;
    const UINT port = GetDlgItemInt(IDC_HOSTS_PORT, &ok, FALSE);
    if (!ok || !settings::IsValidHostsPort(port)) {
        identity_error_ =
            Text(L"連接埠須介於 1024 與 65535 之間。", L"The port must be between 1024 and 65535.");
        Refresh();
        return false;
    }
    std::wstring error;
    if (!hosts_.SetIdentity(name, static_cast<int>(port), error)) {
        identity_error_ = error;
        Refresh();
        return false;
    }
    identity_error_.clear();
    return true;
}

std::uint64_t HostsDialog::SelectedPeer() const {
    const int index = ListView_GetNextItem(GetDlgItem(IDC_HOSTS_LIST), -1, LVNI_SELECTED);
    if (index < 0 || static_cast<std::size_t>(index) >= rows_.size()) return 0;
    return rows_[static_cast<std::size_t>(index)];
}

}  // namespace gtg::tray
