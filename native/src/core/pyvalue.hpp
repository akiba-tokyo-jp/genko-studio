#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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
