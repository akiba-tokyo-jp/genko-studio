#include "render/color_edit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/exposure.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "render/color_canvas.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"

namespace genko::render::color_edit {

namespace {

using Rgba = std::array<double, 4>;

// The page at the raster's own resolution (see the header).
struct Grid {
    int dpi = raster::kWorkingDpi;
    Size page;                            // the page in pixels at dpi
    std::uint32_t width = 0, height = 0;  // the raster's pixels
    double sx = 1, sy = 1;                // raster pixels per mm
    bool exact() const { return page.width == static_cast<int>(width) && page.height == static_cast<int>(height); }
};

Grid grid_of(const core::Page& page, const core::ColorRasterView& raster) {
    const double w_mm = page.spec.width_mm.value(), h_mm = page.spec.height_mm.value();
    if (!(w_mm > 0) || !(h_mm > 0)) throw core::OpError("the page has no size");
    Grid g;
    g.width = raster.width();
    g.height = raster.height();
    const double guess = std::round(g.width * 25.4 / w_mm);
    for (const double d : {guess, guess - 1, guess + 1}) {
        if (d < 1 || d > limits::kResolution) continue;
        const int dpi = static_cast<int>(d);
        if (mm_to_px(w_mm, dpi) == static_cast<int>(g.width) && mm_to_px(h_mm, dpi) == static_cast<int>(g.height)) {
            g.dpi = dpi;
            g.page = Size{static_cast<int>(g.width), static_cast<int>(g.height)};
            g.sx = g.sy = dpi / 25.4;  // (drawn as an 8-bit paint layer at its dpi is)
            return g;
        }
    }
    // not a whole dpi: drawn at the raster's resolution or finer (the working dpi at least), then boxed down onto it
    g.sx = g.width / w_mm;
    g.sy = g.height / h_mm;
    const double finest = std::ceil(std::max(g.sx, g.sy) * 25.4);
    limits::check_count(finest, limits::kResolution, "the raster's resolution");
    g.dpi = std::max(raster::kWorkingDpi, static_cast<int>(finest));
    g.page = Size{mm_to_px(w_mm, g.dpi), mm_to_px(h_mm, g.dpi)};
    limits::check_picture(g.page.width, g.page.height, "the page");
    return g;
}

// A mask of the page (at the grid's dpi) on the raster's own pixels.
Image on_raster(const Image& page_mask, const Grid& g) {
    if (g.exact()) return page_mask;
    return page_mask.resize(Size{static_cast<int>(g.width), static_cast<int>(g.height)}, Resample::Box);
}

Image area_on_raster(const core::Json& area, const Grid& g) {
    const selection::AreaMask drawn = selection::area_mask(area, g.dpi);
    Image page_mask = Image::create("L", g.page, Ink(0));
    page_mask.paste(drawn.mask, Point{static_cast<int>(drawn.x0), static_cast<int>(drawn.y0)});
    return on_raster(page_mask, g);
}

Rgba to_linear(const Rgba& p) { return {core::srgb_to_linear(p[0]), core::srgb_to_linear(p[1]), core::srgb_to_linear(p[2]), p[3]}; }

// Straight linear colour back to the raster's samples: a u16 raster holds 0..1 (a resampling overshoot is cut).
Rgba to_samples(const Rgba& linear, bool floating) {
    Rgba out{};
    for (unsigned c = 0; c < 3; ++c) {
        out[c] = core::linear_to_srgb(linear[c]);
        if (!std::isfinite(out[c])) throw core::Error("value", "high-precision edit exceeds finite working range");
        if (!floating) out[c] = std::clamp(out[c], 0.0, 1.0);
    }
    out[3] = std::clamp(linear[3], 0.0, 1.0);
    return out;
}

// `top` (straight linear colour, alpha) over the pixel, as ColorCanvas lays a normal layer: where nothing is under it or
// it hides all, it is the pixel; else the colours mix in linear light.
void lay(core::ColorRasterEdit& out, std::size_t i, const Rgba& top, bool keep_alpha = false) {
    const Rgba under = out.pixel(i);
    const double a = std::clamp(top[3], 0.0, 1.0) * (keep_alpha ? under[3] : 1.0);
    if (!(a > 0)) return;
    if (a == 1 || under[3] == 0) {
        out.set(i, to_samples(Rgba{top[0], top[1], top[2], a}, out.floating()));
        return;
    }
    const Rgba below = to_linear(under);
    const double alpha = a + below[3] * (1 - a);
    Rgba mixed{};
    for (unsigned c = 0; c < 3; ++c) mixed[c] = (top[c] * a + below[c] * below[3] * (1 - a)) / alpha;
    mixed[3] = alpha;
    out.set(i, to_samples(mixed, out.floating()));
}

// The same for a pixel carried whole (one source pixel, nothing resampled): its own samples, when they cover all or
// nothing is under them; mixed as above otherwise.
void lay_whole(core::ColorRasterEdit& out, std::size_t i, const Rgba& samples) {
    if (!(samples[3] > 0)) return;
    if (samples[3] == 1 || out.pixel(i)[3] == 0) {
        out.set(i, samples);
        return;
    }
    lay(out, i, to_linear(samples));
}

// The pixels an area lifts: each its colour and its alpha × the area's coverage, read as one or resampled (in linear
// light, premultiplied, as ColorCanvas resamples a raster).
class Lifted {
public:
    Lifted(const core::ColorRasterView& source, const Image& inside, const Box& box)
        : source_(source), inside_(inside.tobytes()), width_(static_cast<int>(source.width())), height_(static_cast<int>(source.height())),
          box_(box), linear_(Image::create("RGBA", Size{box.width(), box.height()}, Ink{0, 0, 0, 0})) {
        const Image shown = inside.crop(box);
        linear_.blend(source, Size{width_, height_}, box, 1, false, "normal", &shown);
    }
    // The lifted pixel at (x, y) as it was: its samples, its alpha × the coverage (nothing outside the area).
    std::optional<Rgba> whole(int x, int y) const {
        if (x < box_.x0 || y < box_.y0 || x >= box_.x1 || y >= box_.y1) return std::nullopt;
        const std::size_t i = std::size_t(y) * width_ + x;
        const int m = static_cast<unsigned char>(inside_[i]);
        if (m == 0) return std::nullopt;
        Rgba p = source_.pixel(i);
        p[3] = m == 255 ? p[3] : p[3] * (m / 255.0);
        return p;
    }
    // Premultiplied linear colour and alpha at (x, y).
    Rgba premultiplied(int x, int y) const {
        if (x < box_.x0 || y < box_.y0 || x >= box_.x1 || y >= box_.y1) return {0, 0, 0, 0};
        const Rgba p = linear_.linear_pixel(std::size_t(y - box_.y0) * box_.width() + (x - box_.x0));
        return {p[0] * p[3], p[1] * p[3], p[2] * p[3], p[3]};
    }
    // Lays the lifted pixels seen at (u, v) (raster pixels, a pixel spanning [k, k + 1)) over out's pixel i.
    void lay_at(core::ColorRasterEdit& out, std::size_t i, double u, double v, Resample resample) const {
        if (!std::isfinite(u) || !std::isfinite(v)) return;
        if (resample == Resample::Nearest) {
            if (u < -1 || v < -1 || u > width_ + 1 || v > height_ + 1) return;
            if (const auto p = whole(static_cast<int>(std::floor(u)), static_cast<int>(std::floor(v)))) lay_whole(out, i, *p);
            return;
        }
        const bool cubic = resample == Resample::Bicubic;
        const int reach = cubic ? 2 : 1;
        if (u < box_.x0 - reach - 1 || v < box_.y0 - reach - 1 || u > box_.x1 + reach + 1 || v > box_.y1 + reach + 1) return;
        // a position within a billionth of a pixel of a pixel's centre reads that pixel as it is
        double fx = u - .5, fy = v - .5;
        double x0 = std::floor(fx), y0 = std::floor(fy);
        double tx = fx - x0, ty = fy - y0;
        if (tx < 1e-9) tx = 0; else if (tx > 1 - 1e-9) { tx = 0; x0 += 1; }
        if (ty < 1e-9) ty = 0; else if (ty > 1 - 1e-9) { ty = 0; y0 += 1; }
        const int ix = static_cast<int>(x0), iy = static_cast<int>(y0);
        if (tx == 0 && ty == 0) {
            if (const auto p = whole(ix, iy)) lay_whole(out, i, *p);
            return;
        }
        const auto weights = [cubic](double t) {
            std::array<double, 4> w{};  // for the pixels at -1, 0, 1, 2 from the floor
            if (!cubic) { w[1] = 1 - t; w[2] = t; return w; }
            const auto k = [](double d) {  // Pillow's bicubic, a = -0.5
                constexpr double a = -0.5;
                d = std::abs(d);
                if (d < 1) return ((a + 2) * d - (a + 3)) * d * d + 1;
                if (d < 2) return ((a * d - 5 * a) * d + 8 * a) * d - 4 * a;
                return 0.0;
            };
            w[0] = k(t + 1); w[1] = k(t); w[2] = k(1 - t); w[3] = k(2 - t);
            return w;
        };
        const auto wx = weights(tx), wy = weights(ty);
        Rgba sum{};
        std::array<double, 3> lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
        for (int j = 0; j < 4; ++j) {
            if (wy[j] == 0) continue;
            for (int k = 0; k < 4; ++k) {
                if (wx[k] == 0) continue;
                const Rgba p = premultiplied(ix + k - 1, iy + j - 1);
                for (unsigned c = 0; c < 4; ++c) sum[c] += p[c] * wx[k] * wy[j];
                if (p[3] > 0)
                    for (unsigned c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], p[c] / p[3]); hi[c] = std::max(hi[c], p[c] / p[3]); }
            }
        }
        if (!(sum[3] > 1e-12)) return;
        // (a cubic's overshoot, or a colour divided by almost no alpha, stays within the colours it was made of)
        Rgba mixed{};
        for (unsigned c = 0; c < 3; ++c) mixed[c] = std::clamp(sum[c] / sum[3], lo[c], hi[c]);
        mixed[3] = std::min(1.0, sum[3]);
        lay(out, i, mixed);
    }

private:
    const core::ColorRasterView& source_;
    std::string inside_;
    int width_, height_;
    Box box_;
    ColorCanvas linear_;
};

