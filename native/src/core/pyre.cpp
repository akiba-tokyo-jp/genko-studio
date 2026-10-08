#include "core/pyre.hpp"

#include <QChar>

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include "core/error.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace {

// --- the cases of _sre (CPython 3.12, Unicode 15.0.0) -------------------------------------------------------------------
//
// _sre.unicode_tolower and _sre.unicode_iscased, as tables made from them over every code point (Python's unicode
// database, not this build's Qt, whose Unicode is newer). Each row is a run: the code points first, first + step, …,
// last; in kLower each lowercases to itself + delta, in kCasedSelf each is its own lowercase but has an uppercase.

struct LowerRun {
    char32_t first;
    char32_t last;
    std::uint8_t step;
    std::int32_t delta;
};

constexpr LowerRun kLower[] = {
    {0x41, 0x5a, 1, 32}, {0xc0, 0xd6, 1, 32}, {0xd8, 0xde, 1, 32}, {0x100, 0x12e, 2, 1}, {0x130, 0x130, 1, -199},
    {0x132, 0x136, 2, 1}, {0x139, 0x147, 2, 1}, {0x14a, 0x176, 2, 1}, {0x178, 0x178, 1, -121}, {0x179, 0x17d, 2, 1},
    {0x181, 0x181, 1, 210}, {0x182, 0x184, 2, 1}, {0x186, 0x186, 1, 206}, {0x187, 0x187, 1, 1},
    {0x189, 0x18a, 1, 205}, {0x18b, 0x18b, 1, 1}, {0x18e, 0x18e, 1, 79}, {0x18f, 0x18f, 1, 202},
    {0x190, 0x190, 1, 203}, {0x191, 0x191, 1, 1}, {0x193, 0x193, 1, 205}, {0x194, 0x194, 1, 207},
    {0x196, 0x196, 1, 211}, {0x197, 0x197, 1, 209}, {0x198, 0x198, 1, 1}, {0x19c, 0x19c, 1, 211},
    {0x19d, 0x19d, 1, 213}, {0x19f, 0x19f, 1, 214}, {0x1a0, 0x1a4, 2, 1}, {0x1a6, 0x1a6, 1, 218},
    {0x1a7, 0x1a7, 1, 1}, {0x1a9, 0x1a9, 1, 218}, {0x1ac, 0x1ac, 1, 1}, {0x1ae, 0x1ae, 1, 218}, {0x1af, 0x1af, 1, 1},
    {0x1b1, 0x1b2, 1, 217}, {0x1b3, 0x1b5, 2, 1}, {0x1b7, 0x1b7, 1, 219}, {0x1b8, 0x1b8, 1, 1}, {0x1bc, 0x1bc, 1, 1},
    {0x1c4, 0x1c4, 1, 2}, {0x1c5, 0x1c5, 1, 1}, {0x1c7, 0x1c7, 1, 2}, {0x1c8, 0x1c8, 1, 1}, {0x1ca, 0x1ca, 1, 2},
    {0x1cb, 0x1db, 2, 1}, {0x1de, 0x1ee, 2, 1}, {0x1f1, 0x1f1, 1, 2}, {0x1f2, 0x1f4, 2, 1}, {0x1f6, 0x1f6, 1, -97},
    {0x1f7, 0x1f7, 1, -56}, {0x1f8, 0x21e, 2, 1}, {0x220, 0x220, 1, -130}, {0x222, 0x232, 2, 1},
    {0x23a, 0x23a, 1, 10795}, {0x23b, 0x23b, 1, 1}, {0x23d, 0x23d, 1, -163}, {0x23e, 0x23e, 1, 10792},
    {0x241, 0x241, 1, 1}, {0x243, 0x243, 1, -195}, {0x244, 0x244, 1, 69}, {0x245, 0x245, 1, 71}, {0x246, 0x24e, 2, 1},
    {0x370, 0x372, 2, 1}, {0x376, 0x376, 1, 1}, {0x37f, 0x37f, 1, 116}, {0x386, 0x386, 1, 38}, {0x388, 0x38a, 1, 37},
    {0x38c, 0x38c, 1, 64}, {0x38e, 0x38f, 1, 63}, {0x391, 0x3a1, 1, 32}, {0x3a3, 0x3ab, 1, 32}, {0x3cf, 0x3cf, 1, 8},
    {0x3d8, 0x3ee, 2, 1}, {0x3f4, 0x3f4, 1, -60}, {0x3f7, 0x3f7, 1, 1}, {0x3f9, 0x3f9, 1, -7}, {0x3fa, 0x3fa, 1, 1},
    {0x3fd, 0x3ff, 1, -130}, {0x400, 0x40f, 1, 80}, {0x410, 0x42f, 1, 32}, {0x460, 0x480, 2, 1}, {0x48a, 0x4be, 2, 1},
    {0x4c0, 0x4c0, 1, 15}, {0x4c1, 0x4cd, 2, 1}, {0x4d0, 0x52e, 2, 1}, {0x531, 0x556, 1, 48},
    {0x10a0, 0x10c5, 1, 7264}, {0x10c7, 0x10c7, 1, 7264}, {0x10cd, 0x10cd, 1, 7264}, {0x13a0, 0x13ef, 1, 38864},
    {0x13f0, 0x13f5, 1, 8}, {0x1c90, 0x1cba, 1, -3008}, {0x1cbd, 0x1cbf, 1, -3008}, {0x1e00, 0x1e94, 2, 1},
    {0x1e9e, 0x1e9e, 1, -7615}, {0x1ea0, 0x1efe, 2, 1}, {0x1f08, 0x1f0f, 1, -8}, {0x1f18, 0x1f1d, 1, -8},
    {0x1f28, 0x1f2f, 1, -8}, {0x1f38, 0x1f3f, 1, -8}, {0x1f48, 0x1f4d, 1, -8}, {0x1f59, 0x1f5f, 2, -8},
    {0x1f68, 0x1f6f, 1, -8}, {0x1f88, 0x1f8f, 1, -8}, {0x1f98, 0x1f9f, 1, -8}, {0x1fa8, 0x1faf, 1, -8},
    {0x1fb8, 0x1fb9, 1, -8}, {0x1fba, 0x1fbb, 1, -74}, {0x1fbc, 0x1fbc, 1, -9}, {0x1fc8, 0x1fcb, 1, -86},
    {0x1fcc, 0x1fcc, 1, -9}, {0x1fd8, 0x1fd9, 1, -8}, {0x1fda, 0x1fdb, 1, -100}, {0x1fe8, 0x1fe9, 1, -8},
    {0x1fea, 0x1feb, 1, -112}, {0x1fec, 0x1fec, 1, -7}, {0x1ff8, 0x1ff9, 1, -128}, {0x1ffa, 0x1ffb, 1, -126},
    {0x1ffc, 0x1ffc, 1, -9}, {0x2126, 0x2126, 1, -7517}, {0x212a, 0x212a, 1, -8383}, {0x212b, 0x212b, 1, -8262},
    {0x2132, 0x2132, 1, 28}, {0x2160, 0x216f, 1, 16}, {0x2183, 0x2183, 1, 1}, {0x24b6, 0x24cf, 1, 26},
    {0x2c00, 0x2c2f, 1, 48}, {0x2c60, 0x2c60, 1, 1}, {0x2c62, 0x2c62, 1, -10743}, {0x2c63, 0x2c63, 1, -3814},
    {0x2c64, 0x2c64, 1, -10727}, {0x2c67, 0x2c6b, 2, 1}, {0x2c6d, 0x2c6d, 1, -10780}, {0x2c6e, 0x2c6e, 1, -10749},
    {0x2c6f, 0x2c6f, 1, -10783}, {0x2c70, 0x2c70, 1, -10782}, {0x2c72, 0x2c72, 1, 1}, {0x2c75, 0x2c75, 1, 1},
    {0x2c7e, 0x2c7f, 1, -10815}, {0x2c80, 0x2ce2, 2, 1}, {0x2ceb, 0x2ced, 2, 1}, {0x2cf2, 0x2cf2, 1, 1},
    {0xa640, 0xa66c, 2, 1}, {0xa680, 0xa69a, 2, 1}, {0xa722, 0xa72e, 2, 1}, {0xa732, 0xa76e, 2, 1},
    {0xa779, 0xa77b, 2, 1}, {0xa77d, 0xa77d, 1, -35332}, {0xa77e, 0xa786, 2, 1}, {0xa78b, 0xa78b, 1, 1},
    {0xa78d, 0xa78d, 1, -42280}, {0xa790, 0xa792, 2, 1}, {0xa796, 0xa7a8, 2, 1}, {0xa7aa, 0xa7aa, 1, -42308},
    {0xa7ab, 0xa7ab, 1, -42319}, {0xa7ac, 0xa7ac, 1, -42315}, {0xa7ad, 0xa7ad, 1, -42305},
    {0xa7ae, 0xa7ae, 1, -42308}, {0xa7b0, 0xa7b0, 1, -42258}, {0xa7b1, 0xa7b1, 1, -42282},
    {0xa7b2, 0xa7b2, 1, -42261}, {0xa7b3, 0xa7b3, 1, 928}, {0xa7b4, 0xa7c2, 2, 1}, {0xa7c4, 0xa7c4, 1, -48},
    {0xa7c5, 0xa7c5, 1, -42307}, {0xa7c6, 0xa7c6, 1, -35384}, {0xa7c7, 0xa7c9, 2, 1}, {0xa7d0, 0xa7d0, 1, 1},
    {0xa7d6, 0xa7d8, 2, 1}, {0xa7f5, 0xa7f5, 1, 1}, {0xff21, 0xff3a, 1, 32}, {0x10400, 0x10427, 1, 40},
    {0x104b0, 0x104d3, 1, 40}, {0x10570, 0x1057a, 1, 39}, {0x1057c, 0x1058a, 1, 39}, {0x1058c, 0x10592, 1, 39},
    {0x10594, 0x10595, 1, 39}, {0x10c80, 0x10cb2, 1, 64}, {0x118a0, 0x118bf, 1, 32}, {0x16e40, 0x16e5f, 1, 32},
    {0x1e900, 0x1e921, 1, 34},
};

