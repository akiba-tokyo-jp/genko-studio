#pragma once

#include <cstdint>
#include <string_view>

// PhotoCraft v0.2.0 crates/paint/src/rng.rs (MIT: native/third_party/photocraft/NOTICE.md): counter-based randomness.
// Every random value is a pure hash of (seed, index, stream), so a dab's jitter depends only on the line's seed and the
// dab's place along it — never on how the points came in, on threads or on earlier draws: a line drawn again (on
// screen, in print, after saving, on another computer) has the same dabs.

namespace genko::render::dabs {

// SplitMix64's finaliser.
constexpr std::uint64_t mix64(std::uint64_t z) {
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Uniform in [0, 1) (24 bits: exact in float).
constexpr float rand01(std::uint64_t seed, std::uint64_t index, std::uint64_t stream) {
    const std::uint64_t h = mix64(seed ^ mix64(index * 0xD1B54A32D192ED03ULL ^ stream * 0x8CB92BA72F3D8DD7ULL));
    return static_cast<float>(h >> 40) / static_cast<float>(1ULL << 24);
}

// Uniform in [-1, 1).
constexpr float rand_signed(std::uint64_t seed, std::uint64_t index, std::uint64_t stream) {
    return rand01(seed, index, stream) * 2.0f - 1.0f;
}

// A hash of whole coordinates to [0, 1) (noise, the procedural tips and patterns).
constexpr float hash2(std::int32_t x, std::int32_t y, std::uint64_t salt) {
    const std::uint64_t k = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y);
    return static_cast<float>(mix64(k ^ salt * 0xA24BAED4963EE407ULL) >> 40) / static_cast<float>(1ULL << 24);
}

// One stream per random quantity, so they are independent.
namespace stream {
inline constexpr std::uint64_t kSize = 1, kAngle = 2, kRoundness = 3, kFlipX = 4, kFlipY = 5, kScatterX = 6, kScatterY = 7,
                               kCount = 8, kOpacity = 9, kFlow = 10, kFgBg = 11, kHue = 12, kSat = 13, kBright = 14, kDepth = 15,
                               kDualX = 16, kDualY = 17, kDualFlip = 18;
}

// FNV-1a 64 of some bytes (a line's id: its seed, as a paper's "stroke" coordinates take it).
constexpr std::uint64_t fnv1a(std::string_view bytes) {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x00000100000001B3ULL;
    }
    return h;
}

// A seed from some bytes (FNV-1a, then mixed: upstream's seed_from_bytes).
constexpr std::uint64_t seed_from_bytes(std::string_view bytes) { return mix64(fnv1a(bytes)); }

}  // namespace genko::render::dabs
