// PC4 節 D3 (SPEC.md COLOR-08「フィルター」, design 2.4, 4.2): フィルターギャラリー's kernel (filter_gallery: a stack of the
// 47 effects of core/gallery_kinds, laid on in turn) — PhotoCraft v0.2.0's algo/src/artistic_fx.rs (1-1247: each
// effect's pixel work and its reach), fxutil.rs (box and Gaussian blurs of interleaved planes, cell_point) and noise.rs
// (hash01), MIT: native/third_party/photocraft/NOTICE.md. Every effect works the window's straight colours (rgb 0..1 —
// the colours a page of inks means, core::FilterInks::Meaning —, the alpha kept as it was) and may read as far round each
// pixel as its reach; the stack reads the sum of its effects' reaches. What Genko does its own way:
//  - lengths of the working 200 dpi laid on the layer's grid (Ctx::s): a blur's sigma, a box's and a stroke's radius, a
//    shift, the gradient's step; marks with a size — strokes, paper, canvas, cells, noise fields — at the working
//    coordinates (a layer pixel's centre over s), so a 600 dpi layer shows the same marks at the same size on the paper
//    as a 200 dpi one; a grain of one pixel (粒状フィルム, the brushes' grain, 光彩拡散's, ノート用紙's, 粒状's) at the
//    layer's own pixels, as ノイズを加える.
//  - the blurs' running sums in whole numbers (fixed point), so a pixel's result does not depend on where its window
//    starts: the same bytes whatever the tiling and the threads (PhotoCraft's are within 1e-3 of each other).
//  - each effect's reach counted on the layer's grid from what it reads (the window holds all of it).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

#include "core/gallery_kinds.hpp"
#include "core/pyops.hpp"
#include "render/fx.hpp"

namespace genko::render::fx {

namespace {

using core::Json;
using Px = std::array<float, 3>;

// --- the effects ------------------------------------------------------------------------------------------------------

enum class F {
    ColoredPencil, Cutout, DryBrush, FilmGrain, Fresco, NeonGlow, PaintDaubs, PaletteKnife, PlasticWrap, PosterEdges, RoughPastels,
    SmudgeStick, Sponge, Underpainting, Watercolor, AccentedEdges, AngledStrokes, Crosshatch, DarkStrokes, InkOutlines, Spatter,
    SprayedStrokes, SumiE, DiffuseGlow, Glass, OceanRipple, BasRelief, ChalkCharcoal, Charcoal, Chrome, ConteCrayon, GraphicPen,
    HalftonePattern, NotePaper, Photocopy, Plaster, Reticulation, Stamp, TornEdges, WaterPaper, GlowingEdges, Craquelure, Grain,
    MosaicTiles, Patchwork, StainedGlass, Texturizer,
};

constexpr std::array<std::pair<std::string_view, F>, 47> kFilters{{
    {"colored_pencil", F::ColoredPencil}, {"cutout", F::Cutout}, {"dry_brush", F::DryBrush}, {"film_grain", F::FilmGrain},
    {"fresco", F::Fresco}, {"neon_glow", F::NeonGlow}, {"paint_daubs", F::PaintDaubs}, {"palette_knife", F::PaletteKnife},
    {"plastic_wrap", F::PlasticWrap}, {"poster_edges", F::PosterEdges}, {"rough_pastels", F::RoughPastels}, {"smudge_stick", F::SmudgeStick},
    {"sponge", F::Sponge}, {"underpainting", F::Underpainting}, {"watercolor", F::Watercolor}, {"accented_edges", F::AccentedEdges},
    {"angled_strokes", F::AngledStrokes}, {"crosshatch", F::Crosshatch}, {"dark_strokes", F::DarkStrokes}, {"ink_outlines", F::InkOutlines},
    {"spatter", F::Spatter}, {"sprayed_strokes", F::SprayedStrokes}, {"sumi_e", F::SumiE}, {"diffuse_glow", F::DiffuseGlow},
    {"glass", F::Glass}, {"ocean_ripple", F::OceanRipple}, {"bas_relief", F::BasRelief}, {"chalk_charcoal", F::ChalkCharcoal},
    {"charcoal", F::Charcoal}, {"chrome", F::Chrome}, {"conte_crayon", F::ConteCrayon}, {"graphic_pen", F::GraphicPen},
    {"halftone_pattern", F::HalftonePattern}, {"note_paper", F::NotePaper}, {"photocopy", F::Photocopy}, {"plaster", F::Plaster},
    {"reticulation", F::Reticulation}, {"stamp", F::Stamp}, {"torn_edges", F::TornEdges}, {"water_paper", F::WaterPaper},
    {"glowing_edges", F::GlowingEdges}, {"craquelure", F::Craquelure}, {"grain", F::Grain}, {"mosaic_tiles", F::MosaicTiles},
    {"patchwork", F::Patchwork}, {"stained_glass", F::StainedGlass}, {"texturizer", F::Texturizer},
}};

// One effect of the stack with its settings (PhotoCraft's GalleryEffect): its numbers (checks as 0 or 1), its choices, its
// colours (straight, 0..1).
struct Effect {
    F filter = F::Cutout;
    std::map<std::string, float, std::less<>> values;
    std::map<std::string, std::string, std::less<>> choices;
    Px fg{0, 0, 0}, bg{1, 1, 1}, glow{0, 0, 0};
    float get(std::string_view key) const {
        const auto it = values.find(key);
        return it == values.end() ? 0.0f : it->second;
    }
    bool flag(std::string_view key) const { return get(key) != 0.0f; }
    std::string_view choice(std::string_view key) const {
        const auto it = choices.find(key);
        return it == choices.end() ? std::string_view() : std::string_view(it->second);
    }
};

Px colour_of(const Json* c, const Px& fallback) {
    if (c == nullptr || !c->is_array() || c->size() != 3) return fallback;
    Px out{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!(*c)[i].is_number()) return fallback;
        out[i] = static_cast<float>(std::clamp((*c)[i].get<double>(), 0.0, 255.0) / 255.0);
    }
    return out;
}

// The stack's visible effects, each with its settings (their defaults where they are not given) and colours (its own, else
// the stack's: black ink, white paper).
std::vector<Effect> effects_of(const Json& params) {
    std::vector<Effect> out;
    const Px fg = colour_of(core::get(params, "foreground"), Px{0, 0, 0});
    const Px bg = colour_of(core::get(params, "background"), Px{1, 1, 1});
    const Json* list = core::get(params, "effects");
    if (list == nullptr || !list->is_array()) return out;
    for (const Json& item : *list) {
        if (!item.is_object()) continue;
        if (const Json* visible = core::get(item, "visible"); visible != nullptr && visible->is_boolean() && !visible->get<bool>()) continue;
        const Json* name = core::get(item, "filter");
        if (name == nullptr || !name->is_string()) continue;
        const core::GalleryEffectKind* kind = core::gallery_effect(name->get_ref<const std::string&>());
        if (kind == nullptr) continue;
        Effect e;
        for (const auto& [key, f] : kFilters) {
            if (key == kind->filter) e.filter = f;
        }
        const Json* given = core::get(item, "params");
        for (const core::AdjustParam& p : kind->params) {
            const Json* v = given != nullptr && given->is_object() ? core::get(*given, p.key) : nullptr;
            const std::string key(p.key);
            switch (p.type) {
                case core::AdjustParam::Type::Choice: {
                    std::string chosen(p.text);
                    if (v != nullptr && v->is_string() && std::find(p.choices.begin(), p.choices.end(), v->get<std::string>()) != p.choices.end())
                        chosen = v->get<std::string>();
                    e.choices[key] = chosen;
                    break;
                }
                case core::AdjustParam::Type::Bool: e.values[key] = v != nullptr && v->is_boolean() ? (v->get<bool>() ? 1.0f : 0.0f) : static_cast<float>(p.fallback); break;
                case core::AdjustParam::Type::Rgb: {
                    const Px fallback{static_cast<float>(p.numbers[0] / 255.0), static_cast<float>(p.numbers[1] / 255.0), static_cast<float>(p.numbers[2] / 255.0)};
                    e.glow = colour_of(v, fallback);
                    break;
                }
                default: {
                    double n = p.fallback;
                    if (v != nullptr && v->is_number() && !v->is_boolean() && std::isfinite(v->get<double>())) n = std::clamp(v->get<double>(), p.lo, p.hi);
                    e.values[key] = static_cast<float>(n);
                    break;
                }
            }
        }
        e.fg = colour_of(core::get(item, "foreground"), fg);
        e.bg = colour_of(core::get(item, "background"), bg);
        out.push_back(std::move(e));
    }
    return out;
}

// --- lengths and coordinates on the layer's grid -------------------------------------------------------------------------

// The working 200 dpi on a layer's grid (Ctx::s layer pixels a working pixel).
struct Scale {
    float s = 1;
    int k = 1;  // (the gradient's step: a working pixel, in whole layer pixels)
    explicit Scale(double scale) : s(static_cast<float>(scale > 0 ? scale : 1.0)), k(std::max(1, static_cast<int>(std::lround(s)))) {}
    // A radius of whole working pixels (a box, a stroke, a step count) on the grid.
    int r(int working) const { return working <= 0 ? 0 : static_cast<int>(std::lround(static_cast<float>(working) * s)); }
    // A length (a sigma, a shift) on the grid.
    float px(float working) const { return working * s; }
    // A pixel's working coordinate (PhotoCraft's `x as f32`), that of its centre (`x as f32 + 0.5`) and the working pixel
    // holding its centre (`x`).
    float at(int x) const { return (static_cast<float>(x) + 0.5f) / s - 0.5f; }
    float centre(int x) const { return (static_cast<float>(x) + 0.5f) / s; }
    int cell(int x) const { return static_cast<int>(std::floor((static_cast<float>(x) + 0.5f) / s)); }
};

// --- PhotoCraft's helpers (artistic_fx.rs, fxutil.rs, noise.rs) ----------------------------------------------------------

float hash01(int x, int y, std::uint32_t c, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343u ^ static_cast<std::uint32_t>(y) * 0xd8163841u ^ c * 0xcb1ab31fu ^ seed * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / static_cast<float>(1u << 24);
}

float rem_euclid(float a, float b) {
    const float r = std::fmod(a, b);
    return r < 0.0f ? r + std::fabs(b) : r;
}

int rem_euclid(int a, int b) {
    const int r = a % b;
    return r < 0 ? r + std::abs(b) : r;
}

// PhotoCraft's Hermite step (artistic_fx.rs): from e0 to e1; e0 > e1 the falling step.
float smoothstep(float e0, float e1, float x) {
    if (std::fabs(e1 - e0) < 1e-9f) return x >= e1 ? 1.0f : 0.0f;
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float lum(const Px& p) { return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }
Px mix(const Px& a, const Px& b, float t) { return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t}; }
template <typename Fn>
Px map(const Px& p, Fn&& f) {
    return {f(p[0]), f(p[1]), f(p[2])};
}
Px grey(float v) { return {v, v, v}; }
float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float posterize(float v, float levels) {
    const float l = std::max(levels - 1.0f, 1.0f);
    return std::round(clamp01(v) * l) / l;
}
Px saturate(const Px& p, float k) {
    const float l = lum(p);
    return map(p, [l, k](float v) { return l + (v - l) * k; });
}
std::array<float, 3> normalize3(const std::array<float, 3>& v) {
    const float l = std::max(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), 1e-9f);
    return {v[0] / l, v[1] / l, v[2] / l};
}