struct CasedRun {
    char32_t first;
    char32_t last;
    std::uint8_t step;
};

constexpr CasedRun kCasedSelf[] = {
    {0x61, 0x7a, 1}, {0xb5, 0xb5, 1}, {0xdf, 0xf6, 1}, {0xf8, 0xff, 1}, {0x101, 0x137, 2}, {0x13a, 0x148, 2},
    {0x149, 0x177, 2}, {0x17a, 0x17e, 2}, {0x17f, 0x180, 1}, {0x183, 0x185, 2}, {0x188, 0x188, 1}, {0x18c, 0x18c, 1},
    {0x192, 0x192, 1}, {0x195, 0x195, 1}, {0x199, 0x19a, 1}, {0x19e, 0x19e, 1}, {0x1a1, 0x1a5, 2}, {0x1a8, 0x1a8, 1},
    {0x1ad, 0x1ad, 1}, {0x1b0, 0x1b0, 1}, {0x1b4, 0x1b6, 2}, {0x1b9, 0x1b9, 1}, {0x1bd, 0x1bf, 2}, {0x1c6, 0x1c6, 1},
    {0x1c9, 0x1c9, 1}, {0x1cc, 0x1dc, 2}, {0x1dd, 0x1ef, 2}, {0x1f0, 0x1f0, 1}, {0x1f3, 0x1f5, 2}, {0x1f9, 0x21f, 2},
    {0x223, 0x233, 2}, {0x23c, 0x23c, 1}, {0x23f, 0x240, 1}, {0x242, 0x242, 1}, {0x247, 0x24f, 2}, {0x250, 0x254, 1},
    {0x256, 0x257, 1}, {0x259, 0x25b, 2}, {0x25c, 0x25c, 1}, {0x260, 0x261, 1}, {0x263, 0x265, 2}, {0x266, 0x268, 2},
    {0x269, 0x26c, 1}, {0x26f, 0x271, 2}, {0x272, 0x272, 1}, {0x275, 0x275, 1}, {0x27d, 0x27d, 1}, {0x280, 0x282, 2},
    {0x283, 0x283, 1}, {0x287, 0x28c, 1}, {0x292, 0x292, 1}, {0x29d, 0x29e, 1}, {0x345, 0x345, 1}, {0x371, 0x373, 2},
    {0x377, 0x377, 1}, {0x37b, 0x37d, 1}, {0x390, 0x390, 1}, {0x3ac, 0x3ce, 1}, {0x3d0, 0x3d1, 1}, {0x3d5, 0x3d7, 1},
    {0x3d9, 0x3ef, 2}, {0x3f0, 0x3f3, 1}, {0x3f5, 0x3f5, 1}, {0x3f8, 0x3f8, 1}, {0x3fb, 0x3fb, 1}, {0x430, 0x45f, 1},
    {0x461, 0x481, 2}, {0x48b, 0x4bf, 2}, {0x4c2, 0x4ce, 2}, {0x4cf, 0x52f, 2}, {0x561, 0x587, 1},
    {0x10d0, 0x10fa, 1}, {0x10fd, 0x10ff, 1}, {0x13f8, 0x13fd, 1}, {0x1c80, 0x1c88, 1}, {0x1d79, 0x1d79, 1},
    {0x1d7d, 0x1d7d, 1}, {0x1d8e, 0x1d8e, 1}, {0x1e01, 0x1e95, 2}, {0x1e96, 0x1e9b, 1}, {0x1ea1, 0x1eff, 2},
    {0x1f00, 0x1f07, 1}, {0x1f10, 0x1f15, 1}, {0x1f20, 0x1f27, 1}, {0x1f30, 0x1f37, 1}, {0x1f40, 0x1f45, 1},
    {0x1f50, 0x1f57, 1}, {0x1f60, 0x1f67, 1}, {0x1f70, 0x1f7d, 1}, {0x1f80, 0x1f87, 1}, {0x1f90, 0x1f97, 1},
    {0x1fa0, 0x1fa7, 1}, {0x1fb0, 0x1fb4, 1}, {0x1fb6, 0x1fb7, 1}, {0x1fbe, 0x1fbe, 1}, {0x1fc2, 0x1fc4, 1},
    {0x1fc6, 0x1fc7, 1}, {0x1fd0, 0x1fd3, 1}, {0x1fd6, 0x1fd7, 1}, {0x1fe0, 0x1fe7, 1}, {0x1ff2, 0x1ff4, 1},
    {0x1ff6, 0x1ff7, 1}, {0x214e, 0x214e, 1}, {0x2170, 0x217f, 1}, {0x2184, 0x2184, 1}, {0x24d0, 0x24e9, 1},
    {0x2c30, 0x2c5f, 1}, {0x2c61, 0x2c61, 1}, {0x2c65, 0x2c66, 1}, {0x2c68, 0x2c6c, 2}, {0x2c73, 0x2c73, 1},
    {0x2c76, 0x2c76, 1}, {0x2c81, 0x2ce3, 2}, {0x2cec, 0x2cee, 2}, {0x2cf3, 0x2cf3, 1}, {0x2d00, 0x2d25, 1},
    {0x2d27, 0x2d27, 1}, {0x2d2d, 0x2d2d, 1}, {0xa641, 0xa66d, 2}, {0xa681, 0xa69b, 2}, {0xa723, 0xa72f, 2},
    {0xa733, 0xa76f, 2}, {0xa77a, 0xa77c, 2}, {0xa77f, 0xa787, 2}, {0xa78c, 0xa78c, 1}, {0xa791, 0xa793, 2},
    {0xa794, 0xa794, 1}, {0xa797, 0xa7a9, 2}, {0xa7b5, 0xa7c3, 2}, {0xa7c8, 0xa7ca, 2}, {0xa7d1, 0xa7d1, 1},
    {0xa7d7, 0xa7d9, 2}, {0xa7f6, 0xa7f6, 1}, {0xab53, 0xab53, 1}, {0xab70, 0xabbf, 1}, {0xfb00, 0xfb06, 1},
    {0xfb13, 0xfb17, 1}, {0xff41, 0xff5a, 1}, {0x10428, 0x1044f, 1}, {0x104d8, 0x104fb, 1}, {0x10597, 0x105a1, 1},
    {0x105a3, 0x105b1, 1}, {0x105b3, 0x105b9, 1}, {0x105bb, 0x105bc, 1}, {0x10cc0, 0x10cf2, 1}, {0x118c0, 0x118df, 1},
    {0x16e60, 0x16e7f, 1}, {0x1e922, 0x1e943, 1},
};

