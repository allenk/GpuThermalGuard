#pragma once

#include <windows.h>

#include <string>
#include <utility>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>

#include "update/preference.hpp"
#include "update/update_resource.h"

// Update settings: the source, automatic checking and the signer pin. The
// dialog saves only what it can validate; GTG runs elevated, which is what
// HKLM needs.
namespace gtg::update {

class UpdateSettingsDialog final : public ATL::CDialogImpl<UpdateSettingsDialog> {
public:
    enum { IDD = IDD_UPDATE };

    UpdateSettingsDialog(UpdatePreference current, std::wstring this_build,
                         std::wstring source_release)
        : current_(std::move(current)),
          this_build_(std::move(this_build)),
          source_release_(std::move(source_release)) {}

    BEGIN_MSG_MAP(UpdateSettingsDialog)
    MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
    COMMAND_ID_HANDLER(IDOK, OnSave)
    COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
    END_MSG_MAP()

private:
    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnSave(WORD, WORD, HWND, BOOL&);
    LRESULT OnCancel(WORD, WORD, HWND, BOOL&);

    [[nodiscard]] std::wstring Text(int id) const;
    void Note(const std::wstring& text);

    UpdatePreference current_;
    // The source as the dialog opened with it; an invalid one is kept unless
    // the reader changes it.
    std::wstring shown_source_;
    std::wstring this_build_;
    std::wstring source_release_;
};

}  // namespace gtg::update
