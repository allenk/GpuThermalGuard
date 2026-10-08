#pragma once
#include <windows.h>
#include <cwchar>

namespace gtg::overlay::integration {
// Read-only admission, also rechecked by the helper immediately before loading.
// A name is never evidence of a supported graphics API. Actual D3D11 inspection
// remains mandatory after the hook and before any drawing.
inline bool TargetAdmitted(HANDLE process) noexcept {
    // INVALID_HANDLE_VALUE aliases the current-process pseudo handle. Accept
    // only real handles acquired by the UI/helper, never this accidental alias.
    if (!process || process == INVALID_HANDLE_VALUE) return false;
    const DWORD pid = GetProcessId(process);
    DWORD session{}, own_session{};
    BOOL critical = TRUE;
    USHORT machine{}, native{};
    if (!pid || !ProcessIdToSessionId(pid, &session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || !session ||
        session != own_session || !IsProcessCritical(process, &critical) || critical ||
        !IsWow64Process2(process, &machine, &native) || machine != IMAGE_FILE_MACHINE_UNKNOWN ||
        native != IMAGE_FILE_MACHINE_AMD64 || WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
        return false;
    wchar_t image[32768]{}, windows[MAX_PATH]{};
    DWORD length = 32768;
    const UINT count = GetWindowsDirectoryW(windows, MAX_PATH);
    if (!count || count >= MAX_PATH || !QueryFullProcessImageNameW(process, 0, image, &length))
        return false;
    if (_wcsnicmp(image, windows, count) == 0 &&
        (image[count] == L'\\' || image[count] == L'/' || image[count] == 0))
        return false;
    const auto* slash = wcsrchr(image, L'\\');
    const auto* name = slash ? slash + 1 : image;
    return _wcsicmp(name, L"GpuThermalGuard.exe") != 0;
}
}  // namespace gtg::overlay::integration