// (the box passes in whole numbers: render/fx.hpp's gauss_box_radii, whole_box_line and whole_box_passes, D2's too)

// How far a Gaussian of `sigma` (the grid's pixels) reads (artistic_fx.rs gs).
int gauss_reach(float sigma) {
    if (sigma < 0.3f) return 0;
    const std::array<int, 3> radii = gauss_box_radii(sigma);
    return radii[0] + radii[1] + radii[2] + 1;
}

// --- the blurs in whole numbers -------------------------------------------------------------------------------------------

// Samples as whole numbers of 2^-30 (a sample's magnitude kept to 1024: the running sums stay far inside 2^63).
constexpr double kUnit = 1073741824.0;
using Whole = std::vector<std::int64_t>;

std::int64_t to_whole(float v) {
    if (!std::isfinite(v)) v = 0.0f;
    return std::llround(static_cast<double>(std::clamp(v, -1024.0f, 1024.0f)) * kUnit);
}
float from_whole(std::int64_t q) { return static_cast<float>(static_cast<double>(q) / kUnit); }

Whole whole_of(const float* v, std::size_t count) {
    Whole out(count);
    for (std::size_t i = 0; i < count; ++i) out[i] = to_whole(v[i]);
    return out;
}
void back(const Whole& q, float* v) {
    for (std::size_t i = 0; i < q.size(); ++i) v[i] = from_whole(q[i]);
}

// Box passes of the radii along the rows, then along the columns, of a w × h × n buffer (whole numbers of 2^-30, a sample
// kept to ±1024).
void box_passes(std::vector<float>& buf, std::size_t w, std::size_t h, std::size_t n, const std::vector<int>& radii) {
    whole_box_passes(buf, w, h, n, radii, 1.0 / kUnit, 1024.0);
}

void box_blur_n(std::vector<float>& buf, std::size_t w, std::size_t h, std::size_t n, int r) { box_passes(buf, w, h, n, {r}); }

// The almost-Gaussian of three box passes each way (fxutil.rs gauss_blur_n); nothing below a sigma of 0.3.
void gauss_blur_n(std::vector<float>& buf, std::size_t w, std::size_t h, std::size_t n, float sigma) {
    if (sigma < 0.3f) return;
    const std::array<int, 3> radii = gauss_box_radii(sigma);
    box_passes(buf, w, h, n, {radii[0], radii[1], radii[2]});
}

// A running mean of radius r steps along the direction d ((1, 0), (0, 1), (1, 1) or (1, -1)) of a w × h × n buffer
// (artistic_fx.rs line_blur).
void line_blur(std::vector<float>& buf, std::size_t n, std::size_t w, std::size_t h, std::pair<int, int> d, int r) {
    if (r <= 0 || w == 0 || h == 0) return;
    const int wi = static_cast<int>(w), hi = static_cast<int>(h);
    const auto inside = [wi, hi](int x, int y) { return x >= 0 && y >= 0 && x < wi && y < hi; };
    Whole q = whole_of(buf.data(), buf.size()), line, gathered;
    std::vector<std::size_t> idx;
    for (int sy = 0; sy < hi; ++sy)
        for (int sx = 0; sx < wi; ++sx) {
            if (inside(sx - d.first, sy - d.second)) continue;  // (not the start of a line)
            idx.clear();
            for (int x = sx, y = sy; inside(x, y); x += d.first, y += d.second) idx.push_back((std::size_t(y) * w + std::size_t(x)) * n);
            gathered.resize(idx.size() * n);
            for (std::size_t k = 0; k < idx.size(); ++k) std::copy(q.begin() + std::ptrdiff_t(idx[k]), q.begin() + std::ptrdiff_t(idx[k] + n), gathered.begin() + std::ptrdiff_t(k * n));
            whole_box_line(gathered.data(), idx.size(), n, n, r, line);
            for (std::size_t k = 0; k < idx.size(); ++k) std::copy(gathered.begin() + std::ptrdiff_t(k * n), gathered.begin() + std::ptrdiff_t((k + 1) * n), q.begin() + std::ptrdiff_t(idx[k]));
        }
    back(q, buf.data());
}

// --- the working window --------------------------------------------------------------------------------------------------

// The colours of every pixel of the window (artistic_fx.rs Win), the layer's grid coordinates.
struct Win {
    Rect r;
    std::size_t w = 0, h = 0;
    std::vector<Px> c;

    std::pair<int, int> xy(std::size_t i) const { return {r.x0 + static_cast<int>(i % w), r.y0 + static_cast<int>(i / w)}; }
    std::vector<float> lum() const {
        std::vector<float> out(c.size());
        for (std::size_t i = 0; i < c.size(); ++i) out[i] = fx::lum(c[i]);
        return out;
    }
    // The bilinear sample at (x, y) (pixel centres at .5), clamped to the window.
    Px sample(float x, float y) const {
        const float fx = std::clamp(x - 0.5f - static_cast<float>(r.x0), 0.0f, static_cast<float>(w - 1));
        const float fy = std::clamp(y - 0.5f - static_cast<float>(r.y0), 0.0f, static_cast<float>(h - 1));
        const std::size_t x0 = static_cast<std::size_t>(std::floor(fx)), y0 = static_cast<std::size_t>(std::floor(fy));
        const std::size_t x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
        const float ax = fx - static_cast<float>(x0), ay = fy - static_cast<float>(y0);
        const auto g = [this](std::size_t xx, std::size_t yy) { return c[yy * w + xx]; };
        return mix(mix(g(x0, y0), g(x1, y0), ax), mix(g(x0, y1), g(x1, y1), ax), ay);
    }
    // The pixel nearest to (x, y), clamped to the window.
    Px at(int x, int y) const {
        const std::size_t xx = static_cast<std::size_t>(std::clamp(x - r.x0, 0, static_cast<int>(w) - 1));
        const std::size_t yy = static_cast<std::size_t>(std::clamp(y - r.y0, 0, static_cast<int>(h) - 1));
        return c[yy * w + xx];
    }
};

void blur1(std::vector<float>& p, std::size_t w, std::size_t h, float sigma) { gauss_blur_n(p, w, h, 1, sigma); }

void blur3(std::vector<Px>& c, std::size_t w, std::size_t h, float sigma) {
    if (sigma < 0.3f) return;
    std::vector<float> flat(c.size() * 3);
    for (std::size_t i = 0; i < c.size(); ++i) std::copy(c[i].begin(), c[i].end(), flat.begin() + std::ptrdiff_t(i * 3));
    gauss_blur_n(flat, w, h, 3, sigma);
    for (std::size_t i = 0; i < c.size(); ++i) c[i] = {flat[i * 3], flat[i * 3 + 1], flat[i * 3 + 2]};
}

void line_blur3(std::vector<Px>& c, std::size_t w, std::size_t h, std::pair<int, int> d, int r) {
    if (r <= 0) return;
    std::vector<float> flat(c.size() * 3);
    for (std::size_t i = 0; i < c.size(); ++i) std::copy(c[i].begin(), c[i].end(), flat.begin() + std::ptrdiff_t(i * 3));
    line_blur(flat, 3, w, h, d, r);
    for (std::size_t i = 0; i < c.size(); ++i) c[i] = {flat[i * 3], flat[i * 3 + 1], flat[i * 3 + 2]};
}

// The Sobel gradient of a plane (its edges repeated), its stencil `k` pixels a step: the gradient a working pixel.
std::pair<std::vector<float>, std::vector<float>> sobel(const std::vector<float>& p, std::size_t w, std::size_t h, int k) {
    const int wi = static_cast<int>(w), hi = static_cast<int>(h);
    const auto at = [&](int x, int y) { return p[std::size_t(std::clamp(y, 0, hi - 1)) * w + std::size_t(std::clamp(x, 0, wi - 1))]; };
    std::vector<float> gx(w * h, 0.0f), gy(w * h, 0.0f);
    for (int y = 0; y < hi; ++y)
        for (int x = 0; x < wi; ++x) {
            const std::size_t i = std::size_t(y) * w + std::size_t(x);
            gx[i] = ((at(x + k, y - k) + 2.0f * at(x + k, y) + at(x + k, y + k)) - (at(x - k, y - k) + 2.0f * at(x - k, y) + at(x - k, y + k))) / 8.0f;
            gy[i] = ((at(x - k, y + k) + 2.0f * at(x, y + k) + at(x + k, y + k)) - (at(x - k, y - k) + 2.0f * at(x, y - k) + at(x + k, y - k))) / 8.0f;
        }
    return {std::move(gx), std::move(gy)};
}

std::vector<float> grad_mag(const std::vector<float>& p, std::size_t w, std::size_t h, int k) {
    const auto [gx, gy] = sobel(p, w, h, k);
    std::vector<float> out(gx.size());
    for (std::size_t i = 0; i < gx.size(); ++i) out[i] = std::sqrt(gx[i] * gx[i] + gy[i] * gy[i]);
    return out;
}

std::pair<int, int> direction(std::string_view name) {
    if (name == "horizontal") return {1, 0};
    if (name == "vertical") return {0, 1};
    if (name == "leftDiagonal") return {1, 1};
    return {1, -1};  // (rightDiagonal: bottom left to top right)
}

