// PC4 節 D4 (SPEC.md COLOR-08「フィルター」): フィルター → 描画's kernels — 雲模様 1・雲模様 2 (PhotoCraft v0.2.0's
// engine/src/extra_cmds.rs 208-260: hash, clouds_value, clouds), ファイバー・逆光・照明効果 (algo/src/render.rs 1-278: fibers,
// lens_flare, lighting; noise.rs hash01; fxutil.rs smoothstep), MIT: native/third_party/photocraft/NOTICE.md. They work the
// colours (on a page of inks the colours its inks mean, back by its rules: FilterInks::Meaning). 雲模様 and ファイバー draw
// their pattern at each pixel's place in the working 200 dpi (the same on the paper at every resolution: design 2.2) and
// fill the base box (a render: Kernel::fills); 雲模様 2 lays the clouds on the pixels by their difference; 逆光 and 照明効果
// are sized by the base box itself (shares of it), 照明効果's bumps read over the working pixel's span. Upstream runs 雲模様
// outside its filters (no smart filter, no 前回のフィルター, the colours not recorded): here they are filter_raster's kinds with
// their colours in the settings (design 9 章).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "core/pyops.hpp"
#include "render/fx.hpp"
#include "render/fx_d2.hpp"

namespace genko::render::fx {

namespace {

using core::Json;
using d2::number;
using d2::text;

constexpr float kPi = std::numbers::pi_v<float>;

float radians(float degrees) { return degrees * kPi / 180.0f; }

// noise.rs hash01: a hash of a place, a channel and a seed in [0, 1).
float hash01(int x, int y, std::uint32_t c, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343u ^ static_cast<std::uint32_t>(y) * 0xd8163841u ^ c * 0xcb1ab31fu ^ seed * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / static_cast<float>(1u << 24);
}

float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return x >= e1 ? 1.0f : 0.0f;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A pixel's centre on the working 200 dpi (the layer's grid scaled by s; a part of it worked alone: its place added),
// upstream's pixel coordinate at s 1.
float working(int v, int origin, double s) { return static_cast<float>((static_cast<double>(v) + origin + 0.5) / s - 0.5); }

// --- 雲模様 (extra_cmds.rs 208-260) ------------------------------------------------------------------------------------

float clouds_hash(int x, int y, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343u ^ static_cast<std::uint32_t>(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0x00ffffffu) / static_cast<float>(0x00ffffffu);
}

// Smooth value noise summed over octaves (fBm), 0..1, stretched to use the colours' whole range.
float clouds_value(float x, float y, float base, std::uint32_t seed) {
    float sum = 0.0f, amp = 1.0f, freq = 1.0f / base, norm = 0.0f;
    for (std::uint32_t o = 0; o < 8; ++o) {
        const float fx = x * freq, fy = y * freq;
        const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
        const float tx = fx - static_cast<float>(ix), ty = fy - static_cast<float>(iy);
        const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
        const std::uint32_t s = seed + o * 7919u;
        const float top = clouds_hash(ix, iy, s) + (clouds_hash(ix + 1, iy, s) - clouds_hash(ix, iy, s)) * sx;
        const float bot = clouds_hash(ix, iy + 1, s) + (clouds_hash(ix + 1, iy + 1, s) - clouds_hash(ix, iy + 1, s)) * sx;
        sum += (top + (bot - top) * sy) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return std::clamp((sum / norm - 0.5f) * 2.2f + 0.5f, 0.0f, 1.0f);
}

// The clouds' size: a quarter of the page's longer side in working pixels (upstream: of the document's), 16..512.
float clouds_base(const Ctx& ctx) {
    const double longest = std::max(ctx.grid_width, ctx.grid_height) / std::max(ctx.s, 1e-9);
    return static_cast<float>(std::clamp(longest / 4.0, 16.0, 512.0));
}

std::vector<float> clouds(const Json& params, const Window& src, const Rect& out, const Ctx& ctx, bool difference) {
    const std::size_t n = src.ch;
    const std::uint32_t seed = d2::seed_of(params);
    const auto fg = d2::colour(params, "foreground", {0, 0, 0});
    const auto bg = d2::colour(params, "background", {1, 1, 1});
    const float base = clouds_base(ctx);
    std::vector<float> res = src.crop(out);
    std::size_t i = 0;
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x, i += n) {
            const float t = clouds_value(working(x, ctx.x0, ctx.s), working(y, ctx.y0, ctx.s), base, seed);
            float* px = res.data() + i;
            for (std::size_t k = 0; k < 3; ++k) {
                const float c = fg[k] + (bg[k] - fg[k]) * t;
                px[k] = difference ? std::fabs(px[k] - c) : c;
            }
            if (!difference) px[n - 1] = 1.0f;
        }
    return res;
}

// --- ファイバー (render.rs fibers) --------------------------------------------------------------------------------------

float value_noise(float x, float y, std::uint32_t seed) {
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const float tx = x - static_cast<float>(ix), ty = y - static_cast<float>(iy);
    const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
    const auto h = [seed](int a, int b) { return hash01(a, b, 0, seed); };
    const float top = h(ix, iy) + (h(ix + 1, iy) - h(ix, iy)) * sx;
    const float bot = h(ix, iy + 1) + (h(ix + 1, iy + 1) - h(ix, iy + 1)) * sx;
    return top + (bot - top) * sy;
}

std::vector<float> fibers(const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
    const std::size_t n = src.ch;
    const float var = std::clamp(static_cast<float>(number(params, "variance", 16)), 1.0f, 64.0f);
    const float st = std::clamp(static_cast<float>(number(params, "strength", 4)), 1.0f, 64.0f);
    const std::uint32_t seed = d2::seed_of(params);
    const auto fg = d2::colour(params, "foreground", {0, 0, 0});
    const auto bg = d2::colour(params, "background", {1, 1, 1});
    // (the frequency across rises with the variance, down the fibres falls with the strength)
    const float fx = 0.08f + var / 64.0f * 0.6f;
    const float fy = fx / (2.0f + st * 1.5f) * (0.5f + var / 32.0f);
    std::vector<float> res = src.crop(out);
    std::size_t i = 0;
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x, i += n) {
            const float xf = working(x, ctx.x0, ctx.s), yf = working(y, ctx.y0, ctx.s);
            float sum = 0.0f, amp = 1.0f, norm = 0.0f, ax = fx, ay = fy;
            for (std::uint32_t o = 0; o < 4; ++o) {
                sum += value_noise(xf * ax, yf * ay, seed + o * 977u) * amp;
                norm += amp;
                amp *= 0.55f;
                ax *= 2.0f;
                ay *= 2.0f;
            }
            // (a fine grain down each column: the hairlines)
            const float grain = value_noise(xf * 0.9f, yf * fy * 0.25f, seed ^ 0x5bd1u) - 0.5f;
            const float t = std::clamp((sum / norm - 0.5f) * (1.6f + var / 40.0f) + 0.5f + grain * 0.25f, 0.0f, 1.0f);
            float* px = res.data() + i;
            for (std::size_t k = 0; k < 3; ++k) px[k] = fg[k] + (bg[k] - fg[k]) * t;
            px[n - 1] = 1.0f;
        }
    return res;
}

