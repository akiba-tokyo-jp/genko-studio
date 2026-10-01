#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "core/error.hpp"

// Python 3.12's random.Random (Lib/random.py over Modules/_randommodule.c): the Mersenne Twister (MT19937) seeded
// and turned into numbers exactly as CPython does it, so that a seeded sequence is the same, number for number and
// bit for bit, as the Python baseline's (a pencil's grain, a spray's drops, a rough panel border).
// docs/cpp-migration/ARCHITECTURE.md §4.

namespace genko::core {

class PyRandom {
public:
    // random.Random(0).
    PyRandom();
    // random.Random(n): every bit of |n| is used.
    explicit PyRandom(std::int64_t seed);

    // random.Random(text) (seed version 2: the UTF-8 bytes followed by their SHA-512, as one big integer).
    static PyRandom from_str(std::string_view text);
    // random.Random(b"…") (the bytes followed by their SHA-512).
    static PyRandom from_bytes(std::string_view bytes);

    void seed(std::int64_t a);
    void seed_str(std::string_view text);
    void seed_bytes(std::string_view bytes);

    // random(): a float in [0, 1) with 53 random bits.
    double random();
    // uniform(a, b): a + (b - a) * random().
    double uniform(double a, double b);
    // getrandbits(k), 0 <= k <= 64 (Python's ints are unbounded; Genko never asks for more).
    std::uint64_t getrandbits(int k);
    // randrange(stop) and randrange(start, stop, step), randint(a, b); core::Error("value") with Python's message
    // for an empty range or a zero step.
    std::int64_t randrange(std::int64_t stop);
    std::int64_t randrange(std::int64_t start, std::int64_t stop, std::int64_t step = 1);
    std::int64_t randint(std::int64_t a, std::int64_t b);
    // gauss(mu, sigma): the Box–Muller pair, the second kept for the next call.
    double gauss(double mu = 0.0, double sigma = 1.0);

    // choice(seq): core::Error("index") for an empty sequence (Python's IndexError).
    template <class T>
    const T& choice(std::span<const T> seq) {
        if (seq.empty()) throw Error("index", "Cannot choose from an empty sequence");
        return seq[static_cast<std::size_t>(randbelow(static_cast<std::uint64_t>(seq.size())))];
    }
    template <class T>
    const T& choice(const std::vector<T>& seq) {
        return choice(std::span<const T>(seq));
    }

    // shuffle(x), in place.
    template <class T>
    void shuffle(std::vector<T>& x) {
        for (std::size_t i = x.size(); i-- > 1;) {
            const auto j = static_cast<std::size_t>(randbelow(static_cast<std::uint64_t>(i + 1)));
            std::swap(x[i], x[j]);
        }
    }

    // _randbelow(n) for n > 0: getrandbits(n.bit_length()) until it is below n.
    std::uint64_t randbelow(std::uint64_t n);

    // The next raw 32-bit output of the generator (genrand_uint32).
    std::uint32_t next_uint32();

private:
    void init_genrand(std::uint32_t s);
    void init_by_array(std::span<const std::uint32_t> key);
    // random_seed(n) for n given as its 32-bit words, least significant first.
    void seed_words(std::vector<std::uint32_t> words);
    // The big-endian bytes followed by their SHA-512, as Python's int.from_bytes(… + sha512(…).digest()).
    void seed_hashed(std::string_view bytes);

    static constexpr int kN = 624;
    std::array<std::uint32_t, kN> mt_{};
    int index_ = kN + 1;
    std::optional<double> gauss_next_;
};

}  // namespace genko::core