// Kuwahara's smoothing (the flattest of four quadrants' means): the painterly base of several of the Artistic effects;
// its quadrants' half side `r` working pixels.
int kuwahara_half(int r, const Scale& sc) { return std::max(1, sc.r(std::max(1, (r + 1) / 2))); }
int kuwahara_reach(int r, const Scale& sc) { return 2 * kuwahara_half(r, sc) + 1; }

void kuwahara(Win& win, int r, const Scale& sc) {
    const int h2 = kuwahara_half(r, sc);
    const std::size_t w = win.w, h = win.h;
    std::vector<float> m;
    m.reserve(w * h * 5);
    for (const Px& p : win.c) {
        const float l = lum(p);
        m.insert(m.end(), {p[0], p[1], p[2], l, l * l});
    }
    box_blur_n(m, w, h, 5, h2);
    const int wi = static_cast<int>(w), hi = static_cast<int>(h);
    const auto get = [&](int x, int y) { return m.data() + (std::size_t(std::clamp(y, 0, hi - 1)) * w + std::size_t(std::clamp(x, 0, wi - 1))) * 5; };
    const int o = h2;
    for (int y = 0; y < hi; ++y)
        for (int x = 0; x < wi; ++x) {
            float best = std::numeric_limits<float>::max();
            Px col{0, 0, 0};
            for (const auto& [dx, dy] : {std::pair{-o, -o}, std::pair{o, -o}, std::pair{-o, o}, std::pair{o, o}}) {
                const float* q = get(x + dx, y + dy);
                const float var = q[4] - q[3] * q[3];
                if (var < best) {
                    best = var;
                    col = {q[0], q[1], q[2]};
                }
            }
            win.c[std::size_t(y) * w + std::size_t(x)] = col;
        }
}

void unsharp(Win& win, float sigma, float amount) {
    if (amount <= 0.0f) return;
    std::vector<Px> b = win.c;
    blur3(b, win.w, win.h, sigma);
    for (std::size_t i = 0; i < win.c.size(); ++i) {
        Px& p = win.c[i];
        const Px& q = b[i];
        p = {p[0] + (p[0] - q[0]) * amount, p[1] + (p[1] - q[1]) * amount, p[2] + (p[2] - q[2]) * amount};
    }
}

// Smooth value noise (0..1) on a unit lattice.
float vnoise(float x, float y, std::uint32_t seed) {
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const float tx = x - static_cast<float>(ix), ty = y - static_cast<float>(iy);
    const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
    const auto hh = [seed](int a, int b) { return hash01(a, b, 0, seed); };
    const float top = hh(ix, iy) + (hh(ix + 1, iy) - hh(ix, iy)) * sx;
    const float bot = hh(ix, iy + 1) + (hh(ix + 1, iy + 1) - hh(ix, iy + 1)) * sx;
    return top + (bot - top) * sy;
}

// Three octaves of value noise (0..1).
float fbm(float x, float y, std::uint32_t seed) {
    return (vnoise(x, y, seed) * 0.57f + vnoise(x * 2.03f, y * 2.03f, seed ^ 0x51u) * 0.29f + vnoise(x * 4.1f, y * 4.1f, seed ^ 0xa3u) * 0.14f) / 1.0f;
}

// Strokes: value noise stretched `len` working pixels along the direction d and `wid` across (x, y working coordinates).
float strokes(float x, float y, std::pair<int, int> d, float len, float wid, std::uint32_t seed) {
    const float dx = static_cast<float>(d.first), dy = static_cast<float>(d.second);
    const float l = std::max(std::sqrt(dx * dx + dy * dy), 1e-6f);
    const float ux = dx / l, uy = dy / l;
    const float u = x * ux + y * uy;
    const float v = -x * uy + y * ux;
    return vnoise(u / std::max(len, 0.5f), v / std::max(wid, 0.3f), seed);
}

// The light's direction (y down).
std::pair<float, float> light_dir(std::string_view name) {
    const float s = std::numbers::sqrt2_v<float> / 2.0f;
    if (name == "bottom") return {0.0f, 1.0f};
    if (name == "bottomLeft") return {-s, s};
    if (name == "left") return {-1.0f, 0.0f};
    if (name == "topLeft") return {-s, -s};
    if (name == "topRight") return {s, -s};
    if (name == "right") return {1.0f, 0.0f};
    if (name == "bottomRight") return {s, s};
    return {0.0f, -1.0f};
}

// Lambert's shading (1: flat) of a height field of gradient (gx, gy).
float shade(float gx, float gy, std::pair<float, float> light, float k) {
    const float nx = -k * gx, ny = -k * gy, nz = 1.0f;
    const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
    const float lx = light.first, ly = light.second, lz = 1.0f;
    const float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    const float d = (nx * lx + ny * ly + nz * lz) / (nl * ll);
    return d / (lz / ll);
}

// A basket weave of threads `p` wide with `noise` irregularity.
float weave(float x, float y, float p, float noise) {
    const int cx = static_cast<int>(std::floor(x / p)), cy = static_cast<int>(std::floor(y / p));
    const bool over_h = rem_euclid(cx + cy, 2) == 0;
    const float u = rem_euclid(x, p) / p, v = rem_euclid(y, p) / p;
    const float ridge = over_h ? std::sin(v * std::numbers::pi_v<float>) : std::sin(u * std::numbers::pi_v<float>);
    const float n = vnoise(x / (p * 0.7f), y / (p * 2.0f), 97);
    return ridge * (1.0f - noise) + n * noise;
}

// The textures (heights 0..1) at working coordinates; `s` their scale (1: 100 %).
float texture(std::string_view kind, float x, float y, float s) {
    x /= s;
    y /= s;
    if (kind == "brick") {
        const float bw = 24.0f, bh = 12.0f;
        const float row = std::floor(y / bh);
        const float xo = x + (static_cast<int>(row) % 2 == 0 ? 0.0f : bw / 2.0f);
        const float u = rem_euclid(xo, bw), v = rem_euclid(y, bh);
        const float d = std::min(std::min(u, bw - u), std::min(v, bh - v));
        return smoothstep(0.6f, 2.2f, d) * (0.85f + 0.15f * vnoise(x / 3.0f, y / 3.0f, 91));
    }
    if (kind == "burlap") return weave(x, y, 7.0f, 0.35f);
    if (kind == "canvas") return weave(x, y, 3.5f, 0.18f);
    if (kind == "sandstone") return fbm(x / 7.0f, y / 7.0f, 93) * 0.7f + hash01(static_cast<int>(x), static_cast<int>(y), 0, 94) * 0.3f;
    if (kind == "blocks") {
        const float b = 18.0f;
        const float u = rem_euclid(x, b) / b, v = rem_euclid(y, b) / b;
        return std::min(std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v)) * 4.0f, 1.0f);
    }
    if (kind == "frosted") return fbm(x / 2.5f, y / 2.5f, 95);
    if (kind == "tinyLens") {
        const float p = 12.0f;
        const float u = (rem_euclid(x, p) / p - 0.5f) * 2.0f, v = (rem_euclid(y, p) / p - 0.5f) * 2.0f;
        return std::sqrt(std::max(1.0f - (u * u + v * v), 0.0f));
    }
    return 0.5f;
}

// テクスチャライザー's core: the window shaded as a relief of a texture.
void texturize(Win& win, std::string_view kind, float scaling, float relief, std::string_view light, bool invert, const Scale& sc) {
    if (relief <= 0.0f) return;
    const float s = std::clamp(scaling / 100.0f, 0.5f, 2.0f);
    const auto l = light_dir(light);
    const float k = relief / 50.0f * 3.0f;
    const float sign = invert ? -1.0f : 1.0f;
    for (std::size_t i = 0; i < win.c.size(); ++i) {
        const auto [x, y] = win.xy(i);
        const float fx = sc.at(x), fy = sc.at(y);
        const float gx = (texture(kind, fx + 1.0f, fy, s) - texture(kind, fx - 1.0f, fy, s)) * 0.5f * sign;
        const float gy = (texture(kind, fx, fy + 1.0f, s) - texture(kind, fx, fy - 1.0f, s)) * 0.5f * sign;
        const float f = std::clamp(shade(gx, gy, l, k), 0.0f, 2.0f);
        win.c[i] = map(win.c[i], [f](float v) { return v * f; });
    }
}

// A cell's jittered point (fxutil.rs cell_point: cells of `cell` from the origin).
std::pair<float, float> cell_point(int cx, int cy, float cell, std::uint32_t seed) {
    return {(static_cast<float>(cx) + 0.1f + 0.8f * hash01(cx, cy, 11, seed)) * cell, (static_cast<float>(cy) + 0.1f + 0.8f * hash01(cx, cy, 12, seed)) * cell};
}

// The nearest and the second nearest cells' points (their distances, the nearest point).
struct Voronoi {
    float d1, d2;
    std::pair<float, float> p1;
};
Voronoi voronoi(float x, float y, float cell, std::uint32_t seed) {
    const int cx = static_cast<int>(std::floor(x / cell)), cy = static_cast<int>(std::floor(y / cell));
    Voronoi out{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), {x, y}};
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            const auto p = cell_point(cx + i, cy + j, cell, seed);
            const float d = std::sqrt((p.first - x) * (p.first - x) + (p.second - y) * (p.second - y));
            if (d < out.d1) {
                out.d2 = out.d1;
                out.d1 = d;
                out.p1 = p;
            } else if (d < out.d2) {
                out.d2 = d;
            }
        }
    return out;
}

// The highlights brightened (粒状フィルム・こする's ハイライト範囲 and 強さ).
Px highlights(const Px& p, float l, float area, float intensity) {
    if (area <= 0.0f) return p;
    const float m = smoothstep(1.0f - area / 20.0f * 0.75f, 1.02f, l) * (1.0f - intensity / 10.0f * 0.6f);
    return map(p, [m](float v) { return v + (1.0f - v) * m; });
}

// --- what shows, past its edges ----------------------------------------------------------------------------------------

