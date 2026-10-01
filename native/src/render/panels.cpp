// Panels on the page: where they reach (Python's genko/placement.py and frames.offset/_round_corners), the masks that
// cut the layers (render._clip_mask, _draw_frame_mask, fill_frame, _bleed_shape_mask) and their borders
// (render._draw_frames, draw_border, _dashes, _rough, _bleed_border, _draw_crop_marks).

#include <algorithm>
#include <cmath>

#include "core/error.hpp"
#include "core/frames.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyrandom.hpp"
#include "core/stroke_geom.hpp"
#include "render/page_internal.hpp"

namespace genko::render::detail {

namespace {

using core::Num;

constexpr double kEdgeEps = 0.5;  // placement.EDGE_EPS_MM
constexpr double kEps = 1e-6;     // frames.EPS

double pmin(double a, double b) { return b < a ? b : a; }
double pmax(double a, double b) { return b > a ? b : a; }

double v(const Num& n) { return n.value(); }

bool has_poly(const core::Frame& frame) { return frame.poly && !frame.poly->empty(); }

// frames.shape as floats: the polygon, or the rect's corners
std::vector<PointMM> shape_mm(const core::Frame& frame) {
    std::vector<PointMM> out;
    for (const core::Point& p : core::shape(frame)) out.push_back(PointMM{v(p.x), v(p.y)});
    return out;
}

double signed_area(const std::vector<PointMM>& pts) {
    // sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(points, points[1:] + points[:1])) / 2
    std::vector<double> terms;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const PointMM& a = pts[i];
        const PointMM& b = pts[(i + 1) % pts.size()];
        terms.push_back(a.x * b.y - b.x * a.y);
    }
    return core::py_float_sum(terms) / 2;
}

// frames._round_corners(pts, radius, keep)
std::vector<PointMM> round_corners(const std::vector<PointMM>& pts, double radius, const std::vector<bool>& keep) {
    std::vector<PointMM> out;
    const std::size_t n = pts.size();
    for (std::size_t i = 0; i < n; ++i) {
        const PointMM& p = pts[i];
        if (i < keep.size() && keep[i]) {
            out.push_back(p);
            continue;
        }
        const PointMM& a = pts[(i + n - 1) % n];
        const PointMM& b = pts[(i + 1) % n];
        const double la = core::py_dist(p.x, p.y, a.x, a.y);
        const double lb = core::py_dist(p.x, p.y, b.x, b.y);
        if (la < kEps || lb < kEps) {
            out.push_back(p);
            continue;
        }
        const double ux = (a.x - p.x) / la;
        const double uy = (a.y - p.y) / la;
        const double vx = (b.x - p.x) / lb;
        const double vy = (b.y - p.y) / lb;
        const double cos = pmax(-1.0, pmin(1.0, ux * vx + uy * vy));
        const double half = core::py_acos(cos) / 2;
        if (half < 1e-3 || half > core::kPi / 2 - 1e-3) {
            out.push_back(p);
            continue;
        }
        const double t = pmin(pmin(radius / core::py_tan(half), la / 2), lb / 2);
        const double r = t * core::py_tan(half);
        const PointMM s0{p.x + ux * t, p.y + uy * t};
        const PointMM s1{p.x + vx * t, p.y + vy * t};
        const double bisx = ux + vx;
        const double bisy = uy + vy;
        double bl = core::py_hypot(bisx, bisy);
        if (bl == 0.0) bl = 1.0;
        const double d = r / core::py_sin(half);
        const PointMM c{p.x + bisx / bl * d, p.y + bisy / bl * d};
        const double a0 = core::py_atan2(s0.y - c.y, s0.x - c.x);
        const double a1 = core::py_atan2(s1.y - c.y, s1.x - c.x);
        const double sweep = core::py_fmod(a1 - a0 + core::kPi, 2 * core::kPi) - core::kPi;
        const auto steps = static_cast<std::int64_t>(pmax(3.0, std::trunc(std::fabs(sweep) * r / 0.6)));
        for (std::int64_t k = 0; k <= steps; ++k) {
            const double angle = a0 + sweep * static_cast<double>(k) / static_cast<double>(steps);
            out.push_back(PointMM{c.x + r * core::py_cos(angle), c.y + r * core::py_sin(angle)});
        }
    }
    return out;
}

const core::Json* style_of(const core::Frame& frame) {
    if (!frame.line) return nullptr;
    return &*frame.line;
}

// Python's style.get(key, default) (style a dict), the value or nothing
const core::Json* get(const core::Json* style, std::string_view key) {
    if (style == nullptr || !style->is_object()) return nullptr;
    const auto it = style->find(key);
    return it == style->end() ? nullptr : &*it;
}

bool truthy(const core::Json* value) { return value != nullptr && core::py_truthy(*value); }

// style.get("kind", "solid") != "solid"
bool styled_kind(const core::Json* style) {
    const core::Json* kind = get(style, "kind");
    if (kind == nullptr) return false;
    return !(kind->is_string() && kind->get<std::string>() == "solid");
}

std::vector<PointD> to_px(const std::vector<PointMM>& pts, int dpi) {
    std::vector<PointD> out;
    out.reserve(pts.size());
    for (const PointMM& p : pts) out.push_back(xy_point(p.x, p.y, dpi));
    return out;
}

}  // namespace

