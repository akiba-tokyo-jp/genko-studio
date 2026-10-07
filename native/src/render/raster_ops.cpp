#include "render/raster_ops.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QResource>
#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/areas.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/rulers.hpp"
#include "core/stroke_tools.hpp"
#include "core/strokes.hpp"
#include "render/brushes.hpp"
#include "render/draw.hpp"
#include "render/fill.hpp"
#include "render/filters.hpp"
#include "render/op_limits.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/page_internal.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
#include "render/stroke.hpp"
#include "render/vectorize.hpp"
#include "render/warp.hpp"

static void initialize_stamp_resources() { Q_INIT_RESOURCE(genko_materials); }

namespace genko::render {

namespace {

using core::Document;
using core::Json;
using core::Layer;
using core::LayerKind;
using core::LayerRole;
using core::Num;
using core::OpContext;
using core::OpError;
using core::Page;

constexpr int kWorkingDpi = raster::kWorkingDpi;
constexpr int kMaskDpi = 150;                          // ops.MASK_DPI
constexpr int kGradientDpi = 150;                      // ops.GRADIENT_DPI
constexpr std::int64_t kOpsMaxImagePixels = 120'000'000;  // ops.MAX_IMAGE_PIXELS

// --- the helpers of ops.py ------------------------------------------------------------------------------------------

// ops._paint_target: the layer an edit works on (layer_id, else the role in "layer", ink by default)
std::size_t raster_paint_target(Page& page, const Json& op) {
    std::size_t at = 0;
    if (core::truthy_at(op, "layer_id")) {
        at = core::layer_by_id(page, core::py_str(op["layer_id"]));
    } else {
        const Json* layer = core::get(op, "layer");
        at = core::layer_for_role(page, core::role_from(Json(layer != nullptr && core::py_truthy(*layer) ? core::py_str(*layer) : "ink")));
    }
    const Layer& target = page.layers[at];
    if (target.locked) throw OpError("the layer is locked");
    if (target.kind != LayerKind::Strokes && target.kind != LayerKind::Raster && target.kind != LayerKind::Tone) {
        throw OpError("this layer cannot be painted on (choose a pen, paint or tone layer)");
    }
    return at;
}

// ops._resolve_layer: by id, else by role (made when the page has none)
std::size_t resolve_layer(Page& page, const Json& op) {
    if (core::truthy_at(op, "id")) {
        const Json& id = op["id"];
        for (std::size_t i = 0; i < page.layers.size(); ++i) {
            if (id.is_string() && page.layers[i].id == id.get_ref<const std::string&>()) return i;
        }
        throw OpError("no layer " + core::py_str(id));
    }
    if (core::truthy_at(op, "layer")) return core::layer_for_role(page, core::role_from(Json(core::py_str(op["layer"]))));
    throw OpError("layer id or role required");
}

// ops._rgb: the op's colour, else the brush's, else the ink's (any number of values, as Python takes them)
std::vector<std::int64_t> rgb_of(const Json& op, const Document& doc) {
    if (core::truthy_at(op, "rgb")) return core::int_tuple(op["rgb"]);
    if (!doc.brush_rgb.empty()) return doc.brush_rgb;
    return {20, 20, 20};
}

// float(op.get(key, fallback))
double float_at(const Json& op, std::string_view key, double fallback) {
    const Json* v = core::get(op, key);
    return v == nullptr ? fallback : core::finite_float(*v, key);
}

// float(op.get(key, fallback) or 0)
double float_or_zero(const Json& op, std::string_view key, double fallback) {
    const Json* v = core::get(op, key);
    if (v == nullptr) return fallback;
    return core::py_truthy(*v) ? core::finite_float(*v, key) : 0.0;
}

// ops._fill_threshold: 色の誤差 (0..100) as the darkness a pixel needs to be a wall (37 → 160)
int fill_threshold(const Json& op) {
    const double tolerance = core::py_max(0.0, core::py_min(100.0, float_at(op, "tolerance", 37.25)));
    return static_cast<int>(core::py_max(8.0, core::py_min(250.0, core::py_round_whole(255 * (1 - tolerance / 100)))));
}

struct Ignore {
    bool draft = false;
    bool text = false;
};

// ops._ignored
Ignore ignored(const Json& op) {
    const Json* given = core::get(op, "ignore");
    Ignore out;
    if (given == nullptr || !core::py_truthy(*given)) return out;
    const std::vector<Json> items = given->is_string() ? std::vector<Json>{*given} : core::iterate(*given);
    for (const Json& v : items) {
        if (!(v == Json("draft") || v == Json("text"))) throw OpError("ignore takes draft and text");
    }
    for (const Json& v : items) {
        if (v == Json("draft")) out.draft = true;
        if (v == Json("text")) out.text = true;
    }
    return out;
}

// A render's context for the helpers of page_internal.hpp, over a whole picture.
detail::Ctx context(const Page& page, const Document* episode, int dpi, Size size, std::vector<std::string>& omitted) {
    detail::Ctx ctx;
    ctx.page = &page;
    ctx.episode = episode;
    ctx.dpi = dpi;
    ctx.size = size;
    ctx.mode = "proof";
    ctx.omitted = &omitted;
    return ctx;
}

Size page_px(const Page& page, int dpi) {
    const int w = mm_to_px(page.spec.width_mm.value(), dpi);
    const int h = mm_to_px(page.spec.height_mm.value(), dpi);
    limits::check_picture(w, h, "the page");
    return Size{w, h};
}

// ops._fill_reference: what a fill looks at — the page as seen, the target layer alone, or the reference layers —
// in grey ("L")
Image fill_reference(const Document& doc, const Page& page, std::size_t target, const std::string& reference, int dpi,
                     const Ignore& ignore) {
    if (reference == "page") {
        Page filtered;
        const Page* seen = &page;
        if (ignore.draft) {
            filtered = page;
            filtered.layers.clear();
            for (std::size_t i = 0; i < page.layers.size(); ++i) {
                const Layer& layer = page.layers[i];
                if (layer.role != LayerRole::Draft && layer.role != LayerRole::Name && (layer.exportable || i == target)) {
                    filtered.layers.push_back(layer);
                }
            }
            seen = &filtered;
        }
        RenderOptions options;
        options.mode = !page.name_ok && !ignore.draft ? "name" : "proof";
        // ("text" left out: Python draws the page without its book — the page's own lines, its nombre as the defaults
        // have it, no onion skin)
        Document alone;
        const Document* episode = &doc;
        if (ignore.text) {
            for (const core::StoryLine* line : doc.story_for_page(page.index)) alone.story.push_back(*line);
            episode = &alone;
        }
        page_px(page, dpi);
        return render_page(*seen, dpi, options, episode).image.convert("L");
    }
    if (reference != "layer" && reference != "reference") throw OpError("reference must be page, layer or reference");
    std::vector<const Layer*> looked;
    if (reference == "layer") {
        looked.push_back(&page.layers[target]);
    } else {
        for (const Layer& layer : page.layers) {
            if (layer.reference) looked.push_back(&layer);
        }
    }
    if (looked.empty()) throw OpError("no layer is set as the reference (set_layer reference: true)");
    const Size size = page_px(page, dpi);
    Image base = Image::create("RGBA", size, Ink{255, 255, 255, 255});
    std::vector<std::string> omitted;
    const detail::Ctx ctx = context(page, &doc, dpi, size, omitted);
    for (const Layer* layer : looked) {
        if (layer->raster_png && !layer->raster_png->empty()) {
            base = alpha_composite(base, selection::open_picture(*layer->raster_png).convert("RGBA").resize(size));
        }
        if (const auto lines = detail::layer_lines(ctx, *layer, nullptr, nullptr, false)) base = alpha_composite(base, *lines);
    }
    Image image = base.convert("RGB");
    detail::draw_frames(image, Box{0, 0, size.width, size.height}, page, size, dpi);
    return image.convert("L");
}

// --- fills --------------------------------------------------------------------------------------------------------

void fill(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    const std::size_t ti = raster_paint_target(page, op);
    const int dpi = fills::kFillDpi;
    const double x_mm = core::finite_float(core::subscript(op, "x_mm"), "x_mm");
    const double ax = fills::px(x_mm, dpi);
    const double y_mm = core::finite_float(core::subscript(op, "y_mm"), "y_mm");
    const double ay = fills::px(y_mm, dpi);
    const std::string reference = core::truthy_at(op, "reference") ? core::py_str(op["reference"]) : "page";
    const Ignore ignore = ignored(op);
    const Image ref = fill_reference(doc, page, ti, reference, dpi, ignore);
    const core::Frame* panel = page.frame_at(Num(x_mm), Num(y_mm));
    std::optional<std::array<double, 4>> window;
    if (panel != nullptr) {  // search only the clicked panel's box (and a little around it)
        const core::Rect r = panel->rect;
        window = std::array<double, 4>{
            core::py_max(0.0, fills::px((r.x - Num(2)).value(), dpi)), core::py_max(0.0, fills::px((r.y - Num(2)).value(), dpi)),
            core::py_min(static_cast<double>(ref.width()), fills::px((r.x + r.width + Num(2)).value(), dpi)),
            core::py_min(static_cast<double>(ref.height()), fills::px((r.y + r.height + Num(2)).value(), dpi))};
    }
    const double gap = fills::px(float_or_zero(op, "gap_mm", 0.3), dpi);
    limits::check_count(gap, static_cast<double>(limits::kSteps), "gap_mm");
    const int threshold = fill_threshold(op);
    const double expand = core::py_max(0.0, fills::px(float_or_zero(op, "expand_mm", 0.15), dpi));
    limits::check_count(expand, static_cast<double>(limits::kSteps), "expand_mm");
    const auto mask = fills::region_mask(ref, ax, ay, gap, threshold, expand, window);
    if (!mask) throw OpError("nothing to fill there (the click is on a line)");
    const std::vector<std::int64_t> rgb = rgb_of(op, doc);
    const double opacity = float_at(op, "opacity", 1.0);
    if (auto patch = fills::mask_patch(*mask, dpi, rgb, opacity)) page.layers[ti].patches.push_back(std::move(*patch));
}

void fill_area(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    const std::size_t ti = raster_paint_target(page, op);
    const Json area = core::op_area(op);
    std::optional<core::Patch> patch;
    if (core::truthy_at(area, "poly")) {
        const std::vector<std::int64_t> rgb = rgb_of(op, doc);
        const double opacity = float_at(op, "opacity", 1.0);
        patch = fills::polygon_patch(area["poly"], rgb, opacity);
    } else {
        const selection::AreaMask m = selection::area_mask(area);
        const std::vector<std::int64_t> rgb = rgb_of(op, doc);
        const double opacity = float_at(op, "opacity", 1.0);
        patch = fills::mask_patch(m.mask, fills::kFillDpi, rgb, opacity, {static_cast<double>(m.x0), static_cast<double>(m.y0)});
    }
    if (!patch) throw OpError("the area is empty");
    page.layers[ti].patches.push_back(std::move(*patch));
}

void fill_enclosed(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    const std::size_t ti = raster_paint_target(page, op);
    std::vector<std::pair<double, double>> poly;
    const Json* given = core::get(op, "poly");
    const std::vector<Json> items = core::iterate(given != nullptr && core::py_truthy(*given) ? *given : Json::array());
    limits::check_count(static_cast<double>(items.size()), static_cast<double>(limits::kPoints), "poly");
    for (const Json& p : items) {
        const double x = core::finite_float(core::subscript(p, 0), "poly");
        const double y = core::finite_float(core::subscript(p, 1), "poly");
        poly.emplace_back(x, y);
    }
    if (poly.size() < 3) throw OpError("poly needs three points or more (the lasso)");
    const int dpi = fills::kFillDpi;
    const std::string reference = core::truthy_at(op, "reference") ? core::py_str(op["reference"]) : "page";
    const Ignore ignore = ignored(op);
    const Image ref = fill_reference(doc, page, ti, reference, dpi, ignore);
    std::vector<double> xs;
    std::vector<double> ys;
    for (const auto& p : poly) xs.push_back(fills::px(p.first, dpi));
    for (const auto& p : poly) ys.push_back(fills::px(p.second, dpi));
    const double x0 = core::py_max(0.0, *std::min_element(xs.begin(), xs.end()) - 2);
    const double y0 = core::py_max(0.0, *std::min_element(ys.begin(), ys.end()) - 2);
    const double x1 = core::py_min(static_cast<double>(ref.width()), *std::max_element(xs.begin(), xs.end()) + 3);
    const double y1 = core::py_min(static_cast<double>(ref.height()), *std::max_element(ys.begin(), ys.end()) + 3);
    if (x1 - x0 < 3 || y1 - y0 < 3) throw OpError("the lasso is too small");
    const Box box{static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1), static_cast<int>(y1)};
    const BoolGrid walls = compare(ref.crop(box), fill_threshold(op));
    const double gap = fills::px(float_or_zero(op, "gap_mm", 0.3), dpi);
    limits::check_count(gap, static_cast<double>(limits::kSteps), "gap_mm");
    BoolGrid free = fills::dilate(walls, gap);
    for (unsigned char& v : free.cells) v = v != 0 ? 0 : 1;
    Image inside_img = Image::create("L", Size{box.width(), box.height()}, Ink(0));
    {
        std::vector<PointD> corners;
        for (std::size_t i = 0; i < xs.size(); ++i) corners.push_back(PointD{xs[i] - x0, ys[i] - y0});
        Draw(inside_img).polygon(corners, Ink(255));
    }
    const BoolGrid inside = compare(inside_img, 0, true);
    // outside the lasso nothing is a wall, and a free ring goes round the window: whatever reaches the ring without
    // crossing a line inside the lasso (an area running out of it, or leaking through a gap) is left
    BoolGrid open(free.width + 2, free.height + 2, true);
    for (int y = 0; y < free.height; ++y) {
        for (int x = 0; x < free.width; ++x) open.at(x + 1, y + 1) = (free.at(x, y) != 0 || inside.at(x, y) == 0) ? 1 : 0;
    }
    const BoolGrid reached = fills::region(open, 0, 0);
    BoolGrid enclosed(free.width, free.height);
    for (int y = 0; y < free.height; ++y) {
        for (int x = 0; x < free.width; ++x) {
            enclosed.at(x, y) = (free.at(x, y) != 0 && inside.at(x, y) != 0 && reached.at(x + 1, y + 1) == 0) ? 1 : 0;
        }
    }
    if (!enclosed.any()) {
        throw OpError("nothing closed inside the lasso (every area runs out of it, or leaks through a gap)");
    }
    const double expand = core::py_max(0.0, fills::px(float_or_zero(op, "expand_mm", 0.15), dpi));
    limits::check_count(expand, static_cast<double>(limits::kSteps), "expand_mm");
    enclosed = fills::dilate(enclosed, expand + gap);
    Image mask = Image::create("L", ref.size(), Ink(0));
    mask.paste(grid_image(enclosed), Point{box.x0, box.y0});
    const std::vector<std::int64_t> rgb = rgb_of(op, doc);
    const double opacity = float_at(op, "opacity", 1.0);
    if (auto patch = fills::mask_patch(mask, dpi, rgb, opacity)) page.layers[ti].patches.push_back(std::move(*patch));
}

void fill_gaps(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    const std::size_t ti = raster_paint_target(page, op);
    const int dpi = 150;
    const Size size = page_px(page, dpi);
    const Image painted =
        layer_image(page, page.layers[ti], dpi, &doc).getchannel(3).point([](int v) { return v > 40 ? 255 : 0; }).resize(size);
    if (!painted.getbbox()) throw OpError("the layer has no colour yet (fill first, then the spots left over)");
    // (Python draws the whole page here too, and never looks at it: not drawn here)
    const double reach = core::py_max(1.0, core::py_round_whole(float_at(op, "max_mm", 1.5) / 25.4 * dpi));
    limits::check_count(reach, limits::kReach, "max_mm");
    // close the painted shape (grow then shrink): what fills in is the small leftover
    const Image grown = painted.filter(Filter::gaussian_blur(reach)).point([](int v) { return v > 8 ? 255 : 0; });
    Image closed = grown.filter(Filter::gaussian_blur(reach)).point([](int v) { return v > 247 ? 255 : 0; });
    closed = chops::lighter(closed, painted);
    Image holes = chops::subtract(closed, painted);
    if (core::truthy_at(op, "area")) holes = chops::darker(holes, selection::to_mask(op["area"], page, &doc, dpi).resize(size));
    if (!holes.getbbox()) return;
    std::vector<std::int64_t> rgb;
    if (core::truthy_at(op, "rgb")) {
        rgb = core::int_tuple(op["rgb"]);
    } else {
        // the layer's commonest colour (Counter.most_common: of the commonest, the first seen)
        const Image image = layer_image(page, page.layers[ti], 60, &doc);
        const std::string pixels = image.tobytes();
        std::map<std::uint32_t, std::pair<std::size_t, std::size_t>> counts;  // colour → (count, first seen)
        for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
            if (static_cast<unsigned char>(pixels[i + 3]) <= 200) continue;
            const std::uint32_t key = (static_cast<std::uint32_t>(static_cast<unsigned char>(pixels[i])) << 16) |
                                      (static_cast<std::uint32_t>(static_cast<unsigned char>(pixels[i + 1])) << 8) |
                                      static_cast<std::uint32_t>(static_cast<unsigned char>(pixels[i + 2]));
            auto [it, added] = counts.emplace(key, std::pair<std::size_t, std::size_t>{0, i});
            (void)added;
            ++it->second.first;
        }
        if (counts.empty()) {
            rgb = {20, 20, 20};
        } else {
            auto best = counts.begin();
            for (auto it = counts.begin(); it != counts.end(); ++it) {
                if (it->second.first > best->second.first ||
                    (it->second.first == best->second.first && it->second.second < best->second.second)) {
                    best = it;
                }
            }
            rgb = {static_cast<std::int64_t>(best->first >> 16), static_cast<std::int64_t>((best->first >> 8) & 0xff),
                   static_cast<std::int64_t>(best->first & 0xff)};
        }
    }
    if (rgb.size() > 3) rgb.resize(3);
    const double opacity = float_at(op, "opacity", 1.0);
    if (auto patch = fills::mask_patch(holes, dpi, rgb, opacity)) page.layers[ti].patches.push_back(std::move(*patch));
}

