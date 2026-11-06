#include "update/json.hpp"

#include <charconv>
#include <format>
#include <utility>

namespace gtg::update::json {
namespace {

class Parser {
public:
    Parser(const std::string_view text, const Limits& limits) : text_(text), limits_(limits) {}

    ParseResult Run() {
        ParseResult result;
        if (text_.size() > limits_.max_bytes) {
            result.error =
                std::format("{} bytes, over the {} byte limit", text_.size(), limits_.max_bytes);
            return result;
        }
        if (text_.starts_with("\xEF\xBB\xBF")) {
            result.error = "offset 0: byte order mark";
            return result;
        }
        Value value;
        if (!ParseValue(value, 1)) {
            result.error = std::format("offset {}: {}", at_, error_);
            return result;
        }
        SkipSpace();
        if (at_ != text_.size()) {
            result.error = std::format("offset {}: data after the value", at_);
            return result;
        }
        result.value = std::move(value);
        return result;
    }

private:
    bool Fail(const char* why) {
        error_ = why;
        return false;
    }

    [[nodiscard]] bool Peek(const char c) const noexcept {
        return at_ < text_.size() && text_[at_] == c;
    }

    void SkipSpace() noexcept {
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
            ++at_;
        }
    }

    bool ParseValue(Value& out, const int depth) {
        if (depth > limits_.max_depth) return Fail("nested too deeply");
        if (++values_ > limits_.max_values) return Fail("too many values");
        SkipSpace();
        if (at_ >= text_.size()) return Fail("unexpected end");
        switch (text_[at_]) {
            case '{':
                return ParseObject(out, depth);
            case '[':
                return ParseArray(out, depth);
            case '"':
                out.kind = Value::Kind::String;
                return ParseString(out.text);
            case 't':
                return Literal("true", out, Value::Kind::Bool, true);
            case 'f':
                return Literal("false", out, Value::Kind::Bool, false);
            case 'n':
                return Literal("null", out, Value::Kind::Null, false);
            default:
                return ParseNumber(out);
        }
    }

    bool Literal(const std::string_view word, Value& out, const Value::Kind kind,
                 const bool boolean) {
        if (text_.substr(at_, word.size()) != word) return Fail("unknown literal");
        at_ += word.size();
        out.kind = kind;
        out.boolean = boolean;
        return true;
    }

