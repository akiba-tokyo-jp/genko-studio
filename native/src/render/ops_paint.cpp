// The paint ops of M3④ that draw (Python's ops._add_shape, ops._smudge and layerops.liquify): 図形 — a line, polyline,
// curve, rectangle, ellipse or polygon drawn as a pen line, filled, or both; 色混ぜ — blur, push the colour along or even
// it out where the brush passes, laid over the layer as a picture; ゆがみ — the layer pushed, pinched, bloated or
// twirled under the drag (its pen lines' points move; its pixels are fetched from where the drag pulled them).

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "core/brushes.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/stroke_tools.hpp"
#include "core/strokes.hpp"
#include "render/distort_ops.hpp"
#include "render/draw.hpp"
#include "render/fill_patches.hpp"
#include "render/filters.hpp"
#include "render/image_ops.hpp"
#include "render/ink_edit.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"
#include "render/raster_ops.hpp"
#include "render/selection.hpp"
#include "render/shape_parts.hpp"
#include "render/stroke.hpp"

namespace genko::render {

namespace {

// A pixel position as Python's whole number, held to where it still means "far off the picture" (an int cannot hold
// every one Python's can; past a billion pixels each is off any picture alike).
// (v: a whole number held as a double, as core::py_round_whole and std::trunc give it)
int held_px(double v) { return static_cast<int>(std::clamp(v, -1e9, 1e9)); }

using core::Document;
using core::Json;
using core::Layer;
using core::OpContext;
using core::OpError;
using core::Page;

constexpr double kTau = 6.283185307179586;  // math.tau

// math.radians
double radians(double degrees) { return degrees * (core::kPi / 180.0); }

Json op_get(const Json& op, std::string_view key) {
    const Json* value = core::get(op, key);
    return value != nullptr ? *value : Json();
}

// float(op.get(key, fallback))
double float_at(const Json& op, std::string_view key, double fallback) {
    const Json* value = core::get(op, key);
    return value != nullptr ? core::to_float(*value) : fallback;
}

// ops._rgb: op["rgb"] or the book's brush colour or (20, 20, 20), as ints
std::vector<std::int64_t> rgb_of(const Json& rgb, const Document& doc) {
    if (core::py_truthy(rgb)) {
        std::vector<std::int64_t> out;
        for (const Json& v : core::iterate(rgb)) out.push_back(core::to_int(v));
        return out;
    }
    if (!doc.brush_rgb.empty()) return doc.brush_rgb;
    return {20, 20, 20};
}

// --- 図形 ---------------------------------------------------------------------------------------------------------

}  // namespace

ShapeOutline shape_points(const std::string& kind, const Json& op) {
    if (kind == "rect" || kind == "ellipse" || kind == "polygon") {
        const Json box = op_get(op, "box");
        if (!core::py_truthy(box) || core::length(box) != 4) throw OpError("box [x, y, w, h] is required");
        const std::vector<double> v = core::unpack_floats(box, 4);
        const double x = v[0], y = v[1], w = v[2], h = v[3];
        const double cx = x + w / 2, cy = y + h / 2;
        ShapeOutline out;
        out.closed = true;
        if (kind == "rect") {
            const Json given = op_get(op, "radius_mm");
            const double r = core::py_max(0.0, core::py_min(core::py_min(core::py_truthy(given) ? core::to_float(given) : 0.0, w / 2), h / 2));
            if (r <= 0) {
                out.points = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
                return out;
            }
            const std::array<std::array<double, 3>, 4> corners{{{x + w - r, y + r, -90}, {x + w - r, y + h - r, 0},
                                                                {x + r, y + h - r, 90}, {x + r, y + r, 180}}};
            for (const auto& [ox, oy, start] : corners) {
                for (int i = 0; i < 9; ++i) {
                    const double a = radians(start + 90.0 * i / 8);
                    out.points.push_back({ox + r * core::py_cos(a), oy + r * core::py_sin(a)});
                }
            }
            return out;
        }
        if (kind == "ellipse") {
            constexpr int n = 96;
            for (int k = 0; k < n; ++k) {
                const double a = kTau * k / n;
                out.points.push_back({cx + w / 2 * core::py_cos(a), cy + h / 2 * core::py_sin(a)});
            }
            return out;
        }
        const Json sides_given = op_get(op, "sides");
        const std::int64_t sides = std::max<std::int64_t>(3, std::min<std::int64_t>(24, core::to_int(core::py_or(sides_given, 5))));
        const Json angle = op_get(op, "angle");
        const double turn = radians(core::to_float(core::py_or(angle, 0))) - core::kPi / 2;
        for (std::int64_t k = 0; k < sides; ++k) {
            const double a = turn + kTau * static_cast<double>(k) / static_cast<double>(sides);
            out.points.push_back({cx + w / 2 * core::py_cos(a), cy + h / 2 * core::py_sin(a)});
        }
        return out;
    }
    std::vector<std::array<double, 2>> points;
    for (const Json& p : core::iterate(core::py_or(op_get(op, "points"), Json::array()))) {
        points.push_back({core::to_float(core::subscript(p, 0)), core::to_float(core::subscript(p, 1))});
    }
    if (points.size() < 2) throw OpError("points needs at least two [x, y]");
    ShapeOutline out;
    if (kind == "curve") {
        if (points.size() == 2) {
            out.points = points;
            return out;
        }
        // a smooth curve through the points (Catmull–Rom)
        std::vector<std::array<double, 2>> ext{points.front()};
        ext.insert(ext.end(), points.begin(), points.end());
        ext.push_back(points.back());
        for (std::size_t i = 1; i + 2 < ext.size(); ++i) {
            const auto& p0 = ext[i - 1];
            const auto& p1 = ext[i];
            const auto& p2 = ext[i + 1];
            const auto& p3 = ext[i + 2];
            for (int k = 0; k < 12; ++k) {
                const double t = k / 12.0;
                std::array<double, 2> q{};
                for (int j = 0; j < 2; ++j) {
                    const double a = 2 * p1[j];
                    const double b = (-p0[j] + p2[j]) * t;
                    const double c = (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t * t;
                    const double d = (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * core::py_pow(t, 3);
                    q[j] = 0.5 * (a + b + c + d);
                }
                out.points.push_back(q);
            }
        }
        out.points.push_back(points.back());
        out.closed = core::truthy_at(op, "closed");
        return out;
    }
    out.points = points;
    out.closed = core::truthy_at(op, "closed") && kind == "polyline";
    return out;
}

namespace {

void add_shape(OpContext& c) {
    const Json& op = c.op;
    const std::string kind = core::truthy_at(op, "shape") ? core::py_str(op["shape"]) : std::string();
    static const std::vector<std::string> kShapes{"line", "polyline", "curve", "rect", "ellipse", "polygon"};
    if (std::find(kShapes.begin(), kShapes.end(), kind) == kShapes.end()) {
        throw OpError("shape must be one of line, polyline, curve, rect, ellipse, polygon");
    }
    const std::size_t at = core::require_page(c.doc, op);
    Page& page = c.doc.edit_page(at);
    Layer& target = core::paint_target(page, op);
    const ShapeOutline shape = shape_points(kind, op);
    const bool line = core::py_truthy(core::get_or(op, "line", true));
    const bool filled = core::truthy_at(op, "fill");
    if (filled && (shape.closed || kind == "rect" || kind == "ellipse" || kind == "polygon")) {
        const Json fill_rgb = core::py_or(op_get(op, "fill_rgb"), op_get(op, "rgb"));
        const auto fill_ink = rgb_of(fill_rgb, c.doc);  // (before the opacity, as Python reads them)
        const double fill_opacity = float_at(op, "opacity", 1.0);
        if (auto patch = fills::polygon_patch(shape.points, fill_ink, fill_opacity)) {
            target.patches.push_back(std::move(*patch));
        }
    }
    if (line) {
        core::PenPoints drawn;
        for (const auto& [x, y] : shape.points) drawn.push_back(core::PenPoint{core::py_round(x, 3), core::py_round(y, 3), 1.0});
        if (shape.closed) {
            const auto& [x, y] = shape.points.front();
            drawn.push_back(core::PenPoint{core::py_round(x, 3), core::py_round(y, 3), 1.0});
        }
        auto stroke = std::make_shared<core::Stroke>(core::coerce_stroke(drawn));
        stroke->kind = core::brush_kind(core::py_or(op_get(op, "kind"), "mili"), c.doc);
        const Json width = op_get(op, "width_mm");
        stroke->width_mm = core::py_truthy(width) ? core::to_float(width) : c.doc.brush_width_mm;
        if (core::truthy_at(op, "rgb")) {
            std::vector<std::int64_t> rgb;
            for (const Json& v : core::iterate(op["rgb"])) rgb.push_back(core::to_int(v));
            stroke->rgb = std::move(rgb);
        }
        if (const Json* opacity = core::get(op, "opacity"); opacity != nullptr && !opacity->is_null()) {
            stroke->opacity = core::py_max(0.0, core::py_min(1.0, core::to_float(*opacity)));
        }
        core::require_finite(core::stroke_to_dict(*stroke), "the shape");
        std::vector<core::StrokePtr> items = target.strokes->items;
        items.push_back(std::move(stroke));
        target.strokes = core::make_strokes(std::move(items));
    }
}

// --- 色混ぜ --------------------------------------------------------------------------------------------------------

constexpr int kSmudgeDpi = 200;

// 色混ぜ on a precise or ink paint layer (PC4 E5, design section E 0.4: the PC2 task): the same work as the 8-bit one's
// below — the stroke drawn, blurred and × its strength the cover through which the blurred (ぼかし, なじませ) or carried
// (指先) values are laid over the layer's — in the layer's own values on its own grid: a precise layer's samples in float
// (HDR kept), an ink layer's plates. An ink layer's values are what it shows — its inks with its lines and pictures drawn
// (ink_edit::with_marks), as the picture it was lent before (review P2-2: its lines smudged too, a layer of lines alone
// and its second stroke alike) — and what changed goes into its own inks, under its lines (they stay lines). Only the
// pixels it changes are written (the others keep their bytes); its work in the imaging budget (past it: too_large), a
// brush wider than four times the layer's side refused (its blurs' work).
void smudge_values(OpContext& c, std::size_t at, const std::string& id, const std::string& mode, const core::PenPoints& points, double width,
                   double strength) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    const Page& seen = c.doc.page(at);
    const Layer& layer = seen.layers[core::layer_by_id(seen, id)];
    const bool inks = seen.color.has_value();
    Size grid;
    if (inks) {
        if (layer.ink_raster) {
            const core::InkRasterHeader head = core::ink_raster_header(*layer.ink_raster);
            grid = Size{static_cast<int>(head.width), static_cast<int>(head.height)};
        } else {  // (none yet: ink_edit::with_marks's blank)
            grid = Size{mm_to_px(seen.spec.width_mm.value(), raster::kWorkingDpi), mm_to_px(seen.spec.height_mm.value(), raster::kWorkingDpi)};
        }
    } else {
        const core::ColorRasterView view(*layer.color_raster);
        grid = Size{static_cast<int>(view.width()), static_cast<int>(view.height())};
    }
    const double w_mm = seen.spec.width_mm.value(), h_mm = seen.spec.height_mm.value();
    const double sx = grid.width / w_mm, sy = grid.height / h_mm, scale = std::sqrt(sx * sy);
    if (!(width * scale <= 4.0 * std::max(grid.width, grid.height))) {
        throw OpError("the smudge is too wide for the layer (its width is past four times the layer's side): too_large");
    }
    const int dpi = std::max(1, static_cast<int>(std::lround(scale * 25.4)));
    double min_x = points.front().x, max_x = points.front().x, min_y = points.front().y, max_y = points.front().y;
    for (const auto& p : points) {
        min_x = core::py_min(min_x, p.x);
        max_x = core::py_max(max_x, p.x);
        min_y = core::py_min(min_y, p.y);
        max_y = core::py_max(max_y, p.y);
    }
    const double pad = width * 1.5;
    const double x0 = core::py_max(0.0, min_x - pad), y0 = core::py_max(0.0, min_y - pad);
    const double x1 = core::py_min(w_mm, max_x + pad), y1 = core::py_min(h_mm, max_y + pad);
    const double bx0 = core::py_round_whole(x0 * sx), by0 = core::py_round_whole(y0 * sy);
    const double bx1 = std::min<double>(core::py_round_whole(x1 * sx), grid.width), by1 = std::min<double>(core::py_round_whole(y1 * sy), grid.height);
    if (bx1 - bx0 < 2 || by1 - by0 < 2) throw OpError("the brush is off the page");
    const Box box{static_cast<int>(bx0), static_cast<int>(by0), static_cast<int>(bx1), static_cast<int>(by1)};
    // an ink layer: what it shows (its marks drawn) to work, its own inks to write (both whole, in the budget)
    std::optional<image_ops::Held> whole;
    std::optional<core::InkPixels> own_inks;
    std::optional<image_ops::Pixels> read;
    if (inks) {
        const std::uint64_t sample = seen.color->depth == 32 ? sizeof(float) : sizeof(std::uint16_t);
        whole.emplace(2 * std::uint64_t(grid.width) * std::uint64_t(grid.height) * (seen.color->planes() + 1) * sample);
        const core::InkPixels shown = ink_edit::with_marks(seen, layer, &c.doc);
        if (static_cast<int>(shown.width) != grid.width || static_cast<int>(shown.height) != grid.height) {
            throw core::Error("value", "the layer " + id + "'s inks are not of its grid");
        }
        own_inks = layer.ink_raster ? core::decode_ink_raster(*layer.ink_raster)
                                    : core::blank_ink_pixels(shown.space, shown.depth, shown.width, shown.height, shown.planes);
        read.emplace(image_ops::inks_part(shown, *seen.color, box));
    } else {
        read.emplace(image_ops::layer_pixels(c, at, id, box));
    }
    image_ops::Pixels& p = *read;
    const unsigned n = p.all();
    const auto w = static_cast<std::size_t>(box.width()), h = static_cast<std::size_t>(box.height());
    bool any = false;
    for (std::size_t i = 0; i < p.size() && !any; ++i) any = p.at(i)[n - 1] > 0.0f;
    if (!any) throw OpError("there is nothing on this layer to blend there");
    // the cover: the stroke drawn at the grid's resolution (from the box's corner), blurred, × the strength
    core::PenPoints shifted;
    const double ox = box.x0 / sx, oy = box.y0 / sy;
    for (const auto& q : points) shifted.push_back(core::PenPoint{q.x - ox, q.y - oy, q.p.value_or(0.7)});
    Image cover = Image::create("L", Size{box.width(), box.height()}, Ink(0));
    {
        Draw draw(cover);
        draw_stroke_mm(draw, shifted, dpi, width, Ink(255));
    }
    cover = cover.filter(Filter::gaussian_blur(core::py_max(1.0, width * scale / 6)));
    cover = cover.point([strength](int v) { return static_cast<int>(v * strength); });
    const std::string share = cover.tobytes();
    // the worked values: blurred, or carried forward along the line (each step's spot blended with the colour carried) —
    // premultiplied by the alpha, so a clear pixel lends no colour (its values are no colour: an ink layer's no ink, a
    // precise layer's black)
    const unsigned alpha = n - 1;
    const auto premultiplied = [&](std::size_t i, float* to) {
        const float* v = p.at(i);
        for (unsigned ch = 0; ch < alpha; ++ch) to[ch] = v[ch] * v[alpha];
        to[alpha] = v[alpha];
    };
    const image_ops::Held held(std::uint64_t(w) * h * n * sizeof(float));
    std::vector<float> worked(w * h * n);
    for (std::size_t i = 0; i < p.size(); ++i) premultiplied(i, worked.data() + i * n);
    if (mode == "blur" || mode == "blend") {
        image_ops::gauss_blur(worked, w, h, n, static_cast<float>(core::py_max(1.0, width * scale / (mode == "blur" ? 3 : 1.5))));
    } else {
        const auto r = static_cast<long>(std::max<std::int64_t>(2, core::py_round_int(width * scale / 2)));
        // (the spot's offsets from its centre, -r .. r - 1 each way, cut to those that ever meet the box — review P1-1: an
        // offset that never meets it neither reads nor lays a pixel of it, so the result is the same —: every step's centre
        // lies between its segment's ends, rounded as they are)
        long least_x = 0, most_x = 0, least_y = 0, most_y = 0;
        for (std::size_t i = 0; i < shifted.size(); ++i) {
            const long px = held_px(core::py_round_whole(shifted[i].x * sx)), py = held_px(core::py_round_whole(shifted[i].y * sy));
            least_x = i == 0 ? px : std::min(least_x, px);
            most_x = i == 0 ? px : std::max(most_x, px);
            least_y = i == 0 ? py : std::min(least_y, py);
            most_y = i == 0 ? py : std::max(most_y, py);
        }
        // (a pixel more each way: a step's centre rounded as its ends are, to the float)
        const long lo_x = std::max(-r, -(most_x + 1)), hi_x = std::min(r - 1, long(w) - 1 - (least_x - 1));
        const long lo_y = std::max(-r, -(most_y + 1)), hi_y = std::min(r - 1, long(h) - 1 - (least_y - 1));
        const auto span_x = std::size_t(std::max(0L, hi_x - lo_x + 1)), span_y = std::size_t(std::max(0L, hi_y - lo_y + 1));
        // (the spot, what is carried and what is laid: three of them, in the budget)
        if (double(span_x) * double(span_y) * double(n) * sizeof(float) * 3.0 > 4.0e18) image_ops::too_large();
        const image_ops::Held spots(3 * std::uint64_t(span_x) * std::uint64_t(span_y) * n * sizeof(float));
        std::vector<float> carried, here(span_x * span_y * n), laid(span_x * span_y * n);
        const auto in_box = [&](long x, long y) { return x >= 0 && y >= 0 && x < long(w) && y < long(h); };
        for (std::size_t i = 0; i + 1 < shifted.size() && span_x > 0 && span_y > 0; ++i) {
            const double ax = shifted[i].x, ay = shifted[i].y, bx = shifted[i + 1].x, by = shifted[i + 1].y;
            const auto steps = std::max<std::int64_t>(1, core::loop_count(core::py_dist(ax, ay, bx, by) * scale / core::py_max(1.0, static_cast<double>(r) / 3.0)));
            for (std::int64_t k = 0; k < steps; ++k) {
                const double t = static_cast<double>(k) / static_cast<double>(steps);
                const long cx = held_px(core::py_round_whole((ax + (bx - ax) * t) * sx));
                const long cy = held_px(core::py_round_whole((ay + (by - ay) * t) * sy));
                // (the spot as a crop: nothing past the box)
                for (std::size_t qy = 0; qy < span_y; ++qy) {
                    for (std::size_t qx = 0; qx < span_x; ++qx) {
                        const long x = cx + lo_x + long(qx), y = cy + lo_y + long(qy);
                        float* to = here.data() + (qy * span_x + qx) * n;
                        if (!in_box(x, y)) {
                            std::fill_n(to, n, 0.0f);
                        } else {
                            std::copy_n(worked.begin() + static_cast<std::ptrdiff_t>((std::size_t(y) * w + std::size_t(x)) * n), n, to);
                        }
                    }
                }
                if (carried.empty()) {
                    carried = here;
                } else {
                    for (std::size_t v = 0; v < carried.size(); ++v) carried[v] += (here[v] - carried[v]) * static_cast<float>(1 - strength);
                }
                for (std::size_t v = 0; v < laid.size(); ++v) laid[v] = here[v] + (carried[v] - here[v]) * static_cast<float>(strength);
                for (std::size_t qy = 0; qy < span_y; ++qy) {  // (pasted where it lies on the box)
                    for (std::size_t qx = 0; qx < span_x; ++qx) {
                        const long x = cx + lo_x + long(qx), y = cy + lo_y + long(qy);
                        if (!in_box(x, y)) continue;
                        std::copy_n(laid.data() + (qy * span_x + qx) * n, n, worked.begin() + static_cast<std::ptrdiff_t>((std::size_t(y) * w + std::size_t(x)) * n));
                    }
                }
            }
        }
    }
    // the worked values laid through the cover over the layer's, premultiplied (only the pixels they change: one whose
    // worked values are its own keeps its bytes)
    std::vector<std::uint8_t> marks(p.size(), 0);
    std::vector<float> own(n);
    bool changed = false;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const float k = static_cast<unsigned char>(share[i]) / 255.0f;
        if (!(k > 0.0f)) continue;
        const float* o = worked.data() + i * n;
        premultiplied(i, own.data());
        if (std::equal(own.begin(), own.end(), o)) continue;
        float* v = p.at(i);
        const float a = std::clamp(own[alpha] + (o[alpha] - own[alpha]) * k, 0.0f, 1.0f);
        for (unsigned ch = 0; ch < alpha; ++ch) v[ch] = a > 0.0f ? (own[ch] + (o[ch] - own[ch]) * k) / a : 0.0f;
        v[alpha] = a;
        marks[i] = 1;
        changed = true;
    }
    if (!changed) return;
    if (inks) {
        image_ops::put_inks_part(*own_inks, p, box, &marks);  // (into its own inks: its lines and pictures stay as they are)
        core::Page& edited = c.doc.edit_page(at);
        core::Layer& out = edited.layers[core::layer_by_id(edited, id)];
        out.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(*own_inks));
        out.ink_memo = {};
    } else {
        image_ops::put_layer_pixels(c.doc, at, id, std::vector<image_ops::PixelsPart>{image_ops::PixelsPart{box, &p, &marks}});
    }
}

void smudge(OpContext& c) {
    use_brushes_of(c.doc);
    const Json& op = c.op;
    const std::size_t at = core::require_page(c.doc, op);
    Page& page = c.doc.edit_page(at);
    Layer& target = core::paint_target(page, op);
    const std::string mode = core::truthy_at(op, "mode") ? core::py_str(op["mode"]) : std::string("blur");
    if (mode != "blur" && mode != "smudge" && mode != "blend") throw OpError("mode must be blur, smudge or blend");
    const core::PenPoints points = core::parse_points(core::py_or(op_get(op, "points"), Json::array()));
    if (points.size() < 2) throw OpError("points needs at least two [x_mm, y_mm] pairs");
    const Json width_given = op_get(op, "width_mm");
    const double width = core::py_max(0.3, core::py_truthy(width_given) ? core::to_float(width_given) : 6.0);
    const double strength = core::py_max(0.05, core::py_min(1.0, float_at(op, "strength", 0.6)));
    if (smudges_values(page, target)) {  // (PC4 E5: a precise or ink paint layer's own values)
        smudge_values(c, at, target.id, mode, points, width, strength);
        return;
    }
    constexpr int dpi = kSmudgeDpi;
    const double scale = dpi / 25.4;
    double min_x = points.front().x, max_x = points.front().x, min_y = points.front().y, max_y = points.front().y;
    for (const auto& p : points) {
        min_x = core::py_min(min_x, p.x);
        max_x = core::py_max(max_x, p.x);
        min_y = core::py_min(min_y, p.y);
        max_y = core::py_max(max_y, p.y);
    }
    const double pad = width * 1.5;
    const double x0 = core::py_max(0.0, min_x - pad), y0 = core::py_max(0.0, min_y - pad);
    const double x1 = core::py_min(page.spec.width_mm.value(), max_x + pad), y1 = core::py_min(page.spec.height_mm.value(), max_y + pad);
    // (compared as Python's whole numbers first: past the page, the box is never made)
    const double bx0 = core::py_round_whole(x0 * scale), by0 = core::py_round_whole(y0 * scale);
    const double bx1 = core::py_round_whole(x1 * scale), by1 = core::py_round_whole(y1 * scale);
    if (bx1 - bx0 < 2 || by1 - by0 < 2) throw OpError("the brush is off the page");
    const Box box{static_cast<int>(bx0), static_cast<int>(by0), static_cast<int>(bx1), static_cast<int>(by1)};
    const Image layer = layer_image(page, target, dpi, &c.doc).crop(box);
    if (!layer.getbbox()) throw OpError("there is nothing on this layer to blend there");
    core::PenPoints shifted;
    for (const auto& p : points) shifted.push_back(core::PenPoint{p.x - x0, p.y - y0, p.p.value_or(0.7)});
    Image cover = Image::create("L", layer.size(), Ink(0));
    {
        Draw draw(cover);
        draw_stroke_mm(draw, shifted, dpi, width, Ink(255));
    }
    cover = cover.filter(Filter::gaussian_blur(core::py_max(1.0, width * scale / 6)));
    cover = cover.point([strength](int v) { return static_cast<int>(v * strength); });
    Image worked;
    if (mode == "blur" || mode == "blend") {
        const double radius = core::py_max(1.0, width * scale / (mode == "blur" ? 3 : 1.5));
        worked = layer.filter(Filter::gaussian_blur(radius));
    } else {
        // 指先: carry the colour under the start of each step forward along the line
        worked = layer.copy();
        const auto r = static_cast<int>(std::max<std::int64_t>(2, core::py_round_int(width * scale / 2)));
        std::optional<Image> carried;
        for (std::size_t i = 0; i + 1 < shifted.size(); ++i) {
            const double ax = shifted[i].x, ay = shifted[i].y, bx = shifted[i + 1].x, by = shifted[i + 1].y;
            const auto steps = std::max<std::int64_t>(
                1, core::loop_count(core::py_dist(ax, ay, bx, by) * scale / core::py_max(1.0, r / 3.0)));
            for (std::int64_t k = 0; k < steps; ++k) {
                const double t = static_cast<double>(k) / static_cast<double>(steps);
                const int cx = held_px(core::py_round_whole((ax + (bx - ax) * t) * scale));
                const int cy = held_px(core::py_round_whole((ay + (by - ay) * t) * scale));
                const Box spot{cx - r, cy - r, cx + r, cy + r};
                const Image here = worked.crop(spot);
                carried = carried ? blend(*carried, here, 1 - strength) : here;
                worked.paste(blend(here, *carried, strength), Point{spot.x0, spot.y0});
            }
        }
    }
    const Image result = composite(worked, layer, cover);
    const std::vector<Image> bands = chops::difference(result, layer).split();
    Image moved = bands[0];
    for (std::size_t i = 1; i < bands.size(); ++i) moved = chops::lighter(moved, bands[i]);
    // (every channel: RGBA boxes see only alpha)
    const auto changed = moved.point([](int v) { return v > 2 ? 255 : 0; }).getbbox();
    if (!changed) return;
    core::Patch templ;
    templ.attrs = Json{{"mode", "image"}, {"opacity", 1.0}};
    if (auto patch = selection::to_patch(result.crop(*changed).convert("RGBA"), box.x0 + changed->x0, box.y0 + changed->y0, templ, dpi)) {
        target.patches.push_back(std::move(*patch));
    }
}

// --- ゆがみ --------------------------------------------------------------------------------------------------------

struct Dab {
    double x, y, dx, dy;
};

// layerops._displace: where a point goes under one dab (mm)
std::pair<double, double> displace(double x, double y, const Dab& dab, double radius, const std::string& mode, double strength) {
    const double rx = x - dab.x, ry = y - dab.y;
    const double d = std::hypot(rx, ry);  // (np.hypot: the C library's)
    const double clipped = std::min(std::max(1 - d / radius, 0.0), 1.0);
    const double fall = clipped * clipped * strength;  // (numpy's ** 2 is a square)
    if (mode == "push") return {x + dab.dx * fall, y + dab.dy * fall};
    if (mode == "pinch" || mode == "bloat") {
        const double k = mode == "pinch" ? -0.5 : 0.5;
        return {x + rx * fall * k, y + ry * fall * k};
    }
    const double angle = fall * (mode == "twirl_cw" ? 0.6 : -0.6);
    const double c = std::cos(angle), s = std::sin(angle);
    return {dab.x + rx * c - ry * s, dab.y + rx * s + ry * c};
}

// layerops._undo: the place one dab moved to (tx, ty) back to where it came from (a few fixed-point steps)
std::pair<double, double> undo(double tx, double ty, const Dab& dab, double radius, const std::string& mode, double strength) {
    double x = tx, y = ty;
    for (int i = 0; i < 5; ++i) {
        const auto [fx, fy] = displace(x, y, dab, radius, mode, strength);
        x = tx - (fx - x);
        y = ty - (fy - y);
    }
    return {x, y};
}

// layerops._dabs: (x, y, dx, dy) along the drag, a quarter radius apart
std::vector<Dab> dabs_of(const std::vector<std::array<double, 2>>& points, double radius) {
    std::vector<Dab> out;
    if (points.size() == 1) return {Dab{points[0][0], points[0][1], 0.0, 0.0}};
    const double step = core::py_max(0.2, radius / 4);
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const double x0 = points[i][0], y0 = points[i][1], x1 = points[i + 1][0], y1 = points[i + 1][1];
        const double length = core::py_hypot(x1 - x0, y1 - y0);
        const auto n = std::max<std::int64_t>(1, core::loop_count(length / step));
        for (std::int64_t k = 0; k < n; ++k) {
            const double t = static_cast<double>(k) / static_cast<double>(n);
            out.push_back(Dab{x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, (x1 - x0) / static_cast<double>(n), (y1 - y0) / static_cast<double>(n)});
        }
    }
    return out;
}