// The colours of the clear pixels (alpha 0) the effects read round what shows: each the mean of the colours that show (by
// their alpha) in the smallest square round it of radius 1, 2, 4 … `radius` that holds any — one that holds none within
// `radius`, black (it reaches nothing that shows). What shows is then worked as if it went on past its edges, never with
// the colours a clear pixel keeps unseen (black, or what was erased): a ぼかし, a Kuwahara, an edge never brings them in.
void fill_clear(Win& win, const std::vector<float>& alpha, int radius) {
    if (radius <= 0) return;
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < win.c.size(); ++i)
        if (!(alpha[i] > 0.0f)) open.push_back(i);
    if (open.empty()) return;
    std::vector<float> shown(win.c.size() * 4);
    for (std::size_t i = 0; i < win.c.size(); ++i) {
        const float a = alpha[i] > 0.0f ? std::min(alpha[i], 1.0f) : 0.0f;
        for (std::size_t c = 0; c < 3; ++c) shown[i * 4 + c] = win.c[i][c] * a;
        shown[i * 4 + 3] = a;
    }
    for (int r = std::min(1, radius);; r = std::min(radius, r * 2)) {
        std::vector<float> mean = shown;
        box_blur_n(mean, win.w, win.h, 4, r);
        std::vector<std::size_t> still;
        for (const std::size_t i : open) {
            const float a = mean[i * 4 + 3];
            if (a > 0.0f) {
                win.c[i] = {mean[i * 4] / a, mean[i * 4 + 1] / a, mean[i * 4 + 2] / a};
            } else {
                still.push_back(i);
            }
        }
        open = std::move(still);
        if (open.empty() || r >= radius) break;
    }
    for (const std::size_t i : open) win.c[i] = {0.0f, 0.0f, 0.0f};
}

// --- each effect's reach and settings ---------------------------------------------------------------------------------

float cutout_sigma(const Effect& e) { return 0.5f + e.get("edge_simplicity") * (1.1f - 0.3f * e.get("edge_fidelity")); }
int daubs_radius(const Effect& e) {
    const std::string_view type = e.choice("brush_type");
    const bool wide = type == "wideSharp" || type == "wideBlurry";
    return static_cast<int>(std::max(e.get("brush_size") / 2.0f * (wide ? 1.5f : 1.0f), 1.0f));
}
float plastic_sigma(const Effect& e) { return 0.5f + e.get("smoothness") * 0.5f + (15.0f - e.get("detail")) * 0.2f; }
int watercolor_radius(const Effect& e) { return 1 + static_cast<int>((15.0f - e.get("brush_detail")) / 2.5f); }
float basrelief_sigma(const Effect& e) { return 0.3f + e.get("smoothness") * 0.5f + (15.0f - e.get("detail")) * 0.12f; }
float patch_size(const Effect& e) { return 3.0f + e.get("square_size") * 1.5f; }
float glass_cell(const Effect& e) { return e.get("cell_size") * 1.6f + 1.0f; }
int truncated(float v) { return static_cast<int>(std::max(0.0f, v)); }  // (PhotoCraft's `as usize`)

// How far an effect reads round a pixel, on the layer's grid (artistic_fx.rs effect_reach, counted from what each reads).
int effect_reach(const Effect& e, const Scale& sc) {
    const auto g = [&e](std::string_view k) { return e.get(k); };
    const auto gs = [&sc](float sigma) { return gauss_reach(sc.px(sigma)); };
    const auto cells = [&sc](float working) { return static_cast<int>(std::ceil(sc.px(working))) + 1; };
    const int k = sc.k;
    switch (e.filter) {
        case F::ColoredPencil: return k + 1;
        case F::Cutout: return gs(cutout_sigma(e));
        case F::DryBrush:
        case F::Fresco: return kuwahara_reach(1 + truncated(g("brush_size")), sc) + gs(1.0f) + k + 1;
        case F::FilmGrain:
        case F::DiffuseGlow:
        case F::ChalkCharcoal:
        case F::Reticulation:
        case F::Grain:
        case F::MosaicTiles:
        case F::Texturizer:
        case F::ConteCrayon:
        case F::GraphicPen:
        case F::Craquelure: return 0;
        case F::RoughPastels: return sc.r(truncated(g("stroke_length") / 2.0f)) + 1;
        case F::NeonGlow: return gs(std::fabs(g("glow_size")) * 0.6f + 0.5f);
        case F::PaintDaubs: return kuwahara_reach(daubs_radius(e), sc) + gs(1.5f) + k + 1;
        case F::PaletteKnife: return kuwahara_reach(truncated(std::max(g("stroke_size") / 3.0f, 1.0f)), sc) + gs(g("softness") * 0.5f);
        case F::PlasticWrap: return gs(plastic_sigma(e)) + k + 1;
        case F::PosterEdges: return gs(0.6f + g("edge_thickness") * 0.6f) + k + 1;
        case F::SmudgeStick: return sc.r(truncated(g("stroke_length") * 1.5f)) + 1;
        case F::Sponge: return kuwahara_reach(1 + truncated(g("brush_size")), sc);
        case F::Underpainting: return gs(0.5f + g("brush_size") * 0.4f);
        case F::Watercolor: return kuwahara_reach(watercolor_radius(e), sc) + k + 1;
        case F::AccentedEdges: return gs(0.4f + g("smoothness") * 0.3f) + k + gs(g("edge_width") * 0.5f) + 1;
        case F::AngledStrokes:
        case F::Crosshatch: return sc.r(truncated(g("stroke_length") / 3.0f)) + gs(1.0f) + 1;
        case F::DarkStrokes: return sc.r(8) + 1;
        case F::InkOutlines: return std::max(sc.r(truncated(g("stroke_length") / 2.0f)), k) + 1;
        case F::Spatter: return cells(g("spray_radius"));
        case F::SprayedStrokes: return cells(g("spray_radius")) + sc.r(truncated(g("stroke_length") / 2.0f)) + 1;
        case F::SumiE: return gs(g("stroke_width") * 0.2f) + sc.r(truncated(g("stroke_width") / 2.0f)) + 1;
        case F::Glass: return cells(g("distortion") * 2.0f);
        case F::OceanRipple: return cells(g("ripple_magnitude") * 0.9f);
        case F::BasRelief: return gs(basrelief_sigma(e)) + k + 1;
        case F::Charcoal: return gs(1.0f) + k + 1;
        case F::Chrome: return gs(1.0f + g("smoothness") * 0.4f) + k + 1;
        case F::HalftonePattern: return gs(0.5f + g("size") * 0.3f);
        case F::NotePaper: return gs(1.0f) + gs(1.2f) + k + 1;
        case F::Photocopy: return gs(1.0f + g("detail") * 0.5f);
        case F::Plaster: return gs(0.5f + g("smoothness") * 0.8f) + gs(1.5f) + k + 1;
        case F::Stamp: return gs(0.3f + g("smoothness") * 0.3f);
        case F::TornEdges: return gs(1.0f);
        case F::WaterPaper: return sc.r(truncated(g("fiber_length") / 6.0f)) + 1;
        case F::GlowingEdges: return gs(0.3f + g("smoothness") * 0.25f) + k + gs(g("edge_width") * 0.4f) + 1;
        case F::Patchwork: return cells(patch_size(e)) + 1;
        case F::StainedGlass: return cells(glass_cell(e) * 2.5f) + 1;
    }
    return 0;
}

// --- each effect's pixel work (artistic_fx.rs apply) ----------------------------------------------------------------------