// ImageDraw.floodfill(image, seed, value, thresh=8) on flood_fill's "RGB" picture, as Python's _flood_fill meets it:
// nothing for a value of fewer than three numbers (floodfill's IndexError, which it takes for a seed off the
// picture); a value of five or more Pillow refuses when it is written (TypeError) — Python then paints the panel
// with it, which fails the same way. A colour past 0..255 Pillow writes clipped, and clipped it can be the colour of
// the pixels it fills: refused here (COMP-01a). The colour written is then the value, which the seed is not within
// the threshold of, so a pixel filled is filled once.
void floodfill_rgb(Image& image, int x, int y, const std::vector<std::int64_t>& value) {
    std::string pixels = image.tobytes();
    const int w = image.width();
    const int h = image.height();
    const auto px = [&](int px_, int py_) {
        return reinterpret_cast<unsigned char*>(pixels.data() +
                                                (static_cast<std::size_t>(py_) * static_cast<std::size_t>(w) + static_cast<std::size_t>(px_)) * 3);
    };
    const unsigned char* seed = px(x, y);
    const std::array<int, 3> background{seed[0], seed[1], seed[2]};
    if (value.size() < 3) return;
    double diff = 0;
    for (std::size_t i = 0; i < 3; ++i) diff += std::fabs(static_cast<double>(value[i]) - background[i]);
    if (diff <= 8) return;  // (seed point already has fill color)
    if (value.size() > 4) throw core::PyTypeError("color must be int, or tuple of one, three or four elements");
    if (std::any_of(value.begin(), value.begin() + 3, [](std::int64_t v) { return v < 0 || v > 255; })) {
        throw OpError("rgb is [r, g, b], each 0..255");
    }
    const unsigned char ink[3] = {static_cast<unsigned char>(value[0]), static_cast<unsigned char>(value[1]),
                                  static_cast<unsigned char>(value[2])};
    const auto close = [&](const unsigned char* p) {
        return std::abs(p[0] - background[0]) + std::abs(p[1] - background[1]) + std::abs(p[2] - background[2]) <= 8;
    };
    std::vector<std::pair<int, int>> stack{{x, y}};
    std::memcpy(px(x, y), ink, 3);
    while (!stack.empty()) {
        const auto [cx, cy] = stack.back();
        stack.pop_back();
        const std::pair<int, int> next[4] = {{cx + 1, cy}, {cx - 1, cy}, {cx, cy + 1}, {cx, cy - 1}};
        for (const auto& [nx, ny] : next) {
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            unsigned char* p = px(nx, ny);
            if (!close(p)) continue;
            std::memcpy(p, ink, 3);
            stack.emplace_back(nx, ny);
        }
    }
    Image filled = Image::frombytes("RGB", image.size(), pixels);
    filled.set_transparency(image.transparency());  // (drawn on in place: the picture keeps its info)
    image = std::move(filled);
}

void flood_fill(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* layer_value = core::get(op, "layer");
    const LayerRole role =
        core::role_from(Json(layer_value != nullptr && core::py_truthy(*layer_value) ? core::py_str(*layer_value) : "ink"));
    if (role == LayerRole::Ink && !doc.page(at).name_ok && core::gated(doc)) throw OpError("ink flood_fill requires name_ok");
    const std::vector<std::int64_t> rgb = core::int_tuple(core::truthy_at(op, "rgb") ? op["rgb"] : Json::array({0, 0, 0}));
    const double x_mm = float_at(op, "x_mm", 0);
    const double y_mm = float_at(op, "y_mm", 0);
    const double gap_mm = float_at(op, "gap_mm", 0);
    Page& page = doc.edit_page(at);
    Layer& layer = page.layers[core::layer_for_role(page, role)];
    if (layer.locked) throw OpError("the layer is locked");  // (Python fills it)
    const int dpi = 72;
    Image image = layer.raster_png && !layer.raster_png->empty() ? selection::open_picture(*layer.raster_png).convert("RGB")
                                                                 : Image::create("RGB", page_px(page, dpi), Ink{255, 255, 255});
    if (gap_mm > 0) {
        const double radius = std::max(1.0, static_cast<double>(mm_to_px(gap_mm, dpi)));
        const double size = radius * 2 + 1;
        limits::check_count(size, static_cast<double>(limits::kRankSize), "gap_mm");
        image = image.filter(Filter::max_filter(static_cast<int>(size)));
    }
    const int sx = std::min(std::max(0, mm_to_px(x_mm, dpi)), image.width() - 1);
    const int sy = std::min(std::max(0, mm_to_px(y_mm, dpi)), image.height() - 1);
    floodfill_rgb(image, sx, sy, rgb);
    layer.kind = LayerKind::Raster;
    raster::save_raster(page, layer, image);  // (pages/NNN/<role>.png)
}

