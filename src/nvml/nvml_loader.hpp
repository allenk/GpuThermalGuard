#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nvml.h>
#include "nvml/power_operation.hpp"

namespace gtg::nvml {

struct DeviceSnapshot {
    unsigned int index{};
    std::string name;
    std::string uuid;
    std::string vbios_version;
    std::optional<int> temperature_c;
    std::optional<unsigned int> power_usage_mw;
    std::optional<std::uint64_t> memory_total_bytes;
    std::optional<std::uint64_t> memory_used_bytes;
    std::optional<std::uint64_t> memory_reserved_bytes;
    std::optional<unsigned int> gpu_utilization_percent;
    std::optional<unsigned int> configured_power_limit_mw;
    std::optional<unsigned int> enforced_power_limit_mw;
    std::optional<unsigned int> default_power_limit_mw;
    std::optional<unsigned int> minimum_power_limit_mw;
    std::optional<unsigned int> maximum_power_limit_mw;
    std::optional<unsigned int> gpu_max_tlimit_c;
    std::optional<unsigned int> slowdown_tlimit_c;
    std::optional<unsigned int> shutdown_tlimit_c;
    std::optional<int> thermal_margin_c;
};

class Library final {
public:
    Library() = default;
    ~Library();

    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    [[nodiscard]] bool Initialize();
    [[nodiscard]] std::string driver_version() const;
    [[nodiscard]] std::vector<DeviceSnapshot> ProbeDevices();
    [[nodiscard]] bool BindDevice(unsigned int device_index);
    [[nodiscard]] bool BindDeviceByUuid(const std::string& uuid);
    [[nodiscard]] const std::string& bound_uuid() const noexcept { return bound_uuid_; }
    [[nodiscard]] std::optional<int> ReadBoundTemperature();
    [[nodiscard]] std::optional<unsigned int> ReadBoundPowerUsageMillwatts();
    [[nodiscard]] std::optional<unsigned int> ReadBoundPowerLimitMillwatts();
    [[nodiscard]] bool SetBoundPowerLimitWatts(
        unsigned int watts, const wchar_t* context = L"power");
    [[nodiscard]] bool SetPowerLimitWatts(unsigned int device_index, unsigned int watts);
    [[nodiscard]] bool can_set_power_limit() const noexcept {
        return device_set_power_limit_ != nullptr;
    }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
    // Copy before another setter if retaining evidence across safe fallback.
    [[nodiscard]] const PowerOperationResult& last_power_result() const noexcept {
        return last_power_result_;
    }

private:
    template <typename Function>
    bool BindRequired(Function& function, const char* export_name);

    template <typename Function>
    void BindOptional(Function& function, const char* export_name);

    [[nodiscard]] std::string ErrorText(nvmlReturn_t result) const;
    [[nodiscard]] std::optional<int> ReadTemperature(nvmlDevice_t device) const;
    void ReadThermalLimits(nvmlDevice_t device, DeviceSnapshot& snapshot) const;
    void Reset() noexcept;

    void* module_{nullptr};
    bool initialized_{false};
    std::string last_error_;
    std::string bound_uuid_;
    mutable nvmlReturn_t last_temperature_result_{NVML_SUCCESS};
    PowerOperationResult last_power_result_;
    nvmlDevice_t bound_device_{nullptr};
    unsigned int bound_device_index_{};
    std::optional<unsigned int> bound_minimum_power_mw_;
    std::optional<unsigned int> bound_maximum_power_mw_;

    decltype(&::nvmlInit_v2) init_v2_{nullptr};
    decltype(&::nvmlShutdown) shutdown_{nullptr};
    decltype(&::nvmlErrorString) error_string_{nullptr};
    decltype(&::nvmlSystemGetDriverVersion) system_get_driver_version_{nullptr};
    decltype(&::nvmlDeviceGetCount_v2) device_get_count_v2_{nullptr};
    decltype(&::nvmlDeviceGetHandleByIndex_v2) device_get_handle_by_index_v2_{nullptr};
    decltype(&::nvmlDeviceGetName) device_get_name_{nullptr};
    decltype(&::nvmlDeviceGetUUID) device_get_uuid_{nullptr};
    decltype(&::nvmlDeviceGetVbiosVersion) device_get_vbios_version_{nullptr};
    decltype(&::nvmlDeviceGetTemperatureV) device_get_temperature_v_{nullptr};
    decltype(&::nvmlDeviceGetTemperature) device_get_temperature_legacy_{nullptr};
    decltype(&::nvmlDeviceGetTemperatureThreshold) device_get_temperature_threshold_{nullptr};
    decltype(&::nvmlDeviceGetMarginTemperature) device_get_margin_temperature_{nullptr};
    decltype(&::nvmlDeviceGetPowerUsage) device_get_power_usage_{nullptr};
    decltype(&::nvmlDeviceGetUtilizationRates) device_get_utilization_rates_{nullptr};
    decltype(&::nvmlDeviceGetMemoryInfo_v2) device_get_memory_info_v2_{nullptr};
    decltype(&::nvmlDeviceGetMemoryInfo) device_get_memory_info_legacy_{nullptr};
    decltype(&::nvmlDeviceGetPowerManagementLimit) device_get_power_limit_{nullptr};
    decltype(&::nvmlDeviceGetEnforcedPowerLimit) device_get_enforced_power_limit_{nullptr};
    decltype(&::nvmlDeviceGetPowerManagementDefaultLimit) device_get_default_power_limit_{nullptr};
    decltype(&::nvmlDeviceGetPowerManagementLimitConstraints) device_get_power_constraints_{nullptr};
    decltype(&::nvmlDeviceSetPowerManagementLimit) device_set_power_limit_{nullptr};
    decltype(&::nvmlDeviceGetFieldValues) device_get_field_values_{nullptr};
};

}  // namespace gtg::nvml
