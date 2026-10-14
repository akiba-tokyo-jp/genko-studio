#include "render/ink_edit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <utility>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "render/image.hpp"
#include "render/imaging.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"

namespace genko::render::ink_edit {

namespace {

using core::InkPixels;
using core::PageColor;

// --- the raster on the page ------------------------------------------------------------------------------------------

// The page at the raster's own resolution (as render/color_edit's): a whole dpi when one gives the page exactly the
// raster's size, else the working dpi or finer, boxed down onto the raster.
struct Grid {
    int dpi = raster::kWorkingDpi;
    Size page;                            // the page in pixels at dpi
    std::uint32_t width = 0, height = 0;  // the raster's pixels
    double sx = 1, sy = 1;                // raster pixels per mm
    bool exact() const { return page.width == static_cast<int>(width) && page.height == static_cast<int>(height); }
};

Grid grid_of(const core::Page& page, std::uint32_t width, std::uint32_t height) {
    const double w_mm = page.spec.width_mm.value(), h_mm = page.spec.height_mm.value();
    if (!(w_mm > 0) || !(h_mm > 0)) throw core::OpError("the page has no size");
    Grid g;
    g.width = width;
    g.height = height;
    const double guess = std::round(g.width * 25.4 / w_mm);
    for (const double d : {guess, guess - 1, guess + 1}) {
        if (d < 1 || d > limits::kResolution) continue;
        const int dpi = static_cast<int>(d);
        if (mm_to_px(w_mm, dpi) == static_cast<int>(g.width) && mm_to_px(h_mm, dpi) == static_cast<int>(g.height)) {
            g.dpi = dpi;
            g.page = Size{static_cast<int>(g.width), static_cast<int>(g.height)};
            g.sx = g.sy = dpi / 25.4;
            return g;
        }
    }
    g.sx = g.width / w_mm;
    g.sy = g.height / h_mm;
    const double finest = std::ceil(std::max(g.sx, g.sy) * 25.4);
    limits::check_count(finest, limits::kResolution, "the raster's resolution");
    g.dpi = std::max(raster::kWorkingDpi, static_cast<int>(finest));
    g.page = Size{mm_to_px(w_mm, g.dpi), mm_to_px(h_mm, g.dpi)};
    limits::check_picture(g.page.width, g.page.height, "the page");
    return g;
}

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

Box intersection(const Box& a, const Box& b) {
    const Box out{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    return out.x1 > out.x0 && out.y1 > out.y0 ? out : Box{0, 0, 0, 0};
}

// Raster pixels [x0, x1) × [y0, y1) covering the points (raster pixels), widened by `margin` and kept on the raster.
Box covering(const std::vector<std::pair<double, double>>& points, int margin, std::uint32_t width, std::uint32_t height) {
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (const auto& [x, y] : points) {
        if (!std::isfinite(x) || !std::isfinite(y)) return Box{0, 0, static_cast<int>(width), static_cast<int>(height)};
        x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
    }
    const auto clamp = [](double v, std::uint32_t hi) { return static_cast<int>(std::clamp(v, 0.0, static_cast<double>(hi))); };
    return Box{clamp(std::floor(x0) - margin, width), clamp(std::floor(y0) - margin, height), clamp(std::ceil(x1) + margin, width),
               clamp(std::ceil(y1) + margin, height)};
}

// --- inks as values ----------------------------------------------------------------------------------------------------

// A raster's samples read and written as values 0..1 (each sample / its most: CMYK's ink, Lab's encoding), the alpha
// straight. Inks mix linearly in these values, as the page composites them (InkCanvas).
struct Inks {
    InkPixels p;
    unsigned n = 0, all = 0;
    double most = 255;
    explicit Inks(InkPixels pixels) : p(std::move(pixels)), n(core::colour_channels(p.space)), all(n + 1), most(p.max()) {}
    std::uint16_t* at(std::size_t i) { return p.samples.data() + i * all; }
    const std::uint16_t* at(std::size_t i) const { return p.samples.data() + i * all; }
    double alpha(std::size_t i) const { return at(i)[n] / most; }
    std::uint16_t sample(double v) const { return static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0, 1.0) * most)); }
    void clear(std::size_t i) {  // (no ink, transparent: as a blank raster holds it)
        std::uint16_t* s = at(i);
        for (unsigned c = 0; c < n; ++c) s[c] = 0;
        if (p.space == core::InkSpace::Lab) {
            s[0] = static_cast<std::uint16_t>(most);
            s[1] = s[2] = p.depth == 8 ? 128 : 32896;
        }
        s[n] = 0;
    }
    void set_alpha(std::size_t i, double a) {
        if (!(a > 0)) {
            clear(i);
            return;
        }
        at(i)[n] = sample(a);
    }
    std::string take() const { return core::encode_ink_raster(p); }
};

// `top` (values 0..1, its colours then its alpha) over the pixel, as a normal layer composites: where nothing is under
// it or it hides all, it is the pixel; else the inks mix (alpha over).
void lay(Inks& out, std::size_t i, const double* top, bool keep_alpha = false) {
    const double under = out.alpha(i);
    const double a = std::clamp(top[out.n], 0.0, 1.0) * (keep_alpha ? under : 1.0);
    if (!(a > 0)) return;
    std::uint16_t* s = out.at(i);
    if (a >= 1 || under == 0) {
        for (unsigned c = 0; c < out.n; ++c) s[c] = out.sample(top[c]);
        s[out.n] = out.sample(a);
        return;
    }
    const double alpha = a + under * (1 - a);
    for (unsigned c = 0; c < out.n; ++c) s[c] = out.sample((top[c] * a + (s[c] / out.most) * under * (1 - a)) / alpha);
    s[out.n] = out.sample(alpha);
}

