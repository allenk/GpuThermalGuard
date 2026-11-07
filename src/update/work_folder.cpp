#include "update/work_folder.hpp"

#include <aclapi.h>
#include <sddl.h>

#include <cstdint>
#include <vector>

namespace gtg::update {
namespace {

class Token {
public:
    Token() = default;

    ~Token() {
        if (handle_ != nullptr) CloseHandle(handle_);
    }

    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;

    HANDLE* Out() noexcept { return &handle_; }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }

private:
    HANDLE handle_{};
};

// The token a program the user starts normally runs with, without administrator
// rights: the linked, filtered token when this process is elevated, otherwise
// this process's own with Administrators made deny-only. Under UAC that group
// is deny-only already; where UAC is off, every program would otherwise count
// as one without administrator rights that can write System32.
bool UserToken(Token& user) noexcept {
    Token process;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, process.Out())) {
        return false;
    }
    TOKEN_ELEVATION_TYPE type{};
    DWORD bytes = 0;
    if (GetTokenInformation(process.Get(), TokenElevationType, &type, sizeof(type), &bytes) &&
        type == TokenElevationTypeFull) {
        TOKEN_LINKED_TOKEN linked{};
        if (!GetTokenInformation(process.Get(), TokenLinkedToken, &linked, sizeof(linked),
                                 &bytes)) {
            return false;
        }
        *user.Out() = linked.LinkedToken;
        return true;
    }
    std::uint8_t administrators[SECURITY_MAX_SID_SIZE]{};
    DWORD size = sizeof(administrators);
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &size)) {
        return false;
    }
    SID_AND_ATTRIBUTES disable{administrators, 0};
    Token restricted;
    if (!CreateRestrictedToken(process.Get(), 0, 1, &disable, 0, nullptr, 0, nullptr,
                               restricted.Out())) {
        return false;
    }
    return DuplicateToken(restricted.Get(), SecurityIdentification, user.Out()) != FALSE;
}

bool Grants(const HANDLE token, PSECURITY_DESCRIPTOR descriptor, const DWORD access) noexcept {
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE,
                            FILE_ALL_ACCESS};
    PRIVILEGE_SET privileges{};
    DWORD privileges_size = sizeof(privileges);
    DWORD granted = 0;
    BOOL status = FALSE;
    return AccessCheck(descriptor, token, access, &mapping, &privileges, &privileges_size, &granted,
                       &status) &&
           status;
}

}  // namespace

bool SecureFolder(const std::filesystem::path& folder, const wchar_t* sddl, DWORD& error) noexcept {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor,
                                                              nullptr)) {
        error = GetLastError();
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    bool ok = CreateDirectoryW(folder.c_str(), &attributes) != FALSE;
    if (!ok && GetLastError() == ERROR_ALREADY_EXISTS) {
        // A folder left from an earlier update, or one somebody else made:
        // either way it gets this access list, protected from its parent.
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL dacl = nullptr;
        if (GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present) {
            const DWORD status = SetNamedSecurityInfoW(
                const_cast<wchar_t*>(folder.c_str()), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                dacl, nullptr);
            ok = status == ERROR_SUCCESS;
            SetLastError(status);
        }
    }
    if (!ok) error = GetLastError();
    LocalFree(descriptor);
    return ok;
}

InstallFolder InspectInstallFolder(const std::filesystem::path& folder) noexcept {
    InstallFolder result;
    wchar_t root[MAX_PATH + 1]{};
    const std::wstring& text = folder.native();
    result.remote = text.starts_with(L"\\\\");
    if (!result.remote && GetVolumePathNameW(text.c_str(), root, MAX_PATH + 1)) {
        result.remote = GetDriveTypeW(root) == DRIVE_REMOTE;
        DWORD flags = 0;
        if (GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0)) {
            result.acls = (flags & FILE_PERSISTENT_ACLS) != 0;
        }
    }
    DWORD needed = 0;
    constexpr SECURITY_INFORMATION kWhat =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    (void)GetFileSecurityW(text.c_str(), kWhat, nullptr, 0, &needed);
    if (needed == 0) return result;
    std::vector<std::uint8_t> buffer(needed);
    auto* descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(buffer.data());
    Token user;
    if (GetFileSecurityW(text.c_str(), kWhat, descriptor, needed, &needed) && UserToken(user)) {
        // Adding a file plants one; deleting a child lets a folder of GTG's be
        // renamed away and replaced.
        result.user_writable = Grants(user.Get(), descriptor, FILE_ADD_FILE) ||
                               Grants(user.Get(), descriptor, FILE_DELETE_CHILD);
    }
    return result;
}

}  // namespace gtg::update
