#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/pynum.hpp"

// What the op implementations need to behave exactly as Python's _apply_one does with the JSON it is given: the
// exceptions Python raises (and which of them apply_ops catches), and Python's conversions, iteration, indexing and
// unpacking of JSON values, with Python's messages.
//
// The CommandBus words the exceptions as apply_ops does:
//   OpError        (ApplyError)        "ops[i] <op>: <message> ‖ <usage>"
//   OpKeyError     (KeyError)          "ops[i] <op>: not found: <repr> (a key the op needs, …) ‖ <usage>"
//   PyValueError, PyTypeError and any other core::Error (ValueError, TypeError)
//                                      "ops[i] <op>: a value of the wrong type (<message>) ‖ <usage>"
//   PyUncaught     (IndexError, AttributeError, OverflowError, …: apply_ops does not catch them, so Python stops
//                   with a traceback; this build reports them as an error with the code "python_error")
//   core::Error("not_yet_ported")      "ops[i] <op>: <message>", code not_yet_ported

namespace genko::core {

// (PyValueError, PyTypeError and PyUncaught: core/error.hpp)

// core::Error("not_yet_ported", message): a part of an op that this build does not have yet.
[[noreturn]] void not_yet_ported(const std::string& message);

// op.get(key): the value, or null when the key is missing.
const Json* get(const Json& object, std::string_view key);
// op.get(key, fallback)
Json get_or(const Json& object, std::string_view key, const Json& fallback);
// key in op
bool has(const Json& object, std::string_view key);
// bool(op.get(key))
bool truthy_at(const Json& object, std::string_view key);

// float(x): PyValueError for a str that is not a number, PyTypeError for other values that are not numbers.
double to_float(const Json& value);
// int(x): PyValueError for a str that is not an int, PyTypeError for other values that are not numbers.
std::int64_t to_int(const Json& value);
// The number as Python keeps it (an int stays an int; a bool is an int); PyTypeError for anything else.
Num to_num(const Json& value);

// for item in x: a list's items, a str's characters, a dict's keys; PyTypeError "'int' object is not iterable".
std::vector<Json> iterate(const Json& value);
// len(x): PyTypeError "object of type 'int' has no len()".
std::size_t length(const Json& value);
// x[i] for an int i: a list's item or a str's character (PyUncaught IndexError when out of range), a dict's value
// for the key i (OpKeyError), PyTypeError "'int' object is not subscriptable".
Json subscript(const Json& value, std::int64_t index);
// x[key] for a str key: a dict's value (OpKeyError when missing); PyTypeError for a list ("list indices must be
// integers or slices, not str"), a str ("string indices must be integers, not 'str'") and other values.
Json subscript(const Json& value, std::string_view key);

// a, b, … = (float(v) for v in x): the items converted one by one; Python's unpacking errors ("not enough values to
// unpack (expected 4, got 3)", "too many values to unpack (expected 4)").
std::vector<double> unpack_floats(const Json& value, std::size_t expected);

// tuple(int(v) for v in x) (any length; Python's errors).
std::vector<std::int64_t> int_tuple(const Json& value);

// Python's `a < b` (and <=, >, >=) for JSON numbers and bools; PyTypeError "'<' not supported between instances of
// 'int' and 'str'" for other values.
bool py_less(const Json& a, const Json& b, std::string_view op = "<");

// The value as a hashable key (dict lookups, set and frozenset membership): PyTypeError "unhashable type: 'list'"
// for a list or a dict.
void require_hashable(const Json& value);

// x == y as Python compares JSON values (1 == 1.0 == True; a str is never a number).
bool py_equals(const Json& a, const Json& b);

// round(x, n) for a JSON number (an int stays an int).
Json round_json(const Json& value, int ndigits);

// int(x) for a float that counts the steps of a loop (points along a line, …). Python's int is unbounded, so a loop
// over 1e300 steps runs until Python has no memory left: past 10 million steps this is PyUncaught("MemoryError").
// NaN is Python's ValueError (PyValueError); an infinity its OverflowError, which apply_ops does not catch.
std::int64_t loop_count(double steps);

// The JSON of a number list (ints stay ints).
Json nums_json(const std::vector<Num>& values);

}  // namespace genko::core
