#include "core/pyvalue.hpp"

#include <cmath>
#include <limits>

#include "core/command_bus.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"

namespace genko::core {

namespace {

constexpr std::int64_t kIntMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kIntMin = std::numeric_limits<std::int64_t>::min();

bool is_number(const Json& v) {
    return v.is_boolean() || v.is_number_integer() || v.is_number_unsigned() || v.is_number_float();
}

// The UTF-8 characters of a str.
Json characters(const Json& text) { return py_list(text); }

}  // namespace

double to_float(const Json& value) {
    if (is_number(value)) return py_float(value);
    try {
        return py_float(value);
    } catch (const Error& error) {
        if (value.is_string()) throw PyValueError(error.what());
        throw PyTypeError(error.what());
    }
}

std::int64_t to_int(const Json& value) {
    switch (value.type()) {
        case Json::value_t::boolean: return value.get<bool>() ? 1 : 0;
        case Json::value_t::number_integer: return value.get<std::int64_t>();
        case Json::value_t::number_unsigned: {
            const auto u = value.get<std::uint64_t>();
            return u > static_cast<std::uint64_t>(kIntMax) ? kIntMax : static_cast<std::int64_t>(u);
        }
        case Json::value_t::number_float: {
            const double d = value.get<double>();
            if (std::isnan(d)) throw PyValueError("cannot convert float NaN to integer");
            const double t = std::trunc(d);
            if (t >= 9223372036854775808.0) return kIntMax;
            if (t < -9223372036854775808.0) return kIntMin;
            return static_cast<std::int64_t>(t);
        }
        case Json::value_t::string: {
            const auto& text = value.get_ref<const std::string&>();
            try {
                return py_int(value);
            } catch (const Error& error) {
                const std::string message = error.what();
                if (message.starts_with("invalid literal")) throw PyValueError(message);
                // a decimal number beyond 64 bits: held at the end of the range on its side
                const auto first = text.find_first_not_of(" \t\n\r\v\f");
                return first != std::string::npos && text[first] == '-' ? kIntMin : kIntMax;
            }
        }
        default:
            throw PyTypeError("int() argument must be a string, a bytes-like object or a real number, not '" +
                              py_type_name(value) + "'");
    }
}

std::int64_t py_int_of(double x) {
    if (std::isnan(x)) throw PyValueError("cannot convert float NaN to integer");
    if (std::isinf(x)) throw PyValueError("cannot convert float infinity to integer");
    const double t = std::trunc(x);
    if (t >= 9223372036854775808.0) return kIntMax;
    if (t < -9223372036854775808.0) return kIntMin;
    return static_cast<std::int64_t>(t);
}

std::int64_t checked_count(std::int64_t n, std::int64_t limit) {
    if (n > limit) throw PyValueError("too many points (" + std::to_string(n) + "): the shape is far too large");
    return n;
}

double math_sin(double x) {
    if (std::isinf(x)) throw PyValueError("math domain error");
    return py_sin(x);
}

double math_cos(double x) {
    if (std::isinf(x)) throw PyValueError("math domain error");
    return py_cos(x);
}

Json py_item(const Json& value, std::size_t i) {
    if (value.is_array()) {
        if (i >= value.size()) throw PyIndexError("list index out of range");
        return value[i];
    }
    if (value.is_string()) {
        const Json chars = characters(value);
        if (i >= chars.size()) throw PyIndexError("string index out of range");
        return chars[i];
    }
    if (value.is_object()) throw OpKeyError(py_key_repr(i));
    throw PyTypeError("'" + py_type_name(value) + "' object is not subscriptable");
}

std::size_t py_len(const Json& value) {
    if (value.is_array() || value.is_object()) return value.size();
    if (value.is_string()) return characters(value).size();
    throw PyTypeError("object of type '" + py_type_name(value) + "' has no len()");
}

bool py_equal(const Json& a, const Json& b) {
    if (is_number(a) && is_number(b)) {
        if ((a.is_number_integer() || a.is_boolean()) && (b.is_number_integer() || b.is_boolean())) {
            return py_int(a) == py_int(b);
        }
        if (a.is_number_unsigned() || b.is_number_unsigned()) {
            if (a.is_number_unsigned() && b.is_number_unsigned()) return a.get<std::uint64_t>() == b.get<std::uint64_t>();
            if (a.is_number_float() || b.is_number_float()) return py_float(a) == py_float(b);
            return false;  // (a negative or small int never equals a number beyond int64)
        }
        // an int and a float: equal when the float is that whole number (exact for every int Genko keeps)
        const Num x = a.is_number_float() ? Num(a.get<double>()) : Num(py_int(a));
        const Num y = b.is_number_float() ? Num(b.get<double>()) : Num(py_int(b));
        return x == y;
    }
    if (a.is_null() || b.is_null()) return a.is_null() && b.is_null();
    if (a.is_string() || b.is_string()) {
        return a.is_string() && b.is_string() && a.get_ref<const std::string&>() == b.get_ref<const std::string&>();
    }
    if (a.is_array() && b.is_array()) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!py_equal(a[i], b[i])) return false;
        }
        return true;
    }
    if (a.is_object() && b.is_object()) {
        if (a.size() != b.size()) return false;
        for (const auto& [key, item] : a.items()) {
            const auto it = b.find(key);
            if (it == b.end() || !py_equal(item, *it)) return false;
        }
        return true;
    }
    return false;
}

Json py_iter(const Json& value) {
    if (value.is_array() || value.is_object() || value.is_string()) return py_list(value);
    throw PyTypeError("'" + py_type_name(value) + "' object is not iterable");
}

Json py_get(const Json& object, std::string_view key, const Json& fallback) {
    if (!object.is_object()) throw PyTypeError("'" + py_type_name(object) + "' object has no attribute 'get'");
    const auto it = object.find(key);
    return it == object.end() ? fallback : *it;
}

Json py_or(const Json& value, const Json& fallback) { return py_truthy(value) ? value : fallback; }

std::string py_key_repr(std::size_t i) { return std::to_string(i); }

}  // namespace genko::core
