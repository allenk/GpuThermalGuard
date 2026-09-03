#include "service/service_main.hpp"

#include <windows.h>
#include <sddl.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <optional>

#include "core/protection.hpp"
#include "core/restore_policy.hpp"
#include "ipc/protocol.hpp"
#include "nvml/nvml_loader.hpp"
#include "settings/settings.hpp"
#include "logging/logger.hpp"

namespace gtg::service {
namespace {

constexpr wchar_t kServiceName[] = L"GpuThermalGuard";
constexpr DWORD kPollIntervalMs = 250;

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop_event = nullptr;

std::wstring AsciiToWide(const char* text) {
    if (text == nullptr) return {};
    return std::wstring(text, text + std::strlen(text));
}

class PipeServer final {
public:
    PipeServer() {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, FALSE};
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1,
                &descriptor, nullptr) != FALSE) {
            attributes.lpSecurityDescriptor = descriptor;
        }
        pipe_ = CreateNamedPipeW(ipc::kPipeName, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT,
            1, sizeof(ipc::Response), sizeof(ipc::Request), 0,
            descriptor != nullptr ? &attributes : nullptr);
        if (descriptor != nullptr) LocalFree(descriptor);
    }
    ~PipeServer() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    }
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    [[nodiscard]] std::optional<ipc::Request> Poll() {
        if (pipe_ == INVALID_HANDLE_VALUE) return std::nullopt;
        if (!connected_) {
            connected_ = ConnectNamedPipe(pipe_, nullptr) != FALSE ||
                GetLastError() == ERROR_PIPE_CONNECTED;
            if (!connected_) return std::nullopt;
        }
        ipc::Request request{};
        DWORD read = 0;
        if (ReadFile(pipe_, &request, sizeof(request), &read, nullptr) != FALSE &&
            read == sizeof(request) && ipc::IsValid(request)) {
            return request;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE) ResetConnection();
        return std::nullopt;
    }

    void Reply(const ipc::Response& response) {
        if (!connected_) return;
        DWORD written = 0;
        (void)WriteFile(pipe_, &response, sizeof(response), &written, nullptr);
        (void)FlushFileBuffers(pipe_);
        ResetConnection();
    }

private:
    void ResetConnection() {
        if (connected_) DisconnectNamedPipe(pipe_);
        connected_ = false;
    }
    HANDLE pipe_{INVALID_HANDLE_VALUE};
    bool connected_{false};
};

void DebugLog(const std::wstring& message) {
    OutputDebugStringW((L"GpuThermalGuard: " + message + L"\n").c_str());
    logging::Info(message);
}

