#include "core/pynum.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "core/error.hpp"

namespace genko::core {

namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kExactInDouble = std::int64_t{1} << 53;

// --- checked int64 arithmetic (Python's ints do not overflow; ours fall back to a float) -----------------------

bool add_overflows(std::int64_t a, std::int64_t b) {
    return (b > 0 && a > kInt64Max - b) || (b < 0 && a < kInt64Min - b);
}

bool sub_overflows(std::int64_t a, std::int64_t b) {
    return (b < 0 && a > kInt64Max + b) || (b > 0 && a < kInt64Min + b);
}

bool mul_overflows(std::int64_t a, std::int64_t b) {
    if (a == 0 || b == 0) return false;
    if (a > 0) {
        if (b > 0) return a > kInt64Max / b;
        return b < kInt64Min / a;
    }
    if (b > 0) return a < kInt64Min / b;
    return b < kInt64Max / a;
}

// --- the digits of a double --------------------------------------------------------------------------------------

// The shortest digits that read back as |x| (no sign, no point) and the place of the decimal point: the value is
// 0.DIGITS × 10^decpt (Python's _Py_dg_dtoa mode 0).
struct ShortDigits {
    std::string digits;
    int decpt = 0;
};

ShortDigits shortest_digits(double x) {
    std::array<char, 64> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), std::fabs(x), std::chars_format::scientific);
    const std::string_view text(buf.data(), static_cast<std::size_t>(r.ptr - buf.data()));
    const auto e = text.find('e');
    ShortDigits out;
    for (const char c : text.substr(0, e)) {
        if (c != '.') out.digits += c;
    }
    std::string_view exp = text.substr(e + 1);
    bool negative = false;
    if (!exp.empty() && (exp.front() == '+' || exp.front() == '-')) {
        negative = exp.front() == '-';
        exp.remove_prefix(1);
    }
    int value = 0;
    std::from_chars(exp.data(), exp.data() + exp.size(), value);
    out.decpt = (negative ? -value : value) + 1;
    return out;
}

// The exact decimal expansion of a finite double: every digit before and after the point (a double's expansion
// always ends; 1074 places is enough for the smallest).
struct ExactDigits {
    bool negative = false;
    std::string digits;  // the whole part followed by the fraction
    std::size_t point = 0;  // digits before the point
};

ExactDigits exact_digits(double x) {
    static constexpr int kPlaces = 1100;
    std::string buf(400 + kPlaces, '\0');
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), x, std::chars_format::fixed, kPlaces);
    std::string_view text(buf.data(), static_cast<std::size_t>(r.ptr - buf.data()));
    ExactDigits out;
    if (!text.empty() && text.front() == '-') {
        out.negative = true;
        text.remove_prefix(1);
    }
    const auto dot = text.find('.');
    out.point = dot;
    out.digits.reserve(text.size());
    out.digits.append(text.substr(0, dot));
    out.digits.append(text.substr(dot + 1));
    return out;
}

double read_double(const std::string& text) {
    double value = 0.0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    if (r.ec == std::errc::result_out_of_range) {
        throw Error("value", "rounded value too large to represent");
    }
    return value;
}

}  // namespace

// --- repr and format --------------------------------------------------------------------------------------------

std::string py_float_repr(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    const ShortDigits sd = shortest_digits(x);
    const std::string& digits = sd.digits;
    const int n = static_cast<int>(digits.size());
    std::string out = std::signbit(x) ? "-" : "";
    int decpt = sd.decpt;
    // Python's float_repr_style 'short': an exponent below 1e-4 and from 1e16 up.
    if (decpt <= -4 || decpt > 16) {
        const int exponent = decpt - 1;
        out += digits[0];
        if (n > 1) {
            out += '.';
            out.append(digits, 1);
        }
        out += 'e';
        out += exponent < 0 ? '-' : '+';
        const int magnitude = exponent < 0 ? -exponent : exponent;
        if (magnitude < 10) out += '0';
        out += std::to_string(magnitude);
        return out;
    }
    if (decpt <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-decpt), '0');
        out += digits;
    } else if (decpt < n) {
        out.append(digits, 0, static_cast<std::size_t>(decpt));
        out += '.';
        out.append(digits, static_cast<std::size_t>(decpt));
    } else {
        out += digits;
        out.append(static_cast<std::size_t>(decpt - n), '0');
        out += ".0";
    }
    return out;
}

