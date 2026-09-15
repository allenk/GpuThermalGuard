#include "nvml/nvml_loader.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <sstream>
#include <utility>

#include "logging/logger.hpp"

namespace gtg::nvml {
namespace {

std::atomic<std::uint64_t> g_power_operation_id{0};

template <typename Function>
Function GetExport(void* module, const char* name) noexcept {
    return reinterpret_cast<Function>(
        ::GetProcAddress(static_cast<HMODULE>(module), name));
}

template <typename Function, typename... Args>
std::optional<unsigned int> ReadUnsigned(Function function, Args... args) {
    if (function == nullptr) {
        return std::nullopt;
    }
    unsigned int value = 0;
    if (function(args..., &value) != NVML_SUCCESS) {
        return std::nullopt;
    }
    return value;
}

std::optional<unsigned int> FieldAsUnsigned(const nvmlFieldValue_t& field) {
    if (field.nvmlReturn != NVML_SUCCESS) {
        return std::nullopt;
    }
    switch (field.valueType) {
        case NVML_VALUE_TYPE_UNSIGNED_INT:
            return field.value.uiVal;
        case NVML_VALUE_TYPE_UNSIGNED_LONG:
            return static_cast<unsigned int>(field.value.ulVal);
        case NVML_VALUE_TYPE_UNSIGNED_LONG_LONG:
            return static_cast<unsigned int>(field.value.ullVal);
        case NVML_VALUE_TYPE_SIGNED_LONG_LONG:
            if (field.value.sllVal >= 0) {
                return static_cast<unsigned int>(field.value.sllVal);
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

}  // namespace

Library::~Library() {
    Reset();
}

template <typename Function>
bool Library::BindRequired(Function& function, const char* export_name) {
    function = GetExport<Function>(module_, export_name);
    if (function != nullptr) {
        return true;
    }
    last_error_ = std::string("nvml.dll is missing required export: ") + export_name;
    return false;
}

template <typename Function>
void Library::BindOptional(Function& function, const char* export_name) {
    function = GetExport<Function>(module_, export_name);
}

void Library::Reset() noexcept {
    if (initialized_ && shutdown_ != nullptr) {
        (void)shutdown_();
    }
    initialized_ = false;
    bound_device_ = nullptr;
    bound_device_index_ = 0;
    bound_minimum_power_mw_.reset();
    bound_maximum_power_mw_.reset();
    if (module_ != nullptr) {
        ::FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
}

bool Library::Initialize() {
    Reset();
    last_error_.clear();

    module_ = ::LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module_ == nullptr) {
        const auto error = ::GetLastError();
        std::ostringstream stream;
        stream << "LoadLibraryExW(System32\\nvml.dll) failed with Win32 error " << error;
        last_error_ = stream.str();
        return false;
    }

    const bool required_exports =
        BindRequired(init_v2_, "nvmlInit_v2") &&
        BindRequired(shutdown_, "nvmlShutdown") &&
        BindRequired(error_string_, "nvmlErrorString") &&
        BindRequired(system_get_driver_version_, "nvmlSystemGetDriverVersion") &&
        BindRequired(device_get_count_v2_, "nvmlDeviceGetCount_v2") &&
        BindRequired(device_get_handle_by_index_v2_, "nvmlDeviceGetHandleByIndex_v2") &&
        BindRequired(device_get_name_, "nvmlDeviceGetName") &&
        BindRequired(device_get_uuid_, "nvmlDeviceGetUUID") &&
        BindRequired(device_get_vbios_version_, "nvmlDeviceGetVbiosVersion") &&
        BindRequired(device_get_power_usage_, "nvmlDeviceGetPowerUsage") &&
        BindRequired(device_get_power_limit_, "nvmlDeviceGetPowerManagementLimit") &&
        BindRequired(device_get_default_power_limit_, "nvmlDeviceGetPowerManagementDefaultLimit") &&
        BindRequired(device_get_power_constraints_, "nvmlDeviceGetPowerManagementLimitConstraints") &&
        BindRequired(device_get_field_values_, "nvmlDeviceGetFieldValues");

    if (!required_exports) {
        Reset();
        return false;
    }

    BindOptional(device_get_enforced_power_limit_, "nvmlDeviceGetEnforcedPowerLimit");
    BindOptional(device_get_temperature_v_, "nvmlDeviceGetTemperatureV");
    BindOptional(device_get_temperature_legacy_, "nvmlDeviceGetTemperature");
    BindOptional(device_get_temperature_threshold_, "nvmlDeviceGetTemperatureThreshold");
    BindOptional(device_get_margin_temperature_, "nvmlDeviceGetMarginTemperature");
    BindOptional(device_get_utilization_rates_, "nvmlDeviceGetUtilizationRates");
    BindOptional(device_get_memory_info_v2_, "nvmlDeviceGetMemoryInfo_v2");
    BindOptional(device_get_memory_info_legacy_, "nvmlDeviceGetMemoryInfo");
    BindOptional(device_set_power_limit_, "nvmlDeviceSetPowerManagementLimit");
    if (device_get_temperature_v_ == nullptr && device_get_temperature_legacy_ == nullptr) {
        last_error_ = "nvml.dll exposes neither nvmlDeviceGetTemperatureV nor nvmlDeviceGetTemperature";
        Reset();
        return false;
    }

    const nvmlReturn_t result = init_v2_();
    if (result != NVML_SUCCESS) {
        last_error_ = "nvmlInit_v2 failed: " + ErrorText(result);
        Reset();
        return false;
    }

    initialized_ = true;
    return true;
}

std::string Library::ErrorText(const nvmlReturn_t result) const {
    if (error_string_ == nullptr) {
        return "NVML error " + std::to_string(static_cast<int>(result));
    }
    const char* text = error_string_(result);
    return text != nullptr ? std::string(text) : "Unknown NVML error";
}

std::string Library::driver_version() const {
    if (!initialized_) {
        return {};
    }
    std::array<char, NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE> buffer{};
    const nvmlReturn_t result =
        system_get_driver_version_(buffer.data(), static_cast<unsigned int>(buffer.size()));
    return result == NVML_SUCCESS ? std::string(buffer.data()) : std::string{};
}

std::optional<int> Library::ReadTemperature(const nvmlDevice_t device) const {
    if (device_get_temperature_v_ != nullptr) {
        nvmlTemperature_t temperature{};
        temperature.version = nvmlTemperature_v1;
        temperature.sensorType = NVML_TEMPERATURE_GPU;
        last_temperature_result_ = device_get_temperature_v_(device, &temperature);
        if (last_temperature_result_ == NVML_SUCCESS) {
            return temperature.temperature;
        }
        if (last_temperature_result_ != NVML_ERROR_NOT_SUPPORTED &&
            last_temperature_result_ != NVML_ERROR_FUNCTION_NOT_FOUND &&
            last_temperature_result_ != NVML_ERROR_ARGUMENT_VERSION_MISMATCH) return std::nullopt;
    }

    if (device_get_temperature_legacy_ != nullptr) {
        unsigned int temperature = 0;
        last_temperature_result_ = device_get_temperature_legacy_(device, NVML_TEMPERATURE_GPU, &temperature);
        if (last_temperature_result_ == NVML_SUCCESS) {
            return static_cast<int>(temperature);
        }
    }
    return std::nullopt;
}

void Library::ReadThermalLimits(
    const nvmlDevice_t device,
    DeviceSnapshot& snapshot) const {
    std::array<nvmlFieldValue_t, 3> fields{};
    fields[0].fieldId = NVML_FI_DEV_TEMPERATURE_GPU_MAX_TLIMIT;
    fields[1].fieldId = NVML_FI_DEV_TEMPERATURE_SLOWDOWN_TLIMIT;
    fields[2].fieldId = NVML_FI_DEV_TEMPERATURE_SHUTDOWN_TLIMIT;

    if (device_get_field_values_ != nullptr &&
        device_get_field_values_(device, static_cast<int>(fields.size()), fields.data()) ==
            NVML_SUCCESS) {
        snapshot.gpu_max_tlimit_c = FieldAsUnsigned(fields[0]);
        snapshot.slowdown_tlimit_c = FieldAsUnsigned(fields[1]);
        snapshot.shutdown_tlimit_c = FieldAsUnsigned(fields[2]);
    }

    // R595/R597 drivers can expose these legacy queries on Blackwell while the
    // newer field IDs still report NVML_ERROR_NOT_SUPPORTED. Keep the newer
    // query first and use the old API only as a read-only compatibility path.
    if (!snapshot.gpu_max_tlimit_c.has_value()) {
        snapshot.gpu_max_tlimit_c = ReadUnsigned(
            device_get_temperature_threshold_, device, NVML_TEMPERATURE_THRESHOLD_GPU_MAX);
    }
    if (!snapshot.slowdown_tlimit_c.has_value()) {
        snapshot.slowdown_tlimit_c = ReadUnsigned(
            device_get_temperature_threshold_, device, NVML_TEMPERATURE_THRESHOLD_SLOWDOWN);
    }
    if (!snapshot.shutdown_tlimit_c.has_value()) {
        snapshot.shutdown_tlimit_c = ReadUnsigned(
            device_get_temperature_threshold_, device, NVML_TEMPERATURE_THRESHOLD_SHUTDOWN);
    }

    if (device_get_margin_temperature_ != nullptr) {
        nvmlMarginTemperature_t margin{};
        margin.version = nvmlMarginTemperature_v1;
        if (device_get_margin_temperature_(device, &margin) == NVML_SUCCESS) {
            snapshot.thermal_margin_c = margin.marginTemperature;
        }
    }
}

std::vector<DeviceSnapshot> Library::ProbeDevices() {
    std::vector<DeviceSnapshot> snapshots;
    if (!initialized_) {
        last_error_ = "NVML is not initialized";
        return snapshots;
    }

    unsigned int count = 0;
    nvmlReturn_t result = device_get_count_v2_(&count);
    if (result != NVML_SUCCESS) {
        last_error_ = "nvmlDeviceGetCount_v2 failed: " + ErrorText(result);
        return snapshots;
    }

    snapshots.reserve(count);
    for (unsigned int index = 0; index < count; ++index) {
        nvmlDevice_t device = nullptr;
        result = device_get_handle_by_index_v2_(index, &device);
        if (result != NVML_SUCCESS) {
            last_error_ = "nvmlDeviceGetHandleByIndex_v2 failed: " + ErrorText(result);
            continue;
        }

        DeviceSnapshot snapshot;
        snapshot.index = index;

        std::array<char, 256> text{};
        if (device_get_name_(device, text.data(), static_cast<unsigned int>(text.size())) ==
            NVML_SUCCESS) {
            snapshot.name = text.data();
        }
        text.fill('\0');
        if (device_get_uuid_(device, text.data(), static_cast<unsigned int>(text.size())) ==
            NVML_SUCCESS) {
            snapshot.uuid = text.data();
        }
        text.fill('\0');
        if (device_get_vbios_version_(device, text.data(), static_cast<unsigned int>(text.size())) ==
            NVML_SUCCESS) {
            snapshot.vbios_version = text.data();
        }

        snapshot.temperature_c = ReadTemperature(device);
        snapshot.power_usage_mw = ReadUnsigned(device_get_power_usage_, device);
        bool memory_read = false;
        if (device_get_memory_info_v2_ != nullptr) {
            nvmlMemory_v2_t memory{};
            memory.version = nvmlMemory_v2;
            if (device_get_memory_info_v2_(device, &memory) == NVML_SUCCESS) {
                snapshot.memory_total_bytes = memory.total;
                snapshot.memory_used_bytes = memory.used;
                snapshot.memory_reserved_bytes = memory.reserved;
                memory_read = true;
            }
        }
        if (!memory_read && device_get_memory_info_legacy_ != nullptr) {
            nvmlMemory_t memory{};
            if (device_get_memory_info_legacy_(device, &memory) == NVML_SUCCESS) {
                snapshot.memory_total_bytes = memory.total;
                snapshot.memory_used_bytes = memory.used;
            }
        }
        if (device_get_utilization_rates_ != nullptr) {
            nvmlUtilization_t utilization{};
            if (device_get_utilization_rates_(device, &utilization) == NVML_SUCCESS) {
                snapshot.gpu_utilization_percent = utilization.gpu;
            }
        }
        snapshot.configured_power_limit_mw = ReadUnsigned(device_get_power_limit_, device);
        snapshot.enforced_power_limit_mw =
            ReadUnsigned(device_get_enforced_power_limit_, device);
        snapshot.default_power_limit_mw =
            ReadUnsigned(device_get_default_power_limit_, device);

        unsigned int minimum = 0;
        unsigned int maximum = 0;
        if (device_get_power_constraints_(device, &minimum, &maximum) == NVML_SUCCESS) {
            snapshot.minimum_power_limit_mw = minimum;
            snapshot.maximum_power_limit_mw = maximum;
        }

        ReadThermalLimits(device, snapshot);
        snapshots.push_back(std::move(snapshot));
    }

    return snapshots;
}

bool Library::BindDevice(const unsigned int device_index) {
    bound_device_ = nullptr;
    bound_uuid_.clear();
    if (!initialized_) {
        last_error_ = "NVML is not initialized";
        return false;
    }
    nvmlDevice_t device = nullptr;
    const nvmlReturn_t handle_result =
        device_get_handle_by_index_v2_(device_index, &device);
    if (handle_result != NVML_SUCCESS) {
        last_error_ = "nvmlDeviceGetHandleByIndex_v2 failed: " + ErrorText(handle_result);
        return false;
    }

    std::array<char, NVML_DEVICE_UUID_BUFFER_SIZE> uuid{};
    const auto uuid_result = device_get_uuid_(device, uuid.data(), static_cast<unsigned>(uuid.size()));
    if (uuid_result != NVML_SUCCESS) {
        last_error_ = "nvmlDeviceGetUUID failed: " + ErrorText(uuid_result);
        return false;
    }

    unsigned int minimum = 0;
    unsigned int maximum = 0;
    const nvmlReturn_t constraint_result =
        device_get_power_constraints_(device, &minimum, &maximum);
    if (constraint_result != NVML_SUCCESS) {
        last_error_ = "nvmlDeviceGetPowerManagementLimitConstraints failed: " +
                      ErrorText(constraint_result);
        return false;
    }

    bound_device_ = device;
    bound_uuid_ = uuid.data();
    bound_device_index_ = device_index;
    bound_minimum_power_mw_ = minimum;
    bound_maximum_power_mw_ = maximum;
    last_error_.clear();
    return true;
}

bool Library::BindDeviceByUuid(const std::string& uuid) {
    if (uuid.empty()) return BindDevice(0);
    bound_device_ = nullptr;
    if (!initialized_) return false;
    unsigned count{};
    if (device_get_count_v2_(&count) != NVML_SUCCESS) return false;
    for (unsigned index = 0; index < count; ++index) {
        if (BindDevice(index) && bound_uuid_ == uuid) return true;
    }
    bound_device_ = nullptr;
    bound_uuid_.clear();
    last_error_ = "Original GPU UUID unavailable; refusing another GPU";
    return false;
}

std::optional<int> Library::ReadBoundTemperature() {
    if (bound_device_ == nullptr) {
        last_error_ = "No NVML device is bound";
        return std::nullopt;
    }
    const auto temperature = ReadTemperature(bound_device_);
    if (!temperature.has_value()) {
        last_error_ = "bound-device temperature query failed code=" +
            std::to_string(static_cast<int>(last_temperature_result_)) + ": " + ErrorText(last_temperature_result_);
    } else {
        last_error_.clear();
    }
    return temperature;
}

std::optional<unsigned int> Library::ReadBoundPowerUsageMillwatts() {
    if (bound_device_ == nullptr) {
        last_error_ = "No NVML device is bound";
        return std::nullopt;
    }
    auto value = ReadUnsigned(device_get_power_usage_, bound_device_);
    if (!value.has_value()) {
        last_error_ = "bound-device power-usage query failed";
    } else {
        last_error_.clear();
    }
    return value;
}

std::optional<unsigned int> Library::ReadBoundPowerLimitMillwatts() {
    if (bound_device_ == nullptr) {
        last_error_ = "No NVML device is bound";
        return std::nullopt;
    }
    auto value = ReadUnsigned(device_get_power_limit_, bound_device_);
    if (!value.has_value()) {
        last_error_ = "bound-device power-limit query failed";
    } else {
        last_error_.clear();
    }
    return value;
}

bool Library::SetBoundPowerLimitWatts(const unsigned int watts, const wchar_t* context) {
    last_power_result_ = {};
    last_power_result_.operation_id =
        g_power_operation_id.fetch_add(1, std::memory_order_relaxed) + 1;
    last_power_result_.requested_mw = static_cast<std::uint64_t>(watts) * 1000ULL;
    auto finish = [&]() {
        // Never let formatting/allocation failure alter the hardware result.
        // This bounded queue does not wait on disk or the journal mutex.
        try {
            const auto message = FormatPowerOperationResult(last_power_result_,
                context != nullptr ? context : L"power");
            if (last_power_result_.verified) {
                (void)logging::TryInfo(message);
            } else {
                (void)logging::TryWarning(message);
            }
        } catch (...) {
            (void)logging::TryWarning(L"POWER_OP diagnostic formatting failed");
        }
        return last_power_result_.verified;
    };
    auto precondition_failure = [&](const char* message) {
        last_error_ = message;
        last_power_result_.precondition_error = message;
        return finish();
    };
    if (bound_device_ == nullptr || !bound_minimum_power_mw_ || !bound_maximum_power_mw_) {
        return precondition_failure("No NVML device is bound");
    }
    if (device_set_power_limit_ == nullptr) {
        return precondition_failure("nvml.dll does not expose nvmlDeviceSetPowerManagementLimit");
    }
    if (device_get_power_limit_ == nullptr) {
        return precondition_failure("nvml.dll does not expose nvmlDeviceGetPowerManagementLimit");
    }
    if (watts > (std::numeric_limits<unsigned int>::max() / 1000U)) {
        return precondition_failure("Requested power limit overflows NVML milliwatts");
    }
    const unsigned int milliwatts = watts * 1000U;
    if (milliwatts < *bound_minimum_power_mw_ || milliwatts > *bound_maximum_power_mw_) {
        return precondition_failure("Requested power limit is outside the device constraints");
    }
    const auto id = last_power_result_.operation_id;
    try {
        last_power_result_ = ExecutePowerOperation(milliwatts,
            [&]() { return device_set_power_limit_(bound_device_, milliwatts); },
            [&](unsigned int* value) { return device_get_power_limit_(bound_device_, value); },
            [&](nvmlReturn_t result) { return ErrorText(result); },
            []() {
                return std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            });
    } catch (...) {
        // A diagnostic allocation may fail after hardware accepted the write.
        // Return unverified so the caller retains its latch and performs safe
        // fallback; do not let an exception bypass that safety transaction.
        (void)logging::TryError(L"POWER_OP evidence capture failed; hardware state unverified");
        try { last_error_ = "power operation evidence capture failed; hardware state unverified"; }
        catch (...) { last_error_.clear(); }
        return finish();
    }
    last_power_result_.operation_id = id;
    try {
        if (last_power_result_.verified) {
            last_error_.clear();
        } else if (last_power_result_.setter->code != NVML_SUCCESS) {
            last_error_ = "nvmlDeviceSetPowerManagementLimit failed: " +
                last_power_result_.setter->message;
        } else {
            const auto& read = last_power_result_.recheck ?
                *last_power_result_.recheck : *last_power_result_.readback;
            last_error_ = read.code != NVML_SUCCESS ?
                "power limit write acknowledged but read-back failed: " + read.message :
                "power limit mismatch: requested " + std::to_string(milliwatts) +
                    " mW, observed " + std::to_string(*read.actual_mw) + " mW";
        }
    } catch (...) {
        last_error_.clear();
    }
    return finish();
}

bool Library::SetPowerLimitWatts(const unsigned int device_index, const unsigned int watts) {
    if (!initialized_) {
        last_error_ = "NVML is not initialized";
        return false;
    }
    if (device_set_power_limit_ == nullptr) {
        last_error_ = "nvml.dll does not expose nvmlDeviceSetPowerManagementLimit";
        return false;
    }
    if (watts > (std::numeric_limits<unsigned int>::max() / 1000U)) {
        last_error_ = "Requested power limit overflows NVML milliwatts";
        return false;
    }

    if (bound_device_ == nullptr || bound_device_index_ != device_index) {
        if (!BindDevice(device_index)) return false;
    }
    return SetBoundPowerLimitWatts(watts);
}

}  // namespace gtg::nvml
