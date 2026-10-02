// Panels on the page: the masks that cut the layers (render._clip_mask, _draw_frame_mask, fill_frame,
// _bleed_shape_mask) and their borders (render._draw_frames, draw_border, _dashes, _rough, _bleed_border,
// _draw_crop_marks). Where a panel reaches is core's (genko/placement.py and frames.py: core/placement.hpp,
// core/frames.hpp).

#include <algorithm>
#include <cmath>

#include "core/error.hpp"
#include "core/frames.hpp"
#include "core/placement.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/pyrandom.hpp"
#include "render/page_internal.hpp"

namespace genko::render::detail {

namespace {

using core::Num;
using core::py_max;
using core::py_min;

double v(const Num& n) { return n.value(); }

bool has_poly(const core::Frame& frame) { return frame.poly && !frame.poly->empty(); }

// the points as Python passes them to frames.offset (floats)
std::vector<core::Point> num_points(const std::vector<PointMM>& pts) {
    std::vector<core::Point> out;
    out.reserve(pts.size());
    for (const PointMM& p : pts) out.push_back(core::Point{Num(p.x), Num(p.y)});
    return out;
}

const core::Json* style_of(const core::Frame& frame) {
    if (!frame.line) return nullptr;
    return &*frame.line;
}

// (style or {}).get(key): the value or nothing
const core::Json* get(const core::Json* style, std::string_view key) { return style != nullptr ? core::get(*style, key) : nullptr; }

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

std::vector<PointMM> mm_points(std::span<const core::Point> points) {
    std::vector<PointMM> out;
    out.reserve(points.size());
    for (const core::Point& p : points) out.push_back(PointMM{v(p.x), v(p.y)});
    return out;
}

std::vector<PointMM> outline_mm(const core::Frame& frame) { return mm_points(core::outline(frame)); }

std::optional<std::vector<PointMM>> bleed_outline_mm(const core::Page& page, const core::Frame* frame,
                                                     std::optional<double> beyond_mm) {
    const auto outline = core::bleed_outline(page, frame, beyond_mm);
    if (!outline) return std::nullopt;
    return mm_points(*outline);
}

std::vector<PointMM> offset_outline(const std::vector<PointMM>& points, double d) {
    return mm_points(core::offset(num_points(points), d));
}

const core::Frame* panel_of(const core::Page& page, double x, double y) {
    for (const core::Frame* frame : page.leaf_frames()) {
        if (!frame->clip) continue;
        const auto shape = core::bleed_poly(page, frame);
        if (shape) {
            if (core::in_poly(*shape, x, y)) return frame;
        } else if (frame->bleed && !has_poly(*frame)) {
            if (core::clip_box(page, frame, "bleed").contains(Num(x), Num(y))) return frame;
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
    const auto outline = bleed_outline_mm(page, &frame, std::nullopt);
    draw.polygon(to_px(*outline, dpi), Ink(255));
    return true;
}

namespace {

void draw_panel_area(Draw& draw, const core::Page& page, const core::Frame& frame, int dpi) {
    const auto shape = core::bleed_poly(page, &frame);
    if (bleed_shape_mask(draw, page, frame, dpi)) return;
    if (shape) {  // a slanted bleed panel: out to the bleed, its slanted sides kept
        draw.polygon(to_px(*shape, dpi), Ink(255));
    } else if (frame.bleed && !has_poly(frame)) {  // a bleed panel runs out to the bleed
        const Box b = rect_px(core::clip_box(page, &frame, "bleed"), dpi);
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
            const double step = py_min(left, length - t0);
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
        const std::int64_t steps = std::max<std::int64_t>(1, core::py_trunc_int(length));
        const double div = length != 0.0 ? length : 1.0;  // (length or 1)
        const double nx = -(b.y - a.y) / div;
        const double ny = (b.x - a.x) / div;
        const auto n_knots = static_cast<std::size_t>(core::py_trunc_int(length / 7)) + 2;
        std::vector<double> knots;
        knots.reserve(n_knots);
        for (std::size_t k = 0; k < n_knots; ++k) knots.push_back(rng.uniform(-amount_mm, amount_mm));
        knots.front() = 0.0;  // (the corners stay where they are)
        knots.back() = 0.0;
        for (std::int64_t k = 0; k < steps; ++k) {
            const double t = static_cast<double>(k) / static_cast<double>(steps);
            const double pos = t * static_cast<double>(knots.size() - 1);
            const std::size_t at = std::min(knots.size() - 2, static_cast<std::size_t>(core::py_trunc_int(pos)));
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
    if (truthy(get(style, "rgb"))) rgb_values = core::int_tuple(*get(style, "rgb"));
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
        const double gap = number("gap_mm", py_max(0.6, width_mm));
        const int thin = std::max(1, static_cast<int>(core::py_round_int(width_px * 0.6)));
        ring(points, thin);
        ring(offset_outline(points, gap + width_mm * 0.6), thin);
    } else if (kind == "dashed" || kind == "dotted") {
        const double on = number("dash_mm", kind == "dashed" ? 3.0 : 0.01);
        const double off = number("gap_mm", kind == "dashed" ? 1.8 : py_max(1.0, width_mm * 2.2));
        for (const auto& piece : dashes(points, py_max(0.01, on), py_max(0.2, off))) {
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
    const auto points = bleed_outline_mm(page, &frame);
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
        const auto shape = core::bleed_poly(page, frame);
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
                if (!core::on_bleed_edge(page, a, b)) {
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
            const core::OuterEdges open = core::outer_edges(page, *frame);
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
    const double length = py_min(10.0, v(room) - 1.5);
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

}  // namespace genko::render::detail