// --- geometry -----------------------------------------------------------------------------------------------------

std::pair<int, int> xy(double x_mm, double y_mm, int dpi) { return {mm_to_px(x_mm, dpi), mm_to_px(y_mm, dpi)}; }

PointD xy_point(double x_mm, double y_mm, int dpi) {
    const auto [x, y] = xy(x_mm, y_mm, dpi);
    return PointD{static_cast<double>(x), static_cast<double>(y)};
}

Edges outer_edges(const core::Page& page, const core::Frame& frame) {
    const core::Rect inner = page.inner_rect_mm();
    const core::Rect& r = frame.rect;
    const Num eps(kEdgeEps);
    Edges e;
    e.left = core::py_abs(r.x - inner.x) < eps;
    e.top = core::py_abs(r.y - inner.y) < eps;
    e.right = core::py_abs((r.x + r.width) - (inner.x + inner.width)) < eps;
    e.bottom = core::py_abs((r.y + r.height) - (inner.y + inner.height)) < eps;
    return e;
}

std::optional<std::vector<PointMM>> bleed_poly(const core::Page& page, const core::Frame* frame) {
    if (frame == nullptr || !frame->bleed || !has_poly(*frame)) return std::nullopt;
    const core::Rect inner = page.inner_rect_mm();
    const core::Rect bleed = page.bleed_rect_mm();
    const double ix0 = v(inner.x);
    const double ix1 = v(inner.x + inner.width);
    const double iy0 = v(inner.y);
    const double iy1 = v(inner.y + inner.height);
    std::vector<PointMM> out;
    for (const core::Point& point : *frame->poly) {
        double x = v(point.x);
        double y = v(point.y);
        if (std::fabs(x - ix0) < kEdgeEps) {
            x = v(bleed.x);
        } else if (std::fabs(x - ix1) < kEdgeEps) {
            x = v(bleed.x + bleed.width);
        }
        if (std::fabs(y - iy0) < kEdgeEps) {
            y = v(bleed.y);
        } else if (std::fabs(y - iy1) < kEdgeEps) {
            y = v(bleed.y + bleed.height);
        }
        out.push_back(PointMM{x, y});
    }
    return out;
}

std::optional<std::vector<PointMM>> bleed_outline(const core::Page& page, const core::Frame* frame,
                                                  std::optional<double> beyond_mm) {
    if (frame == nullptr || !frame->bleed) return std::nullopt;
    const core::Rect inner = page.inner_rect_mm();
    double far_left = 0;
    double far_top = 0;
    double far_right = 0;
    double far_bottom = 0;
    if (!beyond_mm) {
        const core::Rect b = page.bleed_rect_mm();
        far_left = v(b.x);
        far_top = v(b.y);
        far_right = v(b.x + b.width);
        far_bottom = v(b.y + b.height);
    } else {
        far_left = -*beyond_mm;
        far_top = -*beyond_mm;
        far_right = v(page.spec.width_mm + Num(*beyond_mm));
        far_bottom = v(page.spec.height_mm + Num(*beyond_mm));
    }
    const double ix0 = v(inner.x);
    const double ix1 = v(inner.x + inner.width);
    const double iy0 = v(inner.y);
    const double iy1 = v(inner.y + inner.height);
    std::vector<PointMM> out;
    std::vector<bool> moved;
    for (const PointMM& p : shape_mm(*frame)) {
        double nx = p.x;
        double ny = p.y;
        if (std::fabs(p.x - ix0) < kEdgeEps) {
            nx = far_left;
        } else if (std::fabs(p.x - ix1) < kEdgeEps) {
            nx = far_right;
        }
        if (std::fabs(p.y - iy0) < kEdgeEps) {
            ny = far_top;
        } else if (std::fabs(p.y - iy1) < kEdgeEps) {
            ny = far_bottom;
        }
        moved.push_back(nx != p.x || ny != p.y);
        out.push_back(PointMM{nx, ny});
    }
    const double radius = frame->corner_mm;
    if (radius > 0) return round_corners(out, radius, moved);
    return out;
}

