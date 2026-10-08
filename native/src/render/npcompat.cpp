// numpy's PCG64, SeedSequence, pairwise summation and float32 math (numpy 2.x: numpy/random/bit_generator.pyx,
// numpy/random/src/pcg64, numpy/_core/src/umath/loops_utils.h.src, npy_math_internal.h.src).
//
// This file is compiled with -fno-builtin: sinf, cosf, hypotf and atan2f are called in libm as numpy calls them.

#include "render/npcompat.hpp"

#include <cmath>

namespace genko::render::np {

namespace {

// --- SeedSequence (pool size 4) ------------------------------------------------------------------------------------

constexpr std::uint32_t kInitA = 0x43b0d7e5;
constexpr std::uint32_t kMultA = 0x931e8875;
constexpr std::uint32_t kInitB = 0x8b51f9dd;
constexpr std::uint32_t kMultB = 0x58f38ded;
constexpr std::uint32_t kMixMultL = 0xca01f9dd;
constexpr std::uint32_t kMixMultR = 0x4973f715;
constexpr int kXShift = 16;
constexpr std::size_t kPoolSize = 4;

std::uint32_t hashmix(std::uint32_t value, std::uint32_t& hash_const) {
    value ^= hash_const;
    hash_const *= kMultA;
    value *= hash_const;
    value ^= value >> kXShift;
    return value;
}

std::uint32_t mix(std::uint32_t x, std::uint32_t y) {
    std::uint32_t result = kMixMultL * x - kMixMultR * y;
    result ^= result >> kXShift;
    return result;
}

std::vector<std::uint32_t> pool_of(const std::vector<std::uint32_t>& entropy) {
    std::vector<std::uint32_t> pool(kPoolSize, 0);
    std::uint32_t hash_const = kInitA;
    for (std::size_t i = 0; i < kPoolSize; ++i) pool[i] = hashmix(i < entropy.size() ? entropy[i] : 0, hash_const);
    for (std::size_t src = 0; src < kPoolSize; ++src) {
        for (std::size_t dst = 0; dst < kPoolSize; ++dst) {
            if (src != dst) pool[dst] = mix(pool[dst], hashmix(pool[src], hash_const));
        }
    }
    for (std::size_t src = kPoolSize; src < entropy.size(); ++src) {
        for (std::size_t dst = 0; dst < kPoolSize; ++dst) pool[dst] = mix(pool[dst], hashmix(entropy[src], hash_const));
    }
    return pool;
}

// generate_state(n_words, np.uint64)
std::vector<std::uint64_t> generate_state(const std::vector<std::uint32_t>& pool, std::size_t n_words) {
    std::vector<std::uint32_t> words(n_words * 2);
    std::uint32_t hash_const = kInitB;
    for (std::size_t i = 0; i < words.size(); ++i) {
        std::uint32_t data = pool[i % pool.size()];
        data ^= hash_const;
        hash_const *= kMultB;
        data *= hash_const;
        data ^= data >> kXShift;
        words[i] = data;
    }
    std::vector<std::uint64_t> out(n_words);
    for (std::size_t i = 0; i < n_words; ++i) out[i] = static_cast<std::uint64_t>(words[2 * i]) | (static_cast<std::uint64_t>(words[2 * i + 1]) << 32);
    return out;
}

// --- 128-bit arithmetic (portable: no __int128 on MSVC) -------------------------------------------------------------

struct U128 {
    std::uint64_t hi;
    std::uint64_t lo;
};

U128 mul64(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a_lo = a & 0xffffffffu, a_hi = a >> 32;
    const std::uint64_t b_lo = b & 0xffffffffu, b_hi = b >> 32;
    const std::uint64_t p0 = a_lo * b_lo;
    const std::uint64_t p1 = a_lo * b_hi;
    const std::uint64_t p2 = a_hi * b_lo;
    const std::uint64_t p3 = a_hi * b_hi;
    const std::uint64_t middle = (p0 >> 32) + (p1 & 0xffffffffu) + (p2 & 0xffffffffu);
    const std::uint64_t lo = (p0 & 0xffffffffu) | (middle << 32);
    const std::uint64_t hi = p3 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
    return U128{hi, lo};
}

U128 mul128(U128 a, U128 b) {  // (mod 2**128)
    U128 out = mul64(a.lo, b.lo);
    out.hi += a.hi * b.lo + a.lo * b.hi;
    return out;
}

U128 add128(U128 a, U128 b) {
    U128 out{a.hi + b.hi, a.lo + b.lo};
    if (out.lo < a.lo) ++out.hi;
    return out;
}

constexpr U128 kMultiplier{2549297995355413924ULL, 4865540595714422341ULL};  // PCG_DEFAULT_MULTIPLIER_128

}  // namespace

Pcg64::Pcg64(const std::vector<std::uint32_t>& seed_words) {
    const std::vector<std::uint64_t> s = generate_state(pool_of(seed_words.empty() ? std::vector<std::uint32_t>{0} : seed_words), 4);
    const U128 initstate{s[0], s[1]};
    const U128 initseq{s[2], s[3]};
    // pcg_setseq_128_srandom_r
    inc_hi_ = (initseq.hi << 1) | (initseq.lo >> 63);
    inc_lo_ = (initseq.lo << 1) | 1u;
    state_hi_ = 0;
    state_lo_ = 0;
    step();
    const U128 sum = add128(U128{state_hi_, state_lo_}, initstate);
    state_hi_ = sum.hi;
    state_lo_ = sum.lo;
    step();
}

Pcg64 Pcg64::from_seed(std::uint64_t seed) {
    std::vector<std::uint32_t> words;
    if (seed == 0) words.push_back(0);
    while (seed > 0) {
        words.push_back(static_cast<std::uint32_t>(seed & 0xffffffffu));
        seed >>= 32;
    }
    return Pcg64(words);
}

void Pcg64::step() {
    const U128 next = add128(mul128(U128{state_hi_, state_lo_}, kMultiplier), U128{inc_hi_, inc_lo_});
    state_hi_ = next.hi;
    state_lo_ = next.lo;
}

std::uint64_t Pcg64::next64() {
    step();
    const std::uint64_t x = state_hi_ ^ state_lo_;
    const unsigned rot = static_cast<unsigned>(state_hi_ >> 58);  // (state >> 122)
    return rot == 0 ? x : (x >> rot) | (x << (64 - rot));
}

double Pcg64::random() { return static_cast<double>(next64() >> 11) * (1.0 / 9007199254740992.0); }

float pairwise_sum(const float* a, std::size_t n) {
    if (n < 8) {
        float res = 0.0f;
        for (std::size_t i = 0; i < n; ++i) res += a[i];
        return res;
    }
    if (n <= 128) {
        float r[8];
        for (int j = 0; j < 8; ++j) r[j] = a[j];
        std::size_t i = 8;
        for (; i < n - (n % 8); i += 8) {
            for (int j = 0; j < 8; ++j) r[j] += a[i + static_cast<std::size_t>(j)];
        }
        float res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; ++i) res += a[i];
        return res;
    }
    std::size_t n2 = n / 2;
    n2 -= n2 % 8;
    return pairwise_sum(a, n2) + pairwise_sum(a + n2, n - n2);
}

float mean(const float* a, std::size_t n) {
    const float sum = 0.0f + pairwise_sum(a, n);
    return static_cast<float>(static_cast<double>(sum) / static_cast<double>(n));
}

float sinf(float x) { return ::sinf(x); }
float cosf(float x) { return ::cosf(x); }
float hypotf(float x, float y) { return ::hypotf(x, y); }
float atan2f(float y, float x) { return ::atan2f(y, x); }
double hypot(double x, double y) { return ::hypot(x, y); }

float remainder(float a, float b) {
    float mod = std::fmod(a, b);
    if (b == 0.0f) return mod;
    if (mod != 0.0f) {
        if ((b < 0) != (mod < 0)) mod += b;
    } else {
        mod = std::copysign(0.0f, b);
    }
    return mod;
}

}  // namespace genko::render::np