// A pixel carried whole (its samples, its alpha × a coverage): its own samples when it hides all or nothing is under
// it; mixed as above otherwise.
void lay_whole(Inks& out, std::size_t i, const std::uint16_t* samples, std::uint16_t alpha) {
    if (alpha == 0) return;
    if (alpha == static_cast<std::uint16_t>(out.most) || out.at(i)[out.n] == 0) {
        std::copy_n(samples, out.n, out.at(i));
        out.at(i)[out.n] = alpha;
        return;
    }
    std::array<double, 5> top{};
    for (unsigned c = 0; c < out.n; ++c) top[c] = samples[c] / out.most;
    top[out.n] = alpha / out.most;
    lay(out, i, top.data());
}

// The pixels an area lifts from a raster: each its samples and its alpha × the area's coverage, read whole or resampled
// (premultiplied by the alpha).
class Lifted {
public:
    Lifted(const Inks& source, std::string inside, const Box& box)
        : source_(source), inside_(std::move(inside)), width_(static_cast<int>(source.p.width)), height_(static_cast<int>(source.p.height)),
          box_(box) {}
    // The lifted pixel's alpha at (x, y) as a sample (its alpha × the coverage), or 0 outside the area.
    std::uint16_t whole_alpha(int x, int y) const {
        if (x < box_.x0 || y < box_.y0 || x >= box_.x1 || y >= box_.y1) return 0;
        const std::size_t i = std::size_t(y) * width_ + x;
        const int m = static_cast<unsigned char>(inside_[i]);
        const std::uint16_t a = source_.at(i)[source_.n];
        return m == 255 ? a : static_cast<std::uint16_t>(std::lround(a * (m / 255.0)));
    }
    // Premultiplied values (colours × alpha, alpha) at (x, y).
    std::array<double, 5> premultiplied(int x, int y) const {
        std::array<double, 5> out{};
        const std::uint16_t a = whole_alpha(x, y);
        if (a == 0) return out;
        const std::uint16_t* s = source_.at(std::size_t(y) * width_ + x);
        const double alpha = a / source_.most;
        for (unsigned c = 0; c < source_.n; ++c) out[c] = s[c] / source_.most * alpha;
        out[source_.n] = alpha;
        return out;
    }
    // Lays the lifted pixels seen at (u, v) (raster pixels, a pixel spanning [k, k + 1)) over out's pixel i.
    void lay_at(Inks& out, std::size_t i, double u, double v, Resample resample) const {
        if (!std::isfinite(u) || !std::isfinite(v)) return;
        const unsigned n = source_.n;
        if (resample == Resample::Nearest) {
            if (u < -1 || v < -1 || u > width_ + 1 || v > height_ + 1) return;
            const int x = static_cast<int>(std::floor(u)), y = static_cast<int>(std::floor(v));
            if (const std::uint16_t a = whole_alpha(x, y)) lay_whole(out, i, source_.at(std::size_t(y) * width_ + x), a);
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
            if (const std::uint16_t a = whole_alpha(ix, iy)) lay_whole(out, i, source_.at(std::size_t(iy) * width_ + ix), a);
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
        std::array<double, 5> sum{};
        std::array<double, 4> lo{INFINITY, INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY, -INFINITY};
        for (int j = 0; j < 4; ++j) {
            if (wy[j] == 0) continue;
            for (int k = 0; k < 4; ++k) {
                if (wx[k] == 0) continue;
                const auto p = premultiplied(ix + k - 1, iy + j - 1);
                for (unsigned c = 0; c <= n; ++c) sum[c] += p[c] * wx[k] * wy[j];
                if (p[n] > 0) {
                    for (unsigned c = 0; c < n; ++c) { lo[c] = std::min(lo[c], p[c] / p[n]); hi[c] = std::max(hi[c], p[c] / p[n]); }
                }
            }
        }
        if (!(sum[n] > 1e-12)) return;
        // (a cubic's overshoot, or a value divided by almost no alpha, stays within the inks it was made of)
        std::array<double, 5> mixed{};
        for (unsigned c = 0; c < n; ++c) mixed[c] = std::clamp(sum[c] / sum[n], lo[c], hi[c]);
        mixed[n] = std::min(1.0, sum[n]);
        lay(out, i, mixed.data());
    }

private:
    const Inks& source_;
    std::string inside_;
    int width_, height_;
    Box box_;
};

// The area's pixels lose their alpha (what stays where they were); the area on the raster (its bytes) and its box.
std::optional<std::pair<std::string, Box>> leave(Inks& out, const core::Json& area, const Grid& g) {
    const Image inside = area_on_raster(area, g);
    const auto box = inside.getbbox();
    if (!box) return std::nullopt;
    std::string covered = inside.tobytes();
    for (int y = box->y0; y < box->y1; ++y) {
        for (int x = box->x0; x < box->x1; ++x) {
            const std::size_t i = std::size_t(y) * g.width + x;
            const int m = static_cast<unsigned char>(covered[i]);
            if (m == 0) continue;
            out.set_alpha(i, m == 255 ? 0.0 : out.alpha(i) * (1 - m / 255.0));
        }
    }
    return std::make_pair(std::move(covered), *box);
}

// A layer's marks drawn for its inks (lines, pictures) kept inside its own pixels when its alpha is locked (透明保護),
// as the page and the 8-bit bake clip them: their alpha times the layer's.
void inside_own_pixels(InkPixels& drawn, const core::Layer& layer) {
    if (!layer.lock_alpha || !layer.ink_raster) return;
    InkPixels own = core::decode_ink_raster(*layer.ink_raster);
    if (own.width != drawn.width || own.height != drawn.height) {
        own = resize_inks(own, Size{static_cast<int>(drawn.width), static_cast<int>(drawn.height)}, Resample::Bicubic);
    }
    const unsigned n = core::colour_channels(drawn.space), on = core::colour_channels(own.space);
    const double own_most = own.max();
    for (std::size_t i = 0; i < drawn.pixel_count(); ++i) {
        std::uint16_t& a = drawn.samples[i * (n + 1) + n];
        a = static_cast<std::uint16_t>(std::lround(a * (own.samples[i * (on + 1) + on] / own_most)));
    }
}

Inks layer_inks(const core::Layer& layer) {
    if (!layer.ink_raster) throw core::OpError("the layer " + layer.id + " holds no ink pixels");
    return Inks(core::decode_ink_raster(*layer.ink_raster));
}

// --- through the screen's picture -------------------------------------------------------------------------------------

using Shown = std::unordered_map<std::uint32_t, std::vector<std::uint16_t>>;

std::uint32_t key(const unsigned char* p) { return (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2]; }

std::uint16_t alpha_sample(unsigned char a, int depth) { return depth == 16 ? static_cast<std::uint16_t>(a * 257) : a; }

// The colours the lent layers showed: each colour's first inks (its colour samples).
Shown own_colours(const std::vector<Lending::Loan>& loans) {
    Shown out;
    for (const Lending::Loan& loan : loans) {
        const InkPixels& p = loan.pixels;
        const unsigned n = core::colour_channels(p.space), all = n + 1;
        const auto* shown = reinterpret_cast<const unsigned char*>(loan.shown.data());
        for (std::size_t i = 0; i < p.pixel_count(); ++i) {
            if (p.samples[i * all + n] == 0) continue;
            out.try_emplace(key(shown + i * 4), p.samples.begin() + std::ptrdiff_t(i * all), p.samples.begin() + std::ptrdiff_t(i * all + n));
        }
    }
    return out;
}

// The colours the rules show: the table's inks, the greys' (Genko's blacks among them, white the paper).
Shown ruled_colours(const PageColor& color) {
    std::vector<std::vector<double>> inks;
    for (const core::InkEntry& entry : color.inks) inks.push_back(entry.ink);
    for (int v = 0; v <= 255; ++v) {
        const unsigned char grey[3] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v), static_cast<unsigned char>(v)};
        std::vector<double> ink(core::colour_channels(color.space));
        ink::inks_of(color, grey, nullptr, ink.data(), 1);
        inks.push_back(std::move(ink));
    }
    InkPixels row = core::blank_ink_pixels(color.space, color.depth, static_cast<std::uint32_t>(inks.size()), 1);
    const unsigned n = core::colour_channels(color.space), all = n + 1;
    for (std::size_t i = 0; i < inks.size(); ++i) {
        for (unsigned c = 0; c < n; ++c) row.samples[i * all + c] = ink::encode_sample(color.space, c, inks[i][c], color.depth);
        row.samples[i * all + n] = static_cast<std::uint16_t>(row.max());
    }
    const std::string shown = ink::to_rgba8(row, color).tobytes();
    Shown out;
    for (std::size_t i = 0; i < inks.size(); ++i) {
        out.try_emplace(key(reinterpret_cast<const unsigned char*>(shown.data()) + i * 4), row.samples.begin() + std::ptrdiff_t(i * all),
                        row.samples.begin() + std::ptrdiff_t(i * all + n));
    }
    return out;
}

