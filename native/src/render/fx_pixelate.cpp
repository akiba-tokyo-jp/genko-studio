// PC4 節 D2 (SPEC.md COLOR-08「フィルター」): フィルター → ピクセレート's kernels — カラーハーフトーン, 水晶, ファセット, ぶれ,
// メゾティント, モザイク, 点描. PhotoCraft v0.2.0's algo/src/pixelate.rs (color_halftone, crystallize, facet, fragment,
// mezzotint, pointillize), stylize.rs (mosaic) and fxutil.rs (cell_point), MIT: native/third_party/photocraft/NOTICE.md.
// Cells and screens are anchored at the base box's corner, their sizes pixels of the working 200 dpi (Ctx::s), so a
// pattern falls on the same place of the paper at every resolution; メゾティント's random pattern is of the working pixels
// (the surface's place on the grid added: Ctx::x0, y0). Sums are made in a fixed order a cell (each cell's pixels read
// whole from whichever window holds them): the same bytes whatever the tiling.

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "render/fx_d2.hpp"
#include "render/fx_rank.hpp"

namespace genko::render::fx {

using core::Json;

namespace {

using d2::number;
using d2::text;

// A cell's jittered point (fxutil.rs cell_point): cells of `cell` pixels anchored at `ox`, `oy`.
std::pair<float, float> cell_point(int cx, int cy, float cell, std::uint32_t seed, float ox, float oy) {
    return {ox + (static_cast<float>(cx) + 0.1f + 0.8f * hash01(cx, cy, 11, seed)) * cell, oy + (static_cast<float>(cy) + 0.1f + 0.8f * hash01(cx, cy, 12, seed)) * cell};
}

int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// The colours of a pixel weighted by its alpha (premultiplied), its alpha last.
void add_premultiplied(const float* px, std::size_t n, double* acc, double k = 1.0) {
    const double a = px[n - 1];
    for (std::size_t c = 0; c + 1 < n; ++c) acc[c] += static_cast<double>(px[c]) * a * k;
    acc[n - 1] += a * k;
}

// A premultiplied sum of `count` pixels back as a straight pixel (a clear one's colours 0).
void mean_of(const double* acc, double count, std::size_t n, float* out) {
    if (count <= 0) {
        for (std::size_t c = 0; c < n; ++c) out[c] = 0.0f;
        return;
    }
    const double a = acc[n - 1] / count;
    for (std::size_t c = 0; c + 1 < n; ++c) out[c] = a > 1e-7 ? static_cast<float>(acc[c] / count / a) : 0.0f;
    out[n - 1] = static_cast<float>(a);
}

// メゾティント's threshold of a pattern at a working pixel (pixelate.rs mezzo_threshold).
float mezzo_threshold(const std::string& type, int x, int y, std::uint32_t c, std::uint32_t seed) {
    const auto lines = [&](int len) {
        const int phase = static_cast<int>(hash01(0, y, c + 40, seed) * static_cast<float>(len));
        return hash01(floor_div(x + phase, len), y, c, seed);
    };
    const auto strokes = [&](int len) {
        const int d = x - y;
        const int phase = static_cast<int>(hash01(d, 0, c + 50, seed) * static_cast<float>(len));
        return hash01(floor_div(x + y + phase, len), d, c + 60, seed);
    };
    if (type == "medium_dots") return hash01(floor_div(x, 2), floor_div(y, 2), c, seed);
    if (type == "grainy_dots") return 0.5f * hash01(x, y, c, seed) + 0.5f * hash01(floor_div(x, 3), floor_div(y, 3), c + 7, seed);
    if (type == "coarse_dots") return hash01(floor_div(x, 3), floor_div(y, 3), c, seed);
    if (type == "short_lines") return lines(6);
    if (type == "medium_lines") return lines(14);
    if (type == "long_lines") return lines(32);
    if (type == "short_strokes") return strokes(5);
    if (type == "medium_strokes") return strokes(10);
    if (type == "long_strokes") return strokes(20);
    return hash01(x, y, c, seed);  // (fine_dots)
}

// Is a screen of this space subtractive (its samples ink, 1 the darkest: a CMYK or multichannel page's plates)?
bool inked(const Ctx& ctx) { return ctx.space == Space::Cmyk || ctx.space == Space::Multi; }

}  // namespace

Kernels pixelate_kernels() {
    Kernels all;
    {  // カラーハーフトーン (filter.pixelate.colorHalftone): each colour screened at its own angle into dots whose area follows
       // its ink (1 − the value of RGB's and grey's; a CMYK or multichannel page's plates their ink — CMYK's four at the four
       // angles), the largest dot's radius working pixels
        Kernel k;
        k.kind = "pixelate_color_halftone";
        const auto rmax = [](const Json& params, const Ctx& ctx) { return std::max(1.0f, ctx.px(std::max(1.0, number(params, "max_radius", 8)))); };
        k.halo = [rmax](const Json& params, const Ctx& ctx) { return d2::halo_of(static_cast<int>(std::ceil(rmax(params, ctx) * 3.0f)) + 1); };
        k.run = [rmax](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const std::size_t cc = n - 1;
            std::vector<float> res = src.crop(out);
            const float r_most = rmax(params, ctx);
            const float cell = r_most * std::numbers::sqrt2_v<float>;
            const bool sub = inked(ctx);
            const Rect& b = ctx.bounds;
            const float ox = static_cast<float>(b.x0), oy = static_cast<float>(b.y0);
            const double angles[4] = {number(params, "channel1", 108), number(params, "channel2", 162), number(params, "channel3", 90), number(params, "channel4", 45)};
            std::vector<std::pair<float, float>> rot;
            for (std::size_t c = 0; c < std::max<std::size_t>(cc, 3); ++c) {
                const double a = angles[std::min<std::size_t>(c, 3)] * std::numbers::pi / 180.0;
                rot.emplace_back(static_cast<float>(std::sin(a)), static_cast<float>(std::cos(a)));
            }
            const Rect clip = b.intersect(src.rect);
            if (clip.empty()) return res;
            // (a channel's value at a point: the mean of five taps, clamped to the base box so a cell across its edge reads its
            // pixels)
            const auto value = [&](float x, float y, std::size_t c) {
                const float d = cell * 0.25f;
                float acc = 0;
                for (const auto& [dx, dy] : {std::pair{0.0f, 0.0f}, std::pair{-d, -d}, std::pair{d, -d}, std::pair{-d, d}, std::pair{d, d}}) {
                    const int ix = std::clamp(static_cast<int>(std::floor(x + dx)), clip.x0, clip.x1 - 1);
                    const int iy = std::clamp(static_cast<int>(std::floor(y + dy)), clip.y0, clip.y1 - 1);
                    acc += src.get(ix, iy, c);
                }
                return acc / 5.0f;
            };
            const float touch = std::numbers::pi_v<float> / 4.0f;
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    const float fx = static_cast<float>(x) + 0.5f - ox, fy = static_cast<float>(y) + 0.5f - oy;
                    for (std::size_t c = 0; c < cc; ++c) {
                        const auto [s, co] = rot[c];
                        const float u = fx * co + fy * s, w2 = -fx * s + fy * co;  // (into the screen's space)
                        const float cu = std::floor(u / cell), cv = std::floor(w2 / cell);
                        float cov = 0;
                        for (int dj = -1; dj <= 1; ++dj)
                            for (int di = -1; di <= 1; ++di) {
                                const float mu = (cu + static_cast<float>(di) + 0.5f) * cell, mv = (cv + static_cast<float>(dj) + 0.5f) * cell;
                                const float d = std::sqrt((u - mu) * (u - mu) + (w2 - mv) * (w2 - mv));
                                if (d >= r_most + 0.5f) continue;
                                const float raw = std::clamp(value(mu * co - mv * s + ox, mu * s + mv * co + oy, c), 0.0f, 1.0f);
                                const float ink = sub ? raw : 1.0f - raw;
                                // (a dot's area follows its ink until the dots touch, then grows to cover the cell's corners)
                                const float r = ink <= touch ? cell * std::sqrt(ink / std::numbers::pi_v<float>)
                                                             : cell * (0.5f + (ink - touch) / (1.0f - touch) * (std::numbers::sqrt2_v<float> / 2.0f - 0.5f));
                                cov = std::max(cov, std::min(std::clamp(r - d + 0.5f, 0.0f, 1.0f), std::numbers::pi_v<float> * r * r));
                            }
                        res[i + c] = sub ? cov : 1.0f - cov;
                    }
                }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 4.0 * 9.0 * 8.0; };
        all.push_back(std::move(k));
    }
    {  // 水晶 (filter.pixelate.crystallize): Voronoi cells round jittered points (cells of the working pixels' size, of the
       // seed), each the mean colour of its pixels (premultiplied; on a fine grid its pixels a working pixel apart)
        Kernel k;
        k.kind = "pixelate_crystallize";
        const auto cell_of = [](const Json& params, const Ctx& ctx) { return std::max(2.0f, ctx.px(std::max(2.0, number(params, "cell_size", 10)))); };
        k.halo = [cell_of](const Json& params, const Ctx& ctx) { return d2::halo_of(static_cast<int>(std::ceil(cell_of(params, ctx) * 3.0f)) + 1); };
        k.run = [cell_of](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const float cell = cell_of(params, ctx);
            const std::uint32_t seed = d2::seed_of(params);
            const Rect& b = ctx.bounds;
            const float ox = static_cast<float>(b.x0), oy = static_cast<float>(b.y0);
            const int step = std::max(1, static_cast<int>(std::floor(ctx.s)));  // (the mean over a working pixel's grid)
            const auto grid_cell = [&](float v, float o) { return static_cast<int>(std::floor((v - o) / cell)); };
            const int c0x = grid_cell(static_cast<float>(out.x0), ox) - 2, c0y = grid_cell(static_cast<float>(out.y0), oy) - 2;
            const int c1x = grid_cell(static_cast<float>(out.x1), ox) + 2, c1y = grid_cell(static_cast<float>(out.y1), oy) + 2;
            const std::size_t gw = static_cast<std::size_t>(c1x - c0x + 1), gh = static_cast<std::size_t>(c1y - c0y + 1);
            const auto owner = [&](int x, int y) {
                const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
                const int cx = grid_cell(fx, ox), cy = grid_cell(fy, oy);
                float best = std::numeric_limits<float>::max();
                std::pair<int, int> who{cx, cy};
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const auto [px, py] = cell_point(cx + dx, cy + dy, cell, seed, ox, oy);
                        const float d = (px - fx) * (px - fx) + (py - fy) * (py - fy);
                        if (d < best) {
                            best = d;
                            who = {cx + dx, cy + dy};
                        }
                    }
                return who;
            };
            // (the cells whose pixels can reach `out`: their pixels summed, in their rows' order, inside the base box)
            std::vector<double> acc(gw * gh * (n + 1), 0.0);
            const Rect win = Rect{b.x0 + static_cast<int>(std::floor(static_cast<float>(c0x) * cell)), b.y0 + static_cast<int>(std::floor(static_cast<float>(c0y) * cell)),
                                  b.x0 + static_cast<int>(std::ceil(static_cast<float>(c1x + 1) * cell)), b.y0 + static_cast<int>(std::ceil(static_cast<float>(c1y + 1) * cell))}
                                 .intersect(src.rect)
                                 .intersect(b);
            for (int y = win.y0; y < win.y1; ++y) {
                if ((y - b.y0) % step != 0) continue;
                for (int x = win.x0; x < win.x1; ++x) {
                    if ((x - b.x0) % step != 0) continue;
                    const auto [cx, cy] = owner(x, y);
                    if (cx < c0x || cy < c0y || cx > c1x || cy > c1y) continue;
                    double* a = acc.data() + (std::size_t(cy - c0y) * gw + std::size_t(cx - c0x)) * (n + 1);
                    add_premultiplied(src.px(x, y), n, a);
                    a[n] += 1.0;
                }
            }
            std::vector<float> res = src.crop(out);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    if (!b.contains(x, y)) continue;
                    const auto [cx, cy] = owner(x, y);
                    if (cx < c0x || cy < c0y || cx > c1x || cy > c1y) continue;
                    const double* a = acc.data() + (std::size_t(cy - c0y) * gw + std::size_t(cx - c0x)) * (n + 1);
                    if (a[n] <= 0) continue;
                    mean_of(a, a[n], n, res.data() + i);
                }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 2.0 * 9.0 * 6.0 + 20.0; };
        all.push_back(std::move(k));
    }
    {  // ファセット (filter.pixelate.facet): Kuwahara (pixelate.rs facet) — each pixel's colour the mean of the least varied
       // (in lightness) of its four overlapping quadrants of side 2 working pixels + 1, flat facets with edges kept; the colours
       // of the pixels that show weighed by their alpha, its own alpha kept (no clear pixel's colour comes in)
        Kernel k;
        k.kind = "pixelate_facet";
        const auto radius = [](const Ctx& ctx) { return d2::reach(2.0, ctx.s, 1); };
        k.halo = [radius](const Json&, const Ctx& ctx) { return Halo::margin(radius(ctx) + 2); };
        k.run = [radius](const Json&, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const int r = radius(ctx);
            std::vector<float> res = src.crop(out);
            std::vector<double> sum(n), chosen(n);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    double best = std::numeric_limits<double>::max();
                    bool found = false;
                    for (const auto& [qx, qy] : {std::pair{-r, -r}, std::pair{0, -r}, std::pair{-r, 0}, std::pair{0, 0}}) {
                        std::fill(sum.begin(), sum.end(), 0.0);
                        double s1 = 0, s2 = 0, weight = 0;
                        for (int yy = y + qy; yy <= y + qy + r; ++yy)
                            for (int xx = x + qx; xx <= x + qx + r; ++xx) {
                                const float* p = src.px(std::clamp(xx, src.rect.x0, src.rect.x1 - 1), std::clamp(yy, src.rect.y0, src.rect.y1 - 1));
                                const double a = p[n - 1];
                                if (!(a > 0)) continue;
                                for (std::size_t c = 0; c + 1 < n; ++c) sum[c] += static_cast<double>(p[c]) * a;
                                const double l = lightness(ctx, p);
                                s1 += l * a;
                                s2 += l * l * a;
                                weight += a;
                            }
                        if (!(weight > 0)) continue;
                        const double mean = s1 / weight;
                        const double var = s2 / weight - mean * mean;
                        if (var < best) {
                            best = var;
                            found = true;
                            for (std::size_t c = 0; c + 1 < n; ++c) chosen[c] = sum[c] / weight;
                        }
                    }
                    if (!found) continue;
                    for (std::size_t c = 0; c + 1 < n; ++c) res[i + c] = static_cast<float>(chosen[c]);
                }
            return res;
        };
        k.cost = [radius](const Json&, const Ctx& ctx) {
            const double r = radius(ctx);
            return 4.0 * (r + 1) * (r + 1) * 6.0 + 10.0;
        };
        all.push_back(std::move(k));
    }
    {  // ぶれ (filter.pixelate.fragment): the mean of four copies moved 4 working pixels left, right, up and down
       // (premultiplied)
        Kernel k;
        k.kind = "pixelate_fragment";
        const auto shift = [](const Ctx& ctx) { return d2::reach(4.0, ctx.s, 1); };
        k.halo = [shift](const Json&, const Ctx& ctx) { return Halo::margin(shift(ctx) + 1); };
        k.run = [shift](const Json&, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const int d = shift(ctx);
            std::vector<float> res(out.area() * n);
            std::vector<double> acc(n);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    std::fill(acc.begin(), acc.end(), 0.0);
                    for (const auto& [dx, dy] : {std::pair{-d, 0}, std::pair{d, 0}, std::pair{0, -d}, std::pair{0, d}}) {
                        const int xx = std::clamp(x + dx, src.rect.x0, src.rect.x1 - 1), yy = std::clamp(y + dy, src.rect.y0, src.rect.y1 - 1);
                        add_premultiplied(src.px(xx, yy), n, acc.data(), 0.25);
                    }
                    mean_of(acc.data(), 1.0, n, res.data() + i);
                }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 4.0 * 4.0 * 2.0 + 4.0; };
        all.push_back(std::move(k));
    }
    {  // メゾティント (filter.pixelate.mezzotint): each colour thresholded against a random pattern of dots, lines or strokes
       // (of the working pixels, of the seed): pure values — a page of inks' plates full ink or none
        Kernel k;
        k.kind = "pixelate_mezzotint";
        k.halo = [](const Json&, const Ctx&) { return Halo::margin(0); };
        k.run = [](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const std::string type = text(params, "type", "fine_dots");
            const std::uint32_t seed = d2::seed_of(params);
            std::vector<float> res = src.crop(out);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y) {
                const int wy = d2::working(y, ctx.y0, ctx.s);
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    const int wx = d2::working(x, ctx.x0, ctx.s);
                    for (std::size_t c = 0; c + 1 < n; ++c) res[i + c] = res[i + c] > mezzo_threshold(type, wx, wy, static_cast<std::uint32_t>(c), seed) ? 1.0f : 0.0f;
                }
            }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 4.0 * 4.0; };
        all.push_back(std::move(k));
    }
    {  // モザイク (filter.pixelate.mosaic): square cells of the working pixels' size from the base box's corner, each the mean
       // colour of its part inside the base box (premultiplied)
        Kernel k;
        k.kind = "pixelate_mosaic";
        const auto side = [](const Json& params, const Ctx& ctx) { return d2::reach(std::max(1.0, number(params, "cell_size", 10)), ctx.s, 1); };
        k.halo = [side](const Json& params, const Ctx& ctx) { return d2::halo_of(side(params, ctx) + 1); };
        k.run = [side](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const int cs = side(params, ctx);
            const Rect& b = ctx.bounds;
            std::vector<float> res = src.crop(out);
            std::vector<double> acc(n);
            std::vector<float> mean(n);
            const int gx0 = floor_div(out.x0 - b.x0, cs), gx1 = floor_div(out.x1 - 1 - b.x0, cs);
            const int gy0 = floor_div(out.y0 - b.y0, cs), gy1 = floor_div(out.y1 - 1 - b.y0, cs);
            for (int gy = gy0; gy <= gy1; ++gy)
                for (int gx = gx0; gx <= gx1; ++gx) {
                    const Rect whole{b.x0 + gx * cs, b.y0 + gy * cs, b.x0 + (gx + 1) * cs, b.y0 + (gy + 1) * cs};
                    const Rect cellr = whole.intersect(b);
                    std::fill(acc.begin(), acc.end(), 0.0);
                    double count = 0;
                    for (int yy = cellr.y0; yy < cellr.y1; ++yy)
                        for (int xx = cellr.x0; xx < cellr.x1; ++xx) {
                            add_premultiplied(src.px(xx, yy), n, acc.data());
                            count += 1;
                        }
                    mean_of(acc.data(), count, n, mean.data());
                    const Rect fill = whole.intersect(out);
                    for (int yy = fill.y0; yy < fill.y1; ++yy)
                        for (int xx = fill.x0; xx < fill.x1; ++xx)
                            std::copy(mean.begin(), mean.end(), res.begin() + static_cast<std::ptrdiff_t>((std::size_t(yy - out.y0) * std::size_t(out.width()) + std::size_t(xx - out.x0)) * n));
                }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 3.0 * 4.0 + 8.0; };
        all.push_back(std::move(k));
    }
    {  // 点描 (filter.pixelate.pointillize): dots of random sizes (cells of the working pixels' size, of the seed) in the
       // picture's colour at their centre, each a little lighter or darker, on a canvas of the background colour — the
       // layer's alpha kept as it was under the canvas
        Kernel k;
        k.kind = "pixelate_pointillize";
        const auto cell_of = [](const Json& params, const Ctx& ctx) { return std::max(2.0f, ctx.px(std::max(2.0, number(params, "cell_size", 5)))); };
        k.halo = [cell_of](const Json& params, const Ctx& ctx) { return d2::halo_of(static_cast<int>(std::ceil(cell_of(params, ctx) * 2.0f)) + 1); };
        k.run = [cell_of](const Json& params, const Window& src, const Rect& out, const Ctx& ctx) {
            const std::size_t n = src.ch;
            const float cell = cell_of(params, ctx);
            const std::uint32_t seed = d2::seed_of(params);
            const Rect& b = ctx.bounds;
            const float ox = static_cast<float>(b.x0), oy = static_cast<float>(b.y0);
            const std::array<float, 3> bg = d2::colour(params, "background", {1.0f, 1.0f, 1.0f});
            std::vector<float> res = src.crop(out);
            std::vector<float> dot(n), canvas(n);
            std::size_t i = 0;
            for (int y = out.y0; y < out.y1; ++y)
                for (int x = out.x0; x < out.x1; ++x, i += n) {
                    const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
                    const int cx = static_cast<int>(std::floor((fx - ox) / cell)), cy = static_cast<int>(std::floor((fy - oy) / cell));
                    // (the dot whose edge the pixel is deepest inside)
                    bool any = false;
                    float depth = 0, px0 = 0, py0 = 0;
                    int bx = 0, by = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int gx = cx + dx, gy = cy + dy;
                            const auto [qx, qy] = cell_point(gx, gy, cell, seed, ox, oy);
                            const float rad = cell * (0.45f + 0.35f * hash01(gx, gy, 13, seed));
                            const float d = std::sqrt((qx - fx) * (qx - fx) + (qy - fy) * (qy - fy));
                            const float in = rad - d;
                            if (in > -0.5f && (!any || in > depth)) {
                                any = true;
                                depth = in;
                                px0 = qx;
                                py0 = qy;
                                bx = gx;
                                by = gy;
                            }
                        }
                    const float own_alpha = res[i + n - 1];
                    for (std::size_t c = 0; c + 1 < n; ++c) canvas[c] = c < 3 ? bg[c] : 1.0f;
                    canvas[n - 1] = own_alpha;
                    if (any) {
                        const int sx = std::clamp(static_cast<int>(std::floor(px0)), src.rect.x0, src.rect.x1 - 1);
                        const int sy = std::clamp(static_cast<int>(std::floor(py0)), src.rect.y0, src.rect.y1 - 1);
                        for (std::size_t c = 0; c < n; ++c) dot[c] = src.get(sx, sy, c);
                        const float j = (hash01(bx, by, 17, seed) - 0.5f) * 0.08f;
                        for (std::size_t c = 0; c + 1 < n; ++c) dot[c] = std::clamp(dot[c] + j, 0.0f, 1.0f);
                        d2::mix(canvas.data(), dot.data(), std::clamp(depth + 0.5f, 0.0f, 1.0f), n);  // (the dot over the canvas, by their alpha)
                    }
                    std::copy(canvas.begin(), canvas.end(), res.begin() + static_cast<std::ptrdiff_t>(i));
                }
            return res;
        };
        k.cost = [](const Json&, const Ctx&) { return 9.0 * 8.0 + 12.0; };
        all.push_back(std::move(k));
    }
    return all;
}

}  // namespace genko::render::fx
