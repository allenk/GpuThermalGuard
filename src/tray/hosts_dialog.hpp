#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>

#include "peer/link.hpp"
#include "peer/pairing_code.hpp"
#include "resources/resource.h"
#include "tray/hosts_controller.hpp"

#ifdef GTG_FEATURE_UPDATE
#include "update/hosts_updates.hpp"
#include "update/tray_updates.hpp"
#include "update/update_resource.h"
#endif

// The Hosts dialog: on and off, this machine's name and port, the paired
// hosts in the reader's order, and pairing in both directions -- a code to
// show (Copy, QR) and a box to paste one into.
namespace gtg::tray {

class HostsDialog final : public ATL::CDialogImpl<HostsDialog> {
public:
    enum { IDD = IDD_HOSTS };

    explicit HostsDialog(HostsController& hosts) : hosts_(hosts) {}
#ifdef GTG_FEATURE_UPDATE
    // With the updater: whether paired hosts may update this machine, and
    // asking them to update.
    HostsDialog(HostsController& hosts, update::TrayUpdates& updates, update::HostsUpdates& asking)
        : hosts_(hosts), updates_(&updates), asking_(&asking) {}
#endif

    BEGIN_MSG_MAP(HostsDialog)
    MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
    MESSAGE_HANDLER(WM_TIMER, OnTimer)
    MESSAGE_HANDLER(WM_DRAWITEM, OnDrawItem)
    MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    COMMAND_HANDLER(IDC_HOSTS_ENABLED, BN_CLICKED, OnEnabled)
    COMMAND_HANDLER(IDC_HOSTS_JOIN_CODE, EN_CHANGE, OnJoinCodeChanged)
    COMMAND_ID_HANDLER(IDC_HOSTS_CREATE, OnCreate)
    COMMAND_ID_HANDLER(IDC_HOSTS_COPY, OnCopy)
    COMMAND_ID_HANDLER(IDC_HOSTS_JOIN, OnJoin)
    COMMAND_ID_HANDLER(IDC_HOSTS_UP, OnMove)
    COMMAND_ID_HANDLER(IDC_HOSTS_DOWN, OnMove)
    COMMAND_ID_HANDLER(IDC_HOSTS_REMOVE, OnRemove)
    COMMAND_ID_HANDLER(IDOK, OnClose)
    COMMAND_ID_HANDLER(IDCANCEL, OnClose)
    NOTIFY_HANDLER(IDC_HOSTS_LIST, LVN_ITEMCHANGED, OnListChanged)
#ifdef GTG_FEATURE_UPDATE
    COMMAND_HANDLER(IDC_HOSTS_UPDATE_ALLOW, BN_CLICKED, OnUpdateAllow)
    COMMAND_ID_HANDLER(IDC_HOSTS_UPDATE_CHECK, OnUpdateCommand)
    COMMAND_ID_HANDLER(IDC_HOSTS_UPDATE_ONE, OnUpdateCommand)
    COMMAND_ID_HANDLER(IDC_HOSTS_UPDATE_FORCE, OnUpdateCommand)
    COMMAND_ID_HANDLER(IDC_HOSTS_UPDATE_ALL, OnUpdateCommand)
    COMMAND_ID_HANDLER(IDC_HOSTS_UPDATE_STOP, OnUpdateCommand)
#endif
    END_MSG_MAP()

private:
    static constexpr UINT_PTR kRefreshTimer = 1;
    static constexpr UINT kRefreshIntervalMs = 500;

    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDrawItem(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnEnabled(WORD, WORD, HWND, BOOL&);
    LRESULT OnJoinCodeChanged(WORD, WORD, HWND, BOOL&);
    LRESULT OnCreate(WORD, WORD, HWND, BOOL&);
    LRESULT OnCopy(WORD, WORD, HWND, BOOL&);
    LRESULT OnJoin(WORD, WORD, HWND, BOOL&);
    LRESULT OnMove(WORD, WORD id, HWND, BOOL&);
    LRESULT OnRemove(WORD, WORD, HWND, BOOL&);
    LRESULT OnClose(WORD, WORD, HWND, BOOL&);
    LRESULT OnListChanged(int, LPNMHDR, BOOL&);
#ifdef GTG_FEATURE_UPDATE
    LRESULT OnUpdateAllow(WORD, WORD, HWND, BOOL&);
    LRESULT OnUpdateCommand(WORD, WORD id, HWND, BOOL&);
    void RefreshUpdates();
#endif

    void Localize();
    void Refresh();
    void RefreshList(const peer::Table* table);
    void RefreshPairing(const peer::Table* table);
    void RefreshNote(const peer::Table* table);
    // Applies the name and port boxes. False, with the reason in the note, if
    // the port is not one GTG can use.
    bool ApplyIdentity();
    [[nodiscard]] std::uint64_t SelectedPeer() const;
    void SetText(int id, const std::wstring& text);

    HostsController& hosts_;
    // The peers in the list, in its order.
    std::vector<std::uint64_t> rows_;
    std::string shown_code_;
    // The QR code of shown_code_: one flag per module, row by row.
    std::vector<bool> qr_modules_;
    int qr_size_{};
    // Drawn only while the code is live, so a phone never reads a dead one.
    bool qr_live_{};
    std::optional<peer::PairingCode> pasted_;
    peer::CodeError paste_error_{peer::CodeError::Empty};
    std::uint64_t selected_peer_{};
    std::wstring identity_error_;
#ifdef GTG_FEATURE_UPDATE
    update::TrayUpdates* updates_{};
    update::HostsUpdates* asking_{};
#endif
};

}  // namespace gtg::tray
