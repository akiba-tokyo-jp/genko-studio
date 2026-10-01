#include "core/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "core/error.hpp"
#include "core/pynum.hpp"

namespace genko::core {

namespace {

constexpr char kHex[] = "0123456789abcdef";

// --- UTF-8 ------------------------------------------------------------------------------------------------

// One well-formed UTF-8 sequence at s[i] (Unicode table 3-7): its length and code point, or 0 when it is not
// well formed; `valid` is then how many bytes of it were (the lead and the good continuation bytes).
struct Utf8Step {
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    std::size_t valid = 0;
    bool truncated = false;  // the bytes ran out in the middle of a sequence
};

Utf8Step utf8_step(std::string_view s, std::size_t i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char lead = byte(i);
    Utf8Step step;
    if (lead < 0x80) {
        step.length = 1;
        step.code_point = lead;
        return step;
    }
    std::size_t need = 0;
    unsigned char lo = 0x80, hi = 0xBF;
    std::uint32_t cp = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        need = 1;
        cp = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        need = 2;
        cp = lead & 0x0Fu;
        if (lead == 0xE0) lo = 0xA0;
        if (lead == 0xED) hi = 0x9F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        need = 3;
        cp = lead & 0x07u;
        if (lead == 0xF0) lo = 0x90;
        if (lead == 0xF4) hi = 0x8F;
    } else {
        step.valid = 0;
        return step;  // invalid start byte
    }
    step.valid = 1;
    for (std::size_t k = 1; k <= need; ++k) {
        if (i + k >= s.size()) {
            step.truncated = true;
            return step;
        }
        const unsigned char c = byte(i + k);
        const unsigned char low = k == 1 ? lo : 0x80;
        const unsigned char high = k == 1 ? hi : 0xBF;
        if (c < low || c > high) return step;
        cp = (cp << 6) | (c & 0x3Fu);
        ++step.valid;
    }
    step.length = need + 1;
    step.code_point = cp;
    return step;
}

void append_hex4(std::string& out, std::uint32_t v) {
    out += "\\u";
    out += kHex[(v >> 12) & 0xF];
    out += kHex[(v >> 8) & 0xF];
    out += kHex[(v >> 4) & 0xF];
    out += kHex[v & 0xF];
}

// --- dumping -----------------------------------------------------------------------------------------------

void write_string(std::string& out, std::string_view s, bool ensure_ascii) {
    out += '"';
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20 || (ensure_ascii && c == 0x7F)) {
                        append_hex4(out, c);
                    } else {
                        out += static_cast<char>(c);
                    }
            }
            ++i;
            continue;
        }
        const Utf8Step step = utf8_step(s, i);
        if (step.length == 0) throw Error("json", "a string to write is not valid UTF-8");
        if (ensure_ascii) {
            std::uint32_t cp = step.code_point;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                append_hex4(out, 0xD800 + (cp >> 10));
                append_hex4(out, 0xDC00 + (cp & 0x3FF));
            } else {
                append_hex4(out, cp);
            }
        } else {
            out.append(s.substr(i, step.length));
        }
        i += step.length;
    }
    out += '"';
}

void write_newline(std::string& out, const DumpOptions& o, int level) {
    out += '\n';
    out.append(static_cast<std::size_t>(o.indent) * static_cast<std::size_t>(level), ' ');
}

