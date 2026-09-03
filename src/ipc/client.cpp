#include "ipc/client.hpp"

#include <windows.h>

namespace gtg::ipc {

bool Send(const Command command, Response& response) noexcept {
    if (!WaitNamedPipeW(kPipeName, 600)) return false;
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
        CloseHandle(pipe);
        return false;
    }
    const Request request{.command = command};
    DWORD written = 0;
    DWORD read = 0;
    const bool success = WriteFile(pipe, &request, sizeof(request), &written, nullptr) != FALSE &&
        written == sizeof(request) &&
        ReadFile(pipe, &response, sizeof(response), &read, nullptr) != FALSE &&
        read == sizeof(response) && IsValid(response);
    CloseHandle(pipe);
    return success;
}

}  // namespace gtg::ipc
