#include "core/pyops.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

#include "core/command_bus.hpp"
#include "core/limits.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

void not_yet_ported(const std::string& message) { throw Error("not_yet_ported", message); }

void raise_index_error(std::string_view message) { throw PyUncaught("IndexError", std::string(message)); }

void raise_attribute_error(const Json& value, std::string_view attribute) {
    throw PyUncaught("AttributeError", "'" + py_type_name(value) + "' object has no attribute '" + std::string(attribute) + "'");
}

const Json* get(const Json& object, std::string_view key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &*it;
}

Json get_or(const Json& object, std::string_view key, const Json& fallback) {
    const Json* value = get(object, key);
    return value != nullptr ? *value : fallback;
}

bool has(const Json& object, std::string_view key) { return get(object, key) != nullptr; }

bool truthy_at(const Json& object, std::string_view key) {
    const Json* value = get(object, key);
    return value != nullptr && py_truthy(*value);
}

const Json* dict_get(const Json& object, std::string_view key) {
    if (!object.is_object()) raise_attribute_error(object, "get");
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

Json py_get(const Json& object, std::string_view key, const Json& fallback) {
    const Json* value = dict_get(object, key);
    return value != nullptr ? *value : fallback;
}

const Json& get_else(const Json& object, std::string_view key, const Json& fallback) {
    const Json* value = dict_get(object, key);
    return value != nullptr && py_truthy(*value) ? *value : fallback;
}

Json py_or(const Json& value, const Json& fallback) { return py_truthy(value) ? value : fallback; }

double to_float(const Json& value) {
    try {
        return py_float(value);
    } catch (const Error& error) {
        if (value.is_string()) throw PyValueError(error.what());
        throw PyTypeError(error.what());
    }
}

std::int64_t to_int(const Json& value) {
    if (value.is_number_float()) {
        const double d = value.get<double>();
        if (std::isnan(d)) throw PyValueError("cannot convert float NaN to integer");
        if (std::isinf(d)) throw PyUncaught("OverflowError", "cannot convert float infinity to integer");
    }
    try {
        return py_int(value);
    } catch (const Error& error) {
        if (value.is_string()) throw PyValueError(error.what());
        throw PyTypeError(error.what());
    }
}

std::int64_t to_int_held(const Json& value) {
    if (value.is_number_float()) return py_trunc_held(value.get<double>());
    if (value.is_number_unsigned() || value.is_string()) {
        if (const auto big = py_big_int_text(value)) {
            return big->front() == '-' ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
        }
    }
    return to_int(value);
}

Num to_num(const Json& value) {
    if (value.is_boolean()) return Num(value.get<bool>() ? 1 : 0);
    if (const auto n = Num::from_json(value)) return *n;
    throw PyTypeError("expected a number, not '" + py_type_name(value) + "'");
}

double to_real(const Json& value) {
    if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
    if (value.is_number()) return value.get<double>();
    throw PyTypeError("must be real number, not " + py_type_name(value));
}

std::vector<Json> iterate(const Json& value) {
    if (value.is_array()) return std::vector<Json>(value.begin(), value.end());
    if (value.is_string() || value.is_object()) {
        const Json items = py_list(value);
        return std::vector<Json>(items.begin(), items.end());
    }
    throw PyTypeError("'" + py_type_name(value) + "' object is not iterable");
}

std::size_t length(const Json& value) {
    if (value.is_array() || value.is_object()) return value.size();
    if (value.is_string()) return py_list(value).size();
    throw PyTypeError("object of type '" + py_type_name(value) + "' has no len()");
}

Json subscript(const Json& value, std::int64_t index) {
    if (value.is_array() || value.is_string()) {
        const Json characters = value.is_string() ? py_list(value) : Json();
        const Json& items = value.is_string() ? characters : value;
        const auto size = static_cast<std::int64_t>(items.size());
        const std::int64_t at = index < 0 ? index + size : index;
        if (at < 0 || at >= size) raise_index_error(value.is_array() ? "list index out of range" : "string index out of range");
        return items[static_cast<std::size_t>(at)];
    }
    if (value.is_object()) {
        // (a JSON object's keys are strings: an int key is never there)
        throw OpKeyError(std::to_string(index));
    }
    throw PyTypeError("'" + py_type_name(value) + "' object is not subscriptable");
}

Json subscript(const Json& value, std::string_view key) {
    if (value.is_object()) {
        const Json* found = get(value, key);
        if (found == nullptr) throw OpKeyError(py_repr_str(key));
        return *found;
    }
    if (value.is_array()) throw PyTypeError("list indices must be integers or slices, not str");
    if (value.is_string()) throw PyTypeError("string indices must be integers, not 'str'");
    throw PyTypeError("'" + py_type_name(value) + "' object is not subscriptable");
}

Json py_slice(const Json& list, std::int64_t start, std::int64_t stop) {
    const auto n = static_cast<std::int64_t>(list.size());
    const auto clamp = [n](std::int64_t i) {
        if (i < 0) i += n;
        return i < 0 ? 0 : (i > n ? n : i);
    };
    const std::int64_t a = clamp(start);
    const std::int64_t b = clamp(stop);
    Json out = Json::array();
    for (std::int64_t i = a; i < b; ++i) out.push_back(list[static_cast<std::size_t>(i)]);
    return out;
}

std::vector<double> unpack_floats(const Json& value, std::size_t expected) {
    const std::vector<Json> items = iterate(value);
    std::vector<double> out;
    // (unpacking takes one item more than it needs, to see that there is none: that one is converted too)
    for (std::size_t i = 0; i < items.size() && i <= expected; ++i) out.push_back(to_float(items[i]));
    if (out.size() < expected) {
        throw PyValueError("not enough values to unpack (expected " + std::to_string(expected) + ", got " +
                           std::to_string(out.size()) + ")");
    }
    if (out.size() > expected) throw PyValueError("too many values to unpack (expected " + std::to_string(expected) + ")");
    return out;
}

std::vector<std::int64_t> int_tuple(const Json& value) {
    std::vector<std::int64_t> out;
    for (const Json& v : iterate(value)) out.push_back(to_int(v));
    return out;
}

std::vector<Json> unpack_values(const Json& value, std::size_t expected) {
    if (!(value.is_array() || value.is_string() || value.is_object())) {
        throw PyTypeError("cannot unpack non-iterable " + py_type_name(value) + " object");
    }
    std::vector<Json> items = iterate(value);
    if (items.size() < expected) {
        throw PyValueError("not enough values to unpack (expected " + std::to_string(expected) + ", got " +
                           std::to_string(items.size()) + ")");
    }
    if (items.size() > expected) throw PyValueError("too many values to unpack (expected " + std::to_string(expected) + ")");
    return items;
}

bool py_less(const Json& a, const Json& b, std::string_view op) {
    const auto number = [](const Json& v) -> std::optional<Num> {
        if (v.is_boolean()) return Num(v.get<bool>() ? 1 : 0);
        return Num::from_json(v);
    };
    const auto x = number(a);
    const auto y = number(b);
    if (!x || !y) {
        if (a.is_string() && b.is_string()) {
            const auto& s = a.get_ref<const std::string&>();
            const auto& t = b.get_ref<const std::string&>();
            if (op == "<") return s < t;
            if (op == "<=") return s <= t;
            if (op == ">") return s > t;
            return s >= t;
        }
        throw PyTypeError("'" + std::string(op) + "' not supported between instances of '" + py_type_name(a) + "' and '" +
                          py_type_name(b) + "'");
    }
    if (op == "<") return *x < *y;
    if (op == "<=") return *x <= *y;
    if (op == ">") return *x > *y;
    return *x >= *y;
}

void require_hashable(const Json& value) {
    if (value.is_array()) throw PyTypeError("unhashable type: 'list'");
    if (value.is_object()) throw PyTypeError("unhashable type: 'dict'");
}

bool py_equals(const Json& a, const Json& b) {
    const auto number = [](const Json& v) -> std::optional<Num> {
        if (v.is_boolean()) return Num(v.get<bool>() ? 1 : 0);
        return Num::from_json(v);
    };
    const auto x = number(a);
    const auto y = number(b);
    if (x || y) return x && y && *x == *y;
    if (a.type() != b.type()) return false;
    if (a.is_array()) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!py_equals(a[i], b[i])) return false;
        }
        return true;
    }
    if (a.is_object()) {
        if (a.size() != b.size()) return false;
        for (const auto& [key, item] : a.items()) {
            const Json* other = get(b, key);
            if (other == nullptr || !py_equals(item, *other)) return false;
        }
        return true;
    }
    return a == b;
}

