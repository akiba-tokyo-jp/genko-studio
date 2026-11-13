// The balloons (fukidashi@1, SPEC BALLOON-01): tails_of, the shapes of a group (each line's balloon, its tails, the
// joins) as outlines of real pixels (render/text/balloon_clean), their fill and line laid on the page in one mix, a
// fading tail, the balloon eraser's cuts, a turned balloon drawn upright and laid turned, a picture balloon, the
// thought's trail, draw_group and draw_lines. (The letters, _paint_text and what it calls: lettering.cpp.)
//
// Every balloon is drawn clean: a true ellipse, an even line, tails that run out of the outline. The hand-drawn look
// (the egg-shaped oval, the pen's swelling line, the wobble) is gone since 2026-11-13, in books of before too.

#include "render/text/balloons.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/pyrandom.hpp"
#include "core/ruby.hpp"
#include "core/type_text.hpp"
#include "render/draw.hpp"
#include "render/libm_float.hpp"
#include "render/page.hpp"
#include "render/text/balloon_clean.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"
#include "render/text/memo.hpp"

namespace genko::render::text {

using core::Json;
using balloon::Pt;

namespace {

const std::vector<std::int64_t> kOutline{20, 20, 20};   // OUTLINE
const std::vector<std::int64_t> kPaper{255, 255, 255};  // PAPER

bool one_of(std::string_view kind, std::initializer_list<std::string_view> kinds) {
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}
// SQUARE, (*SQUARE, "rounded"), NO_TAIL, UNEVEN
bool square(std::string_view kind) { return one_of(kind, {"box", "narration", "dotted_box", "tone_box", "fancy_box"}); }
bool no_tail(std::string_view kind) {
    return one_of(kind, {"narration", "sfx", "none", "flash", "picture", "dotted_box", "tone_box", "fancy_box"});
}

// st[key] / st.get(key): the value, or null when it is not there
const Json& at(const Json& st, std::string_view key) {
    static const Json none;
    if (!st.is_object()) return none;
    const auto it = st.find(key);
    return it == st.end() ? none : *it;
}
// float(value or fallback)
double float_or(const Json& value, double fallback) { return core::py_truthy(value) ? core::to_float(value) : fallback; }
// int(value or 0)
std::int64_t int_or_zero(const Json& value) { return core::py_truthy(value) ? core::to_int(value) : 0; }
// line.balloon or "speech"
std::string kind_of(const core::StoryLine& line) { return line.balloon.empty() ? std::string("speech") : line.balloon; }
// line.w_mm or 40
double mm_or(const core::Num& value, double fallback) { return value.truthy() ? value.value() : fallback; }
// a number of the book's data in arithmetic (a tail's end, a cut's point)
double number(const Json& value) { return core::to_real(value); }
// math.dist(a, b) for a point of the data: both of two dimensions
double dist_to(Pt a, const Json& b) {
    if (core::length(b) != 2) throw core::PyValueError("both points must have the same number of dimensions");
    return core::py_dist(a[0], a[1], number(core::subscript(b, 0)), number(core::subscript(b, 1)));
}

Box page_box(const PagePart& part) {
    return Box{part.origin.x, part.origin.y, part.origin.x + part.image->width(), part.origin.y + part.image->height()};
}
bool overlaps(const Box& a, const Box& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }

// The line's box in pixels: (px(x), px(y), px(x + w), px(y + h)).
std::array<std::int64_t, 4> box_px(const core::StoryLine& ln, int dpi) {
    const double x = ln.x_mm.value();
    const double y = ln.y_mm.value();
    return {px(x, dpi), px(y, dpi), px(x + mm_or(ln.w_mm, 40), dpi), px(y + mm_or(ln.h_mm, 20), dpi)};
}

// --- the shapes -----------------------------------------------------------------------------------------------------

using clean::P;

double clamp01(double v) { return v < 0 ? 0.0 : (v > 1 ? 1.0 : v); }

// A plane of coverage as an "L" picture (0..255, rounded to the nearest).
Image as_image(const clean::Plane& plane) {
    std::string bytes(plane.values.size(), '\0');
    for (std::size_t i = 0; i < plane.values.size(); ++i) {
        bytes[i] = static_cast<char>(static_cast<std::uint8_t>(clamp01(static_cast<double>(plane.values[i])) * 255.0 + 0.5));
    }
    return Image::frombytes("L", Size{plane.width, plane.height}, bytes);
}

// A style's length in mm as pixels (not rounded), or `fallback` when it is not set.
double mm_px(const Json& value, int dpi, double fallback) {
    return core::py_truthy(value) ? core::to_float(value) / 25.4 * dpi : fallback;
}

// How far a balloon's outline trembles (px): style.wave_mm, or — a book of before — its wobble as a wave of
// kWobbleWave × wobble × the box's narrow side (the hand-drawn wobble is gone: SPEC BALLOON-01).
double wave_px(const Json& st, const core::StoryLine& line, int dpi) {
    double mm = 0.0;
    if (core::py_truthy(at(st, "wave_mm"))) {
        mm = core::to_float(at(st, "wave_mm"));
    } else if (const double wobble = float_or(at(st, "wobble"), 0); wobble > 0) {
        mm = balloon::kWobbleWave * core::py_min(1.0, wobble) * core::py_min(mm_or(line.w_mm, 40), mm_or(line.h_mm, 20));
    }
    return mm > 0 ? mm / 25.4 * dpi : 0.0;
}

// A line's balloon: its outline, the outline its tails leave it from (a burst's valleys, a cloud's), and whether a
// tail's join with it is rounded.
struct Body {
    clean::Contour outline;
    clean::Contour guide;
    P centre;
    double least = 0.0;  // (its narrow half size)
    bool smooth = false;
    std::string kind;
};

Body body_of(const core::StoryLine& line, const Json& lst, const clean::Box& box, const Box& region, int dpi) {
    Body body;
    body.kind = kind_of(line);
    body.centre = {box.cx(), box.cy()};
    body.least = core::py_min(box.rx(), box.ry());
    const std::string& kind = body.kind;
    if (line.path && !line.path->empty()) {  // drawn by hand
        clean::Contour outline;
        for (const core::Point& p : *line.path) {
            outline.push_back({static_cast<double>(px(p.x.value(), dpi) - region.x0), static_cast<double>(px(p.y.value(), dpi) - region.y0)});
        }
        if (core::py_truthy(at(lst, "path_curve"))) outline = clean::smooth_closed(outline);
        body.outline = outline;
        body.guide = std::move(outline);
        body.smooth = true;
        return body;
    }
    const double corner = mm_px(at(lst, "corner_mm"), dpi, -1.0);
    if (kind == "rounded") {
        body.outline = core::py_truthy(at(lst, "squircle")) ? clean::squircle(box) : clean::rounded(box, corner >= 0 ? corner : body.least * 0.6);
        body.smooth = true;
    } else if (square(kind)) {
        body.outline = clean::rounded(box, corner >= 0 ? corner : 0.0);
        body.smooth = true;
    } else if (kind == "capsule") {
        body.outline = clean::rounded(box, body.least);
        body.smooth = true;
    } else if (kind == "polygon") {
        const std::int64_t sides = int_or_zero(at(lst, "sides"));
        body.outline = clean::polygon(box, sides == 0 ? 8 : static_cast<int>(sides), corner >= 0 ? corner : body.least * 0.14);
        body.smooth = true;
    } else if (kind == "cloud") {
        const double rx = box.rx(), ry = box.ry();
        const double bump = core::py_max(2.0, body.least * 0.3);
        const double keep = 1 - bump / core::py_max(1.0, body.least);
        std::int64_t n = int_or_zero(at(lst, "bumps"));
        if (n == 0) n = std::max<std::int64_t>(8, core::py_trunc_int(core::kPi * (rx + ry) * keep / (bump * 1.4)));
        body.outline = clean::cloud(box, static_cast<int>(n));
        body.guide = clean::scaled_about(clean::ellipse(box), body.centre, 0.8);
    } else if (kind == "shout") {
        const double rx = box.rx(), ry = box.ry();
        std::int64_t spikes = int_or_zero(at(lst, "spikes"));
        if (spikes == 0) spikes = std::max<std::int64_t>(12, core::py_trunc_int((rx + ry) / core::py_max(4.0, body.least / 3)));
        const double depth = core::py_max(0.05, core::py_min(0.6, float_or(at(lst, "spike_depth"), 0.2)));
        const double amount = core::py_max(0.0, core::py_min(1.0, float_or(at(lst, "spike_jitter"), 0)));
        const double alt = core::py_max(0.0, core::py_min(0.8, float_or(at(lst, "spike_alt"), 0)));
        std::vector<double> jitter;
        if (amount > 0) {  // (each point's own reach, the same for the same line)
            core::PyRandom rng = core::PyRandom::from_str(line.id.empty() ? std::string_view("shout") : std::string_view(line.id));
            const std::int64_t count = alt > 0 && spikes % 2 != 0 ? spikes + 1 : spikes;
            for (std::int64_t i = 0; i < count; ++i) jitter.push_back(rng.uniform(-0.5, 0.5) * depth * 2 * amount);
        }
        body.outline = clean::burst(box, static_cast<int>(spikes), depth, alt, jitter);
        body.guide = clean::scaled_about(clean::ellipse(box), body.centre, (1 - depth) * 0.97);
    } else if (kind == "electric") {
        const double rx = box.rx(), ry = box.ry();
        std::int64_t teeth = int_or_zero(at(lst, "spikes"));
        if (teeth == 0) teeth = std::max<std::int64_t>(14, core::py_trunc_int((rx + ry) / core::py_max(3.0, body.least / 4)));
        const double depth = core::py_max(0.04, core::py_min(0.4, float_or(at(lst, "spike_depth"), 0.12)));
        body.outline = clean::sawtooth(box, static_cast<int>(teeth), depth);
        body.guide = clean::scaled_about(clean::squircle(box), body.centre, (1 - depth) * 0.97);
    } else {  // speech, thought, whisper, flash
        body.outline = clean::ellipse(box);
        body.smooth = true;
    }
    if (body.guide.empty()) body.guide = body.outline;
    if (body.smooth && kind != "flash") {
        const double wave = wave_px(lst, line, dpi);
        if (wave > 0) {  // (a trembling outline: its tails leave the outline it trembles about, with a corner)
            body.outline = clean::waved(body.outline, wave, core::py_max(wave * 7, 2.4 / 25.4 * dpi));
            body.smooth = false;
        }
    }
    return body;
}

// _fade_mask(size, fades): 1 everywhere but near each fading tail's tip, falling to 0 at the tip.
std::vector<float> fade_keep(Size size, const std::vector<std::pair<clean::Box, P>>& fades) {
    const auto w = static_cast<std::size_t>(size.width);
    const auto h = static_cast<std::size_t>(size.height);
    std::vector<float> keep(w * h, 1.0f);
    for (const auto& [box, tip] : fades) {
        const double reach =
            core::py_max(1.0, clean::distance({box.cx(), box.cy()}, tip) - core::py_min(box.x1 - box.x0, box.y1 - box.y0) / 2) * 0.7;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const double dx = static_cast<double>(x) + 0.5 - tip.x;
                const double dy = static_cast<double>(y) + 0.5 - tip.y;
                const auto v = static_cast<float>(clamp01(std::sqrt(dx * dx + dy * dy) / reach));
                float& k = keep[y * w + x];
                if (v < k) k = v;
            }
        }
    }
    return keep;
}

