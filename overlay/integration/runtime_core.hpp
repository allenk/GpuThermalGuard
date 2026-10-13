// The D3D12 runtime a target actually loaded. A game may ship its own Agility
// SDK Core beside its executable and keep the system loader, and then a method
// address taken from the system runtime is an offset into a different binary.
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include "control.hpp"
#include "runtime_export.hpp"
#include "runtime_image_identity.hpp"
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace gtg::overlay::integration {
// An image larger than this is not a runtime we recognise, and the bound keeps
// a malformed module entry from sizing a read.
inline constexpr DWORD kMaxRuntimeImageSize = 64U * 1024U * 1024U;

// A module snapshot of a process that is loading reports ERROR_BAD_LENGTH; the
// documented answer is to ask again rather than to conclude anything.
inline constexpr unsigned kRuntimeSnapshotAttempts = 20;
inline constexpr DWORD kRuntimeSnapshotRetryMs = 10;
inline constexpr DWORD kMaxSignerOrganization = 256;

// Discovery refuses by throwing: the reason is the evidence, and the helper
// publishes the sentence it carries.
inline void RequireRuntime(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

class RuntimeHandle {
public:
    explicit RuntimeHandle(HANDLE value) : value_(value) {}

    ~RuntimeHandle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }

    RuntimeHandle(const RuntimeHandle&) = delete;
    RuntimeHandle& operator=(const RuntimeHandle&) = delete;

    HANDLE Value() const { return value_; }

private:
    HANDLE value_{};
};

// Trust verification is resolved at run time from System32, so an absent API
// refuses the app-local route instead of failing the build or the whole run.
class RuntimeModule {
public:
    explicit RuntimeModule(const wchar_t* name)
        : value_(LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        RequireRuntime(value_ != nullptr, "runtime verification system module unavailable");
    }

    ~RuntimeModule() { FreeLibrary(value_); }

    RuntimeModule(const RuntimeModule&) = delete;
    RuntimeModule& operator=(const RuntimeModule&) = delete;

    template <typename Fn>
    Fn Function(const char* name) const {
        const auto function = GetProcAddress(value_, name);
        RequireRuntime(function != nullptr, "runtime verification API unavailable");
        return reinterpret_cast<Fn>(function);
    }

private:
    HMODULE value_{};
};

// Admission for a binary this process is about to load: the game already runs
// that Core, and loading it here is new exposure. Offline verification with
// cache-only retrieval, then the signing organisation. This is an admission
// check and not a revocation-fresh one, and it is not what makes the resulting
// address trustworthy — the remote image and prologue checks do that.
inline bool MicrosoftRuntimeSignature(const std::filesystem::path& path, HANDLE file) {
    RuntimeModule trust_module(L"wintrust.dll");
    RuntimeModule crypto_module(L"crypt32.dll");
    const auto verify = trust_module.Function<decltype(&WinVerifyTrust)>("WinVerifyTrust");
    const auto provider = trust_module.Function<decltype(&WTHelperProvDataFromStateData)>(
        "WTHelperProvDataFromStateData");
    const auto signer = trust_module.Function<decltype(&WTHelperGetProvSignerFromChain)>(
        "WTHelperGetProvSignerFromChain");
    const auto name = crypto_module.Function<decltype(&CertGetNameStringW)>("CertGetNameStringW");
    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path.c_str();
    file_info.hFile = file;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file_info;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = verify(nullptr, &action, &data);
    bool trusted{};
    if (status == ERROR_SUCCESS) {
        auto* information = provider(data.hWVTStateData);
        auto* signing = information ? signer(information, 0, FALSE, 0) : nullptr;
        if (signing && signing->csCertChain && signing->pasCertChain &&
            signing->pasCertChain[0].pCert) {
            wchar_t organization[kMaxSignerOrganization]{};
            auto* attribute = const_cast<void*>(static_cast<const void*>(szOID_ORGANIZATION_NAME));
            const auto length = name(signing->pasCertChain[0].pCert, CERT_NAME_ATTR_TYPE, 0,
                                     attribute, organization, kMaxSignerOrganization);
            trusted = length > 1 && length <= kMaxSignerOrganization &&
                      _wcsicmp(organization, L"Microsoft Corporation") == 0;
        }
    }
    // The verify state is closed on every path, including a refusal.
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)verify(nullptr, &action, &data);
    return trusted;
}