bool on_bleed_edge(const core::Page& page, const PointMM& a, const PointMM& b) {
    const core::Rect bleed = page.bleed_rect_mm();
    const std::pair<double, int> sides[] = {{v(bleed.x), 0}, {v(bleed.x + bleed.width), 0}, {v(bleed.y), 1},
                                            {v(bleed.y + bleed.height), 1}};
    for (const auto& [fixed, index] : sides) {
        const double ca = index == 0 ? a.x : a.y;
        const double cb = index == 0 ? b.x : b.y;
        if (std::fabs(ca - fixed) < kEdgeEps && std::fabs(cb - fixed) < kEdgeEps) return true;
    }
    return false;
}

bool in_poly(std::span<const PointMM> points, double x, double y) {
    bool inside = false;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const PointMM& p0 = points[i];
        const PointMM& p1 = points[(i + 1) % points.size()];
        if ((p0.y > y) != (p1.y > y) && x < p0.x + (y - p0.y) * (p1.x - p0.x) / (p1.y - p0.y)) inside = !inside;
    }
    return inside;
}

core::Rect clip_box(const core::Page& page, const core::Frame* frame, std::string_view clip_to) {
    const core::Rect full{Num(0), Num(0), page.spec.width_mm, page.spec.height_mm};
    if (frame == nullptr || clip_to == "none") return full;
    const core::Rect& r = frame->rect;
    if (clip_to != "bleed") return r;
    const Edges edges = outer_edges(page, *frame);
    const core::Rect bleed = page.bleed_rect_mm();
    const Num x0 = edges.left ? bleed.x : r.x;
    const Num y0 = edges.top ? bleed.y : r.y;
    const Num x1 = edges.right ? bleed.x + bleed.width : r.x + r.width;
    const Num y1 = edges.bottom ? bleed.y + bleed.height : r.y + r.height;
    return core::Rect{x0, y0, x1 - x0, y1 - y0};
}

std::vector<PointMM> outline_mm(const core::Frame& frame) {
    std::vector<PointMM> out;
    for (const core::Point& p : core::outline(frame)) out.push_back(PointMM{v(p.x), v(p.y)});
    return out;
}

std::vector<PointMM> offset_outline(const std::vector<PointMM>& points, double d) {
    const std::size_t n = points.size();
    if (n < 3) return points;
    const double sign = signed_area(points) > 0 ? 1.0 : -1.0;
    std::vector<PointMM> out;
    for (std::size_t i = 0; i < n; ++i) {
        const PointMM& p0 = points[(i + n - 1) % n];
        const PointMM& p1 = points[i];
        const PointMM& p2 = points[(i + 1) % n];
        double normals[2][2];
        const PointMM* ends[2][2] = {{&p0, &p1}, {&p1, &p2}};
        for (int k = 0; k < 2; ++k) {
            const double dx = ends[k][1]->x - ends[k][0]->x;
            const double dy = ends[k][1]->y - ends[k][0]->y;
            double length = core::py_hypot(dx, dy);
            if (length == 0.0) length = 1.0;
            normals[k][0] = -dy / length * sign;
            normals[k][1] = dx / length * sign;
        }
        double mx = normals[0][0] + normals[1][0];
        double my = normals[0][1] + normals[1][1];
        const double length = core::py_hypot(mx, my);
        if (length < 1e-6) {
            out.push_back(PointMM{p1.x + normals[0][0] * d, p1.y + normals[0][1] * d});
            continue;
        }
        mx = mx / length;
        my = my / length;
        const double cos = pmax(0.25, mx * normals[0][0] + my * normals[0][1]);
        out.push_back(PointMM{p1.x + mx * d / cos, p1.y + my * d / cos});
    }
    return out;
}

