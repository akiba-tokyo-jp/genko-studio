#include "core/pyconv.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <system_error>

#include "core/error.hpp"
#include "core/pynum.hpp"

namespace genko::core {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

std::string_view strip(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Python's rule for "_" in numbers (PEP 515): only one at a time and only between two digits.
bool remove_underscores(std::string_view s, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_') {
            if (i == 0 || i + 1 >= s.size() || !is_digit(s[i - 1]) || !is_digit(s[i + 1])) return false;
            continue;
        }
        out += s[i];
    }
    return true;
}

bool equals_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (x != b[i]) return false;
    }
    return true;
}

std::optional<double> parse_float_text(std::string_view original) {
    std::string text;
    if (!remove_underscores(strip(original), text) || text.empty()) return std::nullopt;
    std::string_view body = text;
    bool negative = false;
    if (body.front() == '+' || body.front() == '-') {
        negative = body.front() == '-';
        body.remove_prefix(1);
    }
    if (equals_ignore_case(body, "inf") || equals_ignore_case(body, "infinity")) {
        return negative ? -HUGE_VAL : HUGE_VAL;
    }
    if (equals_ignore_case(body, "nan")) return negative ? -NAN : NAN;
    if (body.empty() || !(is_digit(body.front()) || body.front() == '.')) return std::nullopt;
    double value = 0.0;
    const auto r = std::from_chars(body.data(), body.data() + body.size(), value, std::chars_format::general);
    if (r.ptr != body.data() + body.size()) return std::nullopt;
    if (r.ec == std::errc::result_out_of_range) {
        // overflow → inf, underflow → 0 (Python's float() does not raise for either)
        const auto e = body.find_first_of("eE");
        const bool tiny = e != std::string_view::npos && body.substr(e + 1).starts_with('-');
        value = tiny ? 0.0 : HUGE_VAL;
    } else if (r.ec != std::errc{}) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

std::optional<std::int64_t> parse_int_text(std::string_view original) {
    std::string text;
    if (!remove_underscores(strip(original), text) || text.empty()) return std::nullopt;
    std::string_view body = text;
    bool negative = false;
    if (body.front() == '+' || body.front() == '-') {
        negative = body.front() == '-';
        body.remove_prefix(1);
    }
    if (body.empty()) return std::nullopt;
    for (const char c : body) {
        if (!is_digit(c)) return std::nullopt;
    }
    std::uint64_t magnitude = 0;
    const auto r = std::from_chars(body.data(), body.data() + body.size(), magnitude);
    if (r.ec != std::errc{}) throw Error("format", "int(" + py_repr_str(original) + ") is too large for this build");
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!negative && magnitude > limit) {
        throw Error("format", "int(" + py_repr_str(original) + ") is too large for this build");
    }
    if (negative && magnitude > limit + 1) {
        throw Error("format", "int(" + py_repr_str(original) + ") is too large for this build");
    }
    if (negative) return magnitude == limit + 1 ? std::numeric_limits<std::int64_t>::min()
                                                : -static_cast<std::int64_t>(magnitude);
    return static_cast<std::int64_t>(magnitude);
}

std::uint32_t decode_one(std::string_view s, std::size_t& i) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t length = 1;
    std::uint32_t cp = c;
    if (c >= 0xF0) {
        length = 4;
        cp = c & 0x07u;
    } else if (c >= 0xE0) {
        length = 3;
        cp = c & 0x0Fu;
    } else if (c >= 0xC0) {
        length = 2;
        cp = c & 0x1Fu;
    }
    for (std::size_t k = 1; k < length && i + k < s.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    }
    i += length;
    return cp;
}

