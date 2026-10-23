// PC4 節 D4 (SPEC.md COLOR-08「フィルター」, design 3.2): render_picture — フィルター → 描画 → 炎…・額縁…・木…
// (render/render_picture.hpp). PhotoCraft v0.2.0's algo/src/render2.rs (1-1530) and engine/src/render_cmds.rs (1-183),
// MIT: native/third_party/photocraft/NOTICE.md. Upstream's shapes are its own designs (L-system-like branching, turbulent
// flame strands, ornaments walked along the frame's edge); they are ported as they are, made in working pixels and laid
// on the layer's grid scaled by s.

#include "render/render_picture.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <tuple>
#include <numbers>
#include <optional>
#include <string>

#include "core/areas.hpp"
#include "core/color_model.hpp"
#include "core/error.hpp"
#include "core/filter_kinds.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/filters.hpp"
#include "render/fx.hpp"
#include "render/image_ops.hpp"
#include "render/ink_edit.hpp"
#include "render/paint_fill.hpp"
#include "render/path_raster.hpp"
#include "render/raster.hpp"
#include "render/segment/segment.hpp"
#include "render/selection.hpp"
#include "render/shape_parts.hpp"

namespace genko::render::picture {

namespace {

using core::Json;
using core::OpError;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTau = 2.0f * std::numbers::pi_v<float>;
constexpr double kWorkingPerMm = 200.0 / 25.4;
using Rgba = std::array<float, 4>;

// --- the shapes ---------------------------------------------------------------------------------------------------------

Prim capsule(XY a, XY b, float ra, float rb, Rgba c0, Rgba c1) {
    Prim p;
    p.shape = Prim::Shape::Capsule;
    p.x0 = a.first, p.y0 = a.second, p.x1 = b.first, p.y1 = b.second, p.r0 = ra, p.r1 = rb;
    p.c0 = c0, p.c1 = c1;
    return p;
}

Prim ellipse(XY c, float rx, float ry, float angle, Rgba c0, Rgba c1) {
    Prim p;
    p.shape = Prim::Shape::Ellipse;
    p.x0 = c.first, p.y0 = c.second, p.r0 = rx, p.r1 = ry, p.angle = angle;
    p.c0 = c0, p.c1 = c1;
    return p;
}

Prim disc(XY c, float r, Rgba col) { return ellipse(c, r, r, 0.0f, col, col); }
Prim with_soft(Prim p, float s) {
    p.soft = s;
    return p;
}
Prim additive(Prim p) {
    p.blend = Prim::Blend::Add;
    return p;
}
Prim lighten(Prim p) {
    p.blend = Prim::Blend::Lighten;
    return p;
}

// The pixels a shape may touch (render2.rs Prim::bounds): none when not finite.
fx::Rect bounds_of(const Prim& p) {
    const float s = std::max(p.soft, 1.0f) + 1.0f;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (p.shape == Prim::Shape::Capsule) {
        const float r = std::max(p.r0, p.r1) + s;
        x0 = std::min(p.x0, p.x1) - r, y0 = std::min(p.y0, p.y1) - r, x1 = std::max(p.x0, p.x1) + r, y1 = std::max(p.y0, p.y1) + r;
    } else {
        const float sn = std::sin(p.angle), cs = std::cos(p.angle);
        const float ex = std::sqrt((p.r0 * cs) * (p.r0 * cs) + (p.r1 * sn) * (p.r1 * sn)) + s;
        const float ey = std::sqrt((p.r0 * sn) * (p.r0 * sn) + (p.r1 * cs) * (p.r1 * cs)) + s;
        x0 = p.x0 - ex, y0 = p.y0 - ey, x1 = p.x0 + ex, y1 = p.y0 + ey;
    }
    if (!(std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1))) return fx::Rect{};
    const auto cl = [](float v) { return static_cast<int>(std::clamp(v, -1.0e8f, 1.0e8f)); };
    return fx::Rect{cl(std::floor(x0)), cl(std::floor(y0)), cl(std::ceil(x1)), cl(std::ceil(y1))};
}

// A shape's coverage (0..1) and colour place (0..1) at a pixel's centre (render2.rs Prim::eval).
std::pair<float, float> eval(const Prim& p, float x, float y) {
    const float soft = std::max(p.soft, 1.0f);
    if (p.shape == Prim::Shape::Capsule) {
        const float dx = p.x1 - p.x0, dy = p.y1 - p.y0;
        const float l2 = dx * dx + dy * dy;
        const float t = l2 > 1e-9f ? std::clamp(((x - p.x0) * dx + (y - p.y0) * dy) / l2, 0.0f, 1.0f) : 0.0f;
        const float px = p.x0 + dx * t - x, py = p.y0 + dy * t - y;
        const float d = std::sqrt(px * px + py * py);
        const float r = p.r0 + (p.r1 - p.r0) * t;
        const float thin = std::clamp(2.0f * r, 0.0f, 1.0f);  // (hairlines thinner than a pixel fade instead of vanishing)
        return {std::clamp((std::max(r, 0.5f) - d) / soft + 0.5f, 0.0f, 1.0f) * thin, t};
    }
    const float s = std::sin(p.angle), co = std::cos(p.angle);
    const float dx = x - p.x0, dy = y - p.y0;
    const float u = dx * co + dy * s, v = -dx * s + dy * co;
    const float rx = std::max(p.r0, 0.05f), ry = std::max(p.r1, 0.05f);
    const float e = std::sqrt((u / rx) * (u / rx) + (v / ry) * (v / ry));
    const float edge = (1.0f - e) * std::min(rx, ry);
    const float thin = std::clamp(2.0f * std::min(rx, ry), 0.0f, 1.0f);
    return {std::clamp(edge / soft + 0.5f, 0.0f, 1.0f) * thin, std::min(e, 1.0f)};
}

Rgba lerp4(const Rgba& a, const Rgba& b, float t) { return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t}; }

// The shapes made, at most kPictureMostShapes (refused past it: render_picture refuses before it makes them where it can
// tell — 炎's — and here as a last guard).
// (the shapes' memory taken from the imaging budget a block at a time as they are made — twice a block, the list's growth —:
// past it the work is refused, not run out of memory; outside a budget — the windows' small pictures — nothing)
inline constexpr std::size_t kShapeBlock = std::size_t(1) << 16;
void hold_block(std::vector<std::unique_ptr<image_ops::Held>>& held, std::size_t made, std::size_t each) {
    if (made % kShapeBlock == 0) held.push_back(std::make_unique<image_ops::Held>(std::uint64_t(kShapeBlock) * each * 2));
}

struct Sink {
    std::vector<Prim> out;
    std::vector<std::unique_ptr<image_ops::Held>> held;
    void push(const Prim& p) {
        if (out.size() >= core::kPictureMostShapes) refuse();
        hold_block(held, out.size(), sizeof(Prim));
        out.push_back(p);
    }
    [[noreturn]] static void refuse() {
        throw OpError("render_picture would draw more than " + std::to_string(core::kPictureMostShapes) +
                      " shapes: fewer flames (a longer interval, a shorter path) or a smaller frame");
    }
};

// --- randomness (render2.rs Rng: SplitMix64) -------------------------------------------------------------------------

struct Rng {
    std::uint64_t state = 0;
    static Rng make(std::uint32_t seed, std::uint64_t salt) {
        return Rng{(std::uint64_t(seed) << 32) ^ (salt * 0x9E3779B97F4A7C15ull) ^ 0x2545F4914F6CDD1Dull};
    }
    std::uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float f() { return static_cast<float>(next() >> 40) / static_cast<float>(1u << 24); }  // [0, 1)
    float range(float a, float b) { return a + (b - a) * f(); }
    float sym() { return f() * 2.0f - 1.0f; }
};

// Smooth 1-D value noise in [−1, 1], and two octaves of it.
float noise1(float x, std::uint64_t seed) {
    const auto h = [seed](std::int64_t i) { return Rng{static_cast<std::uint64_t>(i) * 0xD6E8FEB86659FD93ull ^ seed}.sym(); };
    const float i = std::floor(x);
    const float f = x - i;
    const auto k = static_cast<std::int64_t>(i);
    const float t = f * f * (3.0f - 2.0f * f);
    return h(k) + (h(k + 1) - h(k)) * t;
}
float fbm1(float x, std::uint64_t seed) { return (noise1(x, seed) + 0.5f * noise1(x * 2.13f + 7.1f, seed ^ 0xA5A5u)) / 1.5f; }

// --- a polyline by its length (render2.rs Spine) -------------------------------------------------------------------------

struct Spine {
    std::vector<XY> pts;
    std::vector<float> cum{0.0f};
    explicit Spine(std::vector<XY> points) : pts(std::move(points)) {
        for (std::size_t k = 1; k < pts.size(); ++k)
            cum.push_back(cum.back() + std::sqrt(std::pow(pts[k].first - pts[k - 1].first, 2.0f) + std::pow(pts[k].second - pts[k - 1].second, 2.0f)));
    }
    float len() const { return cum.back(); }
    // The point and unit tangent at the length s.
    std::pair<XY, XY> at(float s) const {
        const std::size_t n = pts.size();
        if (n == 0) return {{0.0f, 0.0f}, {1.0f, 0.0f}};
        if (n == 1) return {pts[0], {0.0f, -1.0f}};
        s = std::clamp(s, 0.0f, len());
        const auto it = std::lower_bound(cum.begin(), cum.end(), s);
        std::size_t k = static_cast<std::size_t>(it - cum.begin());
        if (it == cum.end() || *it != s) k = k == 0 ? 0 : k - 1;
        k = std::min(k, n - 2);
        const XY a = pts[k], b = pts[k + 1];
        const float seg = std::max(cum[k + 1] - cum[k], 1e-6f);
        const float t = std::clamp((s - cum[k]) / seg, 0.0f, 1.0f);
        const float dx = b.first - a.first, dy = b.second - a.second;
        const float l = std::max(std::sqrt(dx * dx + dy * dy), 1e-6f);
        return {{a.first + dx * t, a.second + dy * t}, {dx / l, dy / l}};
    }
};

// --- 炎 (render2.rs Flame) --------------------------------------------------------------------------------------------------

enum class FlameType { OneAlongPath, MultipleAlongPath, MultiplePathDirections, MultipleVariousLength, CandleLight, MultipleOneDirection };
enum class FlameStyle { Normal, Violent, Flat };
enum class FlameShape { Parallel, ToCenter, Spread, Oval, Pointed };

struct FlameSpec {
    FlameType type = FlameType::OneAlongPath;
    float length = 150, width = 40, angle = 0, interval = 60;
    bool randomize_length = false, adjust_interval_for_loops = true;
    std::optional<Rgba> color;
    float turbulent = 25, jag = 25, opacity = 75, flame_lines = 20, flame_bottom_alignment = 20;
    FlameStyle style = FlameStyle::Normal;
    FlameShape shape = FlameShape::Parallel;
    bool randomize_shapes = false;
    std::uint32_t seed = 0;
    unsigned quality = 2;
};

float num(const Json& spec, const char* key, double fallback) {
    const Json* v = core::get(spec, key);
    return static_cast<float>(v != nullptr && v->is_number() && !v->is_boolean() ? v->get<double>() : fallback);
}
bool flag(const Json& spec, const char* key, bool fallback) {
    const Json* v = core::get(spec, key);
    return v != nullptr && v->is_boolean() ? v->get<bool>() : fallback;
}
std::string word(const Json& spec, const char* key, const char* fallback) {
    const Json* v = core::get(spec, key);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string(fallback);
}
std::uint32_t seed_of(const Json& spec) {
    const Json* v = core::get(spec, "seed");
    if (v == nullptr || !v->is_number() || v->is_boolean()) return 0;
    return static_cast<std::uint32_t>(std::clamp(v->get<double>(), 0.0, 4294967295.0));
}
Rgba colour(const Json& spec, const char* key, Rgba fallback) {
    const Json* v = core::get(spec, key);
    if (v == nullptr || !v->is_array() || v->size() != 3) return fallback;
    Rgba out{0, 0, 0, 1};
    for (std::size_t k = 0; k < 3; ++k) out[k] = (*v)[k].is_number() ? static_cast<float>((*v)[k].get<double>() / 255.0) : fallback[k];
    return out;
}

