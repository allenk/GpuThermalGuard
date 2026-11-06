#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The pairing code: what one GTG shows and another pastes to pair with it.
//
// It carries where to find the issuer and a single-use secret, and nothing
// else. It is moved between machines by Ctrl+C / Ctrl+V -- through the cloud
// clipboard, Remote Desktop or a chat window -- so it will sit in a history
// somewhere; it is worth nothing there because it is spent at the first
// pairing and expires after ten minutes, and the pair key is agreed by ECDH,
// not taken from it.
//
// 25 bytes as 40 Crockford base32 symbols in groups of five behind "GTG-":
//
//   GTG-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX
//
// Every character is in the QR alphanumeric set, so the QR code of it is the
// small kind (29 x 29 modules). Reading is forgiving, because people paste and
// retype: case, spaces, line breaks and dashes are ignored, full-width forms
// from an East Asian input method are read as their ASCII, and O reads as 0, I
// and L as 1. A CRC-16 catches a mistyped or swapped character before anything
// is sent.
namespace gtg::peer {

inline constexpr std::size_t kSecretBytes = 16;
inline constexpr std::size_t kCodeBytes = 25;
inline constexpr std::size_t kCodeSymbols = kCodeBytes * 8 / 5;
inline constexpr std::string_view kCodePrefix = "GTG";
// High nibble: the code's format; low nibble: the address family. Only IPv4
// in the first version; 6 is kept for IPv6.
inline constexpr std::uint8_t kCodeFormat = 1;
inline constexpr std::uint8_t kCodeFamilyIpv4 = 4;

struct PairingCode {
    std::array<std::uint8_t, 4> address{};   // IPv4, in network order
    std::uint16_t port{};
    std::array<std::uint8_t, kSecretBytes> secret{};

    friend bool operator==(const PairingCode&, const PairingCode&) = default;
};

enum class CodeError {
    None,
    Empty,
    Character,   // something that is not part of a code
    Length,      // incomplete, or more than one code
    Checksum,    // a character mistyped or two swapped
    Format,      // a later GTG's code, or not a GTG code
    Address,     // an address no host can have
};

struct ParsedCode {
    std::optional<PairingCode> code;
    CodeError error{CodeError::None};
};

namespace code_detail {

inline constexpr std::string_view kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

// CRC-16/CCITT-FALSE. A burst of up to 16 bits is always caught, so any one
// symbol (5 bits) mistyped and any two neighbours swapped (10 bits) are.
[[nodiscard]] constexpr std::uint16_t Crc16(const std::uint8_t* data,
                                            const std::size_t size) noexcept {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 0; i < size; ++i) {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(data[i]) << 8));
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) != 0 ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                                      : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

// The symbol's value, or -1. Lower case, O, I and L are read as Crockford
// says to; U is not a symbol.
[[nodiscard]] constexpr int SymbolValue(char32_t c) noexcept {
    if (c >= U'a' && c <= U'z') c = c - U'a' + U'A';
    if (c == U'O') c = U'0';
    if (c == U'I' || c == U'L') c = U'1';
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        if (static_cast<char32_t>(kAlphabet[i]) == c) return static_cast<int>(i);
    }
    return -1;
}

[[nodiscard]] constexpr bool Ignored(const char32_t c) noexcept {
    return c == U' ' || c == U'\t' || c == U'\r' || c == U'\n' || c == U'-' ||
           c == U'\u00A0' ||                     // no-break space
           (c >= U'\u2010' && c <= U'\u2015') ||   // dashes a chat window substitutes
           c == U'\u3000';                       // ideographic space
}

// Full-width ASCII, as an East Asian input method types it, read as ASCII.
[[nodiscard]] constexpr char32_t Narrow(const char32_t c) noexcept {
    return c >= U'\uFF01' && c <= U'\uFF5E' ? c - 0xFEE0 : c;
}

