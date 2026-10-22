#include "render/dabs/procedural.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

#include "render/dabs/maths.hpp"
#include "render/dabs/rng.hpp"

// PhotoCraft v0.2.0 crates/paint/src/procedural.rs, MIT: native/third_party/photocraft/NOTICE.md. Upstream's f32
// arithmetic is kept; its sin, cos and atan2 are the engine's fixed ones.

namespace genko::render::dabs {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = 6.28318530717958647692f;

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float sinr(float radians) { return static_cast<float>(sin_cos(static_cast<double>(radians) * 180.0 / 3.141592653589793).first); }
float cosr(float radians) { return static_cast<float>(sin_cos(static_cast<double>(radians) * 180.0 / 3.141592653589793).second); }
float atan2r(float y, float x) { return static_cast<float>(atan2_degrees(y, x) * 3.141592653589793 / 180.0); }

std::int32_t rem(std::int32_t a, std::int32_t m) {
    const std::int32_t r = a % m;
    return r < 0 ? r + m : r;
}

float fract(float v) { return v - std::trunc(v); }  // (Rust's f32::fract: the sign of v)

// Tileable value noise with `period` lattice cells.
float value_noise(float x, float y, std::int32_t period, std::uint64_t salt) {
    const float x0 = std::floor(x), y0 = std::floor(y);
    const float tx = smooth(x - x0), ty = smooth(y - y0);
    const auto xi = static_cast<std::int32_t>(x0), yi = static_cast<std::int32_t>(y0);
    const auto h = [&](std::int32_t i, std::int32_t j) { return hash2(rem(i, period), rem(j, period), salt); };
    const float a = h(xi, yi) + (h(xi + 1, yi) - h(xi, yi)) * tx;
    const float b = h(xi, yi + 1) + (h(xi + 1, yi + 1) - h(xi, yi + 1)) * tx;
    return a + (b - a) * ty;
}

// Tileable fractal noise in 0..1.
float fbm(float x, float y, float size, std::int32_t cells, std::uint32_t octaves, std::uint64_t salt) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    std::int32_t c = cells;
    for (std::uint32_t o = 0; o < octaves; ++o) {
        sum += value_noise(x / size * static_cast<float>(c), y / size * static_cast<float>(c), c, salt + std::uint64_t(o) * 7919) * amp;
        norm += amp;
        amp *= 0.5f;
        c *= 2;
    }
    return sum / norm;
}

float powi(float v, int n) {
    float out = 1.0f;
    for (int i = 0; i < n; ++i) out *= v;
    return out;
}

}  // namespace

PatternImage pattern(std::string_view style, std::uint32_t size, std::uint32_t seed) {
    const std::size_t n = std::clamp<std::uint32_t>(size, 8, 1024);
    const float s = static_cast<float>(n);
    const std::uint64_t salt = std::uint64_t(seed) * 0x0100000001B3ULL + 17;
    PatternImage out{n, n, std::vector<float>(n * n, 0.0f)};
    for (std::size_t y = 0; y < n; ++y) {
        for (std::size_t x = 0; x < n; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            float v = 0.0f;
            if (style == "noise") {
                v = fbm(fx, fy, s, 8, 4, salt);
            } else if (style == "canvas") {
                // over and under: threads every 8 px, alternating which way is on top
                const float period = 8.0f;
                const auto cx = static_cast<std::int32_t>(std::floor(fx / period)), cy = static_cast<std::int32_t>(std::floor(fy / period));
                const float u = fract(fx / period), w = fract(fy / period);
                const float warp = sinr(kPi * u), weft = sinr(kPi * w);
                const float top = (cx + cy) % 2 == 0 ? warp : weft;
                v = std::clamp(0.25f + 0.65f * top + (hash2(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), salt) - 0.5f) * 0.15f, 0.0f, 1.0f);
            } else if (style == "dots") {
                const float period = 8.0f;
                const float u = fract(fx / period) - 0.5f, w = fract(fy / period) - 0.5f;
                const float d = std::sqrt(u * u + w * w);
                v = 1.0f - smooth(std::clamp((0.35f - d) / 0.1f + 0.5f, 0.0f, 1.0f));
            } else {  // paper
                const float base = fbm(fx, fy, s, 16, 4, salt);
                const float grain = hash2(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), salt ^ 0x55);
                v = std::clamp((base - 0.5f) * 2.2f + 0.5f + (grain - 0.5f) * 0.35f, 0.0f, 1.0f);
            }
            out.data[y * n + x] = v;
        }
    }
    return out;
}