void gradient_fill(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    const std::size_t ti = raster_paint_target(page, op);
    // (fx, fy), (tx, ty) = [float(v) for v in op["from"][:2]], [float(v) for v in op["to"][:2]]
    const std::string message = "gradient_fill needs from and to: [x_mm, y_mm]";
    const auto two = [&](std::string_view key) {
        const Json* v = core::get(op, key);
        if (v == nullptr || !(v->is_array() || v->is_string())) throw OpError(message);
        const std::vector<Json> items = core::iterate(*v);
        std::vector<double> out;
        for (std::size_t i = 0; i < items.size() && i < 2; ++i) {
            double x = 0;
            try {
                x = core::to_float(items[i]);
            } catch (const core::Error&) {
                throw OpError(message);
            }
            if (!std::isfinite(x)) throw OpError(std::string(key) + " must be a finite number");
            out.push_back(x);
        }
        return out;
    };
    const std::vector<double> from = two("from");
    const std::vector<double> to = two("to");
    if (from.size() != 2 || to.size() != 2) throw OpError(message);
    if (core::py_hypot(to[0] - from[0], to[1] - from[1]) < 0.5) throw OpError("the gradient needs a longer drag");
    const int dpi = kGradientDpi;
    Image shown;
    std::int64_t x0 = 0;
    std::int64_t y0 = 0;
    if (core::truthy_at(op, "area")) {
        selection::AreaMask m = selection::area_mask(core::op_area(op), dpi);
        shown = std::move(m.mask);
        x0 = m.x0;
        y0 = m.y0;
    } else {
        const double w = core::py_round_whole(page.spec.width_mm.value() / 25.4 * dpi);
        const double h = core::py_round_whole(page.spec.height_mm.value() / 25.4 * dpi);
        limits::check_sides(w, h, "the page");
        shown = Image::create("L", Size{static_cast<int>(std::max(0.0, w)), static_cast<int>(std::max(0.0, h))}, Ink(255));
    }
    const Json* shape = core::get(op, "shape");
    if (shape != nullptr && !shape->is_null() && *shape != Json("linear") && *shape != Json("radial") && *shape != Json("ellipse")) {
        throw OpError("shape must be linear, radial or ellipse");
    }
    Json spec = Json::object();
    spec["from"] = Json::array({from[0], from[1]});
    spec["to"] = Json::array({to[0], to[1]});
    spec["shape"] = shape != nullptr && core::py_truthy(*shape) ? *shape : Json("linear");
    spec["rgb_from"] = core::truthy_at(op, "rgb_from") ? op["rgb_from"] : Json::array({20, 20, 20});
    spec["rgb_to"] = core::truthy_at(op, "rgb_to")     ? op["rgb_to"]
                     : core::truthy_at(op, "rgb_from") ? op["rgb_from"]
                                                       : Json::array({20, 20, 20});
    spec["opacity_from"] = core::get_or(op, "opacity_from", Json(1.0));
    spec["opacity_to"] = core::get_or(op, "opacity_to", Json(core::truthy_at(op, "rgb_to") ? 1.0 : 0.0));
    const Json extras = core::gradient_extras(op);
    for (const auto& [key, value] : extras.items()) spec[key] = value;
    // (an opacity Python reads as NaN or an infinity: refused; Python's float() errors as they are)
    for (const char* key : {"opacity_from", "opacity_to"}) {
        if (const Json* v = core::get(spec, key); v != nullptr && !core::truthy_at(spec, "stops")) {
            const double x = core::to_float(*v);
            if (!std::isfinite(x)) throw OpError(std::string(key) + " must be a finite number");
        }
    }
    const detail::GradientColours colours(spec);
    const int w = shown.width();
    const int h = shown.height();
    const double scale = dpi / 25.4;
    const std::string alpha_in = shown.tobytes();
    std::string data(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, '\0');
    std::vector<double> gxs(static_cast<std::size_t>(w));
    for (int i = 0; i < w; ++i) gxs[static_cast<std::size_t>(i)] = (static_cast<double>(i + x0) + 0.5) / scale;
    std::size_t k = 0;
    for (int j = 0; j < h; ++j) {
        const double gy = (static_cast<double>(j + y0) + 0.5) / scale;
        for (int i = 0; i < w; ++i) {
            unsigned char rgb[3];
            double share = 0;
            colours.at(gxs[static_cast<std::size_t>(i)], gy, rgb, share);
            const std::size_t px = static_cast<std::size_t>(j) * static_cast<std::size_t>(w) + static_cast<std::size_t>(i);
            // (share * np.asarray(shown, dtype=float)).round().astype("uint8")
            const double alpha = std::nearbyint(share * static_cast<double>(static_cast<unsigned char>(alpha_in[px])));
            if (!(alpha >= 0 && alpha <= 255)) throw OpError("from and to of the gradient are too far apart or off the page");
            data[k++] = static_cast<char>(rgb[0]);
            data[k++] = static_cast<char>(rgb[1]);
            data[k++] = static_cast<char>(rgb[2]);
            data[k++] = static_cast<char>(static_cast<unsigned char>(static_cast<int>(alpha)));
        }
    }
    const Image image = Image::frombytes("RGBA", Size{w, h}, data);
    core::Patch templ;
    templ.attrs["mode"] = "image";
    templ.attrs["opacity"] = 1.0;
    auto patch = selection::to_patch(image, x0, y0, templ, dpi);
    if (!patch) throw OpError("the gradient has nothing to show there");
    page.layers[ti].patches.push_back(std::move(*patch));
}

// --- masks --------------------------------------------------------------------------------------------------------

// ops._mask_image: the layer's mask over the page at MASK_DPI ("L"; all white — everything shows — when it has none)
Image mask_image(const Page& page, const Layer& layer) {
    const double w = std::max(1.0, core::py_round_whole(page.spec.width_mm.value() / 25.4 * kMaskDpi));
    const double h = std::max(1.0, core::py_round_whole(page.spec.height_mm.value() / 25.4 * kMaskDpi));
    limits::check_sides(w, h, "the page");
    const Size size{static_cast<int>(w), static_cast<int>(h)};
    if (layer.mask && layer.mask->png && !layer.mask->png->empty()) {
        return selection::open_picture(*layer.mask->png).convert("L").resize(size);
    }
    return Image::create("L", size, Ink(255));
}

core::Mask mask_of(const Image& image, bool enabled) {
    core::Mask mask;
    mask.png = fills::png_bytes(image);
    mask.enabled = enabled;
    return mask;
}

void set_layer_mask(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    Layer& layer = page.layers[core::layer_by_id(page, core::truthy_at(op, "id") ? core::py_str(op["id"]) : std::string())];
    if (layer.locked) throw OpError("the layer is locked");  // (Python masks it)
    if (layer.kind == LayerKind::Folder) throw OpError("a folder cannot take a mask (mask the layers in it)");
    if (core::truthy_at(op, "delete")) {
        layer.mask.reset();
        return;
    }
    Image image = mask_image(page, layer);
    if (core::truthy_at(op, "area")) {
        const selection::AreaMask shown = selection::area_mask(op["area"], kMaskDpi);
        image = Image::create("L", image.size(), Ink(0));
        image.paste(shown.mask, Point{static_cast<int>(shown.x0), static_cast<int>(shown.y0)});
    } else if (core::truthy_at(op, "fill")) {
        const Json& fill = op["fill"];
        if (fill != Json("show") && fill != Json("hide")) throw OpError("fill must be show or hide");
        image = Image::create("L", image.size(), Ink(fill == Json("show") ? 255 : 0));
    }
    if (core::truthy_at(op, "invert")) image = ops::invert(image);
    const bool enabled = core::has(op, "enabled") ? core::py_truthy(op["enabled"]) : (layer.mask ? layer.mask->enabled : true);
    layer.mask = mask_of(image, enabled);
}

void paint_mask(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    Layer& layer = page.layers[core::layer_by_id(page, core::truthy_at(op, "id") ? core::py_str(op["id"]) : std::string())];
    if (layer.locked) throw OpError("the layer is locked");  // (Python masks it)
    if (layer.kind == LayerKind::Folder) throw OpError("a folder cannot take a mask (mask the layers in it)");
    const Json* raw = core::get(op, "points");
    core::PenPoints points = core::parse_points(raw != nullptr && core::py_truthy(*raw) ? *raw : Json::array());
    if (points.empty()) throw OpError("points needs at least one [x_mm, y_mm] pair");
    limits::check_count(static_cast<double>(points.size()), static_cast<double>(limits::kPoints), "points");
    for (const core::PenPoint& p : points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw OpError("points must be a finite number");
        limits::check_coordinate(p.x / 25.4 * kMaskDpi, "points");
        limits::check_coordinate(p.y / 25.4 * kMaskDpi, "points");
    }
    Image image = mask_image(page, layer);
    if (points.size() == 1) points = {points[0], core::PenPoint{points[0].x + 0.01, points[0].y + 0.01, std::nullopt}};
    core::PenPoints flat;  // the mask takes the whole width, whatever the pressure
    for (const core::PenPoint& p : points) flat.push_back(core::PenPoint{p.x, p.y, std::nullopt});
    const Json* width_value = core::get(op, "width_mm");
    const double width = width_value != nullptr && core::py_truthy(*width_value) ? core::finite_float(*width_value, "width_mm") : 3.0;
    limits::check_count(std::fabs(width) / 25.4 * kMaskDpi / 2, limits::kReach, "width_mm");
    const int value = core::py_truthy(core::get_or(op, "show", Json(true))) ? 255 : 0;
    {
        Draw draw(image);
        draw_stroke_mm(draw, flat, kMaskDpi, width, Ink(value), false);
    }
    layer.mask = mask_of(image, layer.mask ? layer.mask->enabled : true);
}

// --- several layers into one ------------------------------------------------------------------------------------------

bool core_role(LayerRole role) {
    return role == LayerRole::Name || role == LayerRole::Ink || role == LayerRole::Bg || role == LayerRole::Finish;
}

// layerops._check_mergeable
void check_mergeable(const Page& page, const std::vector<std::size_t>& layers) {
    for (const std::size_t i : layers) {
        const Layer& layer = page.layers[i];
        if (layer.kind == LayerKind::Placed || (layer.tone && core::py_truthy(*layer.tone)) || layer.kind == LayerKind::Tone) {
            throw OpError("placed images and tones cannot be merged");
        }
        if (layer.locked) throw OpError("the layer is locked");
    }
}

// layerops._into_pixels: the layer becomes a paint layer holding the picture
void into_pixels(const Page& page, Layer& target, const Image& picture) {
    target.strokes = core::empty_strokes();
    target.patches.clear();
    target.mask.reset();
    target.kind = LayerKind::Raster;
    target.fill.reset();
    target.adjust.reset();
    target.effect.reset();
    target.fill_rgb.reset();
    target.blend = "normal";
    target.opacity = 1.0;
    target.clip = false;
    raster::save_raster(page, target, picture);
}

