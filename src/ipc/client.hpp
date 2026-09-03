#pragma once

#include "ipc/protocol.hpp"

namespace gtg::ipc {

[[nodiscard]] bool Send(Command command, Response& response) noexcept;

}  // namespace gtg::ipc