const core::Frame* panel_of(const core::Page& page, double x, double y) {
    for (const core::Frame* frame : page.leaf_frames()) {
        if (!frame->clip) continue;
        const auto shape = bleed_poly(page, frame);
        if (shape) {
            if (in_poly(*shape, x, y)) return frame;
        } else if (frame->bleed && !has_poly(*frame)) {
            if (clip_box(page, frame, "bleed").contains(Num(x), Num(y))) return frame;
        } else if (core::contains(*frame, Num(x), Num(y))) {
            return frame;
        }
    }
    return nullptr;
}

// --- masks ----------------------------------------------------------------------------------------------------------

void fill_frame(Draw& draw, const core::Frame& frame, int dpi, int fill) {
    if (core::rounded(frame)) {
        draw.polygon(to_px(outline_mm(frame), dpi), Ink(fill));
    } else if (has_poly(frame)) {
        std::vector<PointMM> pts;
        for (const core::Point& p : *frame.poly) pts.push_back(PointMM{v(p.x), v(p.y)});
        draw.polygon(to_px(pts, dpi), Ink(fill));
    } else {
        const Box b = rect_px(frame.rect, dpi);
        draw.rectangle(BoxF{static_cast<double>(b.x0), static_cast<double>(b.y0), static_cast<double>(b.x1), static_cast<double>(b.y1)},
                       Ink(fill));
    }
}

bool bleed_shape_mask(Draw& draw, const core::Page& page, const core::Frame& frame, int dpi) {
    if (!frame.bleed || !core::rounded(frame)) return false;
    const auto outline = bleed_outline(page, &frame, std::nullopt);
    draw.polygon(to_px(*outline, dpi), Ink(255));
    return true;
}

namespace {

void draw_panel_area(Draw& draw, const core::Page& page, const core::Frame& frame, int dpi) {
    const auto shape = bleed_poly(page, &frame);
    if (bleed_shape_mask(draw, page, frame, dpi)) return;
    if (shape) {  // a slanted bleed panel: out to the bleed, its slanted sides kept
        draw.polygon(to_px(*shape, dpi), Ink(255));
    } else if (frame.bleed && !has_poly(frame)) {  // a bleed panel runs out to the bleed
        const Box b = rect_px(clip_box(page, &frame, "bleed"), dpi);
        draw.rectangle(BoxF{static_cast<double>(b.x0), static_cast<double>(b.y0), static_cast<double>(b.x1), static_cast<double>(b.y1)},
                       Ink(255));
    } else {
        fill_frame(draw, frame, dpi);
    }
}

}  // namespace

std::optional<Image> clip_mask(const core::Page& page, Size size, int dpi, const Box& area) {
    std::vector<const core::Frame*> leaves;
    for (const core::Frame* frame : page.leaf_frames()) {
        if (frame->clip) leaves.push_back(frame);
    }
    if (leaves.empty()) return std::nullopt;
    Image mask = Image::create("L", Size{area.width(), area.height()}, Ink(0));
    PageCanvas canvas(mask, area, size);
    for (const core::Frame* frame : leaves) draw_panel_area(canvas.draw(), page, *frame, dpi);
    canvas.commit();
    return mask;
}

Image frame_mask(const core::Page& page, const core::Frame& frame, Size size, int dpi, const Box& area) {
    Image mask = Image::create("L", Size{area.width(), area.height()}, Ink(0));
    PageCanvas canvas(mask, area, size);
    draw_panel_area(canvas.draw(), page, frame, dpi);
    canvas.commit();
    return mask;
}

// --- borders --------------------------------------------------------------------------------------------------------

std::vector<std::vector<PointMM>> dashes(const std::vector<PointMM>& points, double on, double off) {
    std::vector<PointMM> ring(points);
    ring.push_back(points.front());
    std::vector<std::vector<PointMM>> pieces;
    std::vector<PointMM> current{ring.front()};
    double left = on;
    bool drawing = true;
    for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
        const PointMM& a = ring[i];
        const PointMM& b = ring[i + 1];
        const double length = core::py_dist(a.x, a.y, b.x, b.y);
        double t0 = 0.0;
        while (length - t0 > 1e-9) {
            const double step = pmin(left, length - t0);
            const double t1 = t0 + step;
            const PointMM p{a.x + (b.x - a.x) * t1 / length, a.y + (b.y - a.y) * t1 / length};
            if (drawing) current.push_back(p);
            left -= step;
            t0 = t1;
            if (left <= 1e-9) {
                if (drawing && current.size() > 1) pieces.push_back(current);
                drawing = !drawing;
                left = drawing ? on : off;
                current = {p};
            }
        }
    }
    if (drawing && current.size() > 1) pieces.push_back(current);
    return pieces;
}

