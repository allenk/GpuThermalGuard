#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#ifdef GTG_FEATURE_UPDATE
#include "update/hosts_wire.hpp"
#endif

// What crosses the network between paired GTGs, byte by byte.
//
// State, and nothing that asks for anything: there is no request for data, no
// setting, no command and no acknowledgement. A paired host that is
// compromised can lie about its own numbers and nothing more.
//
// Every datagram starts with the same 12 bytes:
//
//   magic "GH" | version | type | sender id (8)
//
// State and goodbye are sealed (AES-256-GCM): the 44-byte header below is the
// associated data, so none of it can be altered, and the body is ciphertext
// followed by the tag. Pair request and accept are in the clear, with a MAC
// under a key only the code's holders have.
//
// Every type has one exact size, checked before anything else is read. All
// integers are little-endian, written and read byte by byte: no struct is ever
// copied to or from the wire.
#ifdef GTG_FEATURE_UPDATE
//
// With the updater built in, two more sealed types carry what it says: its
// status, and the one request a host can make -- that another update to the
// release its own source names. The link seals and opens their bodies without
// reading them; a GTG without them drops both types as not its own.
#endif
namespace gtg::peer {

inline constexpr std::uint8_t kProtocolVersion = 1;
inline constexpr std::size_t kMaxDatagramBytes = 512;
inline constexpr std::size_t kIdBytes = 8;
inline constexpr std::size_t kPublicKeyBytes = 64;   // P-256, X then Y
inline constexpr std::size_t kMacBytes = 32;         // HMAC-SHA256
inline constexpr std::size_t kSaltBytes = 16;
inline constexpr std::size_t kTagBytes = 16;
inline constexpr std::size_t kCodeIdBytes = 4;
// The sender's name, UTF-8. 48 bytes is 48 Latin letters or 16 CJK ones.
inline constexpr std::size_t kNameBytes = 48;

#ifdef GTG_FEATURE_UPDATE
enum class Type : std::uint8_t {
    PairRequest = 1,
    PairAccept = 2,
    State = 3,
    Goodbye = 4,
    UpdateStatus = 5,
    UpdateTrigger = 6,
};
#else
enum class Type : std::uint8_t { PairRequest = 1, PairAccept = 2, State = 3, Goodbye = 4 };
#endif

inline constexpr std::size_t kPrefixBytes = 2 + 1 + 1 + kIdBytes;
inline constexpr std::size_t kSealedHeaderBytes = kPrefixBytes + kSaltBytes + 8 + 8;
inline constexpr std::size_t kPairRequestBytes =
    kPrefixBytes + kCodeIdBytes + kPublicKeyBytes + kNameBytes + kMacBytes;
inline constexpr std::size_t kPairAcceptBytes =
    kPrefixBytes + 4 + kIdBytes + kPublicKeyBytes + kNameBytes + kMacBytes;

// The records a host sends, in wire order. Their own list, not the
// dashboard's enum, so the dashboard can change without changing the wire.
enum class Record : std::uint8_t { Temperature, Power, Vram, Gpu, Cpu, Ram, Fps, Network };
inline constexpr std::size_t kRecords = 8;

// What the sender's protection reads.
enum class Protection : std::uint8_t {
    Normal,
    Warning,     // near the trigger temperature
    SafePower,   // protected: held at the safe power limit
    Fault,       // a write that did not verify, or a lost GPU
    Unknown,     // no protection running: no GPU, no service
};
inline constexpr std::uint8_t kProtectionCount = 5;

// One record as the sender's own Tiny bar draws it: the reading's height on
// the record's axis, the threshold's, and whether the sender's own rules raise
// it. The receiver draws these as they are and recomputes nothing. A missing
// reading is nothing, never zero.
struct RecordState {
    std::optional<float> level;   // 0 to 1
    std::optional<float> tick;    // 0 to 1
    bool raised{};
    bool alarm{};                 // FPS far from steady (drawn white)
    std::optional<float> value;   // the reading in its unit: C, W, %, fps, bytes/s
    std::optional<float> value2;  // Network only: sent bytes/s (value is received)