// layerops._composite: the layers drawn over each other as they show (blend, opacity, clip), over transparency
Image composite_layers(const Document& doc, const Page& page, const std::vector<std::size_t>& layers, int dpi) {
    const Size size = page_px(page, dpi);
    Image out = Image::create("RGBA", size, Ink{0, 0, 0, 0});
    std::optional<Image> prev;
    std::vector<std::string> omitted;
    const detail::Ctx ctx = context(page, &doc, dpi, size, omitted);
    for (const std::size_t i : layers) {
        const Layer& layer = page.layers[i];
        if (layer.kind == LayerKind::Folder) continue;
        if (layer.kind == LayerKind::Adjust) {
            out = detail::adjusted(ctx, std::move(out), layer, layer.clip && prev ? &*prev : nullptr, Box{0, 0, size.width, size.height});
            continue;
        }
        const Image picture = layer_image(page, layer, dpi, &doc);
        const double opacity = layer.opacity;
        const Image* clip = layer.clip && prev ? &*prev : nullptr;
        const Image below_alpha = out.getchannel(3);
        const std::string blend = layer.blend.empty() ? "normal" : layer.blend;
        out = detail::blend_over(out, picture, blend, opacity, clip);
        if (blend != "normal") {  // where only this layer is, it still shows
            Image shown = picture.getchannel(3);
            if (clip != nullptr) shown = chops::multiply(shown, *clip);
            out.putalpha(chops::lighter(below_alpha, shown.point([opacity](int v) { return static_cast<int>(core::py_trunc_int(v * opacity)); })));
        }
        prev = picture.getchannel(3);
    }
    return out;
}

// layerops._ids
std::vector<std::string> layer_ids(const Json& op) {
    std::vector<std::string> ids;
    const Json* given = core::get(op, "ids");
    if (given != nullptr && core::py_truthy(*given)) {
        for (const Json& v : core::iterate(*given)) ids.push_back(core::py_str(v));
    }
    if (ids.empty()) throw OpError("ids is the list of layer ids");
    return ids;
}

// layerops._layers: the positions of the layers named, in page order
std::vector<std::size_t> layers_named(const Page& page, const std::vector<std::string>& ids) {
    for (const std::string& id : ids) {
        const bool found = std::any_of(page.layers.begin(), page.layers.end(), [&](const Layer& l) { return l.id == id; });
        if (!found) throw OpError("no layer " + id);
    }
    const std::set<std::string> wanted(ids.begin(), ids.end());
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (wanted.contains(page.layers[i].id)) out.push_back(i);
    }
    return out;
}

// The layers after the merge: the target kept, the others emptied (a core layer) or gone.
void after_merge(Page& page, const std::vector<std::size_t>& merged) {
    std::vector<bool> gone(page.layers.size(), false);
    for (std::size_t k = 1; k < merged.size(); ++k) {
        Layer& layer = page.layers[merged[k]];
        if (core_role(layer.role)) {  // layerops._empty
            layer.strokes = core::empty_strokes();
            layer.patches.clear();
            layer.raster_png.reset();
        } else {
            gone[merged[k]] = true;
        }
    }
    std::vector<Layer> kept;
    kept.reserve(page.layers.size());
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (!gone[i]) kept.push_back(std::move(page.layers[i]));
    }
    page.layers = std::move(kept);
}

void merge_layers(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const std::vector<std::string> ids = layer_ids(op);
    Page& page = doc.edit_page(at);
    std::vector<std::size_t> layers;
    for (const std::size_t i : layers_named(page, ids)) {
        if (page.layers[i].kind != LayerKind::Folder) layers.push_back(i);
    }
    if (layers.size() < 2) throw OpError("choose two or more layers to merge");
    check_mergeable(page, layers);
    const Image picture = composite_layers(doc, page, layers, kWorkingDpi);
    Layer& target = page.layers[layers.front()];
    into_pixels(page, target, picture);
    if (core::truthy_at(op, "name")) target.title = core::py_str(op["name"]);
    after_merge(page, layers);
}

// layerops._parents_visible
bool parents_visible(const Page& page, const Layer& layer) {
    const auto by_id = [&page](const Json& id) -> const Layer* {
        if (!id.is_string()) return nullptr;
        const Layer* found = nullptr;
        for (const Layer& item : page.layers) {
            if (item.id == id.get_ref<const std::string&>()) found = &item;
        }
        return found;
    };
    const Layer* parent = by_id(layer.parent_id);
    std::set<std::string> seen;
    while (parent != nullptr && !seen.contains(parent->id)) {
        if (!parent->visible) return false;
        seen.insert(parent->id);
        parent = by_id(parent->parent_id);
    }
    return true;
}

void merge_visible(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    std::vector<std::size_t> shown;
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        const Layer& layer = page.layers[i];
        if (layer.visible && layer.kind != LayerKind::Folder && layer.role != LayerRole::Name && layer.role != LayerRole::Draft &&
            parents_visible(page, layer)) {
            shown.push_back(i);
        }
    }
    if (shown.empty()) throw OpError("no layer is showing");
    const Image picture = composite_layers(doc, page, shown, kWorkingDpi);
    if (core::py_truthy(core::get_or(op, "copy", Json(true)))) {
        Layer layer;
        layer.id = core::truthy_at(op, "id") ? core::py_str(op["id"]) : core::new_id();
        layer.role = LayerRole::User;
        layer.kind = LayerKind::Raster;
        layer.title = core::truthy_at(op, "name") ? core::py_str(op["name"]) : std::string("表示レイヤーのコピー");
        layer.exportable = true;
        for (const Layer& item : page.layers) {
            if (item.id == layer.id) throw OpError("layer " + layer.id + " exists");
        }
        raster::save_raster(page, layer, picture);
        page.layers.push_back(std::move(layer));
        return;
    }
    check_mergeable(page, shown);
    into_pixels(page, page.layers[shown.front()], picture);
    after_merge(page, shown);
}

// --- Python's repr of a Layer (merge_down's error for a folder, which is not among the layers it looks in) ------------

std::string bytes_repr(std::string_view bytes) {
    const bool single = bytes.find('\'') != std::string_view::npos;
    const bool dbl = bytes.find('"') != std::string_view::npos;
    const char quote = single && !dbl ? '"' : '\'';
    std::string out = "b";
    out += quote;
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == static_cast<unsigned char>(quote) || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c < 0x20 || c >= 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += quote;
    return out;
}

std::string opt_str_repr(const std::optional<std::string>& v) { return v ? core::py_repr_str(*v) : "None"; }
std::string opt_json_repr(const std::optional<Json>& v) { return v ? core::py_repr(*v) : "None"; }

template <class T, class F>
std::string tuple_repr(const std::vector<T>& values, F item) {
    std::string out = "(";
    for (std::size_t i = 0; i < values.size(); ++i) out += (i == 0 ? "" : ", ") + item(values[i]);
    if (values.size() == 1) out += ",";
    return out + ")";
}

std::string stroke_repr(const core::Stroke& s) {
    std::string points = "[";
    for (std::size_t i = 0; i < s.points.size(); ++i) {
        points += (i == 0 ? "(" : ", (") + core::py_float_repr(s.points[i].x) + ", " + core::py_float_repr(s.points[i].y) + ")";
    }
    points += "]";
    const auto floats = [](const std::vector<double>& v) {
        std::string out = "[";
        for (std::size_t i = 0; i < v.size(); ++i) out += (i == 0 ? "" : ", ") + core::py_float_repr(v[i]);
        return out + "]";
    };
    const std::string rgb =
        s.rgb ? tuple_repr(*s.rgb, [](std::int64_t v) { return std::to_string(v); }) : std::string("None");
    return "Stroke(id=" + core::py_repr_str(s.id) + ", points=" + points + ", pressure=" + floats(s.pressure) +
           ", width_mm=" + core::py_float_repr(s.width_mm) + ", kind=" + core::py_repr_str(s.kind) + ", handles=None, rgb=" + rgb +
           ", opacity=" + core::py_float_repr(s.opacity) + ", rotation=" + floats(s.rotation) +
           ", pressure_opacity=" + core::py_float_repr(s.pressure_opacity) + ")";
}