// The nearest colour shown within 2 a channel (what straight alpha's rounding leaves where only the alpha changed).
const std::vector<std::uint16_t>* near_shown(const Shown& shown, const unsigned char* p) {
    if (shown.empty()) return nullptr;
    const std::vector<std::uint16_t>* best = nullptr;
    int far = 7;  // (more than 3 × 2)
    for (int dr = -2; dr <= 2; ++dr) {
        for (int dg = -2; dg <= 2; ++dg) {
            for (int db = -2; db <= 2; ++db) {
                const int r = p[0] + dr, g = p[1] + dg, b = p[2] + db;
                const int d = std::abs(dr) + std::abs(dg) + std::abs(db);
                if (r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255 || d >= far) continue;
                const unsigned char c[3] = {static_cast<unsigned char>(r), static_cast<unsigned char>(g), static_cast<unsigned char>(b)};
                if (const auto it = shown.find(key(c)); it != shown.end()) {
                    best = &it->second;
                    far = d;
                }
            }
        }
    }
    return best;
}

// An op's picture (RGBA) back in inks (see the header for the order).
InkPixels into_inks(const Image& picture, const PageColor& color, const Lending::Loan* loan, const Shown& own, const Shown& ruled) {
    const auto w = static_cast<std::uint32_t>(picture.width()), h = static_cast<std::uint32_t>(picture.height());
    InkPixels out = core::blank_ink_pixels(color.space, color.depth, w, h);
    const unsigned n = core::colour_channels(color.space), all = n + 1;
    const bool same = loan != nullptr && loan->pixels.width == w && loan->pixels.height == h;
    const auto* lent = same ? reinterpret_cast<const unsigned char*>(loan->shown.data()) : nullptr;
    std::vector<unsigned char> rgb;
    std::vector<std::size_t> where;
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(picture.raw()->image[y]);
        for (std::uint32_t x = 0; x < w; ++x) {
            const unsigned char* p = row + std::size_t(x) * 4;
            if (p[3] == 0) continue;  // (transparent: no ink)
            const std::size_t i = std::size_t(y) * w + x;
            std::uint16_t* dst = out.samples.data() + i * all;
            if (lent != nullptr) {
                // (its colour as it was, or within 2 a channel of it — the rounding of a mix that barely reached it, or
                // of straight alpha where only the alpha changed —: its own inks)
                const unsigned char* was = lent + i * 4;
                if (was[3] != 0 && std::abs(was[0] - p[0]) <= 2 && std::abs(was[1] - p[1]) <= 2 && std::abs(was[2] - p[2]) <= 2) {
                    const std::uint16_t* inks = loan->pixels.samples.data() + i * all;
                    std::copy_n(inks, n, dst);
                    dst[n] = was[3] == p[3] ? inks[n] : alpha_sample(p[3], color.depth);
                    continue;
                }
            }
            dst[n] = alpha_sample(p[3], color.depth);
            const std::array<int, 3> c{p[0], p[1], p[2]};
            const bool grey = p[0] == p[1] && p[1] == p[2];
            const bool genko = grey && (p[0] == 0 || p[0] == 10 || p[0] == 15 || p[0] == 20 || p[0] == 255);
            const std::vector<std::uint16_t>* inks = nullptr;
            if (core::ink_for(color, c) == nullptr) {  // (the table's colour: its inks, by the rules below)
                if (const auto it = own.find(key(p)); it != own.end()) {
                    inks = &it->second;
                } else if (!genko) {
                    if (const auto ruled_it = ruled.find(key(p)); ruled_it != ruled.end()) inks = &ruled_it->second;
                    else if (!grey && (inks = near_shown(own, p)) == nullptr) inks = near_shown(ruled, p);
                }
            }
            if (inks != nullptr) {
                std::copy(inks->begin(), inks->end(), dst);
                continue;
            }
            rgb.insert(rgb.end(), p, p + 3);
            where.push_back(i);
        }
    }
    if (!where.empty()) {  // (the rules: the table, Genko's blacks and white, greys, the profile)
        std::vector<double> inks(where.size() * n);
        ink::inks_of(color, rgb.data(), nullptr, inks.data(), where.size());
        for (std::size_t k = 0; k < where.size(); ++k) {
            std::uint16_t* dst = out.samples.data() + where[k] * all;
            for (unsigned c = 0; c < n; ++c) dst[c] = ink::encode_sample(color.space, c, inks[k * n + c], color.depth);
        }
    }
    return out;
}