// The area's pixels lose their alpha (what stays where they were); the area on the raster and its box, if any.
std::optional<std::pair<Image, Box>> leave(core::ColorRasterEdit& out, const core::Json& area, const Grid& g) {
    Image inside = area_on_raster(area, g);
    const auto box = inside.getbbox();
    if (!box) return std::nullopt;
    const std::string covered = inside.tobytes();
    for (int y = box->y0; y < box->y1; ++y) {
        for (int x = box->x0; x < box->x1; ++x) {
            const std::size_t i = std::size_t(y) * g.width + x;
            const int m = static_cast<unsigned char>(covered[i]);
            if (m == 0) continue;
            out.set_alpha(i, m == 255 ? 0.0 : out.pixel(i)[3] * (1 - m / 255.0));
        }
    }
    return std::make_pair(std::move(inside), *box);
}

Box intersection(const Box& a, const Box& b) {
    const Box out{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    return out.x1 > out.x0 && out.y1 > out.y0 ? out : Box{0, 0, 0, 0};
}

// Raster pixels [x0, x1) × [y0, y1) covering the points (raster pixels), widened by `margin` and kept on the raster.
Box covering(const std::vector<std::pair<double, double>>& points, int margin, const Grid& g) {
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (const auto& [x, y] : points) {
        if (!std::isfinite(x) || !std::isfinite(y)) return Box{0, 0, static_cast<int>(g.width), static_cast<int>(g.height)};
        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
    }
    const auto clamp = [](double v, std::uint32_t hi) { return static_cast<int>(std::clamp(v, 0.0, static_cast<double>(hi))); };
    return Box{clamp(std::floor(x0) - margin, g.width), clamp(std::floor(y0) - margin, g.height),
               clamp(std::ceil(x1) + margin, g.width), clamp(std::ceil(y1) + margin, g.height)};
}

}  // namespace