// re._casefix._EXTRA_CASES: a lowercase letter → the other lowercase letters with the same uppercase
struct ExtraCase {
    char32_t lower;
    std::array<char32_t, 2> others;  // (0: none)
};

constexpr ExtraCase kExtraCases[] = {
    {0x69, {0x131}}, {0x73, {0x17f}}, {0xb5, {0x3bc}}, {0x131, {0x69}}, {0x17f, {0x73}}, {0x345, {0x3b9, 0x1fbe}},
    {0x390, {0x1fd3}}, {0x3b0, {0x1fe3}}, {0x3b2, {0x3d0}}, {0x3b5, {0x3f5}}, {0x3b8, {0x3d1}},
    {0x3b9, {0x345, 0x1fbe}}, {0x3ba, {0x3f0}}, {0x3bc, {0xb5}}, {0x3c0, {0x3d6}}, {0x3c1, {0x3f1}}, {0x3c2, {0x3c3}},
    {0x3c3, {0x3c2}}, {0x3c6, {0x3d5}}, {0x3d0, {0x3b2}}, {0x3d1, {0x3b8}}, {0x3d5, {0x3c6}}, {0x3d6, {0x3c0}},
    {0x3f0, {0x3ba}}, {0x3f1, {0x3c1}}, {0x3f5, {0x3b5}}, {0x432, {0x1c80}}, {0x434, {0x1c81}}, {0x43e, {0x1c82}},
    {0x441, {0x1c83}}, {0x442, {0x1c84, 0x1c85}}, {0x44a, {0x1c86}}, {0x463, {0x1c87}}, {0x1c80, {0x432}},
    {0x1c81, {0x434}}, {0x1c82, {0x43e}}, {0x1c83, {0x441}}, {0x1c84, {0x442, 0x1c85}}, {0x1c85, {0x442, 0x1c84}},
    {0x1c86, {0x44a}}, {0x1c87, {0x463}}, {0x1c88, {0xa64b}}, {0x1e61, {0x1e9b}}, {0x1e9b, {0x1e61}},
    {0x1fbe, {0x345, 0x3b9}}, {0x1fd3, {0x390}}, {0x1fe3, {0x3b0}}, {0xa64b, {0x1c88}}, {0xfb05, {0xfb06}},
    {0xfb06, {0xfb05}},
};

