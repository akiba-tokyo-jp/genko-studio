// Python 3.12's random.Random. MT19937 as in Modules/_randommodule.c, the seeding and the derived numbers as in
// Lib/random.py. Compiled with -fno-builtin (see CMakeLists.txt): gauss calls libm's log as CPython does.

#include "core/pyrandom.hpp"

#include <QByteArray>
#include <QCryptographicHash>

#include <cmath>
#include <string>

#include "core/pynum.hpp"

namespace genko::core {

namespace {

constexpr int kM = 397;
constexpr std::uint32_t kMatrixA = 0x9908b0dfU;
constexpr std::uint32_t kUpperMask = 0x80000000U;
constexpr std::uint32_t kLowerMask = 0x7fffffffU;
constexpr double kTwoPi = 6.283185307179586;  // math.tau

int bit_length(std::uint64_t n) {
    int bits = 0;
    while (n != 0) {
        ++bits;
        n >>= 1;
    }
    return bits;
}

// Python's a // b for ints.
std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

std::string str_of(std::int64_t v) { return std::to_string(v); }

}  // namespace

PyRandom::PyRandom() { seed(0); }

PyRandom::PyRandom(std::int64_t seed_value) { seed(seed_value); }

PyRandom PyRandom::from_str(std::string_view text) {
    PyRandom out;
    out.seed_str(text);
    return out;
}

PyRandom PyRandom::from_bytes(std::string_view bytes) {
    PyRandom out;
    out.seed_bytes(bytes);
    return out;
}

void PyRandom::init_genrand(std::uint32_t s) {
    mt_[0] = s;
    int mti = 1;
    for (; mti < kN; ++mti) {
        mt_[static_cast<std::size_t>(mti)] =
            1812433253U * (mt_[static_cast<std::size_t>(mti - 1)] ^ (mt_[static_cast<std::size_t>(mti - 1)] >> 30)) +
            static_cast<std::uint32_t>(mti);
    }
    index_ = mti;
}

void PyRandom::init_by_array(std::span<const std::uint32_t> key) {
    init_genrand(19650218U);
    std::size_t i = 1;
    std::size_t j = 0;
    const std::size_t n = static_cast<std::size_t>(kN);
    std::size_t k = n > key.size() ? n : key.size();
    for (; k != 0; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525U)) + key[j] + static_cast<std::uint32_t>(j);
        ++i;
        ++j;
        if (i >= n) {
            mt_[0] = mt_[n - 1];
            i = 1;
        }
        if (j >= key.size()) j = 0;
    }
    for (k = n - 1; k != 0; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941U)) - static_cast<std::uint32_t>(i);
        ++i;
        if (i >= n) {
            mt_[0] = mt_[n - 1];
            i = 1;
        }
    }
    mt_[0] = 0x80000000U;
}

void PyRandom::seed_words(std::vector<std::uint32_t> words) {
    // random_seed: the absolute value split into 32-bit chunks from the right, as many as its bits need (one for 0).
    while (words.size() > 1 && words.back() == 0) words.pop_back();
    if (words.empty()) words.push_back(0);
    init_by_array(words);
    gauss_next_.reset();
}

void PyRandom::seed(std::int64_t a) {
    const std::uint64_t n = a < 0 ? (~static_cast<std::uint64_t>(a) + 1U) : static_cast<std::uint64_t>(a);
    seed_words({static_cast<std::uint32_t>(n & 0xffffffffU), static_cast<std::uint32_t>(n >> 32)});
}

void PyRandom::seed_hashed(std::string_view bytes) {
    const QByteArray data(bytes.data(), static_cast<qsizetype>(bytes.size()));
    const QByteArray digest = QCryptographicHash::hash(data, QCryptographicHash::Sha512);
    std::string all(bytes);
    all.append(digest.constData(), static_cast<std::size_t>(digest.size()));
    // int.from_bytes(all, "big"), then its 32-bit words from the least significant end
    std::vector<std::uint32_t> words((all.size() + 3) / 4, 0U);
    for (std::size_t pos = 0; pos < all.size(); ++pos) {
        const std::size_t from_end = all.size() - 1 - pos;  // byte significance
        words[from_end / 4] |= static_cast<std::uint32_t>(static_cast<unsigned char>(all[pos])) << (8 * (from_end % 4));
    }
    seed_words(std::move(words));
}

void PyRandom::seed_str(std::string_view text) { seed_hashed(text); }

