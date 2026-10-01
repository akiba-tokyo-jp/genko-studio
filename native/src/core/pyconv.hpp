#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/json.hpp"

namespace genko::core {

// Python's built-in conversions applied to values as json.loads makes them (None, bool, int, float, str, list,
// dict). The reader uses them where Python's reader converts (float(…), int(…), str(…), bool(…), list(…),
// dict(…)), so a C++ load succeeds or fails exactly where the Python one does. Failures throw
// core::Error("format") with Python's TypeError/ValueError message.

// Python's type name: "NoneType", "bool", "int", "float", "str", "list", "dict".
std::string py_type_name(const Json& value);

// bool(x): None, False, 0, 0.0, "", [] and {} are false.
bool py_truthy(const Json& value);

// float(x): numbers, booleans and numeric strings (" 1.5 ", "1_000", "inf", "nan").
double py_float(const Json& value);

// int(x): ints, floats (truncated), booleans and decimal strings.
std::int64_t py_int(const Json& value);

// str(x): a str as it is, numbers as Python prints them, True/False/None, lists and dicts as Python's repr.
std::string py_str(const Json& value);

// repr(x).
std::string py_repr(const Json& value);
std::string py_repr_str(std::string_view text);

// list(x) / tuple(x): a list stays as it is, a str becomes its characters, a dict its keys.
Json py_list(const Json& value);

// dict(x): a dict, or a list of [key, value] pairs with str keys.
Json py_dict(const Json& value);

}  // namespace genko::core