bool is_one_of(const Json& value, std::initializer_list<std::string_view> names) {
    if (!value.is_string()) return false;
    for (const std::string_view name : names) {
        if (value.get_ref<const std::string&>() == name) return true;
    }
    return false;
}

bool is_blank(std::string_view text) {
    std::size_t at = 0;
    while (at < text.size()) {
        const auto c = static_cast<unsigned char>(text[at]);
        std::uint32_t cp = c;
        std::size_t n = 1;
        if (c >= 0xF0) {
            n = 4;
            cp = c & 0x07u;
        } else if (c >= 0xE0) {
            n = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xC0) {
            n = 2;
            cp = c & 0x1Fu;
        }
        for (std::size_t k = 1; k < n && at + k < text.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[at + k]) & 0x3Fu);
        const bool space = (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x20) || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
                           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
                           cp == 0x3000;
        if (!space) return false;
        at += n;
    }
    return true;
}

Json round_json(const Json& value, int ndigits) {
    if (value.is_boolean()) return Json(value.get<bool>() ? 1 : 0);
    if (const auto n = Num::from_json(value)) return py_round(*n, ndigits).json();
    throw PyTypeError("type " + py_type_name(value) + " doesn't define __round__ method");
}

std::int64_t loop_count(double steps) {
    if (std::isnan(steps)) throw PyValueError("cannot convert float NaN to integer");
    if (std::isinf(steps)) throw PyUncaught("OverflowError", "cannot convert float infinity to integer");
    constexpr double kMost = limits::kLoopSteps;
    if (steps >= kMost) throw PyUncaught("MemoryError", "");
    if (steps <= -kMost) return -static_cast<std::int64_t>(kMost);  // (callers take max(1, …))
    return py_trunc_int(steps);
}