// _dotted_edge(boxes, width): round dots along each box's edge, about three line-widths apart.
std::vector<clean::Contour> dotted_edge(const std::vector<clean::Box>& boxes, double width) {
    std::vector<clean::Contour> dots;
    const double r = core::py_max(1.0, width * 0.9);
    const double step = core::py_max(3.0, width * 3.2);
    for (const clean::Box& b : boxes) {
        const double x0 = b.x0 + r, y0 = b.y0 + r, x1 = b.x1 - r, y1 = b.y1 - r;
        const P edges[4][2] = {{{x0, y0}, {x1, y0}}, {{x1, y0}, {x1, y1}}, {{x1, y1}, {x0, y1}}, {{x0, y1}, {x0, y0}}};
        for (const auto& edge : edges) {
            const double length = clean::distance(edge[0], edge[1]);
            const std::int64_t n = std::max<std::int64_t>(1, core::py_round_int(length / step));
            for (std::int64_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(n);
                dots.push_back(clean::circle({edge[0].x + (edge[1].x - edge[0].x) * t, edge[0].y + (edge[1].y - edge[0].y) * t}, r));
            }
        }
    }
    return dots;
}

// _corner_marks(boxes, width): a small filled diamond on each corner of each box (a 飾り枠's corners).
std::vector<clean::Contour> corner_marks(const std::vector<clean::Box>& boxes, double width) {
    std::vector<clean::Contour> marks;
    const double d = width * 3.5;
    for (const clean::Box& b : boxes) {
        for (const P c : {P{b.x0, b.y0}, P{b.x1, b.y0}, P{b.x1, b.y1}, P{b.x0, b.y1}}) {
            marks.push_back({{c.x, c.y - d}, {c.x + d, c.y}, {c.x, c.y + d}, {c.x - d, c.y}});
        }
    }
    return marks;
}

// _tone_dots(size, dpi, origin): a light dot tone (about 10 %), dots a millimetre apart, lined up with the page.
std::vector<clean::Contour> tone_dots(Size size, int dpi, std::int64_t ox, std::int64_t oy) {
    std::vector<clean::Contour> dots;
    const double pitch = core::py_max(3.0, dpi / 25.4 * 1.0);
    const double r = core::py_max(0.5, pitch * 0.17);
    double y = -core::py_fmod(static_cast<double>(oy), pitch);
    std::int64_t row = 0;
    while (y < size.height + pitch) {
        double x = -core::py_fmod(static_cast<double>(ox), pitch) + (row % 2 != 0 ? pitch / 2 : 0);
        while (x < size.width + pitch) {
            dots.push_back(clean::circle({x, y}, r));
            x += pitch;
        }
        y += pitch;
        ++row;
    }
    return dots;
}

// _cuts_mask(lines, size, origin, dpi, scale): where the balloon eraser went (each line's style.cuts, mm from its
// box's corner), or nothing.
std::optional<Image> cuts_mask(const std::vector<const core::StoryLine*>& lines, Size size, std::int64_t ox, std::int64_t oy,
                               int dpi, std::int64_t scale) {
    std::vector<std::pair<const core::StoryLine*, Json>> strokes;
    for (const core::StoryLine* ln : lines) {
        const Json st = style_of(*ln);
        const Json& cuts = at(st, "cuts");
        if (!core::py_truthy(cuts)) continue;
        for (const Json& c : core::iterate(cuts)) strokes.emplace_back(ln, c);
    }
    if (strokes.empty()) return std::nullopt;
    Image mask = Image::create("L", size, 0);
    Draw draw(mask);
    for (const auto& [ln, cut] : strokes) {
        std::vector<PointD> pts;
        const Json* points = core::dict_get(cut, "points");
        if (points != nullptr && core::py_truthy(*points)) {
            for (const Json& p : core::iterate(*points)) {
                const double x = static_cast<double>((px(ln->x_mm.value() + number(core::subscript(p, 0)), dpi) - ox) * scale);
                const double y = static_cast<double>((px(ln->y_mm.value() + number(core::subscript(p, 1)), dpi) - oy) * scale);
                pts.push_back(PointD{x, y});
            }
        }
        const double width_mm = core::to_float(core::py_get(cut, "width_mm", Json(2.0)));
        const std::int64_t width = std::max<std::int64_t>(1, px(width_mm, dpi) * scale);
        if (pts.size() == 1) pts.push_back(pts.front());
        // (no points: Python's line() and pts[0] raise IndexError)
        if (pts.empty()) throw core::PyUncaught("IndexError", "list index out of range");
        draw.line(pts, Ink(255), static_cast<int>(width), Joint::Curve);
        for (const PointD& p : {pts.front(), pts.back()}) {
            const double half = static_cast<double>(width) / 2;
            draw.ellipse(BoxF{p.x - half, p.y - half, p.x + half, p.y + half}, Ink(255));
        }
    }
    return mask;
}