// The layer an op names to work on, as it finds it: layer_id (filter_raster: id; flood_fill looks at the role alone),
// else the role in "layer" (ink by default).
const core::Layer* target_of(const core::Page& page, const core::Json& args, std::string_view op) {
    const char* key = op == "filter_raster" ? "id" : "layer_id";
    if (op != "flood_fill" && core::truthy_at(args, key)) {
        const std::string id = core::py_str(args[key]);
        for (const core::Layer& layer : page.layers) {
            if (layer.id == id) return &layer;
        }
        return nullptr;
    }
    std::string role = "ink";
    if (core::truthy_at(args, "layer")) role = core::py_str(args["layer"]);
    for (const core::Layer& layer : page.layers) {
        if (core::to_string(layer.role) == role) return &layer;
    }
    return nullptr;
}

}  // namespace

// --- the inks themselves ---------------------------------------------------------------------------------------------

std::string erase(const core::Page& page, const core::Layer& layer, const core::PenPoints& points, double width_mm,
                  std::string_view texture, const std::string& seed) {
    Inks out = layer_inks(layer);
    const Grid g = grid_of(page, out.p.width, out.p.height);
    const bool hard = texture != "soft" && texture != "rough";
    const std::string taken = on_raster(raster::eraser_mask(g.page, points, width_mm, g.dpi, hard ? "" : texture, seed), g).tobytes();
    for (std::size_t i = 0; i < taken.size(); ++i) {
        const int m = static_cast<unsigned char>(taken[i]);
        if (m == 0) continue;
        if (hard && m == 255) {
            out.clear(i);  // (as the 8-bit eraser stamps a clear pixel)
            continue;
        }
        out.set_alpha(i, std::max(0.0, out.alpha(i) - m / 255.0));
    }
    return out.take();
}

std::string delete_area(const core::Page& page, const core::Layer& layer, const core::Json& area) {
    Inks out = layer_inks(layer);
    (void)leave(out, area, grid_of(page, out.p.width, out.p.height));
    return out.take();
}

std::string move_area(const core::Page& page, const core::Layer& layer, const core::Json& area, const selection::Matrix& m, Resample resample) {
    const Inks source = layer_inks(layer);
    const Grid g = grid_of(page, source.p.width, source.p.height);
    const selection::Matrix inverse = selection::invert(m);
    Inks out = source;
    auto left = leave(out, area, g);
    if (!left) return out.take();
    const Box box = left->second;
    const Lifted lifted(source, std::move(left->first), box);
    std::vector<std::pair<double, double>> corners;
    for (const auto& [x, y] : {std::pair{box.x0, box.y0}, std::pair{box.x1, box.y0}, std::pair{box.x1, box.y1}, std::pair{box.x0, box.y1}}) {
        const double xm = x / g.sx, ym = y / g.sy;
        corners.emplace_back((m[0] * xm + m[2] * ym + m[4]) * g.sx, (m[1] * xm + m[3] * ym + m[5]) * g.sy);
    }
    const Box to = covering(corners, 3, g.width, g.height);
    for (int y = to.y0; y < to.y1; ++y) {
        for (int x = to.x0; x < to.x1; ++x) {
            const double xm = (x + .5) / g.sx, ym = (y + .5) / g.sy;
            const double u = (inverse[0] * xm + inverse[2] * ym + inverse[4]) * g.sx;
            const double v = (inverse[1] * xm + inverse[3] * ym + inverse[5]) * g.sy;
            lifted.lay_at(out, std::size_t(y) * g.width + x, u, v, resample);
        }
    }
    return out.take();
}

std::string warp_area(const core::Page& page, const core::Layer& layer, const core::Json& area, const warp::Go& go, Resample resample) {
    const Inks source = layer_inks(layer);
    const Grid g = grid_of(page, source.p.width, source.p.height);
    Inks out = source;
    auto left = leave(out, area, g);
    if (!left) return out.take();
    const Box box = left->second;
    const Lifted lifted(source, std::move(left->first), box);
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
    const Box reach = covering(all, 3, g.width, g.height);
    if (reach.width() <= 0 || reach.height() <= 0) return out.take();
    std::vector<std::array<P, 3>> sources, places;
    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            sources.push_back({from[j][i], from[j][i + 1], from[j + 1][i + 1]});
            places.push_back({to[j][i], to[j][i + 1], to[j + 1][i + 1]});
            sources.push_back({from[j][i], from[j + 1][i + 1], from[j + 1][i]});
            places.push_back({to[j][i], to[j + 1][i + 1], to[j + 1][i]});
        }
    }
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
        const Box part = intersection(covering({places[t][0], places[t][1], places[t][2]}, 1, g.width, g.height), reach);
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
    return out.take();
}