std::string enum_repr(std::string_view type, std::string_view value) {
    std::string name(value);
    for (char& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return "<" + std::string(type) + "." + name + ": '" + std::string(value) + "'>";
}

std::string layer_repr(const Layer& l) {
    const auto num = [](const std::optional<Num>& v) { return v ? v->repr() : std::string("None"); };
    std::string strokes = "[";
    for (std::size_t i = 0; i < l.stroke_count(); ++i) strokes += (i == 0 ? "" : ", ") + stroke_repr(*l.strokes->items[i]);
    strokes += "]";
    std::string region = "None";
    if (l.region) {
        region = "[";
        for (std::size_t i = 0; i < l.region->size(); ++i) {
            region += (i == 0 ? "(" : ", (") + (*l.region)[i].x.repr() + ", " + (*l.region)[i].y.repr() + ")";
        }
        region += "]";
    }
    std::string patches = "[";
    for (std::size_t i = 0; i < l.patches.size(); ++i) {
        const core::Patch& p = l.patches[i];
        std::string item = "{";
        bool first = true;
        const auto add = [&](const std::string& key, const std::string& value) {
            item += (first ? "" : ", ") + core::py_repr_str(key) + ": " + value;
            first = false;
        };
        for (const auto& [key, value] : p.attrs.items()) add(key, core::py_repr(value));
        if (p.png) add("png", bytes_repr(*p.png));
        if (p.asset) add("asset", core::py_repr_str(*p.asset));
        for (const auto& [key, value] : p.after_asset.items()) add(key, core::py_repr(value));
        patches += (i == 0 ? "" : ", ") + item + "}";
    }
    patches += "]";
    const std::string fill_rgb = l.fill_rgb ? tuple_repr(*l.fill_rgb, [](const Num& v) { return v.repr(); }) : std::string("None");
    const std::string placement = l.placement_mm ? "Rect(x=" + l.placement_mm->x.repr() + ", y=" + l.placement_mm->y.repr() +
                                                       ", width=" + l.placement_mm->width.repr() + ", height=" +
                                                       l.placement_mm->height.repr() + ")"
                                                 : std::string("None");
    const std::string mask = l.mask ? "{'png': " + (l.mask->png ? bytes_repr(*l.mask->png) : std::string("None")) +
                                          ", 'enabled': " + (l.mask->enabled ? "True" : "False") + "}"
                                    : std::string("None");
    const std::string color = l.color ? tuple_repr(*l.color, [](std::int64_t v) { return std::to_string(v); }) : std::string("None");
    const auto b = [](bool v) { return std::string(v ? "True" : "False"); };
    return "Layer(id=" + core::py_repr_str(l.id) + ", role=" + enum_repr("LayerRole", core::to_string(l.role)) +
           ", kind=" + enum_repr("LayerKind", core::to_string(l.kind)) + ", visible=" + b(l.visible) + ", exportable=" + b(l.exportable) +
           ", strokes=" + strokes + ", raster_relpath=" + opt_str_repr(l.raster_relpath) + ", fill_rgb=" + fill_rgb +
           ", lpi=" + num(l.lpi) + ", density=" + num(l.density) + ", region=" + region + ", opacity=" + core::py_float_repr(l.opacity) +
           ", material_id=" + opt_str_repr(l.material_id) + ", angle=" + core::py_float_repr(l.angle) + ", title=" + core::py_repr_str(l.title) +
           ", blend=" + core::py_repr_str(l.blend) + ", clip=" + b(l.clip) + ", lock_alpha=" + b(l.lock_alpha) + ", locked=" + b(l.locked) +
           ", panel_clip=" + b(l.panel_clip) + ", panel_each=" + b(l.panel_each) + ", patches=" + patches + ", tone=" + opt_json_repr(l.tone) +
           ", parent_id=" + core::py_repr(l.parent_id) + ", asset=" + opt_str_repr(l.asset) + ", frame_id=" + opt_str_repr(l.frame_id) +
           ", placement_mm=" + placement + ", fit=" + core::py_repr_str(l.fit) + ", clip_to=" + core::py_repr_str(l.clip_to) +
           ", source=" + opt_json_repr(l.source) + ", finish=" + opt_json_repr(l.finish) + ", mask=" + mask + ", color=" + color +
           ", reference=" + b(l.reference) + ", fill=" + opt_json_repr(l.fill) + ", adjust=" + opt_json_repr(l.adjust) +
           ", effect=" + opt_json_repr(l.effect) + ", color_prints=" + b(l.color_prints) + ", screen=" + opt_json_repr(l.screen) + ")";
}

void merge_down(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    const std::size_t ui = core::layer_by_id(page, core::truthy_at(op, "id") ? core::py_str(op["id"]) : std::string());
    // ops._merge_down: into the layer below it in the same folder
    std::vector<std::size_t> siblings;
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        const Layer& item = page.layers[i];
        if (item.parent_id == page.layers[ui].parent_id && item.kind != LayerKind::Folder) siblings.push_back(i);
    }
    const auto found = std::find(siblings.begin(), siblings.end(), ui);
    if (found == siblings.end()) throw core::PyValueError(layer_repr(page.layers[ui]) + " is not in list");
    if (found == siblings.begin()) throw OpError("there is no layer below to merge into");
    const std::size_t li = *(found - 1);
    for (const std::size_t i : {ui, li}) {
        const Layer& item = page.layers[i];
        if (item.kind == LayerKind::Placed || item.kind == LayerKind::Tone || (item.tone && core::py_truthy(*item.tone))) {
            throw OpError("placed images and tones cannot be merged");
        }
        if (item.locked) throw OpError("the layer is locked");
    }
    const Layer& upper = page.layers[ui];
    const Layer& lower = page.layers[li];
    const bool has_raster = [](const Layer& l) { return l.raster_png && !l.raster_png->empty(); }(upper) ||
                            [](const Layer& l) { return l.raster_png && !l.raster_png->empty(); }(lower);
    const std::string blend = upper.blend.empty() ? "normal" : upper.blend;
    const bool plain = upper.kind == LayerKind::Strokes && lower.kind == LayerKind::Strokes && !has_raster && !upper.mask &&
                       !lower.mask && blend == "normal" && !upper.clip && upper.opacity >= 1 && upper.panel_clip == lower.panel_clip &&
                       upper.panel_each == lower.panel_each && upper.color == lower.color;
    if (plain) {
        Layer& into = page.layers[li];
        std::vector<core::StrokePtr> strokes = into.strokes->items;
        strokes.insert(strokes.end(), upper.strokes->items.begin(), upper.strokes->items.end());
        into.strokes = core::make_strokes(std::move(strokes));
        into.patches.insert(into.patches.end(), upper.patches.begin(), upper.patches.end());
    } else {
        const int dpi = kWorkingDpi;
        const Image below = layer_image(page, lower, dpi, &doc);
        const Image above = layer_image(page, upper, dpi, &doc);
        const std::optional<Image> clip = upper.clip ? std::optional<Image>(below.getchannel(3)) : std::nullopt;
        const double opacity = upper.opacity;
        Image merged = detail::blend_over(below, above, blend, opacity, clip ? &*clip : nullptr);
        if (blend != "normal") {
            // (a blend mode keeps the lower alpha; where only the upper layer is, it still shows)
            Image shown = clip ? chops::multiply(above.getchannel(3), *clip) : above.getchannel(3);
            shown = shown.point([opacity](int v) { return static_cast<int>(core::py_trunc_int(v * opacity)); });
            merged.putalpha(chops::lighter(below.getchannel(3), shown));
        }
        Layer& into = page.layers[li];
        into.strokes = core::empty_strokes();
        into.patches.clear();
        into.mask.reset();
        into.kind = LayerKind::Raster;
        raster::save_raster(page, into, merged);
    }
    page.layers.erase(page.layers.begin() + static_cast<std::ptrdiff_t>(ui));
}

void convert_layer(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    const std::vector<std::size_t> named =
        layers_named(page, {core::truthy_at(op, "id") ? core::py_str(op["id"]) : std::string()});
    Layer& layer = page.layers[named.front()];
    const std::string to = core::truthy_at(op, "to") ? core::py_str(op["to"]) : std::string();
    if (layer.locked) throw OpError("the layer is locked");
    if (to == "paint") {
        if (layer.kind == LayerKind::Folder || layer.kind == LayerKind::Adjust || layer.kind == LayerKind::Tone ||
            (layer.tone && core::py_truthy(*layer.tone))) {
            throw OpError("this layer cannot become a paint layer");
        }
        Image picture = layer_image(page, layer, kWorkingDpi, &doc);
        if (layer.kind == LayerKind::Fill && ((layer.fill && core::py_truthy(*layer.fill)) || (layer.fill_rgb && !layer.fill_rgb->empty()))) {
            picture = detail::fill_layer_image(layer, picture.size(), kWorkingDpi, false, Box{0, 0, picture.width(), picture.height()});
        }
        const std::string blend = layer.blend;
        const double opacity = layer.opacity;
        const bool clip = layer.clip;
        into_pixels(page, layer, picture);  // (the mask is drawn into the pixels)
        layer.blend = blend;
        layer.opacity = opacity;
        layer.clip = clip;
        layer.asset.reset();
        return;
    }
    if (to == "pen") {
        if (layer.kind != LayerKind::Raster && layer.kind != LayerKind::Strokes) throw OpError("only a paint layer can become a pen layer");
        const Image picture = layer_image(page, layer, kWorkingDpi, &doc);
        const double min_mm = float_at(op, "min_mm", 0.8);
        std::vector<core::Stroke> traced = vectorize::trace_layer(picture, kWorkingDpi, min_mm);
        if (traced.empty()) throw OpError("the layer has no marks to trace");
        std::vector<core::StrokePtr> strokes;
        for (core::Stroke& s : traced) strokes.push_back(std::make_shared<const core::Stroke>(std::move(s)));
        layer.strokes = core::make_strokes(std::move(strokes));
        layer.patches.clear();
        layer.raster_png.reset();
        layer.kind = LayerKind::Strokes;
        return;
    }
    throw OpError("to must be paint or pen");
}

// --- the selection's ops ------------------------------------------------------------------------------------------------

// ops._interp
Resample interp(const Json& op) {
    const std::string kind = core::truthy_at(op, "interp") ? core::py_str(op["interp"]) : "bilinear";
    if (kind == "nearest") return Resample::Nearest;
    if (kind == "bilinear") return Resample::Bilinear;
    if (kind == "bicubic") return Resample::Bicubic;
    throw OpError("interp must be nearest, bilinear or bicubic");
}

// tuple(float(v) for v in op.get("matrix") or IDENTITY)
std::vector<double> matrix_of(const Json& op) {
    if (!core::truthy_at(op, "matrix")) return {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    std::vector<double> out;
    for (const Json& v : core::iterate(op["matrix"])) out.push_back(core::finite_float(v, "matrix"));
    return out;
}

void delete_or_transform_area(OpContext& c, bool transform) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    const std::size_t ti = raster_paint_target(page, op);
    const Json area = core::op_area(op);
    selection::Items items = selection::lift(page.layers[ti], area, page);
    if (!transform) return;
    Layer& target = page.layers[ti];
    if (core::truthy_at(op, "warp")) {
        const warp::Go go = warp::mapping(selection::area_bbox(area), op["warp"]);
        selection::drop_warped(target, std::move(items), go, interp(op));
        return;
    }
    const std::vector<double> m = matrix_of(op);
    if (m.size() != 6) throw OpError("matrix is [a, b, c, d, e, f]");
    const Resample resample = interp(op);
    try {
        selection::drop(target, std::move(items), selection::Matrix{m[0], m[1], m[2], m[3], m[4], m[5]}, false, resample);
    } catch (const core::PyUncaught&) {
        throw;
    } catch (const core::Error& error) {
        if (error.code() != "value") throw;
        throw OpError(error.what());  // (Python's ValueError becomes the op's error here)
    }
}

void delete_area(OpContext& c) { delete_or_transform_area(c, false); }
void transform_area(OpContext& c) { delete_or_transform_area(c, true); }

void validate_patch_items(const selection::Items& items,bool bounded=false){
    for (const core::Patch& patch : items.patches) {
        const Json* box = patch.attrs.contains("box") ? &patch.attrs["box"] : core::get(patch.after_asset, "box");
        bool good = box != nullptr && box->is_array() && box->size() == 4;
        if (good) {
            for (const Json& v : *box) {
                try {
                    good = good && std::isfinite(core::to_float(v));
                } catch (const core::Error&) {
                    good = false;
                }
            }
        }
        if(good && bounded){
            for(const Json& v:*box)limits::check_coordinate(core::to_float(v)*selection::kWorkingDpi/25.4,"material patch box");
        }
        if (good && patch.png) {
            try {
                (void)selection::open_picture(*patch.png);
            } catch (const core::Error&) {
                good = false;
            }
        }
        if (!good || !patch.png) throw OpError("items: a patch needs a box [x, y, w, h] (mm) and a png (a picture, base64)");
    }
}

void paste(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    const std::size_t ti = raster_paint_target(page, op);
    selection::Items items = selection::items_from_json(core::truthy_at(op, "items") ? op["items"] : Json::object());
    if (items.strokes.empty() && items.patches.empty()) throw OpError("nothing to paste");
    validate_patch_items(items);
    const std::vector<double> m = matrix_of(op);
    if (m.size() != 6) {
        throw core::PyValueError(m.size() < 6 ? "not enough values to unpack (expected 6, got " + std::to_string(m.size()) + ")"
                                              : "too many values to unpack (expected 6)");
    }
    selection::drop(page.layers[ti], std::move(items), selection::Matrix{m[0], m[1], m[2], m[3], m[4], m[5]}, true);
}

// --- pixels in --------------------------------------------------------------------------------------------------------

// The repr of a chunk type, as Pillow's messages give it (b'IDAT')
std::string cid_repr(std::string_view cid) { return bytes_repr(cid); }

bool is_cid(std::string_view cid) {
    if (cid.size() != 4) return false;
    return std::all_of(cid.begin(), cid.end(), [](char ch) {
        const auto c = static_cast<unsigned char>(ch);
        return std::isalnum(c) != 0 || c == '_';
    });
}

std::uint32_t be32(std::string_view b, std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3]));
}

std::uint32_t crc32_of(std::string_view data, std::uint32_t crc = 0) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) != 0 ? 0xedb88320U ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const char ch : data) crc = table[(crc ^ static_cast<unsigned char>(ch)) & 0xff] ^ (crc >> 8);
    return ~crc;
}

[[noreturn]] void unreadable(const std::string& why) { throw OpError("not a readable image: " + why); }