std::string py_format_g(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    std::array<char, 64> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), x, std::chars_format::general, 6);
    return std::string(buf.data(), r.ptr);
}

// --- round --------------------------------------------------------------------------------------------------------

double py_round(double x, int ndigits) {
    if (!std::isfinite(x)) return x;
    // CPython: past these, x rounds to itself or to a zero of its sign.
    if (ndigits > 323) return x;
    if (ndigits < -308) return 0.0 * x;
    const ExactDigits exact = exact_digits(x);
    const std::string& d = exact.digits;
    const long long cut = static_cast<long long>(exact.point) + ndigits;  // digits kept from the left
    std::string kept;
    bool up = false;
    if (cut >= 0) {
        const auto keep = static_cast<std::size_t>(std::min<long long>(cut, static_cast<long long>(d.size())));
        kept = d.substr(0, keep);
        if (keep < d.size()) {
            const char first = d[keep];
            if (first > '5') {
                up = true;
            } else if (first == '5') {
                const bool rest_zero = d.find_first_not_of('0', keep + 1) == std::string::npos;
                if (!rest_zero) {
                    up = true;
                } else {
                    const int last = kept.empty() ? 0 : kept.back() - '0';
                    up = (last % 2) == 1;  // a tie: to even
                }
            }
        }
    }
    if (up) {
        std::size_t k = kept.size();
        while (k > 0) {
            --k;
            if (kept[k] == '9') {
                kept[k] = '0';
            } else {
                ++kept[k];
                break;
            }
            if (k == 0) {
                kept.insert(kept.begin(), '1');
                break;
            }
        }
        if (kept.empty()) kept = "1";
    }
    if (kept.empty()) kept = "0";
    // kept × 10^(-ndigits), read back as the nearest double (the sign of x even for a zero, as Python's is)
    std::string text = exact.negative ? "-" : "";
    text += kept;
    text += 'e';
    text += std::to_string(-ndigits);
    return read_double(text);
}

Num py_round(const Num& x, int ndigits) {
    if (!x.is_int()) return Num(py_round(x.value(), ndigits));
    if (ndigits >= 0) return x;
    // An int rounded to tens, hundreds…: to the nearest multiple, ties to the even multiple (Python's
    // _PyLong_DivmodNear).
    if (ndigits < -18) return Num(0);
    std::int64_t unit = 1;
    for (int k = 0; k < -ndigits; ++k) unit *= 10;
    const std::int64_t v = x.int_value();
    std::int64_t q = v / unit;
    std::int64_t r = v % unit;
    if (r < 0) {  // floor division
        r += unit;
        --q;
    }
    const std::int64_t twice = r > kInt64Max / 2 ? kInt64Max : 2 * r;
    if (twice > unit || (twice == unit && (q % 2 != 0))) ++q;
    if (mul_overflows(q, unit)) return Num(static_cast<double>(q) * static_cast<double>(unit));
    return Num(q * unit);
}

// --- Num ----------------------------------------------------------------------------------------------------------

bool Num::same(const Num& other) const noexcept {
    if (is_int_ != other.is_int_) return false;
    if (is_int_) return int_ == other.int_;
    return std::bit_cast<std::uint64_t>(float_) == std::bit_cast<std::uint64_t>(other.float_);
}

Json Num::json() const {
    if (is_int_) return Json(int_);
    return Json(float_);
}