Json nums_json(const std::vector<Num>& values) {
    Json out = Json::array();
    for (const Num& v : values) out.push_back(v.json());
    return out;
}

namespace {

bool py_space(char32_t c) {
    return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

// The code point that starts at text[i] and its length in bytes (UTF-8).
std::pair<char32_t, std::size_t> code_point_at(std::string_view text, std::size_t i) {
    const auto c = static_cast<unsigned char>(text[i]);
    std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    if (i + n > text.size()) n = text.size() - i;
    char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
    for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
    return {cp, n};
}

}  // namespace

std::string py_strip(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    std::size_t i = 0;
    bool leading = true;
    while (i < text.size()) {
        const auto [cp, n] = code_point_at(text, i);
        if (!py_space(cp)) {
            if (leading) begin = i;
            leading = false;
            end = i + n;
        }
        i += n;
    }
    if (leading) return {};
    return std::string(text.substr(begin, end - begin));
}

double finite_float(const Json& value, std::string_view key) {
    const double x = to_float(value);
    if (!std::isfinite(x)) throw OpError(std::string(key) + " must be a finite number");
    return x;
}

double int_whole(const Json& value, std::string_view key) {
    if (value.is_number_float()) {
        const double d = value.get<double>();
        if (!std::isfinite(d)) throw OpError(std::string(key) + " must be a finite number");
        return py_trunc(d);
    }
    if (const auto big = py_big_int_text(value)) return std::strtod(big->c_str(), nullptr);
    return static_cast<double>(to_int(value));
}

}  // namespace genko::core