    bool ParseObject(Value& out, const int depth) {
        out.kind = Value::Kind::Object;
        ++at_;
        SkipSpace();
        if (Peek('}')) {
            ++at_;
            return true;
        }
        for (;;) {
            SkipSpace();
            if (!Peek('"')) return Fail("expected a key");
            std::string key;
            if (!ParseString(key)) return false;
            for (const std::string& existing : out.keys) {
                if (existing == key) return Fail("duplicate key");
            }
            SkipSpace();
            if (!Peek(':')) return Fail("expected ':'");
            ++at_;
            Value member;
            if (!ParseValue(member, depth + 1)) return false;
            out.keys.push_back(std::move(key));
            out.items.push_back(std::move(member));
            SkipSpace();
            if (Peek(',')) {
                ++at_;
                continue;
            }
            if (Peek('}')) {
                ++at_;
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(Value& out, const int depth) {
        out.kind = Value::Kind::Array;
        ++at_;
        SkipSpace();
        if (Peek(']')) {
            ++at_;
            return true;
        }
        for (;;) {
            Value element;
            if (!ParseValue(element, depth + 1)) return false;
            out.items.push_back(std::move(element));
            SkipSpace();
            if (Peek(',')) {
                ++at_;
                continue;
            }
            if (Peek(']')) {
                ++at_;
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseString(std::string& out) {
        ++at_;
        for (;;) {
            if (at_ >= text_.size()) return Fail("unterminated string");
            const auto c = static_cast<unsigned char>(text_[at_]);
            if (c == '"') {
                ++at_;
                return true;
            }
            if (c < 0x20) return Fail("control character in a string");
            if (c == '\\') {
                if (!ParseEscape(out)) return false;
            } else if (c < 0x80) {
                out.push_back(static_cast<char>(c));
                ++at_;
            } else if (!CopyUtf8(out)) {
                return false;
            }
            if (out.size() > limits_.max_string_bytes) return Fail("string too long");
        }
    }

    bool Hex4(std::uint32_t& unit) {
        if (text_.size() - at_ < 4) return Fail("short escape");
        unit = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            unit <<= 4;
            if (c >= '0' && c <= '9') {
                unit |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                unit |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                unit |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return Fail("bad hex in an escape");
            }
        }
        return true;
    }

    bool ParseEscape(std::string& out) {
        ++at_;
        if (at_ >= text_.size()) return Fail("unterminated escape");
        const char c = text_[at_++];
        switch (c) {
            case '"':
                out.push_back('"');
                return true;
            case '\\':
                out.push_back('\\');
                return true;
            case '/':
                out.push_back('/');
                return true;
            case 'b':
                out.push_back('\b');
                return true;
            case 'f':
                out.push_back('\f');
                return true;
            case 'n':
                out.push_back('\n');
                return true;
            case 'r':
                out.push_back('\r');
                return true;
            case 't':
                out.push_back('\t');
                return true;
            case 'u':
                return ParseCodePoint(out);
            default:
                return Fail("unknown escape");
        }
    }

    bool ParseCodePoint(std::string& out) {
        std::uint32_t unit = 0;
        if (!Hex4(unit)) return false;
        std::uint32_t code = unit;
        if (unit >= 0xDC00 && unit <= 0xDFFF) return Fail("lone surrogate");
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            // A high surrogate must be followed by a second escape holding the low one.
            if (text_.size() - at_ < 2 || text_[at_] != '\\' || text_[at_ + 1] != 'u') {
                return Fail("lone surrogate");
            }
            at_ += 2;
            std::uint32_t low = 0;
            if (!Hex4(low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return Fail("lone surrogate");
            code = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
        }
        if (code == 0) return Fail("NUL in a string");
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | code >> 6));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | code >> 12));
            out.push_back(static_cast<char>(0x80 | (code >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | code >> 18));
            out.push_back(static_cast<char>(0x80 | (code >> 12 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        return true;
    }

    // One UTF-8 sequence, checked as the Unicode standard defines well-formed:
    // no overlong forms, no surrogates, nothing past U+10FFFF.
    bool CopyUtf8(std::string& out) {
        const auto lead = static_cast<unsigned char>(text_[at_]);
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED) high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F;
        } else {
            return Fail("malformed UTF-8");
        }
        if (text_.size() - at_ < length) return Fail("malformed UTF-8");
        for (std::size_t i = 1; i < length; ++i) {
            const auto next = static_cast<unsigned char>(text_[at_ + i]);
            const unsigned char floor = i == 1 ? low : 0x80;
            const unsigned char ceiling = i == 1 ? high : 0xBF;
            if (next < floor || next > ceiling) return Fail("malformed UTF-8");
        }
        out.append(text_.substr(at_, length));
        at_ += length;
        return true;
    }

    static bool IsDigit(const char c) noexcept { return c >= '0' && c <= '9'; }

    bool Digits() {
        const std::size_t begin = at_;
        while (at_ < text_.size() && IsDigit(text_[at_]))
            ++at_;
        return at_ > begin;
    }

    bool ParseNumber(Value& out) {
        const std::size_t begin = at_;
        if (Peek('-')) ++at_;
        if (Peek('0')) {
            ++at_;
        } else if (at_ < text_.size() && text_[at_] >= '1' && text_[at_] <= '9') {
            (void)Digits();
        } else {
            return Fail("unexpected character");
        }
        bool integral = true;
        if (Peek('.')) {
            ++at_;
            if (!Digits()) return Fail("digits expected after '.'");
            integral = false;
        }
        if (Peek('e') || Peek('E')) {
            ++at_;
            if (Peek('+') || Peek('-')) ++at_;
            if (!Digits()) return Fail("digits expected in an exponent");
            integral = false;
        }
        out.kind = Value::Kind::Number;
        if (integral) {
            std::int64_t number = 0;
            const char* first = text_.data() + begin;
            const char* last = text_.data() + at_;
            const auto [end, error] = std::from_chars(first, last, number);
            if (error == std::errc{} && end == last) out.integer = number;
        }
        return true;
    }

    std::string_view text_;
    const Limits& limits_;
    std::size_t at_{};
    std::size_t values_{};
    const char* error_{""};
};

}  // namespace

const Value* Value::Find(const std::string_view key) const noexcept {
    if (kind != Kind::Object) return nullptr;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) return &items[i];
    }
    return nullptr;
}

ParseResult Parse(const std::string_view text, const Limits& limits) {
    return Parser(text, limits).Run();
}

std::string Quote(const std::string_view utf8) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size() + 2);
    out.push_back('"');
    for (const char c : utf8) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (byte < 0x20) {
                    out.push_back('\\');
                    out.push_back('u');
                    out += "00";
                    out.push_back(kDigits[byte >> 4]);
                    out.push_back(kDigits[byte & 0x0F]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace gtg::update::json