FlameSpec flame_spec(const Json& spec) {
    FlameSpec out;
    const std::string type = word(spec, "flame_type", "one_flame_along_path");
    out.type = type == "multiple_flames_along_path"         ? FlameType::MultipleAlongPath
               : type == "multiple_flames_path_directions" ? FlameType::MultiplePathDirections
               : type == "multiple_flames_various_length"  ? FlameType::MultipleVariousLength
               : type == "candle_light"                    ? FlameType::CandleLight
               : type == "multiple_flames_one_direction"   ? FlameType::MultipleOneDirection
                                                           : FlameType::OneAlongPath;
    out.length = num(spec, "length", 150);
    out.randomize_length = flag(spec, "randomize_length", false);
    out.width = num(spec, "width", 40);
    out.angle = num(spec, "angle", 0);
    out.interval = num(spec, "interval", 60);
    out.adjust_interval_for_loops = flag(spec, "adjust_interval_for_loops", true);
    if (flag(spec, "use_custom_color", false)) out.color = colour(spec, "color", {1.0f, 0.55f, 0.1f, 1.0f});
    out.turbulent = num(spec, "turbulent", 25);
    out.jag = num(spec, "jag", 25);
    out.opacity = num(spec, "opacity", 75);
    out.flame_lines = num(spec, "flame_lines", 20);
    out.flame_bottom_alignment = num(spec, "flame_bottom_alignment", 20);
    const std::string style = word(spec, "flame_style", "normal");
    out.style = style == "violent" ? FlameStyle::Violent : style == "flat" ? FlameStyle::Flat : FlameStyle::Normal;
    const std::string shape = word(spec, "flame_shape", "parallel");
    out.shape = shape == "to_center" ? FlameShape::ToCenter : shape == "spread" ? FlameShape::Spread : shape == "oval" ? FlameShape::Oval
                : shape == "pointed" ? FlameShape::Pointed : FlameShape::Parallel;
    out.randomize_shapes = flag(spec, "randomize_shapes", false);
    out.seed = seed_of(spec);
    const std::string quality = word(spec, "quality", "medium");
    out.quality = quality == "draft" ? 0 : quality == "low" ? 1 : quality == "high" ? 3 : quality == "fine" ? 4 : 2;
    return out;
}

// The natural colour of a flame at t along it (0 its base, 1 its tip), or a custom colour's core → colour → rim.
Rgba flame_colour(float t, const std::optional<Rgba>& custom) {
    const float a = std::pow(1.0f - t, 0.7f) * 0.9f + 0.1f * (1.0f - t);
    if (custom) {
        const Rgba& c = *custom;
        const std::array<float, 3> core{c[0] * 0.4f + 0.6f, c[1] * 0.4f + 0.6f, c[2] * 0.4f + 0.6f};
        const std::array<float, 3> rim{c[0] * 0.6f, c[1] * 0.6f, c[2] * 0.6f};
        const float k = std::clamp(t, 0.0f, 1.0f);
        std::array<float, 3> rgb{};
        if (k < 0.35f) {
            const float u = k / 0.35f;
            for (std::size_t i = 0; i < 3; ++i) rgb[i] = core[i] + (c[i] - core[i]) * u;
        } else {
            const float u = (k - 0.35f) / 0.65f;
            for (std::size_t i = 0; i < 3; ++i) rgb[i] = c[i] + (rim[i] - c[i]) * u;
        }
        return {rgb[0], rgb[1], rgb[2], a * c[3]};
    }
    static constexpr std::array<std::pair<float, std::array<float, 3>>, 5> kStops{{{0.0f, {1.0f, 0.98f, 0.85f}},
                                                                                     {0.2f, {1.0f, 0.86f, 0.35f}},
                                                                                     {0.45f, {1.0f, 0.55f, 0.08f}},
                                                                                     {0.75f, {0.9f, 0.25f, 0.02f}},
                                                                                     {1.0f, {0.6f, 0.08f, 0.0f}}}};
    const float k = std::clamp(t, 0.0f, 1.0f);
    std::array<float, 3> rgb = kStops[4].second;
    for (std::size_t w = 0; w + 1 < kStops.size(); ++w) {
        if (k <= kStops[w + 1].first) {
            const float u = (k - kStops[w].first) / (kStops[w + 1].first - kStops[w].first);
            for (std::size_t i = 0; i < 3; ++i) rgb[i] = kStops[w].second[i] + (kStops[w + 1].second[i] - kStops[w].second[i]) * u;
            break;
        }
    }
    return {rgb[0], rgb[1], rgb[2], a};
}

constexpr std::array<std::size_t, 5> kQualitySegments{6, 10, 16, 24, 36};

std::size_t flame_lines(const FlameSpec& spec) { return static_cast<std::size_t>(std::round(std::clamp(spec.flame_lines, 1.0f, 100.0f) * 0.6f + 3.0f)); }
std::size_t flame_segments(const FlameSpec& spec, float len) {
    const float pieces = std::max(len / 12.0f, 0.0f);
    return std::min<std::size_t>(std::max<std::size_t>(kQualitySegments[std::min(spec.quality, 4u)], pieces >= 160.0f ? 160 : static_cast<std::size_t>(pieces)), 160);
}

// One flame: its spine (base → tip), its width and its random stream.
void draw_flame(Sink& out, const Spine& spine, float width, const FlameSpec& spec, Rng& rng) {
    const float len = spine.len();
    if (len < 1.0f) return;
    const std::size_t lines = flame_lines(spec);
    const std::size_t segs = flame_segments(spec, len);
    float turb = std::clamp(spec.turbulent, 0.0f, 100.0f) / 100.0f, jag = std::clamp(spec.jag, 0.0f, 100.0f) / 100.0f;
    if (spec.style == FlameStyle::Violent) {
        turb = std::min(turb * 1.8f + 0.2f, 2.0f);
        jag = std::min(jag * 1.8f + 0.15f, 2.0f);
    } else if (spec.style == FlameStyle::Flat) {
        turb *= 0.35f;
        jag *= 0.35f;
    }
    static constexpr std::array<FlameShape, 5> kShapes{FlameShape::Parallel, FlameShape::ToCenter, FlameShape::Spread, FlameShape::Oval, FlameShape::Pointed};
    const FlameShape shape = spec.randomize_shapes ? kShapes[rng.next() % 5] : spec.shape;
    const float opacity = std::clamp(spec.opacity, 0.0f, 100.0f) / 100.0f;
    const float half = std::max(width, 1.0f) / 2.0f;
    const std::uint64_t seed = rng.next();
    // (the whole flame sways; each line flickers on top of it)
    const auto sway = [&](float u) { return turb * half * 1.6f * fbm1(u * 1.7f, seed ^ 0x5A5Au) * std::pow(u, 1.4f); };
    for (int k = 0; k < 6; ++k) {  // (a soft glow added along the spine)
        const float u = static_cast<float>(k) / 6.0f * 0.75f + 0.05f;
        const auto [p, d] = spine.at(u * len);
        const float o = sway(u);
        const Rgba c = flame_colour(u, spec.color);
        const float r = half * (1.25f - u) + 2.0f;
        out.push(additive(with_soft(ellipse({p.first - d.second * o, p.second + d.first * o}, r, r * 1.2f, std::atan2(d.second, d.first),
                                            {c[0], c[1], c[2], 0.16f * opacity}, {c[0], c[1] * 0.6f, c[2] * 0.3f, 0.0f}),
                                    std::max(r, 2.0f))));
    }
    const float line_r = std::clamp(half / static_cast<float>(lines) * 2.2f, 0.8f, 10.0f);
    for (std::size_t li = 0; li < lines; ++li) {
        const float lane = lines == 1 ? 0.0f : static_cast<float>(li) / static_cast<float>(lines - 1) * 2.0f - 1.0f;
        const std::uint64_t lseed = seed ^ (static_cast<std::uint64_t>(li) * 0x517CC1B727220Aull);
        const float start = (std::clamp(spec.flame_bottom_alignment, 0.0f, 100.0f) / 100.0f) * rng.f() * 0.4f;
        // (the outer lines shorter: the flame's tongue)
        const float reach = (1.0f - 0.55f * std::pow(std::fabs(lane), 1.5f)) * rng.range(0.7f, 1.0f);
        const float end = std::clamp(start + reach * (1.0f - start), start + 0.05f, 1.0f);
        const float lr = line_r * rng.range(0.6f, 1.3f);
        std::optional<std::tuple<XY, float, float>> prev;
        for (std::size_t k = 0; k <= segs; ++k) {
            const float u = start + (end - start) * static_cast<float>(k) / static_cast<float>(segs);
            const float s = u * len;
            const auto [pt, d] = spine.at(s);
            const float nx = -d.second, ny = d.first;
            float spread = 1.0f;
            switch (shape) {
                case FlameShape::Parallel: spread = 1.0f - 0.6f * u; break;
                case FlameShape::ToCenter: spread = 1.0f - u; break;
                case FlameShape::Spread: spread = 1.0f + u; break;
                case FlameShape::Oval: spread = std::sin(kPi * (0.15f + 0.85f * u)) * 1.3f; break;
                case FlameShape::Pointed: spread = (1.0f - u) * (1.0f - u); break;
            }
            const float flick = turb * half * 0.5f * fbm1(s / (len * 0.2f + 6.0f) + static_cast<float>(li) * 0.37f, lseed) * u +
                                jag * half * 0.25f * noise1(s / 5.0f, lseed ^ 0x77u) * u;
            const float off = lane * half * spread + sway(u) + flick;
            const XY p{pt.first + nx * off, pt.second + ny * off};
            const float r = lr * (1.0f - u * 0.75f);
            // (the lines fade out toward their own tip)
            const float fade_t = std::clamp((u - start) / std::max(end - start, 1e-3f), 0.0f, 1.0f);
            if (prev) {
                const auto& [q, rq, uq] = *prev;
                // (faded in over the first tenth — a soft base — and out toward the line's tip)
                const auto f = [&](Rgba c, float uu, float t) {
                    return Rgba{c[0], c[1], c[2], c[3] * opacity * (1.0f - t * t * t) * std::sqrt(std::clamp((uu - start) / 0.08f, 0.0f, 1.0f))};
                };
                out.push(lighten(with_soft(capsule(q, p, rq, r, f(flame_colour(uq, spec.color), uq, fade_t), f(flame_colour(u, spec.color), u, fade_t)),
                                           1.0f + lr * 0.6f)));
            }
            prev = std::make_tuple(p, r, u);
        }
    }
}

