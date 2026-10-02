#pragma once

#include <array>
#include <span>
#include <vector>

// numpy's arithmetic on the small vectors and matrices of the 3D guides (genko/mesh3d.py, prim3d.py, persp3d.py),
// to the last bit of the Python baseline (numpy 2.4 with scipy-openblas 0.3.31 and its Haswell kernels).
//
// Element-wise operations are plain IEEE operations, one rounding each, never contracted. matmul goes to OpenBLAS,
// whose kernels round in their own orders (measured with tools/migration/geom3d_harness.py blas; the contract tests
// check them on the reference machine first):
//  - a matrix product (dgemm), any size: each element is a fused multiply-add chain from +0.0 over k;
//  - a 3×3 matrix times a vector (dgemv, the 3-row tail of the transposed kernel): 0.0 + fma(a2, x2, fma(a0, x0, a1 x1));
//  - a 4×4 matrix times a vector (its 4-row block): 0.0 + ((a0 x0 + a2 x2) + (a1 x1 + a3 x3)), not fused;
//  - a dot product of two vectors (ddot), and np.linalg.norm: 0.0 + ((a0 b0 + a1 b1) + a2 b2), not fused.
// A product with one row ((1, 3) @ M.T) is a matrix times a vector to numpy; with more rows it is a matrix product.

namespace genko::core::la {

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<Vec3, 3>;  // rows
using Vec4 = std::array<double, 4>;
using Mat4 = std::array<Vec4, 4>;

Mat3 identity3();
Mat4 identity4();
Mat3 transpose(const Mat3& m);
Mat4 transpose(const Mat4& m);

// a @ b
Mat3 matmul(const Mat3& a, const Mat3& b);
Mat4 matmul(const Mat4& a, const Mat4& b);
// a @ x
Vec3 matvec(const Mat3& a, const Vec3& x);
Vec4 matvec(const Mat4& a, const Vec4& x);
// rows @ m.T (an N×3 array times a transposed matrix): numpy's matrix-vector product for one row, its matrix product
// for more.
std::vector<Vec3> rows_mt(std::span<const Vec3> rows, const Mat3& m);
std::vector<Vec4> rows_mt(std::span<const Vec4> rows, const Mat4& m);
// a @ b for two vectors.
double dot(const Vec3& a, const Vec3& b);
// np.linalg.norm of a vector of 3 and of 2.
double norm(const Vec3& a);
double norm2(double x, double y);
// np.cross.
Vec3 cross(const Vec3& a, const Vec3& b);

// The column j of a matrix (m[:, j]).
Vec3 column(const Mat3& m, int j);

// Element-wise operations, in Python's order of evaluation (left to right).
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 operator-(const Vec3& a) { return {-a[0], -a[1], -a[2]}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline Vec3 operator/(const Vec3& a, double s) { return {a[0] / s, a[1] / s, a[2] / s}; }
inline Vec3 operator*(const Vec3& a, const Vec3& b) { return {a[0] * b[0], a[1] * b[1], a[2] * b[2]}; }

// numpy's maximum and minimum of two float64 values (a NaN wins; on a tie the second, as the SIMD loops give it).
double np_maximum(double a, double b);
double np_minimum(double a, double b);

// --- the math module and numpy's own math (libm called as Python and numpy call it) -----------------------------------

// np.hypot: libm's hypot (math.hypot is CPython's own: core::py_hypot).
double np_hypot(double x, double y);
// math.hypot of three numbers (CPython's vector_norm).
double py_hypot3(double x, double y, double z);
// math.exp: core::Error("overflow", "math range error") where Python raises OverflowError.
double py_exp(double x);
// math.log: core::Error("value", "math domain error") for x <= 0.
double py_log(double x);
// math.remainder (IEEE remainder): core::Error("value", "math domain error") for an infinite x or a zero y.
double py_remainder(double x, double y);
// math.floor / math.ceil to an integer: core::Error("value", …) for NaN, core::Error("overflow", …) for infinities
// (Python's ValueError and OverflowError).
double py_floor_int(double x);
double py_ceil_int(double x);

inline constexpr double kTau = 6.283185307179586;  // math.tau

}  // namespace genko::core::la