// _flash_lines(boxes, width): spokes round each balloon, from just inside its edge to outside it, each tapering to a
// point at its inner end; long and short by turns, at equal distances round the edge.
std::vector<clean::Contour> flash_spokes(const std::vector<clean::Box>& boxes, double width, int dpi) {
    std::vector<clean::Contour> spokes;
    for (const clean::Box& box : boxes) {
        const clean::Contour edge = clean::ellipse(box);
        const std::vector<double> lengths = clean::lengths_of(edge);
        const double total = lengths.back();
        const double pitch = core::py_max(width * 2.2, 0.7 / 25.4 * dpi);
        std::int64_t n = std::max<std::int64_t>(24, core::py_round_int(total / pitch));
        if (n % 2 != 0) ++n;
        const P centre{box.cx(), box.cy()};
        const double half = core::py_max(0.5, width * 0.55);
        for (std::int64_t i = 0; i < n; ++i) {
            const double s = total * static_cast<double>(i) / static_cast<double>(n);
            const P on = clean::point_at(edge, lengths, s);
            const P along = clean::direction_at(edge, lengths, s);
            const double inner = i % 2 == 0 ? 0.90 : 0.96;
            const double outer = i % 2 == 0 ? 1.18 : 1.12;
            const P tip{centre.x + (on.x - centre.x) * inner, centre.y + (on.y - centre.y) * inner};
            const P end{centre.x + (on.x - centre.x) * outer, centre.y + (on.y - centre.y) * outer};
            spokes.push_back({tip, {end.x + along.x * half, end.y + along.y * half}, {end.x - along.x * half, end.y - along.y * half}});
        }
    }
    return spokes;
}

// --- one group's shapes (_paint_shapes) ------------------------------------------------------------------------------

using Tails = std::vector<std::pair<const core::StoryLine*, Json>>;

// What _paint_shapes lays on the page over its region: where the group lies (its shapes' union, the cuts and a fading
// tail taken off), its line, a tone_box's dots; the fill's colour and how much of it covers, the line's colour. Each
// mask is the region's size.
struct Masks {
    bool fill = false;
    double opacity = 1.0;
    std::vector<std::int64_t> paper;
    Image cover;
    Image tone;  // (empty unless tone_box)
    std::vector<std::int64_t> line_rgb;
    Image band;
};

std::vector<std::int64_t> colour_of(const Json& value, const std::vector<std::int64_t>& fallback) {
    // tuple(int(v) for v in value or fallback)[:3]
    std::vector<std::int64_t> out = core::py_truthy(value) ? core::int_tuple(value) : fallback;
    if (out.size() > 3) out.resize(3);
    return out;
}

// The line's width in pixels: style.border_mm (0.35 mm when not set; 0: no line), a whole number of pixels from 1.5 up
// (a box's edge stays sharp), never thinner than one.
double line_width(const Json& st, int dpi) {
    const Json& border = at(st, "border_mm");
    const double mm = border.is_null() ? 0.35 : core::to_float(border);
    if (!(mm > 0)) return 0.0;
    const double width = mm / 25.4 * dpi;
    return width >= 1.5 ? std::floor(width + 0.5) : 1.0;
}

