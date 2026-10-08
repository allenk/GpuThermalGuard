#pragma once
#include "../ipc/osd_section.hpp"

namespace gtg::overlay::integration {
inline bool AcceptRequest(const ipc::Request& request, std::uint32_t serial, std::uint32_t begun,
                          std::uint32_t pid, std::uint64_t now_us) noexcept {
    return serial && request.serial == serial && begun == serial &&
           ipc::RequestPlausible(request) && request.writer_pid == pid && request.heartbeat_us &&
           now_us >= request.heartbeat_us && now_us - request.heartbeat_us <= 10'000'000;
}
}  // namespace gtg::overlay::integration