std::string erase(const core::Page& page, const core::Layer& layer, const core::PenPoints& points, double width_mm,
                  std::string_view texture, const std::string& seed) {
    core::ColorRasterEdit out(*layer.color_raster);
    const Grid g = grid_of(page, core::ColorRasterView(*layer.color_raster));
    const bool hard = texture != "soft" && texture != "rough";
    const std::string taken = on_raster(raster::eraser_mask(g.page, points, width_mm, g.dpi, hard ? "" : texture, seed), g).tobytes();
    for (std::size_t i = 0; i < taken.size(); ++i) {
        const int m = static_cast<unsigned char>(taken[i]);
        if (m == 0) continue;
        if (hard && m == 255) {
            out.set(i, Rgba{0, 0, 0, 0});  // (as the 8-bit eraser stamps a clear pixel)
            continue;
        }
        out.set_alpha(i, std::max(0.0, out.pixel(i)[3] - m / 255.0));
    }
    return std::move(out).take();
}

std::string delete_area(const core::Page& page, const core::Layer& layer, const core::Json& area) {
    core::ColorRasterEdit out(*layer.color_raster);
    (void)leave(out, area, grid_of(page, core::ColorRasterView(*layer.color_raster)));
    return std::move(out).take();
}

std::string move_area(const core::Page& page, const core::Layer& layer, const core::Json& area, const selection::Matrix& m,
                      Resample resample) {
    const core::ColorRasterView source(*layer.color_raster);
    const Grid g = grid_of(page, source);
    const selection::Matrix inverse = selection::invert(m);
    core::ColorRasterEdit out(*layer.color_raster);
    const auto left = leave(out, area, g);
    if (!left) return std::move(out).take();
    const auto& [inside, box] = *left;
    const Lifted lifted(source, inside, box);
    // where they go: the box's corners through m
    std::vector<std::pair<double, double>> corners;
    for (const auto& [x, y] : {std::pair{box.x0, box.y0}, std::pair{box.x1, box.y0}, std::pair{box.x1, box.y1}, std::pair{box.x0, box.y1}}) {
        const double xm = x / g.sx, ym = y / g.sy;
        corners.emplace_back((m[0] * xm + m[2] * ym + m[4]) * g.sx, (m[1] * xm + m[3] * ym + m[5]) * g.sy);
    }
    const Box to = covering(corners, 3, g);
    for (int y = to.y0; y < to.y1; ++y) {
        for (int x = to.x0; x < to.x1; ++x) {
            const double xm = (x + .5) / g.sx, ym = (y + .5) / g.sy;
            const double u = (inverse[0] * xm + inverse[2] * ym + inverse[4]) * g.sx;
            const double v = (inverse[1] * xm + inverse[3] * ym + inverse[5]) * g.sy;
            lifted.lay_at(out, std::size_t(y) * g.width + x, u, v, resample);
        }
    }
    return std::move(out).take();
}

