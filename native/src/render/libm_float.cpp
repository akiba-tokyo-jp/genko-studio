// glibc 2.39's sinf, cosf and powf (sysdeps/ieee754/flt-32), as x86-64 glibc builds them for a CPU with FMA and AVX2
// (sysdeps/x86_64/fpu/multiarch: s_sinf-fma.c, s_cosf-fma.c, e_powf-fma.c, compiled with -mfma -mavx2, where GCC fuses
// each a * b + c into one multiply-add). The algorithms and tables are Arm's optimized-routines (math/sinf.c, cosf.c,
// sincosf.h, sincosf_data.c, powf.c, powf_log2_data.c, exp2f_data.c):
//
//   Copyright (c) 2018-2024, Arm Limited.
//   SPDX-License-Identifier: MIT OR Apache-2.0 WITH LLVM-exception
//
// taken here under its MIT terms (native/third_party has the texts; docs/cpp-migration/THIRD_PARTY.md). What C leaves to
// the machine is written out: the NaN an invalid operation gives (x86's, as the reference's), the NaN `x + y` passes on.

#include "render/libm_float.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace genko::render::libm {

namespace {

std::uint32_t asuint(float f) { return std::bit_cast<std::uint32_t>(f); }
float asfloat(std::uint32_t i) { return std::bit_cast<float>(i); }
std::uint64_t asuint64(double f) { return std::bit_cast<std::uint64_t>(f); }
double asdouble(std::uint64_t i) { return std::bit_cast<double>(i); }

bool is_nan(float x) { return (asuint(x) & 0x7fffffffU) > 0x7f800000U; }
float quieted(float x) { return asfloat(asuint(x) | 0x00400000U); }

// __math_invalidf(x): (x - x) / (x - x). A NaN comes back quieted; anything else gives x86's default NaN (the sign
// bit set), which the reference's C library gives.
float invalid(float x) { return is_nan(x) ? quieted(x) : asfloat(0xffc00000U); }

// x + y where one of them may be a NaN, as SSE adds them: the first NaN, quieted.
float nan_sum(float x, float y) {
    if (is_nan(x)) return quieted(x);
    if (is_nan(y)) return quieted(y);
    return x + y;
}

// __issignalingf (x86: a quiet NaN has the top bit of its fraction set)
bool is_signaling(float x) { return ((asuint(x) ^ 0x00400000U) & 0x7fffffffU) > 0x7fc00000U; }

// --- sinf, cosf (s_sincosf.h, sysdeps/x86/fpu/s_sincosf_data.c) ---------------------------------------------------

struct SinCos {
    double sign[4];  // the sign of the sine in quadrants 0..3
    double hpi_inv;  // 2 / PI * 2^24 (no TOINT_INTRINSICS on x86-64)
    double hpi;      // PI / 2
    double c0, c1, c2, c3, c4;  // the cosine polynomial (negated in the second entry)
    double s1, s2, s3;          // the sine polynomial
};

constexpr SinCos kSinCos[2] = {
    {{1.0, -1.0, -1.0, 1.0}, 0x1.45F306DC9C883p+23, 0x1.921FB54442D18p0, 0x1p0, -0x1.ffffffd0c621cp-2,
     0x1.55553e1068f19p-5, -0x1.6c087e89a359dp-10, 0x1.99343027bf8c3p-16, -0x1.555545995a603p-3, 0x1.1107605230bc4p-7,
     -0x1.994eb3774cf24p-13},
    {{1.0, -1.0, -1.0, 1.0}, 0x1.45F306DC9C883p+23, 0x1.921FB54442D18p0, -0x1p0, 0x1.ffffffd0c621cp-2,
     -0x1.55553e1068f19p-5, 0x1.6c087e89a359dp-10, -0x1.99343027bf8c3p-16, -0x1.555545995a603p-3, 0x1.1107605230bc4p-7,
     -0x1.994eb3774cf24p-13}};

// 4 / PI to 192 bits, 8 new bits an entry
constexpr std::uint32_t kInvPio4[24] = {
    0xa2,       0xa2f9,     0xa2f983,   0xa2f9836e, 0xf9836e4e, 0x836e4e44, 0x6e4e4415, 0x4e441529,
    0x441529fc, 0x1529fc27, 0x29fc2757, 0xfc2757d1, 0x2757d1f5, 0x57d1f534, 0xd1f534dd, 0xf534ddc0,
    0x34ddc0db, 0xddc0db62, 0xc0db6295, 0xdb629599, 0x6295993c, 0x95993c43, 0x993c4390, 0x3c439041};

constexpr double kPi63 = 0x1.921FB54442D18p-62;  // 2 PI * 2^-64
constexpr float kPio4 = 0x1.921FB6p-1f;

// The top 12 bits of the float without its sign.
std::uint32_t abstop12(float x) { return (asuint(x) >> 20) & 0x7ff; }

// The sine of x (x2: x squared) by the polynomial of p; n odd: the cosine polynomial.
float sinf_poly(double x, double x2, const SinCos* p, int n) {
    if ((n & 1) == 0) {
        const double x3 = x * x2;
        const double s1 = std::fma(x2, p->s3, p->s2);
        const double x7 = x3 * x2;
        const double s = std::fma(x3, p->s1, x);
        return static_cast<float>(std::fma(x7, s1, s));
    }
    const double x4 = x2 * x2;
    const double c2 = std::fma(x2, p->c4, p->c3);
    const double c1 = std::fma(x2, p->c1, p->c0);
    const double x6 = x4 * x2;
    const double c = std::fma(x4, p->c2, c1);
    return static_cast<float>(std::fma(x6, c2, c));
}

// x reduced to -PI/4..PI/4 by one multiply-subtract (|x| <= 120), its quadrant in n
double reduce_fast(double x, const SinCos* p, int& n) {
    const double r = x * p->hpi_inv;
    n = (static_cast<std::int32_t>(r) + 0x800000) >> 24;
    return std::fma(-static_cast<double>(n), p->hpi, x);
}

// the float of bits xi (>= 2.0, its sign ignored) reduced with 4 / PI to 192 bits, its quadrant in n
double reduce_large(std::uint32_t xi, int& n) {
    const std::uint32_t* arr = &kInvPio4[(xi >> 26) & 15];
    const int shift = static_cast<int>((xi >> 23) & 7);
    xi = (xi & 0xffffff) | 0x800000;
    xi <<= shift;
    std::uint64_t res0 = static_cast<std::uint32_t>(xi * arr[0]);  // (a 32-bit product)
    const std::uint64_t res1 = static_cast<std::uint64_t>(xi) * arr[4];
    const std::uint64_t res2 = static_cast<std::uint64_t>(xi) * arr[8];
    res0 = (res2 >> 32) | (res0 << 32);
    res0 += res1;
    const std::uint64_t quadrant = (res0 + (1ULL << 61)) >> 62;
    res0 -= quadrant << 62;
    const double x = static_cast<double>(static_cast<std::int64_t>(res0));
    n = static_cast<int>(quadrant);
    return x * kPi63;
}

// --- powf (e_powf.c, e_powf_log2_data.c, e_exp2f_data.c; POWF_SCALE 1 without TOINT_INTRINSICS) ---------------------

struct InvcLogc {
    double invc, logc;
};

constexpr InvcLogc kPowfLog2[16] = {
    {0x1.661ec79f8f3bep+0, -0x1.efec65b963019p-2}, {0x1.571ed4aaf883dp+0, -0x1.b0b6832d4fca4p-2},
    {0x1.49539f0f010bp+0, -0x1.7418b0a1fb77bp-2},  {0x1.3c995b0b80385p+0, -0x1.39de91a6dcf7bp-2},
    {0x1.30d190c8864a5p+0, -0x1.01d9bf3f2b631p-2}, {0x1.25e227b0b8eap+0, -0x1.97c1d1b3b7afp-3},
    {0x1.1bb4a4a1a343fp+0, -0x1.2f9e393af3c9fp-3}, {0x1.12358f08ae5bap+0, -0x1.960cbbf788d5cp-4},
    {0x1.0953f419900a7p+0, -0x1.a6f9db6475fcep-5}, {0x1p+0, 0x0p+0},
    {0x1.e608cfd9a47acp-1, 0x1.338ca9f24f53dp-4},  {0x1.ca4b31f026aap-1, 0x1.476a9543891bap-3},
    {0x1.b2036576afce6p-1, 0x1.e840b4ac4e4d2p-3},  {0x1.9c2d163a1aa2dp-1, 0x1.40645f0c6651cp-2},
    {0x1.886e6037841edp-1, 0x1.88e9c2c1b9ff8p-2},  {0x1.767dcf5534862p-1, 0x1.ce0a44eb17bccp-2}};

constexpr double kPowfLog2Poly[5] = {0x1.27616c9496e0bp-2, -0x1.71969a075c67ap-2, 0x1.ec70a6ca7baddp-2,
                                     -0x1.7154748bef6c8p-1, 0x1.71547652ab82bp0};

// uint(2^(i/32)) - (i << 47)
constexpr std::uint64_t kExp2f[32] = {
    0x3ff0000000000000, 0x3fefd9b0d3158574, 0x3fefb5586cf9890f, 0x3fef9301d0125b51, 0x3fef72b83c7d517b,
    0x3fef54873168b9aa, 0x3fef387a6e756238, 0x3fef1e9df51fdee1, 0x3fef06fe0a31b715, 0x3feef1a7373aa9cb,
    0x3feedea64c123422, 0x3feece086061892d, 0x3feebfdad5362a27, 0x3feeb42b569d4f82, 0x3feeab07dd485429,
    0x3feea47eb03a5585, 0x3feea09e667f3bcd, 0x3fee9f75e8ec5f74, 0x3feea11473eb0187, 0x3feea589994cce13,
    0x3feeace5422aa0db, 0x3feeb737b0cdc5e5, 0x3feec49182a3f090, 0x3feed503b23e255d, 0x3feee89f995ad3ad,
    0x3feeff76f2fb5e47, 0x3fef199bdd85529c, 0x3fef3720dcef9069, 0x3fef5818dcfba487, 0x3fef7c97337b9b5f,
    0x3fefa4afa2a490da, 0x3fefd0765b6e4540};

constexpr double kExp2fShift = 0x1.8p+52 / 32;  // shift_scaled
constexpr double kExp2fPoly[3] = {0x1.c6af84b912394p-5, 0x1.ebfce50fac4f3p-3, 0x1.62e42ff0c52d6p-1};
constexpr std::uint32_t kSignBias = 1U << (5 + 11);
constexpr std::uint32_t kOff = 0x3f330000;

// log2 of the float of bits ix (a subnormal normalised: its biased exponent negative)
double log2_inline(std::uint32_t ix) {
    const std::uint32_t tmp = ix - kOff;
    const int i = static_cast<int>((tmp >> (23 - 4)) % 16);
    const std::uint32_t top = tmp & 0xff800000;
    const std::uint32_t iz = ix - top;
    const int k = static_cast<std::int32_t>(top) >> 23;  // (an arithmetic shift)
    const double invc = kPowfLog2[i].invc;
    const double logc = kPowfLog2[i].logc;
    const double z = asfloat(iz);
    // log2(x) = log1p(z/c-1)/ln2 + log2(c) + k
    const double r = std::fma(z, invc, -1.0);
    const double y0 = logc + static_cast<double>(k);
    const double* a = kPowfLog2Poly;
    const double r2 = r * r;
    double y = std::fma(a[0], r, a[1]);
    const double p = std::fma(a[2], r, a[3]);
    const double r4 = r2 * r2;
    double q = std::fma(a[4], r, y0);
    q = std::fma(p, r2, q);
    y = std::fma(y, r4, q);
    return y;
}

// 2^xd (xd in -1021..1023), the sign of sign_bias
double exp2_inline(double xd, std::uint32_t sign_bias) {
    // x = k/N + r with r in [-1/(2N), 1/(2N)]
    double kd = xd + kExp2fShift;
    const std::uint64_t ki = asuint64(kd);
    kd -= kExp2fShift;
    const double r = xd - kd;
    // exp2(x) = 2^(k/N) * 2^r ~= s * (C0*r^3 + C1*r^2 + C2*r + 1)
    std::uint64_t t = kExp2f[ki % 32];
    const std::uint64_t ski = ki + sign_bias;
    t += ski << (52 - 5);
    const double s = asdouble(t);
    const double z = std::fma(kExp2fPoly[0], r, kExp2fPoly[1]);
    const double r2 = r * r;
    double y = std::fma(kExp2fPoly[2], r, 1.0);
    y = std::fma(z, r2, y);
    return y * s;
}

// 0: not an int, 1: an odd one, 2: an even one (of the bits of a non-zero finite float)
int checkint(std::uint32_t iy) {
    const int e = static_cast<int>(iy >> 23 & 0xff);
    if (e < 0x7f) return 0;
    if (e > 0x7f + 23) return 2;
    if (iy & ((1U << (0x7f + 23 - e)) - 1)) return 0;
    if (iy & (1U << (0x7f + 23 - e))) return 1;
    return 2;
}

bool zeroinfnan(std::uint32_t ix) { return 2 * ix - 1 >= 2U * 0x7f800000 - 1; }

// __math_oflowf, __math_uflowf, __math_may_uflowf: their values (y * y with the sign given)
float xflow(std::uint32_t sign, float y) {
    volatile float v = sign != 0 ? -y : y;  // (math_opt_barrier)
    return v * y;
}

}  // namespace