std::vector<PointMM> rough_outline(const std::vector<PointMM>& points, std::string_view seed, double amount_mm) {
    core::PyRandom rng = core::PyRandom::from_str(seed);
    std::vector<PointMM> ring(points);
    ring.push_back(points.front());
    std::vector<PointMM> out;
    for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
        const PointMM& a = ring[i];
        const PointMM& b = ring[i + 1];
        const double length = core::py_dist(a.x, a.y, b.x, b.y);
        const auto steps = static_cast<std::int64_t>(pmax(1.0, std::trunc(length)));
        const double div = length != 0.0 ? length : 1.0;  // (length or 1)
        const double nx = -(b.y - a.y) / div;
        const double ny = (b.x - a.x) / div;
        const auto n_knots = static_cast<std::size_t>(std::trunc(length / 7)) + 2;
        std::vector<double> knots;
        knots.reserve(n_knots);
        for (std::size_t k = 0; k < n_knots; ++k) knots.push_back(rng.uniform(-amount_mm, amount_mm));
        knots.front() = 0.0;  // (the corners stay where they are)
        knots.back() = 0.0;
        for (std::int64_t k = 0; k < steps; ++k) {
            const double t = static_cast<double>(k) / static_cast<double>(steps);
            const double pos = t * static_cast<double>(knots.size() - 1);
            const std::size_t at = std::min(knots.size() - 2, static_cast<std::size_t>(std::trunc(pos)));
            const double f = (1 - core::py_cos((pos - static_cast<double>(at)) * core::kPi)) / 2;
            const double wobble = knots[at] * (1 - f) + knots[at + 1] * f;
            out.push_back(PointMM{a.x + (b.x - a.x) * t + nx * wobble, a.y + (b.y - a.y) * t + ny * wobble});
        }
    }
    return out;
}

void draw_border(Draw& draw, const std::vector<PointMM>& points, double width_mm, int dpi, const core::Json* style,
                 std::string_view seed) {
    // style = style or {}
    const core::Json* kind_value = get(style, "kind");
    std::string kind = "solid";
    if (kind_value != nullptr && core::py_truthy(*kind_value)) {
        kind = kind_value->is_string() ? kind_value->get<std::string>() : core::py_str(*kind_value);
    }
    std::vector<std::int64_t> rgb_values{20, 20, 20};
    if (truthy(get(style, "rgb"))) rgb_values = json_ints(*get(style, "rgb"));
    if (rgb_values.size() > 3) rgb_values.resize(3);
    const Ink rgb = Ink::tuple(rgb_values);
    const int width_px = std::max(1, mm_to_px(width_mm, dpi));
    const auto ring = [&](const std::vector<PointMM>& pts, int width) {
        std::vector<PointMM> closed(pts);
        closed.push_back(pts.front());
        draw.line(to_px(closed, dpi), rgb, width, Joint::Curve);
    };
    const auto number = [&](std::string_view key, double fallback) {
        const core::Json* value = get(style, key);
        return value != nullptr ? core::py_float(*value) : fallback;
    };
    if (kind == "double") {
        const double gap = number("gap_mm", pmax(0.6, width_mm));
        const int thin = std::max(1, static_cast<int>(std::nearbyint(width_px * 0.6)));
        ring(points, thin);
        ring(offset_outline(points, gap + width_mm * 0.6), thin);
    } else if (kind == "dashed" || kind == "dotted") {
        const double on = number("dash_mm", kind == "dashed" ? 3.0 : 0.01);
        const double off = number("gap_mm", kind == "dashed" ? 1.8 : pmax(1.0, width_mm * 2.2));
        for (const auto& piece : dashes(points, pmax(0.01, on), pmax(0.2, off))) {
            if (kind == "dotted") {
                const auto [x, y] = xy(piece.front().x, piece.front().y, dpi);
                const double r = width_px / 2.0 + 0.5;
                draw.ellipse(BoxF{x - r, y - r, x + r, y + r}, rgb);
            } else {
                draw.line(to_px(piece, dpi), rgb, width_px);
            }
        }
    } else if (kind == "rough") {
        const auto pts = rough_outline(points, seed.empty() ? std::string_view("frame") : seed, number("wobble_mm", 0.35));
        ring(pts, width_px);
    } else {
        ring(points, width_px);
    }
}

