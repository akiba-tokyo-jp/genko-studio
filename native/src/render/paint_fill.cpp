#include "render/paint_fill.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/ops_util.hpp"
#include "render/color_canvas.hpp"
#include "render/image_ops.hpp"
#include "render/page.hpp"
#include "render/selection.hpp"

// PhotoCraft v0.2.0 crates/algo/src/paint.rs (lines 60-120: composite_area, dither_noise; 125-155: paint_gradient's
// colour and coverage), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::paint_fill {

namespace {

using core::OpError;
using image_ops::Pixels;
using image_ops::Rgba;

// (a band's pixels at most: 4 million — 64 MB of a precise layer's floats)
constexpr std::int64_t kBandPixels = 4'000'000;

// The coverage as an "L" picture the size of the layer's grid (none: all of it covered).
std::optional<Image> on_grid(const core::Page& page, const Coverage& coverage, Size grid) {
    const double width_mm = page.spec.width_mm.value();
    Image picture = coverage.picture;
    Point offset = coverage.offset;
    int dpi = coverage.dpi;
    if (coverage.area) {
        // (the area drawn at the grid's own resolution: an 8-bit layer's grid is the page at 200 dpi)
        dpi = std::max(1, static_cast<int>(std::lround(grid.width * 25.4 / width_mm)));
        selection::AreaMask m = selection::area_mask(*coverage.area, dpi);
        picture = std::move(m.mask);
        offset = Point{static_cast<int>(m.x0), static_cast<int>(m.y0)};
    }
    const Size on_page{mm_to_px(width_mm, dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    Image placed = Image::create("L", on_page, Ink(0));
    placed.paste(picture, offset);
    if (placed.size() == grid) return placed;
    return placed.resize(grid, grid.width < on_page.width ? Resample::Box : Resample::Bilinear);
}

}  // namespace

float dither_noise(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E3779B1u ^ static_cast<std::uint32_t>(y) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return static_cast<float>(static_cast<std::uint16_t>(h & 0xFFFFu)) / 65535.0f;
}

bool paint(core::OpContext& c, std::size_t at, const std::string& layer_id, const std::optional<Coverage>& coverage, const Colour& colour,
           const How& how) {
    if (!image_ops::is_blend(how.blend)) throw OpError("blend is not one of the blends (normal, multiply, screen, …)");
    core::Document& doc = c.doc;
    image_ops::Window found;
    (void)image_ops::layer_pixels(c, at, layer_id, Box{0, 0, 0, 0}, &found);  // (its grid; its lines and pictures in its pixels)
    const Size grid = found.grid;
    if (grid.width <= 0 || grid.height <= 0) return false;
    std::optional<Image> covered;
    Box box{0, 0, grid.width, grid.height};
    if (coverage) {
        covered = on_grid(doc.page(at), *coverage, grid);
        const auto drawn = covered->getbbox();
        if (!drawn) return false;
        box = *drawn;
    }
    const core::Page& page = doc.page(at);
    const bool keep_alpha = page.layers[core::layer_by_id(page, layer_id)].lock_alpha;
    const RgbSpace space = RgbSpace::of(page);
    const double width_mm = page.spec.width_mm.value(), height_mm = page.spec.height_mm.value();
    const auto opacity = static_cast<float>(std::clamp(how.opacity, 0.0, 1.0));
    const std::int64_t rows = std::max<std::int64_t>(1, kBandPixels / std::max(1, box.width()));
    bool any = false;
    for (int y0 = box.y0; y0 < box.y1; y0 = static_cast<int>(std::min<std::int64_t>(box.y1, y0 + rows))) {
        const Box part{box.x0, y0, box.x1, static_cast<int>(std::min<std::int64_t>(box.y1, y0 + rows))};
        image_ops::Window window;
        Pixels p = image_ops::layer_pixels(c, at, layer_id, part, &window);
        const Box& b = window.box;
        const std::size_t w = static_cast<std::size_t>(b.width());
        const bool inks = p.kind() == Pixels::Kind::Inks;
        const bool eight = p.kind() == Pixels::Kind::Eight;
        std::vector<Rgba> shown;
        if (inks) shown = image_ops::rgba_of(p);
        const std::string share = covered ? covered->crop(b).tobytes() : std::string();
        std::vector<double> row(w * 3), alpha(w);
        bool changed = false;
        for (int y = 0; y < b.height(); ++y) {
            const double y_mm = (static_cast<double>(b.y0 + y) + 0.5) * height_mm / grid.height;
            for (std::size_t x = 0; x < w; ++x) {
                const double x_mm = (static_cast<double>(b.x0) + static_cast<double>(x) + 0.5) * width_mm / grid.width;
                double rgba[4] = {0, 0, 0, 1};
                colour(x_mm, y_mm, rgba);
                if (how.dither && eight) {
                    // (one level of grey noise breaks an 8-bit layer's bands without speckle: PhotoCraft's)
                    const double n = (dither_noise(b.x0 + static_cast<int>(x), b.y0 + y) - 0.5) / 255.0;
                    for (unsigned k = 0; k < 3; ++k) rgba[k] = std::clamp(rgba[k] + n, 0.0, 1.0);
                }
                for (unsigned k = 0; k < 3; ++k) row[x * 3 + k] = rgba[k];
                alpha[x] = rgba[3];
            }
            if (!inks && !space.is_srgb()) space.from_srgb(row.data(), w);  // (the page's values)
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const float k = covered ? static_cast<float>(static_cast<unsigned char>(share[i]) / 255.0) * opacity : opacity;
                if (!(k > 0.0f)) continue;
                const Rgba source{static_cast<float>(row[x * 3]), static_cast<float>(row[x * 3 + 1]), static_cast<float>(row[x * 3 + 2]),
                                  static_cast<float>(std::clamp(alpha[x], 0.0, 1.0))};
                if (inks) {
                    Rgba out = image_ops::composite(how.blend, shown[i], source, k);
                    if (keep_alpha) out[3] = shown[i][3];
                    shown[i] = out;
                } else {
                    float* v = p.at(i);
                    Rgba out = image_ops::composite(how.blend, Rgba{v[0], v[1], v[2], v[3]}, source, k);
                    if (keep_alpha) out[3] = v[3];
                    for (unsigned n = 0; n < 4; ++n) v[n] = out[n];
                }
                changed = true;
            }
        }
        if (!changed) continue;
        if (inks) image_ops::from_rgba(p, shown);
        image_ops::put_layer_pixels(doc, at, layer_id, p, b);
        any = true;
    }
    return any;
}

}  // namespace genko::render::paint_fill
