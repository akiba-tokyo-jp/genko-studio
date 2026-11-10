// BLAKE3 (render/blake3.hpp).

#include "render/blake3.hpp"

#include <algorithm>

extern "C" {
#include "blake3.h"
}

namespace genko::render::blake3 {

std::string hex(std::string_view bytes) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, bytes.data(), bytes.size());
    unsigned char out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, out, BLAKE3_OUT_LEN);
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(2 * BLAKE3_OUT_LEN, '0');
    for (std::size_t i = 0; i < BLAKE3_OUT_LEN; ++i) {
        text[2 * i] = kDigits[out[i] >> 4];
        text[2 * i + 1] = kDigits[out[i] & 15];
    }
    return text;
}

bool is_hex_hash(std::string_view text) {
    return text.size() == 2 * BLAKE3_OUT_LEN &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string version() { return blake3_version(); }

}  // namespace genko::render::blake3