Masks shape_masks(const std::vector<const core::StoryLine*>& lines, const std::vector<std::array<std::int64_t, 4>>& boxes,
                  const Tails& tails, const Box& region, int dpi, const Json& st) {
    const std::int64_t rx0 = region.x0, ry0 = region.y0;
    const Size size{region.width(), region.height()};
    const double width = line_width(st, dpi);
    const std::string kind = kind_of(*lines.front());
    const auto local = [&](const std::array<std::int64_t, 4>& b) {
        return clean::Box{static_cast<double>(b[0] - rx0), static_cast<double>(b[1] - ry0), static_cast<double>(b[2] - rx0),
                          static_cast<double>(b[3] - ry0)};
    };
    const auto at_px = [&](const Json& point) {
        return P{static_cast<double>(px(number(core::subscript(point, 0)), dpi) - rx0),
                 static_cast<double>(px(number(core::subscript(point, 1)), dpi) - ry0)};
    };
    // the group's shapes: each line's balloon, then their tails, the joins and the bubbles
    std::vector<clean::Shape> shapes;
    std::vector<Body> bodies;
    std::vector<clean::Box> locals;
    std::vector<std::pair<clean::Box, P>> fades;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        locals.push_back(local(boxes[i]));
        bodies.push_back(body_of(*lines[i], style_of(*lines[i]), locals.back(), region, dpi));
        shapes.push_back(clean::whole(bodies.back().outline));
    }
    for (std::size_t i = 0; i + 1 < bodies.size(); ++i) {  // (joined balloons: their notches rounded, or a neck between them)
        const Body& a = bodies[i];
        const Body& b = bodies[i + 1];
        if (!a.smooth || !b.smooth) continue;
        const double least = core::py_min(a.least, b.least);
        const double reach = core::py_max(width * 2, least * 0.22);
        std::vector<clean::Shape> joins = clean::joins_between(a.outline, b.outline, reach);
        if (joins.empty() && !clean::inside(a.outline, b.centre) && !clean::inside(b.outline, a.centre)) {
            const std::vector<double> la = clean::lengths_of(a.outline);
            const std::vector<double> lb = clean::lengths_of(b.outline);
            const P from = clean::point_at(a.outline, la, clean::facing(a.outline, la, a.centre, b.centre));
            const P to = clean::point_at(b.outline, lb, clean::facing(b.outline, lb, b.centre, a.centre));
            if (clean::distance(from, to) <= least * 1.2) {
                const clean::Shape between = clean::neck(a.centre, b.centre, least * 0.7);
                for (const Body* body : {&a, &b}) {
                    for (clean::Shape& join : clean::joins_between(between.points, body->outline, reach)) joins.push_back(std::move(join));
                }
                shapes.push_back(between);
            }
        }
        for (clean::Shape& join : joins) shapes.push_back(std::move(join));
    }
    for (const auto& [line, tail] : tails) {
        const std::size_t index = static_cast<std::size_t>(std::find(lines.begin(), lines.end(), line) - lines.begin());
        const clean::Box& box = locals[index];
        const Body& body = bodies[index];
        const P tip = at_px(core::subscript(tail, "to"));
        const double shortest = core::py_min(box.x1 - box.x0, box.y1 - box.y0);
        const Json* kind_value = core::dict_get(tail, "kind");
        const std::string tail_style = core::py_str(kind_value != nullptr && core::py_truthy(*kind_value) ? *kind_value : Json("wedge"));
        // (a letterer's tail is broad where it leaves the balloon: about a third of its narrow side)
        const Json* width_value = core::dict_get(tail, "width_mm");
        const double base = width_value != nullptr && core::py_truthy(*width_value)
                                ? static_cast<double>(px(core::to_float(core::subscript(tail, "width_mm")), dpi))
                                : core::py_max(4.0, shortest / (tail_style == "wedge" ? 3.0 : 4.0));
        const Json* vias = core::dict_get(tail, "vias");
        const bool bent = vias != nullptr && core::py_truthy(*vias);
        if (!bent && (body.kind == "thought" || tail_style == "bubbles")) {
            // bubbles toward the speaker instead of a tail
            for (clean::Contour& bubble : clean::bubble_trail(body.guide, body.centre, tip, core::py_max(2.0, body.least * 0.2))) {
                shapes.push_back(clean::whole(std::move(bubble)));
            }
            continue;
        }
        clean::TailSpec spec;
        spec.tip = tip;
        spec.base = base;
        spec.kind = tail_style == "inward" ? std::string_view("inward")
                    : tail_style == "straight" ? std::string_view("straight")
                    : tail_style == "zigzag" ? std::string_view("zigzag")
                    : tail_style == "fade" ? std::string_view("fade")
                                           : std::string_view("wedge");
        spec.depth = core::py_max(base * 0.5, width * 3 + 2);
        if (bent) {  // 折れ線のしっぽ
            for (const Json& v : core::iterate(*vias)) spec.bends.push_back(at_px(v));
        } else {
            const Json* via_value = core::dict_get(tail, "via");
            if (via_value != nullptr && core::py_truthy(*via_value)) {
                spec.has_via = true;
                spec.via = at_px(*via_value);
            }
            // (an electric balloon's tail: a lightning bolt)
            spec.bolt = body.kind == "electric" && !spec.has_via && spec.kind != "inward";
        }
        if (body.smooth) {  // (the tail runs out of the outline: no corner where they meet)
            const Json* fillet = core::dict_get(tail, "fillet_mm");
            spec.fillet = fillet != nullptr && !fillet->is_null() ? core::py_max(0.0, core::to_float(*fillet)) / 25.4 * dpi
                                                                   : core::py_max(width * 2, core::py_min(base * 0.3, shortest * 0.12));
        }
        clean::Tail made = clean::tail(body.guide, body.centre, spec);
        if (spec.kind == "inward") made.shape.owner = static_cast<int>(index);
        shapes.push_back(std::move(made.shape));
        for (clean::Shape& join : made.joins) shapes.push_back(std::move(join));
        if (tail_style == "fade") fades.emplace_back(box, tip);
    }

    // their coverage: the fill is their union; the line, what lies within its width of the union's edge
    const clean::Plane fill = clean::fill_plane(shapes, size.width, size.height);
    const std::size_t count = fill.values.size();
    const bool dashes = kind == "whisper" || core::py_truthy(at(st, "dash_mm"));
    std::vector<clean::Run> runs;
    if (dashes) {  // (dashes of one length all the way round, none cut short where the outline closes)
        const double on = core::py_max(width, mm_px(at(st, "dash_mm"), dpi, 1.1 / 25.4 * dpi));
        runs = clean::edge_runs(clean::dashed(shapes, on, core::py_max(width * 1.2, on * 0.6)));
    } else {
        runs = clean::edge_runs(shapes);
    }
    const auto within = [&](double reach) {
        clean::Plane plane = clean::ribbon_plane(runs, reach, size.width, size.height, !dashes);
        for (std::size_t i = 0; i < count; ++i) plane.values[i] *= fill.values[i];
        return plane;
    };
    std::vector<float> band(count, 0.0f);
    std::vector<float> inner = fill.values;  // (the fill inside the line: kept solid under a fading tail)
    if (width > 0) {
        band = within(width).values;
        for (std::size_t i = 0; i < count; ++i) inner[i] = fill.values[i] - band[i];
    }
    const auto lighter = [&](const std::vector<float>& more) {
        for (std::size_t i = 0; i < count; ++i) band[i] = std::max(band[i], more[i]);
    };
    if (width > 0 && (core::py_truthy(at(st, "double")) || kind == "fancy_box")) {
        // a second line inside the first: two widths in, one width wide
        const clean::Plane far = within(width * 4);
        const clean::Plane close = within(width * 3);
        std::vector<float> second(count);
        for (std::size_t i = 0; i < count; ++i) second[i] = std::max(0.0f, far.values[i] - close.values[i]);
        lighter(second);
    }
    std::vector<float> cover = fill.values;
    if (!fades.empty()) {  // 消えるしっぽ: its outline and fill thin away toward the tip
        const std::vector<float> keep = fade_keep(size, fades);
        for (std::size_t i = 0; i < count; ++i) {
            band[i] *= keep[i];
            cover[i] = std::max(cover[i] * keep[i], inner[i]);
        }
    }
    if (width > 0 && kind == "dotted_box") band = clean::union_plane(dotted_edge(locals, core::py_max(2.0, width)), size.width, size.height).values;
    if (width > 0 && kind == "fancy_box") lighter(clean::union_plane(corner_marks(locals, core::py_max(2.0, width)), size.width, size.height).values);
    if (width > 0 && kind == "flash") band = clean::union_plane(flash_spokes(locals, width, dpi), size.width, size.height).values;
    if (const std::int64_t scale = dpi < 300 ? 2 : 1;
        const auto cut = cuts_mask(lines, Size{size.width * static_cast<int>(scale), size.height * static_cast<int>(scale)}, rx0, ry0, dpi, scale)) {
        // フキダシ消しゴム: those parts of the balloon (its fill and its line) are gone
        const std::string gone = (scale > 1 ? cut->reduce(static_cast<int>(scale), static_cast<int>(scale)) : *cut).tobytes();
        for (std::size_t i = 0; i < count; ++i) {
            const float keep = 1.0f - static_cast<float>(static_cast<unsigned char>(gone[i])) / 255.0f;
            cover[i] *= keep;
            band[i] *= keep;
        }
    }
    Masks out;
    out.fill = at(st, "fill") != Json("none");
    if (out.fill) {
        out.paper = colour_of(at(st, "fill_rgb"), kPaper);
        const Json& covering = at(st, "fill_opacity");
        if (!covering.is_null() && core::to_float(covering) < 1) out.opacity = core::py_max(0.0, core::to_float(covering));
    }
    if (kind == "tone_box") {  // (a light dot tone laid inside, under the words)
        const clean::Plane edge = clean::ribbon_plane(clean::edge_runs(shapes), core::py_max(2.0, static_cast<double>(px(1.0, dpi))), size.width, size.height);
        clean::Plane dots = clean::union_plane(tone_dots(size, dpi, rx0, ry0), size.width, size.height);
        for (std::size_t i = 0; i < count; ++i) dots.values[i] *= cover[i] * (1.0f - edge.values[i]);
        out.tone = as_image(dots);
    }
    out.line_rgb = colour_of(at(st, "line_rgb"), kOutline);
    clean::Plane plane;
    plane.width = size.width;
    plane.height = size.height;
    plane.values = std::move(cover);
    out.cover = as_image(plane);
    plane.values = std::move(band);
    out.band = as_image(plane);
    return out;
}

// The key of a group's shapes: everything _paint_shapes reads of its lines, their tails and its region.
std::optional<std::string> masks_key(const std::vector<const core::StoryLine*>& lines, const Tails& tails, const Box& region, int dpi) {
    Json items = Json::array();
    for (const core::StoryLine* ln : lines) {
        Json path;
        if (ln->path) {
            path = Json::array();
            for (const core::Point& p : *ln->path) path.push_back(Json::array({p.x.json(), p.y.json()}));
        }
        items.push_back(Json::array({ln->id, ln->balloon, ln->x_mm.json(), ln->y_mm.json(), ln->w_mm.json(), ln->h_mm.json(), path,
                                     detail::keyed_style(ln->style)}));
    }
    Json tail_items = Json::array();
    for (const auto& [line, tail] : tails) {
        tail_items.push_back(Json::array({std::find(lines.begin(), lines.end(), line) - lines.begin(), tail}));
    }
    return detail::memo_key(Json::array({"masks", dpi, region.x0, region.y0, region.x1, region.y1, items, tail_items}));
}

