#include "peer/crypto.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <dpapi.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace gtg::peer {
namespace {

// Bound into every protected key, so DPAPI alone does not unprotect it for
// another program of the same user.
constexpr char kEntropy[] = "GpuThermalGuard Hosts pair key v1";

template <typename T>
T Resolve(const HMODULE module, const char* name) noexcept {
    return module == nullptr ? nullptr : reinterpret_cast<T>(GetProcAddress(module, name));
}

PUCHAR Writable(const Bytes bytes) noexcept {
    // CNG takes inputs through non-const pointers and does not write them.
    static UCHAR empty = 0;
    return bytes.empty() ? &empty : const_cast<PUCHAR>(bytes.data());
}

ULONG Size(const Bytes bytes) noexcept {
    return static_cast<ULONG>(bytes.size());
}

}  // namespace

struct Crypto::Api {
    decltype(&BCryptOpenAlgorithmProvider) open{};
    decltype(&BCryptCloseAlgorithmProvider) close{};
    decltype(&BCryptSetProperty) set_property{};
    decltype(&BCryptGenRandom) random{};
    decltype(&BCryptCreateHash) create_hash{};
    decltype(&BCryptHashData) hash_data{};
    decltype(&BCryptFinishHash) finish_hash{};
    decltype(&BCryptDestroyHash) destroy_hash{};
    decltype(&BCryptGenerateSymmetricKey) symmetric_key{};
    decltype(&BCryptEncrypt) encrypt{};
    decltype(&BCryptDecrypt) decrypt{};
    decltype(&BCryptDestroyKey) destroy_key{};
    decltype(&BCryptGenerateKeyPair) generate_pair{};
    decltype(&BCryptFinalizeKeyPair) finalize_pair{};
    decltype(&BCryptExportKey) export_key{};
    decltype(&BCryptImportKeyPair) import_pair{};
    decltype(&BCryptSecretAgreement) agree{};
    decltype(&BCryptDeriveKey) derive{};
    decltype(&BCryptDestroySecret) destroy_secret{};
    decltype(&CryptProtectData) protect{};
    decltype(&CryptUnprotectData) unprotect{};
};

EphemeralKey::~EphemeralKey() {
    if (owner_ != nullptr) owner_->DestroyKey(handle_);
}

EphemeralKey::EphemeralKey(EphemeralKey&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)),
      public_(other.public_) {}