// --- 逆光 (render.rs lens_flare) ------------------------------------------------------------------------------------------

struct Ghost {
    float t, r;
    std::array<float, 3> tint;
    float k;
};

std::vector<float> lens_flare(const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
    const std::size_t n = src.ch;
    const Rect& b = ctx.bounds;
    const float bw = static_cast<float>(b.width()), bh = static_cast<float>(b.height());
    const float diag = std::max(std::hypot(bw, bh), 1.0f);
    const float cx = static_cast<float>(b.x0) + static_cast<float>(number(params, "center_x", 0.5)) * bw;
    const float cy = static_cast<float>(b.y0) + static_cast<float>(number(params, "center_y", 0.5)) * bh;
    const float mx = static_cast<float>(b.x0) + bw / 2.0f, my = static_cast<float>(b.y0) + bh / 2.0f;
    const float bright = std::clamp(static_cast<float>(number(params, "brightness", 100)), 10.0f, 300.0f) / 100.0f;
    const float s = diag / 1000.0f;  // (its elements sized by the base box: the same at every resolution)
    const std::string lens = text(params, "lens", "zoom");
    float core = 14, glow = 90, ring_r = 0, ring_w = 0, rays = 0.5f, streak = 0;
    std::vector<Ghost> ghosts;
    if (lens == "prime35") {
        core = 12, glow = 70, ring_r = 90, ring_w = 6, rays = 0.3f;
        ghosts = {{0.7f, 18, {0.6f, 1.0f, 0.7f}, 0.12f}, {1.4f, 30, {1.0f, 0.7f, 0.5f}, 0.09f}};
    } else if (lens == "prime105") {
        core = 22, glow = 130, rays = 0.8f;
        ghosts = {{0.5f, 14, {1.0f, 0.8f, 0.5f}, 0.12f}, {1.6f, 50, {0.6f, 0.7f, 1.0f}, 0.07f}};
    } else if (lens == "movie_prime") {
        core = 10, glow = 60, rays = 0.2f, streak = 1;
        ghosts = {{0.4f, 8, {0.5f, 0.7f, 1.0f}, 0.2f}, {1.0f, 20, {0.4f, 0.6f, 1.0f}, 0.12f}, {1.6f, 12, {0.5f, 0.8f, 1.0f}, 0.16f}};
    } else {  // (zoom)
        ghosts = {{0.35f, 10, {0.6f, 0.8f, 1.0f}, 0.18f}, {0.6f, 26, {0.5f, 1.0f, 0.6f}, 0.10f}, {0.85f, 6, {1.0f, 0.7f, 0.4f}, 0.25f},
                  {1.25f, 40, {0.7f, 0.5f, 1.0f}, 0.08f}, {1.5f, 16, {1.0f, 0.6f, 0.5f}, 0.15f}, {1.9f, 70, {0.5f, 0.7f, 1.0f}, 0.06f}};
    }
    std::vector<float> res = src.crop(out);
    std::size_t i = 0;
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x, i += n) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            const float dx = fx - cx, dy = fy - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            std::array<float, 3> f{};
            // the hot core and the wide glow (warm white)
            const float g = std::exp(-std::pow(d / (core * s), 2.0f)) + 0.35f * std::exp(-d / (glow * s));
            f[0] += g * 1.0f;
            f[1] += g * 0.95f;
            f[2] += g * 0.85f;
            if (rays > 0.0f) {  // (rays: the angle's ripple, fading with the distance)
                const float th = std::atan2(dy, dx);
                const float m = std::pow(std::sin(th * 12.0f) * 0.5f + 0.5f, 8.0f) + std::pow(std::sin(th * 7.0f + 1.3f) * 0.5f + 0.5f, 12.0f) * 0.6f;
                const float rr = rays * m * std::exp(-d / (glow * 1.6f * s));
                for (float& v : f) v += rr * 0.6f;
            }
            if (streak > 0.0f) {  // (the anamorphic streak across)
                const float st = std::exp(-(std::fabs(dy) / (2.5f * s))) * std::exp(-(std::fabs(dx) / (450.0f * s)));
                f[0] += st * 0.5f;
                f[1] += st * 0.7f;
                f[2] += st;
            }
            if (ring_r > 0.0f) {  // (the halo ring, its colours a little apart)
                const float offsets[3] = {1.04f, 1.0f, 0.96f};
                for (std::size_t k = 0; k < 3; ++k) {
                    const float rd = (d - ring_r * s * offsets[k]) / (ring_w * s);
                    f[k] += 0.18f * std::exp(-rd * rd);
                }
            }
            for (const Ghost& gh : ghosts) {  // (the ghosts along the line through the centre)
                const float gx = cx + (mx - cx) * gh.t * 2.0f, gy = cy + (my - cy) * gh.t * 2.0f;
                const float gd = std::sqrt((fx - gx) * (fx - gx) + (fy - gy) * (fy - gy)) / (gh.r * s);
                if (gd < 1.2f) {
                    const float e = smoothstep(1.0f, 0.85f, gd) * (0.6f + 0.4f * gd);
                    for (std::size_t k = 0; k < 3; ++k) f[k] += e * gh.k * gh.tint[k];
                }
            }
            float* px = res.data() + i;
            for (std::size_t k = 0; k < 3; ++k) {  // (screen: highlights kept below white)
                const float a = std::max(f[k] * bright, 0.0f);
                px[k] = 1.0f - (1.0f - px[k]) * (1.0f - std::min(a, 1.0f));
            }
        }
    return res;
}

