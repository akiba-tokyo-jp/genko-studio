// The math functions as CPython 3.12 computes them (Modules/mathmodule.c, Objects/floatobject.c), with its errors.
// Every call of libm that stands for one of Python's math functions is here (numpy's float32 functions are
// render/npcompat's, numpy's float64 hypot core/linalg3's).
//
// This file is compiled with -fno-builtin and kept out of line: the compiler must call the platform's libm exactly
// as CPython does, never fold pow(x, 2.0) into x*x or merge sin and cos into sincos, so the last bit of every
// result is the one the Python build on the same system gets. (MSVC: /fp:precise and the intrinsics turned off with
// #pragma function below.)

#include <cfloat>
#include <cmath>

#include "core/error.hpp"
#include "core/pynum.hpp"

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(sin, cos, tan, atan2, exp, log, sqrt)
#endif

namespace genko::core {

namespace {

struct DoubleLength {
    double hi;
    double lo;
};

DoubleLength dl_fast_sum(double a, double b) {
    // Algorithm 1.1 (Ogita, Rump, Oishi): compensated sum of two numbers with |a| >= |b|.
    const double x = a + b;
    const double y = (a - x) + b;
    return {x, y};
}

DoubleLength dl_mul(double x, double y) {
    // Algorithm 3.5: the exact error of a product (CPython uses fma(); Dekker's split gives the same bits).
    const double z = x * y;
    const double zz = std::fma(x, y, -z);
    return {z, zz};
}

// CPython's vector_norm(): the length of a vector, correctly rounded in nearly every case.
double vector_norm(int n, double* vec, double max, bool found_nan) {
    if (std::isinf(max)) return max;
    if (found_nan) return NAN;
    if (max == 0.0 || n <= 1) return max;
    int max_e = 0;
    std::frexp(max, &max_e);
    if (max_e < -1023) {
        // ldexp(1.0, -max_e) would overflow: make subnormals normal first.
        for (int i = 0; i < n; ++i) vec[i] /= DBL_MIN;
        return DBL_MIN * vector_norm(n, vec, max / DBL_MIN, found_nan);
    }
    const double scale = std::ldexp(1.0, -max_e);
    double csum = 1.0, frac1 = 0.0, frac2 = 0.0;
    for (int i = 0; i < n; ++i) {
        double x = vec[i];
        x *= scale;                         // lossless scaling
        DoubleLength pr = dl_mul(x, x);     // lossless squaring
        DoubleLength sm = dl_fast_sum(csum, pr.hi);  // lossless addition
        csum = sm.hi;
        frac1 += pr.lo;                     // lossy addition
        frac2 += sm.lo;                     // lossy addition
    }
    double h = std::sqrt(csum - 1.0 + (frac1 + frac2));
    DoubleLength pr = dl_mul(-h, h);
    DoubleLength sm = dl_fast_sum(csum, pr.hi);
    csum = sm.hi;
    frac1 += pr.lo;
    frac2 += sm.lo;
    const double x = csum - 1.0 + (frac1 + frac2);
    h += x / (2.0 * h);  // differential correction
    return h / scale;
}

bool is_odd_integer(double x) { return std::fmod(std::fabs(x), 2.0) == 1.0; }

// math.hypot and math.dist of n coordinates (their absolute values): vector_norm with its NaN and max.
double norm_of(int n, double* coordinates) {
    bool found_nan = false;
    double max = 0.0;
    for (int i = 0; i < n; ++i) {
        found_nan = found_nan || std::isnan(coordinates[i]);
        if (coordinates[i] > max) max = coordinates[i];
    }
    return vector_norm(n, coordinates, max, found_nan);
}

// CPython's math_1(x, func, can_overflow=0) for the result r of a libm function at x: a NaN from what is not NaN (the
// sine of an infinity), or an infinity from a finite x, is ValueError "math domain error".
double math_1(double x, double r) {
    if (std::isnan(r) && !std::isnan(x)) throw PyValueError("math domain error");
    if (std::isinf(r) && std::isfinite(x)) throw PyValueError("math domain error");
    return r;
}

}  // namespace

double py_hypot(double x, double y) {
    double coordinates[2] = {std::fabs(x), std::fabs(y)};
    return norm_of(2, coordinates);
}

double py_hypot(double x, double y, double z) {
    double coordinates[3] = {std::fabs(x), std::fabs(y), std::fabs(z)};
    return norm_of(3, coordinates);
}

double py_dist(double ax, double ay, double bx, double by) {
    double diffs[2] = {std::fabs(ax - bx), std::fabs(ay - by)};
    return norm_of(2, diffs);
}

double py_sin(double x) { return math_1(x, std::sin(x)); }
double py_cos(double x) { return math_1(x, std::cos(x)); }
double py_tan(double x) { return math_1(x, std::tan(x)); }

double py_acos(double x) {
    if (x < -1.0 || x > 1.0) throw PyValueError("math domain error");
    return std::acos(x);
}

double py_exp(double x) {
    const double r = std::exp(x);  // (math_1 with can_overflow: an infinity from a finite x is OverflowError)
    if (std::isinf(r) && std::isfinite(x)) throw PyUncaught("OverflowError", "math range error");
    return r;
}

double py_log(double x) {
    // CPython's m_log: NaN and +inf as they are, log(0) and log(-x) a domain error
    if (std::isnan(x)) return x;
    if (x > 0.0) return std::log(x);
    throw PyValueError("math domain error");
}

double py_remainder(double x, double y) {
    // CPython's m_remainder (the exact IEEE remainder), with math_2's ValueError for its NaN results
    if (std::isfinite(x) && std::isfinite(y)) {
        if (y == 0.0) throw PyValueError("math domain error");
        const double absx = std::fabs(x);
        const double absy = std::fabs(y);
        const double m = std::fmod(absx, absy);
        const double c = absy - m;
        double r = 0.0;
        if (m < c) {
            r = m;
        } else if (m > c) {
            r = -c;
        } else {
            r = m - 2.0 * std::fmod(0.5 * (absx - m), absy);
        }
        return std::copysign(1.0, x) * r;
    }
    if (std::isnan(x)) return x;
    if (std::isnan(y)) return y;
    if (std::isinf(x)) throw PyValueError("math domain error");
    return x;  // (y is an infinity)
}

double py_atan2(double y, double x) {
    // CPython's m_atan2: the special cases first, then libm.
    if (std::isnan(x) || std::isnan(y)) return NAN;
    if (std::isinf(y)) {
        if (std::isinf(x)) {
            if (std::copysign(1.0, x) == 1.0) return std::copysign(0.25 * kPi, y);
            return std::copysign(0.75 * kPi, y);
        }
        return std::copysign(0.5 * kPi, y);
    }
    if (std::isinf(x) || y == 0.0) {
        if (std::copysign(1.0, x) == 1.0) return std::copysign(0.0, y);
        return std::copysign(kPi, y);
    }
    return std::atan2(y, x);
}

double py_pow(double iv, double iw) {
    // CPython's float_pow.
    if (iw == 0) return 1.0;
    if (std::isnan(iv)) return iv;
    if (std::isnan(iw)) return iv == 1.0 ? 1.0 : iw;
    if (std::isinf(iw)) {
        iv = std::fabs(iv);
        if (iv == 1.0) return 1.0;
        if ((iw > 0.0) == (iv > 1.0)) return std::fabs(iw);
        return 0.0;
    }
    if (std::isinf(iv)) {
        const bool odd = is_odd_integer(iw);
        if (iw > 0.0) return odd ? iv : std::fabs(iv);
        return odd ? std::copysign(0.0, iv) : 0.0;
    }
    if (iv == 0.0) {
        const bool odd = is_odd_integer(iw);
        if (iw < 0.0) throw Error("value", "0.0 cannot be raised to a negative power");
        return odd ? iv : 0.0;
    }
    bool negate = false;
    if (iv < 0.0) {
        if (iw != std::floor(iw)) throw Error("value", "a negative number raised to a fractional power is complex");
        iv = -iv;
        negate = is_odd_integer(iw);
    }
    if (iv == 1.0) return negate ? -1.0 : 1.0;
    const double ix = std::pow(iv, iw);
    if (std::isinf(ix)) throw Error("value", "(34, 'Numerical result out of range')");
    return negate ? -ix : ix;
}

double py_fmod(double vx, double wx) {
    // CPython's float_rem: the remainder takes the sign of the divisor.
    if (wx == 0.0) throw Error("value", "float modulo");
    double mod = std::fmod(vx, wx);
    if (mod != 0.0) {
        if ((wx < 0) != (mod < 0)) mod += wx;
    } else {
        mod = std::copysign(0.0, wx);
    }
    return mod;
}

}  // namespace genko::core