// Py_UNICODE_ISPRINTABLE for the characters likely in a manuscript (an approximation of the Unicode tables:
// separators other than the ASCII space, format characters, private use and surrogates are not printable).
bool printable(std::uint32_t cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0xA0) || cp == 0xAD) return false;
    if (cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F)) return false;
    if ((cp >= 0x205F && cp <= 0x206F) || cp == 0x3000 || cp == 0xFEFF) return false;
    if ((cp >= 0xD800 && cp <= 0xF8FF) || (cp >= 0xFFF9 && cp <= 0xFFFB) || cp == 0xFFFE || cp == 0xFFFF) return false;
    if (cp >= 0xE0000) return false;
    return true;
}

}  // namespace

std::string py_type_name(const Json& value) {
    switch (value.type()) {
        case Json::value_t::null: return "NoneType";
        case Json::value_t::boolean: return "bool";
        case Json::value_t::number_integer:
        case Json::value_t::number_unsigned: return "int";
        case Json::value_t::number_float: return "float";
        case Json::value_t::string: return "str";
        case Json::value_t::array: return "list";
        case Json::value_t::object: return "dict";
        default: return "object";
    }
}

bool py_truthy(const Json& value) {
    switch (value.type()) {
        case Json::value_t::null: return false;
        case Json::value_t::boolean: return value.get<bool>();
        case Json::value_t::number_integer: return value.get<std::int64_t>() != 0;
        case Json::value_t::number_unsigned: return value.get<std::uint64_t>() != 0;
        case Json::value_t::number_float: return value.get<double>() != 0.0;
        case Json::value_t::string: return !value.get_ref<const std::string&>().empty();
        case Json::value_t::array:
        case Json::value_t::object: return !value.empty();
        default: return true;
    }
}

double py_float(const Json& value) {
    switch (value.type()) {
        case Json::value_t::boolean: return value.get<bool>() ? 1.0 : 0.0;
        case Json::value_t::number_integer: return static_cast<double>(value.get<std::int64_t>());
        case Json::value_t::number_unsigned: return static_cast<double>(value.get<std::uint64_t>());
        case Json::value_t::number_float: return value.get<double>();
        case Json::value_t::string: {
            const auto& text = value.get_ref<const std::string&>();
            if (auto parsed = parse_float_text(text)) return *parsed;
            throw Error("format", "could not convert string to float: " + py_repr_str(text));
        }
        default:
            throw Error("format", "float() argument must be a string or a real number, not '" + py_type_name(value) + "'");
    }
}

std::int64_t py_int(const Json& value) {
    switch (value.type()) {
        case Json::value_t::boolean: return value.get<bool>() ? 1 : 0;
        case Json::value_t::number_integer: return value.get<std::int64_t>();
        case Json::value_t::number_unsigned: {
            const auto u = value.get<std::uint64_t>();
            if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw Error("format", "int " + std::to_string(u) + " is too large for this build");
            }
            return static_cast<std::int64_t>(u);
        }
        case Json::value_t::number_float: {
            const double d = value.get<double>();
            if (std::isnan(d)) throw Error("format", "cannot convert float NaN to integer");
            if (std::isinf(d)) throw Error("format", "cannot convert float infinity to integer");
            const double t = std::trunc(d);
            if (t >= 9223372036854775808.0 || t < -9223372036854775808.0) {
                throw Error("format", "int(" + py_float_repr(d) + ") is too large for this build");
            }
            return static_cast<std::int64_t>(t);
        }
        case Json::value_t::string: {
            const auto& text = value.get_ref<const std::string&>();
            if (auto parsed = parse_int_text(text)) return *parsed;
            throw Error("format", "invalid literal for int() with base 10: " + py_repr_str(text));
        }
        default:
            throw Error("format", "int() argument must be a string, a bytes-like object or a real number, not '" +
                                      py_type_name(value) + "'");
    }
}