void PyRandom::seed_bytes(std::string_view bytes) { seed_hashed(bytes); }

std::uint32_t PyRandom::next_uint32() {
    static constexpr std::uint32_t mag01[2] = {0x0U, kMatrixA};
    if (index_ >= kN) {
        int kk = 0;
        for (; kk < kN - kM; ++kk) {
            const std::uint32_t y = (mt_[static_cast<std::size_t>(kk)] & kUpperMask) |
                                    (mt_[static_cast<std::size_t>(kk + 1)] & kLowerMask);
            mt_[static_cast<std::size_t>(kk)] = mt_[static_cast<std::size_t>(kk + kM)] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        for (; kk < kN - 1; ++kk) {
            const std::uint32_t y = (mt_[static_cast<std::size_t>(kk)] & kUpperMask) |
                                    (mt_[static_cast<std::size_t>(kk + 1)] & kLowerMask);
            mt_[static_cast<std::size_t>(kk)] =
                mt_[static_cast<std::size_t>(kk + (kM - kN))] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        const std::uint32_t y = (mt_[kN - 1] & kUpperMask) | (mt_[0] & kLowerMask);
        mt_[kN - 1] = mt_[kM - 1] ^ (y >> 1) ^ mag01[y & 0x1U];
        index_ = 0;
    }
    std::uint32_t y = mt_[static_cast<std::size_t>(index_++)];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= (y >> 18);
    return y;
}

double PyRandom::random() {
    const std::uint32_t a = next_uint32() >> 5;
    const std::uint32_t b = next_uint32() >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

double PyRandom::uniform(double a, double b) { return a + (b - a) * random(); }

std::uint64_t PyRandom::getrandbits(int k) {
    if (k < 0) throw Error("value", "number of bits must be non-negative");
    if (k > 64) throw Error("value", "getrandbits: more than 64 bits");
    if (k == 0) return 0;
    if (k <= 32) return next_uint32() >> (32 - k);
    // words from the least significant; the last one keeps only its top bits
    std::uint64_t out = 0;
    int left = k;
    for (int i = 0; i < 2; ++i, left -= 32) {
        std::uint32_t r = next_uint32();
        if (left < 32) r >>= (32 - left);
        out |= static_cast<std::uint64_t>(r) << (32 * i);
    }
    return out;
}

std::uint64_t PyRandom::randbelow(std::uint64_t n) {
    const int k = bit_length(n);
    std::uint64_t r = getrandbits(k);
    while (r >= n) r = getrandbits(k);
    return r;
}

std::int64_t PyRandom::randrange(std::int64_t stop) {
    if (stop > 0) return static_cast<std::int64_t>(randbelow(static_cast<std::uint64_t>(stop)));
    throw Error("value", "empty range for randrange()");
}

std::int64_t PyRandom::randrange(std::int64_t start, std::int64_t stop, std::int64_t step) {
    if (step == 1) {
        if (stop > start) {
            const std::uint64_t width = static_cast<std::uint64_t>(stop) - static_cast<std::uint64_t>(start);
            return static_cast<std::int64_t>(static_cast<std::uint64_t>(start) + randbelow(width));
        }
        throw Error("value", "empty range in randrange(" + str_of(start) + ", " + str_of(stop) + ")");
    }
    const std::int64_t width = stop - start;
    std::int64_t n = 0;
    if (step > 0) {
        n = floor_div(width + step - 1, step);
    } else if (step < 0) {
        n = floor_div(width + step + 1, step);
    } else {
        throw Error("value", "zero step for randrange()");
    }
    if (n <= 0) {
        throw Error("value",
                    "empty range in randrange(" + str_of(start) + ", " + str_of(stop) + ", " + str_of(step) + ")");
    }
    return start + step * static_cast<std::int64_t>(randbelow(static_cast<std::uint64_t>(n)));
}

std::int64_t PyRandom::randint(std::int64_t a, std::int64_t b) { return randrange(a, b + 1); }

double PyRandom::gauss(double mu, double sigma) {
    std::optional<double> z = gauss_next_;
    gauss_next_.reset();
    if (!z) {
        const double x2pi = random() * kTwoPi;
        const double g2rad = std::sqrt(-2.0 * std::log(1.0 - random()));
        z = py_cos(x2pi) * g2rad;
        gauss_next_ = py_sin(x2pi) * g2rad;
    }
    return mu + *z * sigma;
}

}  // namespace genko::core