void write_value(std::string& out, const Json& v, const DumpOptions& o, int level) {
    switch (v.type()) {
        case Json::value_t::null: out += "null"; return;
        case Json::value_t::boolean: out += v.get<bool>() ? "true" : "false"; return;
        case Json::value_t::number_integer: {
            char buf[32];
            const auto r = std::to_chars(buf, buf + sizeof buf, v.get<std::int64_t>());
            out.append(buf, r.ptr);
            return;
        }
        case Json::value_t::number_unsigned: {
            char buf[32];
            const auto r = std::to_chars(buf, buf + sizeof buf, v.get<std::uint64_t>());
            out.append(buf, r.ptr);
            return;
        }
        case Json::value_t::number_float: {
            const double d = v.get<double>();
            if (!std::isfinite(d)) throw Error("value", "Out of range float values are not JSON compliant");
            out += py_float_repr(d);
            return;
        }
        case Json::value_t::string: write_string(out, v.get_ref<const std::string&>(), o.ensure_ascii); return;
        case Json::value_t::array: {
            if (v.empty()) {
                out += "[]";
                return;
            }
            out += '[';
            const bool pretty = o.indent >= 0;
            if (pretty) write_newline(out, o, level + 1);
            bool first = true;
            for (const auto& item : v) {
                if (!first) {
                    out += o.item_separator;
                    if (pretty) write_newline(out, o, level + 1);
                }
                first = false;
                write_value(out, item, o, level + 1);
            }
            if (pretty) write_newline(out, o, level);
            out += ']';
            return;
        }
        case Json::value_t::object: {
            if (v.empty()) {
                out += "{}";
                return;
            }
            out += '{';
            const bool pretty = o.indent >= 0;
            if (pretty) write_newline(out, o, level + 1);
            std::vector<const Json::object_t::value_type*> items;
            items.reserve(v.size());
            for (const auto& item : v.get_ref<const Json::object_t&>()) items.push_back(&item);
            if (o.sort_keys) {
                std::stable_sort(items.begin(), items.end(),
                                 [](const auto* a, const auto* b) { return a->first < b->first; });
            }
            bool first = true;
            for (const auto* item : items) {
                if (!first) {
                    out += o.item_separator;
                    if (pretty) write_newline(out, o, level + 1);
                }
                first = false;
                write_string(out, item->first, o.ensure_ascii);
                out += o.key_separator;
                write_value(out, item->second, o, level + 1);
            }
            if (pretty) write_newline(out, o, level);
            out += '}';
            return;
        }
        case Json::value_t::binary:
        case Json::value_t::discarded: break;
    }
    throw Error("json", "a value has no JSON form");
}

// --- parsing (CPython's Modules/_json.c scanner, its accepted syntax and its messages) -------------------------

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    Parser(std::string_view text, ParseRepairs* repairs, const ParseOptions& options)
        : s_(text), repairs_(repairs), options_(options) {}

    Json document() {
        std::size_t idx = skip_ws(0);
        Json value = scan(idx, 0);
        idx = skip_ws(idx);
        if (idx != s_.size()) fail("Extra data", idx);
        return value;
    }