void apply_effect(const Effect& e, Win& win, const Scale& sc, const Ctx& ctx) {
    const auto g = [&e](std::string_view k) { return e.get(k); };
    const std::size_t w = win.w, h = win.h, n = win.c.size();
    const Px fg = e.fg, bg = e.bg;
    const int k = sc.k;
    switch (e.filter) {
        // ---------------- アーティスティック ----------------
        case F::ColoredPencil: {
            const float pw = g("pencil_width");
            const float pressure = g("stroke_pressure") / 15.0f;
            const Px paper = map(bg, [&](float v) { return v * (0.7f + 0.3f * g("paper_brightness") / 50.0f); });
            const std::vector<float> l = win.lum();
            const std::vector<float> e2 = grad_mag(l, w, h, k);
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float wx = sc.at(x), wy = sc.at(y);
                const float d = 1.0f - l[i];
                const float h1 = strokes(wx, wy, {1, 1}, 5.0f + pw * 3.0f, 0.5f + pw * 0.45f, 11);
                const float h2 = strokes(wx, wy, {1, -1}, 5.0f + pw * 3.0f, 0.5f + pw * 0.45f, 12);
                const float hatch = std::max(h1, h2 * smoothstep(0.3f, 0.8f, d));
                const float cov = clamp01(d * (0.55f + pressure) + e2[i] * 3.0f - (1.0f - hatch) * 0.8f + 0.25f);
                win.c[i] = mix(paper, map(win.c[i], [](float v) { return v * 0.88f; }), cov);
            }
            break;
        }
        case F::Cutout: {
            blur3(win.c, w, h, sc.px(cutout_sigma(e)));
            const float levels = std::round(g("number_of_levels"));
            for (Px& p : win.c) {  // (flat tones by the lightness's levels, each region's hue kept)
                const float l = std::max(lum(p), 1e-4f);
                const float kk = posterize(l, levels) / l;
                p = map(p, [&](float v) { return posterize(clamp01(v * kk), levels * 2.0f); });
            }
            break;
        }
        case F::DryBrush:
        case F::Fresco: {
            const bool fresco = e.filter == F::Fresco;
            kuwahara(win, 1 + truncated(g("brush_size")), sc);
            unsharp(win, sc.px(1.0f), fresco ? 0.9f : 0.5f);
            const float levels = 3.0f + g("brush_detail");
            const float amp = 0.02f + (g("texture") - 1.0f) * 0.05f;
            const std::vector<float> l = fresco ? grad_mag(win.lum(), w, h, k) : std::vector<float>();
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float nn = (hash01(x, y, 0, 21) - 0.5f) * amp;  // (the brush's grain: the layer's pixels)
                Px p = map(win.c[i], [&](float v) { return posterize(v, levels) * 0.6f + v * 0.4f + nn; });
                if (fresco) p = map(p, [&](float v) { return std::pow(clamp01((v - 0.5f) * 1.3f + 0.45f), 1.15f) * (1.0f - clamp01(l[i] * 2.5f) * 0.6f); });
                win.c[i] = p;
            }
            break;
        }
        case F::FilmGrain: {
            const float grain = g("grain") / 20.0f;
            const float area = g("highlight_area"), inten = g("intensity");
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float l = lum(win.c[i]);
                const float nn = (hash01(x, y, 0, 31) - 0.5f) * grain * 0.5f * (0.3f + 2.8f * l * (1.0f - l));
                win.c[i] = highlights(map(win.c[i], [nn](float v) { return v + nn; }), l, area, inten);
            }
            break;
        }
        case F::NeonGlow: {
            const float size = g("glow_size");
            const float br = g("glow_brightness") / 50.0f;
            const Px glow = e.glow;
            const std::vector<float> l = win.lum();
            std::vector<float> m = l;
            if (size < 0.0f)
                for (float& v : m) v = 1.0f - v;
            blur1(m, w, h, sc.px(std::fabs(size) * 0.6f + 0.5f));
            for (std::size_t i = 0; i < n; ++i) {
                const Px base = mix(fg, bg, l[i]);
                const float kk = clamp01(m[i] * 1.2f) * br * 2.0f;
                for (std::size_t c = 0; c < 3; ++c) win.c[i][c] = 1.0f - (1.0f - base[c]) * (1.0f - clamp01(glow[c] * kk));
            }
            break;
        }
        case F::PaintDaubs: {
            const std::string_view type = e.choice("brush_type");
            kuwahara(win, daubs_radius(e), sc);
            const float sharp = g("sharpness") / 40.0f;
            if (type == "wideBlurry") {
                blur3(win.c, w, h, sc.px(1.5f));
            } else if (type == "wideSharp") {
                unsharp(win, sc.px(1.5f), sharp * 4.0f);
            } else {
                unsharp(win, sc.px(1.2f), sharp * 2.5f);
            }
            if (type == "lightRough" || type == "darkRough" || type == "sparkle") {
                const std::vector<float> edges = type == "sparkle" ? grad_mag(win.lum(), w, h, k) : std::vector<float>();
                for (std::size_t i = 0; i < n; ++i) {
                    const auto [x, y] = win.xy(i);
                    const float nn = fbm(sc.at(x) / 2.0f, sc.at(y) / 2.0f, 41) - 0.5f;
                    if (type == "lightRough") {
                        win.c[i] = map(win.c[i], [nn](float v) { return v + std::max(nn, 0.0f) * 0.35f; });
                    } else if (type == "darkRough") {
                        win.c[i] = map(win.c[i], [nn](float v) { return v * (1.0f - std::max(nn, 0.0f) * 0.7f); });
                    } else {  // (sparkles: the layer's pixels)
                        const float spark = clamp01(edges[i] * 4.0f) * smoothstep(0.75f, 0.95f, hash01(x, y, 0, 42)) * 0.8f;
                        win.c[i] = map(win.c[i], [spark](float v) { return v + spark; });
                    }
                }
            }
            break;
        }
        case F::PaletteKnife: {
            kuwahara(win, truncated(std::max(g("stroke_size") / 3.0f, 1.0f)), sc);
            const float levels = 2.0f + g("stroke_detail") * 3.0f;
            for (Px& p : win.c) p = map(p, [levels](float v) { return posterize(v, levels) * 0.5f + v * 0.5f; });
            blur3(win.c, w, h, sc.px(g("softness") * 0.5f));
            break;
        }
        case F::PlasticWrap: {
            const float hs = g("highlight_strength") / 20.0f;
            std::vector<float> l = win.lum();
            blur1(l, w, h, sc.px(plastic_sigma(e)));
            const auto [gx, gy] = sobel(l, w, h, k);
            const auto lv = normalize3({-0.45f, -0.65f, 1.0f});
            const auto hv = normalize3({lv[0], lv[1], lv[2] + 1.0f});
            for (std::size_t i = 0; i < n; ++i) {
                const float kk = 14.0f;
                const auto nv = normalize3({-kk * gx[i], -kk * gy[i], 1.0f});
                const float ndh = std::max(nv[0] * hv[0] + nv[1] * hv[1] + nv[2] * hv[2], 0.0f);
                const float tilt = 1.0f - nv[2];
                const float spec = std::pow(ndh, 30.0f) * smoothstep(0.01f, 0.12f, tilt) + smoothstep(0.25f, 0.6f, tilt) * 0.25f;
                win.c[i] = map(win.c[i], [&](float v) { return v * 0.88f + spec * hs * 1.3f; });
            }
            break;
        }
        case F::PosterEdges: {
            std::vector<float> l = win.lum();
            blur1(l, w, h, sc.px(0.6f + g("edge_thickness") * 0.6f));
            const std::vector<float> edge = grad_mag(l, w, h, k);
            const float inten = g("edge_intensity") / 10.0f;
            const float levels = g("posterization") + 2.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const float ink = smoothstep(0.015f, 0.06f, edge[i] * (1.0f + inten * 3.0f)) * (0.55f + inten * 0.45f);
                win.c[i] = map(win.c[i], [&](float v) { return posterize(v, levels) * (1.0f - ink); });
            }
            break;
        }
        case F::RoughPastels: {
            const float len = g("stroke_length");
            const float det = g("stroke_detail") / 20.0f;
            const std::vector<Px> orig = win.c;
            line_blur3(win.c, w, h, {1, -1}, sc.r(truncated(len / 2.0f)));
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float nn = strokes(sc.at(x), sc.at(y), {1, -1}, 3.0f + len * 0.8f, 1.1f, 51) - 0.5f;
                const Px p = mix(win.c[i], orig[i], det * 0.5f);
                win.c[i] = map(p, [nn](float v) { return v + nn * 0.3f; });
            }
            texturize(win, e.choice("texture"), g("scaling"), g("relief"), e.choice("light"), e.flag("invert"), sc);
            break;
        }
        case F::SmudgeStick: {
            line_blur3(win.c, w, h, {1, 1}, sc.r(truncated(g("stroke_length") * 1.5f)));
            const float area = g("highlight_area"), inten = g("intensity");
            for (Px& p : win.c) {
                const float l = lum(p);
                const Px q = map(p, [l](float v) { return v * (0.7f + 0.3f * l); });
                p = highlights(q, l, area, inten);
            }
            break;
        }
        case F::Sponge: {
            kuwahara(win, 1 + truncated(g("brush_size")), sc);
            const float def = g("definition") / 25.0f;
            const float scale = 1.5f + g("smoothness") * 0.5f;
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float s = fbm(sc.at(x) / scale, sc.at(y) / scale, 61);
                const float hole = smoothstep(0.58f, 0.75f, s);
                win.c[i] = map(win.c[i], [&](float v) { return clamp01((v - 0.5f) * (1.0f + def * 0.6f) + 0.5f) * (1.0f - hole * def * 0.4f); });
            }
            break;
        }
        case F::Underpainting: {
            const std::vector<Px> orig = win.c;
            blur3(win.c, w, h, sc.px(0.5f + g("brush_size") * 0.4f));
            for (std::size_t i = 0; i < n; ++i) win.c[i] = saturate(mix(orig[i], win.c[i], 0.8f), 1.15f);
            const float cov = g("texture_coverage") / 40.0f;
            texturize(win, e.choice("texture"), g("scaling"), g("relief") * (0.5f + cov * 1.5f), e.choice("light"), e.flag("invert"), sc);
            break;
        }
        case F::Watercolor: {
            kuwahara(win, watercolor_radius(e), sc);
            const float sh = g("shadow_intensity") / 10.0f;
            const std::vector<float> edge = grad_mag(win.lum(), w, h, k);
            const float amp = (g("texture") - 1.0f) * 0.04f;
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float nn = (fbm(sc.at(x) / 3.0f, sc.at(y) / 3.0f, 71) - 0.5f) * amp;
                const float kk = 1.0f - clamp01(edge[i] * (3.0f + sh * 8.0f)) * 0.6f;
                win.c[i] = map(saturate(win.c[i], 1.2f), [&](float v) { return std::pow(clamp01(v * kk + nn), 1.1f + sh * 0.6f); });
            }
            break;
        }
        // ---------------- ブラシストローク ----------------
        case F::AccentedEdges: {
            std::vector<float> l = win.lum();
            blur1(l, w, h, sc.px(0.4f + g("smoothness") * 0.3f));
            std::vector<float> edge = grad_mag(l, w, h, k);
            blur1(edge, w, h, sc.px(g("edge_width") * 0.5f));
            const float eb = g("edge_brightness") / 50.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const float kk = clamp01(edge[i] * 6.0f);
                win.c[i] = eb >= 0.5f ? map(win.c[i], [&](float v) { return v + (1.0f - v) * kk * (eb - 0.5f) * 2.0f; })
                                      : map(win.c[i], [&](float v) { return v * (1.0f - kk * (0.5f - eb) * 2.0f); });
            }
            break;
        }
        case F::AngledStrokes:
        case F::Crosshatch: {
            const float len = g("stroke_length");
            const int r = sc.r(truncated(len / 3.0f));
            if (e.filter == F::Crosshatch) {  // (its noise drawn out into the strokes: at the working pixels)
                const float st = g("strength");
                for (std::size_t i = 0; i < n; ++i) {
                    const auto [x, y] = win.xy(i);
                    const float nn = (hash01(sc.cell(x), sc.cell(y), 0, 81) - 0.5f) * 0.12f * st;
                    win.c[i] = map(win.c[i], [nn](float v) { return v + nn; });
                }
            }
            const std::vector<float> l = win.lum();
            std::vector<Px> a = win.c;
            line_blur3(a, w, h, {1, 1}, r);
            line_blur3(win.c, w, h, {1, -1}, r);
            if (e.filter == F::AngledStrokes) {
                const float bal = g("direction_balance") / 100.0f;
                for (std::size_t i = 0; i < n; ++i) win.c[i] = mix(a[i], win.c[i], smoothstep(1.0f - bal - 0.15f, 1.0f - bal + 0.15f, l[i]));
                unsharp(win, sc.px(1.0f), g("sharpness") / 10.0f * 1.5f);
            } else {
                for (std::size_t i = 0; i < n; ++i) win.c[i] = mix(a[i], win.c[i], 0.5f);
                unsharp(win, sc.px(1.0f), g("sharpness") / 20.0f * 3.0f);
            }
            break;
        }
        case F::DarkStrokes: {
            const float bal = g("balance") / 10.0f, bi = g("black_intensity") / 10.0f, wi = g("white_intensity") / 10.0f;
            const std::vector<float> l = win.lum();
            std::vector<Px> a = win.c;
            line_blur3(a, w, h, {1, 1}, sc.r(3));
            line_blur3(win.c, w, h, {1, -1}, sc.r(8));
            const float t = 0.25f + bal * 0.5f;
            for (std::size_t i = 0; i < n; ++i) {
                const Px d = map(a[i], [bi](float v) { return v * (1.0f - bi * 0.85f); });
                const Px li = map(win.c[i], [wi](float v) { return v + (1.0f - v) * wi * 0.5f; });
                win.c[i] = mix(d, li, smoothstep(t - 0.1f, t + 0.1f, l[i]));
            }
            break;
        }
        case F::InkOutlines: {
            const float di = g("dark_intensity") / 50.0f, li = g("light_intensity") / 50.0f;
            const std::vector<float> l = win.lum();
            const std::vector<float> edge = grad_mag(l, w, h, k);
            line_blur3(win.c, w, h, {1, 1}, sc.r(truncated(g("stroke_length") / 2.0f)));
            for (std::size_t i = 0; i < n; ++i) {
                const float ink = clamp01(edge[i] * 5.0f) * di * 1.5f;
                const float lt = smoothstep(0.5f, 1.0f, l[i]) * li, dk = smoothstep(0.5f, 0.0f, l[i]) * di * 0.8f;
                win.c[i] = map(win.c[i], [&](float v) { return (v + (1.0f - v) * lt) * (1.0f - dk) * (1.0f - clamp01(ink)); });
            }
            break;
        }
        case F::Spatter:
        case F::SprayedStrokes: {
            const float rad = g("spray_radius");
            const float scale = e.filter == F::Spatter ? 0.6f + g("smoothness") * 0.25f : 1.2f;
            const Win src = win;
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float fx = sc.at(x) / scale, fy = sc.at(y) / scale;
                const float dx = (vnoise(fx, fy, 91) - 0.5f) * 2.0f * sc.px(rad);
                const float dy = (vnoise(fx, fy, 92) - 0.5f) * 2.0f * sc.px(rad);
                win.c[i] = src.sample(static_cast<float>(x) + 0.5f + dx, static_cast<float>(y) + 0.5f + dy);
            }
            if (e.filter == F::SprayedStrokes)
                line_blur3(win.c, w, h, direction(e.choice("stroke_direction")), sc.r(truncated(g("stroke_length") / 2.0f)));
            break;
        }
        case F::SumiE: {
            const float sw = g("stroke_width");
            const float pr = g("stroke_pressure") / 15.0f, con = g("contrast") / 40.0f;
            blur3(win.c, w, h, sc.px(sw * 0.2f));
            line_blur3(win.c, w, h, {1, 1}, sc.r(truncated(sw / 2.0f)));
            for (Px& p : win.c) {
                const float l = lum(p);
                const float ink = std::pow(smoothstep(0.65f, 0.15f, l), 1.0f - pr * 0.5f);
                p = map(p, [&](float v) { return clamp01((v - 0.5f) * (1.0f + con) + 0.5f) * (1.0f - ink * 0.9f * (0.5f + con)); });
            }
            break;
        }
        // ---------------- 変形 ----------------
        case F::DiffuseGlow: {
            const float gr = g("graininess") / 10.0f, ga = g("glow_amount") / 20.0f, ca = g("clear_amount") / 20.0f;
            const float thr = 0.2f + ca * 0.7f;
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float l = lum(win.c[i]);
                float m = smoothstep(thr - 0.3f, thr + 0.1f, l) * ga * 1.4f;
                if (hash01(x, y, 0, 101) < gr * 0.3f * (m + 0.15f)) m += 0.5f;  // (the grain: the layer's pixels)
                win.c[i] = mix(win.c[i], bg, clamp01(m));
            }
            break;
        }
        case F::Glass: {
            const float dist = g("distortion");
            const float step = 0.5f + g("smoothness") * 0.5f;
            const std::string_view kind = e.choice("texture");
            const float s = std::clamp(g("scaling") / 100.0f, 0.5f, 2.0f);
            const float sign = e.flag("invert") ? -1.0f : 1.0f;
            const float most = dist * 2.0f;
            const Win src = win;
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float fx = sc.at(x), fy = sc.at(y);
                const float gx = texture(kind, fx + step, fy, s) - texture(kind, fx - step, fy, s);
                const float gy = texture(kind, fx, fy + step, s) - texture(kind, fx, fy - step, s);
                const float dx = std::clamp(gx * sign * dist * 3.0f, -most, most);
                const float dy = std::clamp(gy * sign * dist * 3.0f, -most, most);
                win.c[i] = src.sample(static_cast<float>(x) + 0.5f + sc.px(dx), static_cast<float>(y) + 0.5f + sc.px(dy));
            }
            break;
        }
        case F::OceanRipple: {
            const float s = 2.0f + g("ripple_size") * 1.2f;
            const float mag = g("ripple_magnitude") * 0.9f;
            const Win src = win;
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float fx = sc.at(x) / s, fy = sc.at(y) / s;
                const float dx = (vnoise(fx, fy, 111) - 0.5f) * 2.0f * sc.px(mag);
                const float dy = (vnoise(fx, fy, 112) - 0.5f) * 2.0f * sc.px(mag);
                win.c[i] = src.sample(static_cast<float>(x) + 0.5f + dx, static_cast<float>(y) + 0.5f + dy);
            }
            break;
        }
        // ---------------- スケッチ ----------------
        case F::BasRelief: {
            std::vector<float> l = win.lum();
            const std::vector<float> orig = l;
            blur1(l, w, h, sc.px(basrelief_sigma(e)));
            const auto [gx, gy] = sobel(l, w, h, k);
            const auto lt = light_dir(e.choice("light"));
            const float kk = 3.0f + g("detail") * 0.4f;
            for (std::size_t i = 0; i < n; ++i) {
                const float t = clamp01(orig[i] * 0.5f + 0.25f + kk * -(gx[i] * lt.first + gy[i] * lt.second));
                win.c[i] = mix(fg, bg, t);
            }
            break;
        }
        case F::ChalkCharcoal: {
            const float ca = g("charcoal_area") / 20.0f, ch = g("chalk_area") / 20.0f, pr = g("stroke_pressure") / 5.0f;
            const Px mid = mix(fg, bg, 0.5f);
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float wx = sc.at(x), wy = sc.at(y);
                const float l = lum(win.c[i]);
                const float cn = strokes(wx, wy, {1, 1}, 9.0f, 0.9f, 121) - 0.5f;
                const float kn = strokes(wx, wy, {1, -1}, 9.0f, 0.9f, 122) - 0.5f;
                const float cc = smoothstep(0.25f, 0.6f, (1.0f - l) * (0.4f + ca * 1.2f) + cn * (0.6f + pr * 0.4f) - 0.15f);
                const float kc = smoothstep(0.25f, 0.6f, l * (0.4f + ch * 1.2f) + kn * (0.6f + pr * 0.4f) - 0.15f);
                win.c[i] = mix(mix(mid, fg, cc), bg, kc * (1.0f - cc));
            }
            break;
        }
        case F::Charcoal: {
            const float th = g("charcoal_thickness"), det = g("detail") / 5.0f, bal = g("light_dark_balance") / 100.0f;
            std::vector<float> l = win.lum();
            blur1(l, w, h, sc.px(1.0f));
            const std::vector<float> edge = grad_mag(l, w, h, k);
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float sn = strokes(sc.at(x), sc.at(y), {1, 1}, 8.0f + th * 3.0f, 0.6f + th * 0.5f, 131) - 0.5f;
                const float cov = smoothstep(1.0f - bal - 0.2f, 1.0f - bal + 0.2f, 1.0f - l[i] + sn * 0.6f) + clamp01(edge[i] * det * 6.0f);
                win.c[i] = mix(bg, fg, clamp01(cov));
            }
            break;
        }
        case F::Chrome: {
            const float det = g("detail") / 10.0f, sm = g("smoothness") / 10.0f;
            std::vector<float> l = win.lum();
            blur1(l, w, h, sc.px(1.0f + sm * 4.0f));
            const std::vector<float> edge = grad_mag(l, w, h, k);
            for (std::size_t i = 0; i < n; ++i) {
                const float v = 0.5f + 0.5f * std::sin((l[i] * (2.5f + det * 3.0f) + edge[i] * 6.0f) * 2.0f * std::numbers::pi_v<float>);
                win.c[i] = grey(smoothstep(0.05f, 0.95f, v));
            }
            break;
        }
        case F::ConteCrayon: {
            const float fl = g("foreground_level") / 15.0f, bl = g("background_level") / 15.0f;
            const Px mid = mix(fg, bg, 0.5f);
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float l = lum(win.c[i]);
                const float sn = strokes(sc.at(x), sc.at(y), {1, -1}, 7.0f, 1.0f, 141) - 0.5f;
                const float cf = clamp01((0.6f - l) * 2.0f * fl * 1.4f + sn * 0.5f);
                const float cb = clamp01((l - 0.4f) * 2.0f * bl * 1.4f - sn * 0.5f);
                win.c[i] = mix(mix(mid, fg, cf), bg, cb * (1.0f - cf));
            }
            texturize(win, e.choice("texture"), g("scaling"), g("relief"), e.choice("light"), e.flag("invert"), sc);
            break;
        }
        case F::GraphicPen: {
            // (the pen's strokes: the noise of the working pixels drawn out along the direction — PhotoCraft's running mean
            // of radius r over each working pixel's hash —, read from the coordinates alone; a layer pixel's ink the working
            // pixels' round it, between them: the same strokes on the paper whatever the resolution, their edges smooth on a
            // finer grid)
            const int r = truncated(g("stroke_length") / 2.0f);
            const std::pair<int, int> d = direction(e.choice("stroke_direction"));
            const float bal = g("light_dark_balance") / 100.0f;
            const std::vector<float> l = win.lum();
            const float sd = 0.2887f / std::sqrt(static_cast<float>(2 * r + 1));
            const auto ink = [r, d, sd, bal](int i, int j, float lightness) {
                double sum = 0;
                for (int step = -r; step <= r; ++step) sum += hash01(i + step * d.first, j + step * d.second, 0, 151);
                const float z = (static_cast<float>(sum / static_cast<double>(2 * r + 1)) - 0.5f) / sd;
                return smoothstep(bal + 0.04f, bal - 0.04f, lightness + z * 0.16f);
            };
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float wx = sc.at(x), wy = sc.at(y);
                const int i0 = static_cast<int>(std::floor(wx)), j0 = static_cast<int>(std::floor(wy));
                const float ax = wx - static_cast<float>(i0), ay = wy - static_cast<float>(j0);
                float cover = ink(i0, j0, l[i]);
                if (ax > 0.0f || ay > 0.0f) {
                    const float right = ink(i0 + 1, j0, l[i]), below = ink(i0, j0 + 1, l[i]), across = ink(i0 + 1, j0 + 1, l[i]);
                    const float top = cover + (right - cover) * ax, bottom = below + (across - below) * ax;
                    cover = top + (bottom - top) * ay;
                }
                win.c[i] = mix(bg, fg, cover);
            }
            break;
        }
        case F::HalftonePattern: {
            const float size = g("size");
            const float con = g("contrast") / 50.0f;
            const float cell = 3.0f + size * 2.0f;
            std::vector<float> l = win.lum();
            blur1(l, w, h, sc.px(0.5f + size * 0.3f));
            const Rect b{ctx.bounds.x0 + ctx.x0, ctx.bounds.y0 + ctx.y0, ctx.bounds.x1 + ctx.x0, ctx.bounds.y1 + ctx.y0};
            const float cx = static_cast<float>(b.x0 + b.x1) / 2.0f / sc.s, cy = static_cast<float>(b.y0 + b.y1) / 2.0f / sc.s;
            const std::string_view type = e.choice("pattern_type");
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float fx = sc.centre(x), fy = sc.centre(y);
                const float d = clamp01((1.0f - l[i] - 0.5f) * (1.0f + con * 3.0f) + 0.5f);
                const float aa = 1.0f / cell;
                float ink = 0;
                if (type == "circle") {
                    const float rr = std::sqrt((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy)) / cell;
                    const float q = std::fabs(rr - std::trunc(rr) - 0.5f) * 2.0f;
                    ink = smoothstep(d + aa, d - aa, q);
                } else if (type == "line") {
                    const float t = fy / cell;
                    const float q = std::fabs(t - std::trunc(t) - 0.5f) * 2.0f;
                    ink = smoothstep(d + aa, d - aa, q);
                } else {
                    const float tu = fx / cell, tv = fy / cell;
                    const float u = tu - std::trunc(tu) - 0.5f, v = tv - std::trunc(tv) - 0.5f;
                    const float q = std::sqrt(u * u + v * v) * 2.0f;
                    const float rr = std::sqrt(d) * 1.15f;
                    ink = smoothstep(rr + aa, rr - aa, q);
                }
                win.c[i] = mix(bg, fg, ink);
            }
            break;
        }
        case F::NotePaper: {
            const float bal = g("image_balance") / 50.0f, gr = g("graininess") / 20.0f, rel = g("relief") / 25.0f;
            std::vector<float> l = win.lum();
            blur1(l, w, h, sc.px(1.0f));
            std::vector<float> m(n);
            for (std::size_t i = 0; i < n; ++i) m[i] = smoothstep(bal - 0.05f, bal + 0.05f, l[i]);
            blur1(m, w, h, sc.px(1.2f));
            const auto [gx, gy] = sobel(m, w, h, k);
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float nn = (hash01(x, y, 0, 161) - 0.5f) * gr * 0.3f;  // (the paper's grain: the layer's pixels)
                const float f = std::clamp(shade(gx[i], gy[i], light_dir("topLeft"), rel * 6.0f), 0.0f, 2.0f);
                win.c[i] = map(mix(fg, bg, m[i]), [&](float v) { return (v + nn) * f; });
            }
            break;
        }
        case F::Photocopy: {
            const float det = g("detail"), dk = g("darkness") / 50.0f;
            const std::vector<float> l = win.lum();
            std::vector<float> b = l;
            blur1(b, w, h, sc.px(1.0f + det * 0.5f));
            for (std::size_t i = 0; i < n; ++i) {
                const float hp = l[i] - b[i];
                const float ink = smoothstep(0.0f, 0.06f, -hp * (1.0f + dk * 8.0f)) + smoothstep(0.35f, 0.05f, l[i]) * dk * 1.5f;
                win.c[i] = mix(bg, fg, clamp01(ink));
            }
            break;
        }
        case F::Plaster: {
            const float bal = g("image_balance") / 50.0f;
            std::vector<float> l = win.lum();
            blur1(l, w, h, sc.px(0.5f + g("smoothness") * 0.8f));
            std::vector<float> m(n);
            for (std::size_t i = 0; i < n; ++i) m[i] = smoothstep(bal + 0.1f, bal - 0.1f, l[i]);
            blur1(m, w, h, sc.px(1.5f));
            const auto [gx, gy] = sobel(m, w, h, k);
            const auto lt = light_dir(e.choice("light"));
            for (std::size_t i = 0; i < n; ++i) {
                const float f = std::clamp(shade(gx[i], gy[i], lt, 6.0f), 0.0f, 2.0f);
                win.c[i] = map(mix(bg, fg, m[i] * 0.6f), [f](float v) { return v * f; });
            }
            break;
        }
        case F::Reticulation: {
            const float den = g("density") / 50.0f, fl = g("foreground_level") / 50.0f, bl = g("background_level") / 50.0f;
            const Px ink_c = mix(bg, fg, 0.3f + fl * 0.7f);
            const Px paper = mix(bg, fg, bl * 0.6f);
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float nn = 0.5f * hash01(x, y, 0, 171) + 0.5f * vnoise(sc.at(x) / 1.8f, sc.at(y) / 1.8f, 172);
                const float t = (1.0f - lum(win.c[i])) + (nn - 0.5f) * (0.4f + den * 1.2f);
                win.c[i] = mix(paper, ink_c, smoothstep(0.45f, 0.55f, t));
            }
            break;
        }
        case F::Stamp: {
            const float bal = g("light_dark_balance") / 50.0f;
            std::vector<float> l = win.lum();
            blur1(l, w, h, sc.px(0.3f + g("smoothness") * 0.3f));
            for (std::size_t i = 0; i < n; ++i) win.c[i] = mix(bg, fg, smoothstep(bal + 0.02f, bal - 0.02f, l[i]));
            break;
        }
        case F::TornEdges: {
            const float bal = g("image_balance") / 50.0f, sm = g("smoothness"), con = g("contrast");
            std::vector<float> l = win.lum();
            blur1(l, w, h, sc.px(1.0f));
            const float rough = (16.0f - sm) / 15.0f * 0.35f;
            const float aa = 0.25f / con;
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float t = l[i] + (fbm(sc.at(x) / 2.0f, sc.at(y) / 2.0f, 181) - 0.5f) * rough;
                win.c[i] = mix(bg, fg, smoothstep(bal + aa, bal - aa, t));
            }
            break;
        }
        case F::WaterPaper: {
            const float fl = g("fiber_length");
            const float br = g("brightness") / 100.0f, co = g("contrast") / 100.0f;
            const int r = sc.r(truncated(fl / 6.0f));
            line_blur3(win.c, w, h, {1, 0}, r);
            line_blur3(win.c, w, h, {0, 1}, r);
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float wx = sc.at(x), wy = sc.at(y);
                const float fib = std::max(strokes(wx, wy, {1, 0}, fl, 1.0f, 191), strokes(wx, wy, {0, 1}, fl, 1.0f, 192));
                win.c[i] = map(win.c[i], [&](float v) { return (v * (0.85f + 0.25f * fib) - 0.5f) * (co * 1.6f + 0.2f) + 0.5f + (br - 0.5f) * 0.8f; });
            }
            break;
        }
        // ---------------- 表現手法 ----------------
        case F::GlowingEdges: {
            blur3(win.c, w, h, sc.px(0.3f + g("smoothness") * 0.25f));
            std::vector<Px> e3(n, Px{0, 0, 0});
            std::vector<float> plane(n);
            for (std::size_t c = 0; c < 3; ++c) {
                for (std::size_t i = 0; i < n; ++i) plane[i] = win.c[i][c];
                const std::vector<float> mag = grad_mag(plane, w, h, k);
                for (std::size_t i = 0; i < n; ++i) e3[i][c] = mag[i];
            }
            blur3(e3, w, h, sc.px(g("edge_width") * 0.4f));
            const float kk = 2.0f + g("edge_brightness") * 1.2f;
            for (std::size_t i = 0; i < n; ++i) win.c[i] = map(e3[i], [kk](float v) { return clamp01(v * kk); });
            break;
        }
        // ---------------- テクスチャ ----------------
        case F::Craquelure: {
            const float sp = g("crack_spacing"), dep = g("crack_depth") / 10.0f, bri = g("crack_brightness") / 10.0f;
            const float cell = sp * 1.3f + 1.0f;
            const float width = 0.8f + dep * 0.8f;
            const auto crack = [&](float x, float y) {
                const Voronoi v = voronoi(x, y, cell, 201);
                return 1.0f - smoothstep(0.0f, width, v.d2 - v.d1);
            };
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float fx = sc.centre(x), fy = sc.centre(y);
                const float c0 = crack(fx, fy);
                const float gx = crack(fx + 1.0f, fy) - c0, gy = crack(fx, fy + 1.0f) - c0;
                const float f = std::clamp(shade(-gx, -gy, light_dir("topLeft"), dep * 2.0f), 0.0f, 2.0f);
                win.c[i] = map(win.c[i], [&](float v) { return v * (0.55f + bri * 0.5f) * (1.0f - c0 * dep * 0.6f) * f; });
            }
            break;
        }
        case F::Grain: {
            const float it = g("intensity") / 100.0f, co = g("contrast") / 100.0f;
            const std::string_view type = e.choice("grain_type");
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float wx = sc.at(x), wy = sc.at(y);
                const auto hc = [px = x, py = y](std::uint32_t c) { return hash01(px, py, c, 211) - 0.5f; };  // (the grain: the layer's pixels)
                const Px p = win.c[i];
                Px q = p;
                if (type == "soft") {
                    q = {p[0] + hc(0) * it * 0.4f, p[1] + hc(1) * it * 0.4f, p[2] + hc(2) * it * 0.4f};
                } else if (type == "sprinkles") {
                    q = hash01(x, y, 3, 211) < it * 0.15f ? bg : p;
                } else if (type == "clumped") {
                    q = map(p, [&](float v) { return v + (vnoise(wx / 2.0f, wy / 2.0f, 212) - 0.5f) * it * 1.2f; });
                } else if (type == "contrasty") {
                    q = map(p, [&](float v) { return (v - 0.5f) * (1.0f + it * 2.0f) + 0.5f + hc(4) * it * 0.5f; });
                } else if (type == "enlarged") {
                    const float fx = wx / 3.0f, fy = wy / 3.0f;
                    q = {p[0] + (vnoise(fx, fy, 213) - 0.5f) * it, p[1] + (vnoise(fx, fy, 214) - 0.5f) * it, p[2] + (vnoise(fx, fy, 215) - 0.5f) * it};
                } else if (type == "stippled") {
                    const Px st = lum(p) > hash01(x, y, 5, 211) ? bg : fg;
                    q = mix(p, st, 0.3f + it * 0.7f);
                } else if (type == "horizontal") {
                    q = map(p, [&](float v) { return v + (vnoise(wx / 25.0f, wy, 216) - 0.5f) * it; });
                } else if (type == "vertical") {
                    q = map(p, [&](float v) { return v + (vnoise(wx, wy / 25.0f, 217) - 0.5f) * it; });
                } else if (type == "speckle") {
                    q = hash01(x, y, 6, 211) < it * 0.1f ? mix(p, fg, 0.85f) : p;
                } else {  // (regular)
                    q = {p[0] + hc(0) * it * 0.8f, p[1] + hc(1) * it * 0.8f, p[2] + hc(2) * it * 0.8f};
                }
                win.c[i] = map(q, [co](float v) { return (v - 0.5f) * (0.5f + co) + 0.5f; });
            }
            break;
        }
        case F::MosaicTiles: {
            const float ts = g("tile_size"), gw = g("grout_width"), lg = g("lighten_grout") / 10.0f;
            const auto edge = [ts](float x, float y) {
                const float jx = x + (vnoise(x / ts * 0.7f, y / ts * 0.7f, 221) - 0.5f) * ts * 0.3f;
                const float jy = y + (vnoise(x / ts * 0.7f, y / ts * 0.7f, 222) - 0.5f) * ts * 0.3f;
                const float u = rem_euclid(jx, ts), v = rem_euclid(jy, ts);
                return std::min(std::min(u, ts - u), std::min(v, ts - v));
            };
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float fx = sc.centre(x), fy = sc.centre(y);
                const float de = edge(fx, fy);
                const float grout = smoothstep(gw * 0.5f + 0.7f, gw * 0.5f - 0.3f, de);
                const auto hgt = [&](float xx, float yy) { return smoothstep(gw * 0.5f, gw * 0.5f + 2.5f, edge(xx, yy)); };
                const float s = (hgt(fx - 1.0f, fy - 1.0f) - hgt(fx + 1.0f, fy + 1.0f)) * 0.35f;
                const float l = lum(win.c[i]);
                const Px gc = grey(clamp01(l * 0.5f + lg * 0.45f));
                win.c[i] = mix(map(win.c[i], [s](float v) { return v * (1.0f + s); }), gc, grout);
            }
            break;
        }
        case F::Patchwork: {
            const float size = patch_size(e);
            const float rel = g("relief") / 25.0f;
            const Win src = win;
            const auto hv = [size](float x, float y) {
                const int bx = static_cast<int>(std::floor(x / size)), by = static_cast<int>(std::floor(y / size));
                const float u = rem_euclid(x, size), v = rem_euclid(y, size);
                const float bevel = smoothstep(0.0f, 1.5f, std::min(std::min(u, size - u), std::min(v, size - v)));
                return hash01(bx, by, 0, 231) * bevel;
            };
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float fx = sc.centre(x), fy = sc.centre(y);
                const float bx = std::floor(fx / size), by = std::floor(fy / size);
                const Px c = src.at(static_cast<int>((bx + 0.5f) * size * sc.s), static_cast<int>((by + 0.5f) * size * sc.s));
                const float s = (hv(fx - 1.0f, fy - 1.0f) - hv(fx + 1.0f, fy + 1.0f)) * rel * 1.5f;
                win.c[i] = map(c, [s](float v) { return v * (1.0f + s); });
            }
            break;
        }
        case F::StainedGlass: {
            const float cell = glass_cell(e);
            const float bt = g("border_thickness");
            const float li = g("light_intensity") / 10.0f;
            const Rect b{ctx.bounds.x0 + ctx.x0, ctx.bounds.y0 + ctx.y0, ctx.bounds.x1 + ctx.x0, ctx.bounds.y1 + ctx.y0};
            const float cx = static_cast<float>(b.x0 + b.x1) / 2.0f / sc.s, cy = static_cast<float>(b.y0 + b.y1) / 2.0f / sc.s;
            const float half = std::max(static_cast<float>(std::max(b.width(), b.height())) / 2.0f / sc.s, 1.0f);
            const Win src = win;
            for (std::size_t i = 0; i < n; ++i) {
                const auto [x, y] = win.xy(i);
                const float fx = sc.centre(x), fy = sc.centre(y);
                const Voronoi v = voronoi(fx, fy, cell, 241);
                const float border = smoothstep(bt * 0.5f + 0.6f, bt * 0.5f - 0.4f, (v.d2 - v.d1) * 0.5f);
                const float dn = std::min(std::sqrt((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy)) / half, 1.0f);
                const Px col = map(src.at(static_cast<int>(v.p1.first * sc.s), static_cast<int>(v.p1.second * sc.s)),
                                   [&](float vv) { return vv * (1.0f + li * 0.6f * (1.0f - dn)); });
                win.c[i] = mix(col, fg, border);
            }
            break;
        }
        case F::Texturizer: texturize(win, e.choice("texture"), g("scaling"), g("relief"), e.choice("light"), e.flag("invert"), sc); break;
    }
}

