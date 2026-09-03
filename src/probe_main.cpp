#include "nvml/nvml_loader.hpp"

#include <windows.h>

#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

void PrintPower(const std::string_view label, const std::optional<unsigned int> milliwatts) {
    std::cout << "  " << std::left << std::setw(26) << label;
    if (milliwatts.has_value()) {
        std::cout << std::fixed << std::setprecision(1)
                  << static_cast<double>(*milliwatts) / 1000.0 << " W\n";
    } else {
        std::cout << "not supported\n";
    }
}

void PrintMemory(const std::string_view label, const std::optional<std::uint64_t> bytes) {
    std::cout << "  " << std::left << std::setw(26) << label;
    if (bytes.has_value()) {
        constexpr double kBytesPerGiB = 1024.0 * 1024.0 * 1024.0;
        std::cout << std::fixed << std::setprecision(2)
                  << static_cast<double>(*bytes) / kBytesPerGiB << " GiB\n";
    } else {
        std::cout << "not supported\n";
    }
}

template <typename Value>
void PrintOptional(const std::string_view label, const std::optional<Value>& value, const char* unit) {
    std::cout << "  " << std::left << std::setw(26) << label;
    if (value.has_value()) {
        std::cout << *value << ' ' << unit << '\n';
    } else {
        std::cout << "not supported\n";
    }
}

}  // namespace

int main() {
    ::SetConsoleOutputCP(CP_UTF8);
    std::cout << "GPU Thermal Guard - read-only NVML capability probe\n";
    std::cout << "No GPU settings will be changed.\n\n";

    gtg::nvml::Library nvml;
    if (!nvml.Initialize()) {
        std::cerr << "NVML initialization failed: " << nvml.last_error() << '\n';
        return 1;
    }

    std::cout << "Driver: " << nvml.driver_version() << "\n";
    const auto devices = nvml.ProbeDevices();
    if (devices.empty()) {
        std::cerr << "No accessible NVIDIA GPU found.";
        if (!nvml.last_error().empty()) {
            std::cerr << " " << nvml.last_error();
        }
        std::cerr << '\n';
        return 2;
    }

    for (const auto& device : devices) {
        std::cout << "\nGPU " << device.index << ": " << device.name << "\n";
        std::cout << "  " << std::left << std::setw(26) << "UUID" << device.uuid << '\n';
        std::cout << "  " << std::left << std::setw(26) << "VBIOS" << device.vbios_version << '\n';
        PrintOptional("Temperature", device.temperature_c, "C");
        PrintPower("Power usage", device.power_usage_mw);
        PrintMemory("VRAM used", device.memory_used_bytes);
        PrintMemory("VRAM total", device.memory_total_bytes);
        PrintMemory("VRAM reserved", device.memory_reserved_bytes);
        PrintPower("Configured power limit", device.configured_power_limit_mw);
        PrintPower("Enforced power limit", device.enforced_power_limit_mw);
        PrintPower("Default power limit", device.default_power_limit_mw);
        PrintPower("Minimum power limit", device.minimum_power_limit_mw);
        PrintPower("Maximum power limit", device.maximum_power_limit_mw);
        PrintOptional("GPU max T.Limit", device.gpu_max_tlimit_c, "C");
        PrintOptional("Slowdown T.Limit", device.slowdown_tlimit_c, "C");
        PrintOptional("Shutdown T.Limit", device.shutdown_tlimit_c, "C");
        PrintOptional("Thermal margin", device.thermal_margin_c, "C");
    }

    return 0;
}