GrayTile chalk_tip(std::uint32_t n, std::uint32_t seed) {
    const std::uint64_t salt = std::uint64_t(seed) + 101;
    const float c = static_cast<float>(n) / 2.0f;
    return GrayTile::from_fn(n, n, [&](std::uint32_t x, std::uint32_t y) {
        const float dx = static_cast<float>(x) + 0.5f - c, dy = static_cast<float>(y) + 0.5f - c;
        const float r = std::sqrt(dx * dx + dy * dy) / c;
        const float ang = atan2r(dy, dx);
        const float edge = 0.8f + 0.15f * value_noise((ang + kPi) * 3.0f, 0.5f, 19, salt);
        const float inside = std::clamp((edge - r) * 8.0f, 0.0f, 1.0f);
        const float grain = hash2(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), salt);
        return inside * (grain < 0.2f ? 0.2f : 0.7f + 0.3f * grain);
    });
}

GrayTile spatter_tip(std::uint32_t n, std::uint32_t seed, std::uint32_t drops) {
    const std::uint64_t salt = std::uint64_t(seed) + 202;
    const float s = static_cast<float>(n);
    std::vector<std::tuple<float, float, float>> blobs;
    for (std::uint32_t i = 0; i < drops; ++i) {
        const auto k = static_cast<std::int32_t>(i);
        const float a = hash2(k, 0, salt) * kTau;
        const float d = std::sqrt(hash2(k, 1, salt)) * 0.38f * s;
        const float r = (0.03f + 0.09f * powi(hash2(k, 2, salt), 2)) * s;
        blobs.emplace_back(s / 2.0f + cosr(a) * d, s / 2.0f + sinr(a) * d, r);
    }
    return GrayTile::from_fn(n, n, [&](std::uint32_t x, std::uint32_t y) {
        const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
        float acc = 0.0f;
        for (const auto& [bx, by, r] : blobs) {
            const float d = std::sqrt(powi(px - bx, 2) + powi(py - by, 2));
            acc = std::max(acc, std::clamp(r + 0.5f - d, 0.0f, 1.0f));
        }
        return acc;
    });
}

GrayTile bristle_tip(std::uint32_t n, std::uint32_t seed) {
    const std::uint64_t salt = std::uint64_t(seed) + 303;
    const float c = static_cast<float>(n) / 2.0f;
    return GrayTile::from_fn(n, n, [&](std::uint32_t x, std::uint32_t y) {
        const float dx = (static_cast<float>(x) + 0.5f - c) / c, dy = (static_cast<float>(y) + 0.5f - c) / c;
        const float r = std::sqrt(dx * dx + powi(dy * 1.8f, 2));
        if (r > 1.0f) return 0.0f;
        const float hair = hash2(0, static_cast<std::int32_t>(static_cast<float>(y) / 1.5f), salt);
        const float density = hair < 0.25f ? 0.0f : 0.5f + 0.5f * hair;
        return density * (1.0f - powi(r, 4));
    });
}

GrayTile charcoal_tip(std::uint32_t n, std::uint32_t seed) {
    const std::uint64_t salt = std::uint64_t(seed) + 404;
    const float c = static_cast<float>(n) / 2.0f;
    return GrayTile::from_fn(n, n, [&](std::uint32_t x, std::uint32_t y) {
        const float dx = (static_cast<float>(x) + 0.5f - c) / c, dy = (static_cast<float>(y) + 0.5f - c) / c;
        const float r = std::sqrt(dx * dx + powi(dy * 1.6f, 2));
        const float edge = 0.85f + 0.15f * value_noise(dx * 6.0f + 9.0f, dy * 6.0f + 9.0f, 64, salt);
        if (r > edge) return 0.0f;
        const float row = hash2(0, static_cast<std::int32_t>(y), salt);
        const float along = value_noise(static_cast<float>(x) / 6.0f, static_cast<float>(y) / 1.5f, 1 << 12, salt ^ 0x77);
        const float grit = hash2(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), salt ^ 0x13);
        const float v = (0.35f + 0.65f * row) * (0.4f + 0.6f * along) * (grit < 0.15f ? 0.2f : 1.0f);
        return v * std::clamp((edge - r) * 6.0f, 0.0f, 1.0f);
    });
}

GrayTile leaf_tip(std::uint32_t n) {
    const float c = static_cast<float>(n) / 2.0f;
    return GrayTile::from_fn(n, n, [&](std::uint32_t x, std::uint32_t y) {
        const float u = (static_cast<float>(x) + 0.5f - c) / c, v = (static_cast<float>(y) + 0.5f - c) / c;
        const float t = std::clamp((u + 1.0f) / 2.0f, 0.0f, 1.0f);
        const float half = 0.42f * sinr(kPi * t) * (1.0f - 0.25f * t);
        const float inside = std::clamp((half - std::fabs(v)) * c, 0.0f, 1.0f);
        const float rib = 1.0f - 0.45f * (1.0f - std::clamp(std::fabs(v) * c / 1.2f, 0.0f, 1.0f));
        const float veins = 1.0f - 0.2f * (1.0f - std::clamp(std::fabs(fract(u * 7.0f - std::fabs(v) * 5.0f) - 0.5f) * 8.0f, 0.0f, 1.0f));
        const float stem = u < -0.75f && std::fabs(v) * c < 0.9f ? 1.0f : 0.0f;
        return std::max(inside * rib * veins, stem);
    });
}