detail::Memo<Masks>& remembered_masks() {
    static detail::Memo<Masks> memo(256, 96LL * 1024 * 1024);
    return memo;
}

// The group laid over a part of the page: each pixel the page's own (through what the group does not cover), the
// fill's colour (inside the line) and the line's, by their shares of the pixel — one mix, so an edge takes no colour
// from a second pass. On a picture with transparency the colours are laid over what is there (its own colour kept
// where nothing was: no dark edge on a balloon's own layer).
void composite(Image& image, const Box& to, const Masks& masks, const Box& from) {
    const std::string mode(image.mode());
    const int w = to.width();
    const int h = to.height();
    if (w <= 0 || h <= 0) return;
    const bool all = from == Box{0, 0, masks.cover.width(), masks.cover.height()};
    const Image cover_part = all ? masks.cover : masks.cover.crop(from);
    const Image band_part = all ? masks.band : masks.band.crop(from);
    if (mode != "RGB" && mode != "RGBA") {
        // (any other kind of picture: the fill through what it covers inside the line, then the line)
        if (masks.fill) {
            const double k = masks.opacity;
            const Image inside = chops::subtract(cover_part, band_part).point([k](int v) { return static_cast<int>(static_cast<double>(v) * k + 0.5); });
            image.paste(Ink::tuple(masks.paper), to, &inside);
        }
        if (!masks.tone.empty()) {
            const Image tone = all ? masks.tone : masks.tone.crop(from);
            image.paste(Ink::tuple(masks.line_rgb), to, &tone);
        }
        image.paste(Ink::tuple(masks.line_rgb), to, &band_part);
        return;
    }
    const std::string cover = cover_part.tobytes();
    const std::string band = band_part.tobytes();
    const std::string tone = masks.tone.empty() ? std::string() : (all ? masks.tone : masks.tone.crop(from)).tobytes();
    const std::size_t bands = mode == "RGBA" ? 4 : 3;
    std::string pixels = image.crop(to).tobytes();
    double paper[3] = {255.0, 255.0, 255.0};
    double line[3] = {0.0, 0.0, 0.0};
    for (std::size_t c = 0; c < 3; ++c) {
        if (c < masks.paper.size()) paper[c] = static_cast<double>(masks.paper[c]);
        if (c < masks.line_rgb.size()) line[c] = static_cast<double>(masks.line_rgb[c]);
    }
    const auto count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < count; ++i) {
        const double b = static_cast<double>(static_cast<unsigned char>(band[i])) / 255.0;
        const double c = static_cast<double>(static_cast<unsigned char>(cover[i])) / 255.0;
        if (b <= 0 && c <= 0) continue;
        const double inside = c > b ? c - b : 0.0;
        const double t = tone.empty() ? 0.0 : std::min(inside, static_cast<double>(static_cast<unsigned char>(tone[i])) / 255.0);
        const double w_line = b + t;
        const double w_paper = masks.fill ? (inside - t) * masks.opacity : 0.0;
        const double w_under = 1.0 - w_line - w_paper;
        char* p = &pixels[i * bands];
        if (bands == 3) {
            for (std::size_t k = 0; k < 3; ++k) {
                const double v = static_cast<double>(static_cast<unsigned char>(p[k])) * w_under + paper[k] * w_paper + line[k] * w_line;
                p[k] = static_cast<char>(static_cast<std::uint8_t>(clamp01(v / 255.0) * 255.0 + 0.5));
            }
        } else {
            const double under = static_cast<double>(static_cast<unsigned char>(p[3])) / 255.0 * w_under;
            const double alpha = under + w_line + w_paper;
            if (alpha <= 0) continue;
            for (std::size_t k = 0; k < 3; ++k) {
                const double v = (static_cast<double>(static_cast<unsigned char>(p[k])) * under + paper[k] * w_paper + line[k] * w_line) / alpha;
                p[k] = static_cast<char>(static_cast<std::uint8_t>(clamp01(v / 255.0) * 255.0 + 0.5));
            }
            p[3] = static_cast<char>(static_cast<std::uint8_t>(clamp01(alpha) * 255.0 + 0.5));
        }
    }
    image.paste(Image::frombytes(mode, Size{w, h}, pixels), Point{to.x0, to.y0});
}

// _paint_shapes(image, lines, boxes, tails, region, dpi, st): the shapes worked out over the region, painted where it
// meets the part of the page.
void paint_shapes(const PagePart& part, const std::vector<const core::StoryLine*>& lines,
                  const std::vector<std::array<std::int64_t, 4>>& boxes, const Tails& tails, const Box& region, int dpi,
                  const Json& st) {
    const Box here = page_box(part);
    if (!overlaps(region, here)) return;
    std::shared_ptr<const Masks> masks;
    const std::optional<std::string> key = masks_key(lines, tails, region, dpi);
    if (key) masks = remembered_masks().get(*key);
    if (!masks) {
        masks = std::make_shared<const Masks>(shape_masks(lines, boxes, tails, region, dpi, st));
        if (key) {
            const std::int64_t pixels = static_cast<std::int64_t>(region.width()) * region.height();
            remembered_masks().put(*key, masks, pixels * (masks->tone.empty() ? 2 : 3));
        }
    }
    // (each pixel is its own: the part where the region and the image meet is painted the same)
    const Box meet{std::max(region.x0, here.x0), std::max(region.y0, here.y0), std::min(region.x1, here.x1), std::min(region.y1, here.y1)};
    const Box from{meet.x0 - region.x0, meet.y0 - region.y0, meet.x1 - region.x0, meet.y1 - region.y0};
    const Box to{meet.x0 - part.origin.x, meet.y0 - part.origin.y, meet.x1 - part.origin.x, meet.y1 - part.origin.y};
    composite(*part.image, to, *masks, from);
}

// --- a group ---------------------------------------------------------------------------------------------------------

// Everything of a line that drawing its balloon reads (a turned balloon's sheet is remembered by it).
Json line_json(const core::StoryLine& ln) {
    Json path;
    if (ln.path) {
        path = Json::array();
        for (const core::Point& p : *ln.path) path.push_back(Json::array({p.x.json(), p.y.json()}));
    }
    Json style_runs = Json::array();
    for (const auto& [words, style] : ln.style_runs) style_runs.push_back(Json::array({words, style}));
    Json out = Json::array({ln.id, ln.text, ln.speaker, ln.frame_id ? Json(*ln.frame_id) : Json(), ln.x_mm.json(), ln.y_mm.json(),
                            ln.w_mm.json(), ln.h_mm.json(), ln.balloon,
                            ln.tail ? Json::array({ln.tail->x.json(), ln.tail->y.json()}) : Json(), ln.wrap, Json(ln.ruby_runs), path,
                            Json(ln.emphasis_runs), style_runs, detail::keyed_style(ln.style), Json(ln.tails)});
    if (ln.ruby_spans) out.push_back(core::ruby::to_json(*ln.ruby_spans));  // (ruby-layout@1: the rubies by place)
    return out;
}

struct Sheet {
    Image turned;
};

detail::Memo<Sheet>& remembered_sheets() {
    static detail::Memo<Sheet> memo(64, 64LL * 1024 * 1024);
    return memo;
}

// 画像のフキダシ's picture stretched over its box (Lanczos), by the picture's digest and the box's size: once for all the
// parts of a page it reaches
detail::Memo<Image>& remembered_stretches() {
    static detail::Memo<Image> memo(64, 128LL * 1024 * 1024);
    return memo;
}