std::string warp_area(const core::Page& page, const core::Layer& layer, const core::Json& area, const warp::Go& go,
                      Resample resample) {
    const core::ColorRasterView source(*layer.color_raster);
    const Grid g = grid_of(page, source);
    core::ColorRasterEdit out(*layer.color_raster);
    const auto left = leave(out, area, g);
    if (!left) return std::move(out).take();
    const auto& [inside, box] = *left;
    const Lifted lifted(source, inside, box);
    // warp.warp_image: the box in cells × cells, each cell two triangles, each triangle moved as a whole (affine)
    constexpr int cells = 14;
    using P = std::pair<double, double>;
    std::vector<std::vector<P>> from(cells + 1), to(cells + 1);
    std::vector<P> all;
    for (int j = 0; j <= cells; ++j) {
        for (int i = 0; i <= cells; ++i) {
            const double x = box.x0 + static_cast<double>(box.width()) * i / cells, y = box.y0 + static_cast<double>(box.height()) * j / cells;
            const auto [u, v] = go(x / g.sx, y / g.sy);
            if (!std::isfinite(u) || !std::isfinite(v)) throw core::OpError("the transform stretches the area too far");
            from[j].emplace_back(x, y);
            to[j].emplace_back(u * g.sx, v * g.sy);
            all.push_back(to[j].back());
        }
    }
    const Box reach = covering(all, 3, g);
    if (reach.width() <= 0 || reach.height() <= 0) return std::move(out).take();
    std::vector<std::array<P, 3>> sources, places;
    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            sources.push_back({from[j][i], from[j][i + 1], from[j + 1][i + 1]});
            places.push_back({to[j][i], to[j][i + 1], to[j + 1][i + 1]});
            sources.push_back({from[j][i], from[j + 1][i + 1], from[j + 1][i]});
            places.push_back({to[j][i], to[j + 1][i + 1], to[j + 1][i]});
        }
    }
    // where each triangle lands (barycentric weights of a pixel's centre, all of them at least 0), or nothing
    const auto weights = [](const std::array<P, 3>& d, double x, double y) -> std::optional<std::array<double, 3>> {
        const double ax = d[1].first - d[0].first, ay = d[1].second - d[0].second;
        const double bx = d[2].first - d[0].first, by = d[2].second - d[0].second;
        const double det = ax * by - ay * bx;
        if (!(std::abs(det) >= 1e-12)) return std::nullopt;
        const double px = x - d[0].first, py = y - d[0].second;
        const double l1 = (px * by - py * bx) / det, l2 = (ax * py - ay * px) / det, l0 = 1 - l1 - l2;
        if (!(l0 >= -1e-9 && l1 >= -1e-9 && l2 >= -1e-9)) return std::nullopt;
        return std::array<double, 3>{l0, l1, l2};
    };
    // the last triangle over a pixel is the one it shows (warp.warp_image pastes its pieces in order)
    constexpr std::uint16_t kNone = 0xffff;
    std::vector<std::uint16_t> shown(std::size_t(reach.width()) * reach.height(), kNone);
    for (std::size_t t = 0; t < places.size(); ++t) {
        const Box part = intersection(covering({places[t][0], places[t][1], places[t][2]}, 1, g), reach);
        for (int y = part.y0; y < part.y1; ++y)
            for (int x = part.x0; x < part.x1; ++x)
                if (weights(places[t], x + .5, y + .5)) shown[std::size_t(y - reach.y0) * reach.width() + (x - reach.x0)] = static_cast<std::uint16_t>(t);
    }
    for (int y = reach.y0; y < reach.y1; ++y) {
        for (int x = reach.x0; x < reach.x1; ++x) {
            const std::uint16_t t = shown[std::size_t(y - reach.y0) * reach.width() + (x - reach.x0)];
            if (t == kNone) continue;
            const auto l = weights(places[t], x + .5, y + .5);
            if (!l) continue;
            const auto& s = sources[t];
            const double u = (*l)[0] * s[0].first + (*l)[1] * s[1].first + (*l)[2] * s[2].first;
            const double v = (*l)[0] * s[0].second + (*l)[1] * s[1].second + (*l)[2] * s[2].second;
            lifted.lay_at(out, std::size_t(y) * g.width + x, u, v, resample);
        }
    }
    return std::move(out).take();
}