GrayTile grass_tip(std::uint32_t n, std::uint32_t seed, std::uint32_t blades) {
    const std::uint64_t salt = std::uint64_t(seed) + 505;
    const float s = static_cast<float>(n);
    std::vector<std::tuple<float, float, float, float>> bl;
    for (std::uint32_t i = 0; i < blades; ++i) {
        const auto k = static_cast<std::int32_t>(i);
        bl.emplace_back((0.3f + 0.4f * hash2(k, 0, salt)) * s, (0.55f + 0.42f * hash2(k, 1, salt)) * s, (hash2(k, 2, salt) - 0.5f) * 0.9f,
                        (0.025f + 0.025f * hash2(k, 3, salt)) * s);
    }
    return GrayTile::from_fn(n, n, [&](std::uint32_t x, std::uint32_t y) {
        const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
        const float up = s - py;
        float acc = 0.0f;
        for (const auto& [base, height, lean, width] : bl) {
            if (up < 0.0f || up > height) continue;
            const float t = up / height;
            const float cx = base + lean * height * t * t;
            const float w = width * (1.0f - t) + 0.3f;
            acc = std::max(acc, std::clamp(w - std::fabs(px - cx), 0.0f, 1.0f));
        }
        return acc;
    });
}

GrayTile sponge_tip(std::uint32_t n, std::uint32_t seed) {
    const std::uint64_t salt = std::uint64_t(seed) + 606;
    const float c = static_cast<float>(n) / 2.0f;
    return GrayTile::from_fn(n, n, [&](std::uint32_t x, std::uint32_t y) {
        const float dx = static_cast<float>(x) + 0.5f - c, dy = static_cast<float>(y) + 0.5f - c;
        const float r = std::sqrt(dx * dx + dy * dy) / c;
        const float edge = 0.78f + 0.2f * value_noise((atan2r(dy, dx) + kPi) * 2.5f, 0.5f, 16, salt);
        const float disc = std::clamp((edge - r) * 6.0f, 0.0f, 1.0f);
        const float pores = fbm(static_cast<float>(x), static_cast<float>(y), static_cast<float>(n), 6, 3, salt);
        return disc * std::clamp((pores - 0.42f) * 6.0f, 0.0f, 1.0f);
    });
}

GrayTile star_tip(std::uint32_t n) {
    const float c = static_cast<float>(n) / 2.0f;
    return GrayTile::from_fn(n, n, [&](std::uint32_t x, std::uint32_t y) {
        const float u = std::fabs((static_cast<float>(x) + 0.5f - c) / c), v = std::fabs((static_cast<float>(y) + 0.5f - c) / c);
        const auto ray = [](float a, float b) { return std::max((1.0f - a) * (1.0f - std::min(b / (0.08f * (1.0f - a) + 0.005f), 1.0f)), 0.0f); };
        const float glow = powi(std::max(1.0f - std::sqrt(u * u + v * v) * 2.5f, 0.0f), 2);
        return std::min(std::max({ray(u, v), ray(v, u), glow}), 1.0f);
    });
}

GrayTile rake_tip(std::uint32_t n, std::uint32_t seed, std::uint32_t bristles) {
    const std::uint64_t salt = std::uint64_t(seed) + 707;
    const float s = static_cast<float>(n);
    const std::uint32_t k = std::max<std::uint32_t>(bristles, 1);
    std::vector<std::tuple<float, float, float>> dots;
    for (std::uint32_t i = 0; i < k; ++i) {
        const auto j = static_cast<std::int32_t>(i);
        dots.emplace_back(s * (static_cast<float>(i) + 0.5f) / static_cast<float>(k), s / 2.0f + (hash2(j, 0, salt) - 0.5f) * s * 0.08f,
                          s / static_cast<float>(k) * (0.22f + 0.18f * hash2(j, 1, salt)));
    }
    return GrayTile::from_fn(n, n, [&](std::uint32_t x, std::uint32_t y) {
        const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
        float acc = 0.0f;
        for (const auto& [dx, dy, r] : dots) {
            const float d = std::sqrt(powi(px - dx, 2) + powi(py - dy, 2));
            acc = std::max(acc, std::clamp(r + 0.5f - d, 0.0f, 1.0f) * 0.9f);
        }
        return acc;
    });
}

}  // namespace genko::render::dabs