std::optional<Piece> lift_piece(const core::Page& page, const core::Layer& layer, const core::Json& area) {
    if (!page.color || !layer.ink_raster) return std::nullopt;
    const Inks source = layer_inks(layer);
    const Grid g = grid_of(page, source.p.width, source.p.height);
    const std::string inside = area_on_raster(area, g).tobytes();
    const Lifted lifted(source, inside, Box{0, 0, static_cast<int>(g.width), static_cast<int>(g.height)});
    int x0 = static_cast<int>(g.width), y0 = static_cast<int>(g.height), x1 = 0, y1 = 0;
    for (int y = 0; y < static_cast<int>(g.height); ++y) {
        for (int x = 0; x < static_cast<int>(g.width); ++x) {
            if (lifted.whole_alpha(x, y) == 0) continue;
            x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1);
        }
    }
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    Piece piece;
    piece.pixels = core::blank_ink_pixels(source.p.space, source.p.depth, static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::uint16_t a = lifted.whole_alpha(x, y);
            if (a == 0) continue;
            std::uint16_t* dst = piece.pixels.samples.data() + (std::size_t(y - y0) * piece.pixels.width + (x - x0)) * source.all;
            std::copy_n(source.at(std::size_t(y) * g.width + x), source.n, dst);
            dst[source.n] = a;
        }
    }
    piece.box_mm = {x0 / g.sx, y0 / g.sy, (x1 - x0) / g.sx, (y1 - y0) / g.sy};
    return piece;
}

core::Patch piece_patch(const Piece& piece, const core::PageColor& color) {
    core::Patch patch;
    patch.attrs = core::Json{{"mode", "image"},
                             {"opacity", 1.0},
                             {"box", core::Json::array({piece.box_mm[0], piece.box_mm[1], piece.box_mm[2], piece.box_mm[3]})},
                             {"gkir", core::b64encode(core::encode_ink_raster(piece.pixels))},
                             {"gkir_profile", color.profile}};
    patch.png = std::make_shared<const std::string>(write_png(ink::to_rgba8(piece.pixels, color)));
    return patch;
}

std::optional<core::InkPixels> patch_inks(const core::Patch& patch, const core::PageColor& color) {
    const core::Json* gkir = core::get(patch.attrs, "gkir");
    const core::Json* profile = core::get(patch.attrs, "gkir_profile");
    if (gkir == nullptr || !gkir->is_string() || profile == nullptr || !profile->is_string() || profile->get<std::string>() != color.profile) {
        return std::nullopt;  // (inks of another profile: drawn as their picture)
    }
    try {
        core::InkPixels pixels = core::decode_ink_raster(core::a2b_base64(gkir->get<std::string>()));
        if (pixels.space != color.space || pixels.depth != color.depth || pixels.pixel_count() == 0) return std::nullopt;
        return pixels;
    } catch (const core::Error&) {  // (not a whole raster: drawn as its picture)
        return std::nullopt;
    }
}

std::string lay_piece(const core::Page& page, const core::Layer& layer, const core::InkPixels& piece, const std::array<double, 4>& box_mm) {
    if (!page.color) throw core::OpError("an RGB page holds no ink pixels");
    Inks out = layer.ink_raster ? layer_inks(layer)
                                : Inks(core::blank_ink_pixels(page.color->space, page.color->depth,
                                                              static_cast<std::uint32_t>(mm_to_px(page.spec.width_mm.value(), raster::kWorkingDpi)),
                                                              static_cast<std::uint32_t>(mm_to_px(page.spec.height_mm.value(), raster::kWorkingDpi))));
    if (piece.space != out.p.space || piece.depth != out.p.depth) throw core::OpError("the piece of inks is not in the page's colour mode");
    const Grid g = grid_of(page, out.p.width, out.p.height);
    const Inks source(piece);
    const double bx = box_mm[0] * g.sx, by = box_mm[1] * g.sy, bw = box_mm[2] * g.sx, bh = box_mm[3] * g.sy;
    if (!std::isfinite(bx) || !std::isfinite(by) || !(bw > 0) || !(bh > 0)) return out.take();
    const auto whole = [](double v) { return std::abs(v - std::round(v)) < 1e-3; };
    if (std::abs(bw - piece.width) < 1e-3 && std::abs(bh - piece.height) < 1e-3 && whole(bx) && whole(by)) {
        // (the box spans the piece's own pixels: each laid whole)
        const long ox = std::lround(bx), oy = std::lround(by);
        for (std::uint32_t y = 0; y < piece.height; ++y) {
            const long ty = oy + y;
            if (ty < 0 || ty >= static_cast<long>(g.height)) continue;
            for (std::uint32_t x = 0; x < piece.width; ++x) {
                const long tx = ox + x;
                if (tx < 0 || tx >= static_cast<long>(g.width)) continue;
                const std::uint16_t* s = source.at(std::size_t(y) * piece.width + x);
                lay_whole(out, std::size_t(ty) * g.width + std::size_t(tx), s, s[source.n]);
            }
        }
        return out.take();
    }
    const Lifted lifted(source, std::string(piece.pixel_count(), '\xff'), Box{0, 0, static_cast<int>(piece.width), static_cast<int>(piece.height)});
    const Box to = covering({{bx, by}, {bx + bw, by + bh}}, 1, g.width, g.height);
    for (int y = to.y0; y < to.y1; ++y) {
        for (int x = to.x0; x < to.x1; ++x) {
            const double u = ((x + .5) - bx) / bw * piece.width, v = ((y + .5) - by) / bh * piece.height;
            if (u < 0 || v < 0 || u > piece.width || v > piece.height) continue;
            lifted.lay_at(out, std::size_t(y) * g.width + x, u, v, Resample::Bilinear);
        }
    }
    return out.take();
}

