#pragma once
#include "load_protocol.hpp"

namespace gtg::research {
constexpr DWORD kDeviceDiagnosticMagic = 0x44475447;
enum class DeviceStep : LONG {
    kRecursion = 1,
    kChainIdentity,
    kPresentDevice,
    kPresentDeviceIdentity,
    kSignalContext,
    kQueueIdentity,
    kQueueDevice,
    kQueueDeviceIdentity
};

struct alignas(8) DeviceDiagnostics {
    DWORD magic{}, size{}, pid{}, reserved{};
    volatile LONG first_state{}, first_step{}, first_hresult{}, first_depth{};
    volatile LONG64 first_chain{};
    volatile LONG64 dx11_calls{}, dx12_calls{}, neither_calls{};
};

inline void DeviceSectionName(DWORD pid, wchar_t (&name)[64]) {
    SectionName(pid, name);
    wcscat_s(name, L".Device");
}

inline void RecordDeviceFailure(DeviceDiagnostics& data, DeviceStep step, HRESULT result,
                                unsigned depth, const void* chain) noexcept {
    if (InterlockedCompareExchange(&data.first_state, 1, 0) != 0) return;
    data.first_step = static_cast<LONG>(step);
    data.first_hresult = result;
    data.first_depth = static_cast<LONG>(depth);
    data.first_chain = reinterpret_cast<LONG64>(chain);
    InterlockedExchange(&data.first_state, 2); // Release publication; readers require state2.
}

inline void RecordDeviceCapabilities(DeviceDiagnostics& data, HRESULT dx11, HRESULT dx12) noexcept {
    if (SUCCEEDED(dx11)) InterlockedIncrement64(&data.dx11_calls);
    if (SUCCEEDED(dx12)) InterlockedIncrement64(&data.dx12_calls);
    if (FAILED(dx11) && FAILED(dx12)) InterlockedIncrement64(&data.neither_calls);
}

class DeviceDiagnosticMapping {
public:
    DeviceDiagnosticMapping() = default;
    DeviceDiagnosticMapping(const DeviceDiagnosticMapping&) = delete;
    DeviceDiagnosticMapping& operator=(const DeviceDiagnosticMapping&) = delete;

    ~DeviceDiagnosticMapping() {
        if (data_) UnmapViewOfFile(data_);
        if (handle_) CloseHandle(handle_);
    }

    bool Create(DWORD pid) noexcept {
        wchar_t name[64]{};
        DeviceSectionName(pid, name);
        handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     sizeof(DeviceDiagnostics), name);
        if (!handle_ || GetLastError() == ERROR_ALREADY_EXISTS) return false;
        data_ = static_cast<DeviceDiagnostics*>(
            MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DeviceDiagnostics)));
        if (!data_) return false;
        *data_ = {};
        data_->magic = kDeviceDiagnosticMagic;
        data_->size = sizeof(DeviceDiagnostics);
        data_->pid = pid;
        return true;
    }

    DeviceDiagnostics* Data() const noexcept { return data_; }

private:
    HANDLE handle_{};
    DeviceDiagnostics* data_{};
};
}