// ops._verify_image: what Pillow's Image.open and verify() say of the bytes — for a PNG, its chunks up to the image
// data read with their checksums (a broken one: the file is not identified), the size Pillow refuses, then the
// checksums of the rest — and the size Python refuses; then its pixels decoded. Other formats Pillow reads come with
// a later step. (A PNG of a bit depth Pillow cannot read, or whose pixels cannot be decoded, is refused here, where
// Python keeps it and can never draw the page again.)
void verify_image(const std::string& blob) {
    const std::string_view b = blob;
    if (b.substr(0, 3) == "\xff\xd8\xff" || b.substr(0, 2) == "BM" ||
        b.substr(0, 6) == "GIF87a" || b.substr(0, 6) == "GIF89a") {
        try {
            // Apply the operation's stricter cap before JPEG/BMP/GIF decoding and pixel allocation.
            (void)open_image(blob, PngLimits{kOpsMaxImagePixels});
        } catch (const NotYetPorted&) {
            throw;
        } catch (const core::Error& error) {
            if (error.code() == "unidentified_image") unreadable("cannot identify image file <_io.BytesIO object>");
            unreadable(error.what());
        }
        return;
    }
    static constexpr std::string_view kSignature("\x89PNG\r\n\x1a\n", 8);
    const std::string unknown = "cannot identify image file <_io.BytesIO object>";
    if (b.substr(0, 8) != kSignature) {
        try {
            (void)open_image(blob);
        } catch (const NotYetPorted&) {
            throw;
        } catch (const core::Error&) {
            unreadable(unknown);
        }
        unreadable(unknown);
    }
    std::size_t pos = 8;
    struct Header {
        std::string cid;
        std::size_t start = 0;
        std::uint32_t length = 0;
        bool short_header = false;  // (fewer than four bytes: Python's struct.error)
    };
    // ChunkStream.read
    const auto header = [&]() {
        Header h;
        const std::string_view s = b.substr(std::min(pos, b.size()), 8);
        pos += s.size();
        h.cid = std::string(s.size() > 4 ? s.substr(4) : std::string_view());
        h.start = pos;
        if (s.size() < 4) {
            h.short_header = true;
            return h;
        }
        h.length = be32(s, 0);
        return h;
    };
    // ImageFile._safe_read
    const auto safe_read = [&](std::uint32_t length) -> std::string_view {
        if (b.size() - std::min(pos, b.size()) < length) unreadable("Truncated File Read");
        const std::string_view data = b.substr(pos, length);
        pos += length;
        return data;
    };
    // ChunkStream.crc: the SyntaxError's words, if any
    const auto checksum = [&](const std::string& cid, std::string_view data) -> std::optional<std::string> {
        const std::string_view crc = b.substr(std::min(pos, b.size()), 4);
        pos += crc.size();
        if (crc.size() < 4) return "broken PNG file (incomplete checksum in " + cid_repr(cid) + ")";
        if (crc32_of(data, crc32_of(cid)) != be32(crc, 0)) return "broken PNG file (bad header checksum in " + cid_repr(cid) + ")";
        return std::nullopt;
    };
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::optional<std::size_t> idat;
    for (;;) {  // PngImageFile._open: the chunks up to the first IDAT (or IEND)
        const Header h = header();
        if (h.short_header || !is_cid(h.cid)) unreadable(unknown);
        if (h.cid == "IDAT") {
            idat = h.start;
            break;
        }
        if (h.cid == "IEND") break;
        const std::string_view data = safe_read(h.length);
        if (h.cid == "IHDR") {
            if (h.length < 13) unreadable("Truncated IHDR chunk");
            width = be32(data, 0);
            height = be32(data, 4);
            static constexpr std::pair<int, int> kModes[] = {{1, 0}, {2, 0}, {4, 0}, {8, 0}, {16, 0}, {8, 2}, {16, 2}, {1, 3},
                                                             {2, 3}, {4, 3}, {8, 3}, {8, 4}, {16, 4}, {8, 6}, {16, 6}};
            const std::pair<int, int> mode{static_cast<unsigned char>(data[8]), static_cast<unsigned char>(data[9])};
            if (std::find(std::begin(kModes), std::end(kModes), mode) == std::end(kModes)) unreadable(unknown);
            if (data[11] != 0) unreadable(unknown);  // (SyntaxError "unknown filter category")
        }
        if (checksum(h.cid, data)) unreadable(unknown);
    }
    // Image._decompression_bomb_check (two sides of 32 bits: their product, as Python's int, fits 64 bits unsigned)
    const std::uint64_t pixels = std::max<std::uint64_t>(1, width) * std::max<std::uint64_t>(1, height);
    if (pixels > static_cast<std::uint64_t>(2 * render::kMaxImagePixels)) {
        unreadable("Image size (" + std::to_string(pixels) + " pixels) exceeds limit of " + std::to_string(2 * render::kMaxImagePixels) +
                   " pixels, could be decompression bomb DOS attack.");
    }
    // verify(): from the image data's chunk to IEND, every checksum
    if (!idat) unreadable("list index out of range");
    pos = *idat - 8;
    for (;;) {
        const Header h = header();
        if (h.short_header) unreadable("truncated PNG file");
        if (!is_cid(h.cid)) unreadable("broken PNG file (chunk " + cid_repr(h.cid) + ")");
        if (h.cid == "IEND") break;
        const std::string_view data = safe_read(h.length);
        if (const auto bad = checksum(h.cid, data)) unreadable(*bad);
    }
    if (width * height > kOpsMaxImagePixels) {  // (within the limit above: no overflow)
        throw OpError("image too large: " + std::to_string(width) + "x" + std::to_string(height));
    }
    // every pixel, read as the pages are drawn (Python keeps image data that cannot be decoded, or a compression or
    // interlace method PNG does not have: its checksums are right, and the page can never be drawn again)
    try {
        (void)read_png(blob, PngLimits{kOpsMaxImagePixels});
    } catch (const core::Error& error) {
        unreadable(error.what());
    }
}

// str(Path(text)): pathlib's form of a path ("a//b/./c/" → "a/b/c")
std::string pathlib_text(const std::string& text) {
    const bool absolute = !text.empty() && text.front() == '/';
    std::vector<std::string> parts;
    std::string part;
    for (const char ch : text + "/") {
        if (ch == '/') {
            if (!part.empty() && part != ".") parts.push_back(part);
            part.clear();
        } else {
            part += ch;
        }
    }
    std::string out = absolute ? "/" : "";
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i == 0 ? "" : "/") + parts[i];
    return out.empty() ? "." : out;
}

// Path(str(value)).read_bytes(), with Python's exceptions (which apply_ops lets through)
std::string read_path(const Json& value) {
    const std::string text = pathlib_text(core::py_str(value));
    const std::filesystem::path path = core::path_from_utf8(text);
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        throw core::PyUncaught("IsADirectoryError", "[Errno 21] Is a directory: " + core::py_repr_str(text));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (!std::filesystem::exists(path, ec)) {
            throw core::PyUncaught("FileNotFoundError", "[Errno 2] No such file or directory: " + core::py_repr_str(text));
        }
        throw core::PyUncaught("PermissionError", "[Errno 13] Permission denied: " + core::py_repr_str(text));
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes;
}

void put_raster(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    std::string blob;
    if (core::truthy_at(op, "png_base64")) {
        blob = selection::b64decode(op["png_base64"]);
    } else if (core::truthy_at(op, "path")) {
        blob = read_path(op["path"]);
    }
    if (blob.empty()) throw OpError("put_raster needs path or png_base64");
    verify_image(blob);
    Page& page = doc.edit_page(at);
    std::size_t li = 0;
    if (core::truthy_at(op, "id")) {
        li = resolve_layer(page, op);
    } else {
        const std::string role_name = core::truthy_at(op, "layer") ? core::py_str(op["layer"]) : std::string("ink");
        const auto role = core::layer_role_from(role_name);
        if (!role) throw OpError("unknown layer " + role_name);
        li = core::layer_for_role(page, *role);
    }
    Layer& layer = page.layers[li];
    if (layer.locked) throw OpError("the layer is locked");  // (Python replaces its pixels)
    layer.kind = LayerKind::Raster;
    layer.raster_png = std::make_shared<const std::string>(std::move(blob));
    const std::string name = layer.role == LayerRole::User ? "user-" + layer.id : std::string(core::to_string(layer.role));
    layer.raster_relpath = raster::page_folder(page) + "/" + name + ".png";
    if (layer.role == LayerRole::Name || layer.role == LayerRole::Draft) layer.exportable = false;
}