std::string relayout_inks(const std::string& gkir, const core::PageSpec& spec, const core::Rect& old_frame, const core::Rect& new_frame) {
    const Inks source(core::decode_ink_raster(gkir));
    const double k = raster::kWorkingDpi / 25.4;
    const int width = mm_to_px(spec.width_mm.value(), raster::kWorkingDpi);
    const int height = mm_to_px(spec.height_mm.value(), raster::kWorkingDpi);
    limits::check_picture(width, height, "the paint layer on the new paper");
    // (as relayout_raster: output pixel (u, v) takes input ((u/k - nx)/sx + ox) * k, …)
    const double ow = old_frame.width.value(), oh = old_frame.height.value();
    if (!(ow != 0) || !(oh != 0)) throw core::OpError("the basic frame must be a finite size");
    const double sx = new_frame.width.value() / ow, sy = new_frame.height.value() / oh;
    if (!(sx != 0) || !(sy != 0)) throw core::OpError("the basic frame must be a finite size");
    const std::array<double, 4> c{1 / sx, (old_frame.x.value() - new_frame.x.value() / sx) * k, 1 / sy,
                                  (old_frame.y.value() - new_frame.y.value() / sy) * k};
    for (const double v : c) {
        if (!std::isfinite(v)) throw core::OpError("the basic frame must be a finite size");
    }
    Inks out(core::blank_ink_pixels(source.p.space, source.p.depth, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)));
    const Lifted lifted(source, std::string(source.p.pixel_count(), '\xff'),
                        Box{0, 0, static_cast<int>(source.p.width), static_cast<int>(source.p.height)});
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            lifted.lay_at(out, std::size_t(y) * width + x, c[0] * (x + .5) + c[1], c[2] * (y + .5) + c[3], Resample::Bilinear);
        }
    }
    return out.take();
}

InkPixels with_marks(const core::Page& page, const core::Layer& layer, const core::Document* episode) {
    if (!page.color) throw core::OpError("an RGB page holds no ink pixels");
    if (layer.stroke_count() == 0 && layer.patches.empty()) {
        if (layer.ink_raster) return core::decode_ink_raster(*layer.ink_raster);
        return core::blank_ink_pixels(page.color->space, page.color->depth,
                                      static_cast<std::uint32_t>(mm_to_px(page.spec.width_mm.value(), raster::kWorkingDpi)),
                                      static_cast<std::uint32_t>(mm_to_px(page.spec.height_mm.value(), raster::kWorkingDpi)));
    }
    core::Layer marks;  // (its lines and pictures as the layer draws them, before its own settings)
    marks.id = layer.id;
    marks.role = core::LayerRole::User;
    marks.kind = core::LayerKind::Raster;
    marks.panel_clip = layer.panel_clip;
    marks.panel_each = layer.panel_each;
    marks.strokes = layer.strokes;
    marks.patches = layer.patches;
    InkPixels drawn = composite_ink_layers(page, {marks}, raster::kWorkingDpi, episode);
    inside_own_pixels(drawn, layer);
    return core::decode_ink_raster(lay_piece(page, layer, drawn, {0.0, 0.0, page.spec.width_mm.value(), page.spec.height_mm.value()}));
}

std::string area_coverage(const core::Page& page, std::uint32_t width, std::uint32_t height, const core::Json& area) {
    return area_on_raster(area, grid_of(page, width, height)).tobytes();
}

double raster_scale(const core::Page& page, std::uint32_t width, std::uint32_t height) {
    const Grid g = grid_of(page, width, height);
    return std::sqrt(g.sx * g.sy) / (raster::kWorkingDpi / 25.4);
}

void bake_marks(core::Document& doc) {
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        if (doc.is_deferred(p) || !doc.page(p).color) continue;
        const core::Page& seen = doc.page(p);
        for (std::size_t l = 0; l < seen.layers.size(); ++l) {
            const core::Layer& layer = seen.layers[l];
            if (layer.kind != core::LayerKind::Raster || layer.patches.empty()) continue;
            core::Layer marks;  // (the pictures alone, as the layer draws them before its own settings)
            marks.id = layer.id;
            marks.role = core::LayerRole::User;
            marks.kind = core::LayerKind::Raster;
            marks.panel_clip = marks.panel_each = false;
            marks.patches = layer.patches;
            core::InkPixels drawn = composite_ink_layers(seen, {marks}, raster::kWorkingDpi, &doc);
            inside_own_pixels(drawn, layer);
            core::Page& page = doc.edit_page(p);
            core::Layer& target = page.layers[l];
            const std::array<double, 4> whole{0.0, 0.0, page.spec.width_mm.value(), page.spec.height_mm.value()};
            target.ink_raster = std::make_shared<const std::string>(lay_piece(page, target, drawn, whole));
            target.ink_memo = {};
            target.patches.clear();
        }
    }
}

// --- the eyedropper ----------------------------------------------------------------------------------------------------

std::optional<std::vector<double>> inks_at(const core::Page& page, const core::Document* episode, double x_mm, double y_mm, int dpi) {
    if (!page.color || !std::isfinite(x_mm) || !std::isfinite(y_mm)) return std::nullopt;
    const int x = static_cast<int>(std::floor(x_mm / 25.4 * dpi)), y = static_cast<int>(std::floor(y_mm / 25.4 * dpi));
    if (x < 0 || y < 0 || x >= mm_to_px(page.spec.width_mm.value(), dpi) || y >= mm_to_px(page.spec.height_mm.value(), dpi)) return std::nullopt;
    RenderOptions options = proof_options();
    options.inks = true;
    options.skip_unported = true;
    options.region = RenderRegion{x, y, 1, 1};
    const RenderResult made = render_page(page, dpi, options, episode);
    if (!made.inks || made.inks->pixel_count() == 0) return std::nullopt;
    const unsigned n = core::colour_channels(made.inks->space);
    std::vector<double> out(n);
    for (unsigned c = 0; c < n; ++c) out[c] = ink::decode_sample(made.inks->space, c, made.inks->samples[c], made.inks->depth);
    return out;
}