namespace {

// render._bleed_border: drawn on its own along the outline taken out past the paper, then kept inside the bleed.
void bleed_border(Image& part, const Box& area, const core::Page& page, Size size, const core::Frame& frame, int dpi,
                  const core::Json* style) {
    const auto points = bleed_outline(page, &frame);
    if (!points) return;
    Image lines = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
    {
        PageCanvas canvas(lines, area, size);
        draw_border(canvas.draw(), *points, frame.border_mm, dpi, style, frame.id);
        canvas.commit();
    }
    Box b = rect_px(page.bleed_rect_mm(), dpi);
    b = Box{std::max(0, b.x0), std::max(0, b.y0), std::min(size.width, b.x1), std::min(size.height, b.y1)};
    if (b.x1 < b.x0) throw core::Error("value", "Coordinate 'right' is less than 'left'");
    if (b.y1 < b.y0) throw core::Error("value", "Coordinate 'lower' is less than 'upper'");
    // base.paste(region.convert("RGB"), (x0, y0), region), for the part of it in the area
    const Box in{std::max(b.x0, area.x0), std::max(b.y0, area.y0), std::min(b.x1, area.x1), std::min(b.y1, area.y1)};
    if (in.x1 <= in.x0 || in.y1 <= in.y0) return;
    const Box local{in.x0 - area.x0, in.y0 - area.y0, in.x1 - area.x0, in.y1 - area.y0};
    const Image region = lines.crop(local);
    part.paste(region.convert(part.mode()), Point{local.x0, local.y0}, &region);
}

}  // namespace

void draw_frames(Image& part, const Box& area, const core::Page& page, Size size, int dpi) {
    const Ink ink{20, 20, 20};
    for (const core::Frame* frame : page.leaf_frames()) {
        const int width_px = std::max(1, mm_to_px(frame->border_mm, dpi));
        if (frame->border_mm <= 0) continue;  // a panel without a border
        const core::Json* style = style_of(*frame);
        const bool has_style = truthy(style);
        const auto shape = bleed_poly(page, frame);
        const bool rounded = core::rounded(*frame);
        if (frame->bleed && (rounded || (has_style && (styled_kind(style) || truthy(get(style, "rgb")))))) {
            bleed_border(part, area, page, size, *frame, dpi, style);
            continue;
        }
        PageCanvas canvas(part, area, size);
        Draw& draw = canvas.draw();
        if (shape && !has_style && !rounded) {
            // a slanted bleed panel: a border on the inner sides only (the sides off the paper are cut)
            for (std::size_t i = 0; i < shape->size(); ++i) {
                const PointMM& a = (*shape)[i];
                const PointMM& b = (*shape)[(i + 1) % shape->size()];
                if (!on_bleed_edge(page, a, b)) {
                    const std::vector<PointD> seg{xy_point(a.x, a.y, dpi), xy_point(b.x, b.y, dpi)};
                    draw.line(seg, ink, width_px);
                }
            }
        } else if (has_poly(*frame) || rounded || (has_style && styled_kind(style))) {
            if (has_style || rounded) {
                draw_border(draw, outline_mm(*frame), frame->border_mm, dpi, style, frame->id);
            } else {
                std::vector<PointMM> pts;
                for (const core::Point& p : *frame->poly) pts.push_back(PointMM{v(p.x), v(p.y)});
                draw.polygon(to_px(pts, dpi), std::nullopt, ink, width_px);
            }
        } else if (has_style && truthy(get(style, "rgb"))) {
            draw_border(draw, outline_mm(*frame), frame->border_mm, dpi, style, frame->id);
        } else if (!frame->bleed) {
            const Box b = rect_px(frame->rect, dpi);
            draw.rectangle(BoxF{static_cast<double>(b.x0), static_cast<double>(b.y0), static_cast<double>(b.x1), static_cast<double>(b.y1)},
                           std::nullopt, ink, std::max(1, width_px));
        } else {
            // a bleed panel has no border on the sides that run off the paper
            const Box b = rect_px(frame->rect, dpi);
            const Edges open = outer_edges(page, *frame);
            const auto side = [&](bool skip, int x0, int y0, int x1, int y1) {
                if (skip) return;
                const std::vector<PointD> seg{{static_cast<double>(x0), static_cast<double>(y0)},
                                              {static_cast<double>(x1), static_cast<double>(y1)}};
                draw.line(seg, ink, std::max(1, width_px));
            };
            side(open.top, b.x0, b.y0, b.x1, b.y0);
            side(open.bottom, b.x0, b.y1, b.x1, b.y1);
            side(open.left, b.x0, b.y0, b.x0, b.y1);
            side(open.right, b.x1, b.y0, b.x1, b.y1);
        }
        canvas.commit();
    }
}