    friend bool operator==(const RecordState&, const RecordState&) = default;
};

struct HostState {
    std::wstring name;
    std::array<std::uint8_t, 3> version{};   // the sender's GTG version
    Protection protection{Protection::Unknown};
    bool stale{};                            // the sender's telemetry is old
    std::array<RecordState, kRecords> records{};

    friend bool operator==(const HostState&, const HostState&) = default;
};

// Level and tick on the wire, in ten-thousandths.
inline constexpr std::uint16_t kLevelScale = 10'000;
inline constexpr std::size_t kRecordBytes = 16;
inline constexpr std::size_t kStateBodyBytes = kNameBytes + 4 + 4 + kRecords * kRecordBytes;
inline constexpr std::size_t kStateBytes = kSealedHeaderBytes + kStateBodyBytes + kTagBytes;
inline constexpr std::size_t kGoodbyeBytes = kSealedHeaderBytes + kTagBytes;
// Every GTG since Hosts checks these exact sizes and drops anything else as not
// its own, so they never change: a new kind of datagram is a new type.
static_assert(kStateBytes == 244 && kGoodbyeBytes == 60);
#ifdef GTG_FEATURE_UPDATE
inline constexpr std::size_t kUpdateStatusBytes =
    kSealedHeaderBytes + update::hosts::kStatusBodyBytes + kTagBytes;
inline constexpr std::size_t kUpdateTriggerBytes =
    kSealedHeaderBytes + update::hosts::kTriggerBodyBytes + kTagBytes;
// OpenSealed opens every body into a buffer the size of a state's.
static_assert(update::hosts::kStatusBodyBytes <= kStateBodyBytes &&
              update::hosts::kTriggerBodyBytes <= kStateBodyBytes);
#endif

namespace record_flags {
inline constexpr std::uint8_t kLevel = 1u << 0;
inline constexpr std::uint8_t kTick = 1u << 1;
inline constexpr std::uint8_t kRaised = 1u << 2;
inline constexpr std::uint8_t kAlarm = 1u << 3;
inline constexpr std::uint8_t kValue = 1u << 4;
inline constexpr std::uint8_t kValue2 = 1u << 5;
inline constexpr std::uint8_t kKnown = 0x3F;
}  // namespace record_flags

inline constexpr std::uint8_t kStateFlagStale = 1u << 0;

// The exact size of a datagram of `type`, or 0 for a type that does not exist.
[[nodiscard]] constexpr std::size_t DatagramBytes(const Type type) noexcept {
    switch (type) {
        case Type::PairRequest:
            return kPairRequestBytes;
        case Type::PairAccept:
            return kPairAcceptBytes;
        case Type::State:
            return kStateBytes;
        case Type::Goodbye:
            return kGoodbyeBytes;
#ifdef GTG_FEATURE_UPDATE
        case Type::UpdateStatus:
            return kUpdateStatusBytes;
        case Type::UpdateTrigger:
            return kUpdateTriggerBytes;
#endif
    }
    return 0;
}

static_assert(kStateBytes <= kMaxDatagramBytes && kPairAcceptBytes <= kMaxDatagramBytes &&
              kPairRequestBytes <= kMaxDatagramBytes);

struct Prefix {
    Type type{};
    std::uint64_t sender{};
};

struct SealedHeader {
    Type type{};
    std::uint64_t sender{};
    std::array<std::uint8_t, kSaltBytes> salt{};
    std::uint64_t sequence{};
    std::int64_t sent_ms{};   // the sender's clock, Unix milliseconds
};

namespace wire {

class Writer {
public:
    explicit Writer(std::span<std::uint8_t> out) noexcept : out_(out) {}

    void U8(const std::uint8_t value) noexcept { Put(&value, 1); }

    void U16(const std::uint16_t value) noexcept { Le(value, 2); }

    void U64(const std::uint64_t value) noexcept { Le(value, 8); }

    void F32(const float value) noexcept { Le(std::bit_cast<std::uint32_t>(value), 4); }

