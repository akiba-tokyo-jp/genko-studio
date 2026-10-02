// numpy on OpenBLAS (see linalg3.hpp) and the math functions the 3D guides use. Compiled with -fno-builtin and kept
// out of line, as pymath.cpp is: libm is called as CPython and numpy call it, and the fused multiply-adds are the
// explicit std::fma calls below (the build never contracts).

#include "core/linalg3.hpp"

#include <cfloat>
#include <cmath>

#include "core/error.hpp"

namespace genko::core::la {

namespace {

// dgemm: one element, a fused chain from +0.0
template <std::size_t K, typename RowA, typename ColB>
double gemm_element(const RowA& a, const ColB& b) {
    double s = 0.0;
    for (std::size_t k = 0; k < K; ++k) s = std::fma(a[k], b[k], s);
    return s;
}

// dgemv, 3 rows: y = 0 + (a2 x2 + (a0 x0 + a1 x1)), the two outer sums fused
double gemv3_element(const Vec3& a, const Vec3& x) { return 0.0 + std::fma(a[2], x[2], std::fma(a[0], x[0], a[1] * x[1])); }

// dgemv, 4 rows: two pairs added, not fused
double gemv4_element(const Vec4& a, const Vec4& x) {
    const double p0 = a[0] * x[0];
    const double p1 = a[1] * x[1];
    const double p2 = a[2] * x[2];
    const double p3 = a[3] * x[3];
    return 0.0 + ((p0 + p2) + (p1 + p3));
}

struct DoubleLength {
    double hi;
    double lo;
};

DoubleLength dl_fast_sum(double a, double b) {
    const double x = a + b;
    const double y = (a - x) + b;
    return {x, y};
}

DoubleLength dl_mul(double x, double y) {
    const double z = x * y;
    return {z, std::fma(x, y, -z)};
}

// CPython's vector_norm (Modules/mathmodule.c), as core::py_hypot computes it for two numbers.
double vector_norm(int n, double* vec, double max, bool found_nan) {
    if (std::isinf(max)) return max;
    if (found_nan) return NAN;
    if (max == 0.0 || n <= 1) return max;
    int max_e = 0;
    std::frexp(max, &max_e);
    if (max_e < -1023) {
        for (int i = 0; i < n; ++i) vec[i] /= DBL_MIN;
        return DBL_MIN * vector_norm(n, vec, max / DBL_MIN, found_nan);
    }
    const double scale = std::ldexp(1.0, -max_e);
    double csum = 1.0, frac1 = 0.0, frac2 = 0.0;
    for (int i = 0; i < n; ++i) {
        double x = vec[i];
        x *= scale;
        const DoubleLength pr = dl_mul(x, x);
        const DoubleLength sm = dl_fast_sum(csum, pr.hi);
        csum = sm.hi;
        frac1 += pr.lo;
        frac2 += sm.lo;
    }
    double h = std::sqrt(csum - 1.0 + (frac1 + frac2));
    const DoubleLength pr = dl_mul(-h, h);
    const DoubleLength sm = dl_fast_sum(csum, pr.hi);
    csum = sm.hi;
    frac1 += pr.lo;
    frac2 += sm.lo;
    const double x = csum - 1.0 + (frac1 + frac2);
    h += x / (2.0 * h);
    return h / scale;
}

}  // namespace

Mat3 identity3() { return Mat3{Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0}, Vec3{0.0, 0.0, 1.0}}; }

Mat4 identity4() {
    Mat4 m{};
    for (int i = 0; i < 4; ++i) m[i][i] = 1.0;
    return m;
}

Mat3 transpose(const Mat3& m) {
    Mat3 t{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) t[i][j] = m[j][i];
    }
    return t;
}

Mat4 transpose(const Mat4& m) {
    Mat4 t{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) t[i][j] = m[j][i];
    }
    return t;
}

Mat3 matmul(const Mat3& a, const Mat3& b) {
    const Mat3 bt = transpose(b);
    Mat3 c{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) c[i][j] = gemm_element<3>(a[i], bt[j]);
    }
    return c;
}

Mat4 matmul(const Mat4& a, const Mat4& b) {
    const Mat4 bt = transpose(b);
    Mat4 c{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) c[i][j] = gemm_element<4>(a[i], bt[j]);
    }
    return c;
}

Vec3 matvec(const Mat3& a, const Vec3& x) { return {gemv3_element(a[0], x), gemv3_element(a[1], x), gemv3_element(a[2], x)}; }

Vec4 matvec(const Mat4& a, const Vec4& x) {
    return {gemv4_element(a[0], x), gemv4_element(a[1], x), gemv4_element(a[2], x), gemv4_element(a[3], x)};
}

std::vector<Vec3> rows_mt(std::span<const Vec3> rows, const Mat3& m) {
    std::vector<Vec3> out(rows.size());
    if (rows.size() == 1) {
        out[0] = matvec(m, rows[0]);
        return out;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        for (int j = 0; j < 3; ++j) out[i][j] = gemm_element<3>(rows[i], m[j]);
    }
    return out;
}

std::vector<Vec4> rows_mt(std::span<const Vec4> rows, const Mat4& m) {
    std::vector<Vec4> out(rows.size());
    if (rows.size() == 1) {
        out[0] = matvec(m, rows[0]);
        return out;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        for (int j = 0; j < 4; ++j) out[i][j] = gemm_element<4>(rows[i], m[j]);
    }
    return out;
}

double dot(const Vec3& a, const Vec3& b) {
    const double p0 = a[0] * b[0];
    const double p1 = a[1] * b[1];
    const double p2 = a[2] * b[2];
    return 0.0 + ((p0 + p1) + p2);
}

double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }

double norm2(double x, double y) {
    const double p0 = x * x;
    const double p1 = y * y;
    return std::sqrt(0.0 + (p0 + p1));
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vec3 column(const Mat3& m, int j) { return {m[0][j], m[1][j], m[2][j]}; }

double np_maximum(double a, double b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    return a > b ? a : b;
}

double np_minimum(double a, double b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    return a < b ? a : b;
}

double np_hypot(double x, double y) { return std::hypot(x, y); }

double py_hypot3(double x, double y, double z) {
    double coordinates[3] = {std::fabs(x), std::fabs(y), std::fabs(z)};
    bool found_nan = false;
    double max = 0.0;
    for (const double c : coordinates) {
        found_nan |= std::isnan(c);
        if (c > max) max = c;
    }
    return vector_norm(3, coordinates, max, found_nan);
}

double py_exp(double x) {
    const double r = std::exp(x);
    if (std::isinf(r) && std::isfinite(x)) throw Error("overflow", "math range error");
    return r;
}

double py_log(double x) {
    if (std::isnan(x)) return x;
    if (x > 0.0) return std::log(x);  // (log(inf) is inf, as in Python)
    throw Error("value", "math domain error");
}

double py_remainder(double x, double y) {
    // CPython's m_remainder
    if (std::isfinite(x) && std::isfinite(y)) {
        if (y == 0.0) throw Error("value", "math domain error");
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
    if (std::isinf(x)) throw Error("value", "math domain error");
    return x;
}

double py_floor_int(double x) {
    if (std::isnan(x)) throw Error("value", "cannot convert float NaN to integer");
    if (std::isinf(x)) throw Error("overflow", "cannot convert float infinity to integer");
    return std::floor(x);
}

double py_ceil_int(double x) {
    if (std::isnan(x)) throw Error("value", "cannot convert float NaN to integer");
    if (std::isinf(x)) throw Error("overflow", "cannot convert float infinity to integer");
    return std::ceil(x);
}

}  // namespace genko::core::la
