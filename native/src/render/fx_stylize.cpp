// PC4 節 D2 (SPEC.md COLOR-08「フィルター」): フィルター → 表現手法's kernels — 拡散, エンボス, 押し出し, 輪郭検出, ソラリゼーション,
// タイル, 等高線, 風 (油彩: render/fx_oil.cpp). PhotoCraft v0.2.0's algo/src/stylize.rs (emboss, find_edges, solarize) and
// stylize2.rs (diffuse, extrude, tiles, trace_contour, wind), MIT: native/third_party/photocraft/NOTICE.md. On a page of
// inks they work its lightness (FilterInks::LightnessAndColour: CMYK and multichannel plates as 1 − ink, so a relief, an
// edge, a shade and a streak are as on RGB; a Lab page's L, its a and b moved with it or greyed — 128/255 — where the
// result is a grey), タイル the colours the inks mean. Lengths are pixels of the working 200 dpi (Ctx::s); a pixel's grain
// (拡散's neighbour, 風's streaks) is of the layer's pixels (their place on the grid: Ctx::x0, y0).

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <tuple>

#include "render/fx_d2.hpp"
#include "render/fx_rank.hpp"

namespace genko::render::fx {

using core::Json;

namespace {

using d2::light;
using d2::number;
using d2::text;

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// A pixel of the window (past its edge, the edge's).
const float* pixel(const Window& src, int x, int y) {
    return src.px(std::clamp(x, src.rect.x0, src.rect.x1 - 1), std::clamp(y, src.rect.y0, src.rect.y1 - 1));
}

// A grey `g` of lightness 0..1 in the samples' space, its alpha kept: RGB's and grey's every colour, a Lab page's L (its a
// and b none), a CMYK page's on K alone (its C, M and Y no ink), a multichannel page's every plate.
void set_grey(const Ctx& ctx, float* px, std::size_t n, float g) {
    for (std::size_t c = 0; c + 1 < n; ++c) {
        if (ctx.space == Space::Lab) px[c] = c == 0 ? g : d2::kLabMiddle;
        else if (ctx.space == Space::Cmyk) px[c] = c == 3 ? g : 1.0f;
        else px[c] = g;
    }
}

// A colour made darker by `k` (its lightness × k): a Lab page's L × k, its a and b nearer their middle by k.
float shaded(const Ctx& ctx, std::size_t c, float v, float k) {
    if (!d2::lightness_channel(ctx, c)) return d2::kLabMiddle + (v - d2::kLabMiddle) * k;
    return std::clamp(v * k, 0.0f, 1.0f);
}

}  // namespace

Kernels stylize_kernels() {
    Kernels all;
    {  // 拡散 (filter.stylize.diffuse): each pixel swapped for a random neighbour within a working pixel (標準), only a darker or
       // a lighter one (比較（暗）・比較（明）), or half-way to its most alike neighbour (異方性)
        Kernel k;
        k.kind = "stylize_diffuse";
        const auto step = [](const Ctx& ctx) { return d2::reach(1.0, ctx.s, 1); };
        k.halo = [step](const Json&, const Ctx& ctx) { return Halo::margin(step(ctx) + 1); };
        k.run = [step](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const int d = step(ctx);
            const std::string mode = text(params, "mode", "normal");
            const std::uint32_t seed = d2::seed_of(params);
            std::vector<float> res = src.crop(out);
            static constexpr std::pair<int, int> kRing[8] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    float* px = res.data() + i;
                    const int gx = x + ctx.x0, gy = y + ctx.y0;  // (the grain: the layer's pixels)
                    if (mode == "anisotropic") {
                        const float me = light(ctx, px);
                        float best = std::numeric_limits<float>::max();
                        std::pair<int, int> chosen{0, 0};
                        const std::size_t start = static_cast<std::size_t>(hash01(gx, gy, 3, seed) * 8.0f);
                        for (std::size_t q = 0; q < 8; ++q) {
                            const auto [dx, dy] = kRing[(start + q) % 8];
                            const float l = std::fabs(light(ctx, pixel(src, x + dx * d, y + dy * d)) - me);
                            if (l < best) {
                                best = l;
                                chosen = {dx * d, dy * d};
                            }
                        }
                        d2::mix(px, pixel(src, x + chosen.first, y + chosen.second), 0.5f, n);  // (half-way, by their alpha)
                        continue;
                    }
                    const int dx = static_cast<int>(hash01(gx, gy, 1, seed) * static_cast<float>(2 * d + 1)) - d;
                    const int dy = static_cast<int>(hash01(gx, gy, 2, seed) * static_cast<float>(2 * d + 1)) - d;
                    const float* nb = pixel(src, x + dx, y + dy);
                    if (nb[n - 1] <= 0.0f && px[n - 1] > 0.0f && mode != "normal") continue;
                    const bool take = mode == "darken_only" ? light(ctx, nb) < light(ctx, px) : mode == "lighten_only" ? light(ctx, nb) > light(ctx, px) : true;
                    if (take) std::copy(nb, nb + n, px);
                }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 8.0 * 6.0 + 8.0; };
        all.push_back(std::move(k));
    }
    {  // エンボス (filter.stylize.emboss): a grey relief, 0.5 plus the amount % of the difference of the lightness the height
       // (working pixels) up and down the light's angle
        Kernel k;
        k.kind = "stylize_emboss";
        const auto height = [](const Json& params, const Ctx& ctx) { return std::max(1.0f, ctx.px(std::max(1.0, number(params, "height", 3)))); };
        k.halo = [height](const Json& params, const Ctx& ctx) { return Halo::margin(static_cast<int>(std::ceil(height(params, ctx))) + 2); };
        k.run = [height](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const double a = number(params, "angle", 135) * std::numbers::pi / 180.0;
            const float h = height(params, ctx);
            const int dx = static_cast<int>(std::lround(std::cos(a) * h)), dy = static_cast<int>(std::lround(-std::sin(a) * h));
            const float amount = static_cast<float>(number(params, "amount", 100) / 100.0);
            std::vector<float> res = src.crop(out);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    float* px = res.data() + i;
                    if (ctx.space == Space::Multi) {  // (a multichannel page: each plate's own relief — its lightness)
                        for (std::size_t c = 0; c + 1 < n; ++c)
                            px[c] = std::clamp(0.5f + amount * (pixel(src, x + dx, y + dy)[c] - pixel(src, x - dx, y - dy)[c]), 0.0f, 1.0f);
                        continue;
                    }
                    const float g = 0.5f + amount * (light(ctx, pixel(src, x + dx, y + dy)) - light(ctx, pixel(src, x - dx, y - dy)));
                    set_grey(ctx, px, n, std::clamp(g, 0.0f, 1.0f));
                }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 2.0 * 6.0 + 6.0; };
        all.push_back(std::move(k));
    }
    {  // 押し出し (filter.stylize.extrude): the base box cut into square cells (working pixels) raised as blocks — their front
       // faces moved out from its centre by their height, their sides shaded — or as pyramids of four shaded faces; heights
       // random (of the seed) or by each cell's lightness
        Kernel k;
        k.kind = "stylize_extrude";
        const auto size = [](const Json& params, const Ctx& ctx) { return d2::reach(std::clamp(number(params, "size", 30), 2.0, 255.0), ctx.s, 2); };
        const auto reach = [](const Json& params, const Ctx& ctx) { return static_cast<float>(std::clamp(number(params, "depth", 30), 0.0, 255.0) * 0.25 * ctx.s); };
        k.halo = [size, reach](const Json& params, const Ctx& ctx) {
            return d2::halo_of(static_cast<int>(std::ceil(reach(params, ctx) + static_cast<float>(size(params, ctx)) * 3.0f)) + 2);
        };
        k.run = [size, reach](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const int cs = size(params, ctx);
            const float fcs = static_cast<float>(cs);
            const float depth = static_cast<float>(std::clamp(number(params, "depth", 30), 1.0, 255.0));
            const bool pyramids = text(params, "type", "blocks") == "pyramids";
            const bool level_based = text(params, "depth_mode", "random") == "level_based";
            const Json* solid_value = core::get(params, "solid_front_faces");
            const Json* mask_value = core::get(params, "mask_incomplete_blocks");
            const bool solid_front = solid_value != nullptr && solid_value->is_boolean() && solid_value->get<bool>();
            const bool mask_incomplete = mask_value != nullptr && mask_value->is_boolean() && mask_value->get<bool>();
            const std::uint32_t seed = d2::seed_of(params);
            const Rect& b = ctx.bounds;
            const float bcx = static_cast<float>(b.x0 + b.x1) / 2.0f, bcy = static_cast<float>(b.y0 + b.y1) / 2.0f;
            const float half_diag = std::max(1.0f, std::hypot(static_cast<float>(b.width()), static_cast<float>(b.height())) / 2.0f);
            const float r_out = reach(params, ctx);
            const auto cell_rect = [&](int gx, int gy) { return Rect{b.x0 + gx * cs, b.y0 + gy * cs, b.x0 + (gx + 1) * cs, b.y0 + (gy + 1) * cs}; };
            const auto incomplete = [&](const Rect& r) { return r.intersect(b) != r; };
            struct Info {
                std::vector<float> colour;
                float height = 0;
            };
            // (a cell's mean colour — sampled sparsely: it only colours faces — and its height)
            const auto cell_info = [&](int gx, int gy) {
                const Rect r = cell_rect(gx, gy).intersect(b);
                std::vector<double> acc(n, 0.0);
                double count = 0;
                const int stride = std::max(1, cs / 8);
                for (int yy = r.y0; yy < r.y1; yy += stride)
                    for (int xx = r.x0; xx < r.x1; xx += stride) {
                        const float* p = pixel(src, xx, yy);
                        const double a = p[n - 1];
                        for (std::size_t c = 0; c + 1 < n; ++c) acc[c] += static_cast<double>(p[c]) * a;
                        acc[n - 1] += a;
                        count += 1;
                    }
                Info info;
                info.colour.assign(n, 0.0f);
                if (count > 0) {
                    const double a = acc[n - 1] / count;
                    for (std::size_t c = 0; c + 1 < n; ++c) info.colour[c] = a > 1e-7 ? static_cast<float>(acc[c] / count / a) : 0.0f;
                    info.colour[n - 1] = static_cast<float>(a);
                }
                info.height = level_based ? std::clamp(light(ctx, info.colour.data()), 0.0f, 1.0f) : hash01(gx, gy, 21, seed);
                return info;
            };
            std::vector<std::pair<std::pair<int, int>, Info>> cache;
            const auto info = [&](int gx, int gy) -> const Info& {
                for (const auto& [key, value] : cache)
                    if (key == std::pair{gx, gy}) return value;
                if (cache.size() >= 64) cache.erase(cache.begin());
                cache.emplace_back(std::pair{gx, gy}, cell_info(gx, gy));
                return cache.back().second;
            };
            const auto shade = [&](float* px, const std::vector<float>& col, float kk) {
                for (std::size_t c = 0; c + 1 < n; ++c) px[c] = shaded(ctx, c, col[c], kk);
                px[n - 1] = col[n - 1];
            };
            std::vector<float> res = src.crop(out);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y) {
                cache.clear();
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    if (!b.contains(x, y)) continue;
                    float* px = res.data() + i;
                    const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
                    const int gx = floor_div(x - b.x0, cs), gy = floor_div(y - b.y0, cs);
                    if (pyramids) {
                        const Rect r = cell_rect(gx, gy);
                        if (mask_incomplete && incomplete(r)) continue;
                        const Info& cell = info(gx, gy);
                        const float u = fx - (static_cast<float>(r.x0) + fcs / 2.0f), v = fy - (static_cast<float>(r.y0) + fcs / 2.0f);
                        // (the faces' normals of a pyramid of height hgt over a cell of half side s; the light from the top left)
                        const float hgt = cell.height * depth / 255.0f * fcs;
                        const float s = fcs / 2.0f;
                        const auto sign = [](float t) { return t < 0 ? -1.0f : t > 0 ? 1.0f : 0.0f; };
                        const float nx = std::fabs(u) >= std::fabs(v) ? sign(u) * hgt : 0.0f, ny = std::fabs(u) >= std::fabs(v) ? 0.0f : sign(v) * hgt;
                        const float norm = std::sqrt(nx * nx + ny * ny + s * s);
                        const float lambert = std::max((nx * -0.5f + ny * -0.5f + s * std::numbers::sqrt2_v<float> / 2.0f) / norm, 0.0f);
                        shade(px, cell.colour, 0.35f + 0.9f * lambert);
                        continue;
                    }
                    // (the blocks: its own cell's and the neighbours' whose front faces reach here; the highest wins)
                    const int kk = static_cast<int>(std::ceil(r_out / fcs)) + 1;
                    std::optional<std::tuple<float, int, int, int>> best;  // (height, gx, gy, face: 0 front, 1..4 sides)
                    for (int dy = -kk; dy <= kk; ++dy)
                        for (int dx = -kk; dx <= kk; ++dx) {
                            const int cx = gx + dx, cy = gy + dy;
                            const Rect r = cell_rect(cx, cy);
                            if (r.x1 <= b.x0 || r.y1 <= b.y0 || r.x0 >= b.x1 || r.y0 >= b.y1) continue;
                            if (mask_incomplete && incomplete(r)) continue;
                            const float h = info(cx, cy).height;
                            if (best && std::get<0>(*best) >= h) continue;
                            const float mx = static_cast<float>(r.x0) + fcs / 2.0f, my = static_cast<float>(r.y0) + fcs / 2.0f;
                            const float off = r_out * h / half_diag;
                            const float ox = (mx - bcx) * off, oy = (my - bcy) * off;
                            const float half = fcs / 2.0f;
                            const auto interval = [half](float p, float m, float o) -> std::optional<std::pair<float, float>> {
                                if (std::fabs(o) < 1e-6f) return std::fabs(p - m) <= half ? std::optional(std::pair{0.0f, 1.0f}) : std::nullopt;
                                const float a = (p - m - half) / o, bb = (p - m + half) / o;
                                const float lo = std::max(std::min(a, bb), 0.0f), hi = std::min(std::max(a, bb), 1.0f);
                                return lo <= hi ? std::optional(std::pair{lo, hi}) : std::nullopt;
                            };
                            const auto ix = interval(fx, mx, ox), iy = interval(fy, my, oy);
                            if (!ix || !iy) continue;
                            const float lo = std::max(ix->first, iy->first), hi = std::min(ix->second, iy->second);
                            if (lo > hi) continue;
                            const int face = hi >= 1.0f ? 0 : ix->second < iy->second ? (ox > 0.0f ? 1 : 2) : (oy > 0.0f ? 3 : 4);
                            best = std::tuple{h, cx, cy, face};
                        }
                    if (!best) continue;
                    const auto [h, cx, cy, face] = *best;
                    const Rect r = cell_rect(cx, cy);
                    const float mx = static_cast<float>(r.x0) + fcs / 2.0f, my = static_cast<float>(r.y0) + fcs / 2.0f;
                    const float off = r_out * h / half_diag;
                    const Info& cell = info(cx, cy);
                    if (face == 0) {
                        if (solid_front) {
                            shade(px, cell.colour, 1.0f);
                        } else {  // (the picture moved with the face)
                            const int sx = std::clamp(static_cast<int>(std::floor(fx - (mx - bcx) * off)), r.x0, r.x1 - 1);
                            const int sy = std::clamp(static_cast<int>(std::floor(fy - (my - bcy) * off)), r.y0, r.y1 - 1);
                            for (std::size_t c = 0; c < n; ++c) px[c] = src.get(sx, sy, c);
                        }
                    } else {  // (the light from the top left: the left and top sides lit)
                        shade(px, cell.colour, face == 1 ? 0.85f : face == 2 ? 0.55f : face == 3 ? 0.95f : 0.45f);
                    }
                }
            }
            return res;
        };
        k.cost = [size, reach](const Json& params, const Ctx& ctx) {
            const double kk = std::ceil(reach(params, ctx) / size(params, ctx)) + 1;
            return (2 * kk + 1) * (2 * kk + 1) * 12.0 + 40.0;
        };
        all.push_back(std::move(k));
    }
    {  // 輪郭検出 (filter.stylize.findEdges): dark edges on white — each colour 1 − its Sobel magnitude / 4 (its taps a working
       // pixel apart); a Lab page's L, its a and b grey
        Kernel k;
        k.kind = "stylize_find_edges";
        const auto step = [](const Ctx& ctx) { return d2::reach(1.0, ctx.s, 1); };
        k.halo = [step](const Json&, const Ctx& ctx) { return Halo::margin(step(ctx) + 1); };
        k.run = [step](const Json&, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const int d = step(ctx);
            std::vector<float> res = src.crop(out);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n)
                    for (std::size_t c = 0; c + 1 < n; ++c) {
                        if (!d2::lightness_channel(ctx, c)) {
                            res[i + c] = d2::kLabMiddle;
                            continue;
                        }
                        const auto g = [&](int dx, int dy) { return pixel(src, x + dx * d, y + dy * d)[c]; };
                        const float gx = (g(1, -1) + 2.0f * g(1, 0) + g(1, 1)) - (g(-1, -1) + 2.0f * g(-1, 0) + g(-1, 1));
                        const float gy = (g(-1, 1) + 2.0f * g(0, 1) + g(1, 1)) - (g(-1, -1) + 2.0f * g(0, -1) + g(1, -1));
                        res[i + c] = std::clamp(1.0f - std::sqrt(gx * gx + gy * gy) / 4.0f, 0.0f, 1.0f);
                    }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 4.0 * 10.0; };
        all.push_back(std::move(k));
    }
    {  // ソラリゼーション (filter.stylize.solarize): each colour past the middle turned (1 − it): a Lab page's L, its a and b as
       // they were
        Kernel k;
        k.kind = "stylize_solarize";
        k.halo = [](const Json&, const Ctx&) { return Halo::margin(0); };
        k.run = [](const Json&, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            std::vector<float> res = src.crop(out);
            for (std::size_t i = 0; i < res.size(); i += n)
                for (std::size_t c = 0; c + 1 < n; ++c) {
                    if (!d2::lightness_channel(ctx, c)) continue;
                    if (res[i + c] > 0.5f) res[i + c] = 1.0f - res[i + c];
                }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 4.0; };
        all.push_back(std::move(k));
    }
    {  // タイル (filter.stylize.tiles): the base box cut into square tiles (so many along its shorter side), each moved at
       // random (of the seed) by up to the most offset % of its side, a later one over an earlier; the gaps filled with the
       // background or foreground colour, the picture inverted, or the picture
        Kernel k;
        k.kind = "stylize_tiles";
        k.halo = [](const Json&, const Ctx&) { return Halo::whole(); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const Rect& b = ctx.bounds;
            const int count = std::clamp(static_cast<int>(std::lround(number(params, "count", 10))), 1, 99);
            const float ts = std::max(static_cast<float>(std::min(b.width(), b.height())) / static_cast<float>(count), 1.0f);
            const float m = static_cast<float>(std::clamp(number(params, "max_offset", 10), 0.0, 99.0) / 100.0) * ts;
            const std::string fill = text(params, "fill", "background");
            const std::array<float, 3> fg = d2::colour(params, "foreground", {0, 0, 0}), bg = d2::colour(params, "background", {1, 1, 1});
            const std::uint32_t seed = d2::seed_of(params);
            const auto offset = [&](int gx, int gy) {
                return std::pair{(hash01(gx, gy, 31, seed) * 2.0f - 1.0f) * m, (hash01(gx, gy, 32, seed) * 2.0f - 1.0f) * m};
            };
            std::vector<float> res = src.crop(out);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    if (!b.contains(x, y)) continue;
                    float* px = res.data() + i;
                    const float fx = static_cast<float>(x) + 0.5f - static_cast<float>(b.x0), fy = static_cast<float>(y) + 0.5f - static_cast<float>(b.y0);
                    const int gx = static_cast<int>(std::floor(fx / ts)), gy = static_cast<int>(std::floor(fy / ts));
                    // (the topmost tile over the pixel: the last drawn, row by row)
                    std::optional<std::tuple<std::pair<int, int>, float, float>> hit;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int cx = gx + dx, cy = gy + dy;
                            const auto [ox, oy] = offset(cx, cy);
                            const float u = fx - ox - static_cast<float>(cx) * ts, v = fy - oy - static_cast<float>(cy) * ts;
                            if (u >= 0 && v >= 0 && u < ts && v < ts && static_cast<float>(cx) * ts < static_cast<float>(b.width()) &&
                                static_cast<float>(cy) * ts < static_cast<float>(b.height()) && cx >= 0 && cy >= 0) {
                                const std::pair order{cy, cx};
                                if (!hit || order > std::get<0>(*hit)) hit = std::tuple{order, ox, oy};
                            }
                        }
                    if (hit) {
                        const auto [order, ox, oy] = *hit;
                        const int sx = std::clamp(static_cast<int>(std::floor(static_cast<float>(x) + 0.5f - ox)), b.x0, b.x1 - 1);
                        const int sy = std::clamp(static_cast<int>(std::floor(static_cast<float>(y) + 0.5f - oy)), b.y0, b.y1 - 1);
                        for (std::size_t c = 0; c < n; ++c) px[c] = src.get(sx, sy, c);
                    } else if (fill == "background" || fill == "foreground") {
                        const std::array<float, 3>& with = fill == "background" ? bg : fg;
                        for (std::size_t c = 0; c + 1 < n; ++c) px[c] = c < 3 ? with[c] : 0.0f;
                        px[n - 1] = 1.0f;
                    } else if (fill == "inverse") {
                        for (std::size_t c = 0; c + 1 < n; ++c) px[c] = 1.0f - px[c];
                    }
                }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 9.0 * 4.0 + 8.0; };
        all.push_back(std::move(k));
    }
    {  // 等高線 (filter.stylize.traceContour): where each colour crosses the level (0..255), a line a working pixel wide on
       // the side above it (上) or below it (下), black on white; a Lab page's L, its a and b grey
        Kernel k;
        k.kind = "stylize_trace_contour";
        const auto step = [](const Ctx& ctx) { return d2::reach(1.0, ctx.s, 1); };
        k.halo = [step](const Json&, const Ctx& ctx) { return Halo::margin(step(ctx) + 1); };
        k.run = [step](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const int d = step(ctx);
            const float t = static_cast<float>(std::clamp(number(params, "level", 128), 0.0, 255.0) / 255.0);
            const bool upper = text(params, "edge", "lower") == "upper";
            const Rect& area = ctx.bounds;  // (past the base box, its edge: the window holds what that reaches)
            const auto bright = [&](int x, int y, std::size_t c) {
                return src.get(std::clamp(x, area.x0, area.x1 - 1), std::clamp(y, area.y0, area.y1 - 1), c) > t;
            };
            std::vector<float> res = src.crop(out);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n)
                    for (std::size_t c = 0; c + 1 < n; ++c) {
                        if (!d2::lightness_channel(ctx, c)) {
                            res[i + c] = d2::kLabMiddle;
                            continue;
                        }
                        const bool me = bright(x, y, c);
                        bool line = false;
                        if (me == upper) {
                            for (int k = 1; k <= d && !line; ++k)
                                for (const auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
                                    if (bright(x + dx * k, y + dy * k, c) != me) line = true;
                        }
                        res[i + c] = line ? 0.0f : 1.0f;
                    }
            return res;
        };
        k.cost = [step](const Json&, const Ctx& ctx) { return 4.0 * 4.0 * step(ctx) + 4.0; };
        all.push_back(std::move(k));
    }
    {  // 風 (filter.stylize.wind): streaks trailing downwind from the edges (風; 強風: longer and denser), or the rows' runs
       // moved along it (ずらし), of the seed; the longest 16, 40 or 12 working pixels
        Kernel k;
        k.kind = "stylize_wind";
        const auto longest = [](const Json& params, const Ctx& ctx) {
            const std::string method = text(params, "method", "wind");
            return d2::reach(method == "blast" ? 40.0 : method == "stagger" ? 12.0 : 16.0, ctx.s, 1);
        };
        k.halo = [longest](const Json& params, const Ctx& ctx) { return Halo::margin(longest(params, ctx) + 2); };
        k.run = [longest](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const int lmax = longest(params, ctx);
            const std::string method = text(params, "method", "wind");
            const int d = text(params, "direction", "from_right") == "from_left" ? 1 : -1;  // (streaks move downwind)
            const std::uint32_t seed = d2::seed_of(params);
            const float strength = method == "blast" ? 8.0f : 4.0f;
            std::vector<float> res = src.crop(out);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y) {
                const int gy = y + ctx.y0;
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    float* px = res.data() + i;
                    if (method == "stagger") {  // (each row cut into runs of 8–40 working pixels, each moved by up to the longest)
                        const int seg_len = d2::reach(8.0 + std::floor(hash01(0, gy, 41, seed) * 32.0f), ctx.s, 1);
                        const int phase = static_cast<int>(hash01(1, gy, 42, seed) * static_cast<float>(seg_len));
                        const int seg = floor_div((x + ctx.x0) * d + phase, seg_len);
                        const float h = hash01(seg, gy, 43, seed);
                        const int shift = static_cast<int>(h * h * static_cast<float>(lmax));
                        const float* from = pixel(src, x - d * shift, y);
                        std::copy(from, from + n, px);
                        continue;
                    }
                    float best = 0;
                    int from_x = x;
                    for (int k = 1; k <= lmax; ++k) {
                        const int s = x - d * k;
                        // (each pixel upwind starts a streak of a random length; only an edge makes it show)
                        const float hh = hash01(s + ctx.x0, gy, 44, seed);
                        const float len = std::max(hh * hh * static_cast<float>(lmax), 1.0f);
                        if (static_cast<float>(k) > len) continue;
                        const float edge = std::min(std::fabs(light(ctx, pixel(src, s, y)) - light(ctx, pixel(src, s - d, y))) * strength, 1.0f);
                        const float w = edge * (1.0f - static_cast<float>(k) / (len + 1.0f));
                        if (w > best) {
                            best = w;
                            from_x = s;
                        }
                    }
                    if (best > 0) d2::mix(px, pixel(src, from_x, y), best, n);  // (by their alpha)
                }
            }
            return res;
        };
        k.cost = [longest](const Json& params, const Ctx& ctx) { return 2.0 * 6.0 * longest(params, ctx) + 8.0; };
        all.push_back(std::move(k));
    }
    all.push_back(oil_kernel());
    return all;
}

}  // namespace genko::render::fx