// The flames along each path: one along it, a candle at its start, or many at intervals.
template <typename Each>
void each_flame(const FlameSpec& spec, const std::vector<std::vector<XY>>& paths, Rng& rng, Each&& each) {
    const float length = std::clamp(spec.length, 1.0f, 1000.0f);
    const float width = std::clamp(spec.width, 1.0f, 1000.0f);
    const float interval = std::clamp(spec.interval, 1.0f, 1000.0f);
    const auto pick_len = [&](float var) {
        if (spec.randomize_length || var > 0.0f) return length * rng.range(1.0f - std::max(var, 0.4f), 1.0f + std::max(var, 0.4f) * 0.5f);
        return length;
    };
    for (const std::vector<XY>& path : paths) {
        if (path.empty()) continue;
        segment::check_stop();
        const Spine spine(path);
        const float total = spine.len();
        if (spec.type == FlameType::OneAlongPath && path.size() >= 2 && total >= 1.0f) {
            each(spine, width, spec, false);
            continue;
        }
        if (spec.type == FlameType::CandleLight || spec.type == FlameType::OneAlongPath) {
            // (a candle: a short teardrop rising straight up from the start)
            const XY base = path[0];
            const float h = std::min(length, 400.0f);
            FlameSpec c = spec;
            c.shape = FlameShape::Oval;
            c.turbulent = spec.turbulent * 0.3f;
            c.jag = spec.jag * 0.2f;
            each(Spine({base, {base.first, base.second - h}}), std::max(width * 0.6f, 4.0f), c, true);
            continue;
        }
        const bool closed = path.size() > 2 && std::fabs(path.front().first - path.back().first) < 1.0f && std::fabs(path.front().second - path.back().second) < 1.0f;
        const std::size_t count = spec.adjust_interval_for_loops && closed ? static_cast<std::size_t>(std::max(std::round(total / interval), 1.0f))
                                                                          : static_cast<std::size_t>(std::floor(total / interval)) + 1;
        const float step = spec.adjust_interval_for_loops && closed ? total / static_cast<float>(count) : interval;
        for (std::size_t k = 0; k < count; ++k) {
            if (k % 256 == 0) segment::check_stop();
            const float s = static_cast<float>(k) * step;
            const auto [p, d] = spine.at(s);
            XY dir{d.second, -d.first};  // (left of the path's way: up for a path from left to right)
            if (spec.type == FlameType::MultiplePathDirections) dir = d;
            if (spec.type == FlameType::MultipleOneDirection) {
                const float a = spec.angle * kPi / 180.0f;
                dir = {std::sin(a), -std::cos(a)};
            }
            const float l = pick_len(spec.type == FlameType::MultipleVariousLength ? 0.7f : 0.0f);
            each(Spine({p, {p.first + dir.first * l, p.second + dir.second * l}}), width, spec, false);
        }
    }
}

// The flames counted, each its length at most (no shape made).
std::uint64_t count_shapes(const FlameSpec& spec, const std::vector<std::vector<XY>>& paths) {
    std::uint64_t total = 0;
    const std::size_t lines = flame_lines(spec);
    const float length = std::clamp(spec.length, 1.0f, 1000.0f);
    const float interval = std::clamp(spec.interval, 1.0f, 1000.0f);
    for (const std::vector<XY>& path : paths) {
        if (path.empty()) continue;
        const Spine spine(path);
        const float len = spine.len();
        std::uint64_t flames = 1;
        float longest = len;
        if (!(spec.type == FlameType::OneAlongPath && path.size() >= 2 && len >= 1.0f)) {
            if (spec.type == FlameType::CandleLight || spec.type == FlameType::OneAlongPath) {
                longest = std::min(length, 400.0f);
            } else {
                flames = static_cast<std::uint64_t>(std::floor(len / interval)) + 2;
                longest = length * 1.35f;
            }
        }
        total += flames * (8 + std::uint64_t(lines) * flame_segments(spec, longest));
        if (total > core::kPictureMostShapes) return total;
    }
    return total;
}

// --- 木 (render2.rs Tree) ----------------------------------------------------------------------------------------------------

enum class Leaf { None, Round, Oval, Needle, Heart, Blossom, Long, Frond };
enum class Habit { Branching, Conifer, Palm, Bamboo };

struct TreeKind {
    Habit habit;
    unsigned levels, children;
    float spread, ratio, trunk, droop, wiggle, thick;
    Leaf leaf;
    std::array<float, 3> leaf_col, bark;
    std::optional<std::array<float, 3>> accent;
};

constexpr std::array<float, 3> kBrown{0.36f, 0.25f, 0.16f};
constexpr std::array<float, 3> kGreyBark{0.45f, 0.42f, 0.38f};
constexpr std::array<float, 3> kGreen{0.24f, 0.52f, 0.16f};
constexpr std::array<float, 3> kDarkGreen{0.12f, 0.36f, 0.14f};

// The 34 base types (core::tree_type_names: their names, in this order).
const std::array<TreeKind, 34>& trees() {
    using H = Habit;
    using L = Leaf;
    static const std::array<TreeKind, 34> all{{
        {H::Branching, 5, 3, 38, 0.72f, 0.35f, 0.05f, 0.35f, 0.035f, L::Round, kGreen, kBrown, std::nullopt},                                 // oak
        {H::Branching, 5, 3, 34, 0.74f, 0.32f, 0.0f, 0.25f, 0.03f, L::Heart, {0.3f, 0.55f, 0.15f}, kBrown, std::nullopt},                   // maple
        {H::Conifer, 3, 2, 70, 0.5f, 0.15f, 0.25f, 0.1f, 0.025f, L::Needle, kDarkGreen, kBrown, std::nullopt},                              // pine
        {H::Palm, 1, 9, 0, 0.0f, 0.85f, 0.0f, 0.15f, 0.022f, L::Frond, {0.25f, 0.5f, 0.18f}, {0.55f, 0.45f, 0.32f}, std::nullopt},         // palm
        {H::Branching, 5, 2, 28, 0.75f, 0.4f, 0.1f, 0.3f, 0.02f, L::Oval, {0.45f, 0.65f, 0.2f}, {0.88f, 0.86f, 0.82f}, std::nullopt},      // birch
        {H::Branching, 5, 3, 40, 0.78f, 0.3f, 0.9f, 0.2f, 0.035f, L::Long, {0.45f, 0.62f, 0.22f}, kGreyBark, std::nullopt},                // weeping willow
        {H::Branching, 5, 3, 42, 0.7f, 0.3f, 0.0f, 0.45f, 0.03f, L::Blossom, {0.98f, 0.75f, 0.82f}, {0.3f, 0.2f, 0.17f}, std::nullopt},    // cherry blossom
        {H::Bamboo, 1, 5, 6, 0.0f, 1.0f, 0.0f, 0.05f, 0.012f, L::Long, {0.35f, 0.6f, 0.2f}, {0.55f, 0.65f, 0.3f}, std::nullopt},           // bamboo
        {H::Branching, 5, 3, 10, 0.8f, 0.1f, -0.1f, 0.1f, 0.02f, L::Needle, kDarkGreen, kBrown, std::nullopt},                             // cypress
        {H::Branching, 3, 4, 50, 0.55f, 0.65f, 0.0f, 0.3f, 0.09f, L::Round, {0.35f, 0.55f, 0.2f}, {0.55f, 0.47f, 0.4f}, std::nullopt},     // baobab
        {H::Branching, 4, 3, 30, 0.72f, 0.35f, 0.0f, 0.2f, 0.025f, L::Heart, {0.75f, 0.75f, 0.2f}, kGreyBark, std::nullopt},              // ginkgo
        {H::Conifer, 3, 2, 80, 0.45f, 0.1f, 0.45f, 0.08f, 0.025f, L::Needle, {0.1f, 0.3f, 0.2f}, kBrown, std::nullopt},                    // spruce
        {H::Branching, 6, 2, 34, 0.74f, 0.3f, 0.05f, 0.35f, 0.03f, L::None, kGreen, {0.3f, 0.25f, 0.22f}, std::nullopt},                   // bare winter tree
        {H::Branching, 5, 3, 46, 0.68f, 0.3f, 0.1f, 0.4f, 0.03f, L::Round, kGreen, kBrown, std::array<float, 3>{0.85f, 0.1f, 0.1f}},       // apple
        {H::Branching, 4, 3, 55, 0.75f, 0.45f, -0.25f, 0.25f, 0.03f, L::Round, {0.35f, 0.5f, 0.18f}, kBrown, std::nullopt},                // acacia
        {H::Branching, 5, 3, 12, 0.8f, 0.15f, -0.15f, 0.1f, 0.025f, L::Oval, {0.3f, 0.55f, 0.15f}, kGreyBark, std::nullopt},               // poplar
        {H::Branching, 5, 2, 24, 0.8f, 0.3f, 0.15f, 0.25f, 0.032f, L::Oval, kGreen, kBrown, std::nullopt},                                 // elm
        {H::Branching, 4, 3, 40, 0.7f, 0.3f, 0.0f, 0.3f, 0.03f, L::Blossom, {0.98f, 0.9f, 0.92f}, {0.35f, 0.28f, 0.25f},
         std::array<float, 3>{0.85f, 0.5f, 0.65f}},                                                                                         // magnolia
        {H::Palm, 1, 7, 0, 0.0f, 0.6f, 0.0f, 0.05f, 0.04f, L::Frond, {0.45f, 0.7f, 0.2f}, {0.5f, 0.6f, 0.3f}, std::nullopt},               // banana
        {H::Branching, 4, 3, 60, 0.6f, 0.45f, 0.2f, 0.7f, 0.05f, L::Round, kDarkGreen, {0.4f, 0.3f, 0.22f}, std::nullopt},                 // bonsai
        {H::Branching, 5, 3, 44, 0.72f, 0.3f, 0.0f, 0.35f, 0.03f, L::Blossom, {0.6f, 0.45f, 0.85f}, kBrown, std::nullopt},                 // jacaranda
        {H::Branching, 5, 3, 34, 0.74f, 0.32f, 0.0f, 0.25f, 0.03f, L::Heart, {0.85f, 0.3f, 0.1f}, kBrown, std::nullopt},                   // autumn maple
        {H::Branching, 5, 3, 36, 0.72f, 0.33f, 0.05f, 0.3f, 0.03f, L::Round, {0.9f, 0.7f, 0.15f}, kBrown, std::nullopt},                   // golden autumn
        {H::Branching, 4, 3, 48, 0.68f, 0.35f, 0.1f, 0.6f, 0.04f, L::Long, {0.5f, 0.58f, 0.42f}, kGreyBark,
         std::array<float, 3>{0.25f, 0.28f, 0.12f}},                                                                                        // olive
        {H::Conifer, 3, 2, 75, 0.5f, 0.12f, 0.15f, 0.05f, 0.025f, L::Needle, {0.12f, 0.38f, 0.22f}, kBrown, std::nullopt},                 // fir
        {H::Conifer, 3, 3, 88, 0.6f, 0.2f, 0.0f, 0.15f, 0.03f, L::Needle, {0.2f, 0.4f, 0.25f}, kBrown, std::nullopt},                      // cedar
        {H::Branching, 4, 3, 45, 0.68f, 0.3f, 0.05f, 0.35f, 0.03f, L::Oval, kDarkGreen, kBrown, std::array<float, 3>{0.98f, 0.88f, 0.15f}},  // lemon
        {H::Branching, 5, 3, 45, 0.72f, 0.3f, 0.7f, 0.3f, 0.03f, L::Blossom, {0.7f, 0.55f, 0.9f}, {0.35f, 0.3f, 0.27f}, std::nullopt},     // wisteria
        {H::Branching, 3, 2, 30, 0.7f, 0.45f, 0.0f, 0.2f, 0.015f, L::Oval, {0.4f, 0.7f, 0.2f}, {0.45f, 0.35f, 0.22f}, std::nullopt},       // young sapling
        {H::Branching, 5, 2, 45, 0.68f, 0.4f, 0.1f, 0.8f, 0.035f, L::None, kGreen, {0.32f, 0.29f, 0.26f}, std::nullopt},                   // dead tree
        {H::Palm, 1, 11, 0, 0.0f, 0.88f, 0.0f, 0.35f, 0.018f, L::Frond, {0.3f, 0.55f, 0.15f}, {0.6f, 0.5f, 0.36f},
         std::array<float, 3>{0.4f, 0.3f, 0.15f}},                                                                                          // coconut palm
        {H::Branching, 4, 4, 55, 0.7f, 0.05f, 0.0f, 0.35f, 0.02f, L::Round, kGreen, kBrown, std::nullopt},                                 // shrub
        {H::Branching, 5, 2, 48, 0.7f, 0.3f, 0.1f, 0.6f, 0.03f, L::Blossom, {0.95f, 0.6f, 0.7f}, {0.25f, 0.18f, 0.15f}, std::nullopt},     // plum blossom
        {H::Branching, 4, 3, 70, 0.65f, 0.35f, 0.2f, 0.5f, 0.035f, L::Needle, {0.15f, 0.4f, 0.2f}, {0.35f, 0.27f, 0.2f}, std::nullopt},    // cloud pine
    }};
    return all;
}

