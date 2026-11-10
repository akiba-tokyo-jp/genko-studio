// PC3 節 B (B2, SPEC.md COLOR-06): Layer › Layer Mask › Apply and From Transparency, Edit › Stroke, and a colour laid with
// a layer's alpha kept (fill_area's and outline_area's preserve_transparency). After PhotoCraft v0.2.0's
// engine/src/layer_menu_cmds.rs (apply_mask, mask_from_transparency), extra_cmds.rs (stroke) and pixels.rs (fill_surface's
// lock) — MIT, native/third_party/photocraft/NOTICE.md —, on Genko's three ways (render/image_ops Pixels).

#include "render/ops_masks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "core/areas.hpp"
#include "core/color_model.hpp"
#include "core/error.hpp"
#include "core/masks.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/color_canvas.hpp"
#include "render/fill.hpp"
#include "render/image_ops.hpp"
#include "render/layer_masks.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster_ops.hpp"
#include "render/selection.hpp"
#include "render/selection_ext.hpp"

namespace genko::render {

namespace {

using core::Json;
using core::LayerKind;
using core::OpError;

constexpr int kStrokeDpi = fills::kFillDpi;  // (the band is a fill's patch: at FILL_DPI)

std::string layer_id_of(const Json& op, const char* key) { return core::truthy_at(op, key) ? core::py_str(op[key]) : std::string(); }

// The layer a mask op names ("id"), as it must be to work its pixels: not locked, a pen or paint layer.
const core::Layer& pixel_layer(const core::Page& page, const std::string& id, const char* what) {
    const core::Layer& layer = page.layers[core::layer_by_id(page, id)];
    if (layer.locked) throw OpError("the layer is locked");
    if (layer.kind != LayerKind::Raster && layer.kind != LayerKind::Strokes) {
        throw OpError(std::string(what) + ": the layer " + id + " is not a pen or paint layer (rasterize it first)");
    }
    return layer;
}

// The mask as it shows over a grid of w × h stretched over the page (its density and feather laid on), each 0..1.
std::vector<float> mask_over(const core::Page& page, const core::Mask& mask, std::uint32_t w, std::uint32_t h) {
    const Image decoded = read_png(*mask.png).convert("L");
    const Box whole{0, 0, static_cast<int>(w), static_cast<int>(h)};
    const std::string bytes = layer_masks::shown(page, mask, decoded, Size{static_cast<int>(w), static_cast<int>(h)}, whole, Resample::Bilinear).tobytes();
    std::vector<float> out(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) out[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
    return out;
}

double opacity_of(const Json& op) {
    const Json* v = core::get(op, "opacity");
    if (v == nullptr || v->is_null()) return 1.0;
    const double opacity = core::finite_float(*v, "opacity");
    if (opacity < 0 || opacity > 1) throw OpError("opacity must be 0..1");
    return opacity;
}

std::vector<std::int64_t> colour_of(const Json& op, const core::Document& doc) {
    std::vector<std::int64_t> rgb;
    if (core::truthy_at(op, "rgb")) rgb = core::int_tuple(op["rgb"]);
    else if (!doc.brush_rgb.empty()) rgb = doc.brush_rgb;
    else rgb = {20, 20, 20};
    if (rgb.size() < 3 || std::any_of(rgb.begin(), rgb.begin() + 3, [](std::int64_t v) { return v < 0 || v > 255; })) {
        throw OpError("rgb is [r, g, b], each 0..255");
    }
    rgb.resize(3);
    return rgb;
}

Size page_at(const core::Page& page, int dpi) {
    const double w = std::max(1.0, core::py_round_whole(page.spec.width_mm.value() / 25.4 * dpi));
    const double h = std::max(1.0, core::py_round_whole(page.spec.height_mm.value() / 25.4 * dpi));
    limits::check_sides(w, h, "the page");
    return Size{static_cast<int>(w), static_cast<int>(h)};
}

}  // namespace

void apply_layer_mask(core::OpContext& c) {
    use_brushes_of(c.doc);  // (the lines it draws into pixels, with the book's brushes)
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const std::string id = layer_id_of(op, "id");
    std::optional<core::Mask> mask;
    {
        const core::Layer& layer = pixel_layer(doc.page(at), id, "apply_layer_mask");
        if (!layer.mask || !layer.mask->png || layer.mask->png->empty()) throw OpError("the layer " + id + " has no layer mask");
        mask = *layer.mask;
    }
    image_ops::Pixels p = image_ops::layer_pixels(c, at, id);  // (a pen layer's lines drawn into its pixels)
    const std::vector<float> shown = mask_over(doc.page(at), *mask, p.width(), p.height());
    const unsigned alpha = p.colours();
    for (std::size_t i = 0; i < p.size(); ++i) p.at(i)[alpha] *= shown[i];
    image_ops::put_layer_pixels(doc, at, id, p);
    core::Page& page = doc.edit_page(at);
    page.layers[core::layer_by_id(page, id)].mask.reset();
}

void mask_from_transparency(core::OpContext& c) {
    use_brushes_of(c.doc);  // (the lines it draws into pixels, with the book's brushes)
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const std::string id = layer_id_of(op, "id");
    std::optional<core::Mask> before;
    {
        const core::Layer& layer = pixel_layer(doc.page(at), id, "mask_from_transparency");
        before = layer.mask;
    }
    core::Mask made;
    made.enabled = true;
    if (before) {  // (the settings of the mask it replaces kept, then the op's)
        made.enabled = before->enabled;
        made.linked = before->linked;
        made.density = before->density;
        made.feather_mm = before->feather_mm;
    }
    Json given = Json::object();
    for (const char* key : {"linked", "density", "feather_mm"}) {
        if (const Json* v = core::get(op, key); v != nullptr && !v->is_null()) given[key] = *v;
    }
    try {
        core::read_mask_settings(given, made);
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
    image_ops::Pixels p = image_ops::layer_pixels(c, at, id);
    const unsigned alpha = p.colours();
    const int w = static_cast<int>(p.width()), h = static_cast<int>(p.height());
    std::string grey(p.size(), '\0');
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = std::size_t(y) * std::size_t(w) + std::size_t(x);
            const float a = p.at(i)[alpha];
            const float v = std::isfinite(a) ? std::clamp(a, 0.0f, 1.0f) : 0.0f;
            grey[i] = static_cast<char>(std::lround(v * 255.0f));
            if (v > 0) {
                x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1);
            }
        }
    }
    // (the pixels opaque over the box of what the layer held, as PhotoCraft makes them: the mask hides the rest)
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) p.at(std::size_t(y) * std::size_t(w) + std::size_t(x))[alpha] = 1.0f;
    const Size grid = layer_masks::grid_size(doc.page(at));
    const Image alpha_picture = Image::frombytes("L", Size{w, h}, grey);
    const Image picture = alpha_picture.size() == grid ? alpha_picture
                                                       : alpha_picture.resize(grid, grid.width < w ? Resample::Box : Resample::Bilinear);
    made.png = fills::png_bytes(picture);
    image_ops::put_layer_pixels(doc, at, id, p);
    core::Page& page = doc.edit_page(at);
    page.layers[core::layer_by_id(page, id)].mask = std::move(made);
}