private:
    struct Place {
        bool in_array = false;
        std::size_t index = 0;
        std::string key;
    };

    std::string_view s_;
    ParseRepairs* repairs_;
    ParseOptions options_;
    std::vector<Place> places_;

    std::size_t skip_ws(std::size_t i) const {
        while (i < s_.size() && is_ws(s_[i])) ++i;
        return i;
    }

    std::string pointer() const {
        std::string out;
        for (const auto& place : places_) {
            out = place.in_array ? json_pointer_append(out, place.index) : json_pointer_append(out, place.key);
        }
        return out;
    }

    [[noreturn]] void fail(std::string_view message, std::size_t byte_pos) const {
        // Python counts the characters of the decoded text (after read_text's newline translation):
        // "line L column C (char P)" with C = P - (the last newline before P).
        long long chars = 0, line = 1, last_newline = -1;
        for (std::size_t b = 0; b < byte_pos && b < s_.size(); ++b) {
            const auto c = static_cast<unsigned char>(s_[b]);
            if ((c & 0xC0) == 0x80) continue;  // a continuation byte: part of the previous character
            if (options_.universal_newlines && c == '\r') {
                if (b + 1 < byte_pos && s_[b + 1] == '\n') ++b;
                ++line;
                last_newline = chars;
            } else if (c == '\n') {
                ++line;
                last_newline = chars;
            }
            ++chars;
        }
        throw Error("json", std::string(message) + ": line " + std::to_string(line) + " column " +
                                std::to_string(chars - last_newline) + " (char " + std::to_string(chars) + ")");
    }

    Json scan(std::size_t& idx, int depth) {
        if (idx >= s_.size()) fail("Expecting value", idx);
        const std::size_t length = s_.size();
        switch (s_[idx]) {
            case '"': {
                std::size_t next = idx + 1;
                std::string text = parse_string(next);
                idx = next;
                return Json(std::move(text));
            }
            case '{':
                if (depth >= options_.max_depth) too_deep(idx);
                return parse_object(idx, depth + 1);
            case '[':
                if (depth >= options_.max_depth) too_deep(idx);
                return parse_array(idx, depth + 1);
            case 'n':
                if (idx + 3 < length && s_.substr(idx, 4) == "null") {
                    idx += 4;
                    return Json(nullptr);
                }
                break;
            case 't':
                if (idx + 3 < length && s_.substr(idx, 4) == "true") {
                    idx += 4;
                    return Json(true);
                }
                break;
            case 'f':
                if (idx + 4 < length && s_.substr(idx, 5) == "false") {
                    idx += 5;
                    return Json(false);
                }
                break;
            case 'N':
                if (idx + 2 < length && s_.substr(idx, 3) == "NaN") {
                    idx += 3;
                    return nonfinite();
                }
                break;
            case 'I':
                if (idx + 7 < length && s_.substr(idx, 8) == "Infinity") {
                    idx += 8;
                    return nonfinite();
                }
                break;
            case '-':
                if (idx + 8 < length && s_.substr(idx, 9) == "-Infinity") {
                    idx += 9;
                    return nonfinite();
                }
                break;
            default: break;
        }
        Json number;
        if (!parse_number(idx, number)) fail("Expecting value", idx);
        return number;
    }

    [[noreturn]] void too_deep(std::size_t idx) const {
        fail("JSON nested too deeply (more than " + std::to_string(options_.max_depth) + " levels)", idx);
    }

    Json nonfinite() {
        if (repairs_ != nullptr) repairs_->nonfinite.push_back(pointer());
        return Json(nullptr);
    }

    Json parse_object(std::size_t& idx, int depth) {
        // idx is at '{'
        const std::size_t length = s_.size();
        std::vector<std::pair<std::string, Json>> items;
        std::unordered_map<std::string, std::size_t> index;  // (only once an object is big)
        std::size_t i = skip_ws(idx + 1);
        if (i >= length || s_[i] != '}') {
            places_.push_back(Place{});
            while (true) {
                if (i >= length || s_[i] != '"') fail("Expecting property name enclosed in double quotes", i);
                std::size_t next = i + 1;
                std::string key = parse_string(next);
                i = skip_ws(next);
                if (i >= length || s_[i] != ':') fail("Expecting ':' delimiter", i);
                i = skip_ws(i + 1);
                places_.back().key = key;
                Json value = scan(i, depth);
                // A repeated key keeps its first place and takes the last value (Python's dict).
                std::size_t at = items.size();
                if (items.size() >= 16) {
                    if (index.size() != items.size()) {
                        index.clear();
                        for (std::size_t k = 0; k < items.size(); ++k) index.emplace(items[k].first, k);
                    }
                    if (const auto it = index.find(key); it != index.end()) at = it->second;
                } else {
                    for (std::size_t k = 0; k < items.size(); ++k) {
                        if (items[k].first == key) {
                            at = k;
                            break;
                        }
                    }
                }
                if (at < items.size()) {
                    items[at].second = std::move(value);
                } else {
                    if (!index.empty()) index.emplace(key, items.size());
                    items.emplace_back(std::move(key), std::move(value));
                }
                i = skip_ws(i);
                if (i < length && s_[i] == '}') break;
                if (i >= length || s_[i] != ',') fail("Expecting ',' delimiter", i);
                i = skip_ws(i + 1);
            }
            places_.pop_back();
        }
        idx = i + 1;
        Json out = Json::object();
        auto& map = out.get_ref<Json::object_t&>();
        using Base = std::vector<std::pair<const std::string, Json>>;
        auto& base = static_cast<Base&>(map);
        base.reserve(items.size());
        for (auto& [key, value] : items) base.emplace_back(std::move(key), std::move(value));
        return out;
    }

    Json parse_array(std::size_t& idx, int depth) {
        const std::size_t length = s_.size();
        Json out = Json::array();
        std::size_t i = skip_ws(idx + 1);
        if (i >= length || s_[i] != ']') {
            places_.push_back(Place{true, 0, {}});
            while (true) {
                places_.back().index = out.size();
                out.push_back(scan(i, depth));
                i = skip_ws(i);
                if (i < length && s_[i] == ']') break;
                if (i >= length || s_[i] != ',') fail("Expecting ',' delimiter", i);
                i = skip_ws(i + 1);
            }
            places_.pop_back();
        }
        idx = i + 1;
        return out;
    }

    // From just after the opening quote; leaves `idx` just after the closing quote.
    std::string parse_string(std::size_t& idx) {
        const std::size_t begin = idx - 1;
        const std::size_t length = s_.size();
        std::string out;
        std::size_t end = idx;
        while (true) {
            std::size_t next = end;
            char c = 0;
            for (; next < length; ++next) {
                c = s_[next];
                if (c == '"' || c == '\\') break;
                if (static_cast<unsigned char>(c) <= 0x1F) fail("Invalid control character at", next);
            }
            if (next >= length) fail("Unterminated string starting at", begin);
            out.append(s_.substr(end, next - end));
            if (c == '"') {
                idx = next + 1;
                return out;
            }
            ++next;  // past the backslash
            if (next == length) fail("Unterminated string starting at", begin);
            c = s_[next];
            if (c != 'u') {
                end = next + 1;
                switch (c) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    default: fail("Invalid \\escape", end - 2);
                }
                continue;
            }
            ++next;  // the first hex digit
            end = next + 4;
            if (end >= length) fail("Invalid \\uXXXX escape", next - 1);
            std::uint32_t cp = 0;
            for (; next < end; ++next) {
                const int d = hex_value(s_[next]);
                if (d < 0) fail("Invalid \\uXXXX escape", end - 5);
                cp = (cp << 4) | static_cast<std::uint32_t>(d);
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && end + 6 < length && s_[next] == '\\' && s_[next + 1] == 'u') {
                next += 2;
                std::uint32_t low = 0;
                const std::size_t end2 = end + 6;
                for (; next < end2; ++next) {
                    const int d = hex_value(s_[next]);
                    if (d < 0) fail("Invalid \\uXXXX escape", end2 - 5);
                    low = (low << 4) | static_cast<std::uint32_t>(d);
                }
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    end = end2;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                // Python keeps a lone surrogate in a str; UTF-8 has no way to hold one (and Python cannot write it
                // out again either).
                fail("Lone surrogate \\u" + std::string{kHex[(cp >> 12) & 0xF], kHex[(cp >> 8) & 0xF],
                                                         kHex[(cp >> 4) & 0xF], kHex[cp & 0xF]} +
                         " cannot be read",
                     end - 6);
            }
            append_utf8(out, cp);
        }
    }

    bool parse_number(std::size_t& idx, Json& out) {
        const std::size_t length = s_.size();
        const std::size_t start = idx;
        std::size_t i = idx;
        if (s_[i] == '-') {
            ++i;
            if (i >= length) return false;
        }
        if (s_[i] >= '1' && s_[i] <= '9') {
            ++i;
            while (i < length && is_digit(s_[i])) ++i;
        } else if (s_[i] == '0') {
            ++i;
        } else {
            return false;
        }
        bool is_float = false;
        if (i + 1 < length && s_[i] == '.' && is_digit(s_[i + 1])) {
            is_float = true;
            i += 2;
            while (i < length && is_digit(s_[i])) ++i;
        }
        if (i + 1 < length && (s_[i] == 'e' || s_[i] == 'E')) {
            const std::size_t e_start = i;
            ++i;
            if (i + 1 < length && (s_[i] == '-' || s_[i] == '+')) ++i;
            while (i < length && is_digit(s_[i])) ++i;
            if (is_digit(s_[i - 1])) {
                is_float = true;
            } else {
                i = e_start;
            }
        }
        const std::string_view text = s_.substr(start, i - start);
        idx = i;
        if (!is_float) {
            std::int64_t value = 0;
            auto r = std::from_chars(text.data(), text.data() + text.size(), value);
            if (r.ec == std::errc{} && r.ptr == text.data() + text.size()) {
                out = value;
                return true;
            }
            if (text.front() != '-') {
                std::uint64_t big = 0;
                r = std::from_chars(text.data(), text.data() + text.size(), big);
                if (r.ec == std::errc{} && r.ptr == text.data() + text.size()) {
                    out = big;
                    return true;
                }
            }
            // Python keeps it exactly; the nearest double is the best a Json can do.
            const double d = to_double(text);
            if (!std::isfinite(d)) {
                out = nonfinite();
                return true;
            }
            if (repairs_ != nullptr) repairs_->inexact.push_back(pointer());
            out = d;
            return true;
        }
        const double d = to_double(text);
        if (!std::isfinite(d)) {
            out = nonfinite();
            return true;
        }
        out = d;
        return true;
    }

    // Python's float(text) for JSON number syntax: correctly rounded; an overflow is ±inf and an underflow ±0.0.
    static double to_double(std::string_view text) {
        double value = 0.0;
        const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
        if (r.ec == std::errc{}) return value;
        // Out of range: the power of ten of the first non-zero digit tells an overflow from an underflow.
        const bool negative = text.front() == '-';
        std::string_view mantissa = text.substr(negative ? 1 : 0);
        long long exponent = 0;
        if (const auto e = mantissa.find_first_of("eE"); e != std::string_view::npos) {
            std::string_view digits = mantissa.substr(e + 1);
            mantissa = mantissa.substr(0, e);
            const bool negative_exponent = !digits.empty() && digits.front() == '-';
            if (!digits.empty() && (digits.front() == '-' || digits.front() == '+')) digits.remove_prefix(1);
            long long parsed = 0;
            if (std::from_chars(digits.data(), digits.data() + digits.size(), parsed).ec != std::errc{}) {
                parsed = 1LL << 40;
            }
            exponent = negative_exponent ? -parsed : parsed;
        }
        const auto dot = mantissa.find('.');
        const std::string_view whole = mantissa.substr(0, dot);
        long long lead = 0;
        if (const auto k = whole.find_first_not_of('0'); k != std::string_view::npos) {
            lead = static_cast<long long>(whole.size() - 1 - k);
        } else if (dot != std::string_view::npos) {
            const auto m = mantissa.substr(dot + 1).find_first_not_of('0');
            lead = m == std::string_view::npos ? 0 : -static_cast<long long>(m) - 1;
        }
        const double magnitude = lead + exponent > 0 ? HUGE_VAL : 0.0;
        return negative ? -magnitude : magnitude;
    }
};

}  // namespace