// Native materials reuse the same packaged catalog as the GUI; CLI and desktop never need Python.
std::filesystem::path config_dir();
Json builtin_material(const std::string& id) {
    static const Json catalog=[] {
        initialize_stamp_resources();QFile file(QStringLiteral(":/genko/materials/catalog.json"));
        if (!file.open(QIODevice::ReadOnly)) throw OpError("cannot read built-in materials");
        return core::parse_python_json(file.readAll().toStdString());
    }();
    for (const auto& item:catalog) if (item.value("id",std::string())==id) return item;
    // Existing read-only user library, bounded like the GUI; never rewrite source or manifest.
    const QString root=QDir(QString::fromStdString(core::path_to_utf8(config_dir()))).filePath("materials");
    const QFileInfo directory(root),manifest(QDir(root).filePath("library.json"));
    if(directory.isDir()&&!directory.isSymLink()&&directory.canonicalFilePath()==directory.absoluteFilePath()&&
       manifest.isFile()&&!manifest.isSymLink()&&manifest.canonicalPath()==directory.canonicalFilePath()&&manifest.size()<=1024*1024){
        QFile library(manifest.absoluteFilePath());if(library.open(QIODevice::ReadOnly)){
            const QByteArray bytes=library.read(1024*1024+1);if(bytes.size()<=1024*1024){core::ParseRepairs repairs;const Json items=core::parse_python_json(bytes.toStdString(),&repairs);if(!repairs.nonfinite.empty())throw core::OpError("material library contains nonfinite numbers");
                if(items.is_array())for(const auto& item:items)if(item.is_object()&&item.contains("id")&&item["id"].is_string()&&item["id"]==id)return item;
            }
        }
    }
    throw OpError("no material "+id);
}
void stamp_material(OpContext& c) {
    const auto at=core::require_page(c.doc,c.op);
    const std::string id=core::py_str(core::get_or(c.op,"material_id",Json()));
    const Json material=builtin_material(id);
    const std::string kind=material.value("kind",std::string());
    const auto apply=[&](Json op) { c.doc=core::CommandBus(ops_registry()).apply(c.doc,Json::array({std::move(op)}),c.actor).doc; };
    const auto positioned=[&](Json op) {
        op["page"]=c.doc.page(at).index.json();
        for (const char* key:{"frame_id","id"}) if(core::truthy_at(c.op,key))op[key]=c.op[key];
        if(!core::truthy_at(op,"id"))op["id"]=core::new_id();
        return op;
    };
    const auto centre=[&] {
        return std::pair<double,double>{float_at(c.op,"x_mm",c.doc.page(at).spec.width_mm.value()/2),
                                      float_at(c.op,"y_mm",c.doc.page(at).spec.height_mm.value()/2)};
    };
    if(kind=="effect") {
        Json params=material.value("params",Json::object());
        if(core::get(c.op,"x_mm") && core::get(c.op,"y_mm")){const auto [x,y]=centre();params["center"]=Json::array({x,y});}
        apply(positioned(Json{{"op","add_effect"},{"kind",material.value("effect",std::string("speed"))},{"params",params}}));return;
    }
    if(kind=="brush") {
        const auto key="my_"+QCryptographicHash::hash(QByteArray::fromStdString(id),QCryptographicHash::Sha1).toHex().left(10).toStdString();
        Json op={{"label",material.value("name",std::string("ブラシ"))}};
        const Json brush=material.value("brush",Json::object());
        for(const auto& [k,v]:brush.items())op[k]=v;
        op["op"]="define_brush";op["key"]=key;apply(op);return;
    }
    if(kind=="prim") {
        const auto [x,y]=centre();const std::string shape=material.value("prim",std::string("box"));Json op;
        if(core::truthy_at(material,"scene"))op={{"op","add_scene"},{"kind",material["scene"]}};
        else if(shape=="mannequin" || shape=="figure" || shape=="head" || shape=="hand") {
            op={{"op",shape=="mannequin"?"add_mannequin":shape=="figure"?"add_figure":shape=="head"?"add_head":"add_hand"},{"pos",Json::array({x,y,0})}};
            if(shape=="hand" && core::truthy_at(material,"pose"))op["pose"]=material["pose"];
        }else op={{"op","add_prim3d"},{"kind",shape},{"pos",Json::array({x,y,0})}};
        op=positioned(std::move(op));
        // Match the actual 3D producer's stored identifier before either comparing or locating it.
        const Json created=core::py_str(op["id"]);op["id"]=created;
        for(const auto& prim:c.doc.page(at).prims)
            if(core::py_equals(core::get_or(prim,"id",Json()),created))throw OpError("duplicate 3D id "+core::py_str(created));
        const bool scene=op["op"]=="add_scene";apply(op);
        if(scene)for(auto& prim:c.doc.edit_page(at).prims)if(prim["id"]==created){prim["pos"][0]=core::py_round(x,3);prim["pos"][1]=core::py_round(y,3);break;}
        return;
    }
    if(kind=="image"){
        if(core::truthy_at(c.op,"line_id"))throw NotYetPorted("picture balloon requires the common balloons/text renderer");
        constexpr qint64 maxSourceBytes=64ll<<20; // Same original-file cap as the existing user-preview reader.
        const QString root=QDir(QString::fromStdString(core::path_to_utf8(config_dir()))).filePath("materials");const QFileInfo directory(root);
        const QString name=QString::fromStdString(material.value("file",std::string()));
        if(name.isEmpty()||QDir::isAbsolutePath(name))throw core::OpError("material image must be inside the material library");
        const QFileInfo source(QDir(root).filePath(name));const QString canonical=source.canonicalFilePath();
        if(!directory.isDir()||directory.isSymLink()||directory.canonicalFilePath()!=directory.absoluteFilePath()||
           !source.isFile()||source.isSymLink()||canonical!=source.absoluteFilePath()||!canonical.startsWith(directory.canonicalFilePath()+"/")||source.size()>maxSourceBytes)
            throw core::OpError("unsafe material image source");
        QFile file(canonical);if(!file.open(QIODevice::ReadOnly))throw core::OpError("material image unavailable");
        const QByteArray bytes=file.read(maxSourceBytes+1);if(bytes.size()>maxSourceBytes)throw core::OpError("material image exceeds byte budget");
        const std::string blob=bytes.toStdString();verify_image(blob);const auto picture=selection::open_picture(blob);limits::check_picture(picture.width(),picture.height(),"material image");
        const double width=core::truthy_at(c.op,"width_mm")?float_at(c.op,"width_mm",60):core::truthy_at(material,"width_mm")?float_at(material,"width_mm",60):60;
        const double height=width*(core::truthy_at(material,"aspect")?float_at(material,"aspect",1):1);if(width<=0||height<=0)throw core::OpError("material image size must be positive");
        const auto [x,y]=centre();const Json box=Json::array({x-width/2,y-height/2,width,height});core::require_finite(box);
        for(const auto& value:box)limits::check_coordinate(core::py_float(value)*selection::kWorkingDpi/25.4,"material image box");
        auto& target=core::paint_target(c.doc.edit_page(at),c.op);core::Patch patch;patch.attrs=Json{{"id",core::new_id()},{"box",Json::array({core::py_round(x-width/2,3),core::py_round(y-height/2,3),core::py_round(width,3),core::py_round(height,3)})},{"mode","image"},{"opacity",1.0}};
        patch.png=std::make_shared<const std::string>(blob);target.patches.push_back(std::move(patch));return;
    }
    if(kind=="lettering") throw NotYetPorted("material lettering requires text/balloon rendering");
    if(kind=="lines") {
        auto items=selection::items_from_json(material.value("items",Json::object()));validate_patch_items(items,true);auto matrix=selection::kIdentity;
        if(c.op.contains("x_mm") && !c.op["x_mm"].is_null() && c.op.contains("y_mm") && !c.op["y_mm"].is_null()){
            double loX=std::numeric_limits<double>::infinity(),loY=loX,hiX=-loX,hiY=-loX;
            const auto point=[&](double x,double y){loX=std::min(loX,x);loY=std::min(loY,y);hiX=std::max(hiX,x);hiY=std::max(hiY,y);};
            for(const auto& stroke:items.strokes)for(const auto& p:stroke->points)point(p.x,p.y);
            for(const auto& patch:items.patches){const auto& b=patch.attrs.at("box");const double x=core::py_float(b[0]),y=core::py_float(b[1]);point(x,y);point(x+core::py_float(b[2]),y+core::py_float(b[3]));}
            if(std::isfinite(loX)){matrix[4]=core::py_float(c.op["x_mm"])-(loX+hiX)/2;matrix[5]=core::py_float(c.op["y_mm"])-(loY+hiY)/2;}
        }
        auto& target=core::paint_target(c.doc.edit_page(at),c.op);selection::drop(target,std::move(items),matrix,true);return;
    }
    if (kind!="tone") throw NotYetPorted("material kind:"+kind);
    Json op=material.value("tone",Json::object());
    for (const char* key:{"frame_id","area","at","after","id"}) if(core::truthy_at(c.op,key))op[key]=c.op[key];
    op["op"]="add_tone";op["page"]=c.doc.page(at).index.json();op["name"]=material.value("name",std::string());
    if(!core::truthy_at(op,"id"))op["id"]=core::new_id();
    auto result=core::CommandBus(ops_registry()).apply(c.doc,Json::array({op}),c.actor).doc;
    auto& page=result.edit_page(at);
    page.layers[core::layer_by_id(page,core::py_str(op["id"]))].material_id=id;
    c.doc=std::move(result);
}

// --- filters ----------------------------------------------------------------------------------------------------------

// tokens.config_dir()
std::filesystem::path config_dir() {
    const QString given = qEnvironmentVariable("GENKO_CONFIG_DIR");
    if (!given.isEmpty()) return core::path_from_utf8(given.toStdString());
#ifdef _WIN32
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty()) return core::path_from_utf8(appdata.toStdString()) / "genko";
#endif
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty()) base = qEnvironmentVariable("HOME") + QStringLiteral("/.config");
    return core::path_from_utf8(base.toStdString()) / "genko";
}

// plugins.run's checks: a plugin is a file a person installed in <config>/plugins; this build does not run it (the
// external runner of COMP-04 does)
[[noreturn]] void plugin_filter(const std::string& kind) {
    const std::string key = kind.substr(7);
    std::error_code ec;
    if (key.empty() || key.find('/') != std::string::npos || key.find('\\') != std::string::npos ||
        !std::filesystem::is_regular_file(config_dir() / "plugins" / core::path_from_utf8(key + ".py"), ec)) {
        throw core::PyValueError("no plugin " + key);
    }
    core::not_yet_ported("a filter plugin (" + key + ") runs in the external plugin runner, not in this build");
}

void filter_raster(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    Layer& layer = page.layers[resolve_layer(page, op)];
    if (layer.locked) throw OpError("the layer is locked");  // (Python filters it)
    // (pen lines and shape fills become pixels, so the filter reaches them)
    if (layer.stroke_count() > 0 || !layer.patches.empty()) raster::bake_vectors(page, layer);
    const std::string kind = core::truthy_at(op, "kind") ? core::py_str(op["kind"]) : std::string();
    // ops.filtered_raster
    const Image image = raster::layer_pixels(page, layer);
    Json params = Json::object();
    for (const auto& [key, value] : op.items()) {
        if (key != "op" && key != "page" && key != "layer" && key != "id" && key != "kind" && key != "area") params[key] = value;
    }
    Image filtered;
    try {
        if (kind.starts_with("plugin:")) plugin_filter(kind);
        filtered = filters::apply_filter(image, kind, params);
    } catch (const core::PyUncaught&) {
        throw;
    } catch (const core::Error& error) {
        // (Python's ValueError and TypeError become the op's error)
        if (error.code() != "value" && error.code() != "type") throw;
        throw OpError(error.what());
    }
    if (core::truthy_at(op, "area")) {  // (選択範囲の中だけ)
        const selection::AreaMask inside = selection::area_mask(core::op_area(op), kWorkingDpi);
        Image mask = Image::create("L", image.size(), Ink(0));
        mask.paste(inside.mask, Point{static_cast<int>(inside.x0), static_cast<int>(inside.y0)});
        filtered = filters::within(image, filtered, mask);
    }
    raster::save_raster(page, layer, filtered);
    layer.kind = LayerKind::Raster;
}

// --- the eraser on a paint layer ----------------------------------------------------------------------------------------

void erase(OpContext& c) {
    // The pen lines are core's erase (cut where the eraser went, up to their crossings, or whole; a tone scraped): it
    // does them and then stops at a paint layer's pixels with not_yet_ported. Those are erased here, after it.
    static const core::OpFunction* const lines = core::OpRegistry::builtin().find("erase");
    try {
        (*lines)(c);
        return;
    } catch (const core::Error& error) {
        if (error.code() != "not_yet_ported" || std::string_view(error.what()).find(" on a raster layer") == std::string_view::npos) throw;
    }
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    Page& page = doc.edit_page(at);
    std::size_t li = 0;
    if (core::truthy_at(op, "layer_id")) {
        li = core::layer_by_id(page, core::py_str(op["layer_id"]));
    } else {
        const Json* layer = core::get(op, "layer");
        li = core::layer_for_role(page, core::role_from(Json(layer != nullptr && core::py_truthy(*layer) ? core::py_str(*layer) : "ink")));
    }
    const Json* raw = core::get(op, "points");
    core::PenPoints points = core::parse_points(raw != nullptr && core::py_truthy(*raw) ? *raw : Json::array());
    const Json* width_value = core::get(op, "width_mm");
    const double width = width_value != nullptr ? core::to_float(*width_value) : 2.0;
    if (core::truthy_at(op, "snap_ruler") && core::py_truthy(page.rulers)) {  // (as core's erase snapped them)
        const Json* layer_id = core::get(op, "layer_id");
        const Json lid(layer_id != nullptr && core::py_truthy(*layer_id) ? core::py_str(*layer_id) : std::string());
        const Json only = op.contains("ruler_id") ? op["ruler_id"] : Json(nullptr);
        const core::rulers::FrameContains inside = [&page](const Json& frame_id, double x, double y) {
            if (page.frames.empty() || !frame_id.is_string()) return false;
            const core::Frame* frame = page.find_frame(frame_id.get_ref<const std::string&>());
            return frame != nullptr && core::contains(*frame, Num(x), Num(y));
        };
        points = core::rulers::snap(points, page.rulers, inside, only, lid.get_ref<const std::string&>().empty() ? nullptr : &lid);
    }
    const Json* texture_value = core::get(op, "texture");
    const std::string texture = texture_value != nullptr && core::py_truthy(*texture_value) ? core::py_str(*texture_value) : "";
    Layer& target = page.layers[li];
    const bool has_raster = target.raster_png && !target.raster_png->empty();
    if (target.kind == LayerKind::Raster && !target.patches.empty() && !has_raster) {
        raster::bake_vectors(page, target);  // (fills on a paint layer are erased like its pixels)
    }
    if (target.raster_png && !target.raster_png->empty()) {
        raster::erase_raster(page, target, points, width, kWorkingDpi, texture == "hard" ? "" : texture,
                             target.id + raster::first_point_repr(points));
    }
}