Rgba shade(const std::array<float, 3>& c, float k) {
    return {std::clamp(c[0] * k, 0.0f, 1.0f), std::clamp(c[1] * k, 0.0f, 1.0f), std::clamp(c[2] * k, 0.0f, 1.0f), 1.0f};
}
std::array<float, 3> rgb_of(const Rgba& c) { return {c[0], c[1], c[2]}; }

struct TreeCtx {
    const TreeKind& kind;
    float leaves_amount = 50, leaves_size = 100;
    bool flat = false;
    Rng rng;
    Sink branches;
    std::vector<std::pair<float, Prim>> leaves;
    std::vector<std::unique_ptr<image_ops::Held>> leaves_held;
    std::array<float, 3> bark, leaf;
    XY light, centre;
    float leaf_px = 1;

    void leaf_push(float order, const Prim& p) {
        if (branches.out.size() + leaves.size() >= core::kPictureMostShapes) Sink::refuse();
        hold_block(leaves_held, leaves.size(), sizeof(std::pair<float, Prim>));
        leaves.emplace_back(order, p);
    }

    // The brightness of a leaf at p (the crown's lit side brighter).
    float light_at(XY p) {
        if (flat) return 1.0f;
        const float dx = p.first - centre.first, dy = p.second - centre.second;
        const float l = std::max(std::sqrt(dx * dx + dy * dy), 1.0f);
        return 0.78f + 0.32f * ((dx * light.first + dy * light.second) / l) + 0.12f * rng.sym();
    }

    void add_leaf(XY p, float dir) {
        const float s = leaf_px * rng.range(0.7f, 1.2f);
        const float ang = dir + rng.sym() * 1.2f;
        const float k = light_at(p);
        const auto& col = leaf;
        Prim prim;
        switch (kind.leaf) {
            case Leaf::None: return;
            case Leaf::Round: prim = ellipse(p, s, s * 0.85f, ang, shade(col, k * 1.1f), shade(col, k * 0.8f)); break;
            case Leaf::Oval: prim = ellipse(p, s * 1.2f, s * 0.55f, ang, shade(col, k * 1.1f), shade(col, k * 0.8f)); break;
            case Leaf::Long: prim = ellipse(p, s * 1.6f, s * 0.28f, ang, shade(col, k * 1.1f), shade(col, k * 0.8f)); break;
            case Leaf::Heart: prim = ellipse(p, s * 0.9f, s * 1.1f, ang, shade(col, k * 1.1f), shade(col, k * 0.75f)); break;
            case Leaf::Needle: {  // (a tuft of needles fanning out from the twig)
                const int n = 7;
                for (int j = 0; j < n; ++j) {
                    const float a = ang + (static_cast<float>(j) / static_cast<float>(n - 1) - 0.5f) * 2.4f + rng.sym() * 0.15f;
                    const float l = s * rng.range(1.1f, 1.7f);
                    const XY q{p.first + std::cos(a) * l, p.second + std::sin(a) * l};
                    const float kk = k + 0.1f * rng.sym();
                    leaf_push(kk, capsule(p, q, std::max(s * 0.1f, 0.45f), std::max(s * 0.05f, 0.3f), shade(col, kk * 0.85f), shade(col, kk * 1.1f)));
                }
                return;
            }
            case Leaf::Blossom: prim = ellipse(p, s * 0.75f, s * 0.75f, 0.0f, shade(col, k * 1.08f), shade(col, k * 0.85f)); break;
            case Leaf::Frond: prim = ellipse(p, s * 1.6f, s * 0.3f, ang, shade(col, k), shade(col, k * 0.8f)); break;
        }
        const float order = k + rng.f() * 0.05f;
        leaf_push(order, prim);
        if (kind.accent && rng.f() < 0.12f) {
            const XY q{p.first + rng.sym() * s, p.second + rng.sym() * s};
            const float r = std::max(s * 0.45f, 1.0f);
            leaf_push(k + 1.0f, ellipse(q, r, r, 0.0f, shade(*kind.accent, k * 1.15f), shade(*kind.accent, k * 0.75f)));
        }
    }

    // A curved branch from p heading `ang` (radians, 0 right, y down): its end and its heading there.
    std::pair<XY, float> limb(XY p, float ang, float len, float r0, float r1, float depth01, float leaves_share) {
        const std::size_t segs = std::clamp<std::size_t>(static_cast<std::size_t>(std::max(len / 6.0f, 0.0f)), 2, 10);
        XY q = p;
        float a = ang;
        const float droop = kind.droop * depth01;
        const Rgba col0 = shade(bark, 0.9f + 0.2f * depth01);
        for (std::size_t k = 0; k < segs; ++k) {
            const float t0 = static_cast<float>(k) / static_cast<float>(segs), t1 = static_cast<float>(k + 1) / static_cast<float>(segs);
            a += kind.wiggle * rng.sym() * 0.25f;
            // (gravity pulls the heading toward straight down)
            const float down = kPi / 2.0f;
            const float diff = std::atan2(std::sin(down - a), std::cos(down - a));
            a += diff * droop * 0.12f;
            const float step = len / static_cast<float>(segs);
            const XY nq{q.first + std::cos(a) * step, q.second + std::sin(a) * step};
            branches.push(capsule(q, nq, r0 + (r1 - r0) * t0, r0 + (r1 - r0) * t1, col0, col0));
            if (depth01 > 0.99f && kind.leaf != Leaf::None && k > 0 && rng.f() < leaves_share) add_leaf(nq, a);
            q = nq;
        }
        return {q, a};
    }

    void branch(XY p, float ang, float len, float r, unsigned level) {
        const unsigned levels = kind.levels;
        const float depth01 = static_cast<float>(level) / static_cast<float>(std::max(levels, 1u));
        const float r_end = r * 0.62f;
        const auto [end, a] = limb(p, ang, len, r, r_end, depth01, std::clamp(leaves_amount, 0.0f, 100.0f) / 100.0f * 0.9f);
        if (level >= levels) {
            const float amount = std::clamp(leaves_amount, 0.0f, 100.0f) / 100.0f;
            if (kind.leaf != Leaf::None) {
                const auto n = static_cast<std::size_t>(std::round(amount * 9.0f));
                for (std::size_t i = 0; i < n; ++i) {
                    const float d = len * 0.5f * rng.f();
                    const float th = rng.f() * kTau;
                    add_leaf({end.first + std::cos(th) * d, end.second + std::sin(th) * d}, a);
                }
            }
            return;
        }
        const float spread = kind.spread * kPi / 180.0f;
        const unsigned n = kind.children;
        for (unsigned c = 0; c < n; ++c) {
            const float side = n == 1 ? 0.0f : static_cast<float>(c) / static_cast<float>(n - 1) * 2.0f - 1.0f;
            const float ca = a + side * spread + rng.sym() * spread * 0.35f;
            const float cl = len * kind.ratio * rng.range(0.8f, 1.1f);
            const float cr = r_end * rng.range(0.75f, 0.95f);
            branch(end, ca, cl, cr, level + 1);
        }
    }
};

// --- 額縁 (render2.rs Picture Frame) --------------------------------------------------------------------------------------

// The frame's rectangle walked by its length (clockwise from the top left): the point, its unit tangent and the inward
// normal there.
struct Border {
    float x0, y0, w, h;
    float len() const { return 2.0f * (w + h); }
    struct At {
        XY p, t, n;
    };
    At at(float s) const {
        const float total = len();
        s = std::fmod(s, total);
        if (s < 0.0f) s += total;
        if (s < w) return {{x0 + s, y0}, {1, 0}, {0, 1}};
        if (s < w + h) return {{x0 + w, y0 + s - w}, {0, 1}, {-1, 0}};
        if (s < 2.0f * w + h) return {{x0 + w - (s - w - h), y0 + h}, {-1, 0}, {0, -1}};
        return {{x0, y0 + h - (s - 2.0f * w - h)}, {0, -1}, {1, 0}};
    }
};

void flower(Sink& out, XY c, float r, int petals, const Rgba& petal, const Rgba& centre, float rot) {
    for (int k = 0; k < petals; ++k) {
        const float a = rot + static_cast<float>(k) / static_cast<float>(petals) * kTau;
        const XY p{c.first + std::cos(a) * r * 0.55f, c.second + std::sin(a) * r * 0.55f};
        out.push(ellipse(p, r * 0.5f, r * 0.26f, a, petal, shade(rgb_of(petal), 0.8f)));
    }
    out.push(ellipse(c, r * 0.28f, r * 0.28f, 0.0f, centre, shade(rgb_of(centre), 0.75f)));
}

void leaf_shape(Sink& out, XY p, float ang, float len, const Rgba& col) {
    const XY c{p.first + std::cos(ang) * len * 0.5f, p.second + std::sin(ang) * len * 0.5f};
    out.push(ellipse(c, len * 0.5f, len * 0.22f, ang, shade(rgb_of(col), 1.12f), shade(rgb_of(col), 0.8f)));
}

void star(Sink& out, XY c, float r, int points, const Rgba& col, float rot) {
    for (int k = 0; k < points; ++k) {
        const float a = rot + static_cast<float>(k) / static_cast<float>(points) * kTau;
        out.push(capsule(c, {c.first + std::cos(a) * r, c.second + std::sin(a) * r}, r * 0.28f, 0.4f, col, col));
    }
}

// --- laying the shapes on a layer ------------------------------------------------------------------------------------------

constexpr int kCell = 128;  // (render2.rs CTILE: the shapes binned by cells of the grid)