// layerops._densify: points added along the parts of a line inside the box, `step` apart (pressure follows)
void densify(core::Stroke& stroke, const std::array<double, 4>& box, double step) {
    const std::vector<core::PointF> pts = stroke.points;
    if (pts.size() < 2) return;
    const std::vector<double> pressure = stroke.pressure.size() == pts.size() ? stroke.pressure : std::vector<double>(pts.size(), 1.0);
    const auto [x0, y0, x1, y1] = box;
    std::vector<core::PointF> out{pts[0]};
    std::vector<double> out_p{pressure[0]};
    for (std::size_t k = 1; k < pts.size(); ++k) {
        const double ax = pts[k - 1].x, ay = pts[k - 1].y, bx = pts[k].x, by = pts[k].y;
        const bool touches = !(core::py_max(ax, bx) < x0 || core::py_min(ax, bx) > x1 || core::py_max(ay, by) < y0 || core::py_min(ay, by) > y1);
        const auto n = touches ? std::max<std::int64_t>(1, core::loop_count(core::py_hypot(bx - ax, by - ay) / step)) : std::int64_t{1};
        for (std::int64_t i = 1; i <= n; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(n);
            out.push_back(core::PointF{ax + (bx - ax) * t, ay + (by - ay) * t});
            out_p.push_back(pressure[k - 1] + (pressure[k] - pressure[k - 1]) * t);
        }
    }
    if (out.size() != pts.size()) {
        stroke.points = std::move(out);
        stroke.pressure = std::move(out_p);
    }
}

