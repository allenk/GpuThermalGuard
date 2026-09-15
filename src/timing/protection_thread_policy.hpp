#pragma once

#include <windows.h>

namespace gtg::timing {

struct ProtectionThreadPolicyResult {
    bool description_set{};
    bool priority_set{};
    int effective_priority{THREAD_PRIORITY_ERROR_RETURN};
    DWORD priority_error{};
    bool high_qos_set{};
    DWORD high_qos_error{};
};

// Apply scheduling policy only to the low-duty-cycle protection worker. Keep
// the process in NORMAL_PRIORITY_CLASS and deliberately avoid TIME_CRITICAL.
[[nodiscard]] inline ProtectionThreadPolicyResult ConfigureProtectionThread(
    const wchar_t* description) noexcept {
    ProtectionThreadPolicyResult result;
    const HANDLE thread = GetCurrentThread();
    result.description_set = SUCCEEDED(SetThreadDescription(thread, description));

    result.priority_set = SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST) != FALSE;
    if (!result.priority_set) result.priority_error = GetLastError();
    result.effective_priority = GetThreadPriority(thread);
    if (result.effective_priority == THREAD_PRIORITY_ERROR_RETURN &&
        result.priority_error == ERROR_SUCCESS) {
        result.priority_error = GetLastError();
    }

    THREAD_POWER_THROTTLING_STATE throttling{};
    throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;  // HighQoS: disable execution-speed throttling.
    result.high_qos_set = SetThreadInformation(
        thread, ThreadPowerThrottling, &throttling, sizeof(throttling)) != FALSE;
    if (!result.high_qos_set) result.high_qos_error = GetLastError();
    return result;
}

}  // namespace gtg::timing