// The shapes of each cell of kCell × kCell pixels over what they reach (a flat table: cell c's shapes are items[start[c]] up
// to items[start[c + 1]], in their order), and the most any cell holds.
struct Bins {
    fx::Rect used;
    int nx = 0, ny = 0, cx0 = 0, cy0 = 0;
    std::vector<std::uint32_t> start, items;
    std::size_t most = 0;
};

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// The engine's kernel laying `prims` (the layer's pixels) over the window's straight colours (render2.rs raster_tile and
// composite: each shape's coverage — premultiplied, laid in order normally, added or lightened — then the whole over the
// pixels).
fx::Kernel laying(std::shared_ptr<const std::vector<Prim>> prims, std::shared_ptr<const Bins> bins) {
    fx::Kernel k;
    k.kind = "render_picture";
    k.halo = [](const Json&, const fx::Ctx&) { return fx::Halo::margin(0); };
    // (a tile's list of the shapes of the cells it reaches: at most the most a cell holds, each cell it touches; and its
    // coverage of four floats a pixel)
    k.tile_bytes = [bins](const Json&, const fx::Ctx&, int side) {
        const std::uint64_t across = std::uint64_t(side) / kCell + 2;
        return std::min<std::uint64_t>(bins->items.size(), std::uint64_t(bins->most) * across * across) * sizeof(std::uint32_t) +
               std::uint64_t(side) * std::uint64_t(side) * sizeof(Rgba);
    };
    k.run = [prims, bins](const Json&, const fx::Window& src, const fx::Rect& out, const fx::Ctx&) {
        const std::size_t n = src.ch;
        std::vector<float> res = src.crop(out);
        const fx::Rect t = out.intersect(bins->used);
        if (t.empty()) return res;
        // (the shapes of the cells the tile reaches, in their order; at most what tile_bytes holds)
        std::vector<std::uint32_t> idx;
        for (int cy = floor_div(t.y0, kCell); cy <= floor_div(t.y1 - 1, kCell); ++cy)
            for (int cx = floor_div(t.x0, kCell); cx <= floor_div(t.x1 - 1, kCell); ++cx) {
                const int ix = cx - bins->cx0, iy = cy - bins->cy0;
                if (ix < 0 || iy < 0 || ix >= bins->nx || iy >= bins->ny) continue;
                const std::size_t c = std::size_t(iy) * std::size_t(bins->nx) + std::size_t(ix);
                idx.insert(idx.end(), bins->items.begin() + bins->start[c], bins->items.begin() + bins->start[c + 1]);
            }
        std::sort(idx.begin(), idx.end());
        idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
        if (idx.empty()) return res;
        const std::size_t w = static_cast<std::size_t>(t.width());
        std::vector<Rgba> buf(w * std::size_t(t.height()), Rgba{0, 0, 0, 0});
        for (const std::uint32_t i : idx) {
            const Prim& p = (*prims)[i];
            const fx::Rect r = bounds_of(p).intersect(t);
            if (r.empty()) continue;
            for (int y = r.y0; y < r.y1; ++y) {
                const std::size_t row = std::size_t(y - t.y0) * w;
                for (int x = r.x0; x < r.x1; ++x) {
                    const auto [cov, ct] = eval(p, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                    if (cov <= 0.0f) continue;
                    const Rgba c = p.c0 == p.c1 ? p.c0 : lerp4(p.c0, p.c1, ct);
                    const float a = std::clamp(c[3] * cov, 0.0f, 1.0f);
                    if (a <= 0.0f) continue;
                    Rgba& d = buf[row + std::size_t(x - t.x0)];
                    switch (p.blend) {
                        case Prim::Blend::Add:
                            d[0] += c[0] * a;
                            d[1] += c[1] * a;
                            d[2] += c[2] * a;
                            d[3] += a * (1.0f - d[3]);
                            break;
                        case Prim::Blend::Lighten:
                            d[0] = std::max(d[0], c[0] * a);
                            d[1] = std::max(d[1], c[1] * a);
                            d[2] = std::max(d[2], c[2] * a);
                            d[3] = std::max(d[3], a);
                            break;
                        case Prim::Blend::Normal: {
                            const float k2 = 1.0f - a;
                            d[0] = c[0] * a + d[0] * k2;
                            d[1] = c[1] * a + d[1] * k2;
                            d[2] = c[2] * a + d[2] * k2;
                            d[3] = a + d[3] * k2;
                            break;
                        }
                    }
                }
            }
        }
        const std::size_t ow = static_cast<std::size_t>(out.width());
        for (int y = t.y0; y < t.y1; ++y)
            for (int x = t.x0; x < t.x1; ++x) {
                const Rgba& s = buf[std::size_t(y - t.y0) * w + std::size_t(x - t.x0)];
                const float sa = std::clamp(s[3], 0.0f, 1.0f);
                if (sa <= 0.0f) continue;
                const float inv = 1.0f / std::max(s[3], 1e-6f);
                float* px = res.data() + (std::size_t(y - out.y0) * ow + std::size_t(x - out.x0)) * n;
                const float da = px[n - 1];
                const float oa = sa + da * (1.0f - sa);
                for (std::size_t c = 0; c < 3; ++c) {
                    const float sc = std::clamp(s[c] * inv, 0.0f, 1.0f);
                    px[c] = oa > 1e-7f ? (sc * sa + px[c] * da * (1.0f - sa)) / oa : 0.0f;
                }
                px[n - 1] = oa;
            }
        return res;
    };
    return k;
}

// --- the page's paths (炎) ------------------------------------------------------------------------------------------------

constexpr std::uint64_t kMostPathPoints = 1'000'000;

// How many points flattening `path` (working pixels) within `tol` makes at most (path_raster's pieces, before any is made).
std::uint64_t path_points(const core::vector::Path& path, double tol) {
    std::uint64_t total = 0;
    for (const core::vector::Subpath& sub : path.subpaths) {
        total += 2;
        for (const auto& seg : sub.segments()) {
            const auto& [p0, c1, c2, p1] = seg;
            const double d1 = std::hypot(p0.x - 2.0 * c1.x + c2.x, p0.y - 2.0 * c1.y + c2.y);
            const double d2 = std::hypot(c1.x - 2.0 * c2.x + p1.x, c1.y - 2.0 * c2.y + p1.y);
            const double size = std::hypot(c1.x - p0.x, c1.y - p0.y) + std::hypot(c2.x - c1.x, c2.y - c1.y) + std::hypot(p1.x - c2.x, p1.y - c2.y);
            const double t = std::max(std::min(tol, size * 1e-3), 1e-4);
            const double n = std::ceil(std::sqrt(0.75 * std::max(d1, d2) / t));
            total += !std::isfinite(n) ? 1 : static_cast<std::uint64_t>(std::clamp(n, 1.0, 65536.0));
        }
        if (total > kMostPathPoints) return total;
    }
    return total;
}

// The page's path the flames follow (`name`: a saved path; "work" or none: the work path, else the first saved one), in
// working pixels flattened within half a pixel (a closed subpath ending where it starts); none: the page has none.
std::optional<std::vector<std::vector<XY>>> flame_paths(const core::Page& page, const std::optional<std::string>& name) {
    const core::vector::Path* path = nullptr;
    std::string lower = name.value_or("");
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (name && !name->empty() && lower != "work") {  // (render_cmds.rs flame_paths: "work" in any case is the work path)
        for (const core::vector::NamedPath& named : page.paths)
            if (named.name == *name) path = &named.path;
        if (path == nullptr) throw OpError("render_picture flame: the page has no path named " + core::py_repr_str(*name));
    } else if (page.work_path && !page.work_path->empty()) {
        path = &*page.work_path;
    } else if (!page.paths.empty()) {
        path = &page.paths.front().path;
    }
    if (path == nullptr || path->empty()) return std::nullopt;
    const core::vector::Path working = path->transformed({kWorkingPerMm, 0, 0, kWorkingPerMm, 0, 0});
    if (path_points(working, 0.5) > kMostPathPoints)
        throw OpError("render_picture flame: the path is too long or too finely curved to follow (more than " + std::to_string(kMostPathPoints) + " points)");
    std::vector<std::vector<XY>> out;
    for (const path_raster::Polyline& line : path_raster::flatten_path(working, 0.5)) {
        std::vector<XY> pts;
        pts.reserve(line.pts.size() + 1);
        for (const auto& [x, y] : line.pts) pts.emplace_back(static_cast<float>(x), static_cast<float>(y));
        if (line.closed && !pts.empty()) pts.push_back(pts.front());
        if (!pts.empty()) out.push_back(std::move(pts));
    }
    if (out.empty()) return std::nullopt;
    return out;
}

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

}  // namespace

// --- the shapes of each kind -----------------------------------------------------------------------------------------------

std::uint64_t flame_shapes_at_most(const Json& spec, const std::vector<std::vector<XY>>& paths) { return count_shapes(flame_spec(spec), paths); }

fx::Rect lay(fx::Surface& surface, std::vector<Prim> shapes, const fx::Rect& clip, const std::string* cover, double s, const fx::Tiling& tiling) {
    fx::Rect used;
    for (const Prim& p : shapes) used = used.unite(bounds_of(p).intersect(clip));
    if (used.empty()) return used;
    auto bins = std::make_shared<Bins>();
    bins->used = used;
    bins->cx0 = floor_div(used.x0, kCell);
    bins->cy0 = floor_div(used.y0, kCell);
    bins->nx = floor_div(used.x1 - 1, kCell) - bins->cx0 + 1;
    bins->ny = floor_div(used.y1 - 1, kCell) - bins->cy0 + 1;
    std::uint64_t entries = 0;
    for (const Prim& p : shapes) {
        const fx::Rect r = bounds_of(p).intersect(used);
        if (r.empty()) continue;
        entries += std::uint64_t(floor_div(r.x1 - 1, kCell) - floor_div(r.x0, kCell) + 1) * std::uint64_t(floor_div(r.y1 - 1, kCell) - floor_div(r.y0, kCell) + 1);
    }
    const std::uint64_t cells = std::uint64_t(bins->nx) * std::uint64_t(bins->ny);
    const image_ops::Held bins_held((entries + 2 * cells + 1) * sizeof(std::uint32_t));  // (the table and its filling, counted before they are had)
    const auto each_cell = [&](const auto& visit) {
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            const fx::Rect r = bounds_of(shapes[i]).intersect(used);
            if (r.empty()) continue;
            for (int cy = floor_div(r.y0, kCell); cy <= floor_div(r.y1 - 1, kCell); ++cy)
                for (int cx = floor_div(r.x0, kCell); cx <= floor_div(r.x1 - 1, kCell); ++cx)
                    visit(std::size_t(cy - bins->cy0) * std::size_t(bins->nx) + std::size_t(cx - bins->cx0), static_cast<std::uint32_t>(i));
        }
    };
    bins->start.assign(std::size_t(cells) + 1, 0);
    each_cell([&](std::size_t c, std::uint32_t) { ++bins->start[c + 1]; });
    for (std::size_t c = 0; c < std::size_t(cells); ++c) {
        bins->most = std::max<std::size_t>(bins->most, bins->start[c + 1]);
        bins->start[c + 1] += bins->start[c];
    }
    bins->items.resize(std::size_t(entries));
    std::vector<std::uint32_t> at(bins->start.begin(), bins->start.end() - 1);
    each_cell([&](std::size_t c, std::uint32_t i) { bins->items[at[c]++] = i; });
    const auto prims = std::make_shared<const std::vector<Prim>>(std::move(shapes));
    fx::apply(surface, laying(prims, bins), Json::object(), core::FilterInks::Meaning, clip, cover, s, &used, tiling);
    return used;
}

std::vector<Prim> flame(const Json& spec_json, const std::vector<std::vector<XY>>& paths) {
    const FlameSpec spec = flame_spec(spec_json);
    if (count_shapes(spec, paths) > core::kPictureMostShapes) Sink::refuse();
    Sink out;
    Rng rng = Rng::make(spec.seed, 0xF1A3);
    each_flame(spec, paths, rng, [&](const Spine& spine, float width, const FlameSpec& s, bool candle) {
        draw_flame(out, spine, width, s, rng);
        if (!candle) return;
        // (the candle's bright core)
        const XY base = spine.pts.front();
        const float h = spine.len();
        const Rgba core = flame_colour(0.05f, spec.color);
        out.push(additive(with_soft(ellipse({base.first, base.second - h * 0.28f}, std::max(std::clamp(spec.width, 1.0f, 1000.0f) * 0.12f, 1.5f), h * 0.22f, 0.0f,
                                            {core[0], core[1], core[2], 0.9f * std::clamp(spec.opacity, 0.0f, 100.0f) / 100.0f},
                                            {core[0], core[1], core[2], 0.0f}),
                                    2.0f)));
    });
    return std::move(out.out);
}

std::vector<XY> default_flame_path(const Canvas& c) {
    std::vector<XY> out;
    for (int k = 0; k <= 32; ++k) {
        const float t = static_cast<float>(k) / 32.0f;
        out.emplace_back(c.x0 + c.width * (0.15f + 0.7f * t), c.y0 + c.height * (0.8f - 0.08f * std::sin(kPi * t)));
    }
    return out;
}

