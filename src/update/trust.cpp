#include "update/trust.hpp"

#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <span>

namespace gtg::update {
namespace {

class Module {
public:
    Module() : value_(LoadLibraryExW(L"wintrust.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {}

    ~Module() {
        if (value_ != nullptr) FreeLibrary(value_);
    }

    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    template <typename Fn>
    Fn Function(const char* name) const noexcept {
        return value_ == nullptr ? nullptr : reinterpret_cast<Fn>(GetProcAddress(value_, name));
    }

private:
    HMODULE value_{};
};

}  // namespace

SignerResult VerifySigner(const std::filesystem::path& file, const Digest& pin, const Sha256& sha) {
    SignerResult result;
    const Module trust;
    const auto verify = trust.Function<decltype(&WinVerifyTrust)>("WinVerifyTrust");
    const auto provider =
        trust.Function<decltype(&WTHelperProvDataFromStateData)>("WTHelperProvDataFromStateData");
    const auto signer =
        trust.Function<decltype(&WTHelperGetProvSignerFromChain)>("WTHelperGetProvSignerFromChain");
    if (verify == nullptr || provider == nullptr || signer == nullptr || !sha.Available()) {
        return result;
    }
    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = file.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file_info;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    result.status = verify(nullptr, &action, &data);
    const bool verified = result.status == ERROR_SUCCESS || result.status == CERT_E_UNTRUSTEDROOT;
    if (verified && data.hWVTStateData != nullptr) {
        auto* information = provider(data.hWVTStateData);
        auto* signing = information != nullptr ? signer(information, 0, FALSE, 0) : nullptr;
        if (signing != nullptr && signing->csCertChain > 0 && signing->pasCertChain != nullptr &&
            signing->pasCertChain[0].pCert != nullptr) {
            const CERT_CONTEXT* certificate = signing->pasCertChain[0].pCert;
            result.signer = sha.Of(std::span<const std::uint8_t>(certificate->pbCertEncoded,
                                                                 certificate->cbCertEncoded));
        }
    }
    // The verify state is closed on every path, a refusal included.
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)verify(nullptr, &action, &data);

    if (result.status == TRUST_E_NOSIGNATURE) {
        result.verdict = SignerVerdict::Unsigned;
    } else if (!verified || !result.signer) {
        result.verdict = SignerVerdict::Invalid;
    } else {
        result.verdict = *result.signer == pin ? SignerVerdict::Pinned : SignerVerdict::OtherSigner;
    }
    return result;
}

}  // namespace gtg::update