    void Bytes(const std::span<const std::uint8_t> bytes) noexcept {
        Put(bytes.data(), bytes.size());
    }

    void Zeros(const std::size_t count) noexcept {
        for (std::size_t i = 0; i < count; ++i)
            U8(0);
    }

    // False if anything did not fit; the output is then not to be used.
    [[nodiscard]] bool Ok() const noexcept { return ok_; }

    [[nodiscard]] std::size_t Size() const noexcept { return at_; }

private:
    void Le(const std::uint64_t value, const std::size_t bytes) noexcept {
        for (std::size_t i = 0; i < bytes; ++i) {
            U8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
        }
    }

    void Put(const std::uint8_t* data, const std::size_t size) noexcept {
        if (!ok_ || size > out_.size() - at_) {
            ok_ = false;
            return;
        }
        if (size != 0) std::memcpy(out_.data() + at_, data, size);
        at_ += size;
    }

    std::span<std::uint8_t> out_;
    std::size_t at_{};
    bool ok_{true};
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> in) noexcept : in_(in) {}

    std::uint8_t U8() noexcept {
        if (at_ >= in_.size()) {
            ok_ = false;
            return 0;
        }
        return in_[at_++];
    }

    std::uint16_t U16() noexcept { return static_cast<std::uint16_t>(Le(2)); }

    std::uint64_t U64() noexcept { return Le(8); }

    float F32() noexcept { return std::bit_cast<float>(static_cast<std::uint32_t>(Le(4))); }

    template <std::size_t N>
    std::array<std::uint8_t, N> Bytes() noexcept {
        std::array<std::uint8_t, N> bytes{};
        for (std::size_t i = 0; i < N; ++i)
            bytes[i] = U8();
        return bytes;
    }

    // Reserved bytes must be zero; anything else is a datagram this version
    // does not understand.
    void Zeros(const std::size_t count) noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            if (U8() != 0) ok_ = false;
        }
    }

    [[nodiscard]] bool Ok() const noexcept { return ok_; }

    [[nodiscard]] bool AtEnd() const noexcept { return at_ == in_.size(); }

private:
    std::uint64_t Le(const std::size_t bytes) noexcept {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            value |= static_cast<std::uint64_t>(U8()) << (8 * i);
        }
        return value;
    }

    std::span<const std::uint8_t> in_;
    std::size_t at_{};
    bool ok_{true};
};

// Strict UTF-8 to UTF-16: overlong forms, surrogates and anything past
// U+10FFFF are refused, not repaired.
[[nodiscard]] inline std::optional<std::wstring> DecodeUtf8(const std::string_view bytes) {
    std::wstring out;
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto lead = static_cast<std::uint8_t>(bytes[i]);
        std::uint32_t code = 0;
        std::size_t extra = 0;
        if (lead < 0x80) {
            code = lead;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            code = lead & 0x1Fu;
            extra = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            code = lead & 0x0Fu;
            extra = 2;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            code = lead & 0x07u;
            extra = 3;
        } else {
            return std::nullopt;
        }
        if (extra > bytes.size() - i - 1) return std::nullopt;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto next = static_cast<std::uint8_t>(bytes[i + k]);
            if ((next & 0xC0u) != 0x80u) return std::nullopt;
            code = (code << 6) | (next & 0x3Fu);
        }
        const bool overlong = (extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000);
        if (overlong || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return std::nullopt;
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(code));
        }
        i += extra + 1;
    }
    return out;
}