void liquify(OpContext& c) {
    if (core::truthy_at(c.op, "strokes")) {  // (PC3 C-c: Filter › Liquify's strokes replayed on a field, render/distort_ops)
        liquify_strokes(c);
        return;
    }
    use_brushes_of(c.doc);
    const Json& op = c.op;
    const std::size_t at = core::require_page(c.doc, op);
    Page& page = c.doc.edit_page(at);
    Layer& layer = core::paint_target(page, op);
    const std::string mode = core::truthy_at(op, "mode") ? core::py_str(op["mode"]) : std::string("push");
    if (mode != "push" && mode != "pinch" && mode != "bloat" && mode != "twirl_cw" && mode != "twirl_ccw") {
        throw OpError("mode must be one of push, pinch, bloat, twirl_cw, twirl_ccw");
    }
    std::vector<std::array<double, 2>> points;
    try {
        for (const Json& p : core::iterate(core::py_or(op_get(op, "points"), Json::array()))) {
            points.push_back({core::to_float(core::subscript(p, 0)), core::to_float(core::subscript(p, 1))});
        }
    } catch (const core::PyUncaught& e) {
        if (e.type() != "IndexError") throw;
        throw OpError("points is [[x, y], ...] in mm");
    } catch (const core::PyValueError&) {
        throw OpError("points is [[x, y], ...] in mm");
    } catch (const core::PyTypeError&) {
        throw OpError("points is [[x, y], ...] in mm");
    }
    if (points.empty()) throw OpError("points is [[x, y], ...] in mm");
    const double radius = core::py_max(0.5, float_at(op, "width_mm", 10) / 2);
    const double strength = core::py_max(0.05, core::py_min(1.0, float_at(op, "strength", 0.6)));
    std::vector<Dab> dabs = dabs_of(points, radius);
    if (mode != "push") {
        const std::size_t every = std::max<std::size_t>(1, dabs.size() / 40);
        std::vector<Dab> kept;
        for (std::size_t i = 0; i < dabs.size(); i += every) kept.push_back(Dab{dabs[i].x, dabs[i].y, 0.0, 0.0});
        dabs = std::move(kept);
    }
    double lx = dabs[0].x, hx = dabs[0].x, ly = dabs[0].y, hy = dabs[0].y;
    for (const Dab& d : dabs) {
        lx = core::py_min(lx, d.x);
        hx = core::py_max(hx, d.x);
        ly = core::py_min(ly, d.y);
        hy = core::py_max(hy, d.y);
    }
    const std::array<double, 4> reach{lx - radius, ly - radius, hx + radius, hy + radius};
    // pen lines: their points move (more points first where the tool reaches)
    std::vector<core::StrokePtr> items = layer.strokes->items;
    for (core::StrokePtr& s : items) {
        auto stroke = std::make_shared<core::Stroke>(*s);
        densify(*stroke, reach, radius / 4);
        for (core::PointF& p : stroke->points) {
            double x = p.x, y = p.y;
            for (const Dab& dab : dabs) std::tie(x, y) = displace(x, y, dab, radius, mode, strength);
            p = core::PointF{core::py_round(x, 3), core::py_round(y, 3)};
        }
        s = std::move(stroke);
    }
    layer.strokes = core::make_strokes(std::move(items));
    if (shape_parts::any(layer)) {
        // (pen-shapes@1, PC3 C-b: a shape part the tool reaches bent by the same pushes — each segment cut to 1.5 mm, every
        // anchor and handle moved, as a line's points are; its live shape gone)
        std::vector<core::vector::ShapePart> parts = layer.shapes->items;
        for (core::vector::ShapePart& part : parts) {
            const auto b = shape_parts::drawn_bounds(part);
            const auto ctl = part.path.control_bounds();
            if (!b || !ctl || std::max((*b)[2], (*ctl)[2]) < reach[0] || std::min((*b)[0], (*ctl)[0]) > reach[2] ||
                std::max((*b)[3], (*ctl)[3]) < reach[1] || std::min((*b)[1], (*ctl)[1]) > reach[3]) {
                continue;
            }
            part.path = shape_parts::bent(part.path, [&](double x, double y) {
                for (const Dab& dab : dabs) std::tie(x, y) = displace(x, y, dab, radius, mode, strength);
                return std::pair<double, double>{x, y};
            });
            part.live.reset();
            part = core::vector::part_from_json(core::vector::to_json(part));  // (its normal form, as kept)
        }
        layer.shapes = core::vector::make_shapes(std::move(parts));
    }
    if ((layer.raster_png && !layer.raster_png->empty()) || !layer.patches.empty()) {
        // pixels: each place fetches from where the dabs pulled it
        const int dpi = raster::working_dpi(page);  // (PC4 F-b: a page of grid "finest" at its grid; any other at the working dpi)
        Layer pixels_only = layer;
        pixels_only.strokes = core::empty_strokes();
        pixels_only.shapes = core::vector::empty_shapes();  // (pen-shapes@1: its parts bent above, not drawn in)
        pixels_only.mask.reset();
        pixels_only.vector_mask.reset();  // (vector@1, PC3 C-a)
        Image picture = layer_image(page, pixels_only, dpi, &c.doc);
        if (picture.getbbox()) {
            const double scale = dpi / 25.4;
            const int x0 = std::max(0, held_px(std::trunc(reach[0] * scale)) - 2);
            const int y0 = std::max(0, held_px(std::trunc(reach[1] * scale)) - 2);
            const int x1 = std::min(picture.width(), held_px(std::trunc(reach[2] * scale)) + 3);
            const int y1 = std::min(picture.height(), held_px(std::trunc(reach[3] * scale)) + 3);
            if (x1 > x0 && y1 > y0) {
                const int w = x1 - x0, h = y1 - y0;
                std::vector<double> map_x(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
                std::vector<double> map_y(map_x.size());
                for (int j = 0; j < h; ++j) {
                    for (int i = 0; i < w; ++i) {
                        double sx = (static_cast<double>(x0 + i) + 0.5) / scale, sy = (static_cast<double>(y0 + j) + 0.5) / scale;
                        for (auto dab = dabs.rbegin(); dab != dabs.rend(); ++dab) {  // (backwards: where this place's pixel came from)
                            std::tie(sx, sy) = undo(sx, sy, *dab, radius, mode, strength);
                        }
                        const std::size_t at_px = static_cast<std::size_t>(j) * static_cast<std::size_t>(w) + static_cast<std::size_t>(i);
                        map_x[at_px] = sx * scale - 0.5;
                        map_y[at_px] = sy * scale - 0.5;
                    }
                }
                picture.paste(filters::remap_area(picture, w, h, map_x, map_y), Point{x0, y0});
            }
            layer.patches.clear();
            raster::save_raster(page, layer, picture);
        }
    }
}

}  // namespace

bool smudges_values(const core::Page& page, const core::Layer& layer) {
    if (layer.kind != core::LayerKind::Raster) return false;
    // (an ink page's paint layer — of lines alone too, review P2-2: its lines read with it, and kept lines)
    if (page.color) return layer.raster_png == nullptr;
    // (a precise one without lines, pictures or shape parts: with them, lent the 8-bit picture as before — its lines kept)
    return layer.color_raster != nullptr && layer.stroke_count() == 0 && layer.patches.empty() && !shape_parts::any(layer);
}

void register_paint_ops(core::OpRegistry& registry) {
    registry.add("add_shape", add_shape);
    registry.add("smudge", smudge);
    registry.add("liquify", liquify);
}

}  // namespace genko::render
