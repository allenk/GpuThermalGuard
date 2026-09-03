#include "nvml/nvml_loader.hpp"

#include <windows.h>

#include <array>
#include <limits>
#include <sstream>
#include <utility>

namespace gtg::nvml {
namespace {

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
        if (device_get_temperature_v_(device, &temperature) == NVML_SUCCESS) {
            return temperature.temperature;
        }
    }

    if (device_get_temperature_legacy_ != nullptr) {
        unsigned int temperature = 0;
        if (device_get_temperature_legacy_(device, NVML_TEMPERATURE_GPU, &temperature) ==
            NVML_SUCCESS) {
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

    nvmlDevice_t device = nullptr;
    nvmlReturn_t result = device_get_handle_by_index_v2_(device_index, &device);
    if (result != NVML_SUCCESS) {
        last_error_ = "nvmlDeviceGetHandleByIndex_v2 failed: " + ErrorText(result);
        return false;
    }

    unsigned int minimum = 0;
    unsigned int maximum = 0;
    result = device_get_power_constraints_(device, &minimum, &maximum);
    if (result != NVML_SUCCESS) {
        last_error_ = "nvmlDeviceGetPowerManagementLimitConstraints failed: " + ErrorText(result);
        return false;
    }
    const unsigned int milliwatts = watts * 1000U;
    if (milliwatts < minimum || milliwatts > maximum) {
        last_error_ = "Requested power limit is outside the device constraints";
        return false;
    }

    result = device_set_power_limit_(device, milliwatts);
    if (result != NVML_SUCCESS) {
        last_error_ = "nvmlDeviceSetPowerManagementLimit failed: " + ErrorText(result);
        return false;
    }
    unsigned int verified = 0;
    result = device_get_power_limit_(device, &verified);
    if (result != NVML_SUCCESS) {
        last_error_ = "power limit was written but read-back failed: " + ErrorText(result);
        return false;
    }
    if (verified != milliwatts) {
        last_error_ = "power limit read-back did not match the requested value";
        return false;
    }
    last_error_.clear();
    return true;
}

}  // namespace gtg::nvml