std::string paste(const core::Page& page, const core::Layer& layer, const core::Layer& pasted, const core::Document* episode) {
    const core::ColorRasterView source(*layer.color_raster);
    const Grid g = grid_of(page, source);
    core::ColorRasterEdit out(*layer.color_raster);
    if (core::has_color_strokes(pasted)) {
        // precise lines keep their colour: drawn in linear light at the grid's dpi, over the box they can reach
        if (!pasted.patches.empty())
            core::not_yet_ported("pasting precise-colour lines together with pictures onto high-precision pixels is not supported yet");
        std::vector<std::pair<double, double>> reach;
        for (const core::StrokePtr& stroke : pasted.strokes->items) {
            const double margin = (stroke->width_mm * 2 + 1) / 25.4 * g.dpi + 4;
            for (const auto& p : stroke->points) {
                const double x = p.x / 25.4 * g.dpi, y = p.y / 25.4 * g.dpi;
                reach.emplace_back(x - margin, y - margin);
                reach.emplace_back(x + margin, y + margin);
            }
        }
        const Box all{0, 0, g.page.width, g.page.height};
        Grid page_grid = g;  // (covering() keeps a box on the page's pixels here)
        page_grid.width = static_cast<std::uint32_t>(g.page.width);
        page_grid.height = static_cast<std::uint32_t>(g.page.height);
        const Box box = reach.empty() ? Box{0, 0, 0, 0} : intersection(covering(reach, 0, page_grid), all);
        if (box.width() <= 0 || box.height() <= 0) return std::move(out).take();
        ColorCanvas drawn(Image::create("RGBA", Size{box.width(), box.height()}, Ink{0, 0, 0, 0}));
        blend_color_strokes(drawn, page, pasted, g.dpi, box, episode);
        if (g.exact()) {
            for (int y = box.y0; y < box.y1; ++y)
                for (int x = box.x0; x < box.x1; ++x)
                    lay(out, std::size_t(y) * g.width + x, drawn.linear_pixel(std::size_t(y - box.y0) * box.width() + (x - box.x0)), layer.lock_alpha);
            return std::move(out).take();
        }
        // boxed down onto the raster: each raster pixel the mean of the drawn pixels whose centres fall in it
        // (premultiplied, in linear light)
        const double kx = static_cast<double>(g.width) / g.page.width, ky = static_cast<double>(g.height) / g.page.height;
        const Box onto = intersection(Box{static_cast<int>(std::floor(box.x0 * kx)), static_cast<int>(std::floor(box.y0 * ky)),
                                          static_cast<int>(std::ceil(box.x1 * kx)), static_cast<int>(std::ceil(box.y1 * ky))},
                                      Box{0, 0, static_cast<int>(g.width), static_cast<int>(g.height)});
        for (int y = onto.y0; y < onto.y1; ++y) {
            const int y0 = std::max(box.y0, static_cast<int>(std::ceil(y / ky - .5))), y1 = std::min(box.y1, static_cast<int>(std::ceil((y + 1) / ky - .5)));
            for (int x = onto.x0; x < onto.x1; ++x) {
                const int x0 = std::max(box.x0, static_cast<int>(std::ceil(x / kx - .5))), x1 = std::min(box.x1, static_cast<int>(std::ceil((x + 1) / kx - .5)));
                Rgba sum{};
                const double whole = std::max(1.0, (std::ceil((x + 1) / kx - .5) - std::ceil(x / kx - .5)) * (std::ceil((y + 1) / ky - .5) - std::ceil(y / ky - .5)));
                for (int py = y0; py < y1; ++py) {
                    for (int px = x0; px < x1; ++px) {
                        const Rgba p = drawn.linear_pixel(std::size_t(py - box.y0) * box.width() + (px - box.x0));
                        for (unsigned c = 0; c < 3; ++c) sum[c] += p[c] * p[3];
                        sum[3] += p[3];
                    }
                }
                if (!(sum[3] > 0)) continue;
                lay(out, std::size_t(y) * g.width + x, Rgba{sum[0] / sum[3], sum[1] / sum[3], sum[2] / sum[3], sum[3] / whole}, layer.lock_alpha);
            }
        }
        return std::move(out).take();
    }
    auto picture = drawable_layer_image(page, pasted, g.dpi, episode);
    if (!picture) return std::move(out).take();
    if (!g.exact()) picture = picture->resize(Size{static_cast<int>(g.width), static_cast<int>(g.height)}, Resample::Box);
    const auto box = picture->getbbox();
    if (!box) return std::move(out).take();
    ColorCanvas drawn(Image::create("RGBA", Size{box->width(), box->height()}, Ink{0, 0, 0, 0}));
    drawn.blend(picture->crop(*box), 1, false);
    for (int y = box->y0; y < box->y1; ++y)
        for (int x = box->x0; x < box->x1; ++x)
            lay(out, std::size_t(y) * g.width + x, drawn.linear_pixel(std::size_t(y - box->y0) * box->width() + (x - box->x0)), layer.lock_alpha);
    return std::move(out).take();
}

void bake_marks(core::Document& doc) {
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        if (doc.is_deferred(p)) continue;
        const core::Page& seen = doc.page(p);
        for (std::size_t l = 0; l < seen.layers.size(); ++l) {
            const core::Layer& layer = seen.layers[l];
            if (!layer.color_raster || (layer.stroke_count() == 0 && layer.patches.empty())) continue;
            core::Page& page = doc.edit_page(p);
            core::Layer& target = page.layers[l];
            core::Layer marks;
            marks.id = target.id;
            marks.role = core::LayerRole::User;
            marks.kind = core::LayerKind::Raster;
            marks.panel_clip = false;
            marks.strokes = target.strokes;
            marks.patches = target.patches;
            std::string pixels = paste(page, target, marks, &doc);
            if (pixels != *target.color_raster) target.color_raster = std::make_shared<const std::string>(std::move(pixels));
            target.strokes = core::empty_strokes();
            target.patches.clear();
        }
    }
}

}  // namespace genko::render::color_edit