template <class Run>
const Run* run_of(const Run* begin, const Run* end, char32_t c) {
    const Run* at = std::upper_bound(begin, end, c, [](char32_t value, const Run& run) { return value < run.first; });
    if (at == begin) return nullptr;
    --at;
    return c <= at->last && (c - at->first) % at->step == 0 ? at : nullptr;
}

// _sre.unicode_tolower
char32_t tolower(char32_t c) {
    const LowerRun* run = run_of(std::begin(kLower), std::end(kLower), c);
    return run != nullptr ? static_cast<char32_t>(static_cast<std::int64_t>(c) + run->delta) : c;
}

// _sre.unicode_iscased: the letter has another case (its lowercase or its uppercase is another letter)
bool iscased(char32_t c) {
    return tolower(c) != c || run_of(std::begin(kCasedSelf), std::end(kCasedSelf), c) != nullptr;
}

const ExtraCase* extra_cases(char32_t lower) {
    const auto* at = std::lower_bound(std::begin(kExtraCases), std::end(kExtraCases), lower,
                                      [](const ExtraCase& item, char32_t value) { return item.lower < value; });
    return at != std::end(kExtraCases) && at->lower == lower ? at : nullptr;
}

// --- text as code points ----------------------------------------------------------------------------------------------

std::u32string decode(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (i + n > text.size()) n = text.size() - i;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
        for (std::size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
        out.push_back(cp);
        i += n;
    }
    return out;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

std::string encode(std::u32string_view text) {
    std::string out;
    for (const char32_t cp : text) append_utf8(out, cp);
    return out;
}

// --- the replacement template (re._parser.parse_template) ---------------------------------------------------------------

constexpr std::string_view kMaxGroups = "1073741823";  // _sre.MAXGROUPS (64-bit builds)

bool is_digit(char32_t c) { return c >= U'0' && c <= U'9'; }
bool is_octal(char32_t c) { return c >= U'0' && c <= U'7'; }
bool is_ascii_letter(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }

// str.isidentifier: ASCII as Python has it; beyond ASCII, XID_Start and XID_Continue as Qt's categories give them
// (letters and letter numbers; then marks, digits and connectors, and the few Unicode adds by name).
bool id_start(char32_t c) {
    if (c < 0x80) return is_ascii_letter(c) || c == U'_';
    if (c == 0x1885 || c == 0x1886 || c == 0x2118 || c == 0x212e) return true;
    switch (QChar::category(c)) {
        case QChar::Letter_Uppercase:
        case QChar::Letter_Lowercase:
        case QChar::Letter_Titlecase:
        case QChar::Letter_Modifier:
        case QChar::Letter_Other:
        case QChar::Number_Letter: return true;
        default: return false;
    }
}

bool id_continue(char32_t c) {
    if (c < 0x80) return is_ascii_letter(c) || is_digit(c) || c == U'_';
    if (id_start(c) || c == 0xb7 || c == 0x387 || (c >= 0x1369 && c <= 0x1371) || c == 0x19da) return true;
    switch (QChar::category(c)) {
        case QChar::Mark_NonSpacing:
        case QChar::Mark_SpacingCombining:
        case QChar::Number_DecimalDigit:
        case QChar::Punctuation_Connector: return true;
        default: return false;
    }
}

bool is_identifier(const std::u32string& name) {
    return !name.empty() && id_start(name.front()) && std::all_of(name.begin() + 1, name.end(), id_continue);
}

// re.error(msg, pattern, pos): "<msg> at position <pos>", and its line and column when the pattern has more than one
[[noreturn]] void re_error(const std::string& msg, const std::u32string& pattern, std::int64_t pos) {
    std::string text = msg + " at position " + std::to_string(pos);
    if (pattern.find(U'\n') != std::u32string::npos) {
        // lineno = pattern.count("\n", 0, pos) + 1; colno = pos - pattern.rfind("\n", 0, pos)
        const auto end = static_cast<std::size_t>(std::clamp<std::int64_t>(pos, 0, static_cast<std::int64_t>(pattern.size())));
        const auto lines = std::count(pattern.begin(), pattern.begin() + static_cast<std::ptrdiff_t>(end), U'\n');
        const std::size_t last = end == 0 ? std::u32string::npos : pattern.rfind(U'\n', end - 1);
        const std::int64_t column = pos - (last == std::u32string::npos ? -1 : static_cast<std::int64_t>(last));
        text += " (line " + std::to_string(lines + 1) + ", column " + std::to_string(column) + ")";
    }
    throw PyUncaught("re.error", text);
}

// re._parser.Tokenizer over a str: one character at a time, a backslash with the character after it
class Tokenizer {
public:
    explicit Tokenizer(const std::u32string& text) : text_(text) { advance(); }

    std::optional<std::u32string> get() {
        std::optional<std::u32string> token = next_;
        advance();
        return token;
    }

    bool match(char32_t c) {
        if (next_ && *next_ == std::u32string(1, c)) {
            advance();
            return true;
        }
        return false;
    }

    // whether the next token is the one character `c` passes
    template <class Pred>
    bool next_is(Pred pred) const {
        return next_ && next_->size() == 1 && pred((*next_)[0]);
    }

    std::u32string getuntil(char32_t terminator, const std::string& name) {
        std::u32string result;
        for (;;) {
            const std::optional<std::u32string> c = next_;
            advance();
            if (!c) {
                if (result.empty()) error("missing " + name);
                error("missing " + encode(std::u32string(1, terminator)) + ", unterminated name",
                      static_cast<std::int64_t>(result.size()));
            }
            if (*c == std::u32string(1, terminator)) {
                if (result.empty()) error("missing " + name, 1);
                break;
            }
            result += *c;
        }
        return result;
    }

    [[noreturn]] void error(const std::string& msg, std::int64_t offset = 0) const { re_error(msg, text_, tell() - offset); }

private:
    std::int64_t tell() const {
        return static_cast<std::int64_t>(index_) - static_cast<std::int64_t>(next_ ? next_->size() : 0);
    }

    void advance() {
        std::size_t index = index_;
        if (index >= text_.size()) {
            next_.reset();
            return;
        }
        std::u32string token(1, text_[index]);
        if (text_[index] == U'\\') {
            ++index;
            if (index >= text_.size()) re_error("bad escape (end of pattern)", text_, static_cast<std::int64_t>(text_.size()) - 1);
            token += text_[index];
        }
        index_ = index + 1;
        next_ = std::move(token);
    }

    const std::u32string& text_;
    std::size_t index_ = 0;
    std::optional<std::u32string> next_;
};

// A part of the replacement: words as they are, or the whole match (\g<0>, the only group of a pattern with none)
struct Piece {
    bool match = false;
    std::string words;
};

// int(digits) as Python prints it (no leading zeros; digits of any length)
std::string decimal(std::u32string_view digits) {
    std::size_t first = 0;
    while (first + 1 < digits.size() && digits[first] == U'0') ++first;
    return encode(digits.substr(first));
}

std::uint32_t octal(std::u32string_view digits) {
    std::uint32_t value = 0;
    for (const char32_t c : digits) value = value * 8 + static_cast<std::uint32_t>(c - U'0');
    return value;
}

// parse_template(source, pattern) for a pattern without groups
std::vector<Piece> parse_template(const std::u32string& source) {
    Tokenizer s(source);
    std::vector<Piece> result;
    std::u32string literal;
    const auto addliteral = [&] {
        result.push_back(Piece{false, encode(literal)});
        literal.clear();
    };
    const auto addgroup = [&](const std::string& index, std::int64_t pos) {
        if (index != "0") s.error("invalid group reference " + index, pos);  // (index > pattern.groups)
        addliteral();
        result.push_back(Piece{true, {}});
    };
    for (;;) {
        std::optional<std::u32string> token = s.get();
        if (!token) break;
        std::u32string& t = *token;
        if (t[0] != U'\\') {
            literal += t;
            continue;
        }
        const char32_t c = t[1];
        if (c == U'g') {
            if (!s.match(U'<')) s.error("missing <");
            const std::u32string name = s.getuntil(U'>', "group name");
            const auto offset = static_cast<std::int64_t>(name.size()) + 1;
            if (!std::all_of(name.begin(), name.end(), is_digit)) {
                if (!is_identifier(name)) s.error("bad character in group name " + py_repr_str(encode(name)), offset);
                throw PyUncaught("IndexError", "unknown group name " + py_repr_str(encode(name)));  // (groupindex[name])
            }
            const std::string index = decimal(name);
            if (index.size() > kMaxGroups.size() || (index.size() == kMaxGroups.size() && index >= kMaxGroups)) {
                s.error("invalid group reference " + index, offset);
            }
            addgroup(index, offset);
        } else if (c == U'0') {
            if (s.next_is(is_octal)) {
                t += *s.get();
                if (s.next_is(is_octal)) t += *s.get();
            }
            literal += static_cast<char32_t>(octal(std::u32string_view(t).substr(1)) & 0xff);
        } else if (is_digit(c)) {
            bool isoctal = false;
            if (s.next_is(is_digit)) {
                t += *s.get();
                if (is_octal(c) && is_octal(t[2]) && s.next_is(is_octal)) {
                    t += *s.get();
                    isoctal = true;
                    const std::uint32_t value = octal(std::u32string_view(t).substr(1));
                    if (value > 0377) {
                        s.error("octal escape value " + encode(t) + " outside of range 0-0o377", static_cast<std::int64_t>(t.size()));
                    }
                    literal += static_cast<char32_t>(value);
                }
            }
            if (!isoctal) addgroup(decimal(std::u32string_view(t).substr(1)), static_cast<std::int64_t>(t.size()) - 1);
        } else {
            // ESCAPES: \a \b \f \n \r \t \v \\; another ASCII letter is refused; anything else stays as written
            static constexpr std::pair<char32_t, char32_t> kEscapes[] = {{U'a', U'\a'}, {U'b', U'\b'}, {U'f', U'\f'},
                                                                         {U'n', U'\n'}, {U'r', U'\r'}, {U't', U'\t'},
                                                                         {U'v', U'\v'}, {U'\\', U'\\'}};
            const auto* escape = std::find_if(std::begin(kEscapes), std::end(kEscapes), [c](const auto& e) { return e.first == c; });
            if (escape != std::end(kEscapes)) {
                literal += escape->second;
            } else {
                if (is_ascii_letter(c)) s.error("bad escape " + encode(t), static_cast<std::int64_t>(t.size()));
                literal += t;
            }
        }
    }
    addliteral();
    return result;
}

}  // namespace

