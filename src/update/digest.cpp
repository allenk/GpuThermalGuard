#include "update/digest.hpp"

#include <windows.h>
#include <bcrypt.h>

namespace gtg::update {
namespace {

using Open = decltype(&BCryptOpenAlgorithmProvider);
using Hash = decltype(&BCryptHash);
using Close = decltype(&BCryptCloseAlgorithmProvider);

int HexValue(const char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string ToHex(const std::span<const std::uint8_t> bytes) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::optional<Digest> ParseDigest(const std::string_view hex) noexcept {
    Digest digest{};
    if (hex.size() != digest.size() * 2) return std::nullopt;
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const int high = HexValue(hex[2 * i]);
        const int low = HexValue(hex[2 * i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        digest[i] = static_cast<std::uint8_t>(high << 4 | low);
    }
    return digest;
}

std::optional<Digest> ParseDigest(const std::wstring_view hex) noexcept {
    std::string ascii(hex.size(), '\0');
    for (std::size_t i = 0; i < hex.size(); ++i) {
        // Anything past ASCII is not a hex digit, and '?' is not one either.
        ascii[i] = hex[i] < 0x80 ? static_cast<char>(hex[i]) : '?';
    }
    return ParseDigest(std::string_view(ascii));
}

Sha256::Sha256() noexcept {
    const HMODULE module = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) return;
    module_ = module;
    const auto open = reinterpret_cast<Open>(GetProcAddress(module, "BCryptOpenAlgorithmProvider"));
    const auto hash = reinterpret_cast<Hash>(GetProcAddress(module, "BCryptHash"));
    const auto close =
        reinterpret_cast<Close>(GetProcAddress(module, "BCryptCloseAlgorithmProvider"));
    if (open == nullptr || hash == nullptr || close == nullptr) return;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(open(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return;
    algorithm_ = algorithm;
    hash_ = reinterpret_cast<void*>(hash);
    close_ = reinterpret_cast<void*>(close);
}

Sha256::~Sha256() {
    if (algorithm_ != nullptr) {
        reinterpret_cast<Close>(close_)(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
    }
    if (module_ != nullptr) FreeLibrary(static_cast<HMODULE>(module_));
}

std::optional<Digest> Sha256::Of(const std::span<const std::uint8_t> bytes) const noexcept {
    if (algorithm_ == nullptr || bytes.size() > ULONG_MAX) return std::nullopt;
    // CNG takes its input through a non-const pointer and does not write it.
    static UCHAR empty = 0;
    auto* input = bytes.empty() ? &empty : const_cast<PUCHAR>(bytes.data());
    Digest digest{};
    const NTSTATUS status = reinterpret_cast<Hash>(hash_)(
        static_cast<BCRYPT_ALG_HANDLE>(algorithm_), nullptr, 0, input,
        static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
    if (!BCRYPT_SUCCESS(status)) return std::nullopt;
    return digest;
}

}  // namespace gtg::update