std::string py_repr_str(std::string_view text) {
    const bool has_single = text.find('\'') != std::string_view::npos;
    const bool has_double = text.find('"') != std::string_view::npos;
    const char quote = has_single && !has_double ? '"' : '\'';
    std::string out(1, quote);
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        const std::uint32_t cp = decode_one(text, i);
        char buf[16];
        if (cp == static_cast<std::uint32_t>(quote) || cp == '\\') {
            out += '\\';
            out += static_cast<char>(cp);
        } else if (cp == '\t') {
            out += "\\t";
        } else if (cp == '\n') {
            out += "\\n";
        } else if (cp == '\r') {
            out += "\\r";
        } else if (cp < 0x20 || cp == 0x7F) {
            std::snprintf(buf, sizeof buf, "\\x%02x", static_cast<unsigned>(cp));
            out += buf;
        } else if (cp < 0x7F || printable(cp)) {
            out.append(text.substr(start, i - start));
        } else if (cp <= 0xFF) {
            std::snprintf(buf, sizeof buf, "\\x%02x", static_cast<unsigned>(cp));
            out += buf;
        } else if (cp <= 0xFFFF) {
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(cp));
            out += buf;
        } else {
            std::snprintf(buf, sizeof buf, "\\U%08x", static_cast<unsigned>(cp));
            out += buf;
        }
    }
    out += quote;
    return out;
}

std::string py_repr(const Json& value) {
    if (value.is_string()) return py_repr_str(value.get_ref<const std::string&>());
    return py_str(value);
}

std::string py_str(const Json& value) {
    switch (value.type()) {
        case Json::value_t::null: return "None";
        case Json::value_t::boolean: return value.get<bool>() ? "True" : "False";
        case Json::value_t::number_integer: return std::to_string(value.get<std::int64_t>());
        case Json::value_t::number_unsigned: return std::to_string(value.get<std::uint64_t>());
        case Json::value_t::number_float: return py_float_repr(value.get<double>());
        case Json::value_t::string: return value.get<std::string>();
        case Json::value_t::array: {
            std::string out = "[";
            bool first = true;
            for (const auto& item : value) {
                if (!first) out += ", ";
                first = false;
                out += py_repr(item);
            }
            return out + "]";
        }
        case Json::value_t::object: {
            std::string out = "{";
            bool first = true;
            for (const auto& [key, item] : value.items()) {
                if (!first) out += ", ";
                first = false;
                out += py_repr_str(key) + ": " + py_repr(item);
            }
            return out + "}";
        }
        default: return "<object>";
    }
}

Json py_list(const Json& value) {
    switch (value.type()) {
        case Json::value_t::array: return value;
        case Json::value_t::string: {
            Json out = Json::array();
            const auto& text = value.get_ref<const std::string&>();
            std::size_t i = 0;
            while (i < text.size()) {
                const std::size_t start = i;
                decode_one(text, i);
                out.push_back(text.substr(start, i - start));
            }
            return out;
        }
        case Json::value_t::object: {
            Json out = Json::array();
            for (const auto& [key, item] : value.items()) out.push_back(key);
            return out;
        }
        default: throw Error("format", "'" + py_type_name(value) + "' object is not iterable");
    }
}

Json py_dict(const Json& value) {
    if (value.is_object()) return value;
    if (!value.is_array() && !value.is_string()) {
        throw Error("format", "'" + py_type_name(value) + "' object is not iterable");
    }
    const Json items = py_list(value);
    Json out = Json::object();
    std::size_t n = 0;
    for (const auto& item : items) {
        if (!item.is_array() && !item.is_string() && !item.is_object()) {
            throw Error("format", "cannot convert dictionary update sequence element #" + std::to_string(n) +
                                      " to a sequence");
        }
        const Json pair = py_list(item);
        if (pair.size() != 2) {
            throw Error("format", "dictionary update sequence element #" + std::to_string(n) + " has length " +
                                      std::to_string(pair.size()) + "; 2 is required");
        }
        if (!pair[0].is_string()) {
            throw Error("format", "dictionary keys read from JSON must be strings (element #" + std::to_string(n) + ")");
        }
        out[pair[0].get<std::string>()] = pair[1];
        ++n;
    }
    return out;
}

}  // namespace genko::core