// What each effect costs a pixel (to tell a heavy stack: render::fx::work), roughly: its blurs and smoothings, its noise
// and textures.
double effect_cost(const Effect& e) {
    switch (e.filter) {
        case F::DryBrush:
        case F::Fresco:
        case F::PaintDaubs:
        case F::Watercolor: return 220;
        case F::Sponge:
        case F::PaletteKnife: return 160;
        case F::RoughPastels:
        case F::Underpainting:
        case F::ConteCrayon:
        case F::Texturizer:
        case F::Glass: return 260;
        case F::Craquelure:
        case F::StainedGlass:
        case F::MosaicTiles:
        case F::GraphicPen:
        case F::Patchwork: return 200;
        case F::GlowingEdges:
        case F::AccentedEdges:
        case F::NotePaper:
        case F::Plaster:
        case F::AngledStrokes:
        case F::Crosshatch:
        case F::DarkStrokes: return 140;
        case F::FilmGrain:
        case F::DiffuseGlow:
        case F::Reticulation:
        case F::Grain: return 40;
        default: return 100;
    }
}

}  // namespace

Kernels gallery_kernels() {
    Kernels all;
    Kernel k;  // フィルターギャラリー (filter.filterGallery; the 47 filter.gallery.* as its effects)
    k.kind = "filter_gallery";
    k.halo = [](const Json& params, const Ctx& ctx) {
        // (what the effects read, and as far again where the clear pixels they read take what shows: fill_clear)
        const Scale sc(ctx.s);
        int reach = 0;
        for (const Effect& e : effects_of(params)) reach += effect_reach(e, sc);
        return Halo::margin(2 * reach);
    };
    k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
        const std::vector<Effect> effects = effects_of(params);
        const std::size_t ch = src.ch;
        if (effects.empty() || src.rect.empty() || (ctx.colours != 3 && ctx.colours != 1)) return src.crop(out);
        const Scale sc(ctx.s);
        // (the window on the layer's grid: its marks where the op on the whole grid puts them — a part of it worked alone
        // has its origin, Ctx::x0)
        Win win;
        win.r = Rect{src.rect.x0 + ctx.x0, src.rect.y0 + ctx.y0, src.rect.x1 + ctx.x0, src.rect.y1 + ctx.y0};
        win.w = static_cast<std::size_t>(src.rect.width());
        win.h = static_cast<std::size_t>(src.rect.height());
        win.c.resize(win.w * win.h);
        std::vector<float> alpha(win.c.size(), 1.0f);
        for (std::size_t i = 0; i < win.c.size(); ++i) {  // (the colours 0..1: what the effects take)
            const float* p = src.data.data() + i * ch;
            win.c[i] = ctx.colours == 3 ? Px{clamp01(p[0]), clamp01(p[1]), clamp01(p[2])} : grey(clamp01(p[0]));
            if (ctx.alpha) alpha[i] = p[ch - 1];
        }
        int reach = 0;
        for (const Effect& e : effects) reach += effect_reach(e, sc);
        fill_clear(win, alpha, reach);
        for (const Effect& e : effects) apply_effect(e, win, sc, ctx);
        std::vector<float> res = src.crop(out);
        const std::size_t ow = static_cast<std::size_t>(out.width());
        for (std::size_t i = 0; i * ch < res.size(); ++i) {
            const int x = out.x0 + static_cast<int>(i % ow) + ctx.x0, y = out.y0 + static_cast<int>(i / ow) + ctx.y0;
            const Px c = win.at(x, y);
            float* px = res.data() + i * ch;
            if (ctx.colours == 3) {
                for (std::size_t c3 = 0; c3 < 3; ++c3) px[c3] = clamp01(c[c3]);
            } else {
                px[0] = clamp01(lum(c));
            }
            // (the alpha as it was)
        }
        return res;
    };
    k.cost = [](const Json& params, const Ctx& ctx) {
        const Scale sc(ctx.s);
        double each = 0;
        int reach = 0;
        for (const Effect& e : effects_of(params)) {
            each += effect_cost(e);
            reach += effect_reach(e, sc);
        }
        // (each tile works its window: the tile and its halo — the reach twice, fill_clear — on every side, upstream's auto_tile)
        const double halo = 2.0 * reach;
        const double tile = std::min(2048.0, std::max(256.0, std::ceil(2.0 * halo / 64.0) * 64.0));
        return std::max(1.0, each) * std::pow((tile + 2.0 * halo) / tile, 2.0);
    };
    // (its own work a pixel of the window, past the engine's count: the colours and a copy of them, Kuwahara's five planes
    // and their whole numbers, a blur's whole numbers)
    k.work_bytes = 112;
    all.push_back(std::move(k));
    return all;
}

}  // namespace genko::render::fx
