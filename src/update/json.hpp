#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A strict JSON reader for the update manifest and the files the update script
// writes back. Strict means RFC 8259 and nothing more: no comments, no trailing
// commas, no byte order mark, no duplicate keys, no data after the value, well-
// formed UTF-8 only. Everything it holds is bounded by Limits, because the
// manifest comes from a share somebody else can write to.
namespace gtg::update::json {

struct Limits {
    std::size_t max_bytes{64 * 1024};
    // Every value counts, scalars included. The manifest's deepest is six:
    // root, variants, a variant, files, a file, one of its fields.
    int max_depth{6};
    std::size_t max_string_bytes{1024};
    std::size_t max_values{4096};
};

struct Value {
    enum class Kind {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind{Kind::Null};
    bool boolean{};
    // Set for a number written as a plain integer (no fraction, no exponent)
    // that fits; any other number is a Number without one.
    std::optional<std::int64_t> integer;
    // A string's contents, unescaped, in UTF-8.
    std::string text;
    // An array's elements, or an object's values in the order written.
    std::vector<Value> items;
    // An object's keys, parallel to items.
    std::vector<std::string> keys;

    [[nodiscard]] bool IsObject() const noexcept { return kind == Kind::Object; }

    [[nodiscard]] bool IsArray() const noexcept { return kind == Kind::Array; }

    [[nodiscard]] bool IsString() const noexcept { return kind == Kind::String; }

    // An object's member, or nothing.
    [[nodiscard]] const Value* Find(std::string_view key) const noexcept;
};

struct ParseResult {
    std::optional<Value> value;
    // Where and why it was refused, for the journal.
    std::string error;
};

[[nodiscard]] ParseResult Parse(std::string_view text, const Limits& limits = {});

// The text as a JSON string literal, quotes included. UTF-8 in and out;
// control characters, quotes and backslashes are escaped.
[[nodiscard]] std::string Quote(std::string_view utf8);

}  // namespace gtg::update::json
