#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/pynum.hpp"

// The Python values of every module ported from Python (the ops, the drawing, the rulers, the 3D guides): Python's
// conversions, iteration, indexing, unpacking and comparisons of JSON values as json.loads makes them, with Python's
// messages, and the exceptions Python raises (core/error.hpp) where it raises them.
//
// The CommandBus words the exceptions as apply_ops does:
//   OpError        (ApplyError)        "ops[i] <op>: <message> ‖ <usage>"
//   OpKeyError     (KeyError)          "ops[i] <op>: not found: <repr> (a key the op needs, …) ‖ <usage>"
//   PyValueError, PyTypeError and any other core::Error (ValueError, TypeError)
//                                      "ops[i] <op>: a value of the wrong type (<message>) ‖ <usage>"
//   PyUncaught     (IndexError, AttributeError, OverflowError, ZeroDivisionError, MemoryError, …: apply_ops does not
//                   catch them, so Python's command line stops with a traceback; this build reports them with the code
//                   "python_error", the error ending with the traceback's last line "<type>: <message>")
//   core::Error("not_yet_ported")      "ops[i] <op>: <message>", code not_yet_ported

namespace genko::core {

// (PyValueError, PyTypeError and PyUncaught: core/error.hpp)

// core::Error("not_yet_ported", message): a part of an op that this build does not have yet.
[[noreturn]] void not_yet_ported(const std::string& message);

// Python's IndexError ("list index out of range", …) and AttributeError ("'list' object has no attribute 'get'"),
// which apply_ops lets through: PyUncaught.
[[noreturn]] void raise_index_error(std::string_view message = "list index out of range");
[[noreturn]] void raise_attribute_error(const Json& value, std::string_view attribute);

// op.get(key): the value, or null when the key is missing (or the value is not a dict: for an op, which always is).
const Json* get(const Json& object, std::string_view key);
// op.get(key, fallback)
Json get_or(const Json& object, std::string_view key, const Json& fallback);
// key in op
bool has(const Json& object, std::string_view key);
// bool(op.get(key))
bool truthy_at(const Json& object, std::string_view key);

// x.get(key) for a value that Python takes for a dict (a ruler, a tone, a prim, …): the value or null when the key is
// missing; AttributeError (PyUncaught "'list' object has no attribute 'get'") when x is not a dict.
const Json* dict_get(const Json& object, std::string_view key);
// x.get(key, fallback), likewise.
Json py_get(const Json& object, std::string_view key, const Json& fallback = Json());
// `x.get(key) or fallback`, likewise: the value when it is truthy, else `fallback` (a reference to either: neither may
// be a temporary).
const Json& get_else(const Json& object, std::string_view key, const Json& fallback);
const Json& get_else(const Json& object, std::string_view key, Json&& fallback) = delete;
const Json& get_else(Json&& object, std::string_view key, const Json& fallback) = delete;
// `value or fallback`.
Json py_or(const Json& value, const Json& fallback);

// float(x): PyValueError for a str that is not a number, PyTypeError for other values that are not numbers.
double to_float(const Json& value);
// int(x): PyValueError for a str that is not an int, PyTypeError for other values that are not numbers. A float NaN
// is PyValueError, an infinity PyUncaught OverflowError (as int() raises them); an int past 64 bits is refused
// ("too large for this build": Python's int has no bound).
std::int64_t to_int(const Json& value);
// int(x) of a value used as a count or a bound (copies of a ruler, lines of an effect, a grid's lines): as to_int,
// but an int past 64 bits (a float like 1e300, a long text of digits) is held at the end of the int64 range on its
// side, which is compared and refused as Python's unbounded int would be (core::py_trunc_held).
std::int64_t to_int_held(const Json& value);
// The number as Python keeps it (an int stays an int; a bool is an int); PyTypeError for anything else.
Num to_num(const Json& value);
// A math-module argument (math.cos(x)): an int, a float or a bool; PyTypeError "must be real number, not str".
double to_real(const Json& value);

// for item in x: a list's items, a str's characters, a dict's keys; PyTypeError "'int' object is not iterable".
std::vector<Json> iterate(const Json& value);
// len(x): a list's items, a str's characters (code points), a dict's keys; PyTypeError "object of type 'int' has no
// len()".
std::size_t length(const Json& value);
// x[i] for an int i (negative from the end): a list's item or a str's character (PyUncaught IndexError when out of
// range), a dict's value for the key i (OpKeyError), PyTypeError "'int' object is not subscriptable".
Json subscript(const Json& value, std::int64_t index);
// x[key] for a str key: a dict's value (OpKeyError when missing); PyTypeError for a list ("list indices must be
// integers or slices, not str"), a str ("string indices must be integers, not 'str'") and other values.
Json subscript(const Json& value, std::string_view key);
// x[start:stop] of a list (Python's slice: negative from the end, clamped to the list).
Json py_slice(const Json& list, std::int64_t start, std::int64_t stop);

// a, b, … = (float(v) for v in x): the items converted one by one; Python's unpacking errors ("not enough values to
// unpack (expected 4, got 3)", "too many values to unpack (expected 4)").
std::vector<double> unpack_floats(const Json& value, std::size_t expected);

// tuple(int(v) for v in x) (any length; Python's errors).
std::vector<std::int64_t> int_tuple(const Json& value);

// a, b, … = x (the values as they are, unconverted): PyTypeError "cannot unpack non-iterable int object", PyValueError
// "not enough values to unpack (expected 2, got 1)" / "too many values to unpack (expected 2)".
std::vector<Json> unpack_values(const Json& value, std::size_t expected);

// Python's `a < b` (and <=, >, >=) for JSON numbers and bools; PyTypeError "'<' not supported between instances of
// 'int' and 'str'" for other values.
bool py_less(const Json& a, const Json& b, std::string_view op = "<");

// The value as a hashable key (dict lookups, set and frozenset membership): PyTypeError "unhashable type: 'list'"
// for a list or a dict.
void require_hashable(const Json& value);

// x == y as Python compares JSON values (1 == 1.0 == True; a str is never a number; lists item by item, dicts key by
// key in any order).
bool py_equals(const Json& a, const Json& b);
// `value in ("a", "b", …)` for names: a str that is one of them.
bool is_one_of(const Json& value, std::initializer_list<std::string_view> names);
// `not text.strip()`: nothing but Python's whitespace (str.isspace).
bool is_blank(std::string_view text);

// round(x, n) for a JSON number (an int stays an int).
Json round_json(const Json& value, int ndigits);

// int(x) for a float that counts the steps of a loop (points along a line, …). Python's int is unbounded, so a loop
// over 1e300 steps runs until Python has no memory left: past limits::kLoopSteps this is PyUncaught("MemoryError").
// NaN is Python's ValueError (PyValueError); an infinity its OverflowError, which apply_ops does not catch.
std::int64_t loop_count(double steps);

// The JSON of a number list (ints stay ints).
Json nums_json(const std::vector<Num>& values);

// str.strip(): the text without Python's whitespace (str.isspace: ASCII spaces, \x1c-\x1f, \x85, \xa0, U+1680,
// U+2000-U+200A, U+2028, U+2029, U+202F, U+205F, U+3000) at either end.
std::string py_strip(std::string_view text);

// float(op[key]) for a number that is drawn with or stored: NaN and the infinities (float("nan"), "inf", 1e999 read
// as a string) are refused with OpError "<key> must be a finite number", where Python goes on and draws nothing, hangs
// or writes NaN into the book. Python's errors of float() otherwise.
double finite_float(const Json& value, std::string_view key);

// int(x) as the whole number it is, as a double (Python's int has no bound: a text of digits past 64 bits, a float
// past 2**63, compared and clamped as Python compares them). int()'s errors; NaN and the infinities refused as
// finite_float refuses them.
double int_whole(const Json& value, std::string_view key);

}  // namespace genko::core