// ops.reshape_stroke: edit the existing line, never introduce a new identity.
void reshape_stroke(OpContext& c) {
    Page& page = c.doc.edit_page(core::require_page(c.doc, c.op));
    Layer& target = core::paint_target(page, c.op);
    const Json* id = core::get(c.op, "stroke_id");
    std::vector<core::StrokePtr> lines = target.strokes->items;
    const auto found = std::find_if(lines.begin(), lines.end(), [id](const core::StrokePtr& line) {
        return id != nullptr && Json(line->id) == *id;
    });
    if (found == lines.end()) throw OpError("no line " + core::py_str(id == nullptr ? Json(nullptr) : *id));
    core::Stroke copy = **found;
    if (core::truthy_at(c.op, "points")) {
        const auto points = core::parse_points(c.op["points"]);
        if (points.size() < 2) throw OpError("points needs at least two [x_mm, y_mm] pairs");
        for (const auto& point : points) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || (point.p && !std::isfinite(*point.p))) {
                throw OpError("points and pressure must be finite numbers");
            }
        }
        const auto shape = core::coerce_stroke(points);
        copy.points = shape.points;
        if (!shape.pressure.empty()) copy.pressure = shape.pressure;
        else copy.pressure.resize(std::min(copy.pressure.size(), copy.points.size()));
        if (!copy.pressure.empty() && copy.pressure.size() != copy.points.size()) copy.pressure.clear();
    }
    if (const Json* width = core::get(c.op, "width_mm"); width != nullptr && !width->is_null()) {
        copy.width_mm = core::py_max(0.05, core::finite_float(*width, "width_mm"));
    }
    *found = std::make_shared<const core::Stroke>(std::move(copy));
    target.strokes = core::make_strokes(std::move(lines));
}

// ops.set_stroke_width: selected immutable pen lines keep their identities and other data.
void set_stroke_width(OpContext& c) {
    Page& page = c.doc.edit_page(core::require_page(c.doc, c.op));
    Layer& target = core::paint_target(page, c.op);
    const Json* given = core::get(c.op, "ids");
    const std::vector<Json> ids = given != nullptr && core::py_truthy(*given)
            ? core::iterate(*given) : std::vector<Json>{};
    for (const Json& id : ids) core::require_hashable(id);
    const Json* area = core::get(c.op, "area");
    std::optional<selection::AreaTest> inside;
    if (area != nullptr && core::py_truthy(*area)) inside.emplace(*area);
    std::vector<core::StrokePtr> lines = target.strokes->items;
    bool changed = false;
    for (auto& line : lines) {
        const bool named = std::find(ids.begin(), ids.end(), Json(line->id)) != ids.end();
        if (!named && !(inside && inside->stroke_inside(*line))) continue;
        core::Stroke copy = *line;
        if (const Json* width = core::get(c.op, "width_mm"); width != nullptr && !width->is_null()) {
            copy.width_mm = core::py_max(0.05, core::finite_float(*width, "width_mm"));
        }
        if (const Json* scale = core::get(c.op, "scale"); scale != nullptr && !scale->is_null()) {
            const double scaled = copy.width_mm * core::finite_float(*scale, "scale");
            const double bounded = core::py_max(0.05, scaled);
            // A negative overflow still has Python's valid minimum-width result.
            if (std::isnan(scaled) || !std::isfinite(bounded)) throw OpError("scaled width_mm must be a finite number");
            copy.width_mm = bounded;
        }
        if (core::truthy_at(c.op, "kind")) copy.kind = core::brush_kind(c.op["kind"], c.doc);
        if (core::truthy_at(c.op, "rgb")) {
            if (core::iterate(c.op["rgb"]).size() != 3) throw OpError("rgb is [r, g, b]");
            copy.rgb = core::rgb3(c.op["rgb"], "rgb");
        }
        line = std::make_shared<const core::Stroke>(std::move(copy));
        changed = true;
    }
    if (!changed) throw OpError("no line there");
    target.strokes = core::make_strokes(std::move(lines));
}

// --- the light table -------------------------------------------------------------------------------------------------

// lt.runs_to_strokes's runs: along each row of the line art ("L"), every run of two dark pixels (below 80) or more, as
// (row, first, past the last) — a shorter run is dropped, one running to the row's end kept
template <class Each>
void lt_runs(std::string_view pixels, int width, int height, Each each) {
    for (int y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        int first = 0;
        for (int x = 0; x <= width; ++x) {
            if (x < width && static_cast<unsigned char>(pixels[row + static_cast<std::size_t>(x)]) < 80) continue;
            if (x - first >= 2) each(y, first, x);
            first = x + 1;
        }
    }
}

// ops._lt_convert: the picture of the layer in "layer" (bg by default) as line art (lt.to_line_art), its runs pen lines
// (lt.runs_to_strokes) added after those of the layer in "to" (ink by default) — one across the top when it has none
void lt_convert(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const LayerRole src_role = core::role_from(Json(core::truthy_at(op, "layer") ? core::py_str(op["layer"]) : std::string("bg")));
    const LayerRole dest_role = core::role_from(Json(core::truthy_at(op, "to") ? core::py_str(op["to"]) : std::string("ink")));
    if (dest_role == LayerRole::Ink && !doc.page(at).name_ok && core::gated(doc)) throw OpError("lt_convert to ink requires name_ok");
    Page& page = doc.edit_page(at);
    // (its bytes held, not the layer: the layer the lines go to may yet be added to the page's layers)
    const core::Bytes picture = page.layers[core::layer_for_role(page, src_role)].raster_png;
    if (!picture || picture->empty()) throw OpError("lt_convert needs a raster on the source layer");
    const Json* threshold = core::get(op, "threshold");
    const std::string method = core::truthy_at(op, "method") ? core::py_str(op["method"]) : std::string("adaptive");
    // lt.to_line_art
    const Image gray = selection::open_picture(*picture).convert("L");
    Image binary;
    if (method == "edges" || method == "sobel") {
        binary = ops::invert(gray.filter(Filter::find_edges()));
    } else {
        double cut = 0;
        if (threshold != nullptr && !threshold->is_null()) {
            cut = core::finite_float(*threshold, "threshold");
        } else if (page.lt_threshold) {  // (the one set_lt keeps)
            cut = page.lt_threshold->value();
            if (!std::isfinite(cut)) throw OpError("the page's lt_threshold must be a finite number");
        } else {  // the picture's mean less 12, at least 8
            const std::string raw = gray.tobytes();
            std::uint64_t sum = 0;
            for (const char ch : raw) sum += static_cast<unsigned char>(ch);
            cut = core::py_max(8.0, static_cast<double>(sum) / static_cast<double>(std::max<std::size_t>(1, raw.size())) - 12);
        }
        binary = gray.point([cut](int p) { return p < cut ? 0 : 255; });
    }
    Layer& dest = page.layers[core::layer_for_role(page, dest_role)];
    if (dest.locked) throw OpError("the layer is locked");  // (Python draws on it)
    if (dest.kind == LayerKind::Placed) throw OpError("a placed layer cannot receive strokes");
    // lt.runs_to_strokes (points that are not finite, or more of them than an op may make, refused before a line is added)
    const double width_mm = page.spec.width_mm.value();
    const double height_mm = page.spec.height_mm.value();
    if (!std::isfinite(width_mm)) throw OpError("the page's width_mm must be a finite number");
    if (!std::isfinite(height_mm)) throw OpError("the page's height_mm must be a finite number");
    const int w = binary.width();
    const int h = binary.height();
    const std::string pixels = binary.tobytes();
    std::int64_t count = 0;
    lt_runs(pixels, w, h, [&count](int, int first, int past) { count += past - first; });
    limits::check_count(static_cast<double>(count), static_cast<double>(limits::kPoints), "the line art");
    const double sx = width_mm / std::max(1, w);
    const double sy = height_mm / std::max(1, h);
    std::vector<core::StrokePtr> lines = dest.strokes->items;
    lt_runs(pixels, w, h, [&](int y, int first, int past) {
        core::PenPoints run;
        for (int x = first; x < past; ++x) run.push_back(core::PenPoint{x * sx, y * sy, std::nullopt});
        lines.push_back(std::make_shared<const core::Stroke>(core::coerce_stroke(run)));
    });
    if (lines.empty()) {
        lines.push_back(std::make_shared<const core::Stroke>(core::coerce_stroke(
            core::PenPoints{core::PenPoint{10.0, 10.0, std::nullopt}, core::PenPoint{width_mm - 10, 10.0, std::nullopt}})));
    }
    if (lines.size() != dest.stroke_count()) dest.strokes = core::make_strokes(std::move(lines));
}

// The bus's resolver of the richer areas (selops.resolve on the page the op names)
// Python's reader makes the book's own brushes known to the process (brushes.register, and define_brush adds to
// them): the lines these ops draw are drawn with them, as the page is.
void use_book_brushes(const Document& doc) {
    brushes::clear_custom();
    brushes::register_brushes(doc.brush_custom);
}

core::OpFunction drawing(void (*op)(OpContext&)) {
    return [op](OpContext& c) {
        if (op == merge_down || op == merge_layers || op == merge_visible || op == convert_layer) {
            const auto& page = c.doc.page(core::require_page(c.doc, c.op));
            if (std::any_of(page.layers.begin(), page.layers.end(), [](const Layer& layer) { return bool(layer.color_raster); }))
                throw NotYetPorted("high-precision raster conversion or merge");
        }
        use_book_brushes(c.doc);
        op(c);
    };
}

Json resolve_area(const Document& doc, std::size_t page, const Json& area) {
    use_book_brushes(doc);
    return selection::resolve(area, doc.page(page), &doc);
}

}  // namespace

void register_raster_ops(core::OpRegistry& registry) {
    registry.add("reshape_stroke", drawing(reshape_stroke));
    registry.add("set_stroke_width", drawing(set_stroke_width));
    registry.add("convert_layer", drawing(convert_layer));
    registry.add("merge_down", drawing(merge_down));
    registry.add("merge_layers", drawing(merge_layers));
    registry.add("merge_visible", drawing(merge_visible));
    registry.add("set_layer_mask", drawing(set_layer_mask));
    registry.add("paint_mask", drawing(paint_mask));
    registry.add("put_raster", drawing(put_raster));
    registry.add("filter_raster", drawing(filter_raster));
    registry.add("fill", drawing(fill));
    registry.add("fill_area", drawing(fill_area));
    registry.add("fill_enclosed", drawing(fill_enclosed));
    registry.add("fill_gaps", drawing(fill_gaps));
    registry.add("flood_fill", drawing(flood_fill));
    registry.add("gradient_fill", drawing(gradient_fill));
    registry.add("delete_area", drawing(delete_area));
    registry.add("transform_area", drawing(transform_area));
    registry.add("paste", drawing(paste));
    registry.add("erase", drawing(erase));
    registry.add("erase_raster", drawing(erase));
    registry.add("lt_convert", drawing(lt_convert));
    registry.add("stamp_material", drawing(stamp_material));
    registry.set_area_resolver(resolve_area);
}

}  // namespace genko::render