float sinf(float y) {
    double x = y;
    const SinCos* p = &kSinCos[0];
    int n = 0;
    if (abstop12(y) < abstop12(kPio4)) {
        const double s = x * x;
        if (abstop12(y) < abstop12(0x1p-12f)) return y;  // (glibc raises the underflow of a tiny y)
        return sinf_poly(x, s, p, 0);
    }
    if (abstop12(y) < abstop12(120.0f)) {
        x = reduce_fast(x, p, n);
        const double s = p->sign[n & 3];
        if (n & 2) p = &kSinCos[1];
        return sinf_poly(x * s, x * x, p, n);
    }
    if (abstop12(y) < abstop12(INFINITY)) {
        const std::uint32_t xi = asuint(y);
        const int sign = static_cast<int>(xi >> 31);
        x = reduce_large(xi, n);
        const double s = p->sign[(n + sign) & 3];
        if ((n + sign) & 2) p = &kSinCos[1];
        return sinf_poly(x * s, x * x, p, n);
    }
    return invalid(y);
}

float cosf(float y) {
    double x = y;
    const SinCos* p = &kSinCos[0];
    int n = 0;
    if (abstop12(y) < abstop12(kPio4)) {
        const double x2 = x * x;
        if (abstop12(y) < abstop12(0x1p-12f)) return 1.0f;
        return sinf_poly(x, x2, p, 1);
    }
    if (abstop12(y) < abstop12(120.0f)) {
        x = reduce_fast(x, p, n);
        const double s = p->sign[n & 3];
        if (n & 2) p = &kSinCos[1];
        return sinf_poly(x * s, x * x, p, n ^ 1);
    }
    if (abstop12(y) < abstop12(INFINITY)) {
        const std::uint32_t xi = asuint(y);
        const int sign = static_cast<int>(xi >> 31);
        x = reduce_large(xi, n);
        const double s = p->sign[(n + sign) & 3];
        if ((n + sign) & 2) p = &kSinCos[1];
        return sinf_poly(x * s, x * x, p, n ^ 1);
    }
    return invalid(y);
}