// --- 照明効果 (render.rs lighting) ------------------------------------------------------------------------------------------

using V3 = std::array<float, 3>;
V3 norm3(V3 v) {
    const float m = std::max(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), 1e-9f);
    return {v[0] / m, v[1] / m, v[2] / m};
}
float dot3(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

struct Light {
    enum class Kind { Spot, Point, Infinite } kind = Kind::Spot;
    float x = 0.25f, y = 0.2f, z = 0.6f, target_x = 0.5f, target_y = 0.55f, angle = 135, elevation = 45;
    V3 color{1, 1, 1};
    float intensity = 75, cone = 45, hotspot = 50, radius = 0.8f;
};

std::vector<Light> lights_of(const Json& params) {
    std::vector<Light> out;
    const Json* lights = core::get(params, "lights");
    if (lights == nullptr || !lights->is_array()) return out;
    for (const Json& l : *lights) {
        if (!l.is_object()) continue;
        Light made;
        const std::string kind = text(l, "kind", "spot");
        made.kind = kind == "point" ? Light::Kind::Point : kind == "infinite" ? Light::Kind::Infinite : Light::Kind::Spot;
        made.x = static_cast<float>(number(l, "x", 0.25));
        made.y = static_cast<float>(number(l, "y", 0.2));
        made.z = static_cast<float>(number(l, "z", 0.6));
        made.target_x = static_cast<float>(number(l, "target_x", 0.5));
        made.target_y = static_cast<float>(number(l, "target_y", 0.55));
        made.angle = static_cast<float>(number(l, "angle", 135));
        made.elevation = static_cast<float>(number(l, "elevation", 45));
        const auto c = d2::colour(l, "color", {1, 1, 1});
        made.color = {c[0], c[1], c[2]};
        made.intensity = static_cast<float>(number(l, "intensity", 75));
        made.cone = static_cast<float>(number(l, "cone", 45));
        made.hotspot = static_cast<float>(number(l, "hotspot", 50));
        made.radius = static_cast<float>(number(l, "radius", 0.8));
        out.push_back(made);
    }
    return out;
}

// The pixels a bump's slope is read across (one working pixel — upstream's one pixel at s 1 — either side).
int bump_step(double s) { return std::max(1, static_cast<int>(std::lround(s))); }

std::vector<float> lighting(const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
    const std::size_t n = src.ch;
    const Rect& b = ctx.bounds;
    const float bw = static_cast<float>(b.width()), bh = static_cast<float>(b.height());
    const float shortest = std::max(std::min(bw, bh), 1.0f);
    const float hscale = std::clamp(static_cast<float>(number(params, "height", 50)), 0.0f, 100.0f) / 100.0f * 0.06f * shortest;
    const float gloss = std::clamp(static_cast<float>(number(params, "gloss", 0)), -100.0f, 100.0f);
    const float shininess = std::pow(2.0f, 1.0f + (gloss + 100.0f) / 200.0f * 7.0f);
    const float spec_k = (gloss + 100.0f) / 200.0f * 0.7f;
    const float metal = (std::clamp(static_cast<float>(number(params, "metallic", 0)), -100.0f, 100.0f) + 100.0f) / 200.0f;
    const float expo = std::pow(2.0f, std::clamp(static_cast<float>(number(params, "exposure", 0)), -100.0f, 100.0f) / 50.0f);
    const float ambient = std::clamp(static_cast<float>(number(params, "ambience", 8)), -100.0f, 100.0f) / 100.0f * 0.5f;
    const bool white_is_high = d2::flag(params, "white_is_high", true);
    // the bumps: a colour of the layer, its alpha, its luminance or a channel of the page (Ctx::map, a pixel of it a pixel of
    // the grid)
    const Json* texture_given = core::get(params, "texture");
    std::string texture = texture_given != nullptr && texture_given->is_string() ? texture_given->get<std::string>() : std::string("none");
    const bool channel = texture_given != nullptr && texture_given->is_object() && ctx.map != nullptr && !ctx.map->empty();
    if (texture_given != nullptr && texture_given->is_object() && !channel) texture = "none";  // (no channel read: none)
    const bool bumped = (channel || texture != "none") && hscale > 0.0f;
    const int step = bump_step(ctx.s);
    const auto tex = [&](int x, int y) -> float {
        const int xx = std::clamp(x, b.x0, b.x1 - 1), yy = std::clamp(y, b.y0, b.y1 - 1);
        float v = 0.0f;
        if (channel) {
            v = ctx.map->at(xx + ctx.x0, yy + ctx.y0, 0);
        } else if (texture == "alpha") {
            v = ctx.alpha ? src.at(xx, yy, n - 1) : 1.0f;
        } else if (texture == "luminance") {
            v = 0.299f * src.at(xx, yy, 0) + 0.587f * src.at(xx, yy, 1) + 0.114f * src.at(xx, yy, 2);
        } else {
            v = src.at(xx, yy, texture == "red" ? 0 : texture == "green" ? 1 : 2);
        }
        return white_is_high ? v : 1.0f - v;
    };
    const std::vector<Light> lights = lights_of(params);
    std::vector<float> res = src.crop(out);
    std::size_t i = 0;
    for (int y = out.y0; y < out.y1; ++y)
        for (int x = out.x0; x < out.x1; ++x, i += n) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            V3 nrm{0, 0, 1};
            if (bumped) {
                const float hx = (tex(x + step, y) - tex(x - step, y)) / (2.0f * static_cast<float>(step)) * hscale;
                const float hy = (tex(x, y + step) - tex(x, y - step)) / (2.0f * static_cast<float>(step)) * hscale;
                nrm = norm3({-hx, -hy, 1.0f});
            }
            float* px = res.data() + i;
            const V3 base{px[0], px[1], px[2]};
            V3 lit{ambient, ambient, ambient}, spc{0, 0, 0};
            for (const Light& l : lights) {
                const V3 lpos{static_cast<float>(b.x0) + l.x * bw, static_cast<float>(b.y0) + l.y * bh, l.z * shortest};
                V3 ldir{0, 0, 1};
                float atten = 1.0f;
                if (l.kind == Light::Kind::Infinite) {
                    const float a = radians(l.angle), e = radians(std::clamp(l.elevation, 0.0f, 90.0f));
                    ldir = norm3({std::cos(a) * std::cos(e), -std::sin(a) * std::cos(e), std::sin(e)});
                } else if (l.kind == Light::Kind::Point) {
                    const V3 v{lpos[0] - fx, lpos[1] - fy, lpos[2]};
                    const float dist = std::sqrt(v[0] * v[0] + v[1] * v[1]);
                    const float reach = std::max(l.radius * shortest, 1.0f);
                    ldir = norm3(v);
                    atten = std::pow(std::clamp(1.0f - dist / reach, 0.0f, 1.0f), 2.0f);
                } else {
                    const V3 ld = norm3({lpos[0] - fx, lpos[1] - fy, lpos[2]});
                    const V3 tgt{static_cast<float>(b.x0) + l.target_x * bw, static_cast<float>(b.y0) + l.target_y * bh, 0.0f};
                    const V3 axis = norm3({tgt[0] - lpos[0], tgt[1] - lpos[1], tgt[2] - lpos[2]});
                    const float cosang = -dot3(ld, axis);
                    const float cone = std::clamp(l.cone, 1.0f, 89.0f);
                    const float outer = std::cos(radians(cone));
                    const float inner = std::cos(radians(cone * (std::clamp(l.hotspot, 0.0f, 100.0f) / 100.0f)));
                    ldir = ld;
                    atten = smoothstep(outer, std::max(inner, outer + 1e-4f), cosang);
                }
                if (atten <= 0.0f) continue;
                const float inten = std::clamp(l.intensity, -100.0f, 100.0f) / 50.0f * atten;
                const float ndl = std::max(dot3(nrm, ldir), 0.0f);
                const V3 hv = norm3({ldir[0], ldir[1], ldir[2] + 1.0f});
                const float sp = std::pow(std::max(dot3(nrm, hv), 0.0f), shininess) * spec_k;
                for (std::size_t k = 0; k < 3; ++k) {
                    lit[k] += ndl * inten * l.color[k];
                    // (metallic highlights take the surface's colour, plastic ones the light's)
                    const float sc = l.color[k] * (1.0f - metal) + base[k] * metal;
                    spc[k] += sp * std::max(inten, 0.0f) * sc;
                }
            }
            for (std::size_t k = 0; k < 3; ++k) px[k] = std::max((base[k] * lit[k] + spc[k]) * expo, 0.0f);
        }
    return res;
}

