#pragma once

// The C library's float sinf, cosf and powf as the reference computes them, the same bits on every platform.
//
// numpy computes np.sin, np.cos and np.power of float32 arrays with the C library's sinf, cosf and powf where the
// reference runs it (render/filters.cpp, render/npcompat.hpp). Another C library (Windows' UCRT, macOS's libm) may
// round some of them otherwise, so they are computed here instead (docs/cpp-migration/ARCHITECTURE.md §4: the same
// pixels on Windows and Linux): ported from Arm's optimized-routines (v25.01: math/sinf.c, cosf.c, sincosf.h,
// sincosf_data.c, powf.c, powf_log2_data.c, exp2f_data.c; MIT), the routines the reference's C library runs, as it
// runs them on a CPU with FMA and AVX2 (as the reference machines have): built with -mfma -mavx2, where the compiler
// fuses each a * b + c into one multiply-add. Those multiply-adds are written as std::fma, every other operation as in
// the source (this file is compiled without contraction, GenkoFlags). Checked bit for bit against the reference's C
// library, glibc 2.39 on x86-64: every float for sinf and cosf, and sampled pairs for powf (test_raster_ops'
// libmFloatIsGlibcs).

namespace genko::render::libm {

float sinf(float x);
float cosf(float x);
float powf(float x, float y);

}  // namespace genko::render::libm