EphemeralKey& EphemeralKey::operator=(EphemeralKey&& other) noexcept {
    if (this != &other) {
        if (owner_ != nullptr) owner_->DestroyKey(handle_);
        owner_ = std::exchange(other.owner_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
        public_ = other.public_;
    }
    return *this;
}

Crypto::~Crypto() {
    if (api_ != nullptr) {
        if (aes_ != nullptr) api_->close(static_cast<BCRYPT_ALG_HANDLE>(aes_), 0);
        if (hmac_ != nullptr) api_->close(static_cast<BCRYPT_ALG_HANDLE>(hmac_), 0);
        if (ecdh_ != nullptr) api_->close(static_cast<BCRYPT_ALG_HANDLE>(ecdh_), 0);
    }
    delete api_;
    if (bcrypt_ != nullptr) FreeLibrary(static_cast<HMODULE>(bcrypt_));
    if (crypt32_ != nullptr) FreeLibrary(static_cast<HMODULE>(crypt32_));
}

bool Crypto::Load() noexcept {
    if (loaded_) return true;
    const HMODULE bcrypt = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const HMODULE crypt32 = LoadLibraryExW(L"crypt32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    bcrypt_ = bcrypt;
    crypt32_ = crypt32;
    if (bcrypt == nullptr || crypt32 == nullptr) return false;

    api_ = new (std::nothrow) Api;
    if (api_ == nullptr) return false;
    Api& api = *api_;
    api.open = Resolve<decltype(api.open)>(bcrypt, "BCryptOpenAlgorithmProvider");
    api.close = Resolve<decltype(api.close)>(bcrypt, "BCryptCloseAlgorithmProvider");
    api.set_property = Resolve<decltype(api.set_property)>(bcrypt, "BCryptSetProperty");
    api.random = Resolve<decltype(api.random)>(bcrypt, "BCryptGenRandom");
    api.create_hash = Resolve<decltype(api.create_hash)>(bcrypt, "BCryptCreateHash");
    api.hash_data = Resolve<decltype(api.hash_data)>(bcrypt, "BCryptHashData");
    api.finish_hash = Resolve<decltype(api.finish_hash)>(bcrypt, "BCryptFinishHash");
    api.destroy_hash = Resolve<decltype(api.destroy_hash)>(bcrypt, "BCryptDestroyHash");
    api.symmetric_key = Resolve<decltype(api.symmetric_key)>(bcrypt, "BCryptGenerateSymmetricKey");
    api.encrypt = Resolve<decltype(api.encrypt)>(bcrypt, "BCryptEncrypt");
    api.decrypt = Resolve<decltype(api.decrypt)>(bcrypt, "BCryptDecrypt");
    api.destroy_key = Resolve<decltype(api.destroy_key)>(bcrypt, "BCryptDestroyKey");
    api.generate_pair = Resolve<decltype(api.generate_pair)>(bcrypt, "BCryptGenerateKeyPair");
    api.finalize_pair = Resolve<decltype(api.finalize_pair)>(bcrypt, "BCryptFinalizeKeyPair");
    api.export_key = Resolve<decltype(api.export_key)>(bcrypt, "BCryptExportKey");
    api.import_pair = Resolve<decltype(api.import_pair)>(bcrypt, "BCryptImportKeyPair");
    api.agree = Resolve<decltype(api.agree)>(bcrypt, "BCryptSecretAgreement");
    api.derive = Resolve<decltype(api.derive)>(bcrypt, "BCryptDeriveKey");
    api.destroy_secret = Resolve<decltype(api.destroy_secret)>(bcrypt, "BCryptDestroySecret");
    api.protect = Resolve<decltype(api.protect)>(crypt32, "CryptProtectData");
    api.unprotect = Resolve<decltype(api.unprotect)>(crypt32, "CryptUnprotectData");
    const bool resolved = api.open && api.close && api.set_property && api.random &&
                          api.create_hash && api.hash_data && api.finish_hash && api.destroy_hash &&
                          api.symmetric_key && api.encrypt && api.decrypt && api.destroy_key &&
                          api.generate_pair && api.finalize_pair && api.export_key &&
                          api.import_pair && api.agree && api.derive && api.destroy_secret &&
                          api.protect && api.unprotect;
    if (!resolved) return false;

    BCRYPT_ALG_HANDLE aes = nullptr;
    BCRYPT_ALG_HANDLE hmac = nullptr;
    BCRYPT_ALG_HANDLE ecdh = nullptr;
    if (BCRYPT_SUCCESS(api.open(&aes, BCRYPT_AES_ALGORITHM, nullptr, 0))) aes_ = aes;
    if (BCRYPT_SUCCESS(
            api.open(&hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
        hmac_ = hmac;
    }
    if (BCRYPT_SUCCESS(api.open(&ecdh, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0))) ecdh_ = ecdh;
    if (aes_ == nullptr || hmac_ == nullptr || ecdh_ == nullptr) return false;
    if (!BCRYPT_SUCCESS(
            api.set_property(aes, BCRYPT_CHAINING_MODE,
                             reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                             sizeof(BCRYPT_CHAIN_MODE_GCM), 0))) {
        return false;
    }
    loaded_ = true;
    return true;
}

bool Crypto::Random(const std::span<std::uint8_t> out) const noexcept {
    if (!loaded_) return false;
    return BCRYPT_SUCCESS(api_->random(nullptr, out.data(), static_cast<ULONG>(out.size()),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::optional<Digest> Crypto::Hmac(const Bytes key,
                                   const std::initializer_list<Bytes> parts) const {
    if (!loaded_) return std::nullopt;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(api_->create_hash(static_cast<BCRYPT_ALG_HANDLE>(hmac_), &hash, nullptr, 0,
                                          Writable(key), Size(key), 0))) {
        return std::nullopt;
    }
    bool ok = true;
    for (const Bytes part : parts) {
        if (part.empty()) continue;
        ok = ok && BCRYPT_SUCCESS(api_->hash_data(hash, Writable(part), Size(part), 0));
    }
    Digest digest{};
    ok = ok && BCRYPT_SUCCESS(api_->finish_hash(hash, digest.data(), Size(digest), 0));
    api_->destroy_hash(hash);
    if (!ok) return std::nullopt;
    return digest;
}

std::optional<Key> Crypto::Hkdf(const Bytes input, const Bytes salt, const Bytes info) const {
    // An absent salt is a hash length of zeros (RFC 5869, 2.2).
    const Digest zeros{};
    const auto prk = Hmac(salt.empty() ? Bytes(zeros) : salt, {input});
    if (!prk) return std::nullopt;
    const std::uint8_t counter = 1;
    return Hmac(*prk, {info, Bytes(&counter, 1)});
}

void Crypto::DestroyKey(void* const handle) const noexcept {
    if (handle != nullptr && api_ != nullptr) {
        api_->destroy_key(static_cast<BCRYPT_KEY_HANDLE>(handle));
    }
}

std::optional<EphemeralKey> Crypto::Finish(void* const handle) const {
    EphemeralKey key;
    key.owner_ = this;
    key.handle_ = handle;
    std::array<UCHAR, sizeof(BCRYPT_ECCKEY_BLOB) + 64> blob{};
    ULONG written = 0;
    if (!BCRYPT_SUCCESS(api_->export_key(static_cast<BCRYPT_KEY_HANDLE>(handle), nullptr,
                                         BCRYPT_ECCPUBLIC_BLOB, blob.data(), Size(blob), &written,
                                         0)) ||
        written != blob.size()) {
        return std::nullopt;
    }
    BCRYPT_ECCKEY_BLOB header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    if (header.dwMagic != BCRYPT_ECDH_PUBLIC_P256_MAGIC || header.cbKey != 32) return std::nullopt;
    std::memcpy(key.public_.data(), blob.data() + sizeof(header), 64);
    return key;
}

std::optional<EphemeralKey> Crypto::GenerateEphemeral() const {
    if (!loaded_) return std::nullopt;
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (!BCRYPT_SUCCESS(
            api_->generate_pair(static_cast<BCRYPT_ALG_HANDLE>(ecdh_), &handle, 256, 0))) {
        return std::nullopt;
    }
    if (!BCRYPT_SUCCESS(api_->finalize_pair(handle, 0))) {
        api_->destroy_key(handle);
        return std::nullopt;
    }
    return Finish(handle);
}

std::optional<EphemeralKey> Crypto::ImportEphemeral(const Bytes private_key, const Bytes x,
                                                    const Bytes y) const {
    if (!loaded_ || private_key.size() != 32 || x.size() != 32 || y.size() != 32) {
        return std::nullopt;
    }
    std::array<UCHAR, sizeof(BCRYPT_ECCKEY_BLOB) + 96> blob{};
    const BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDH_PRIVATE_P256_MAGIC, 32};
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), x.data(), 32);
    std::memcpy(blob.data() + sizeof(header) + 32, y.data(), 32);
    std::memcpy(blob.data() + sizeof(header) + 64, private_key.data(), 32);
    BCRYPT_KEY_HANDLE handle = nullptr;
    const bool ok = BCRYPT_SUCCESS(api_->import_pair(static_cast<BCRYPT_ALG_HANDLE>(ecdh_), nullptr,
                                                     BCRYPT_ECCPRIVATE_BLOB, &handle, blob.data(),
                                                     Size(blob), 0));
    SecureZeroMemory(blob.data(), blob.size());
    if (!ok) return std::nullopt;
    return Finish(handle);
}

std::optional<Key> Crypto::Agree(const EphemeralKey& mine, const Bytes theirs) const {
    if (!loaded_ || !mine.Valid() || theirs.size() != 64) return std::nullopt;
    std::array<UCHAR, sizeof(BCRYPT_ECCKEY_BLOB) + 64> blob{};
    const BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDH_PUBLIC_P256_MAGIC, 32};
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), theirs.data(), 64);
    // The import checks that the point is on the curve.
    BCRYPT_KEY_HANDLE peer = nullptr;
    if (!BCRYPT_SUCCESS(api_->import_pair(static_cast<BCRYPT_ALG_HANDLE>(ecdh_), nullptr,
                                          BCRYPT_ECCPUBLIC_BLOB, &peer, blob.data(), Size(blob),
                                          0))) {
        return std::nullopt;
    }
    BCRYPT_SECRET_HANDLE secret = nullptr;
    const bool agreed =
        BCRYPT_SUCCESS(api_->agree(static_cast<BCRYPT_KEY_HANDLE>(mine.handle_), peer, &secret, 0));
    api_->destroy_key(peer);
    if (!agreed) return std::nullopt;
    Key shared{};
    ULONG written = 0;
    const bool derived = BCRYPT_SUCCESS(api_->derive(secret, BCRYPT_KDF_RAW_SECRET, nullptr,
                                                     shared.data(), Size(shared), &written, 0));
    api_->destroy_secret(secret);
    if (!derived || written != shared.size()) return std::nullopt;
    // CNG gives the raw secret least significant byte first.
    std::reverse(shared.begin(), shared.end());
    return shared;
}