std::vector<Prim> tree(const Json& spec, const Canvas& canvas) {
    const int type = static_cast<int>(std::clamp(num(spec, "base_tree_type", 1), 1.0f, 34.0f));
    const TreeKind& kind = trees()[static_cast<std::size_t>(type - 1)];
    const float h = canvas.height * std::clamp(num(spec, "size", 0.8), 0.05f, 2.0f);
    const XY base{canvas.x0 + canvas.width * num(spec, "x", 0.5), canvas.y0 + canvas.height * num(spec, "y", 0.95)};
    static constexpr std::array<float, 5> kLights{180, 225, 270, 315, 0};
    const float la = kLights[static_cast<std::size_t>(std::clamp(num(spec, "light_direction", 3), 1.0f, 5.0f)) - 1] * kPi / 180.0f;
    const std::array<float, 3> leaf = flag(spec, "default_leaves", true) ? kind.leaf_col : rgb_of(colour(spec, "leaves_color", {0.24f, 0.52f, 0.16f, 1}));
    const std::array<float, 3> bark = flag(spec, "custom_branch_color", false) ? rgb_of(colour(spec, "branches_color", {0.36f, 0.25f, 0.16f, 1})) : kind.bark;
    const float thick = std::clamp(num(spec, "branches_thickness", 100), 50.0f, 200.0f) / 100.0f;
    const float bh = std::clamp(num(spec, "branches_height", 100), 50.0f, 300.0f) / 100.0f;
    const float leaves_size = std::clamp(num(spec, "leaves_size", 100), 0.0f, 200.0f);
    TreeCtx t{kind,
              num(spec, "leaves_amount", 50),
              leaves_size,
              flag(spec, "flat_shading", false),
              Rng::make(seed_of(spec), 0x7EEull + static_cast<std::uint64_t>(type)),
              {},
              {},
              {},
              bark,
              leaf,
              {std::cos(la), std::sin(la)},
              {base.first, base.second - h * 0.65f},
              std::max(h * 0.012f * leaves_size / 100.0f, 0.6f)};
    const float up = -kPi / 2.0f;
    const float r0 = std::max(h * kind.thick * thick, 0.8f);
    const float share = std::clamp(t.leaves_amount, 0.0f, 100.0f) / 100.0f * 0.9f;
    switch (kind.habit) {
        case Habit::Branching: {
            const float trunk = std::min(h * kind.trunk * bh, h * 0.92f);
            const auto [top, a] = t.limb(base, up, std::max(trunk, 1.0f), r0, r0 * 0.75f, 0.0f, share);
            // (the crown's length: the tree reaches about h)
            float geo = 0.0f;
            for (unsigned k = 0; k < kind.levels; ++k) geo += std::pow(kind.ratio, static_cast<float>(k));
            const float first = (std::max(h - trunk, h * 0.08f) / std::max(geo, 0.5f)) * 1.05f;
            const unsigned n = std::max(kind.children, 2u);
            for (unsigned c = 0; c < n; ++c) {
                const float side = static_cast<float>(c) / static_cast<float>(n - 1) * 2.0f - 1.0f;
                const float sp = kind.spread * kPi / 180.0f * 0.8f;
                const float ca = a + side * sp + t.rng.sym() * 0.15f;
                const float fl = first * t.rng.range(0.85f, 1.05f);
                t.branch(top, ca, fl, r0 * 0.7f, 1);
            }
            break;
        }
        case Habit::Conifer: {
            const XY top = t.limb(base, up, h, r0, r0 * 0.1f, 0.0f, share).first;
            const float start = std::min(h * kind.trunk * bh, h * 0.8f);
            const auto whorls = static_cast<std::size_t>(14.0f * std::clamp(std::sqrt(h / 300.0f), 0.6f, 2.5f));
            const float spread = kind.spread * kPi / 180.0f;
            for (std::size_t k = 0; k < whorls; ++k) {
                const float f = static_cast<float>(k) / static_cast<float>(whorls);
                const float y = base.second - start - (h - start) * f;
                const float x = base.first + (top.first - base.first) * ((base.second - y) / h);
                const float span = (h - start) * 0.42f * std::pow(1.0f - f, 0.9f) + h * 0.03f;
                for (const float side : {-1.0f, 1.0f}) {
                    const float ang = up + side * spread + t.rng.sym() * 0.1f;
                    t.branch({x, y}, ang, span, std::max(r0 * 0.35f * (1.0f - f), 0.6f), kind.levels);
                    if (kind.levels >= 2) {
                        const float s2 = std::sin(ang + side * 0.5f), c2 = std::cos(ang + side * 0.5f);
                        const XY mid{x + c2 * span * 0.5f, y + s2 * span * 0.5f};
                        t.branch(mid, ang + side * 0.6f, span * 0.4f, std::max(r0 * 0.15f, 0.5f), kind.levels);
                    }
                }
            }
            break;
        }
        case Habit::Palm: {
            const float trunk = std::max(h * kind.trunk * std::min(bh, 1.15f), 1.0f);
            const float lean = t.rng.sym() * kind.wiggle;
            const int segs = 12;
            XY p = base;
            for (int k = 0; k < segs; ++k) {
                const float f = static_cast<float>(k) / static_cast<float>(segs);
                const float a = up + lean * f;
                const XY q{p.first + std::cos(a) * trunk / static_cast<float>(segs), p.second + std::sin(a) * trunk / static_cast<float>(segs)};
                const float r = r0 * (1.0f - 0.35f * f);
                const Rgba ring = shade(bark, k % 2 == 0 ? 1.0f : 0.85f);
                t.branches.push(capsule(p, q, r, r * 0.97f, ring, ring));
                p = q;
            }
            const unsigned fronds = kind.children;
            const float flen = h * 0.38f * std::sqrt(std::clamp(leaves_size, 10.0f, 200.0f) / 100.0f);
            for (unsigned k = 0; k < fronds; ++k) {
                const float a = up + (static_cast<float>(k) / static_cast<float>(fronds) - 0.5f) * kPi * 1.7f + t.rng.sym() * 0.2f;
                XY q = p;
                float ang = a;
                const int fsegs = 10;
                const auto leaflets = static_cast<std::size_t>(std::round(std::clamp(t.leaves_amount, 0.0f, 100.0f) / 100.0f * 3.0f));
                for (int s = 0; s < fsegs; ++s) {
                    // (the fronds arch over and droop toward the ground)
                    const float d = kPi / 2.0f - ang;
                    ang += 0.06f * std::atan2(std::sin(d), std::cos(d));
                    const XY nq{q.first + std::cos(ang) * flen / static_cast<float>(fsegs), q.second + std::sin(ang) * flen / static_cast<float>(fsegs)};
                    const float k2 = t.light_at(nq);
                    t.leaf_push(k2 - 0.1f, capsule(q, nq, std::max(r0 * 0.15f, 0.6f), std::max(r0 * 0.1f, 0.4f), shade(t.leaf, k2 * 0.8f), shade(t.leaf, k2 * 0.8f)));
                    for (std::size_t j = 0; j < leaflets; ++j) {
                        const float along = static_cast<float>(j) / static_cast<float>(leaflets);
                        const XY at{q.first + (nq.first - q.first) * along, q.second + (nq.second - q.second) * along};
                        for (const float side : {-1.0f, 1.0f}) {
                            const float lang = ang + side * 1.1f;
                            const float ll = flen * 0.22f * (1.0f - static_cast<float>(s) / static_cast<float>(fsegs) * 0.7f);
                            const XY tip{at.first + std::cos(lang) * ll, at.second + std::sin(lang) * ll + ll * 0.25f};
                            t.leaf_push(k2, capsule(at, tip, std::max(ll * 0.08f, 0.5f), 0.3f, shade(t.leaf, k2), shade(t.leaf, k2 * 0.9f)));
                        }
                    }
                    q = nq;
                }
            }
            if (kind.accent) {
                for (int i = 0; i < 4; ++i) {
                    const XY c{p.first + t.rng.sym() * r0 * 2.0f, p.second + t.rng.f() * r0 * 2.0f};
                    t.leaf_push(2.0f, disc(c, r0 * 1.1f, shade(*kind.accent, 1.0f)));
                }
            }
            break;
        }
        case Habit::Bamboo: {
            const unsigned stalks = kind.children;
            for (unsigned k = 0; k < stalks; ++k) {
                const float x = base.first + (static_cast<float>(k) - static_cast<float>(stalks - 1) / 2.0f) * r0 * 5.0f + t.rng.sym() * r0;
                const float hh = h * t.rng.range(0.7f, 1.0f);
                const float a = up + t.rng.sym() * kind.spread * kPi / 180.0f * 0.5f;
                const int joints = 8;
                XY p{x, base.second};
                for (int j = 0; j < joints; ++j) {
                    const XY q{p.first + std::cos(a) * hh / static_cast<float>(joints), p.second + std::sin(a) * hh / static_cast<float>(joints)};
                    t.branches.push(capsule(p, q, r0, r0, shade(bark, 1.0f), shade(bark, 0.85f)));
                    t.branches.push(capsule({q.first - r0 * 1.1f, q.second}, {q.first + r0 * 1.1f, q.second}, r0 * 0.25f, r0 * 0.25f, shade(bark, 0.7f), shade(bark, 0.7f)));
                    if (j >= joints / 3) {
                        const auto leaves = static_cast<std::size_t>(std::round(std::clamp(t.leaves_amount, 0.0f, 100.0f) / 100.0f * 4.0f));
                        for (std::size_t i = 0; i < leaves; ++i) {
                            const float side = t.rng.f() < 0.5f ? -1.0f : 1.0f;
                            const float lang = a + side * t.rng.range(0.6f, 1.4f);
                            const float ll = t.leaf_px * 2.2f;
                            t.add_leaf({q.first + std::cos(lang) * ll, q.second + std::sin(lang) * ll}, lang);
                        }
                    }
                    p = q;
                }
            }
            break;
        }
    }
    // (the back leaves first: the darker ones under, the lit ones on top)
    std::stable_sort(t.leaves.begin(), t.leaves.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Prim> out = std::move(t.branches.out);
    out.reserve(out.size() + t.leaves.size());
    for (auto& [order, p] : t.leaves) out.push_back(p);
    return out;
}

std::vector<Prim> picture_frame(const Json& spec, const Canvas& canvas) {
    Sink out;
    const float shortest = std::max(std::min(canvas.width, canvas.height), 8.0f);
    const float margin = shortest * std::clamp(num(spec, "margin", 4), 0.0f, 30.0f) / 100.0f;
    const float unit = shortest * (0.008f + std::clamp(num(spec, "size", 50), 1.0f, 100.0f) / 100.0f * 0.05f);
    const float thick = std::max(unit * 0.04f + shortest * 0.00012f * std::clamp(num(spec, "thickness", 30), 1.0f, 100.0f), 0.6f);
    const float spacing = unit * (4.0f - 3.2f * std::clamp(num(spec, "arrangement", 50), 1.0f, 100.0f) / 100.0f);
    const float inset = margin + unit;
    const Border b{canvas.x0 + inset, canvas.y0 + inset, std::max(canvas.width - 2.0f * inset, 1.0f), std::max(canvas.height - 2.0f * inset, 1.0f)};
    const float total = b.len();
    const auto count = static_cast<std::size_t>(std::max(std::round(total / spacing), 4.0f));
    if (count > core::kPictureMostShapes) Sink::refuse();
    const float step = total / static_cast<float>(count);
    const int frame = static_cast<int>(std::clamp(num(spec, "frame", 1), 1.0f, static_cast<float>(core::frame_style_names().size())));
    Rng rng = Rng::make(seed_of(spec), 0xF8A3Eull + static_cast<std::uint64_t>(frame));
    const Rgba vine = colour(spec, "vine_color", {0.25f, 0.4f, 0.15f, 1}), fl = colour(spec, "flower_color", {0.9f, 0.3f, 0.45f, 1}),
               lf = colour(spec, "leaf_color", {0.3f, 0.6f, 0.2f, 1});
    const std::string_view style = core::frame_style_names()[static_cast<std::size_t>(frame - 1)];
    const auto lines = static_cast<std::size_t>(std::clamp(num(spec, "lines", 1), 1.0f, 5.0f));
    const Rgba white{1, 1, 1, 1}, yellow{0.98f, 0.82f, 0.2f, 1};
    // (a wavy vine along the border)
    const auto vine_path = [&](float amp, float wl, const Rgba& col, float r) {
        const auto n = static_cast<std::size_t>(std::max(std::ceil(total / 3.0f), 8.0f));
        std::optional<XY> prev;
        for (std::size_t k = 0; k <= n; ++k) {
            const float s = static_cast<float>(k) / static_cast<float>(n) * total;
            const Border::At at = b.at(s);
            const float o = amp * std::sin(s / wl * kTau);
            const XY q{at.p.first + at.n.first * o, at.p.second + at.n.second * o};
            if (prev) out.push(capsule(*prev, q, r, r, col, col));
            prev = q;
        }
    };
    const auto wave_off = [](float s, float amp, float wl) { return amp * std::sin(s / wl * kTau); };
    if (style == "vine_with_flowers" || style == "vine_with_leaves" || style == "ivy" || style == "roses" || style == "daisies" || style == "berries") {
        const float amp = unit * 0.5f, wl = step * 2.0f;
        vine_path(amp, wl, vine, thick);
        for (std::size_t k = 0; k < count; ++k) {
            const float s = static_cast<float>(k) * step + step * 0.5f;
            const Border::At at = b.at(s);
            const float o = wave_off(s, amp, wl);
            const XY q{at.p.first + at.n.first * o, at.p.second + at.n.second * o};
            const float side = k % 2 == 0 ? 1.0f : -1.0f;
            const float ta = std::atan2(at.t.second, at.t.first);
            const float la = ta + side * 0.9f + rng.sym() * 0.3f;
            const float ll = unit * rng.range(0.9f, 1.2f);
            if (style == "vine_with_flowers") {
                leaf_shape(out, q, la, ll, lf);
                if (k % 2 == 0) {
                    const XY c{q.first + at.n.first * unit * 0.7f * side, q.second + at.n.second * unit * 0.7f * side};
                    flower(out, c, unit * 0.65f, 5, fl, yellow, rng.f() * 6.3f);
                }
            } else if (style == "vine_with_leaves") {
                leaf_shape(out, q, la, ll, lf);
                leaf_shape(out, q, ta - side * 0.9f, ll * 0.8f, lf);
            } else if (style == "ivy") {
                for (int j = 0; j < 3; ++j) {
                    const float a = la + (static_cast<float>(j) - 1.0f) * 0.7f;
                    leaf_shape(out, q, a, ll * 0.75f, shade(rgb_of(lf), 0.75f + 0.15f * static_cast<float>(j)));
                }
            } else if (style == "roses") {
                leaf_shape(out, q, la, ll, lf);
                if (k % 3 == 0) {
                    const float r = unit * 0.7f;
                    const float fs[4] = {1.0f, 0.75f, 0.5f, 0.28f};
                    for (int j = 0; j < 4; ++j) {
                        const Rgba c = shade(rgb_of(fl), 0.75f + 0.1f * static_cast<float>(j));
                        out.push(ellipse(q, r * fs[j], r * fs[j] * 0.9f, rng.f() * 3.0f, c, shade(rgb_of(fl), 0.55f)));
                    }
                }
            } else if (style == "daisies") {
                leaf_shape(out, q, la, ll * 0.8f, lf);
                if (k % 2 == 1) flower(out, q, unit * 0.6f, 12, white, yellow, rng.f());
            } else {  // (berries)
                leaf_shape(out, q, la, ll, lf);
                for (int j = 0; j < 3; ++j) {
                    const XY c{q.first + rng.sym() * unit * 0.4f, q.second + rng.sym() * unit * 0.4f};
                    out.push(ellipse(c, unit * 0.2f, unit * 0.2f, 0.0f, shade(rgb_of(fl), 1.2f), shade(rgb_of(fl), 0.6f)));
                }
            }
        }
    } else if (style == "bamboo") {
        for (int edge = 0; edge < 4; ++edge) {
            const float s0 = edge == 0 ? 0.0f : edge == 1 ? b.w : edge == 2 ? b.w + b.h : 2.0f * b.w + b.h;
            const float l = edge % 2 == 0 ? b.w : b.h;
            const XY a = b.at(s0 + 0.01f).p, z = b.at(s0 + l - 0.01f).p;
            const auto joints = static_cast<std::size_t>(std::max(std::round(l / (spacing * 2.0f)), 1.0f));
            for (std::size_t j = 0; j < joints; ++j) {
                const float f0 = static_cast<float>(j) / static_cast<float>(joints), f1 = static_cast<float>(j + 1) / static_cast<float>(joints);
                const XY p0{a.first + (z.first - a.first) * f0, a.second + (z.second - a.second) * f0};
                const XY p1{a.first + (z.first - a.first) * f1, a.second + (z.second - a.second) * f1};
                out.push(capsule(p0, p1, unit * 0.35f, unit * 0.35f, vine, shade(rgb_of(vine), 0.8f)));
                out.push(disc(p1, unit * 0.4f, shade(rgb_of(vine), 0.7f)));
            }
            for (std::size_t i = 0; i < std::max<std::size_t>(joints / 2, 1); ++i) {  // (a few leaves along each edge)
                const float f = rng.f();
                leaf_shape(out, {a.first + (z.first - a.first) * f, a.second + (z.second - a.second) * f}, rng.f() * 6.3f, unit * 1.4f, lf);
            }
        }
    } else if (style == "waves" || style == "zigzag" || style == "double_line" || style == "rope") {
        for (std::size_t li = 0; li < lines; ++li) {
            const float off = (static_cast<float>(li) - static_cast<float>(lines - 1) / 2.0f) * thick * 4.0f;
            const auto n = static_cast<std::size_t>(std::max(std::ceil(total / 2.0f), 8.0f));
            std::optional<XY> prev;
            for (std::size_t k = 0; k <= n; ++k) {
                const float s = static_cast<float>(k) / static_cast<float>(n) * total;
                const Border::At at = b.at(s);
                float o = off;
                if (style == "waves") {
                    o += wave_off(s + static_cast<float>(li) * step * 0.25f, unit * 0.4f, step);
                } else if (style == "zigzag") {
                    const float ph = s / step - std::floor(s / step);
                    o += unit * 0.4f * (4.0f * std::fabs(ph - 0.5f) - 1.0f);
                }
                const XY q{at.p.first + at.n.first * o, at.p.second + at.n.second * o};
                if (style == "rope") {
                    if (k % 2 == 0)
                        out.push(ellipse(q, unit * 0.45f, unit * 0.2f, std::atan2(at.t.second, at.t.first) + 0.7f, shade(rgb_of(vine), 1.1f),
                                         shade(rgb_of(vine), 0.65f)));
                } else if (prev) {
                    out.push(capsule(*prev, q, thick, thick, vine, vine));
                }
                prev = q;
            }
            if (style == "double_line") {  // (a second rule inside each line, and the corners' squares)
                const Border inner{b.x0 + unit * 0.6f + off, b.y0 + unit * 0.6f + off, b.w - 2.0f * (unit * 0.6f + off), b.h - 2.0f * (unit * 0.6f + off)};
                const float from[4] = {0.0f, inner.w, inner.w + inner.h, 2.0f * inner.w + inner.h};
                const float to[4] = {inner.w, inner.w + inner.h, 2.0f * inner.w + inner.h, inner.len() - 0.001f};
                for (int e = 0; e < 4; ++e) out.push(capsule(inner.at(from[e]).p, inner.at(to[e]).p, thick * 0.6f, thick * 0.6f, vine, vine));
            }
        }
        if (style == "double_line") {
            const float corners[4] = {0.0f, b.w, b.w + b.h, 2.0f * b.w + b.h};
            for (const float s : corners) out.push(ellipse(b.at(s).p, unit * 0.45f, unit * 0.45f, kPi / 4.0f, fl, fl));
        }
    } else {  // (dots, scallops, snowflakes, stars, hearts)
        for (std::size_t k = 0; k < count; ++k) {
            const float s = static_cast<float>(k) * step;
            const Border::At at = b.at(s);
            const float ta = std::atan2(at.t.second, at.t.first);
            if (style == "dots") {
                const float r = unit * (k % 2 == 0 ? 0.4f : 0.22f);
                out.push(ellipse(at.p, r, r, 0.0f, shade(rgb_of(fl), 1.15f), shade(rgb_of(fl), 0.75f)));
            } else if (style == "scallops") {
                const XY c0 = b.at(s - step * 0.5f).p;
                const XY mid{(c0.first + at.p.first) / 2.0f, (c0.second + at.p.second) / 2.0f};
                const int n = 8;
                const float r = step * 0.5f;
                const auto pt = [&](float a) {
                    return XY{mid.first - at.t.first * r * std::cos(a) - at.n.first * r * std::sin(a), mid.second - at.t.second * r * std::cos(a) - at.n.second * r * std::sin(a)};
                };
                for (int j = 0; j < n; ++j)
                    out.push(capsule(pt(kPi * static_cast<float>(j) / static_cast<float>(n)), pt(kPi * static_cast<float>(j + 1) / static_cast<float>(n)), thick, thick, vine, vine));
            } else if (style == "snowflakes") {
                const float r = unit * 0.6f;
                const Rgba c{0.85f, 0.92f, 1.0f, 1.0f};
                for (int j = 0; j < 6; ++j) {
                    const float a = ta + static_cast<float>(j) * kPi / 3.0f;
                    const XY e{at.p.first + std::cos(a) * r, at.p.second + std::sin(a) * r};
                    out.push(capsule(at.p, e, std::max(thick * 0.6f, 0.6f), std::max(thick * 0.4f, 0.5f), c, c));
                    const XY m{at.p.first + std::cos(a) * r * 0.6f, at.p.second + std::sin(a) * r * 0.6f};
                    for (const float sd : {-0.7f, 0.7f}) {
                        const XY b2{m.first + std::cos(a + sd) * r * 0.3f, m.second + std::sin(a + sd) * r * 0.3f};
                        out.push(capsule(m, b2, std::max(thick * 0.4f, 0.5f), 0.4f, c, c));
                    }
                }
            } else if (style == "stars") {
                star(out, at.p, unit * 0.55f, 5, k % 2 == 0 ? yellow : fl, ta - kPi / 2.0f);
            } else {  // (a heart: two lobes and a point toward the inside)
                const float r = unit * 0.3f;
                const float tx = at.t.first, ty = at.t.second;
                const XY l{at.p.first - tx * r * 0.8f - at.n.first * r * 0.3f, at.p.second - ty * r * 0.8f - at.n.second * r * 0.3f};
                const XY rr{at.p.first + tx * r * 0.8f - at.n.first * r * 0.3f, at.p.second + ty * r * 0.8f - at.n.second * r * 0.3f};
                const XY tip{at.p.first + at.n.first * r * 1.6f, at.p.second + at.n.second * r * 1.6f};
                out.push(capsule(l, tip, r, 0.6f, fl, fl));
                out.push(capsule(rr, tip, r, 0.6f, fl, fl));
            }
        }
    }
    // (fade: the ornaments reaching further inside less opaque)
    const float fade = std::clamp(num(spec, "fade", 0), 0.0f, 100.0f) / 100.0f;
    if (fade > 0.0f) {
        const float cx = canvas.x0 + canvas.width / 2.0f, cy = canvas.y0 + canvas.height / 2.0f;
        for (Prim& p : out.out) {
            const float edge = std::clamp(std::max(std::fabs(p.x0 - cx) / (b.w / 2.0f), std::fabs(p.y0 - cy) / (b.h / 2.0f)), 0.0f, 1.0f);
            const float k = std::clamp(1.0f - fade * (1.0f - edge) * 8.0f, 0.0f, 1.0f);
            p.c0[3] *= k;
            p.c1[3] *= k;
        }
    }
    return std::move(out.out);
}

// --- the op ------------------------------------------------------------------------------------------------------------------

void render_picture(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* kind_given = core::get(op, "kind");
    if (kind_given == nullptr || !kind_given->is_string() || core::picture_kind(kind_given->get_ref<const std::string&>()) == nullptr)
        throw OpError("render_picture's kind must be flame, picture_frame or tree");
    const std::string kind = kind_given->get<std::string>();
    Json given = Json::object();
    for (const auto& [key, value] : op.items()) {
        if (key != "op" && key != "page" && key != "id" && key != "new_layer" && key != "kind" && key != "area") given[key] = value;
    }
    const Json spec = core::normalize_picture(kind, given);
    if (const Json* flag_given = core::get(op, "new_layer"); flag_given != nullptr && !flag_given->is_null() && !flag_given->is_boolean())
        throw OpError("render_picture's new_layer must be true or false");
    const bool asked_new = core::truthy_at(op, "new_layer");
    std::string id;
    if (const Json* named = core::get(op, "id"); named != nullptr && !named->is_null()) {
        if (!named->is_string() || named->get_ref<const std::string&>().empty()) throw OpError("render_picture's id must be a layer's id");
        id = named->get<std::string>();
    }
    const std::optional<Json> area = core::truthy_at(op, "area") ? std::optional<Json>(core::op_area(op)) : std::nullopt;
    // the layer: the paint layer named, else a new one above it (or on top)
    bool made = asked_new || id.empty();
    if (!id.empty()) {
        const core::Page& seen = doc.page(at);
        const core::Layer* found = nullptr;
        for (const core::Layer& l : seen.layers)
            if (l.id == id) found = &l;
        if (found == nullptr) throw OpError("no layer " + id + " on the page");
        if (found->kind != core::LayerKind::Raster && found->kind != core::LayerKind::Strokes) made = true;  // (not a paint layer: a new one)
    }
    if (made) {
        core::Page& page = doc.edit_page(at);
        core::Layer layer;
        layer.id = core::new_id();
        layer.role = core::LayerRole::User;
        layer.kind = core::LayerKind::Raster;
        layer.title = kind == "flame" ? "炎" : kind == "tree" ? "木" : "額縁";
        layer.exportable = true;
        layer.panel_clip = false;  // (it runs past the panels: a frame round the page, a tree over the gutter)
        std::size_t index = page.layers.size();
        for (std::size_t i = 0; i < page.layers.size(); ++i) {
            if (page.layers[i].id != id) continue;
            index = i + 1;
            layer.parent_id = page.layers[i].parent_id;
        }
        id = layer.id;
        page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
    }
    // (an RGB page of 16 or 32 bits: an empty paint layer of its precision; the named one ready — locked refused, not a
    // paint layer refused, precise pen lines made precise paint —, its pixels read once below)
    if (paint_fill::becomes_precise(doc.page(at), doc.page(at).layers[core::layer_by_id(doc.page(at), id)])) paint_fill::make_precise(doc, at, id);
    image_ops::ready_paint_layer(c, at, id);
    core::Page& page = doc.edit_page(at);
    core::Layer& layer = page.layers[core::layer_by_id(page, id)];

    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    // the surface of its pixels (one of the three ways; an 8-bit layer's lines and fills made pixels — as filter_raster
    // bakes them — only when something is drawn, below)
    std::unique_ptr<fx::Surface> surface;
    std::optional<core::InkPixels> inks;
    Image eight;
    bool bake = false;
    if (layer.color_raster) {
        if (layer.stroke_count() > 0 || !layer.patches.empty()) throw OpError("the lines of the layer " + id + " are not in its precise pixels yet");
        surface = fx::precise_surface(*layer.color_raster);
    } else if (page.color && !layer.raster_png) {
        inks = ink_edit::with_marks(page, layer, &doc);
        surface = fx::ink_surface(*inks, *page.color);
    } else {
        bake = layer.stroke_count() > 0 || !layer.patches.empty() || shape_parts::any(layer);
        eight = raster::layer_pixels(page, layer, &doc).convert("RGBA");
        surface = fx::eight_surface(eight);
    }
    const std::uint32_t w = surface->width(), h = surface->height();
    const double s = image_ops::grid_scale(page, w, h);
    const double width_mm = page.spec.width_mm.value();
    const double per_mm = w / std::max(width_mm, 1e-9);  // (the grid's pixels a mm)
    const fx::Rect grid{0, 0, static_cast<int>(w), static_cast<int>(h)};
    // the selection's coverage and box (its frame and clip), else the trim
    std::string cover;
    std::optional<image_ops::Held> cover_held;
    fx::Rect clip = grid;
    Canvas canvas;
    if (area) {
        cover_held.emplace(std::uint64_t(w) * h * 2);
        if (inks || layer.color_raster) {
            cover = ink_edit::area_coverage(page, w, h, *area);
        } else {
            const selection::AreaMask inside = selection::area_mask(*area, raster::dpi_across(page, static_cast<int>(w)));
            Image mask = Image::create("L", Size{static_cast<int>(w), static_cast<int>(h)}, Ink(0));
            mask.paste(inside.mask, Point{static_cast<int>(inside.x0), static_cast<int>(inside.y0)});
            cover = mask.tobytes();
        }
        int x0 = static_cast<int>(w), y0 = static_cast<int>(h), x1 = 0, y1 = 0;
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                if (cover[std::size_t(y) * w + x] == 0) continue;
                x0 = std::min(x0, static_cast<int>(x));
                x1 = std::max(x1, static_cast<int>(x) + 1);
                y0 = std::min(y0, static_cast<int>(y));
                y1 = std::max(y1, static_cast<int>(y) + 1);
            }
        clip = fx::Rect{x0, y0, x1, y1}.intersect(grid);
        canvas = Canvas{static_cast<float>(clip.x0 / s), static_cast<float>(clip.y0 / s), static_cast<float>(std::max(clip.width(), 1) / s),
                        static_cast<float>(std::max(clip.height(), 1) / s)};
    } else {
        const core::Rect trim = page.trim_rect_mm();
        canvas = Canvas{static_cast<float>(trim.x.value() * kWorkingPerMm), static_cast<float>(trim.y.value() * kWorkingPerMm),
                        static_cast<float>(trim.width.value() * kWorkingPerMm), static_cast<float>(trim.height.value() * kWorkingPerMm)};
        // (木・額縁 within it, as upstream's within its canvas; 炎 follows the page's path wherever it goes)
        if (kind != "flame") {
            const fx::Rect within{static_cast<int>(std::floor(canvas.x0 * s)), static_cast<int>(std::floor(canvas.y0 * s)),
                                  static_cast<int>(std::ceil((canvas.x0 + canvas.width) * s)), static_cast<int>(std::ceil((canvas.y0 + canvas.height) * s))};
            clip = clip.intersect(within);
        }
    }
    // the shapes (working pixels), held in the budget while they live
    Json report = Json::object();
    std::vector<Prim> shapes;
    std::optional<image_ops::Held> shapes_held;
    if (kind == "flame") {
        std::optional<std::string> path_name;
        if (const Json* p = core::get(spec, "path"); p != nullptr && p->is_string()) path_name = p->get<std::string>();
        auto paths = flame_paths(page, path_name);
        report["used_path"] = paths.has_value();
        if (!paths) {  // (none: upstream's arc across the lower part of the frame; a candle at one place, the middle at 0.75 down)
            if (flame_spec(spec).type == FlameType::CandleLight)
                paths = std::vector<std::vector<XY>>{{XY{canvas.x0 + canvas.width * 0.5f, canvas.y0 + canvas.height * 0.75f}}};
            else
                paths = std::vector<std::vector<XY>>{default_flame_path(canvas)};
        }
        const std::uint64_t most = flame_shapes_at_most(spec, *paths);
        if (most > core::kPictureMostShapes) Sink::refuse();  // (counted before any is made)
        shapes = flame(spec, *paths);                          // (held a block at a time while they are made)
        shapes_held.emplace(std::uint64_t(shapes.size()) * sizeof(Prim));
    } else if (kind == "tree") {
        shapes = tree(spec, canvas);  // (held a block at a time while they are made)
        shapes_held.emplace(std::uint64_t(shapes.size()) * sizeof(Prim));
        report["tree_type"] = std::string(core::tree_type_names()[static_cast<std::size_t>(spec["base_tree_type"].get<int>() - 1)]);
    } else {
        shapes = picture_frame(spec, canvas);
        shapes_held.emplace(std::uint64_t(shapes.size()) * sizeof(Prim));
        report["frame"] = std::string(core::frame_style_names()[static_cast<std::size_t>(spec["frame"].get<int>() - 1)]);
    }
    segment::check_stop();
    // onto the layer's grid (its pixels the working ones × s), what they reach within the clip, and their cells
    const auto scale = static_cast<float>(s);
    fx::Rect used;
    for (Prim& p : shapes) {
        p.x0 *= scale, p.y0 *= scale, p.x1 *= scale, p.y1 *= scale, p.r0 *= scale, p.r1 *= scale, p.soft *= scale;
        used = used.unite(bounds_of(p).intersect(clip));
    }
    const std::size_t count = shapes.size();
    if (!used.empty()) {
        if (bake) {  // (an 8-bit layer's lines and fills first: the shapes laid over them)
            raster::bake_vectors(page, layer, &doc, core::profiled_rgb(page));
            eight = raster::layer_pixels(page, layer, &doc).convert("RGBA");
            surface = fx::eight_surface(eight);
        }
        // (the shapes laid over the colours the layer's pixels mean; the 8-bit way kept to the selection after, as filter_raster
        // keeps it; the others mixed by its coverage)
        const bool eight_way = !inks && !layer.color_raster;
        (void)lay(*surface, std::move(shapes), clip, eight_way || cover.empty() ? nullptr : &cover, s);
        if (layer.color_raster) {
            layer.color_raster = std::make_shared<const std::string>(fx::take_bytes(*surface));
        } else if (inks) {
            layer.ink_raster = std::make_shared<const std::string>(fx::take_bytes(*surface));
            layer.ink_memo = {};
            layer.strokes = core::empty_strokes();  // (its marks are in its inks: as filter_raster leaves it)
            layer.shapes = core::vector::empty_shapes();
            layer.patches.clear();
        } else {
            Image laid = fx::take_image(*surface);
            if (!cover.empty()) laid = filters::within(eight, laid, Image::frombytes("L", Size{static_cast<int>(w), static_cast<int>(h)}, cover));
            raster::save_raster(page, layer, laid);
        }
        layer.kind = core::LayerKind::Raster;
    }
    report["id"] = id;
    report["new_layer"] = made;
    report["primitives"] = count;
    report["bounds_mm"] = used.empty() ? Json(nullptr)
                                       : Json::array({round3(used.x0 / per_mm), round3(used.y0 / per_mm), round3(used.width() / per_mm), round3(used.height() / per_mm)});
    // (the report in its order: what it drew on, what it drew, then the kind's own)
    Json ordered{{"id", report["id"]}, {"new_layer", report["new_layer"]}, {"bounds_mm", report["bounds_mm"]}, {"primitives", report["primitives"]}};
    for (const char* key : {"used_path", "tree_type", "frame"})
        if (report.contains(key)) ordered[key] = report[key];
    c.report = std::move(ordered);
}

}  // namespace genko::render::picture