// UTF-16 to UTF-8, cut at a character boundary so it fits `limit` bytes.
// An unpaired surrogate becomes U+FFFD.
[[nodiscard]] inline std::string EncodeUtf8(const std::wstring_view text, const std::size_t limit) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t code = static_cast<std::uint16_t>(text[i]);
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.size() &&
            static_cast<std::uint16_t>(text[i + 1]) >= 0xDC00 &&
            static_cast<std::uint16_t>(text[i + 1]) <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) +
                   (static_cast<std::uint16_t>(text[i + 1]) - 0xDC00u);
            ++i;
        } else if (code >= 0xD800 && code <= 0xDFFF) {
            code = 0xFFFD;
        }
        char bytes[4]{};
        std::size_t size = 0;
        if (code < 0x80) {
            bytes[size++] = static_cast<char>(code);
        } else if (code < 0x800) {
            bytes[size++] = static_cast<char>(0xC0 | (code >> 6));
            bytes[size++] = static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            bytes[size++] = static_cast<char>(0xE0 | (code >> 12));
            bytes[size++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            bytes[size++] = static_cast<char>(0x80 | (code & 0x3F));
        } else {
            bytes[size++] = static_cast<char>(0xF0 | (code >> 18));
            bytes[size++] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            bytes[size++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            bytes[size++] = static_cast<char>(0x80 | (code & 0x3F));
        }
        if (out.size() + size > limit) break;
        out.append(bytes, size);
    }
    return out;
}

// Characters a name must not carry: controls, and the direction overrides
// that would let a name display as something other than what it is.
[[nodiscard]] constexpr bool Unprintable(const wchar_t c) noexcept {
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2066 && c <= 0x2069) || c == 0x200E || c == 0x200F || c == 0xFEFF;
}

}  // namespace wire

// A name as it may be shown: unprintable characters dropped, spaces at either
// end trimmed. Used for names typed here and names received alike.
[[nodiscard]] inline std::wstring CleanName(const std::wstring_view name) {
    std::wstring out;
    for (const wchar_t c : name) {
        if (!wire::Unprintable(c)) out.push_back(c);
    }
    const auto first = out.find_first_not_of(L' ');
    if (first == std::wstring::npos) return {};
    const auto last = out.find_last_not_of(L' ');
    return out.substr(first, last - first + 1);
}

// The name field: UTF-8, zero padded, cut at a character boundary.
[[nodiscard]] inline std::array<std::uint8_t, kNameBytes> NameField(const std::wstring_view name) {
    const std::string utf8 = wire::EncodeUtf8(CleanName(name), kNameBytes);
    std::array<std::uint8_t, kNameBytes> field{};
    std::memcpy(field.data(), utf8.data(), utf8.size());
    return field;
}

// The name in a received field. Nothing if the field is not UTF-8 or carries
// anything after its padding begins.
[[nodiscard]] inline std::optional<std::wstring> ReadNameField(
    const std::array<std::uint8_t, kNameBytes>& field) {
    std::size_t length = 0;
    while (length < kNameBytes && field[length] != 0)
        ++length;
    for (std::size_t i = length; i < kNameBytes; ++i) {
        if (field[i] != 0) return std::nullopt;
    }
    const auto decoded =
        wire::DecodeUtf8(std::string_view(reinterpret_cast<const char*>(field.data()), length));
    if (!decoded) return std::nullopt;
    return CleanName(*decoded);
}

// The prefix every datagram starts with. Nothing if the datagram is not ours,
// is of another version, or is not exactly its type's size.
[[nodiscard]] inline std::optional<Prefix> ReadPrefix(
    const std::span<const std::uint8_t> datagram) {
    if (datagram.size() < kPrefixBytes) return std::nullopt;
    wire::Reader reader(datagram);
    if (reader.U8() != 'G' || reader.U8() != 'H') return std::nullopt;
    if (reader.U8() != kProtocolVersion) return std::nullopt;
    const auto type = static_cast<Type>(reader.U8());
    if (DatagramBytes(type) == 0 || datagram.size() != DatagramBytes(type)) return std::nullopt;
    Prefix prefix;
    prefix.type = type;
    prefix.sender = reader.U64();
    return prefix;
}

inline void WritePrefix(wire::Writer& writer, const Type type,
                        const std::uint64_t sender) noexcept {
    writer.U8('G');
    writer.U8('H');
    writer.U8(kProtocolVersion);
    writer.U8(static_cast<std::uint8_t>(type));
    writer.U64(sender);
}

