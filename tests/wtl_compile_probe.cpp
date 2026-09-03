#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlddx.h>
#include <atldlgs.h>

static_assert(_WTL_VER == 0x1001, "GpuThermalGuard requires WTL 10.01");

namespace {

class CompileOnlyDialog final
    : public ATL::CDialogImpl<CompileOnlyDialog>,
      public WTL::CWinDataExchange<CompileOnlyDialog> {
public:
    enum { IDD = 1 };

    BEGIN_MSG_MAP(CompileOnlyDialog)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
    END_MSG_MAP()

    BEGIN_DDX_MAP(CompileOnlyDialog)
    END_DDX_MAP()

    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) { return TRUE; }
};

}  // namespace

bool WtlHeadersAvailable() {
    return _WTL_VER == 0x1001 && sizeof(CompileOnlyDialog) > 0;
}
