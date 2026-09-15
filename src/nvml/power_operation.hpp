#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <nvml.h>

namespace gtg::nvml {

struct PowerStageResult {
    nvmlReturn_t code;
    std::string message;
    std::optional<unsigned int> actual_mw;
    std::int64_t duration_us{};
};

struct PowerOperationResult {
    std::uint64_t operation_id{};
    std::uint64_t requested_mw{};
    std::optional<PowerStageResult> setter;
    std::optional<PowerStageResult> readback;
    std::optional<PowerStageResult> recheck;
    std::string precondition_error;
    bool verified{};
    bool verified_on_recheck{};
    bool retryable{};
};

inline std::wstring FormatPowerOperationResult(
    const PowerOperationResult& result, std::wstring_view context) {
    auto text = [](const std::string& value) {
        // NVML error strings are diagnostic ASCII; bound each field so the
        // complete record fits the deferred logger's 1024-character slot.
        return std::wstring(value.begin(), value.begin() +
            static_cast<std::ptrdiff_t>((std::min)(value.size(), std::size_t{120})));
    };
    auto stage = [&](const std::optional<PowerStageResult>& value) {
        if (!value) return std::wstring(L"not-called");
        return std::format(L"[code={} message=\"{}\" actual_mw={} duration_us={}]",
            static_cast<int>(value->code), text(value->message),
            value->actual_mw ? std::to_wstring(*value->actual_mw) : L"absent",
            value->duration_us);
    };
    return std::format(
        L"POWER_OP id={} context={} requested_mw={} precondition=\"{}\" "
        L"setter={} readback={} recheck={} outcome={} retryable={}",
        result.operation_id, context.substr(0, 48), result.requested_mw,
        text(result.precondition_error), stage(result.setter), stage(result.readback),
        stage(result.recheck), result.verified_on_recheck ? L"verified-on-recheck" :
            result.verified ? L"verified" : L"unverified", result.retryable);
}

// A bounded retry is permissible, not a diagnosis that the error is transient.
// Device-lost/reset/configuration errors require recovery or operator action.
inline bool IsPowerErrorRetryable(nvmlReturn_t code) noexcept {
    switch (code) {
        case NVML_ERROR_TIMEOUT:
        case NVML_ERROR_IRQ_ISSUE:
        case NVML_ERROR_IN_USE:
        case NVML_ERROR_MEMORY:
        case NVML_ERROR_NO_DATA:
        case NVML_ERROR_INSUFFICIENT_RESOURCES:
        case NVML_ERROR_NOT_READY:
        case NVML_ERROR_UNKNOWN:
            return true;
        default:
            return false;
    }
}

template <typename Writer, typename Reader, typename ErrorText, typename Clock>
PowerOperationResult ExecutePowerOperation(unsigned int requested_mw,
    Writer&& writer, Reader&& reader, ErrorText&& error_text, Clock&& now_us) {
    PowerOperationResult result;
    result.requested_mw = requested_mw;
    auto start = now_us();
    auto code = writer();
    auto elapsed = now_us() - start;
    result.setter = PowerStageResult{code, error_text(code), {}, elapsed};
    if (code != NVML_SUCCESS) {
        result.retryable = IsPowerErrorRetryable(code);
        return result;
    }
    auto read = [&]() {
        unsigned int actual = 0;
        const auto read_start = now_us();
        const auto read_code = reader(&actual);
        const auto read_elapsed = now_us() - read_start;
        return PowerStageResult{read_code, error_text(read_code),
            read_code == NVML_SUCCESS ? std::optional<unsigned int>(actual) : std::nullopt,
            read_elapsed};
    };
    auto matches = [&](const PowerStageResult& value) {
        return value.code == NVML_SUCCESS && value.actual_mw == requested_mw;
    };
    result.readback = read();
    result.verified = matches(*result.readback);
    if (result.verified) return result;
    if (result.readback->code != NVML_SUCCESS &&
        !IsPowerErrorRetryable(result.readback->code)) return result;

    // Exactly one additional read. No delay, spin, or repeated normal write.
    // Keep the first result intact: later success does not prove false failure.
    result.recheck = read();
    result.verified = matches(*result.recheck);
    result.verified_on_recheck = result.verified;
    result.retryable = !result.verified &&
        (result.recheck->code == NVML_SUCCESS ||
         IsPowerErrorRetryable(result.recheck->code));
    return result;
}

}  // namespace gtg::nvml