std::shared_ptr<const Image> stretched_picture(const std::string& data, const Image& picture, Size stretch) {
    const std::optional<std::string> key = detail::memo_key(Json::array({detail::digest_of(data), stretch.width, stretch.height}));
    if (key) {
        if (auto found = remembered_stretches().get(*key)) return found;
    }
    auto made = std::make_shared<const Image>(picture.resize(stretch, Resample::Lanczos));
    if (key) remembered_stretches().put(*key, made, static_cast<std::int64_t>(stretch.width) * stretch.height * 4);
    return made;
}

// Pillow's KeyError for OpenType features without raqm (the BASIC layout the reference is held to)
bool needs_raqm(const core::PyUncaught& e) {
    return e.type() == "KeyError" && std::string_view(e.what()).find("without libraqm") != std::string_view::npos;
}

// A line's letters on the part. What they need that this build cannot draw — NotYetPorted, and features across
// ("text_features") — is left out and reported when the caller says so: the balloon and the group's other lines are
// drawn. Without that, it stops the drawing as it is.
void letters(const PagePart& part, const core::StoryLine& line, int dpi, bool show_speaker, const std::optional<std::string>& font_path,
             std::stop_token stop, const Unported* unported, SpeakerFonts* speakers, const LetterParts* parts = nullptr) {
    if (parts != nullptr && !parts->text && !parts->ruby) return;  // (PSD-01: the balloon alone)
    try {
        paint_text(*part.image, line, dpi, show_speaker, font_path, std::move(stop), part.origin, speakers, parts);
    } catch (const NotYetPorted& e) {
        if (unported == nullptr || !*unported) throw;
        (*unported)(e);
    } catch (const core::PyUncaught& e) {
        if (unported == nullptr || !*unported || !needs_raqm(e)) throw;
        (*unported)(NotYetPorted("text_features"));
    }
}

// Where a turned group's sheet lies: the middle of the group's boxes (mm), the sheet's top left (mm), its size and where
// it is laid on the page (px).
struct TurnedSheet {
    Pt centre;
    double left = 0.0;
    double top = 0.0;
    std::int64_t size = 0;
    std::int64_t at_x = 0;
    std::int64_t at_y = 0;
};

TurnedSheet turned_sheet(const std::vector<const core::StoryLine*>& lines, int dpi) {
    double x0 = lines.front()->x_mm.value();
    double y0 = lines.front()->y_mm.value();
    double x1 = x0 + mm_or(lines.front()->w_mm, 40);
    double y1 = y0 + mm_or(lines.front()->h_mm, 20);
    for (const core::StoryLine* ln : lines) {
        x0 = core::py_min(x0, ln->x_mm.value());
        y0 = core::py_min(y0, ln->y_mm.value());
        x1 = core::py_max(x1, ln->x_mm.value() + mm_or(ln->w_mm, 40));
        y1 = core::py_max(y1, ln->y_mm.value() + mm_or(ln->h_mm, 20));
    }
    const Pt centre{(x0 + x1) / 2, (y0 + y1) / 2};
    double reach = core::py_hypot(x1 - x0, y1 - y0) / 2;
    for (const core::StoryLine* ln : lines) {
        for (const Json& tail : balloon::tails_of(*ln)) reach = core::py_max(reach, dist_to(centre, core::subscript(tail, "to")));
    }
    reach += 6;  // room for outlines, halos and the tail's bubbles
    const double left = centre[0] - reach;
    const double top = centre[1] - reach;
    return TurnedSheet{centre, left, top, px(2 * reach, dpi), core::py_round_int(left / 25.4 * dpi), core::py_round_int(top / 25.4 * dpi)};
}

// A line of a turned group as it is drawn upright on its sheet: moved by the sheet's corner, without its turn, its
// tails turned back about the group's middle.
core::StoryLine upright_twin(const core::StoryLine& ln, const TurnedSheet& sheet, double degrees) {
    const double left = sheet.left;
    const double top = sheet.top;
    const Pt& centre = sheet.centre;
    core::StoryLine twin = ln;
    twin.style = Json::object();
    if (ln.style.is_object()) {
        for (const auto& [k, v] : ln.style.items()) {
            if (k != "rotate_deg") twin.style[k] = v;
        }
    }
    twin.x_mm = core::Num(ln.x_mm.value() - left);
    twin.y_mm = core::Num(ln.y_mm.value() - top);
    core::type_text::move_anchor(twin, -left, -top);  // (PC5 G-a review P2-2: a point text's anchor on the sheet too)
    if (ln.path && !ln.path->empty()) {
        std::vector<core::Point> moved;
        for (const core::Point& p : *ln.path) moved.push_back(core::Point{core::Num(p.x.value() - left), core::Num(p.y.value() - top)});
        twin.path = std::move(moved);
    } else {
        twin.path.reset();
    }
    std::vector<Json> tails;
    for (const Json& tail : balloon::tails_of(ln)) {
        Json moved = tail;
        const Json to = core::subscript(tail, "to");
        const Pt t = balloon::turned(Pt{number(core::subscript(to, 0)), number(core::subscript(to, 1))}, centre, -degrees);
        moved["to"] = Json::array({t[0] - left, t[1] - top});
        const Json* via = core::dict_get(tail, "via");
        if (via != nullptr && core::py_truthy(*via)) {
            const Pt v = balloon::turned(Pt{number(core::subscript(*via, 0)), number(core::subscript(*via, 1))}, centre, -degrees);
            moved["via"] = Json::array({v[0] - left, v[1] - top});
        }
        tails.push_back(std::move(moved));
    }
    twin.tail = tails.empty() ? std::nullopt
                              : std::optional<core::Point>(core::Point{core::Num(tails.front()["to"][0].get<double>()),
                                                                       core::Num(tails.front()["to"][1].get<double>())});
    twin.tails = std::move(tails);
    return twin;
}

// _draw_turned(image, lines, dpi, show_speaker, font_path, degrees): drawn upright on its own sheet, turned about its
// centre and laid on the page; the tails turned back first, so they still point where they were aimed. (`parts`,
// PSD-01: only those drawn, on a sheet of their own.)
void draw_turned(const PagePart& part, const std::vector<const core::StoryLine*>& lines, int dpi, bool show_speaker,
                 const std::optional<std::string>& font_path, double degrees, std::stop_token stop, SpeakerFonts* speakers,
                 const LetterParts* parts = nullptr) {
    const TurnedSheet place = turned_sheet(lines, dpi);
    const std::int64_t size = place.size;
    const std::int64_t at_x = place.at_x;
    const std::int64_t at_y = place.at_y;
    if (overlaps(Box{static_cast<int>(at_x), static_cast<int>(at_y), static_cast<int>(at_x + size), static_cast<int>(at_y + size)},
                 page_box(part))) {
        const bool partial = parts != nullptr && !parts->all();
        // (the letters are mixed in the book's gamma, TextGamma: drawn on the sheet over their balloon; a group without
        // balloons — none, sfx — has only letters on its sheet, laid on the page in the gamma as an upright line's are)
        const std::optional<double> gamma = TextGamma::current();
        const std::string kind = kind_of(*lines.front());
        const bool letters_only = (kind == "none" || kind == "sfx") &&
                                  std::none_of(lines.begin(), lines.end(), [](const core::StoryLine* ln) { return ln->balloon == "picture"; });
        Json group = Json::array();
        for (const core::StoryLine* ln : lines) group.push_back(line_json(*ln));
        const std::optional<std::string> key = partial ? std::nullopt
                                                       : detail::memo_key(Json::array({"turned", dpi, font_path ? Json(*font_path) : Json(), group,
                                                                                       gamma ? Json(*gamma) : Json()}));
        std::shared_ptr<const Sheet> sheet = key ? remembered_sheets().get(*key) : nullptr;
        if (!sheet) {
            Image upright_sheet = Image::create("RGBA", Size{static_cast<int>(size), static_cast<int>(size)}, Ink{0, 0, 0, 0});
            std::vector<core::StoryLine> twins;
            twins.reserve(lines.size());
            for (const core::StoryLine* ln : lines) twins.push_back(upright_twin(*ln, place, degrees));
            std::vector<const core::StoryLine*> upright;
            for (const core::StoryLine& twin : twins) upright.push_back(&twin);
            draw_group(whole_page(upright_sheet), upright, dpi, false, font_path, nullptr, stop, nullptr, nullptr, parts);
            const double middle = static_cast<double>(size) / 2;
            auto made = std::make_shared<Sheet>();
            made->turned = upright_sheet.rotate(-degrees, Resample::Bicubic, false, std::pair<double, double>{middle, middle});
            sheet = made;
            if (key) remembered_sheets().put(*key, sheet, size * size * 4);
        }
        if (partial && (parts->text || parts->ruby)) {  // (PSD-01: the letters' own pixels, as paint_text lays a part)
            composite_at(*part.image, sheet->turned, at_x - part.origin.x, at_y - part.origin.y);
        } else if (gamma && letters_only && !partial) {
            paste_in_gamma(*part.image, sheet->turned, Point{static_cast<int>(at_x - part.origin.x), static_cast<int>(at_y - part.origin.y)}, *gamma);
        } else {
            part.image->paste(sheet->turned, Point{static_cast<int>(at_x - part.origin.x), static_cast<int>(at_y - part.origin.y)},
                              &sheet->turned);
        }
    }
    if (show_speaker) {
        std::optional<SpeakerFonts> own;
        if (speakers == nullptr) speakers = &own.emplace(stop);
        for (const core::StoryLine* ln : lines) {
            if (ln->speaker.empty() || kind_of(*ln) == "none") continue;
            const TrueTypeFont& font = speakers->fonts().font(face_of(Json()), std::max<std::int64_t>(8, px(3, dpi)));
            Draw draw(*part.image);
            const std::int64_t x = px(ln->x_mm.value(), dpi);
            const std::int64_t y = std::max<std::int64_t>(0, px(ln->y_mm.value() - 4, dpi));
            draw.text(PointD{static_cast<double>(x - part.origin.x), static_cast<double>(y - part.origin.y)}, u32(ln->speaker), font,
                      Ink{90, 90, 90});
        }
    }
}

}  // namespace

