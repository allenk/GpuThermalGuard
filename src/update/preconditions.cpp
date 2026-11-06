#include "update/preconditions.hpp"

namespace gtg::update {

bool ServiceInstalled() noexcept {
    const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) return false;
    const SC_HANDLE service = OpenServiceW(manager, L"GpuThermalGuard", SERVICE_QUERY_STATUS);
    const bool installed = service != nullptr || GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST;
    if (service != nullptr) CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return installed;
}

std::optional<DWORD> SmartAppControlState() noexcept {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\CI\\Policy",
                     L"VerifiedAndReputablePolicyState", RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return value;
}

bool UpdateRunning(const wchar_t* mutex_name) noexcept {
    const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, mutex_name);
    if (mutex == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
    CloseHandle(mutex);
    return true;
}

}  // namespace gtg::update
