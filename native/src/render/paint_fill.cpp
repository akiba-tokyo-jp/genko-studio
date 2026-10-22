#include "render/paint_fill.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/color_raster.hpp"
#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/vector.hpp"
#include "render/color_canvas.hpp"
#include "render/image_ops.hpp"
#include "render/ink_edit.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
#include "render/shape_parts.hpp"

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

bool becomes_precise(const core::Page& page, const core::Layer& layer) {
    const bool deep = !page.color && page.rgb && page.rgb->depth > 8 && !page.rgb->table;
    const bool empty = layer.color_raster == nullptr && layer.ink_raster == nullptr && (layer.raster_png == nullptr || layer.raster_png->empty()) &&
                       layer.stroke_count() == 0 && layer.patches.empty() && !shape_parts::any(layer);
    return deep && empty && layer.kind == core::LayerKind::Raster && layer.role != core::LayerRole::Tone && !layer.panel_clip && !layer.effect &&
           !layer.screen && !layer.color;
}

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
    if (const core::Page& seen = doc.page(at); becomes_precise(seen, seen.layers[core::layer_by_id(seen, layer_id)])) {
        // (clear precise pixels at the page's working grid, as keep_page_modes widens the 8-bit pixels an op leaves there)
        const int dpi = raster::working_dpi(seen);
        const int width = std::max(1, mm_to_px(seen.spec.width_mm.value(), dpi));
        const int height = std::max(1, mm_to_px(seen.spec.height_mm.value(), dpi));
        limits::check_picture(width, height, "the page");
        const char* precision = seen.rgb->depth == 16 ? "u16" : "f32";
        core::Page& page = doc.edit_page(at);
        core::Layer& layer = page.layers[core::layer_by_id(page, layer_id)];
        layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), precision, [](std::size_t) { return std::array<double, 4>{0, 0, 0, 0}; }));
        layer.raster_png = nullptr;
        layer.raster_memo = {};
        layer.raster_relpath.reset();
    }
    // the layer's kind (as image_ops::layer_pixels takes it): precise, ink (a CMYK, Lab, grey or multichannel page's paint
    // layer that holds no 8-bit picture) or 8-bit
    bool inks = false;
    {
        const core::Page& seen = doc.page(at);
        const core::Layer& layer = seen.layers[core::layer_by_id(seen, layer_id)];
        if (layer.locked) throw OpError("the layer is locked");
        inks = seen.color && !layer.color_raster && !layer.raster_png;
    }
    // (review P2-1: an ink layer painted in the 通常 mode keeps its lines and shape parts as they are — drawn over the paint
    // as before E4 —, its inks alone read and written; another mode makes them its pixels first, as an 8-bit layer's)
    const bool keep_lines = inks && how.blend == "normal";
    std::optional<core::InkPixels> whole_inks;
    if (inks) {
        if (keep_lines) {
            // (its pictures — none, as ink_edit::bake_marks lays them into its inks after every op — laid in first)
            if (!doc.page(at).layers[core::layer_by_id(doc.page(at), layer_id)].patches.empty()) ink_edit::bake_marks(doc);
            const core::Page& seen = doc.page(at);
            const core::Layer& layer = seen.layers[core::layer_by_id(seen, layer_id)];
            whole_inks = layer.ink_raster ? core::decode_ink_raster(*layer.ink_raster)
                                          : core::blank_ink_pixels(seen.color->space, seen.color->depth,
                                                                   static_cast<std::uint32_t>(mm_to_px(seen.spec.width_mm.value(), raster::kWorkingDpi)),
                                                                   static_cast<std::uint32_t>(mm_to_px(seen.spec.height_mm.value(), raster::kWorkingDpi)),
                                                                   seen.color->planes());
        } else {
            const core::Page& seen = doc.page(at);
            whole_inks = ink_edit::with_marks(seen, seen.layers[core::layer_by_id(seen, layer_id)], &doc);
        }
    } else {
        // (an 8-bit layer's lines and pictures made its pixels, a precise pen line made precise: image_ops' way)
        image_ops::Window ready;
        (void)image_ops::layer_pixels(c, at, layer_id, Box{0, 0, 0, 0}, &ready);
    }
    const core::Page& page = doc.page(at);
    const core::Layer& layer = page.layers[core::layer_by_id(page, layer_id)];
    const bool precise = !inks && layer.color_raster != nullptr;
    // (review P3-6: the layer held once — a precise layer's samples in one edit, an ink layer's inks, an 8-bit layer's
    // picture — and its bands worked in floats one after another)
    std::optional<core::ColorRasterEdit> edit;
    std::optional<Image> picture;
    Size grid;
    if (precise) {
        edit.emplace(std::string(*layer.color_raster));
        grid = Size{static_cast<int>(edit->width()), static_cast<int>(edit->height())};
    } else if (inks) {
        grid = Size{static_cast<int>(whole_inks->width), static_cast<int>(whole_inks->height)};
    } else {
        picture = raster::layer_pixels(page, layer).convert("RGBA");
        grid = picture->size();
    }
    if (grid.width <= 0 || grid.height <= 0) return false;
    std::optional<Image> covered;
    Box box{0, 0, grid.width, grid.height};
    if (coverage) {
        covered = on_grid(page, *coverage, grid);
        const auto drawn = covered->getbbox();
        if (!drawn) return false;
        box = *drawn;
    }
    const bool keep_alpha = layer.lock_alpha;
    const RgbSpace space = RgbSpace::of(page);
    const double width_mm = page.spec.width_mm.value(), height_mm = page.spec.height_mm.value();
    const auto opacity = static_cast<float>(std::clamp(how.opacity, 0.0, 1.0));
    const std::int64_t rows = std::max<std::int64_t>(1, kBandPixels / std::max(1, box.width()));
    bool any = false;
    for (int y0 = box.y0; y0 < box.y1; y0 = static_cast<int>(std::min<std::int64_t>(box.y1, y0 + rows))) {
        const Box b{box.x0, y0, box.x1, static_cast<int>(std::min<std::int64_t>(box.y1, y0 + rows))};
        const std::size_t w = static_cast<std::size_t>(b.width());
        // the band's values
        Pixels p = precise  ? Pixels(Pixels::Kind::Precise, static_cast<std::uint32_t>(b.width()), static_cast<std::uint32_t>(b.height()), 3)
                   : inks   ? image_ops::inks_part(*whole_inks, *page.color, b)
                            : image_ops::from_image(picture->crop(b));
        if (precise) {
            p.floating = edit->floating();
            for (int y = 0; y < b.height(); ++y) {
                for (int x = 0; x < b.width(); ++x) {
                    const auto v = edit->pixel(std::size_t(b.y0 + y) * edit->width() + std::size_t(b.x0 + x));
                    float* out = p.at(std::size_t(y) * w + std::size_t(x));
                    for (unsigned k = 0; k < 4; ++k) out[k] = static_cast<float>(v[k]);
                }
            }
        }
        const bool eight = !precise && !inks;
        std::vector<Rgba> shown;
        if (inks) shown = image_ops::rgba_of(p);
        const std::string share = covered ? covered->crop(b).tobytes() : std::string();
        std::vector<double> row(w * 3), alpha(w);
        std::vector<std::uint8_t> touched(p.size(), 0);
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
                touched[i] = 1;
                changed = true;
            }
        }
        if (!changed) continue;
        any = true;
        if (inks) {
            image_ops::from_rgba(p, shown);
            image_ops::put_inks_part(*whole_inks, p, b, &touched);  // (the pixels painted only: the others keep their inks)
        } else if (precise) {
            for (int y = 0; y < b.height(); ++y) {
                for (int x = 0; x < b.width(); ++x) {
                    const std::size_t i = std::size_t(y) * w + std::size_t(x);
                    if (touched[i] == 0) continue;
                    const float* v = p.at(i);
                    std::array<double, 4> rgba{};
                    for (unsigned k = 0; k < 4; ++k) {
                        if (!std::isfinite(v[k])) throw core::Error("value", "the result exceeds the finite working range");
                        rgba[k] = p.floating && k < 3 ? static_cast<double>(v[k]) : std::clamp(static_cast<double>(v[k]), 0.0, 1.0);
                    }
                    edit->set(std::size_t(b.y0 + y) * edit->width() + std::size_t(b.x0 + x), rgba);
                }
            }
        } else {
            picture->paste(image_ops::to_image(p), Point{b.x0, b.y0});
        }
    }
    if (!any) return false;
    core::Page& out_page = doc.edit_page(at);
    core::Layer& out = out_page.layers[core::layer_by_id(out_page, layer_id)];
    if (precise) {
        out.color_raster = std::make_shared<const std::string>(std::move(*edit).take());
    } else if (inks) {
        out.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(*whole_inks));
        out.ink_memo = {};
        if (!keep_lines) {  // (its marks are in its inks: as put_layer_pixels leaves it)
            out.strokes = core::empty_strokes();
            out.shapes = core::vector::empty_shapes();
            out.patches.clear();
        }
    } else {
        raster::save_raster(out_page, out, *picture);
    }
    out.kind = core::LayerKind::Raster;
    return true;
}

}  // namespace genko::render::paint_fill