std::optional<Num> Num::from_json(const Json& value) {
    switch (value.type()) {
        case Json::value_t::number_integer: return Num(value.get<std::int64_t>());
        case Json::value_t::number_unsigned: {
            const auto u = value.get<std::uint64_t>();
            if (u > static_cast<std::uint64_t>(kInt64Max)) return std::nullopt;
            return Num(static_cast<std::int64_t>(u));
        }
        case Json::value_t::number_float: return Num(value.get<double>());
        default: return std::nullopt;
    }
}

std::string Num::repr() const { return is_int_ ? std::to_string(int_) : py_float_repr(float_); }

Num operator+(const Num& a, const Num& b) {
    if (a.is_int() && b.is_int() && !add_overflows(a.int_value(), b.int_value())) {
        return Num(a.int_value() + b.int_value());
    }
    return Num(a.value() + b.value());
}

Num operator-(const Num& a, const Num& b) {
    if (a.is_int() && b.is_int() && !sub_overflows(a.int_value(), b.int_value())) {
        return Num(a.int_value() - b.int_value());
    }
    return Num(a.value() - b.value());
}

Num operator*(const Num& a, const Num& b) {
    if (a.is_int() && b.is_int() && !mul_overflows(a.int_value(), b.int_value())) {
        return Num(a.int_value() * b.int_value());
    }
    return Num(a.value() * b.value());
}

Num operator/(const Num& a, const Num& b) {
    if (a.is_int() && b.is_int()) {
        if (b.int_value() == 0) throw Error("value", "division by zero");
        const std::int64_t x = a.int_value(), y = b.int_value();
        if (x > -kExactInDouble && x < kExactInDouble && y > -kExactInDouble && y < kExactInDouble) {
            return Num(static_cast<double>(x) / static_cast<double>(y));  // CPython's fast path: both exact
        }
        return Num(static_cast<double>(static_cast<long double>(x) / static_cast<long double>(y)));
    }
    if (b.value() == 0.0) throw Error("value", "float division by zero");
    return Num(a.value() / b.value());
}

Num operator-(const Num& a) {
    if (a.is_int() && a.int_value() != kInt64Min) return Num(-a.int_value());
    return Num(-a.value());
}

Num py_mod(const Num& a, const Num& b) {
    if (a.is_int() && b.is_int()) {
        const std::int64_t y = b.int_value();
        if (y == 0) throw Error("value", "integer modulo by zero");
        if (y == -1) return Num(0);
        std::int64_t r = a.int_value() % y;
        if (r != 0 && ((r < 0) != (y < 0))) r += y;
        return Num(r);
    }
    return Num(py_fmod(a.value(), b.value()));
}

Num py_abs(const Num& a) {
    if (a.is_int()) {
        if (a.int_value() == kInt64Min) return Num(-static_cast<double>(a.int_value()));
        return Num(a.int_value() < 0 ? -a.int_value() : a.int_value());
    }
    return Num(std::fabs(a.value()));
}

namespace {

// An int and a float compared exactly (CPython's float_richcompare).
std::partial_ordering compare_int_float(std::int64_t i, double f) {
    if (std::isnan(f)) return std::partial_ordering::unordered;
    if (i > -kExactInDouble && i < kExactInDouble) return static_cast<double>(i) <=> f;
    if (f >= 9223372036854775808.0) return std::partial_ordering::less;
    if (f < -9223372036854775808.0) return std::partial_ordering::greater;
    const double whole = std::trunc(f);
    const auto fi = static_cast<std::int64_t>(whole);
    if (i != fi) return i <=> fi;
    const double fraction = f - whole;
    if (fraction > 0) return std::partial_ordering::less;
    if (fraction < 0) return std::partial_ordering::greater;
    return std::partial_ordering::equivalent;
}

}  // namespace

std::partial_ordering operator<=>(const Num& a, const Num& b) {
    if (a.is_int() && b.is_int()) return a.int_value() <=> b.int_value();
    if (!a.is_int() && !b.is_int()) return a.value() <=> b.value();
    if (a.is_int()) return compare_int_float(a.int_value(), b.value());
    const auto reversed = compare_int_float(b.int_value(), a.value());
    if (reversed == std::partial_ordering::less) return std::partial_ordering::greater;
    if (reversed == std::partial_ordering::greater) return std::partial_ordering::less;
    return reversed;
}