// --- the geometry ----------------------------------------------------------------------------------------------------

namespace balloon {

std::vector<Json> tails_of(const core::StoryLine& line) {
    std::vector<Json> tails;
    for (const Json& t : line.tails) {
        const Json* to = core::dict_get(t, "to");
        if (to != nullptr && core::py_truthy(*to)) tails.push_back(core::py_dict(t));
    }
    if (tails.empty() && line.tail) tails.push_back(Json{{"to", Json::array({line.tail->x.json(), line.tail->y.json()})}});
    return tails;
}

Pt turned(Pt point, Pt centre, double degrees) {
    // (the float sine and cosine, the same bits everywhere: a turned balloon's tail ends on the same pixel on every platform)
    const auto a = static_cast<float>(degrees * (core::kPi / 180.0));
    const double c = static_cast<double>(libm::cosf(a));
    const double s = static_cast<double>(libm::sinf(a));
    const double dx = point[0] - centre[0];
    const double dy = point[1] - centre[1];
    return {centre[0] + dx * c - dy * s, centre[1] + dx * s + dy * c};
}

Pt thought_trail(const core::StoryLine& line, const Panels* panels) {
    const double w = mm_or(line.w_mm, 40);
    const double h = mm_or(line.h_mm, 20);
    const Pt tries[4] = {{-0.3 * w, 1.2 * h}, {1.3 * w, 1.2 * h}, {-0.3 * w, -0.2 * h}, {1.3 * w, -0.2 * h}};
    const std::array<double, 4>* rect = nullptr;
    if (panels != nullptr && line.frame_id) {  // ({frame id: rect}.get(line.frame_id): the last one of that id)
        for (auto it = panels->rbegin(); it != panels->rend(); ++it) {
            if (it->first == *line.frame_id) {
                rect = &it->second;
                break;
            }
        }
    }
    const double lx = line.x_mm.value();
    const double ly = line.y_mm.value();
    if (rect == nullptr) return {lx + tries[0][0], ly + tries[0][1]};
    const auto [x0, y0, rw, rh] = *rect;
    const double inset = 2.0;
    for (const Pt& d : tries) {
        const double tx = lx + d[0];
        const double ty = ly + d[1];
        if (x0 + inset <= tx && tx <= x0 + rw - inset && y0 + inset <= ty && ty <= y0 + rh - inset) return {tx, ty};
    }
    // (no room outside the balloon: the nearest point inside the panel)
    return {core::py_min(core::py_max(lx + tries[0][0], x0 + inset), x0 + rw - inset),
            core::py_min(core::py_max(ly + tries[0][1], y0 + inset), y0 + rh - inset)};
}

}  // namespace balloon

// --- the groups and the page -----------------------------------------------------------------------------------------

