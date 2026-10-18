#include "render/vector_masks.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/pyconv.hpp"
#include "render/fill.hpp"
#include "render/op_limits.hpp"
#include "render/path_raster.hpp"
#include "render/png.hpp"

namespace genko::render::vector_masks {

namespace {

constexpr int kMaskDpi = 150;  // (a layer's mask: ops.MASK_DPI)

unsigned char byte_of(float v) { return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// A Gaussian of `sigma` on one plane as three box passes along the rows, then three along the columns (Kovesi's radii,
// the edges repeated, the sums in double): as a layer mask's feather is blurred (render/layer_masks).
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
    // (the columns a strip at a time, copied side by side: the same sums, read along the rows the plane is kept in)
    constexpr std::size_t kStrip = 64;
    std::vector<float> strip(std::min(kStrip, w) * h);
    for (std::size_t x0 = 0; x0 < w; x0 += kStrip) {
        const std::size_t cols = std::min(kStrip, w - x0);
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t c = 0; c < cols; ++c) strip[c * h + y] = v[y * w + x0 + c];
        for (std::size_t c = 0; c < cols; ++c)
            for (const std::size_t r : radii) pass(strip.data() + c * h, h, 1, r, line);
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t c = 0; c < cols; ++c) v[y * w + x0 + c] = strip[c * h + y];
    }
}

// The path's coverage over `box` of the page drawn at `size` (density not laid on).
std::vector<float> coverage(const core::Page& page, const core::vector::Path& path, Size size, const Box& box) {
    const std::size_t n = static_cast<std::size_t>(std::max(0, box.width())) * static_cast<std::size_t>(std::max(0, box.height()));
    if (path.empty()) return std::vector<float>(n, path.inverted ? 0.0f : 1.0f);
    const double width_mm = page.spec.width_mm.value(), height_mm = page.spec.height_mm.value();
    const double kx = width_mm > 0 ? size.width / width_mm : 0.0, ky = height_mm > 0 ? size.height / height_mm : 0.0;
    const core::vector::Path pixels = path.transformed({kx, 0.0, 0.0, ky, -static_cast<double>(box.x0), -static_cast<double>(box.y0)});
    return path_raster::path_coverage(pixels, path_raster::PixelBox{0, 0, box.width(), box.height()});
}

}  // namespace

bool shows(const core::Layer& layer) { return layer.vector_mask && layer.vector_mask->enabled; }

std::vector<float> values(const core::Page& page, const core::vector::VectorMask& mask, Size size, const Box& area) {
    const std::size_t n = static_cast<std::size_t>(std::max(0, area.width())) * static_cast<std::size_t>(std::max(0, area.height()));
    if (!mask.enabled) return std::vector<float>(n, 1.0f);
    const double feather = std::max(mask.feather_mm, 0.0);
    const double width_mm = page.spec.width_mm.value();
    const double sigma = feather / 2.0 * (width_mm > 0 ? size.width / width_mm : 0.0);
    std::vector<float> out;
    if (sigma >= 0.3 && !mask.path.empty()) {
        const int reach = static_cast<int>(std::ceil(3.0 * sigma));
        limits::check_count(reach, limits::kReach, "feather_mm");
        const Box wide{area.x0 - reach, area.y0 - reach, area.x1 + reach, area.y1 + reach};
        std::vector<float> cover = coverage(page, mask.path, size, wide);
        gauss_blur(cover, static_cast<std::size_t>(wide.width()), static_cast<std::size_t>(wide.height()), sigma);
        out.resize(n);
        for (int y = 0; y < area.height(); ++y) {
            for (int x = 0; x < area.width(); ++x) {
                out[static_cast<std::size_t>(y) * static_cast<std::size_t>(area.width()) + static_cast<std::size_t>(x)] =
                    cover[static_cast<std::size_t>(y + reach) * static_cast<std::size_t>(wide.width()) + static_cast<std::size_t>(x + reach)];
            }
        }
    } else {
        out = coverage(page, mask.path, size, area);
    }
    const float density = static_cast<float>(std::clamp(mask.density, 0.0, 1.0));
    if (density < 1.0f) {
        for (float& v : out) v = 1.0f - density * (1.0f - v);
    }
    return out;
}

Image shown(const core::Page& page, const core::vector::VectorMask& mask, Size size, const Box& area) {
    const std::vector<float> v = values(page, mask, size, area);
    std::string bytes(v.size(), '\0');
    for (std::size_t i = 0; i < v.size(); ++i) bytes[i] = static_cast<char>(byte_of(v[i]));
    return Image::frombytes("L", Size{std::max(0, area.width()), std::max(0, area.height())}, bytes);
}

std::optional<Image> with_vector_mask(const core::Page& page, const core::Layer& layer, Size size, const Box& area, std::optional<Image> mask) {
    if (!shows(layer)) return mask;
    Image vector = shown(page, *layer.vector_mask, size, area);
    if (!mask) return vector;
    return chops::multiply(*mask, vector);
}

void rasterize(const core::Page& page, core::Layer& layer) {
    if (!layer.vector_mask) return;
    const double w = std::max(1.0, core::py_round_whole(page.spec.width_mm.value() / 25.4 * kMaskDpi));
    const double h = std::max(1.0, core::py_round_whole(page.spec.height_mm.value() / 25.4 * kMaskDpi));
    limits::check_sides(w, h, "the page");
    const Size size{static_cast<int>(w), static_cast<int>(h)};
    Image picture = layer.mask && layer.mask->png && !layer.mask->png->empty()
                        ? open_image(*layer.mask->png, kPillowOpenLimits).convert("L").resize(size)
                        : Image::create("L", size, Ink(255));
    const Image vector = shown(page, *layer.vector_mask, size, Box{0, 0, size.width, size.height});
    picture = chops::multiply(picture, vector);
    if (layer.mask) {
        layer.mask->png = fills::png_bytes(picture);
        layer.mask->memo = {};
    } else {
        core::Mask mask;
        mask.png = fills::png_bytes(picture);
        layer.mask = std::move(mask);
    }
    layer.vector_mask.reset();
}

}  // namespace genko::render::vector_masks