bool operator==(const Num& a, const Num& b) { return (a <=> b) == std::partial_ordering::equivalent; }

Num py_sum(std::span<const Num> items) {
    // CPython 3.12 builtin_sum_impl with start=0: an exact int sum while the items are ints; from the first float
    // on (the int so far added to it plainly), floats are added with Neumaier's compensation and ints that fit a C
    // long plainly; an int beyond a C long adds the compensation and ends it (plain additions from there on).
    std::size_t k = 0;
    std::int64_t int_sum = 0;
    for (; k < items.size(); ++k) {
        const Num& item = items[k];
        if (!item.is_int() || add_overflows(int_sum, item.int_value())) break;
        int_sum += item.int_value();
    }
    if (k == items.size()) return Num(int_sum);
    double sum = (Num(int_sum) + items[k]).value();  // (an overflowing int: Python goes on exactly; ours cannot)
    double c = 0.0;
    bool compensating = items[k].is_int() == false;
    for (++k; k < items.size(); ++k) {
        const Num& item = items[k];
        if (!compensating) {
            sum += item.value();
            continue;
        }
        if (!item.is_int()) {
            const double x = item.value();
            const double t = sum + x;
            if (std::fabs(sum) >= std::fabs(x)) {
                c += (sum - t) + x;
            } else {
                c += (x - t) + sum;
            }
            sum = t;
            continue;
        }
        const std::int64_t v = item.int_value();
        if (v >= std::numeric_limits<long>::min() && v <= std::numeric_limits<long>::max()) {
            sum += static_cast<double>(v);
            continue;
        }
        if (c != 0.0 && std::isfinite(c)) sum += c;
        c = 0.0;
        compensating = false;
        sum += item.value();
    }
    if (compensating && c != 0.0 && std::isfinite(c)) sum += c;
    return Num(sum);
}

double py_float_sum(std::span<const double> items) {
    const std::vector<Num> nums(items.begin(), items.end());
    return py_sum(nums).value();
}

double py_median(std::vector<double> items) {
    if (items.empty()) throw Error("value", "no median for empty data");
    // sorted(data): a stable sort with <
    std::stable_sort(items.begin(), items.end(), [](double a, double b) { return a < b; });
    const std::size_t n = items.size();
    if (n % 2 == 1) return items[n / 2];
    const std::size_t i = n / 2;
    return (items[i - 1] + items[i]) / 2;
}

namespace {

void require_finite(double x) {
    if (std::isnan(x)) throw PyValueError("cannot convert float NaN to integer");
    if (std::isinf(x)) throw PyUncaught("OverflowError", "cannot convert float infinity to integer");
}

std::int64_t whole_to_int(double whole) {
    if (whole >= 9223372036854775808.0 || whole < -9223372036854775808.0) {
        throw PyUncaught("OverflowError", "int too large for this build");  // (Python's int has no bound)
    }
    return static_cast<std::int64_t>(whole);
}

}  // namespace

double py_round_whole(double x) {
    require_finite(x);
    return std::nearbyint(x);  // (ties to even, as round() of a float)
}

double py_trunc(double x) {
    require_finite(x);
    return std::trunc(x);
}

double py_floor(double x) {
    require_finite(x);
    return std::floor(x);
}

double py_ceil(double x) {
    require_finite(x);
    return std::ceil(x);
}

std::int64_t py_round_int(double x) { return whole_to_int(py_round_whole(x)); }

std::int64_t py_trunc_int(double x) { return whole_to_int(py_trunc(x)); }

std::int64_t py_trunc_held(double x) {
    const double whole = py_trunc(x);
    if (whole >= 9223372036854775808.0) return std::numeric_limits<std::int64_t>::max();
    if (whole < -9223372036854775808.0) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(whole);
}

}  // namespace genko::core
