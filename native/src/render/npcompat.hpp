#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The parts of numpy 2.x the Python drawing uses (tones.py), reproduced for the same pixels: float32 arithmetic is
// plain C++ float arithmetic (no FMA: GenkoFlags), and these do what numpy does where that is more than one IEEE
// operation.

namespace genko::render::np {

// numpy.random.default_rng(seed) (a SeedSequence over the seed's 32-bit words, then PCG64, XSL-RR 128/64) and its
// random(): the same doubles as numpy, in the same order (rng.random((rows, cols)) fills rows first).
class Pcg64 {
public:
    // default_rng(seed) for an int seed >= 0 given as its 32-bit words, least significant first ([0] for 0).
    explicit Pcg64(const std::vector<std::uint32_t>& seed_words);
    static Pcg64 from_seed(std::uint64_t seed);

    std::uint64_t next64();
    double random();  // (next64() >> 11) * 2**-53

private:
    void step();
    std::uint64_t state_hi_ = 0;
    std::uint64_t state_lo_ = 0;
    std::uint64_t inc_hi_ = 0;
    std::uint64_t inc_lo_ = 0;
};

// numpy's add.reduce of a float32 array (pairwise summation in float32, 8 accumulators, blocks of 128).
float pairwise_sum(const float* a, std::size_t n);

// arr.mean() of a float32 array: float32(double(pairwise sum) / n).
float mean(const float* a, std::size_t n);

// The float32 ufuncs numpy computes with the platform's libm where the reference runs (np.sin, np.cos, np.hypot,
// np.arctan2 of float32 arrays: sinf, cosf, hypotf, atan2f), called out of line so the compiler never folds or merges
// them. sinf and cosf are glibc 2.39's on every platform (render/libm_float.hpp); hypotf and atan2f this machine's.
float sinf(float x);
float cosf(float x);
float hypotf(float x, float y);
float atan2f(float y, float x);
// np.power of a float32 array and a float32 (psd._depth8's gamma): libm's powf, element by element (numpy 2.4 takes
// its own SIMD way only with AVX512_SKX, which the reference's NPY_DISABLE_CPU_FEATURES leaves out): glibc 2.39's on
// every platform (render/libm_float.hpp).
float powf(float x, float y);
// np.hypot of float64 arrays (and of int64 ones, cast to float64): libm's hypot (balloons._fade_mask).
double hypot(double x, double y);

// np.remainder for float32 (Python's %: the sign of the divisor).
float remainder(float a, float b);

}  // namespace genko::render::np