float powf(float x, float y) {
    std::uint32_t sign_bias = 0;
    std::uint32_t ix = asuint(x);
    const std::uint32_t iy = asuint(y);
    if (ix - 0x00800000 >= 0x7f800000 - 0x00800000 || zeroinfnan(iy)) {
        // either (x < 0x1p-126 or inf or nan) or (y is 0 or inf or nan)
        if (zeroinfnan(iy)) {
            if (2 * iy == 0) return is_signaling(x) ? nan_sum(x, y) : 1.0f;
            if (ix == 0x3f800000) return is_signaling(y) ? nan_sum(x, y) : 1.0f;
            if (2 * ix > 2U * 0x7f800000 || 2 * iy > 2U * 0x7f800000) return nan_sum(x, y);
            if (2 * ix == 2 * 0x3f800000U) return 1.0f;
            if ((2 * ix < 2 * 0x3f800000U) == !(iy & 0x80000000)) return 0.0f;  // |x|<1 && y==inf or |x|>1 && y==-inf
            return y * y;
        }
        if (zeroinfnan(ix)) {
            float x2 = x * x;
            if ((ix & 0x80000000) && checkint(iy) == 1) {
                x2 = -x2;
                sign_bias = 1;
            }
            if (2 * ix == 0 && (iy & 0x80000000)) {  // (__math_divzerof: ±1 / 0)
                return sign_bias != 0 ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
            }
            return (iy & 0x80000000) ? 1 / x2 : x2;
        }
        // x and y are non-zero finite
        if (ix & 0x80000000) {
            // finite x < 0
            const int yint = checkint(iy);
            if (yint == 0) return invalid(x);
            if (yint == 1) sign_bias = kSignBias;
            ix &= 0x7fffffff;
        }
        if (ix < 0x00800000) {
            // a subnormal x normalised, so its exponent becomes negative
            ix = asuint(x * 0x1p23f);
            ix &= 0x7fffffff;
            ix -= 23U << 23;
        }
    }
    const double logx = log2_inline(ix);
    const double ylogx = static_cast<double>(y) * logx;  // (cannot overflow: y is single precision)
    if ((asuint64(ylogx) >> 47 & 0xffff) >= asuint64(126.0) >> 47) {
        // |y*log(x)| >= 126
        if (ylogx > 0x1.fffffffd1d571p+6) return xflow(sign_bias, 0x1p97f);  // |x^y| > 0x1.ffffffp127
        if (ylogx > 0x1.fffffffa3aae2p+6) {
            // |x^y| > 0x1.fffffep127: whether it rounds away from 0 (in the rounding mode in force)
            volatile float tiny = 0x1p-25f;
            if ((sign_bias == 0 && 1.0f + tiny != 1.0f) || (sign_bias != 0 && -1.0f - tiny != -1.0f)) {
                return xflow(sign_bias, 0x1p97f);
            }
        }
        if (ylogx <= -150.0) return xflow(sign_bias, 0x1p-95f);
        if (ylogx < -149.0) return xflow(sign_bias, 0x1.4p-75f);
    }
    return static_cast<float>(exp2_inline(ylogx, sign_bias));
}

}  // namespace genko::render::libm
