#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/json.hpp"

namespace genko::core {

// Python's repr(float): the shortest digits that read back as x, written as Python writes them: "1.0", "0.35",
// "100.0", "1e-05", "1e+16", "-0.0", "nan", "inf". (JSON output refuses the last two: see core::dump.)
std::string py_float_repr(double x);

// Python's format(x, "g") (as in f"{x:g}"): six significant digits, no trailing zeros ("182", "0.35",
// "1.23457e+06").
std::string py_format_g(double x);

// Python 3's round(x, ndigits) for a float: x rounded at ndigits decimal places (negative: tens, hundreds, …)
// from its exact binary value, ties to even, read back as the nearest double. NaN and infinities come back
// unchanged; a result too large for a double throws core::Error("value") as Python's OverflowError does.
double py_round(double x, int ndigits);

// A number as Python holds one: an int or a float. project.json keeps them apart ("13" and "13.0") and Python's
// arithmetic keeps ints exact (int + int is an int; "/" always makes a float). Fields that Python keeps as they
// were read (rectangles, the paper, page numbers, …) are Nums, so a book is written back exactly as it was read.
//
// Num(3) is the int 3 and Num(3.0) the float 3.0, as the same literals are in Python. Ints are 64-bit (Python's
// are unbounded): int arithmetic that would overflow gives the nearest float instead.
class Num {
public:
    constexpr Num() noexcept = default;  // the int 0
    template <std::integral T>
        requires(!std::same_as<T, bool>)
    constexpr Num(T v) noexcept : int_(static_cast<std::int64_t>(v)), is_int_(true) {}
    template <std::floating_point T>
    constexpr Num(T v) noexcept : float_(static_cast<double>(v)), is_int_(false) {}
    Num(bool) = delete;

    constexpr bool is_int() const noexcept { return is_int_; }
    // The int (only for ints).
    constexpr std::int64_t int_value() const noexcept { return int_; }
    // Python's float(x).
    constexpr double value() const noexcept { return is_int_ ? static_cast<double>(int_) : float_; }
    // Python's bool(x).
    constexpr bool truthy() const noexcept { return is_int_ ? int_ != 0 : float_ != 0.0; }
    // The same type and the same value (floats bit for bit), unlike ==, which compares values as Python does.
    bool same(const Num& other) const noexcept;

    Json json() const;
    // An int or a float from JSON; nothing for other values (booleans included) and for ints beyond int64.
    static std::optional<Num> from_json(const Json& value);
    // Python's str(x) / repr(x).
    std::string repr() const;

private:
    std::int64_t int_ = 0;
    double float_ = 0.0;
    bool is_int_ = true;
};

Num operator+(const Num& a, const Num& b);
Num operator-(const Num& a, const Num& b);
Num operator*(const Num& a, const Num& b);
// Python's a / b: always a float. Division by zero throws core::Error("value").
Num operator/(const Num& a, const Num& b);
Num operator-(const Num& a);
// Python's a % b: the result has the sign of b. Throws core::Error("value") when b is zero.
Num py_mod(const Num& a, const Num& b);
Num py_abs(const Num& a);
// Python's round(x, ndigits): an int stays an int (rounded to tens, hundreds… for negative ndigits).
Num py_round(const Num& x, int ndigits);
// Exact comparison, as Python compares ints and floats with each other (1 == 1.0).
std::partial_ordering operator<=>(const Num& a, const Num& b);
bool operator==(const Num& a, const Num& b);

// Python 3.12's sum(items): ints are added exactly; once a float is met, floats are added with Neumaier's
// compensation and ints plainly, as CPython 3.12 does (the last bit of a centroid depends on it).
Num py_sum(std::span<const Num> items);
// sum(items) of floats (from the int 0: the first added plainly, so -0.0 becomes 0.0): py_sum of them.
double py_float_sum(std::span<const double> items);

// statistics.median of floats (core::Error("value") when there are none).
double py_median(std::vector<double> items);

// Python's min(a, b) and max(a, b) for floats: the first unless the second is smaller (larger). NaN as Python has
// it: min(1.0, nan) is 1.0, min(nan, 1.0) is nan.
inline double py_min(double a, double b) { return b < a ? b : a; }
inline double py_max(double a, double b) { return b > a ? b : a; }
// max(lo, min(hi, v))
inline double py_clamp(double v, double lo, double hi) { return py_max(lo, py_min(hi, v)); }

// Python's round(x) and int(x) for a float, as an int (round: ties to even; int: towards zero). Python's exceptions
// (core/error.hpp): PyValueError for NaN, PyUncaught OverflowError for an infinity; and OverflowError past 64 bits,
// where Python's int has no bound.
std::int64_t py_round_int(double x);
std::int64_t py_trunc_int(double x);
// round(x) and int(x) as the whole float each is (unbounded, as Python's int: for comparing and clamping), with
// their errors.
double py_round_whole(double x);
double py_trunc(double x);

// The math module as CPython 3.12 computes it: hypot and dist with CPython's own correctly rounded algorithm,
// the rest with the platform's libm called as Python calls it (never folded or merged by the compiler, so the
// last bit matches the Python build on the same OS).
double py_hypot(double x, double y);
double py_dist(double ax, double ay, double bx, double by);
double py_sin(double x);
double py_cos(double x);
double py_tan(double x);
double py_acos(double x);
double py_atan2(double y, double x);
// Python's float ** float (its special cases, then libm pow). Throws core::Error("value") where Python raises.
double py_pow(double x, double y);
// Python's float % float.
double py_fmod(double x, double y);

inline constexpr double kPi = 3.141592653589793;  // math.pi

}  // namespace genko::core