std::optional<std::vector<double>> layer_inks_at(const core::Page& page, const core::Layer& layer, double x_mm, double y_mm) {
    if (!page.color || !layer.ink_raster || !std::isfinite(x_mm) || !std::isfinite(y_mm)) return std::nullopt;
    const double w_mm = page.spec.width_mm.value(), h_mm = page.spec.height_mm.value();
    if (!(w_mm > 0) || !(h_mm > 0) || x_mm < 0 || y_mm < 0 || x_mm >= w_mm || y_mm >= h_mm) return std::nullopt;
    const std::shared_ptr<const InkPixels> pixels = decoded_ink(layer.ink_raster);
    if (!pixels || pixels->pixel_count() == 0) return std::nullopt;
    // (the raster lies stretched over the page)
    const auto x = std::min<std::uint32_t>(pixels->width - 1, static_cast<std::uint32_t>(x_mm / w_mm * pixels->width));
    const auto y = std::min<std::uint32_t>(pixels->height - 1, static_cast<std::uint32_t>(y_mm / h_mm * pixels->height));
    const unsigned n = core::colour_channels(pixels->space), all = n + 1;
    const std::uint16_t* p = pixels->samples.data() + (std::size_t(y) * pixels->width + x) * all;
    if (p[n] * 255.0 <= 20.0 * pixels->max()) return std::nullopt;
    std::vector<double> out(n);
    for (unsigned c = 0; c < n; ++c) out[c] = ink::decode_sample(pixels->space, c, p[c], pixels->depth);
    return out;
}

// --- through the screen's picture ------------------------------------------------------------------------------------

namespace {

// The ops that composite several layers (or make a layer of their own): every ink layer of the page lent.
constexpr std::array<std::string_view, 8> kPageWide{"merge_down", "merge_layers", "merge_visible", "convert_layer",
                                                    "lt_convert", "put_color_raster", "render_prims", "trace_prims"};

std::optional<std::string> text_at(const core::Json& args, const char* key) {
    const core::Json* v = core::get(args, key);
    if (v == nullptr || !v->is_string() || v->get<std::string>().empty()) return std::nullopt;
    return v->get<std::string>();
}

bool truthy_or(const core::Json& args, const char* key, bool fallback) {
    const core::Json* v = core::get(args, key);
    return v == nullptr ? fallback : core::py_truthy(*v);
}

// A layer as a merge takes the one under the others: itself only (its opacity, blend and clipping stay the layer's).
core::Layer as_ground(core::Layer layer) {
    layer.opacity = 1;
    layer.blend = "normal";
    layer.clip = false;
    return layer;
}

// Are a layer and the folders it is in shown (layerops._parents_visible)?
bool shown_in_page(const core::Page& page, const core::Layer& layer) {
    std::vector<std::string> seen;
    const core::Json* parent = &layer.parent_id;
    while (parent->is_string()) {
        const std::string id = parent->get<std::string>();
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) break;
        seen.push_back(id);
        const core::Layer* found = nullptr;
        for (const core::Layer& item : page.layers) {
            if (item.id == id) found = &item;
        }
        if (found == nullptr) break;
        if (!found->visible) return false;
        parent = &found->parent_id;
    }
    return true;
}

}  // namespace

bool lends(std::string_view op, const core::Json& args) {
    if (op == "filter_raster") return core::truthy_at(args, "kind") && core::py_str(args["kind"]).starts_with("plugin:");
    static constexpr std::array<std::string_view, 7> kOnTarget{"smudge", "liquify", "add_shape", "flood_fill", "gradient_fill",
                                                              "fill_gaps", "ruler_to_layer"};
    return std::find(kPageWide.begin(), kPageWide.end(), op) != kPageWide.end() ||
           std::find(kOnTarget.begin(), kOnTarget.end(), op) != kOnTarget.end();
}

Lending::Lending(core::Document& doc, std::size_t at, std::string_view op, const core::Json& args) {
    if (at >= doc.pages.size() || doc.is_deferred(at) || !doc.page(at).color) return;
    page_id_ = doc.page(at).id;
    color_ = *doc.page(at).color;
    had_color_raster_feature_ = std::find(doc.features.begin(), doc.features.end(), core::kColorRasterFeature) != doc.features.end();
    // a merge: what it composites, made again in inks after it (the 8-bit op makes the layers' order and settings)
    {
        const core::Page& page = doc.page(at);
        const auto by_id = [&](const std::string& id) -> const core::Layer* {
            for (const core::Layer& layer : page.layers) {
                if (layer.id == id) return &layer;
            }
            return nullptr;
        };
        Merge merge;
        if (op == "merge_down") {
            if (const auto id = text_at(args, "id"); id && by_id(*id) != nullptr) {
                const core::Layer& upper = *by_id(*id);
                const core::Layer* lower = nullptr;
                for (const core::Layer& layer : page.layers) {
                    if (&layer == &upper) break;
                    if (layer.parent_id == upper.parent_id && layer.kind != core::LayerKind::Folder) lower = &layer;
                }
                if (lower != nullptr) {
                    merge.target = lower->id;
                    merge.layers = {as_ground(*lower), upper};
                }
            }
        } else if (op == "merge_layers") {
            if (const core::Json* given = core::get(args, "ids"); given != nullptr && given->is_array()) {
                for (const core::Layer& layer : page.layers) {
                    const bool named = std::any_of(given->begin(), given->end(), [&](const core::Json& v) { return v.is_string() && v.get<std::string>() == layer.id; });
                    if (named && layer.kind != core::LayerKind::Folder) merge.layers.push_back(layer);
                }
                if (!merge.layers.empty()) merge.target = merge.layers.front().id;
            }
        } else if (op == "merge_visible") {
            for (const core::Layer& layer : page.layers) {
                if (layer.visible && layer.kind != core::LayerKind::Folder && layer.role != core::LayerRole::Name &&
                    layer.role != core::LayerRole::Draft && shown_in_page(page, layer)) {
                    merge.layers.push_back(layer);
                }
            }
            merge.paper = truthy_or(args, "flatten", false);
            if (!merge.layers.empty()) {
                if (!truthy_or(args, "copy", true)) merge.target = merge.layers.front().id;  // (else the layer it makes)
                else if (const auto id = text_at(args, "id")) merge.target = *id;
            }
        } else if (op == "convert_layer") {
            const auto id = text_at(args, "id");
            const auto to = text_at(args, "to");
            if (id && to && *to == "paint" && by_id(*id) != nullptr) {
                merge.target = *id;
                merge.layers = {as_ground(*by_id(*id))};
            }
        }
        if (!merge.layers.empty()) {
            merge_ = std::move(merge);
            before_ = doc.pages[at];
            for (const core::Layer& layer : page.layers) ids_before_.push_back(layer.id);
        }
    }
    std::vector<std::string> ids;
    if (std::find(kPageWide.begin(), kPageWide.end(), op) != kPageWide.end()) {
        for (const core::Layer& layer : doc.page(at).layers) {
            if (layer.ink_raster) ids.push_back(layer.id);
        }
    } else if (const core::Layer* target = target_of(doc.page(at), args, op); target != nullptr && target->ink_raster) {
        ids.push_back(target->id);
    }
    if (ids.empty()) return;
    core::Page& page = doc.edit_page(at);
    for (core::Layer& layer : page.layers) {
        if (!layer.ink_raster || std::find(ids.begin(), ids.end(), layer.id) == ids.end()) continue;
        Loan loan;
        loan.layer = layer.id;
        loan.ink_raster = layer.ink_raster;
        loan.pixels = core::decode_ink_raster(*layer.ink_raster);
        const Image shown = ink::to_rgba8(loan.pixels, color_);
        loan.shown = shown.tobytes();
        loan.lent = std::make_shared<const std::string>(write_png(shown, 1));  // (lent for one op: written fast)
        loan.patches = layer.patches.size();
        layer.raster_png = loan.lent;
        layer.raster_memo = {};
        layer.raster_relpath.reset();
        layer.ink_raster = nullptr;
        layer.ink_memo = {};
        loans_.push_back(std::move(loan));
    }
}

