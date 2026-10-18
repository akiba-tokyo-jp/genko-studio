#include "render/layer_masks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "core/channels.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "render/fill.hpp"
#include "render/op_limits.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"

// (the lending of a mask to a pixel op: render/mask_lending.cpp)

namespace genko::render::layer_masks {

namespace {

unsigned char byte_of(double v) { return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

// A Gaussian of `sigma` on one plane as three box passes along the rows, then three along the columns (Kovesi's radii,
// the edges repeated, the sums in double): as render/image_ops' gauss_blur takes it, for one plane.
void gauss_blur(std::vector<float>& v, std::size_t w, std::size_t h, double sigma) {
    const double n = 3.0;
    const double ideal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
    int wl = static_cast<int>(std::floor(ideal));
    if (wl % 2 == 0) wl -= 1;
    wl = std::max(wl, 1);
    const int wu = wl + 2;
    const double m_value = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0);
    const auto m = static_cast<int>(std::clamp(std::round(m_value), 0.0, n));
    std::array<std::size_t, 3> radii{};
    for (int i = 0; i < 3; ++i) radii[std::size_t(i)] = static_cast<std::size_t>(i < m ? (wl - 1) / 2 : (wu - 1) / 2);
    std::vector<float> line(std::max(w, h));
    const auto pass = [](float* base, std::size_t count, std::size_t stride, std::size_t r, std::vector<float>& tmp) {
        if (r == 0 || count == 0) return;
        for (std::size_t i = 0; i < count; ++i) tmp[i] = base[i * stride];
        const double win = static_cast<double>(2 * r + 1);
        const auto at = [&](long long i) { return static_cast<double>(tmp[static_cast<std::size_t>(std::clamp<long long>(i, 0, static_cast<long long>(count) - 1))]); };
        double sum = 0.0;
        for (long long k = -static_cast<long long>(r); k <= static_cast<long long>(r); ++k) sum += at(k);
        for (std::size_t i = 0; i < count; ++i) {
            base[i * stride] = static_cast<float>(sum / win);
            sum += at(static_cast<long long>(i + r + 1)) - at(static_cast<long long>(i) - static_cast<long long>(r));
        }
    };
    for (const std::size_t r : radii)
        for (std::size_t y = 0; y < h; ++y) pass(v.data() + y * w, w, 1, r, line);
    for (const std::size_t r : radii)
        for (std::size_t x = 0; x < w; ++x) pass(v.data() + x, h, w, r, line);
}

}  // namespace

Image shown(const core::Page& page, const core::Mask& mask, const Image& decoded, Size size, const Box& area, Resample resample) {
    const double density = std::clamp(mask.density, 0.0, 1.0);
    const double feather = std::max(mask.feather_mm, 0.0);
    if (density >= 1.0 && feather <= 0.0) return decoded.resize_region(size, area, resample);
    Image out;
    const double width_mm = page.spec.width_mm.value();
    const double sigma = feather / 2.0 * (width_mm > 0 ? size.width / width_mm : 0.0);
    if (sigma >= 0.3) {
        // (feathered over the area widened by 3σ, so the blur at the area's edge sees what lies past it)
        const int reach = static_cast<int>(std::ceil(3.0 * sigma));
        const Box wide{std::max(0, area.x0 - reach), std::max(0, area.y0 - reach), std::min(size.width, area.x1 + reach),
                       std::min(size.height, area.y1 + reach)};
        const Image part = decoded.resize_region(size, wide, resample);
        const std::string bytes = part.tobytes();
        std::vector<float> values(bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) values[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
        gauss_blur(values, static_cast<std::size_t>(wide.width()), static_cast<std::size_t>(wide.height()), sigma);
        std::string cut(static_cast<std::size_t>(area.width()) * static_cast<std::size_t>(area.height()), '\0');
        for (int y = 0; y < area.height(); ++y) {
            for (int x = 0; x < area.width(); ++x) {
                const std::size_t from = static_cast<std::size_t>(y + area.y0 - wide.y0) * static_cast<std::size_t>(wide.width()) +
                                         static_cast<std::size_t>(x + area.x0 - wide.x0);
                cut[static_cast<std::size_t>(y) * static_cast<std::size_t>(area.width()) + static_cast<std::size_t>(x)] = static_cast<char>(byte_of(values[from]));
            }
        }
        out = Image::frombytes("L", Size{area.width(), area.height()}, cut);
    } else {
        out = decoded.resize_region(size, area, resample);
    }
    if (density < 1.0) out = out.point([density](int v) { return static_cast<int>(std::lround(255.0 - density * (255.0 - v))); });
    return out;
}

bool lends_to(std::string_view op) { return core::channel_target_op(op); }

// --- the grid and the moves (B2) ----------------------------------------------------------------------------------------

namespace {

constexpr int kMaskDpi = 150;    // (an 8-bit page's: ops.MASK_DPI)
constexpr int kFinestDpi = 600;  // (a precise or ink page's at most)

// A pixel layer's resolution: its width over the page's (dots per inch).
double raster_dpi(const core::Page& page, std::uint32_t width) {
    const double width_mm = page.spec.width_mm.value();
    return width_mm > 0 ? width * 25.4 / width_mm : 0.0;
}

// The commonest value of a picture's edge pixels (ties: the lighter).
int background_of(const std::string& v, int w, int h) {
    std::array<std::size_t, 256> count{};
    const auto at = [&](int x, int y) { return static_cast<unsigned char>(v[std::size_t(y) * std::size_t(w) + std::size_t(x)]); };
    for (int x = 0; x < w; ++x) {
        ++count[at(x, 0)];
        if (h > 1) ++count[at(x, h - 1)];
    }
    for (int y = 1; y + 1 < h; ++y) {
        ++count[at(0, y)];
        if (w > 1) ++count[at(w - 1, y)];
    }
    int best = 255;
    for (int k = 255; k >= 0; --k)
        if (count[std::size_t(k)] > count[std::size_t(best)]) best = k;
    return best;
}

// A mask's picture, the area's coverage on its grid, and what stays once the area's part is lifted (0..255).
struct Lift {
    Size size;
    int dpi = kMaskDpi;
    double kx = 1, ky = 1;   // (its pixels per mm)
    std::string value;       // (its bytes)
    std::string share;       // (the area's coverage, a byte each)
    std::optional<Box> box;  // (where the area covers it)
    std::vector<float> left;
};

Lift lift(const core::Page& page, const core::Mask& mask, const core::Json& area) {
    Lift l;
    const Image picture = read_png(*mask.png).convert("L");
    l.size = picture.size();
    const double width_mm = page.spec.width_mm.value(), height_mm = page.spec.height_mm.value();
    if (!(width_mm > 0) || !(height_mm > 0)) throw core::OpError("the page has no size");
    l.kx = l.size.width / width_mm;
    l.ky = l.size.height / height_mm;
    l.dpi = std::max(1, static_cast<int>(std::lround(l.kx * 25.4)));
    const selection::AreaMask drawn = selection::area_mask(area, l.dpi);
    limits::check_coordinate(static_cast<double>(drawn.x0), "area");
    limits::check_coordinate(static_cast<double>(drawn.y0), "area");
    Image covered = Image::create("L", l.size, Ink(0));
    covered.paste(drawn.mask, Point{static_cast<int>(drawn.x0), static_cast<int>(drawn.y0)});
    l.box = covered.getbbox();
    l.value = picture.tobytes();
    l.share = covered.tobytes();
    const auto background = static_cast<float>(background_of(l.value, l.size.width, l.size.height));
    l.left.resize(l.value.size());
    for (std::size_t i = 0; i < l.value.size(); ++i) {
        const float s = static_cast<unsigned char>(l.share[i]) / 255.0f;
        l.left[i] = static_cast<unsigned char>(l.value[i]) * (1 - s) + background * s;
    }
    return l;
}

std::string bytes_of(const std::vector<float>& values) {
    std::string bytes(values.size(), '\0');
    for (std::size_t i = 0; i < values.size(); ++i) bytes[i] = static_cast<char>(std::lround(std::clamp(values[i], 0.0f, 255.0f)));
    return bytes;
}

core::Mask with_picture(const core::Mask& mask, const Image& picture) {
    core::Mask out = mask;
    out.png = fills::png_bytes(picture);
    out.memo = {};
    return out;
}

}  // namespace

int grid_dpi(const core::Page& page) {
    const bool precise = page.rgb && page.rgb->depth > 8;
    if (!page.color && !precise) return kMaskDpi;
    double finest = raster::kWorkingDpi;
    for (const core::Layer& layer : page.layers) {
        try {
            if (layer.color_raster) finest = std::max(finest, raster_dpi(page, core::ColorRasterView(*layer.color_raster).width()));
            if (layer.ink_raster) finest = std::max(finest, raster_dpi(page, core::ink_raster_header(*layer.ink_raster).width));
        } catch (const core::Error&) {  // (a raster this build cannot read: not looked at)
        }
    }
    return static_cast<int>(std::clamp(std::ceil(finest - 1e-6), static_cast<double>(raster::kWorkingDpi), static_cast<double>(kFinestDpi)));
}

Size grid_size(const core::Page& page) {
    const int dpi = grid_dpi(page);
    const double w = std::max(1.0, core::py_round_whole(page.spec.width_mm.value() / 25.4 * dpi));
    const double h = std::max(1.0, core::py_round_whole(page.spec.height_mm.value() / 25.4 * dpi));
    limits::check_sides(w, h, "the page");
    return Size{static_cast<int>(w), static_cast<int>(h)};
}

core::Mask move_mask(const core::Page& page, const core::Mask& mask, const core::Json& area, const std::array<double, 6>& m) {
    if (!mask.png || mask.png->empty()) return mask;
    const Lift l = lift(page, mask, area);
    if (!l.box) return mask;
    std::array<double, 6> inverse{};
    try {
        inverse = selection::invert(m);
    } catch (const core::Error&) {
        throw core::OpError("matrix squashes the area flat");
    }
    const int w = l.size.width, h = l.size.height;
    // where the area's box goes
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (const auto& [px, py] : {std::pair{l.box->x0, l.box->y0}, std::pair{l.box->x1, l.box->y0}, std::pair{l.box->x1, l.box->y1},
                                 std::pair{l.box->x0, l.box->y1}}) {
        const double xm = px / l.kx, ym = py / l.ky;
        const double u = (m[0] * xm + m[2] * ym + m[4]) * l.kx, v = (m[1] * xm + m[3] * ym + m[5]) * l.ky;
        if (!std::isfinite(u) || !std::isfinite(v)) throw core::OpError("matrix moves the area too far");
        x0 = std::min(x0, u); y0 = std::min(y0, v); x1 = std::max(x1, u); y1 = std::max(y1, v);
    }
    const auto clamp_to = [](double v, int hi) { return static_cast<int>(std::clamp(v, 0.0, static_cast<double>(hi))); };
    const Box to{clamp_to(std::floor(x0) - 2, w), clamp_to(std::floor(y0) - 2, h), clamp_to(std::ceil(x1) + 2, w), clamp_to(std::ceil(y1) + 2, h)};
    // the lifted part premultiplied by its share, read bilinear (a position on a pixel's centre reads that pixel)
    const auto lifted = [&](int x, int y, float& value, float& share) {
        if (x < l.box->x0 || y < l.box->y0 || x >= l.box->x1 || y >= l.box->y1) {
            value = share = 0;
            return;
        }
        const std::size_t i = std::size_t(y) * std::size_t(w) + std::size_t(x);
        share = static_cast<unsigned char>(l.share[i]) / 255.0f;
        value = static_cast<unsigned char>(l.value[i]) * share;
    };
    std::vector<float> out = l.left;
    for (int y = to.y0; y < to.y1; ++y) {
        for (int x = to.x0; x < to.x1; ++x) {
            const double xm = (x + .5) / l.kx, ym = (y + .5) / l.ky;
            const double u = (inverse[0] * xm + inverse[2] * ym + inverse[4]) * l.kx - .5;
            const double v = (inverse[1] * xm + inverse[3] * ym + inverse[5]) * l.ky - .5;
            if (!std::isfinite(u) || !std::isfinite(v)) continue;
            double fx = std::floor(u), fy = std::floor(v);
            double tx = u - fx, ty = v - fy;
            if (tx < 1e-9) tx = 0; else if (tx > 1 - 1e-9) { tx = 0; fx += 1; }
            if (ty < 1e-9) ty = 0; else if (ty > 1 - 1e-9) { ty = 0; fy += 1; }
            if (fx < -2 || fy < -2 || fx > w + 1 || fy > h + 1) continue;
            const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
            float value = 0, share = 0;
            for (int j = 0; j < 2; ++j) {
                const double wy = j == 0 ? 1 - ty : ty;
                if (wy == 0) continue;
                for (int k = 0; k < 2; ++k) {
                    const double wx = k == 0 ? 1 - tx : tx;
                    if (wx == 0) continue;
                    float pv = 0, ps = 0;
                    lifted(ix + k, iy + j, pv, ps);
                    value += static_cast<float>(pv * wx * wy);
                    share += static_cast<float>(ps * wx * wy);
                }
            }
            if (!(share > 0)) continue;
            float& here = out[std::size_t(y) * std::size_t(w) + std::size_t(x)];
            here = here * (1 - std::min(share, 1.0f)) + value;
        }
    }
    return with_picture(mask, Image::frombytes("L", l.size, bytes_of(out)));
}

core::Mask warp_mask(const core::Page& page, const core::Mask& mask, const core::Json& area, const warp::Go& go) {
    if (!mask.png || mask.png->empty()) return mask;
    const Lift l = lift(page, mask, area);
    if (!l.box) return mask;
    const int w = l.size.width;
    // the lifted part as a picture of its value over its share, warped as transform_area warps the pixels
    std::string piece(std::size_t(l.box->width()) * std::size_t(l.box->height()) * 4, '\0');
    for (int y = l.box->y0; y < l.box->y1; ++y) {
        for (int x = l.box->x0; x < l.box->x1; ++x) {
            const std::size_t i = std::size_t(y) * std::size_t(w) + std::size_t(x);
            const std::size_t o = (std::size_t(y - l.box->y0) * std::size_t(l.box->width()) + std::size_t(x - l.box->x0)) * 4;
            piece[o] = piece[o + 1] = piece[o + 2] = l.value[i];
            piece[o + 3] = l.share[i];
        }
    }
    const Image picture = Image::frombytes("RGBA", Size{l.box->width(), l.box->height()}, piece);
    Image out = Image::frombytes("L", l.size, bytes_of(l.left));
    if (const auto warped = warp::warp_image(picture, l.box->x0, l.box->y0, go, l.dpi)) {
        const Image share = warped->image.getchannel(3);
        out.paste(warped->image.getchannel(0), Point{static_cast<int>(warped->x0), static_cast<int>(warped->y0)}, &share);
    }
    return with_picture(mask, out);
}

}  // namespace genko::render::layer_masks