std::size_t light_count(const Json& params) {
    const Json* lights = core::get(params, "lights");
    return lights != nullptr && lights->is_array() ? lights->size() : 0;
}

}  // namespace

Kernels render_kernels() {
    Kernels all;
    {  // 雲模様 1 (filter.render.clouds): the base box filled with clouds between the main and sub colours
        Kernel k;
        k.kind = "render_clouds";
        k.halo = [](const Json&, const Ctx&) { return Halo::margin(0); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) { return clouds(params, src, out, ctx, false); };
        k.cost = [](const Json&, const Ctx&) { return 160.0; };
        k.fills = true;
        all.push_back(std::move(k));
    }
    {  // 雲模様 2 (filter.render.differenceClouds): the clouds laid on the pixels by their difference (alpha as it was)
        Kernel k;
        k.kind = "render_difference_clouds";
        k.halo = [](const Json&, const Ctx&) { return Halo::margin(0); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) { return clouds(params, src, out, ctx, true); };
        k.cost = [](const Json&, const Ctx&) { return 160.0; };
        all.push_back(std::move(k));
    }
    {  // ファイバー (filter.render.fibers): the base box filled with fibres between the main and sub colours
        Kernel k;
        k.kind = "render_fibers";
        k.halo = [](const Json&, const Ctx&) { return Halo::margin(0); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) { return fibers(params, src, out, ctx); };
        k.cost = [](const Json&, const Ctx&) { return 100.0; };
        k.fills = true;
        all.push_back(std::move(k));
    }
    {  // 逆光 (filter.render.lensFlare): the flare screened over the pixels (alpha as it was)
        Kernel k;
        k.kind = "render_lens_flare";
        k.halo = [](const Json&, const Ctx&) { return Halo::margin(0); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) { return lens_flare(params, src, out, ctx); };
        k.cost = [](const Json&, const Ctx&) { return 120.0; };
        all.push_back(std::move(k));
    }
    {  // 照明効果 (filter.render.lightingEffects): the pixels shaded as a surface under the lights (alpha as it was)
        Kernel k;
        k.kind = "render_lighting_effects";
        k.halo = [](const Json&, const Ctx& ctx) { return Halo::margin(bump_step(ctx.s) + 1); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) { return lighting(params, src, out, ctx); };
        k.cost = [](const Json& params, const Ctx&) { return 40.0 + 60.0 * static_cast<double>(light_count(params)); };
        all.push_back(std::move(k));
    }
    return all;
}

}  // namespace genko::render::fx
