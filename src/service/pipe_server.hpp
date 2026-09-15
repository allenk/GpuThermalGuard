#pragma once

#include <windows.h>
#include <sddl.h>
#include <optional>
#include "ipc/protocol.hpp"

namespace gtg::service {

// Internal transport, polled by ServiceMain. Never flushes or waits for a client.
class PipeServer final {
public:
    explicit PipeServer(const wchar_t* name = ipc::kPipeName,
                        const wchar_t* security = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)") {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                security, SDDL_REVISION_1,
                &descriptor, nullptr)) return;  // Fail closed, never default ACL.
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        pipe_ = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, sizeof(ipc::Response), sizeof(ipc::Request), 0, &attributes);
        LocalFree(descriptor);
    }
    ~PipeServer() { if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_); }
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;

    [[nodiscard]] std::optional<ipc::Request> Poll() {
        if (pipe_ == INVALID_HANDLE_VALUE) return std::nullopt;
        if (!connected_) {
            connected_ = ConnectNamedPipe(pipe_, nullptr) != FALSE ||
                GetLastError() == ERROR_PIPE_CONNECTED;
            if (!connected_) return std::nullopt;
            connected_at_ = GetTickCount64();
        }
        // Preserve unread reply bytes until client closes, or enforce a bounded
        // lease. Use PIPE_NOWAIT ReadFile rather than synchronous PeekNamedPipe:
        // the latter's wait-mode-independent behavior has multithread caveats.
        if (GetTickCount64() - connected_at_ >= 2'000) {
            ResetConnection();
            return std::nullopt;
        }
        ipc::Request request{};
        DWORD read = 0;
        if (ReadFile(pipe_, &request, sizeof(request), &read, nullptr)) {
            if (!replied_ && read == sizeof(request) && ipc::IsValid(request)) return request;
        } else if (GetLastError() == ERROR_NO_DATA) {
            return std::nullopt;
        }
        ResetConnection();  // Reject malformed/oversized requests.
        return std::nullopt;
    }

    void Reply(const ipc::Response& response) {
        if (!connected_ || replied_) return;
        DWORD written = 0;
        if (!WriteFile(pipe_, &response, sizeof(response), &written, nullptr) ||
            written != sizeof(response)) {
            ResetConnection();
            return;
        }
        replied_ = true;
        // Do not disconnect here: that would discard an unread response.
    }

private:
    void ResetConnection() {
        if (connected_) DisconnectNamedPipe(pipe_);
        connected_ = false;
        replied_ = false;
    }
    HANDLE pipe_{INVALID_HANDLE_VALUE};
    bool connected_{false};
    bool replied_{false};
    ULONGLONG connected_at_{};
};

}  // namespace gtg::service