void ReportStatus(DWORD state, DWORD win32_exit_code = NO_ERROR, DWORD wait_hint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = win32_exit_code;
    g_status.dwWaitHint = wait_hint;
    g_status.dwControlsAccepted = state == SERVICE_START_PENDING
        ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
        ? 0 : g_status.dwCheckPoint + 1;
    if (g_status_handle != nullptr) SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI ControlHandler(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 3'000);
        if (g_stop_event != nullptr) SetEvent(g_stop_event);
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_INTERROGATE) {
        ReportStatus(g_status.dwCurrentState);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD, wchar_t**) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, ControlHandler, nullptr);
    if (g_status_handle == nullptr) return;
    logging::Info(L"SCM entered ServiceMain");
    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3'000);

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr) {
        ReportStatus(SERVICE_STOPPED, GetLastError());
        return;
    }

    nvml::Library nvml;
    if (!nvml.Initialize()) {
        logging::Error(std::format(L"NVML initialization failed: {}",
                                   std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        ReportStatus(SERVICE_STOPPED, ERROR_DEVICE_NOT_AVAILABLE);
        return;
    }

    const ProtectionConfig config = settings::Load().config;
    logging::Info(std::format(L"service config normal={} W safe={} W trigger={} C auto_restore={}",
        config.normal_power_w, config.safe_power_w, config.trigger_temperature_c,
        config.auto_restore ? L"true" : L"false"));
    ProtectionController controller(config);
    if (settings::LoadSafeLatch()) {
        (void)controller.RestorePersistedSafeLatch();
        logging::Warning(L"service restored persisted safe-power latch");
    }
    PipeServer pipe_server;
    std::int64_t last_retry_ms = 0;
    std::optional<int> current_temperature;
    unsigned int current_gpu_index = 0;
    std::optional<ProtectionState> last_logged_state;
    std::int64_t last_snapshot_log_ms = 0;
    std::int64_t last_auto_restore_attempt_ms = 0;
    std::uint32_t trigger_count = settings::LoadTriggerCount();
    bool unavailable_logged = false;
    ReportStatus(SERVICE_RUNNING);
    logging::Info(L"service protection loop running");

    while (WaitForSingleObject(g_stop_event, kPollIntervalMs) == WAIT_TIMEOUT) {
        const auto devices = nvml.ProbeDevices();
        if (devices.empty() || !devices.front().temperature_c.has_value()) {
            (void)controller.SensorUnavailable();
            current_temperature.reset();
            if (!unavailable_logged) {
                logging::Error(std::format(L"service GPU snapshot unavailable: {}",
                    std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
                unavailable_logged = true;
            }
        } else {
            const auto& gpu = devices.front();
            if (unavailable_logged) logging::Info(L"service GPU snapshot stream recovered");
            unavailable_logged = false;
            current_temperature = *gpu.temperature_c;
            current_gpu_index = gpu.index;
            const auto now = static_cast<std::int64_t>(GetTickCount64());
            auto decision = controller.ObserveTemperature(now, *gpu.temperature_c);
            if (decision.action == ProtectionAction::ApplySafePower) {
                const bool applied = nvml.SetPowerLimitWatts(
                    gpu.index, static_cast<unsigned int>(config.safe_power_w));
                std::wstring latch_error;
                (void)settings::SaveSafeLatch(true, latch_error);
                logging::Warning(std::format(L"TRIGGER reason={} temp={} C rise={:.2f} C/s predicted={:.1f} C; requested={} W result={}",
                    AsciiToWide(ToString(decision.trip_reason)),
                    *gpu.temperature_c, decision.rise_c_per_s,
                    decision.predicted_temperature_c, config.safe_power_w,
                    applied ? L"verified" : L"failed"));
                if (!applied) {
                    logging::Error(std::format(L"failed to apply safe power limit: {}",
                        std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
                } else {
                    ++trigger_count;
                    std::wstring counter_error;
                    if (!settings::SaveTriggerCount(trigger_count, counter_error)) {
                        logging::Warning(counter_error);
                    }
                    DebugLog(std::format(L"safe power {} W applied at {} C", config.safe_power_w,
                                         *gpu.temperature_c));
                }
                last_retry_ms = now;
            }
            // Keep all diagnostic file I/O after the first power-limit setter.
            if (!last_logged_state || *last_logged_state != decision.state) {
                logging::Info(std::format(L"service protection state -> {}; temp={} C rise={:.2f} C/s predicted={:.1f} C",
                    AsciiToWide(ToString(decision.state)),
                    *gpu.temperature_c, decision.rise_c_per_s, decision.predicted_temperature_c));
                last_logged_state = decision.state;
            }
            if (ShouldAutomaticallyRestore(config.auto_restore, decision.state) &&
                (last_auto_restore_attempt_ms == 0 ||
                 now - last_auto_restore_attempt_ms >= 5'000)) {
                last_auto_restore_attempt_ms = now;
                logging::Info(std::format(
                    L"service automatic restore eligible; temp={} C target={} W trips={}",
                    *gpu.temperature_c, config.normal_power_w, trigger_count));
                if (nvml.SetPowerLimitWatts(gpu.index,
                                            static_cast<unsigned int>(config.normal_power_w))) {
                    decision = controller.RequestRestore(now, *gpu.temperature_c);
                    std::wstring latch_error;
                    (void)settings::SaveSafeLatch(false, latch_error);
                    logging::Info(std::format(
                        L"service automatic normal-power restore verified: {} W; trips retained={}; protection rearmed",
                        config.normal_power_w, trigger_count));
                } else {
                    logging::Error(std::format(
                        L"service automatic normal-power restore failed; safe latch retained; retry in 5 s: {}",
                        std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
                }
            }
            if (now - last_snapshot_log_ms >= 5'000) {
                last_snapshot_log_ms = now;
                logging::Info(std::format(L"snapshot temp_c={} power_w={} limit_w={} state={}",
                    *gpu.temperature_c,
                    gpu.power_usage_mw ? std::format(L"{:.1f}", *gpu.power_usage_mw / 1000.0) : L"null",
                    gpu.configured_power_limit_mw ? std::format(L"{:.0f}", *gpu.configured_power_limit_mw / 1000.0) : L"null",
                    AsciiToWide(ToString(decision.state))));
            }
            if (decision.safe_latched && now - last_retry_ms >= 2'000) {
                const bool safe_is_effective = gpu.configured_power_limit_mw.has_value() &&
                    *gpu.configured_power_limit_mw <=
                        static_cast<unsigned int>(config.safe_power_w * 1000);
                if (!safe_is_effective) {
                    if (nvml.SetPowerLimitWatts(gpu.index,
                                                static_cast<unsigned int>(config.safe_power_w))) {
                        logging::Warning(std::format(L"service re-applied safe power after drift: {} W",
                                                     config.safe_power_w));
                    } else {
                        logging::Error(std::format(L"service safe-power retry failed: {}",
                            std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
                    }
                }
                last_retry_ms = now;
            }
        }

        const auto request = pipe_server.Poll();
        if (request.has_value()) {
            ipc::Response response{};
            response.protection_state = static_cast<std::uint32_t>(controller.state());
            response.temperature_c = current_temperature.value_or(-1);
            response.normal_power_w = config.normal_power_w;
            response.safe_power_w = config.safe_power_w;
            response.trigger_temperature_c = config.trigger_temperature_c;
            response.trigger_count = trigger_count;
            if (controller.safe_latched()) response.flags |= ipc::SafeLatched;
            if (controller.state() == ProtectionState::ReadyToRestore)
                response.flags |= ipc::ReadyToRestore;

            if (request->command == ipc::Command::RestoreNormalPower) {
                logging::Info(L"tray requested normal-power restore");
                if (!current_temperature.has_value() ||
                    controller.state() != ProtectionState::ReadyToRestore) {
                    response.win32_error = ERROR_NOT_READY;
                } else if (!nvml.SetPowerLimitWatts(current_gpu_index,
                    static_cast<unsigned int>(config.normal_power_w))) {
                    response.win32_error = ERROR_WRITE_FAULT;
                    logging::Error(std::format(L"service normal-power restore failed: {}",
                        std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
                } else {
                    (void)controller.RequestRestore(static_cast<std::int64_t>(GetTickCount64()),
                                                    *current_temperature);
                    std::wstring latch_error;
                    (void)settings::SaveSafeLatch(false, latch_error);
                    response.flags = ipc::LastCommandSucceeded;
                    if (ShouldResetTriggerCount(RestoreInitiator::Manual)) trigger_count = 0;
                    std::wstring counter_error;
                    if (!settings::SaveTriggerCount(trigger_count, counter_error)) {
                        logging::Warning(counter_error);
                    }
                    response.trigger_count = trigger_count;
                    response.protection_state = static_cast<std::uint32_t>(controller.state());
                    logging::Info(std::format(L"service restored and verified normal power: {} W",
                                              config.normal_power_w));
                }
            }
            pipe_server.Reply(response);
        }
    }

    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    ReportStatus(SERVICE_STOPPED);
    logging::Info(L"service protection loop stopped");
}

}  // namespace

int Run() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<wchar_t*>(kServiceName), ServiceMain},
        {nullptr, nullptr},
    };
    if (StartServiceCtrlDispatcherW(table) != FALSE) return 0;
    return static_cast<int>(GetLastError());
}

}  // namespace gtg::service