void outline_area(core::OpContext& c) {
    use_brushes_of(c.doc);  // (the lines it draws into pixels, with the book's brushes)
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const std::string id = layer_id_of(op, "layer_id");
    if (id.empty()) throw OpError("outline_area needs layer_id");
    const core::Page& page = doc.page(at);
    {
        const core::Layer& layer = page.layers[core::layer_by_id(page, id)];
        if (layer.locked) throw OpError("the layer is locked");
        if (layer.kind != LayerKind::Strokes && layer.kind != LayerKind::Raster && layer.kind != LayerKind::Tone) {
            throw OpError("this layer cannot be painted on (choose a pen, paint or tone layer)");
        }
    }
    const Json* width_value = core::get(op, "width_mm");
    if (width_value == nullptr || width_value->is_null()) throw OpError("outline_area needs width_mm");
    const double width_mm = core::finite_float(*width_value, "width_mm");
    if (!(width_mm > 0)) throw OpError("width_mm must be more than 0");
    const double r = width_mm / 25.4 * kStrokeDpi;
    limits::check_count(r, limits::kReach, "width_mm");
    const std::string location = core::truthy_at(op, "location") ? core::py_str(op["location"]) : std::string("center");
    if (location != "inside" && location != "center" && location != "outside") throw OpError("location must be inside, center or outside");
    const std::vector<std::int64_t> rgb = colour_of(op, doc);
    const double opacity = opacity_of(op);
    const bool preserve = core::truthy_at(op, "preserve_transparency");
    // the edge: the area's (resolved by the bus), else the layer's alpha
    const Size whole = page_at(page, kStrokeDpi);
    Image edge;
    Point corner{0, 0};
    if (core::truthy_at(op, "area")) {
        selection::AreaMask m = selection::area_mask(core::op_area(op), kStrokeDpi);
        edge = std::move(m.mask);
        limits::check_coordinate(static_cast<double>(m.x0), "area");
        limits::check_coordinate(static_cast<double>(m.y0), "area");
        corner = Point{static_cast<int>(m.x0), static_cast<int>(m.y0)};
    } else {
        edge = selection::to_mask(Json{{"transparency", id}}, page, &doc, kStrokeDpi);
    }
    const auto drawn = edge.getbbox();
    if (!drawn) throw OpError(core::truthy_at(op, "area") ? "the area is empty" : "the layer has nothing drawn to stroke");
    // (worked over the edge's box widened by the band, on the page)
    const int margin = static_cast<int>(std::ceil(r)) + 2;
    const Box work{std::max(0, corner.x + drawn->x0 - margin), std::max(0, corner.y + drawn->y0 - margin),
                   std::min(whole.width, corner.x + drawn->x1 + margin), std::min(whole.height, corner.y + drawn->y1 + margin)};
    if (work.width() <= 0 || work.height() <= 0) throw OpError("the area is off the page");
    Image local = Image::create("L", Size{work.width(), work.height()}, Ink(0));
    local.paste(edge, Point{corner.x - work.x0, corner.y - work.y0});
    const std::size_t w = static_cast<std::size_t>(work.width()), h = static_cast<std::size_t>(work.height());
    const std::vector<float> m = selection_ext::from_l(local);
    const auto rf = static_cast<float>(r);
    std::vector<float> outer, inner;
    if (location == "inside") {
        outer = m;
        inner = selection_ext::contract(m, w, h, rf);
    } else if (location == "outside") {
        outer = selection_ext::expand(m, w, h, rf);
        inner = m;
    } else {
        outer = selection_ext::expand(m, w, h, rf / 2);
        inner = selection_ext::contract(m, w, h, rf / 2);
    }
    std::vector<float> band(outer.size());
    for (std::size_t i = 0; i < band.size(); ++i) band[i] = std::clamp(outer[i] - inner[i], 0.0f, 1.0f);
    const Image band_picture = selection_ext::to_l(band, local.size());
    if (!band_picture.getbbox()) throw OpError("the stroke has nothing to draw");
    if (preserve) {
        fill_keeping_alpha(c, at, id, band_picture, Point{work.x0, work.y0}, kStrokeDpi, rgb, opacity);
        return;
    }
    auto patch = fills::mask_patch(band_picture, kStrokeDpi, rgb, opacity, {static_cast<double>(work.x0), static_cast<double>(work.y0)});
    if (!patch) throw OpError("the stroke has nothing to draw");
    core::Page& edited = doc.edit_page(at);
    edited.layers[core::layer_by_id(edited, id)].patches.push_back(std::move(*patch));
}

