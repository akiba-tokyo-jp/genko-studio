#include "core/pyvalue.hpp"

#include <cmath>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"

namespace genko::core::pyv {

namespace {

bool is_number(const Json& v) { return v.is_number() || v.is_boolean(); }

// A JSON number or boolean as Python compares it (True is 1). Integers beyond 2^53 are compared exactly with each
// other and as doubles with floats (Python compares an int and a float exactly; for the ids and numbers of the 3D
// guides this never matters).
bool number_eq(const Json& a, const Json& b) {
    const auto as_int = [](const Json& v, std::int64_t& out) {
        if (v.is_boolean()) {
            out = v.get<bool>() ? 1 : 0;
            return true;
        }
        if (v.is_number_integer()) {
            out = v.get<std::int64_t>();
            return true;
        }
        return false;
    };
    std::int64_t x = 0;
    std::int64_t y = 0;
    if (as_int(a, x) && as_int(b, y)) return x == y;
    if (a.is_number_unsigned() || b.is_number_unsigned()) {
        if (a.is_number_unsigned() && b.is_number_unsigned()) return a.get<std::uint64_t>() == b.get<std::uint64_t>();
    }
    const double da = a.is_boolean() ? (a.get<bool>() ? 1.0 : 0.0) : a.get<double>();
    const double db = b.is_boolean() ? (b.get<bool>() ? 1.0 : 0.0) : b.get<double>();
    return da == db;
}

std::size_t code_points(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

}  // namespace

const Json* get(const Json& obj, std::string_view key) {
    if (!obj.is_object()) raise_attribute(obj, "get");
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

const Json* find(const Json& obj, std::string_view key) {
    if (!obj.is_object()) return nullptr;
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

const Json& get_or(const Json& obj, std::string_view key, const Json& fallback) {
    const Json* value = get(obj, key);
    return value != nullptr && py_truthy(*value) ? *value : fallback;
}

bool eq(const Json& a, const Json& b) {
    if (is_number(a) && is_number(b)) return number_eq(a, b);
    if (a.type() != b.type()) {
        // (an int and a float are both numbers; any other mix is unequal)
        return false;
    }
    switch (a.type()) {
        case Json::value_t::null: return true;
        case Json::value_t::string: return a.get_ref<const std::string&>() == b.get_ref<const std::string&>();
        case Json::value_t::array: {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (!eq(a[i], b[i])) return false;
            }
            return true;
        }
        case Json::value_t::object: {
            if (a.size() != b.size()) return false;
            for (const auto& [key, value] : a.items()) {
                const auto it = b.find(key);
                if (it == b.end() || !eq(value, *it)) return false;
            }
            return true;
        }
        default: return a == b;
    }
}

bool is_one_of(const Json& value, std::initializer_list<std::string_view> names) {
    if (!value.is_string()) return false;
    for (const std::string_view name : names) {
        if (value.get_ref<const std::string&>() == name) return true;
    }
    return false;
}

double to_float(const Json& value) {
    try {
        return py_float(value);
    } catch (const Error& error) {
        throw Error(value.is_string() ? "value" : "type", error.what());
    }
}

std::int64_t to_int(const Json& value) {
    try {
        return py_int(value);
    } catch (const Error& error) {
        if (value.is_number_float() && std::isinf(value.get<double>())) throw Error("overflow", error.what());
        throw Error(value.is_string() || value.is_number() ? "value" : "type", error.what());
    }
}

double real(const Json& value) {
    if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
    if (value.is_number()) return value.get<double>();
    throw Error("type", "must be real number, not " + py_type_name(value));
}

Json at(const Json& sequence, std::int64_t index) {
    if (sequence.is_array() || sequence.is_string()) {
        const Json characters = sequence.is_string() ? py_list(sequence) : Json();
        const Json& items = sequence.is_string() ? characters : sequence;
        const auto n = static_cast<std::int64_t>(items.size());
        const std::int64_t i = index < 0 ? index + n : index;
        if (i < 0 || i >= n) raise_index(sequence.is_array() ? "list index out of range" : "string index out of range");
        return items[static_cast<std::size_t>(i)];
    }
    if (sequence.is_object()) throw OpKeyError(std::to_string(index));
    throw Error("type", "'" + py_type_name(sequence) + "' object is not subscriptable");
}

std::size_t len(const Json& value) {
    if (value.is_array() || value.is_object()) return value.size();
    if (value.is_string()) return code_points(value.get_ref<const std::string&>());
    throw Error("type", "object of type '" + py_type_name(value) + "' has no len()");
}

Json slice(const Json& list, std::int64_t start, std::int64_t stop) {
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

void raise_index(std::string_view message) { throw Error("index", std::string(message)); }

void raise_attribute(const Json& value, std::string_view attribute) {
    throw Error("attribute", "'" + py_type_name(value) + "' object has no attribute '" + std::string(attribute) + "'");
}

bool finite(double x) { return std::isfinite(x); }

bool blank(std::string_view text) {
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

}  // namespace genko::core::pyv
