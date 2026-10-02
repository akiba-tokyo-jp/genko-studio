#pragma once

<<<<<<< HEAD
#include <cstddef>
=======
>>>>>>> native/m3-3d
#include <cstdint>
#include <string>
#include <string_view>

<<<<<<< HEAD
#include "core/error.hpp"
#include "core/json.hpp"

// Python's ValueError and TypeError as the ported modules raise them, and the conversions of JSON values that raise
// them where Python does. They are kept apart because Python catches them apart: an op's `except ValueError as exc:
// raise ApplyError(str(exc))` makes the message the op's own, while the batch reports an uncaught ValueError or
// TypeError as "a value of the wrong type (…)" (core::Error) and a KeyError as "not found: …" (core::OpKeyError).

namespace genko::core {

// ValueError (core::Error code "value").
class PyValueError : public Error {
public:
    explicit PyValueError(const std::string& message) : Error("value", message) {}
};

// TypeError (core::Error code "type").
class PyTypeError : public Error {
public:
    explicit PyTypeError(const std::string& message) : Error("type", message) {}
};

// IndexError: Python lets it out of apply_ops (a traceback); here it is an error like the others (code "index").
class PyIndexError : public Error {
public:
    explicit PyIndexError(const std::string& message) : Error("index", message) {}
};

// float(x): numbers and booleans, numeric strings (ValueError otherwise), TypeError for None, lists and dicts.
double to_float(const Json& value);

// int(x): ints, floats (truncated), booleans and decimal strings (ValueError otherwise), TypeError for the rest. An
// int beyond 64 bits (a float like 1e300) is held at the nearest end of the int64 range: Genko only compares such a
// number with a small bound, which gives the same answer.
std::int64_t to_int(const Json& value);

// int(x) (and math.floor / math.ceil after std::floor / std::ceil) for a float: ValueError for NaN, and for an
// infinity (Python's OverflowError); held at the end of the int64 range beyond it.
std::int64_t py_int_of(double x);

// A count of points made from a length (about one per mm, per pixel, …): more than `limit` is refused (ValueError)
// rather than filling memory for minutes, as Python would for a ruler or an effect a light year long.
std::int64_t checked_count(std::int64_t n, std::int64_t limit = 4'000'000);

// math.sin / math.cos: core::py_sin / py_cos, with CPython's ValueError "math domain error" for an infinity (a NaN
// gives a NaN, as in Python). For the code ported from Python's math module (numpy's sin and cos raise nothing).
double math_sin(double x);
double math_cos(double x);

// x[i] for i >= 0: a list's item, a str's i-th character, KeyError (core::OpKeyError) for a dict, TypeError for the
// rest, IndexError past the end.
Json py_item(const Json& value, std::size_t i);

// len(x): a list's items, a str's characters, a dict's keys; TypeError for the rest.
std::size_t py_len(const Json& value);

// The items `for item in x` meets: a list's items, a str's characters, a dict's keys; TypeError for the rest.
Json py_iter(const Json& value);

// x.get(key, default) on a dict (TypeError, as Python's AttributeError, on anything else).
Json py_get(const Json& object, std::string_view key, const Json& fallback = Json());

// `x or fallback`.
Json py_or(const Json& value, const Json& fallback);

// x == y as Python compares the values json.loads makes: numbers by value whatever their type (True == 1 == 1.0),
// str with str, lists item by item, dicts key by key in any order.
bool py_equal(const Json& a, const Json& b);

// The Python repr of an int i (a KeyError's message).
std::string py_key_repr(std::size_t i);

}  // namespace genko::core
=======
#include "core/json.hpp"

// Python's handling of the JSON values the 3D guides keep (dicts and lists from json.loads), for the code ported from
// genko/mesh3d.py, prim3d.py, persp3d.py, mannequin.py and threeops.py: dict.get, ==, float(), the math module's
// argument check and list indexing, with Python's messages.
//
// Errors are core::Error with the code of the Python exception: "value" (ValueError), "type" (TypeError), "index"
// (IndexError), "key" (KeyError), "attribute" (AttributeError), "overflow" (OverflowError). In an op they all become
// "a value of the wrong type (…)"; where Python lets an exception through apply_ops (IndexError, AttributeError,
// OverflowError) the C++ build refuses the op the same way instead of failing harder.

namespace genko::core::pyv {

// obj.get(key): the value, or null when there is none. Throws "attribute" when obj is not a dict.
const Json* get(const Json& obj, std::string_view key);
// obj.get(key) when obj is a dict (nothing for anything else, without an error).
const Json* find(const Json& obj, std::string_view key);
// `obj.get(key) or fallback`.
const Json& get_or(const Json& obj, std::string_view key, const Json& fallback);

// Python's == between JSON values: numbers and booleans by value (True == 1 == 1.0), lists element by element,
// dicts by their keys in any order.
bool eq(const Json& a, const Json& b);
// `value in (…strings)`.
bool is_one_of(const Json& value, std::initializer_list<std::string_view> names);

// float(x) ("value" for a str that is not a number, "type" for None, a list or a dict).
double to_float(const Json& value);
// int(x).
std::int64_t to_int(const Json& value);
// A math-module argument (math.cos(x)): an int, a float or a bool; "type": "must be real number, not str".
double real(const Json& value);

// sequence[i] with Python's negative indexes: a list's item, a str's character; "index": "list index out of range"
// ("string index out of range"); a dict raises KeyError (core::OpKeyError, as the ops report it).
Json at(const Json& sequence, std::int64_t index);
// len(x) for a list, a str (code points) or a dict.
std::size_t len(const Json& value);
// Python's x[a:b] of a list.
Json slice(const Json& list, std::int64_t start, std::int64_t stop);

// The error a Python exception of this kind would raise.
[[noreturn]] void raise_index(std::string_view message = "list index out of range");
[[noreturn]] void raise_attribute(const Json& value, std::string_view attribute);

// Whether a double is finite (the values the 3D ops store must be: a book never keeps NaN or an infinity).
bool finite(double x);

// `not text.strip()`: nothing but Python's whitespace (str.isspace).
bool blank(std::string_view text);

}  // namespace genko::core::pyv
>>>>>>> native/m3-3d