// The text for 23 bytes of content, the checksum appended.
[[nodiscard]] inline std::string Encode(std::array<std::uint8_t, kCodeBytes> bytes) {
    const std::uint16_t crc = Crc16(bytes.data(), kCodeBytes - 2);
    bytes[kCodeBytes - 2] = static_cast<std::uint8_t>(crc >> 8);
    bytes[kCodeBytes - 1] = static_cast<std::uint8_t>(crc & 0xFF);

    std::string text(kCodePrefix);
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t symbols = 0;
    for (const std::uint8_t byte : bytes) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            if (symbols % 5 == 0) text.push_back('-');
            bits -= 5;
            text.push_back(kAlphabet[(buffer >> bits) & 0x1F]);
            ++symbols;
        }
    }
    return text;
}

}  // namespace code_detail

[[nodiscard]] inline std::string FormatCode(const PairingCode& code) {
    std::array<std::uint8_t, kCodeBytes> bytes{};
    bytes[0] = static_cast<std::uint8_t>((kCodeFormat << 4) | kCodeFamilyIpv4);
    for (std::size_t i = 0; i < 4; ++i)
        bytes[1 + i] = code.address[i];
    bytes[5] = static_cast<std::uint8_t>(code.port >> 8);
    bytes[6] = static_cast<std::uint8_t>(code.port & 0xFF);
    for (std::size_t i = 0; i < kSecretBytes; ++i)
        bytes[7 + i] = code.secret[i];
    return code_detail::Encode(bytes);
}

// `text` as pasted or typed, in any case, with or without the prefix.
template <typename Char>
[[nodiscard]] ParsedCode ParseCode(const std::basic_string_view<Char> text) {
    std::string symbols;
    bool any = false;
    for (const Char raw : text) {
        const char32_t c = code_detail::Narrow(static_cast<char32_t>(raw));
        if (code_detail::Ignored(c)) continue;
        any = true;
        // The prefix's letters are symbols too, so the prefix is taken off by
        // length below.
        const int value = code_detail::SymbolValue(c);
        if (value < 0) return {std::nullopt, CodeError::Character};
        symbols.push_back(code_detail::kAlphabet[static_cast<std::size_t>(value)]);
    }
    if (!any) return {std::nullopt, CodeError::Empty};
    if (symbols.size() == kCodePrefix.size() + kCodeSymbols &&
        std::string_view(symbols).substr(0, kCodePrefix.size()) == kCodePrefix) {
        symbols.erase(0, kCodePrefix.size());
    }
    if (symbols.size() != kCodeSymbols) return {std::nullopt, CodeError::Length};

    std::array<std::uint8_t, kCodeBytes> bytes{};
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t out = 0;
    for (const char symbol : symbols) {
        buffer = (buffer << 5) | static_cast<std::uint32_t>(code_detail::SymbolValue(symbol));
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            bytes[out++] = static_cast<std::uint8_t>((buffer >> bits) & 0xFF);
        }
    }

    const std::uint16_t crc = code_detail::Crc16(bytes.data(), kCodeBytes - 2);
    if (bytes[kCodeBytes - 2] != static_cast<std::uint8_t>(crc >> 8) ||
        bytes[kCodeBytes - 1] != static_cast<std::uint8_t>(crc & 0xFF)) {
        return {std::nullopt, CodeError::Checksum};
    }
    if (bytes[0] != ((kCodeFormat << 4) | kCodeFamilyIpv4))
        return {std::nullopt, CodeError::Format};

    PairingCode code;
    for (std::size_t i = 0; i < 4; ++i)
        code.address[i] = bytes[1 + i];
    code.port = static_cast<std::uint16_t>((bytes[5] << 8) | bytes[6]);
    for (std::size_t i = 0; i < kSecretBytes; ++i)
        code.secret[i] = bytes[7 + i];

    // Unspecified, loopback is allowed (two GTGs on one machine, and the tests),
    // multicast, reserved and broadcast are not hosts; nor is port 0.
    const std::uint8_t first = code.address[0];
    const bool broadcast = code.address == std::array<std::uint8_t, 4>{255, 255, 255, 255};
    if (first == 0 || first >= 224 || broadcast || code.port == 0) {
        return {std::nullopt, CodeError::Address};
    }
    return {code, CodeError::None};
}

[[nodiscard]] inline ParsedCode ParseCode(const std::wstring_view text) {
    return ParseCode<wchar_t>(text);
}

[[nodiscard]] inline ParsedCode ParseCode(const std::string_view text) {
    return ParseCode<char>(text);
}

}  // namespace gtg::peer