[[nodiscard]] inline std::array<std::uint8_t, kSealedHeaderBytes> WriteSealedHeader(
    const SealedHeader& header) noexcept {
    std::array<std::uint8_t, kSealedHeaderBytes> bytes{};
    wire::Writer writer(bytes);
    WritePrefix(writer, header.type, header.sender);
    writer.Bytes(header.salt);
    writer.U64(header.sequence);
    writer.U64(static_cast<std::uint64_t>(header.sent_ms));
    return bytes;
}

// The header of a sealed datagram whose prefix has already been read.
[[nodiscard]] inline std::optional<SealedHeader> ReadSealedHeader(
    const std::span<const std::uint8_t> datagram) {
    const auto prefix = ReadPrefix(datagram);
#ifdef GTG_FEATURE_UPDATE
    if (!prefix || (prefix->type != Type::State && prefix->type != Type::Goodbye &&
                    prefix->type != Type::UpdateStatus && prefix->type != Type::UpdateTrigger)) {
#else
    if (!prefix || (prefix->type != Type::State && prefix->type != Type::Goodbye)) {
#endif
        return std::nullopt;
    }
    wire::Reader reader(datagram.subspan(kPrefixBytes));
    SealedHeader header;
    header.type = prefix->type;
    header.sender = prefix->sender;
    header.salt = reader.Bytes<kSaltBytes>();
    header.sequence = reader.U64();
    header.sent_ms = static_cast<std::int64_t>(reader.U64());
    if (!reader.Ok()) return std::nullopt;
    return header;
}

// The joiner's half of pairing. The MAC, under a key derived from the code's
// secret, covers every byte before it.
struct PairRequest {
    std::uint64_t joiner{};
    std::array<std::uint8_t, kCodeIdBytes> code_id{};
    std::array<std::uint8_t, kPublicKeyBytes> public_key{};
    std::array<std::uint8_t, kNameBytes> name{};
    std::array<std::uint8_t, kMacBytes> mac{};
};

enum class AcceptStatus : std::uint8_t {
    Accepted,
    Used,      // the code was already spent by someone else
    Expired,
};
inline constexpr std::uint8_t kAcceptStatusCount = 3;

// The issuer's answer. Refused answers carry no key and no name, and are
// MACed like an acceptance, so only a holder of the code can say it is spent.
struct PairAccept {
    std::uint64_t issuer{};
    AcceptStatus status{};
    std::uint64_t joiner{};
    std::array<std::uint8_t, kPublicKeyBytes> public_key{};
    std::array<std::uint8_t, kNameBytes> name{};
    std::array<std::uint8_t, kMacBytes> mac{};
};

[[nodiscard]] inline std::array<std::uint8_t, kPairRequestBytes> WritePairRequest(
    const PairRequest& request) noexcept {
    std::array<std::uint8_t, kPairRequestBytes> bytes{};
    wire::Writer writer(bytes);
    WritePrefix(writer, Type::PairRequest, request.joiner);
    writer.Bytes(request.code_id);
    writer.Bytes(request.public_key);
    writer.Bytes(request.name);
    writer.Bytes(request.mac);
    return bytes;
}

[[nodiscard]] inline std::optional<PairRequest> ReadPairRequest(
    const std::span<const std::uint8_t> datagram) {
    const auto prefix = ReadPrefix(datagram);
    if (!prefix || prefix->type != Type::PairRequest) return std::nullopt;
    wire::Reader reader(datagram.subspan(kPrefixBytes));
    PairRequest request;
    request.joiner = prefix->sender;
    request.code_id = reader.Bytes<kCodeIdBytes>();
    request.public_key = reader.Bytes<kPublicKeyBytes>();
    request.name = reader.Bytes<kNameBytes>();
    request.mac = reader.Bytes<kMacBytes>();
    if (!reader.Ok() || !reader.AtEnd()) return std::nullopt;
    return request;
}

[[nodiscard]] inline std::array<std::uint8_t, kPairAcceptBytes> WritePairAccept(
    const PairAccept& accept) noexcept {
    std::array<std::uint8_t, kPairAcceptBytes> bytes{};
    wire::Writer writer(bytes);
    WritePrefix(writer, Type::PairAccept, accept.issuer);
    writer.U8(static_cast<std::uint8_t>(accept.status));
    writer.Zeros(3);
    writer.U64(accept.joiner);
    writer.Bytes(accept.public_key);
    writer.Bytes(accept.name);
    writer.Bytes(accept.mac);
    return bytes;
}

[[nodiscard]] inline std::optional<PairAccept> ReadPairAccept(
    const std::span<const std::uint8_t> datagram) {
    const auto prefix = ReadPrefix(datagram);
    if (!prefix || prefix->type != Type::PairAccept) return std::nullopt;
    wire::Reader reader(datagram.subspan(kPrefixBytes));
    PairAccept accept;
    accept.issuer = prefix->sender;
    const std::uint8_t status = reader.U8();
    if (status >= kAcceptStatusCount) return std::nullopt;
    accept.status = static_cast<AcceptStatus>(status);
    reader.Zeros(3);
    accept.joiner = reader.U64();
    accept.public_key = reader.Bytes<kPublicKeyBytes>();
    accept.name = reader.Bytes<kNameBytes>();
    accept.mac = reader.Bytes<kMacBytes>();
    if (!reader.Ok() || !reader.AtEnd()) return std::nullopt;
    return accept;
}

// What a pairing MAC covers: the datagram up to its MAC.
[[nodiscard]] inline std::span<const std::uint8_t> MacInput(
    const std::span<const std::uint8_t> datagram) noexcept {
    return datagram.first(datagram.size() - kMacBytes);
}

namespace state_detail {

// The range a record's reading may take. Anything outside is not a reading a
// GTG sends, so the whole datagram is refused rather than the value clamped.
struct Range {
    float low;
    float high;
};

[[nodiscard]] constexpr Range ValueRange(const Record record) noexcept {
    switch (record) {
        case Record::Temperature:
            return {-60.0F, 200.0F};
        case Record::Power:
            return {0.0F, 5'000.0F};
        case Record::Vram:
        case Record::Gpu:
        case Record::Cpu:
        case Record::Ram:
            return {0.0F, 100.0F};
        case Record::Fps:
            return {0.0F, 10'000.0F};
        case Record::Network:
            return {0.0F, 1.0e12F};
    }
    return {0.0F, 0.0F};
}

[[nodiscard]] inline bool InRange(const float value, const Range range) noexcept {
    return std::isfinite(value) && value >= range.low && value <= range.high;
}

[[nodiscard]] inline std::uint16_t Fraction(const float value) noexcept {
    if (!(value > 0.0F)) return 0;
    if (value >= 1.0F) return kLevelScale;
    return static_cast<std::uint16_t>(std::lround(value * kLevelScale));
}

}  // namespace state_detail

// The state body, before sealing. The sender's values are clamped to what the
// receiver accepts, so a GTG never sends a datagram another GTG refuses.
[[nodiscard]] inline std::array<std::uint8_t, kStateBodyBytes> WriteStateBody(
    const HostState& state) {
    std::array<std::uint8_t, kStateBodyBytes> bytes{};
    wire::Writer writer(bytes);
    writer.Bytes(NameField(state.name));
    writer.U8(state.version[0]);
    writer.U8(state.version[1]);
    writer.U8(state.version[2]);
    writer.U8(0);
    const auto protection = static_cast<std::uint8_t>(state.protection);
    writer.U8(protection < kProtectionCount ? protection
                                            : static_cast<std::uint8_t>(Protection::Unknown));
    writer.U8(state.stale ? kStateFlagStale : 0);
    writer.U8(static_cast<std::uint8_t>(kRecords));
    writer.U8(0);
    for (std::size_t i = 0; i < kRecords; ++i) {
        const RecordState& record = state.records[i];
        const auto range = state_detail::ValueRange(static_cast<Record>(i));
        const auto clamp = [&](const std::optional<float> value) -> std::optional<float> {
            if (!value || !std::isfinite(*value)) return std::nullopt;
            return *value < range.low ? range.low : (*value > range.high ? range.high : *value);
        };
        const auto value = clamp(record.value);
        const auto value2 =
            static_cast<Record>(i) == Record::Network ? clamp(record.value2) : std::nullopt;
        const bool level = record.level && std::isfinite(*record.level);
        const bool tick = record.tick && std::isfinite(*record.tick);
        std::uint8_t flags = 0;
        if (level) flags |= record_flags::kLevel;
        if (tick) flags |= record_flags::kTick;
        if (record.raised) flags |= record_flags::kRaised;
        if (record.alarm) flags |= record_flags::kAlarm;
        if (value) flags |= record_flags::kValue;
        if (value2) flags |= record_flags::kValue2;
        writer.U8(flags);
        writer.U8(0);
        writer.U16(level ? state_detail::Fraction(*record.level) : 0);
        writer.U16(tick ? state_detail::Fraction(*record.tick) : 0);
        writer.U16(0);
        writer.F32(value.value_or(0.0F));
        writer.F32(value2.value_or(0.0F));
    }
    return bytes;
}

// A decrypted state body, every field checked. Nothing if any field is out of
// its range: one bad field means the sender is not a GTG speaking this
// version, and nothing it says is shown.
[[nodiscard]] inline std::optional<HostState> ReadStateBody(
    const std::span<const std::uint8_t> body) {
    if (body.size() != kStateBodyBytes) return std::nullopt;
    wire::Reader reader(body);
    HostState state;
    const auto name = ReadNameField(reader.Bytes<kNameBytes>());
    if (!name) return std::nullopt;
    state.name = *name;
    state.version = {reader.U8(), reader.U8(), reader.U8()};
    reader.Zeros(1);
    const std::uint8_t protection = reader.U8();
    if (protection >= kProtectionCount) return std::nullopt;
    state.protection = static_cast<Protection>(protection);
    const std::uint8_t flags = reader.U8();
    if ((flags & ~kStateFlagStale) != 0) return std::nullopt;
    state.stale = (flags & kStateFlagStale) != 0;
    if (reader.U8() != kRecords) return std::nullopt;
    reader.Zeros(1);
    for (std::size_t i = 0; i < kRecords; ++i) {
        const auto record = static_cast<Record>(i);
        const std::uint8_t bits = reader.U8();
        reader.Zeros(1);
        const std::uint16_t level = reader.U16();
        const std::uint16_t tick = reader.U16();
        reader.Zeros(2);
        const float value = reader.F32();
        const float value2 = reader.F32();
        if ((bits & ~record_flags::kKnown) != 0) return std::nullopt;
        if (level > kLevelScale || tick > kLevelScale) return std::nullopt;
        if ((bits & record_flags::kLevel) == 0 && level != 0) return std::nullopt;
        if ((bits & record_flags::kTick) == 0 && tick != 0) return std::nullopt;
        if ((bits & record_flags::kAlarm) != 0 && record != Record::Fps) return std::nullopt;
        if ((bits & record_flags::kValue2) != 0 && record != Record::Network) return std::nullopt;
        const auto range = state_detail::ValueRange(record);
        if ((bits & record_flags::kValue) != 0 ? !state_detail::InRange(value, range)
                                               : std::bit_cast<std::uint32_t>(value) != 0) {
            return std::nullopt;
        }
        if ((bits & record_flags::kValue2) != 0 ? !state_detail::InRange(value2, range)
                                                : std::bit_cast<std::uint32_t>(value2) != 0) {
            return std::nullopt;
        }
        RecordState& out = state.records[i];
        if ((bits & record_flags::kLevel) != 0) out.level = static_cast<float>(level) / kLevelScale;
        if ((bits & record_flags::kTick) != 0) out.tick = static_cast<float>(tick) / kLevelScale;
        out.raised = (bits & record_flags::kRaised) != 0;
        out.alarm = (bits & record_flags::kAlarm) != 0;
        if ((bits & record_flags::kValue) != 0) out.value = value;
        if ((bits & record_flags::kValue2) != 0) out.value2 = value2;
    }
    if (!reader.Ok() || !reader.AtEnd()) return std::nullopt;
    return state;
}

}  // namespace gtg::peer