std::string dump(const Json& value, const DumpOptions& options) {
    std::string out;
    write_value(out, value, options, 0);
    return out;
}

std::string dump_python_indent2(const Json& value) {
    DumpOptions options;
    options.indent = 2;
    options.item_separator = ",";
    options.key_separator = ": ";
    return dump(value, options);
}

std::string dump_python(const Json& value, bool ensure_ascii) {
    DumpOptions options;
    options.ensure_ascii = ensure_ascii;
    return dump(value, options);
}

std::string dump_canonical(const Json& value) {
    DumpOptions options;
    options.item_separator = ",";
    options.key_separator = ":";
    options.sort_keys = true;
    return dump(value, options);
}

std::optional<std::string> utf8_error(std::string_view bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        if (static_cast<unsigned char>(bytes[i]) < 0x80) {
            ++i;
            continue;
        }
        const Utf8Step step = utf8_step(bytes, i);
        if (step.length != 0) {
            i += step.length;
            continue;
        }
        const char* reason = step.valid == 0  ? "invalid start byte"
                             : step.truncated ? "unexpected end of data"
                                              : "invalid continuation byte";
        const std::size_t span = std::max<std::size_t>(step.valid, 1);
        char hex[8];
        std::snprintf(hex, sizeof hex, "0x%02x", static_cast<unsigned>(static_cast<unsigned char>(bytes[i])));
        if (span == 1) {
            return std::string("'utf-8' codec can't decode byte ") + hex + " in position " + std::to_string(i) + ": " +
                   reason;
        }
        return "'utf-8' codec can't decode bytes in position " + std::to_string(i) + "-" +
               std::to_string(i + span - 1) + ": " + reason;
    }
    return std::nullopt;
}

Json parse_python_json(std::string_view text, ParseRepairs* repairs, const ParseOptions& options) {
    if (auto error = utf8_error(text)) throw Error("json", *error);
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
        if (!options.bytes) throw Error("json", "Unexpected UTF-8 BOM (decode using utf-8-sig): line 1 column 1 (char 0)");
        text.remove_prefix(3);  // json.loads(bytes) decodes with utf-8-sig: the BOM is not a character
    }
    Parser parser(text, repairs, options);
    return parser.document();
}

std::string json_pointer_token(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (const char c : token) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

std::string json_pointer_append(std::string_view pointer, std::string_view token) {
    std::string out(pointer);
    out += '/';
    out += json_pointer_token(token);
    return out;
}

std::string json_pointer_append(std::string_view pointer, std::size_t index) {
    std::string out(pointer);
    out += '/';
    out += std::to_string(index);
    return out;
}

}  // namespace genko::core