// One target's Core, identified rather than assumed. The file handle is held
// for the life of this object so the image cannot be replaced between the read
// that identified it and the attachment that depends on it.
class CoreRuntime {
public:
    CoreRuntime(const MODULEENTRY32W& module, HANDLE process)
        : path_(module.szExePath),
          file_(CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr)) {
        RequireRuntime(file_.Value() != INVALID_HANDLE_VALUE, "retain target Core file identity");
        RequireRuntime(module.modBaseSize >= sizeof(IMAGE_NT_HEADERS64) &&
                           module.modBaseSize <= kMaxRuntimeImageSize,
                       "bounded target Core image");
        wchar_t system_directory[MAX_PATH]{};
        const auto length = GetSystemDirectoryW(system_directory, MAX_PATH);
        RequireRuntime(length && length < MAX_PATH, "system runtime directory");
        system_ =
            SameRuntimeFile(path_, std::filesystem::path(system_directory) / L"D3D12Core.dll");
        // The system runtime is what this helper loads by default, so the
        // existing route needs neither the signature check nor a remote read.
        if (system_) return;
        RequireRuntime(MicrosoftRuntimeSignature(path_, file_.Value()),
                       "app-local Core signature is not a trusted Microsoft runtime");
        const auto sdk_version = CoreSdkVersion(
            module.modBaseSize, [&](std::size_t offset, void* output, std::size_t bytes) {
                SIZE_T read{};
                return ReadProcessMemory(process, module.modBaseAddr + offset, output, bytes,
                                         &read) &&
                       read == bytes;
            });
        RequireRuntime(sdk_version.has_value(), "target Core SDK export invalid or absent");
        sdk_version_ = *sdk_version;
    }

    CoreRuntime(const CoreRuntime&) = delete;
    CoreRuntime& operator=(const CoreRuntime&) = delete;

    const std::filesystem::path& Path() const { return path_; }

    UINT SdkVersion() const { return sdk_version_; }

    bool IsSystem() const { return system_; }

    // Nothing is returned only when the target holds no Core of its own, which
    // is the system route every other title has always taken. Everything else
    // that cannot be established is a refusal naming what failed, so a caller
    // that forgets the identity cannot silently inherit the old behaviour.
    static std::unique_ptr<CoreRuntime> Open(Identity target) {
        RequireRuntime(target.pid && target.created, "target runtime identity required");
        RuntimeHandle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE,
                                          FALSE, target.pid));
        RequireRuntime(process.Value() && CreationTime(process.Value()) == target.created &&
                           WaitForSingleObject(process.Value(), 0) == WAIT_TIMEOUT,
                       "target lifetime changed before runtime discovery");
        HANDLE snapshot_value = INVALID_HANDLE_VALUE;
        for (unsigned attempt = 0; attempt < kRuntimeSnapshotAttempts; ++attempt) {
            snapshot_value = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, target.pid);
            if (snapshot_value != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
            Sleep(kRuntimeSnapshotRetryMs);
        }
        RuntimeHandle snapshot(snapshot_value);
        RequireRuntime(snapshot.Value() != INVALID_HANDLE_VALUE, "target runtime module snapshot");
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        RequireRuntime(Module32FirstW(snapshot.Value(), &module), "target runtime first module");
        MODULEENTRY32W selected{};
        bool found{};
        do {
            if (_wcsicmp(module.szModule, L"D3D12Core.dll") != 0) continue;
            RequireRuntime(!found, "ambiguous target Core runtime");
            selected = module;
            found = true;
        } while (Module32NextW(snapshot.Value(), &module));
        // A truncated enumeration could hide a second candidate, which is the
        // one thing the ambiguity refusal above exists to catch.
        RequireRuntime(GetLastError() == ERROR_NO_MORE_FILES, "incomplete runtime module list");
        if (!found) return nullptr;
        auto result = std::make_unique<CoreRuntime>(selected, process.Value());
        RequireRuntime(CreationTime(process.Value()) == target.created &&
                           WaitForSingleObject(process.Value(), 0) == WAIT_TIMEOUT,
                       "target exited during runtime discovery");
        return result;
    }

private:
    std::filesystem::path path_;
    RuntimeHandle file_;
    UINT sdk_version_{};
    bool system_{};
};
}  // namespace gtg::overlay::integration