void draw_group(const PagePart& part, const std::vector<const core::StoryLine*>& lines, int dpi, bool show_speaker,
                const std::optional<std::string>& font_path, const Panels* panels, std::stop_token stop, const Unported* unported,
                SpeakerFonts* speakers, const LetterParts* parts) {
    if (lines.empty()) return;
    std::optional<SpeakerFonts> own;
    if (speakers == nullptr) speakers = &own.emplace(stop);
    const core::StoryLine& first = *lines.front();
    const std::string kind = kind_of(first);
    const Json st = style_of(first);
    const Json& rotate = at(st, "rotate_deg");
    if (core::py_truthy(rotate) && std::fabs(core::to_float(rotate)) > 0.01) {
        // (its sheet is remembered as a whole: what it cannot draw leaves the whole group out, in draw_lines)
        draw_turned(part, lines, dpi, show_speaker, font_path, core::to_float(rotate), stop, speakers, parts);
        return;
    }
    const bool balloons = parts == nullptr || parts->balloon;  // (PSD-01: the balloon, or only the letters)
    const Box here = page_box(part);
    const Box page_all{0, 0, part.page.width, part.page.height};
    for (const core::StoryLine* ln : lines) {  // 画像のフキダシ: the picture stretched over the box
        if (ln->balloon != "picture" || !balloons) continue;
        const Json lst = style_of(*ln);
        const Json& data = at(lst, "picture");
        if (!core::py_truthy(data)) continue;
        core::require_hashable(data);  // (Python's hash(data) first, wherever the balloon lies)
        const auto place = [&] {
            const std::int64_t bx = px(ln->x_mm.value(), dpi);
            const std::int64_t by = px(ln->y_mm.value(), dpi);
            const Size stretch{static_cast<int>(std::max<std::int64_t>(1, px(mm_or(ln->w_mm, 40), dpi))),
                               static_cast<int>(std::max<std::int64_t>(1, px(mm_or(ln->h_mm, 20), dpi)))};
            return std::pair<Box, Size>{Box{static_cast<int>(bx), static_cast<int>(by), static_cast<int>(bx) + stretch.width,
                                            static_cast<int>(by) + stretch.height},
                                        stretch};
        };
        // (a part of the page the picture does not reach does not open it. Its place is known before the picture —
        // unless working it out fails, which Python meets only with a picture: then the picture first, as there)
        std::optional<std::pair<Box, Size>> placed;
        try {
            placed = place();
        } catch (const std::exception&) {
        }
        if (placed && !overlaps(placed->first, here)) {
            // (but what the group is decided for the whole page: a picture this build cannot open yet that reaches the
            // page leaves its group out of every part, whether the part shows the picture or not — as the whole page
            // drawn leaves it out)
            if (overlaps(placed->first, page_all) && picture_unported(data)) throw NotYetPorted("image_format");
            continue;
        }
        const std::shared_ptr<const std::optional<Image>> picture = remembered_picture(data);
        if (!*picture) continue;
        if (!placed) placed = place();
        const std::shared_ptr<const Image> stretched = stretched_picture(data.get_ref<const std::string&>(), **picture, placed->second);
        part.image->paste(*stretched, Point{placed->first.x0 - part.origin.x, placed->first.y0 - part.origin.y}, stretched.get());
    }
    if (kind == "picture") {
        for (const core::StoryLine* line : lines) letters(part, *line, dpi, show_speaker, font_path, stop, unported, speakers, parts);
        return;
    }
    if (kind != "sfx" && kind != "none" && balloons) {
        std::vector<std::array<std::int64_t, 4>> boxes;
        for (const core::StoryLine* ln : lines) boxes.push_back(box_px(*ln, dpi));
        Tails tails;
        for (const core::StoryLine* ln : lines) {
            if (no_tail(kind_of(*ln))) continue;
            for (Json& t : balloon::tails_of(*ln)) tails.emplace_back(ln, std::move(t));
        }
        for (const core::StoryLine* ln : lines) {  // a thought without a speaker still trails its bubbles, down and away
            if (kind_of(*ln) == "thought" && balloon::tails_of(*ln).empty()) {
                const Pt to = balloon::thought_trail(*ln, panels);
                tails.emplace_back(ln, Json{{"to", Json::array({to[0], to[1]})}});
            }
        }
        std::vector<Json> bends;
        for (const auto& [ln, t] : tails) {
            const Json* vias = core::dict_get(t, "vias");
            if (vias != nullptr && core::py_truthy(*vias)) {
                for (const Json& v : core::iterate(*vias)) bends.push_back(v);
                continue;
            }
            const Json* via = core::dict_get(t, "via");
            if (via != nullptr && core::py_truthy(*via)) bends.push_back(*via);
        }
        std::vector<std::int64_t> xs;
        std::vector<std::int64_t> ys;
        for (const auto& b : boxes) xs.push_back(b[0]);
        for (const auto& b : boxes) xs.push_back(b[2]);
        for (const auto& [ln, t] : tails) xs.push_back(px(number(core::subscript(core::subscript(t, "to"), 0)), dpi));
        for (const Json& v : bends) xs.push_back(px(number(core::subscript(v, 0)), dpi));
        for (const auto& b : boxes) ys.push_back(b[1]);
        for (const auto& b : boxes) ys.push_back(b[3]);
        for (const auto& [ln, t] : tails) ys.push_back(px(number(core::subscript(core::subscript(t, "to"), 1)), dpi));
        for (const Json& v : bends) ys.push_back(px(number(core::subscript(v, 1)), dpi));
        const std::int64_t margin = px(4, dpi);
        const std::int64_t rx0 = std::max<std::int64_t>(0, *std::min_element(xs.begin(), xs.end()) - margin);
        const std::int64_t ry0 = std::max<std::int64_t>(0, *std::min_element(ys.begin(), ys.end()) - margin);
        const std::int64_t rx1 = std::min<std::int64_t>(part.page.width, *std::max_element(xs.begin(), xs.end()) + margin);
        const std::int64_t ry1 = std::min<std::int64_t>(part.page.height, *std::max_element(ys.begin(), ys.end()) + margin);
        if (rx1 > rx0 && ry1 > ry0) {
            const Box region{static_cast<int>(rx0), static_cast<int>(ry0), static_cast<int>(rx1), static_cast<int>(ry1)};
            paint_shapes(part, lines, boxes, tails, region, dpi, st);
        }
    }
    for (const core::StoryLine* line : lines) letters(part, *line, dpi, show_speaker, font_path, stop, unported, speakers, parts);
}

void draw_lines(const PagePart& part, const std::vector<const core::StoryLine*>& lines, int dpi, const std::optional<std::string>& font_path,
                bool show_speaker, const Panels* panels, std::stop_token stop, const Unported* unported, const LetterParts* parts) {
    // joined balloons together, in the reading order of their first line (style.group, a dict's key)
    std::vector<std::vector<const core::StoryLine*>> order;
    std::vector<std::pair<Json, std::size_t>> keyed;
    for (const core::StoryLine* line : lines) {
        const Json st = style_of(*line);
        const Json& key = at(st, "group");
        if (!core::py_truthy(key)) {
            order.push_back({line});
            continue;
        }
        core::require_hashable(key);
        auto found = std::find_if(keyed.begin(), keyed.end(), [&](const auto& k) { return core::py_equals(k.first, key); });
        if (found == keyed.end()) {
            keyed.emplace_back(key, order.size());
            order.emplace_back();
            found = keyed.end() - 1;
        }
        order[found->second].push_back(line);
    }
    SpeakerFonts speakers(stop);  // (opened once for the names of all the groups, when one is drawn)
    for (const auto& group : order) {
        if (stop.stop_requested()) throw Cancelled();
        try {
            draw_group(part, group, dpi, show_speaker, font_path, panels, stop, unported, &speakers, parts);
        } catch (const NotYetPorted& e) {
            if (unported == nullptr || !*unported) throw;
            (*unported)(e);
        } catch (const core::PyUncaught& e) {  // (a turned balloon's letters: its sheet drawn whole or not at all)
            if (unported == nullptr || !*unported || !needs_raqm(e)) throw;
            (*unported)(NotYetPorted("text_features"));
        }
    }
}

EditableLine editable_line(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path) {
    EditableLine out;
    auto lettering = std::make_shared<SetLayout>(set_layout(line, dpi, font_path, LetterParts{false, false, false, false}));
    std::array<double, 6> m = lettering->set.to_image;
    const auto then = [&m](const std::array<double, 6>& q) {
        m = {q[0] * m[0] + q[2] * m[1], q[1] * m[0] + q[3] * m[1], q[0] * m[2] + q[2] * m[3], q[1] * m[2] + q[3] * m[3],
             q[0] * m[4] + q[2] * m[5] + q[4], q[1] * m[4] + q[3] * m[5] + q[5]};
    };
    const Json st = style_of(line);
    const Json& rotate = at(st, "rotate_deg");
    if (core::py_truthy(rotate) && std::fabs(core::to_float(rotate)) > 0.01) {
        // (as draw_turned lays the line alone: set on its upright sheet, the sheet turned about its middle by
        // Image.rotate(-degrees) — clockwise on the page — and laid at its place)
        const double degrees = core::to_float(rotate);
        const TurnedSheet sheet = turned_sheet({&line}, dpi);
        const Point at_sheet = lettering_at(upright_twin(line, sheet, degrees), dpi, lettering->layout.corner_x, lettering->layout.corner_y);
        then({1.0, 0.0, 0.0, 1.0, static_cast<double>(at_sheet.x), static_cast<double>(at_sheet.y)});
        const double middle = static_cast<double>(sheet.size) / 2;
        const double c = std::cos(degrees * core::kPi / 180.0);
        const double s = std::sin(degrees * core::kPi / 180.0);
        then({c, s, -s, c, middle - c * middle + s * middle, middle - s * middle - c * middle});
        then({1.0, 0.0, 0.0, 1.0, static_cast<double>(sheet.at_x), static_cast<double>(sheet.at_y)});
    } else {
        const Point at_page = lettering_at(line, dpi, lettering->layout.corner_x, lettering->layout.corner_y);
        then({1.0, 0.0, 0.0, 1.0, static_cast<double>(at_page.x), static_cast<double>(at_page.y)});
    }
    out.lettering = std::move(lettering);
    out.to_page = m;
    return out;
}

void clear_balloon_cache() {
    clear_layout_cache();
    remembered_masks().clear();
    remembered_sheets().clear();
    remembered_stretches().clear();
}

std::int64_t balloon_cache_bytes() {
    return layout_cache_bytes() + remembered_masks().used() + remembered_sheets().used() + remembered_stretches().used();
}

}  // namespace genko::render::text