void fill_keeping_alpha(core::OpContext& c, std::size_t at, const std::string& id, const Image& coverage, Point offset, int dpi,
                        const std::vector<std::int64_t>& rgb, double opacity) {
    core::Document& doc = c.doc;
    image_ops::Window found;
    (void)image_ops::layer_pixels(c, at, id, Box{0, 0, 0, 0}, &found);  // (its grid: a pen layer's lines in its pixels first)
    const core::Page& page = doc.page(at);
    const Size on_page = page_at(page, dpi);
    Image placed = Image::create("L", on_page, Ink(0));
    placed.paste(coverage, offset);
    const Image on_grid = placed.size() == found.grid ? placed
                                                      : placed.resize(found.grid, found.grid.width < on_page.width ? Resample::Box : Resample::Bilinear);
    const auto box = on_grid.getbbox();
    if (!box) return;
    image_ops::Window window;
    image_ops::Pixels p = image_ops::layer_pixels(c, at, id, *box, &window);
    const std::string share = on_grid.crop(window.box).tobytes();
    const unsigned alpha = p.colours();
    std::array<double, 3> srgb{rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0};
    const double strength = std::clamp(opacity, 0.0, 1.0);
    const auto k_at = [&](std::size_t i) { return static_cast<float>(static_cast<unsigned char>(share[i]) / 255.0 * strength); };
    // (PhotoCraft's fill_surface with the transparency locked: the colour laid over the pixel as a normal layer lays it,
    // the pixel's alpha kept)
    const auto over = [](double under, double a, double top, double k) { return (top * k + under * a * (1 - k)) / (k + a * (1 - k)); };
    switch (p.kind()) {
        case image_ops::Pixels::Kind::Eight: {
            std::array<double, 3> t = srgb;
            if (core::profiled_rgb(page)) RgbSpace::of(page).from_srgb(t.data(), 1);  // (its pixels are the page's values)
            for (std::size_t i = 0; i < p.size(); ++i) {
                float* v = p.at(i);
                const float k = k_at(i);
                if (!(k > 0) || !(v[alpha] > 0)) continue;
                for (unsigned ch = 0; ch < 3; ++ch) v[ch] = static_cast<float>(over(v[ch], v[alpha], t[ch], k));
            }
            break;
        }
        case image_ops::Pixels::Kind::Precise: {
            const RgbSpace space = RgbSpace::of(page);
            std::array<double, 3> t = srgb;
            space.from_srgb(t.data(), 1);
            std::array<double, 3> linear{};
            for (unsigned ch = 0; ch < 3; ++ch) linear[ch] = space.decode(ch, t[ch]);
            for (std::size_t i = 0; i < p.size(); ++i) {
                float* v = p.at(i);
                const float k = k_at(i);
                if (!(k > 0) || !(v[alpha] > 0)) continue;
                for (unsigned ch = 0; ch < 3; ++ch) {
                    v[ch] = static_cast<float>(space.encode(ch, over(space.decode(ch, v[ch]), v[alpha], linear[ch], k)));
                }
            }
            break;
        }
        case image_ops::Pixels::Kind::Inks: {
            std::vector<image_ops::Rgba> px = image_ops::rgba_of(p);
            bool any = false;
            for (std::size_t i = 0; i < px.size(); ++i) {
                const float k = k_at(i);
                if (!(k > 0) || !(px[i][3] > 0)) continue;
                for (unsigned ch = 0; ch < 3; ++ch) px[i][ch] = static_cast<float>(over(px[i][ch], px[i][3], srgb[ch], k));
                any = true;
            }
            if (!any) return;
            image_ops::from_rgba(p, px);
            break;
        }
    }
    image_ops::put_layer_pixels(doc, at, id, p, window.box);
}

}  // namespace genko::render
