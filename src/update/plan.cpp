#include "update/plan.hpp"

#include <format>

#include "update/json.hpp"

namespace gtg::update {

std::uint64_t ProcessCreated(const HANDLE process) noexcept {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return static_cast<std::uint64_t>(created.dwHighDateTime) << 32 | created.dwLowDateTime;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length,
                        nullptr, nullptr);
    return out;
}

std::string FormatPlan(const Plan& plan) {
    std::string files;
    for (const PlanFile& file : plan.files) {
        if (!files.empty()) files.push_back(',');
        files += std::format("{{\"name\":{},\"staged\":{},\"size\":{},\"sha256\":\"{}\"}}",
                             json::Quote(file.name), json::Quote(Utf8(file.staged.wstring())),
                             file.size, ToHex(file.sha256));
    }
    std::string wait;
    for (const PlanProcess& process : plan.wait) {
        if (!wait.empty()) wait.push_back(',');
        // The creation time as text: a 64-bit number is past what a JSON
        // reader is obliged to keep exact.
        wait += std::format("{{\"pid\":{},\"created\":\"{}\"}}", process.pid, process.created);
    }
    return std::format(
        "{{\"format\":1,\"install\":{},\"entry\":{},"
        "\"old\":{{\"version\":{},\"build\":{}}},\"new\":{{\"version\":{},\"build\":{}}},"
        "\"release\":{},\"reinstall\":{},\"files\":[{}],\"wait\":[{}],\"mutex\":{},"
        "\"wait_exit_ms\":{},\"ready_ms\":{}}}\n",
        json::Quote(Utf8(plan.install.wstring())), json::Quote(plan.entry),
        json::Quote(plan.old_version), json::Quote(plan.old_build), json::Quote(plan.new_version),
        json::Quote(plan.new_build), json::Quote(plan.release), plan.reinstall ? "true" : "false",
        files, wait, json::Quote(Utf8(plan.mutex)), plan.wait_exit_ms, plan.ready_ms);
}

}  // namespace gtg::update
