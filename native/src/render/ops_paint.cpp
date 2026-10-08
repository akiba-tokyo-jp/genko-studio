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
#include "core/command_bus.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/stroke_tools.hpp"
#include "core/strokes.hpp"
#include "render/draw.hpp"
#include "render/fill_patches.hpp"
#include "render/filters.hpp"
#include "render/page.hpp"
#include "render/raster.hpp"
#include "render/raster_ops.hpp"
#include "render/selection.hpp"
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
    if ((layer.raster_png && !layer.raster_png->empty()) || !layer.patches.empty()) {
        // pixels: each place fetches from where the dabs pulled it
        constexpr int dpi = raster::kWorkingDpi;
        Layer pixels_only = layer;
        pixels_only.strokes = core::empty_strokes();
        pixels_only.mask.reset();
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

void register_paint_ops(core::OpRegistry& registry) {
    registry.add("add_shape", add_shape);
    registry.add("smudge", smudge);
    registry.add("liquify", liquify);
}

}  // namespace genko::render