PyLiteralPattern::PyLiteralPattern(std::string_view find, bool ignore_case) : find_(decode(find)), ignore_case_(ignore_case) {}

std::pair<std::string, std::int64_t> PyLiteralPattern::subn(std::string_view repl, std::string_view text) const {
    // _sre's pattern_subx: a template without a backslash is used as it is; any other is compiled first
    const std::vector<Piece> pieces =
        repl.find('\\') == std::string_view::npos ? std::vector<Piece>{Piece{false, std::string(repl)}} : parse_template(decode(repl));
    const std::u32string chars = decode(text);
    const std::size_t n = find_.size();
    // each character of the pattern as _sre compiles it: LITERAL (the same character), or, with IGNORECASE and a cased
    // character, LITERAL_UNI_IGNORE (a character whose lowercase is the pattern's) or IN_UNI_IGNORE (one of the
    // lowercases _EXTRA_CASES gives it)
    const auto matches_at = [&](std::size_t at) {
        for (std::size_t k = 0; k < n; ++k) {
            const char32_t p = find_[k];
            const char32_t t = chars[at + k];
            if (!ignore_case_ || !iscased(p)) {
                if (t != p) return false;
                continue;
            }
            const char32_t lo = tolower(p);
            const char32_t seen = tolower(t);
            if (seen == lo) continue;
            const ExtraCase* extra = extra_cases(lo);
            if (extra == nullptr || std::find(extra->others.begin(), extra->others.end(), seen) == extra->others.end() || seen == 0) {
                return false;
            }
        }
        return true;
    };
    std::string out;
    std::int64_t count = 0;
    std::size_t i = 0;
    while (i < chars.size()) {
        if (n > 0 && i + n <= chars.size() && matches_at(i)) {
            const std::string matched = encode(std::u32string_view(chars).substr(i, n));
            for (const Piece& piece : pieces) out += piece.match ? matched : piece.words;
            ++count;
            i += n;
            continue;
        }
        append_utf8(out, chars[i]);
        ++i;
    }
    return {std::move(out), count};
}

}  // namespace genko::core
