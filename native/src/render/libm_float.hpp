#pragma once

// The C library's float sinf, cosf and powf as the reference computes them, the same bits on every platform.
//
// numpy computes np.sin, np.cos and np.power of float32 arrays with the C library's sinf, cosf and powf where the
// reference runs it (render/filters.cpp, render/npcompat.hpp): glibc 2.39's (sysdeps/ieee754/flt-32: s_sinf.c,
// s_cosf.c, s_sincosf.h, e_powf.c and their tables, which are Arm's optimized-routines: math/sinf.c, cosf.c, sincosf.h,
// powf.c and their data, MIT). Another C library (Windows' UCRT, macOS's libm) may round some of them otherwise, so
// they are computed here instead (docs/cpp-migration/ARCHITECTURE.md §4: the same pixels on Windows and Linux).
// x86-64 glibc takes the variant it builds with -mfma -mavx2 on a CPU with FMA and AVX2 (as the reference machines
// have): the multiply-adds the compiler fused there are written as std::fma, every other operation as there (this
// file is compiled without contraction, GenkoFlags). Checked bit for bit against glibc 2.39's libm: every float for
// sinf and cosf, and sampled pairs for powf (test_raster_ops' libmFloatIsGlibcs).

namespace genko::render::libm {

float sinf(float x);
float cosf(float x);
float powf(float x, float y);

}  // namespace genko::render::libm