bool Crypto::Seal(const Key& key, const std::array<std::uint8_t, kNonceBytes>& nonce,
                  const Bytes associated, const Bytes plaintext, const std::span<std::uint8_t> out,
                  const std::span<std::uint8_t, 16> tag) const {
    if (!loaded_ || out.size() != plaintext.size()) return false;
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (!BCRYPT_SUCCESS(api_->symmetric_key(static_cast<BCRYPT_ALG_HANDLE>(aes_), &handle, nullptr,
                                            0, Writable(key), Size(key), 0))) {
        return false;
    }
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = Writable(nonce);
    info.cbNonce = Size(nonce);
    info.pbAuthData = Writable(associated);
    info.cbAuthData = Size(associated);
    info.pbTag = tag.data();
    info.cbTag = static_cast<ULONG>(tag.size());
    UCHAR empty = 0;
    ULONG written = 0;
    const bool ok = BCRYPT_SUCCESS(api_->encrypt(
        handle, Writable(plaintext), Size(plaintext), &info, nullptr, 0,
        out.empty() ? &empty : out.data(), static_cast<ULONG>(out.size()), &written, 0));
    api_->destroy_key(handle);
    return ok && written == out.size();
}

bool Crypto::Open(const Key& key, const std::array<std::uint8_t, kNonceBytes>& nonce,
                  const Bytes associated, const Bytes ciphertext,
                  const std::span<const std::uint8_t, 16> tag,
                  const std::span<std::uint8_t> out) const {
    if (!loaded_ || out.size() != ciphertext.size()) return false;
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (!BCRYPT_SUCCESS(api_->symmetric_key(static_cast<BCRYPT_ALG_HANDLE>(aes_), &handle, nullptr,
                                            0, Writable(key), Size(key), 0))) {
        return false;
    }
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = Writable(nonce);
    info.cbNonce = Size(nonce);
    info.pbAuthData = Writable(associated);
    info.cbAuthData = Size(associated);
    info.pbTag = Writable(tag);
    info.cbTag = static_cast<ULONG>(tag.size());
    UCHAR empty = 0;
    ULONG written = 0;
    const bool ok = BCRYPT_SUCCESS(api_->decrypt(
        handle, Writable(ciphertext), Size(ciphertext), &info, nullptr, 0,
        out.empty() ? &empty : out.data(), static_cast<ULONG>(out.size()), &written, 0));
    api_->destroy_key(handle);
    if (!ok || written != out.size()) {
        std::fill(out.begin(), out.end(), std::uint8_t{0});
        return false;
    }
    return true;
}

std::optional<std::vector<std::uint8_t>> Crypto::Protect(const Bytes secret) const {
    if (!loaded_) return std::nullopt;
    DATA_BLOB in{Size(secret), Writable(secret)};
    DATA_BLOB entropy{sizeof(kEntropy) - 1, reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!api_->protect(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> blob(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return blob;
}

std::optional<std::vector<std::uint8_t>> Crypto::Unprotect(const Bytes blob) const {
    if (!loaded_) return std::nullopt;
    DATA_BLOB in{Size(blob), Writable(blob)};
    DATA_BLOB entropy{sizeof(kEntropy) - 1, reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!api_->unprotect(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                         &out)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> secret(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return secret;
}

}  // namespace gtg::peer