void draw_crop_marks(Draw& draw, const core::Page& page, int dpi) {
    const core::Rect trim = page.trim_rect_mm();
    const core::Rect bleed = page.bleed_rect_mm();
    // min(bleed.x, bleed.y, w - bleed.x - bleed.width, h - bleed.y - bleed.height): the first of the smallest
    const Num candidates[] = {bleed.x, bleed.y, page.spec.width_mm - bleed.x - bleed.width,
                              page.spec.height_mm - bleed.y - bleed.height};
    Num room = candidates[0];
    for (const Num& c : candidates) {
        if (c < room) room = c;
    }
    const Ink ink{0, 0, 0};
    const auto px = [&](double mm) { return static_cast<double>(mm_to_px(mm, dpi)); };
    const auto line = [&](double x0, double y0, double x1, double y1) {
        const std::vector<PointD> seg{{x0, y0}, {x1, y1}};
        draw.line(seg, ink, 1);
    };
    if (room < Num(4)) {
        const double mark = px(5);
        const struct {
            Num x, y;
            int dx, dy;
        } corners[] = {{trim.x, trim.y, -1, -1},
                       {trim.x + trim.width, trim.y, 1, -1},
                       {trim.x, trim.y + trim.height, -1, 1},
                       {trim.x + trim.width, trim.y + trim.height, 1, 1}};
        for (const auto& c : corners) {
            const double x = px(v(c.x));
            const double y = px(v(c.y));
            line(x, y, x + c.dx * mark, y);
            line(x, y, x, y + c.dy * mark);
        }
        return;
    }
    const double gap = 1.0;
    const double length = pmin(10.0, v(room) - 1.5);
    const std::pair<Num, Num> xs[] = {{trim.x, bleed.x}, {trim.x + trim.width, bleed.x + bleed.width}};
    const std::pair<Num, Num> ys[] = {{trim.y, bleed.y}, {trim.y + trim.height, bleed.y + bleed.height}};
    for (int hx = 0; hx < 2; ++hx) {
        for (int vy = 0; vy < 2; ++vy) {
            const double tx = v(xs[hx].first);
            const double bx = v(xs[hx].second);
            const double ty = v(ys[vy].first);
            const double by = v(ys[vy].second);
            const int out_x = hx == 0 ? -1 : 1;
            const int out_y = vy == 0 ? -1 : 1;
            const double x0 = bx + out_x * gap;
            for (const double y : {ty, by}) line(px(x0), px(y), px(x0 + out_x * length), px(y));
            const double y0 = by + out_y * gap;
            for (const double x : {tx, bx}) line(px(x), px(y0), px(x), px(y0 + out_y * length));
        }
    }
    const double cx = v(trim.x + trim.width / Num(2));
    const double cy = v(trim.y + trim.height / Num(2));
    for (const double x : {v(bleed.x) - gap - length, v(bleed.x + bleed.width) + gap}) line(px(x), px(cy), px(x + length), px(cy));
    for (const double y : {v(bleed.y) - gap - length, v(bleed.y + bleed.height) + gap}) line(px(cx), px(y), px(cx), px(y + length));
}

// --- JSON colours -------------------------------------------------------------------------------------------------

std::vector<std::int64_t> json_ints(const core::Json& list) {
    std::vector<std::int64_t> out;
    for (const core::Json& item : core::py_list(list)) out.push_back(core::py_int(item));
    return out;
}

}  // namespace genko::render::detail