void Lending::take_back(core::Document& doc) const {
    if (page_id_.empty()) return;
    std::size_t at = doc.pages.size();
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (!doc.is_deferred(i) && doc.page(i).id == page_id_) at = i;
    }
    if (at == doc.pages.size() || !doc.page(at).color) return;
    const auto& layers = doc.page(at).layers;
    const bool left = std::any_of(layers.begin(), layers.end(), [](const core::Layer& l) { return (l.raster_png && !l.raster_png->empty()) || l.color_raster; });
    if (left) {
        const Shown own = own_colours(loans_);
        const Shown ruled = ruled_colours(color_);
        core::Page& page = doc.edit_page(at);
        const PageColor& color = *page.color;
        for (core::Layer& layer : page.layers) {
            std::optional<InkPixels> made;
            const bool has_pixels = (layer.raster_png && !layer.raster_png->empty()) || layer.color_raster;
            if (merge_ && before_ && has_pixels &&
                (merge_->target ? layer.id == *merge_->target
                                : std::find(ids_before_.begin(), ids_before_.end(), layer.id) == ids_before_.end())) {
                // (the merge's layer — 8-bit or precise, as the op composited it —: what it composited, made again in
                // inks; precise lines by the rules, as the page draws them)
                InkPixels merged = composite_ink_layers(*before_, merge_->layers, raster::kWorkingDpi, &doc, merge_->paper);
                layer.ink_raster = nullptr;
                made = std::move(merged);
                layer.color_raster = nullptr;
                layer.raster_png = nullptr;
                layer.raster_memo = {};
                layer.raster_relpath.reset();
            } else if (layer.color_raster) {  // (precise RGB pixels: through the profile)
                const core::ColorRasterView view(*layer.color_raster);
                std::vector<float> rgba(std::size_t(view.width()) * view.height() * 4);
                for (std::size_t i = 0; i < std::size_t(view.width()) * view.height(); ++i) {
                    const auto px = view.pixel(i);
                    for (unsigned c = 0; c < 4; ++c) rgba[i * 4 + c] = static_cast<float>(px[c]);
                }
                made = ink::from_srgb(rgba, view.width(), view.height(), color);
                layer.color_raster = nullptr;
            } else if (layer.raster_png && !layer.raster_png->empty()) {
                const auto loan = std::find_if(loans_.begin(), loans_.end(), [&](const Loan& l) { return l.layer == layer.id; });
                const Loan* lent = loan != loans_.end() ? &*loan : nullptr;
                if (lent != nullptr && layer.kind == core::LayerKind::Raster && layer.patches.size() > lent->patches) {
                    // (pictures the op laid on the picture it was lent — smudge's, a shape's —: drawn into that picture,
                    // as the 8-bit tools draw them when they work its pixels, so what they did not change keeps its inks)
                    core::Layer drawn = layer;
                    drawn.strokes = core::empty_strokes();
                    drawn.patches.assign(layer.patches.begin() + std::ptrdiff_t(lent->patches), layer.patches.end());
                    raster::bake_vectors(page, drawn);
                    layer.raster_png = drawn.raster_png;
                    layer.patches.resize(lent->patches);
                }
                if (lent != nullptr && (layer.raster_png == lent->lent || *layer.raster_png == *lent->lent)) {
                    layer.ink_raster = lent->ink_raster;  // (as it was lent: its own bytes)
                } else {
                    made = into_inks(open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA"), color, lent, own, ruled);
                }
                layer.raster_png = nullptr;
                layer.raster_memo = {};
                layer.raster_relpath.reset();
            } else {
                continue;
            }
            if (made) {
                if (layer.ink_raster) {  // (inks the op did not see — not lent —: what it drew goes over them)
                    layer.ink_raster = std::make_shared<const std::string>(
                        lay_piece(page, layer, *made, {0.0, 0.0, page.spec.width_mm.value(), page.spec.height_mm.value()}));
                } else {
                    layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(*made));
                }
            }
            layer.ink_memo = {};
        }
    }
    // (put_color_raster's precise pixels, now inks: the book holds none of its own unless it did before)
    if (!had_color_raster_feature_) {
        bool held = false;
        for (std::size_t i = 0; i < doc.pages.size() && !held; ++i) {
            if (doc.is_deferred(i)) continue;
            for (const core::Layer& layer : doc.page(i).layers) held = held || layer.color_raster != nullptr || core::has_color_strokes(layer);
        }
        if (!held) std::erase(doc.features, std::string(core::kColorRasterFeature));
    }
}

}  // namespace genko::render::ink_edit
