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
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <string_view>

#include "core/error.hpp"
#include "core/fukidashi.hpp"
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
#include "render/tones.hpp"

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
    bool soft = false;   // (no corner anywhere: smooth, or smooth but for its trembling — a drip's join with it is rounded)
    std::string kind;
    clean::Contour corners;  // (fukidashi@2: its corners as the pen sees them — where one stroke ends and the next begins)
};

// fukidashi@2: a corner of a box, or of a rounded balloon, rounded by less than this (style.corner_mm) is still a corner
// to the pen — a stroke ends there —; rounded by as much or more (and the rounded balloon's own roundness) it is part of
// one smooth line.
constexpr double kPenCornerMm = 2.0;

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
        if (core::py_truthy(at(lst, "path_curve"))) {
            outline = clean::smooth_closed(outline);
        } else {  // (drawn as given: its points that turn by more than 30° are its corners)
            const std::size_t n = outline.size();
            for (std::size_t i = 0; n >= 3 && i < n; ++i) {
                const P a = outline[(i + n - 1) % n], b = outline[i], c = outline[(i + 1) % n];
                const double in = clean::distance(a, b), on = clean::distance(b, c);
                if (in > 1e-9 && on > 1e-9 && ((b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y)) / (in * on) < 0.8660254037844387) body.corners.push_back(b);
            }
        }
        body.outline = outline;
        body.guide = std::move(outline);
        body.smooth = true;
        return body;
    }
    if (box.x1 - box.x0 < 1 || box.y1 - box.y0 < 1) return body;  // (a box of no pixel: nothing is drawn of it)
    // (style.corner_mm: 0 is square corners; not set, the kind's own)
    const double corner = at(lst, "corner_mm").is_number() ? core::to_float(at(lst, "corner_mm")) / 25.4 * dpi : -1.0;
    // (the pen's corners of a box: its own when its corners are square or rounded by less than kPenCornerMm)
    const bool cornered = at(lst, "corner_mm").is_number() && core::to_float(at(lst, "corner_mm")) < kPenCornerMm;
    if (kind == "rounded") {
        clean::Contour corners;
        body.outline = core::py_truthy(at(lst, "squircle")) ? clean::squircle(box) : clean::rounded(box, corner >= 0 ? corner : body.least * 0.6, &corners);
        if (cornered) body.corners = std::move(corners);
        body.smooth = true;
    } else if (square(kind)) {
        clean::Contour corners;
        body.outline = clean::rounded(box, corner >= 0 ? corner : 0.0, &corners);
        if (corner < 0 || cornered) body.corners = std::move(corners);
        body.smooth = true;
    } else if (kind == "capsule") {
        body.outline = clean::rounded(box, body.least);
        body.smooth = true;
    } else if (kind == "polygon") {
        const std::int64_t sides = int_or_zero(at(lst, "sides"));
        body.outline = clean::polygon(box, sides == 0 ? 8 : static_cast<int>(sides), corner >= 0 ? corner : body.least * 0.14, &body.corners);
        body.smooth = true;
    } else if (kind == "cloud") {
        const double rx = box.rx(), ry = box.ry();
        const double bump = core::py_max(2.0, body.least * 0.3);
        const double keep = 1 - bump / core::py_max(1.0, body.least);
        std::int64_t n = int_or_zero(at(lst, "bumps"));
        if (n == 0) n = std::max<std::int64_t>(8, core::py_trunc_int(core::kPi * (rx + ry) * keep / (bump * 1.4)));
        body.outline = clean::cloud(box, static_cast<int>(n), &body.corners);
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
        body.corners = body.outline;  // (every point and every valley)
        body.guide = clean::scaled_about(clean::ellipse(box), body.centre, (1 - depth) * 0.97);
    } else if (kind == "electric") {
        const double rx = box.rx(), ry = box.ry();
        std::int64_t teeth = int_or_zero(at(lst, "spikes"));
        if (teeth == 0) teeth = std::max<std::int64_t>(14, core::py_trunc_int((rx + ry) / core::py_max(3.0, body.least / 4)));
        const double depth = core::py_max(0.04, core::py_min(0.4, float_or(at(lst, "spike_depth"), 0.12)));
        body.outline = clean::sawtooth(box, static_cast<int>(teeth), depth);
        body.corners = body.outline;
        body.guide = clean::scaled_about(clean::squircle(box), body.centre, (1 - depth) * 0.97);
    } else if (kind == "heart") {  // (fukidashi@2: the symbols)
        body.outline = clean::heart(box, corner, &body.corners);
        body.smooth = true;
    } else if (kind == "diamond") {
        body.outline = clean::diamond(box, corner, &body.corners);
        body.smooth = true;
    } else if (kind == "star") {
        const int points = core::symbol_points(line);
        const double inner = core::star_inner(line);
        body.outline = clean::star(box, points, inner, corner, &body.corners);
        body.guide = clean::star_guide(box, points, inner);
        body.centre = clean::star_centre(box, points, inner);  // (an odd star's points stand round a middle below its box's)
    } else if (kind == "flower") {
        const int petals = core::symbol_points(line);
        body.outline = clean::flower(box, petals, &body.corners);
        body.guide = clean::flower_guide(box, petals);
    } else {  // speech, thought, whisper, flash
        body.outline = clean::ellipse(box);
        body.smooth = true;
    }
    if (body.guide.empty()) body.guide = body.outline;
    body.soft = body.smooth;
    if (body.smooth && kind != "flash") {
        const double wave = wave_px(lst, line, dpi);
        if (wave > 0) {  // (a trembling outline: its tails leave the outline it trembles about, with a corner)
            body.outline = clean::waved(body.outline, wave, core::py_max(wave * 7, 2.4 / 25.4 * dpi));
            body.smooth = false;
            body.corners.clear();  // (a trembling outline is one line all the way round)
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

// --- fukidashi@2 (SPEC BALLOON-02): a tone, and a blend, inside the outline --------------------------------------------

// How far along a tone that grows darker, or a blend of two colours, a place of the group's region lies (0..1): along
// a direction across the group's boxes, or from their middle outward (1 on the ellipse of their bounds).
struct Ramp {
    bool radial = false;
    double cx = 0.0, cy = 0.0;
    double ux = 0.0, uy = 1.0;
    double length = 1.0;
    double rx = 1.0, ry = 1.0;
    double at(double x, double y) const {
        if (radial) {
            const double dx = (x - cx) / rx, dy = (y - cy) / ry;
            return clamp01(std::sqrt(dx * dx + dy * dy));
        }
        return clamp01(((x - cx) * ux + (y - cy) * uy) / length + 0.5);
    }
};

// The ramp over the boxes of a group (region pixels): `degrees` the way it runs (0 to the right, 90 down the page; the
// float sine and cosine, the same bits everywhere).
Ramp ramp_over(const std::vector<clean::Box>& boxes, double degrees, bool radial) {
    Ramp ramp;
    if (boxes.empty()) return ramp;
    double x0 = boxes.front().x0, y0 = boxes.front().y0, x1 = boxes.front().x1, y1 = boxes.front().y1;
    for (const clean::Box& b : boxes) {
        x0 = core::py_min(x0, b.x0);
        y0 = core::py_min(y0, b.y0);
        x1 = core::py_max(x1, b.x1);
        y1 = core::py_max(y1, b.y1);
    }
    ramp.radial = radial;
    ramp.cx = (x0 + x1) / 2;
    ramp.cy = (y0 + y1) / 2;
    ramp.rx = core::py_max(1.0, (x1 - x0) / 2);
    ramp.ry = core::py_max(1.0, (y1 - y0) / 2);
    const auto a = static_cast<float>(degrees * (core::kPi / 180.0));
    ramp.ux = static_cast<double>(libm::cosf(a));
    ramp.uy = static_cast<double>(libm::sinf(a));
    // (straight down, up or across: the float sine of a half turn is not quite 0 — a ramp down the page is the same
    // all along a row)
    if (std::fabs(ramp.ux) < 1e-6) ramp.ux = 0.0;
    if (std::fabs(ramp.uy) < 1e-6) ramp.uy = 0.0;
    ramp.length = core::py_max(1.0, std::fabs(ramp.ux) * (x1 - x0) + std::fabs(ramp.uy) * (y1 - y0));
    return ramp;
}

// A number of a tone's or a blend's settings, kept within its range (the reader does not check a book's styles: what
// is not a number is the default).
double setting(const Json& spec, std::string_view key, double fallback, double least, double most) {
    const Json& value = at(spec, key);
    if (!value.is_number()) return fallback;
    const double v = core::to_float(value);
    if (!(v == v)) return fallback;
    return core::py_max(least, core::py_min(most, v));
}

// style.fill_tone: a screentone inside the outline, in the line's colour — dots, or lines, on the lattice the tone
// layers screen with (render/tones' screen_vector: whole pixels, lined up with the page), each pixel's coverage its
// exact area.
struct ToneSpec {
    double density = 0.0;
    double to = 0.0;
    bool graded = false;  // (it grows from `density` to `to` along its ramp)
    bool lines = false;
    double lpi = 60.0;
    double angle = 45.0;
    double dir = 90.0;
    bool radial = false;
    double inset_mm = 0.0;
};

std::optional<ToneSpec> tone_spec(const Json& st) {
    const Json& spec = at(st, "fill_tone");
    if (!spec.is_object()) return std::nullopt;
    if (!fill_tone_draws(st)) return std::nullopt;  // (nothing to draw: as if it were not there — a tone box keeps its own)
    ToneSpec tone;
    tone.density = setting(spec, "density", 0.0, 0.0, 0.9);
    tone.graded = at(spec, "to").is_number();
    tone.to = tone.graded ? setting(spec, "to", tone.density, 0.0, 0.9) : tone.density;
    tone.lines = at(spec, "pattern") == Json("line");
    tone.lpi = setting(spec, "lpi", 60.0, 20.0, 85.0);
    tone.angle = setting(spec, "angle", 45.0, 0.0, 90.0);
    tone.dir = setting(spec, "dir", 90.0, -360.0, 360.0);
    tone.radial = core::py_truthy(at(spec, "radial"));
    tone.inset_mm = setting(spec, "inset_mm", 0.0, 0.0, 5.0);
    return tone;
}

constexpr int kToneLevels = 64;       // (a tone that grows darker: this many steps of density)
constexpr std::int64_t kToneTile = 512;  // (a lattice that repeats within this many pixels is drawn once and laid like tiles)

// floor(a / b) for b > 0, and the remainder that goes with it (0 .. b − 1).
std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}
std::int64_t floor_mod(std::int64_t a, std::int64_t b) { return a - floor_div(a, b) * b; }

// How much of a pixel lies on the near side of a straight edge: `s` the edge's distance past the pixel's middle,
// (a, b) the edge's unit normal made positive, a ≥ b. The exact area.
double edge_share(double s, double a, double b) {
    const double s1 = (a - b) / 2, s2 = (a + b) / 2;
    if (s <= -s2) return 0.0;
    if (s >= s2) return 1.0;
    if (s < -s1) return (s + s2) * (s + s2) / (2 * a * b);
    if (s > s1) return 1.0 - (s2 - s) * (s2 - s) / (2 * a * b);
    return 0.5 + s / a;
}

// The radius of a tone's dots, in cells, for the dots to cover `density` of the page: a circle of that area — and, above
// π / 4, where a dot meets the four beside it, the bigger circle whose part that is its own alone has that area (from a
// table, a line between its entries: no arc cosine at drawing time).
double dot_radius(double density) {
    if (!(density > 0)) return 0.0;
    static constexpr double kDense[][2] = {{0.7853981633974483, 0.5},  {0.80, 0.5052724328106947}, {0.82, 0.5135272122633895}, {0.84, 0.5227962158058063},
                                           {0.86, 0.5331141606423667}, {0.88, 0.5446096225474605}, {0.90, 0.5575069513285796}};
    if (density <= kDense[0][0]) return std::sqrt(density / core::kPi);
    for (std::size_t k = 1; k < std::size(kDense); ++k) {
        if (density <= kDense[k][0]) {
            const double t = (density - kDense[k - 1][0]) / (kDense[k][0] - kDense[k - 1][0]);
            return kDense[k - 1][1] + (kDense[k][1] - kDense[k - 1][1]) * t;
        }
    }
    return kDense[std::size(kDense) - 1][1];
}

// One period of a dot lattice (m, n) with dots of `radius`: `side` × `side` pixels of coverage, 0..255, from the page's
// corner. Remembered (a balloon's tone at one density is one of them; a tone that grows darker, up to 64).
detail::Memo<std::string>& remembered_tone_tiles() {
    static detail::Memo<std::string> memo(1024, 48LL * 1024 * 1024);
    return memo;
}

// The dot of cell (i, j) of the lattice: its middle on the page (the middle of the tone layers' cell: a pixel's middle
// is its index and a half).
P dot_middle(std::int64_t i, std::int64_t j, std::int64_t m, std::int64_t n) {
    return {(static_cast<double>(i) + 0.5) * static_cast<double>(m) - (static_cast<double>(j) + 0.5) * static_cast<double>(n) + 0.5,
            (static_cast<double>(i) + 0.5) * static_cast<double>(n) + (static_cast<double>(j) + 0.5) * static_cast<double>(m) + 0.5};
}

// The cells of the lattice whose dots (of at most `reach`) can touch the box (x0, y0, x1, y1): the range of i and of j.
std::array<std::int64_t, 4> cells_over(double x0, double y0, double x1, double y1, std::int64_t m, std::int64_t n, double reach) {
    const double norm = static_cast<double>(m * m + n * n);
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    bool first = true;
    for (const double x : {x0 - reach, x1 + reach}) {
        for (const double y : {y0 - reach, y1 + reach}) {
            const double u = (x * static_cast<double>(m) + y * static_cast<double>(n)) / norm;
            const double v = (-x * static_cast<double>(n) + y * static_cast<double>(m)) / norm;
            if (first) u0 = u1 = u, v0 = v1 = v, first = false;
            u0 = core::py_min(u0, u), u1 = core::py_max(u1, u), v0 = core::py_min(v0, v), v1 = core::py_max(v1, v);
        }
    }
    return {static_cast<std::int64_t>(std::floor(u0)) - 1, static_cast<std::int64_t>(std::ceil(u1)) + 1, static_cast<std::int64_t>(std::floor(v0)) - 1,
            static_cast<std::int64_t>(std::ceil(v1)) + 1};
}

std::shared_ptr<const std::string> tone_tile(std::int64_t m, std::int64_t n, std::int64_t side, double radius) {
    const std::optional<std::string> key = detail::memo_key(Json::array({"tone", m, n, side, radius}));
    if (key) {
        if (auto found = remembered_tone_tiles().get(*key)) return found;
    }
    std::vector<clean::Contour> dots;
    if (radius > 0) {
        const auto [i0, i1, j0, j1] = cells_over(0.0, 0.0, static_cast<double>(side), static_cast<double>(side), m, n, radius + 1);
        for (std::int64_t j = j0; j <= j1; ++j) {
            for (std::int64_t i = i0; i <= i1; ++i) {
                const P c = dot_middle(i, j, m, n);
                if (c.x < -radius - 1 || c.y < -radius - 1 || c.x > static_cast<double>(side) + radius + 1 || c.y > static_cast<double>(side) + radius + 1) continue;
                dots.push_back(clean::circle(c, radius));
            }
        }
    }
    const clean::Plane plane = clean::union_plane(dots, static_cast<int>(side), static_cast<int>(side));
    auto bytes = std::make_shared<std::string>(plane.values.size(), '\0');
    for (std::size_t k = 0; k < plane.values.size(); ++k) {
        (*bytes)[k] = static_cast<char>(static_cast<std::uint8_t>(clamp01(static_cast<double>(plane.values[k])) * 255.0 + 0.5));
    }
    if (key) remembered_tone_tiles().put(*key, bytes, static_cast<std::int64_t>(bytes->size()) + 64);
    return bytes;
}

// The tone over the region, cut to where the group lies (`cover`): `ramp` in the region's pixels, (ox, oy) the region's
// corner on the page.
// (`edge`: nothing, or the line's reach at each pixel when white is left inside it — the tone is taken away there. What
// comes back is the tone cut to the group: a byte a pixel, 0..255.)
std::string tone_bytes(const ToneSpec& tone, const Ramp& ramp, Size size, int dpi, std::int64_t ox, std::int64_t oy, const std::vector<float>& cover,
                       const std::vector<float>* edge) {
    const auto w = static_cast<std::size_t>(size.width);
    const auto h = static_cast<std::size_t>(size.height);
    std::string out(w * h, '\0');
    if (!(tone.density > 0) && !(tone.graded && tone.to > 0)) return out;
    // (a pixel of the tone: its share of ink × how much of it the group covers, less the line's reach)
    const auto put = [&](std::size_t at, double share) {
        double v = share * static_cast<double>(cover[at]);
        if (edge != nullptr) v *= 1.0 - static_cast<double>((*edge)[at]);
        out[at] = static_cast<char>(static_cast<std::uint8_t>(clamp01(v) * 255.0 + 0.5));
    };
    // (only where the group lies — `cover` —: the rows and columns it reaches)
    std::size_t x_from = w, x_to = 0, y_from = h, y_to = 0;
    for (std::size_t y = 0; y < h; ++y) {
        const float* row = cover.data() + y * w;
        std::size_t first = 0;
        while (first < w && row[first] <= 0.0f) ++first;
        if (first == w) continue;
        std::size_t last = w;
        while (last > first && row[last - 1] <= 0.0f) --last;
        x_from = std::min(x_from, first), x_to = std::max(x_to, last);
        y_from = std::min(y_from, y), y_to = y + 1;
    }
    if (x_from >= x_to) return out;
    const std::pair<int, int> vector = tones::screen_vector(dpi, tone.lpi, tone.angle);
    const std::int64_t m = std::abs(vector.first), n = std::abs(vector.second);
    const std::int64_t norm = m * m + n * n;
    const double pitch = std::sqrt(static_cast<double>(norm));
    const auto density_at = [&](double x, double y) {  // (a place of the region)
        return tone.graded ? tone.density + (tone.to - tone.density) * ramp.at(x, y) : tone.density;
    };
    if (tone.lines) {
        // stripes along the lattice's (m, n), a cell apart, each density × the cell thick: every pixel's share of the
        // stripe it lies on and of the two beside it
        const double na = static_cast<double>(std::max(m, n)) / pitch, nb = static_cast<double>(std::min(m, n)) / pitch;
        for (std::size_t y = y_from; y < y_to; ++y) {
            const std::int64_t py = static_cast<std::int64_t>(y) + oy;
            for (std::size_t x = x_from; x < x_to; ++x) {
                if (cover[y * w + x] <= 0.0f) continue;
                const std::int64_t px = static_cast<std::int64_t>(x) + ox;
                const double half = density_at(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5) * pitch / 2;
                if (!(half > 0)) continue;
                // (how far across its cell the pixel's middle lies, from the stripe's middle line: −pitch / 2 .. pitch / 2)
                const double s = (static_cast<double>(floor_mod(-px * n + py * m, norm)) / static_cast<double>(norm) - 0.5) * pitch;
                double share = 0.0;
                for (const double d : {s - pitch, s, s + pitch}) share += edge_share(d + half, na, nb) - edge_share(d - half, na, nb);
                put(y * w + x, clamp01(share));
            }
        }
        return out;
    }
    const auto radius_of = [&](double density) { return pitch * dot_radius(density); };
    const auto level_of = [&](double t) { return std::min(kToneLevels - 1, static_cast<int>(t * kToneLevels)); };
    const auto level_density = [&](int level) { return tone.density + (tone.to - tone.density) * (static_cast<double>(level) + 0.5) / kToneLevels; };
    // (a cell's density: at its dot's middle, in the region's pixels)
    const auto cell_level = [&](std::int64_t i, std::int64_t j) {
        const P c = dot_middle(i, j, m, n);
        return level_of(ramp.at(c.x - static_cast<double>(ox), c.y - static_cast<double>(oy)));
    };
    const std::int64_t common = std::gcd(m, n);
    const std::int64_t side = norm / std::max<std::int64_t>(1, common);  // (the lattice repeats every `side` pixels, both ways)
    if (side <= kToneTile) {
        std::array<std::shared_ptr<const std::string>, kToneLevels> tiles{};
        const auto tile_of = [&](int level) -> const std::string& {
            auto& tile = tiles[static_cast<std::size_t>(level)];
            if (!tile) tile = tone_tile(m, n, side, radius_of(tone.graded ? level_density(level) : tone.density));
            return *tile;
        };
        for (std::size_t y = y_from; y < y_to; ++y) {
            const std::int64_t py = static_cast<std::int64_t>(y) + oy;
            const std::size_t row = static_cast<std::size_t>(floor_mod(py, side)) * static_cast<std::size_t>(side);
            for (std::size_t x = x_from; x < x_to; ++x) {
                if (cover[y * w + x] <= 0.0f) continue;
                const std::int64_t px = static_cast<std::int64_t>(x) + ox;
                // (the cell the pixel's middle lies in: whole numbers, as the tone layers count them)
                const int level = tone.graded ? cell_level(floor_div(px * m + py * n, norm), floor_div(-px * n + py * m, norm)) : 0;
                const auto v = static_cast<unsigned char>(tile_of(level)[row + static_cast<std::size_t>(floor_mod(px, side))]);
                put(y * w + x, static_cast<double>(v) / 255.0);
            }
        }
        return out;
    }
    // (a coarse lattice that does not repeat soon: the region's own dots, each drawn where it lies)
    const double most = radius_of(core::py_max(tone.density, tone.to));
    const double fx = static_cast<double>(ox), fy = static_cast<double>(oy);
    const double bx0 = static_cast<double>(x_from), by0 = static_cast<double>(y_from), bx1 = static_cast<double>(x_to), by1 = static_cast<double>(y_to);
    const auto [i0, i1, j0, j1] = cells_over(fx + bx0, fy + by0, fx + bx1, fy + by1, m, n, most + 1);
    std::vector<clean::Contour> dots;
    for (std::int64_t j = j0; j <= j1; ++j) {
        for (std::int64_t i = i0; i <= i1; ++i) {
            const P c = dot_middle(i, j, m, n);
            const double radius = radius_of(tone.graded ? level_density(cell_level(i, j)) : tone.density);
            if (!(radius > 0)) continue;
            const P local{c.x - fx, c.y - fy};
            if (local.x < bx0 - radius - 1 || local.y < by0 - radius - 1 || local.x > bx1 + radius + 1 || local.y > by1 + radius + 1) continue;
            dots.push_back(clean::circle(local, radius));
        }
    }
    const clean::Plane drawn = clean::union_plane(dots, size.width, size.height);
    for (std::size_t y = y_from; y < y_to; ++y) {
        for (std::size_t x = x_from; x < x_to; ++x) {
            if (cover[y * w + x] > 0.0f) put(y * w + x, static_cast<double>(drawn.values[y * w + x]));
        }
    }
    return out;
}

// --- fukidashi@2 (SPEC BALLOON-02): drips ---------------------------------------------------------------------------------

// One drip of a line's balloon as its plan says it, in millimetres: where its upright line stands (from the left of the
// body's box), half its width, and how far down its tip lies (from the top of the body's box); `longest`: the one that
// runs down to the box's foot.
struct Drip {
    double x_mm = 0.0;
    double half_mm = 0.0;
    double tip_mm = 0.0;
    bool longest = false;
};

// Where the upright line at x leaves a closed outline below `from_y` (the y of the lowest crossing); nothing when x
// does not lie inside the outline there.
std::optional<double> leaves_below(const clean::Contour& outline, const std::vector<double>& lengths, double x, double from_y, double far_below) {
    if (!clean::inside(outline, {x, from_y})) return std::nullopt;
    const P on = clean::point_at(outline, lengths, clean::facing(outline, lengths, {x, from_y}, {x, far_below}));
    if (std::fabs(on.x - x) > 0.01 || on.y <= from_y) return std::nullopt;
    return on.y;
}

// The bounds of an outline (x0, y0, x1, y1).
std::array<double, 4> bounds_of(const clean::Contour& outline) {
    std::array<double, 4> b{outline.front().x, outline.front().y, outline.front().x, outline.front().y};
    for (const P& p : outline) {
        b[0] = core::py_min(b[0], p.x), b[1] = core::py_min(b[1], p.y);
        b[2] = core::py_max(b[2], p.x), b[3] = core::py_max(b[3], p.y);
    }
    return b;
}

// The drips of a line's balloon (style.drip_mm and drips, drip_width_mm, drip_seed): which there are, where and how
// long — worked out from the line's millimetres alone, on its balloon made a hundred pixels to the millimetre, so that
// every resolution draws the same drips. They stand in equal slots across the middle of the outline its tails leave from
// — 0.8 of its width, 0.9 for a square kind —, as many as style.drips (not said: one every 3.2 widths, 3 to 16), each as
// wide as style.drip_width_mm (not said: 7% of the body's width, 1.2 to 4 mm), never wider than 0.9 of its slot or 0.3
// of the body. What differs from drip to drip comes from Python's random.Random(drip_seed) alone — the seed the line
// holds —, drawn in one order: first which slot of the middle third holds the longest drip, then for each slot how far
// off its slot's middle it stands (±0.3 of a slot), how long it is (0.25 to 1 of the reach), how wide (0.7 to 1.3 of the
// width), and whether it is there at all (four in five are). A drip the outline has no foot for where it stands, or the
// balloon is too thin to hold, is left out — but the longest is always there: where its own slot cannot hold it, it
// hangs from the outline's middle.
std::vector<Drip> drip_plan(const core::StoryLine& line, const Json& lst) {
    std::vector<Drip> plan;
    const double reach_mm = core::drip_reach(line);
    if (!(reach_mm > 0)) return plan;
    const std::array<double, 4> body_mm = core::body_box(line);
    const double mm = 100.0;  // (pixels a millimetre here)
    const clean::Box box{0.0, 0.0, body_mm[2] * mm, body_mm[3] * mm};
    if (!(box.x1 >= 1) || !(box.y1 >= 1)) return plan;
    const Body body = body_of(line, lst, box, Box{0, 0, 1, 1}, 2540);
    if (body.outline.size() < 3 || body.guide.size() < 3) return plan;
    const double foot = box.y1 + reach_mm * mm;
    const double reach = reach_mm * mm;
    const std::array<double, 4> guide = bounds_of(body.guide);
    const Json& border = at(lst, "border_mm");
    const double line_width = (border.is_number() ? core::py_max(0.0, core::py_min(30.0, core::to_float(border))) : 0.35) * mm;
    const double body_w = box.x1 - box.x0;
    const double usual = at(lst, "drip_width_mm").is_number() ? setting(lst, "drip_width_mm", 2.0, 0.3, 10.0) * mm
                                                              : core::py_max(1.2 * mm, core::py_min(4.0 * mm, body_w * 0.07));
    const double span = (guide[2] - guide[0]) * (square(body.kind) ? 0.9 : 0.8);
    if (!(span > 0)) return plan;
    // (a half up from the nearest whole number is the next one, whatever the last bit of the division says)
    const auto count = static_cast<std::int64_t>(at(lst, "drips").is_number() ? setting(lst, "drips", 3.0, 1.0, 24.0)
                                                                              : core::py_max(3.0, core::py_min(16.0, std::floor(span / (3.2 * usual) + 0.5 + 1e-9))));
    const auto seed = static_cast<std::int64_t>(setting(lst, "drip_seed", 0.0, 0.0, 999999.0));
    core::PyRandom rng(seed);
    const std::int64_t third = count / 3;
    const std::int64_t longest = third + static_cast<std::int64_t>(rng.random() * static_cast<double>(std::max<std::int64_t>(1, count - 2 * third)));
    const double slot = span / static_cast<double>(count);
    const double middle = (guide[0] + guide[2]) / 2;
    const double first = middle - span / 2;
    const std::vector<double> guide_lengths = clean::lengths_of(body.guide);
    const std::vector<double> outline_lengths = clean::lengths_of(body.outline);
    const double from_y = (guide[1] + guide[3]) / 2;
    // (how far inside the balloon its sides begin, at the coarsest: a line and a third of a millimetre, half as much
    // again to spare)
    const double depth = (line_width * 3 + 0.35 * mm) * 1.5;
    for (std::int64_t k = 0; k < count; ++k) {
        const double off = rng.uniform(-0.3, 0.3);
        const double share = rng.uniform(0.25, 1.0);
        const double wide = rng.uniform(0.7, 1.3);
        const bool there = rng.random() >= 0.2 || k == longest;
        if (!there) continue;
        const double half = core::py_max(0.05 * mm, core::py_min(usual * wide / 2, core::py_min(slot * 0.45, body_w * 0.15)));
        // (where it stands: its own place in its slot — and, the longest only, the outline's middle when that cannot hold it)
        for (const double x : {first + (static_cast<double>(k) + 0.5 + off) * slot, middle}) {
            const std::optional<double> root = leaves_below(body.outline, outline_lengths, x, from_y, foot + 10);
            const std::optional<double> left = leaves_below(body.guide, guide_lengths, x - half, from_y, foot + 10);
            const std::optional<double> right = leaves_below(body.guide, guide_lengths, x + half, from_y, foot + 10);
            bool fits = root && left && right;
            double tip = 0.0;
            if (fits) {
                const double clear = core::py_max(*root, core::py_max(leaves_below(body.outline, outline_lengths, x - half, from_y, foot + 10).value_or(*root),
                                                                      leaves_below(body.outline, outline_lengths, x + half, from_y, foot + 10).value_or(*root))) +
                                     half * 0.3;
                tip = core::py_min(foot, core::py_max(k == longest ? foot : *root + reach * share, clear + 1.2 * half));
                fits = tip - clear >= 0.1 * mm && clean::inside(body.outline, {x - half, *left - depth}) && clean::inside(body.outline, {x + half, *right - depth});
            }
            if (fits) {
                plan.push_back(Drip{x / mm, half / mm, tip / mm, k == longest});
                break;
            }
            if (k != longest) break;
        }
    }
    return plan;
}

// What was drawn of a plan's drip at a resolution (the same units as the plan's: fukidashi@2's traces, for the tests).
struct DripDrawn {
    double x_mm = 0.0;
    double half_mm = 0.0;
    double tip_mm = 0.0;
};

// The plan's drips added to the group's shapes at this resolution: each a finger (clean::drip) that hangs from the body's
// outline at the plan's place — on a pixel's middle, so an upright drip is its own mirror image on the page — down to the
// plan's tip (the longest: the box's foot `foot`), its sides beginning a little inside the balloon, its join with a
// balloon without corners rounded (the joins stop short of a corner).
void add_drips(std::vector<clean::Shape>& shapes, const std::vector<Drip>& plan, const Body& body, const clean::Box& box, double foot, double line_width,
               double nominal, int dpi, std::vector<DripDrawn>* drawn) {
    if (plan.empty() || body.outline.size() < 3 || body.guide.size() < 3) return;
    if (!(foot - box.y1 >= 1)) return;
    const double mm = dpi / 25.4;
    const std::array<double, 4> guide = bounds_of(body.guide);
    const std::vector<double> guide_lengths = clean::lengths_of(body.guide);
    const std::vector<double> outline_lengths = clean::lengths_of(body.outline);
    const double from_y = (guide[1] + guide[3]) / 2;
    for (const Drip& one : plan) {
        const double half = core::py_max(1.0, one.half_mm * mm);
        const double x = box.x0 + std::floor(one.x_mm * mm) + 0.5;
        const std::optional<double> root = leaves_below(body.outline, outline_lengths, x, from_y, foot + 10);
        const std::optional<double> left = leaves_below(body.guide, guide_lengths, x - half, from_y, foot + 10);
        const std::optional<double> right = leaves_below(body.guide, guide_lengths, x + half, from_y, foot + 10);
        if (!root || !left || !right) continue;  // (a pixel off the outline's foot at this resolution)
        // (it is a finger from where both its sides have left the outline — on a slope its upper side runs straight
        // down to there —, a little past it: the outline crosses its side, not a corner of it)
        const double clear = core::py_max(*root, core::py_max(leaves_below(body.outline, outline_lengths, x - half, from_y, foot + 10).value_or(*root),
                                                              leaves_below(body.outline, outline_lengths, x + half, from_y, foot + 10).value_or(*root))) +
                             core::py_max(1.5, half * 0.3);
        const double tip = one.longest ? foot : core::py_min(foot, core::py_max(box.y0 + one.tip_mm * mm, clear + 1.0));
        const double length = tip - clear;
        if (!(length >= 1)) continue;
        // (its sides begin a little inside the balloon — less if the balloon is thin there)
        double depth = core::py_max(2.0, line_width * 3 + 2);
        bool held = false;
        for (int attempt = 0; attempt < 4 && !held; ++attempt) {
            held = clean::inside(body.outline, {x - half, *left - depth}) && clean::inside(body.outline, {x + half, *right - depth});
            if (!held) depth /= 2;
        }
        if (!held) continue;
        clean::Shape finger = clean::drip(x, clear, *left - depth, *right - depth, half, length);
        if (finger.points.size() < 3) continue;
        if (drawn != nullptr) drawn->push_back(DripDrawn{(x - box.x0) / mm, half / mm, (tip - box.y0) / mm});
        // (its join with the balloon rounded, as liquid gathers before it runs — a finger slimmer than a line and a half
        // either side has none: the join would be bigger than the drip)
        if (body.soft && one.half_mm * mm >= nominal * 1.5) {
            for (clean::Shape& join : clean::drip_joins(body.outline, finger.points, core::py_max(line_width * 1.5, half * 0.9))) shapes.push_back(std::move(join));
        }
        shapes.push_back(std::move(finger));
    }
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
    Image tone;  // (empty unless tone_box, or — fukidashi@2 — the line has a tone inside: style.fill_tone)
    std::vector<std::int64_t> line_rgb;
    Image band;
    // (fukidashi@2, style.fill_gradient: the fill's colour runs from `paper` to `blend_to` along the ramp, in the
    // region's pixels)
    bool blend = false;
    std::vector<std::int64_t> blend_to;
    Ramp ramp;
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

// fukidashi@2: the pen a group's line is drawn with — style.pen_swell (0..0.9) and pen_angle (the way the heavy side
// faces: 0 to the right, 90 down the page; 45 when not said), pen_taper (0..1) and pen_strokes (1..8; 2 when not said) —,
// or nothing: an even line. A dotted box and a flash have no line to swell.
std::optional<clean::Pen> pen_of(const Json& st, std::string_view kind, double width, int dpi) {
    if (kind == "dotted_box" || kind == "flash") return std::nullopt;
    const double swell = setting(st, "pen_swell", 0.0, 0.0, 0.9);
    const double taper = setting(st, "pen_taper", 0.0, 0.0, 1.0);
    if (!(swell > 0) && !(taper > 0)) return std::nullopt;
    clean::Pen pen;
    pen.width = width;
    pen.swell = swell;
    pen.taper = taper;
    const auto a = static_cast<float>(setting(st, "pen_angle", 45.0, 0.0, 360.0) * (core::kPi / 180.0));
    pen.heavy = {static_cast<double>(libm::cosf(a)), static_cast<double>(libm::sinf(a))};
    pen.strokes = static_cast<int>(setting(st, "pen_strokes", 2.0, 1.0, 8.0));
    pen.step = core::py_max(2.0, 0.5 / 25.4 * dpi);
    // (the line's own width, before it was made whole pixels: what the strokes' rules count in)
    const Json& border = at(st, "border_mm");
    pen.nominal = core::py_max(0.25, (border.is_number() ? core::to_float(border) : 0.35) / 25.4 * dpi);
    return pen;
}

// A group's shapes as they are made for a region at a resolution: each line's balloon, the joins of joined balloons,
// the tails with their joins, the thought's bubbles, and last — from `drips_from` — the drips' fingers and their joins;
// with each balloon's body, its box (`locals`: the body's — the line's box less its drips' reach; `whole_boxes`: the
// line's own), the fading tails, the corners the pen's strokes end at, and the drips as drawn.
struct Built {
    std::vector<clean::Shape> shapes;
    std::size_t drips_from = 0;
    std::vector<Body> bodies;
    std::vector<clean::Box> locals;
    std::vector<clean::Box> whole_boxes;
    std::vector<std::pair<clean::Box, P>> fades;
    clean::Contour corners;
    std::vector<DripDrawn> drips;
};

Built build_shapes(const std::vector<const core::StoryLine*>& lines, const std::vector<std::array<std::int64_t, 4>>& boxes, const Tails& tails,
                   const Box& region, int dpi, double width) {
    const std::int64_t rx0 = region.x0, ry0 = region.y0;
    const auto local = [&](const std::array<std::int64_t, 4>& b) {
        return clean::Box{static_cast<double>(b[0] - rx0), static_cast<double>(b[1] - ry0), static_cast<double>(b[2] - rx0),
                          static_cast<double>(b[3] - ry0)};
    };
    const auto at_px = [&](const Json& point) {
        return P{static_cast<double>(px(number(core::subscript(point, 0)), dpi) - rx0),
                 static_cast<double>(px(number(core::subscript(point, 1)), dpi) - ry0)};
    };
    // the group's shapes: each line's balloon, then their tails, the joins and the bubbles
    Built built;
    std::vector<clean::Shape>& shapes = built.shapes;
    std::vector<Body>& bodies = built.bodies;
    std::vector<clean::Box>& locals = built.locals;
    std::vector<std::pair<clean::Box, P>>& fades = built.fades;
    std::vector<clean::Box>& whole_boxes = built.whole_boxes;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        whole_boxes.push_back(local(boxes[i]));
        locals.push_back(whole_boxes.back());
        if (core::drip_reach(*lines[i]) > 0) {  // (fukidashi@2: the body's foot raised; the drips run down to the box's)
            const std::array<double, 4> body = core::body_box(*lines[i]);
            locals.back().y1 = core::py_max(locals.back().y0 + 1, core::py_min(locals.back().y1, static_cast<double>(px(body[1] + body[3], dpi) - ry0)));
        }
        bodies.push_back(body_of(*lines[i], style_of(*lines[i]), locals.back(), region, dpi));
        shapes.push_back(clean::whole(bodies.back().outline));
    }
    for (std::size_t i = 0; i + 1 < bodies.size(); ++i) {  // (joined balloons: their notches rounded, or a neck between them)
        const Body& a = bodies[i];
        const Body& b = bodies[i + 1];
        if (!a.smooth || !b.smooth) continue;
        const double least = core::py_min(a.least, b.least);
        const double reach = core::py_max(width * 2, least * 0.3);
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
        if (body.outline.size() < 3) continue;  // (a balloon of no size has no tail)
        const P tip = at_px(core::subscript(tail, "to"));
        const double shortest = core::py_min(box.x1 - box.x0, box.y1 - box.y0);
        const Json* kind_value = core::dict_get(tail, "kind");
        const std::string tail_style = core::py_str(kind_value != nullptr && core::py_truthy(*kind_value) ? *kind_value : Json("wedge"));
        // (a slim tail: where it leaves the balloon, a little over a fifth of the balloon's narrow side)
        const Json* width_value = core::dict_get(tail, "width_mm");
        const double base = width_value != nullptr && core::py_truthy(*width_value)
                                ? static_cast<double>(px(core::to_float(core::subscript(tail, "width_mm")), dpi))
                                : core::py_max(4.0, shortest * (tail_style == "wedge" ? 0.22 : 0.2));
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
        spec.line = width;
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
        {  // (the join with the balloon rounded a little — half a millimetre, a line and a half at least, a quarter of
           // the tail's width at most; style fillet_mm says otherwise, 0 a corner)
            const Json* fillet = core::dict_get(tail, "fillet_mm");
            spec.fillet = fillet != nullptr && !fillet->is_null()
                              ? core::py_max(0.0, core::to_float(*fillet)) / 25.4 * dpi
                              : core::py_min(core::py_max(width * 1.5, 0.5 / 25.4 * dpi), base * 0.25);
        }
        clean::Tail made = clean::tail(body.outline, body.guide, body.centre, spec);
        if (spec.kind == "inward") made.shape.owner = static_cast<int>(index);
        built.corners.insert(built.corners.end(), made.corners.begin(), made.corners.end());
        shapes.push_back(std::move(made.shape));
        for (clean::Shape& join : made.joins) shapes.push_back(std::move(join));
        if (tail_style == "fade") fades.emplace_back(box, tip);
    }

    for (const Body& body : bodies) built.corners.insert(built.corners.end(), body.corners.begin(), body.corners.end());
    built.drips_from = shapes.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {  // (fukidashi@2: the drips)
        if (locals[i].y1 < whole_boxes[i].y1) {
            const Json lst = style_of(*lines[i]);
            // (the line's own width, before it was made whole pixels: a drip slimmer than it by the millimetre is so at every resolution)
            const Json& border = at(lst, "border_mm");
            const double nominal = (border.is_number() ? core::py_max(0.0, core::to_float(border)) : 0.35) / 25.4 * dpi;
            add_drips(shapes, drip_plan(*lines[i], lst), bodies[i], locals[i], whole_boxes[i].y1, width, nominal, dpi, &built.drips);
        }
    }
    return built;
}

Masks shape_masks(const std::vector<const core::StoryLine*>& lines, const std::vector<std::array<std::int64_t, 4>>& boxes,
                  const Tails& tails, const Box& region, int dpi, const Json& st) {
    const std::int64_t rx0 = region.x0, ry0 = region.y0;
    const Size size{region.width(), region.height()};
    const double width = line_width(st, dpi);
    const std::string kind = kind_of(*lines.front());
    const Built built = build_shapes(lines, boxes, tails, region, dpi, width);
    const std::vector<clean::Shape>& shapes = built.shapes;
    const std::vector<clean::Box>& locals = built.locals;
    const std::vector<clean::Box>& whole_boxes = built.whole_boxes;
    const std::vector<std::pair<clean::Box, P>>& fades = built.fades;
    // (fukidashi@2: a balloon that drips has its second line, the white a tone leaves inside its line and a tone box's
    // margin along its body and its tails alone — a drip's finger is too slim to hold them, and following it costs a
    // great deal for a thick line: the shapes without the drips, their edge and where they lie)
    const bool dripping = built.drips_from < shapes.size();

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
    std::vector<clean::Shape> body_shapes;
    std::vector<clean::Run> body_runs;
    clean::Plane body_fill;
    if (dripping) {
        body_shapes.assign(shapes.begin(), shapes.begin() + static_cast<std::ptrdiff_t>(built.drips_from));
        if (dashes) {
            const double on = core::py_max(width, mm_px(at(st, "dash_mm"), dpi, 1.1 / 25.4 * dpi));
            body_runs = clean::edge_runs(clean::dashed(body_shapes, on, core::py_max(width * 1.2, on * 0.6)));
        } else {
            body_runs = clean::edge_runs(body_shapes);
        }
        body_fill = clean::fill_plane(body_shapes, size.width, size.height);
    }
    // (what lies within `reach` of the edge of the balloons and their tails — the drips aside —, inside them)
    const auto within_body = [&](double reach, bool round_ends) {
        if (!dripping) return within(reach);
        clean::Plane plane = clean::ribbon_plane(body_runs, reach, size.width, size.height, round_ends);
        for (std::size_t i = 0; i < count; ++i) plane.values[i] *= body_fill.values[i];
        return plane;
    };
    std::vector<float> band(count, 0.0f);
    std::vector<float> inner = fill.values;  // (the fill inside the line: kept solid under a fading tail)
    if (width > 0) {
        if (const std::optional<clean::Pen> pen = pen_of(st, kind, width, dpi)) {
            // (fukidashi@2: the pen's line — its width swelling on one side, its strokes thinning toward their ends; a
            // dashed outline keeps its dashes and takes the swell alone)
            clean::Pen with = *pen;
            with.chain = !dashes;
            with.corners = built.corners;
            clean::Plane plane = clean::pen_plane(clean::pen_runs(runs, shapes, with), size.width, size.height);
            for (std::size_t i = 0; i < count; ++i) plane.values[i] *= fill.values[i];
            band = std::move(plane.values);
        } else {
            band = within(width).values;
        }
        for (std::size_t i = 0; i < count; ++i) inner[i] = fill.values[i] - band[i];
    }
    const auto lighter = [&](const std::vector<float>& more) {
        for (std::size_t i = 0; i < count; ++i) band[i] = std::max(band[i], more[i]);
    };
    if (width > 0 && (core::py_truthy(at(st, "double")) || kind == "fancy_box")) {
        // a second line inside the first: two widths in, one width wide
        const clean::Plane outer = within_body(width * 4, !dashes);
        const clean::Plane closer = within_body(width * 3, !dashes);
        std::vector<float> second(count);
        for (std::size_t i = 0; i < count; ++i) second[i] = std::max(0.0f, outer.values[i] - closer.values[i]);
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
    if (const std::optional<ToneSpec> tone = tone_spec(st)) {
        // (fukidashi@2: a tone inside any outline — the tails and the joined balloons with it, being cut to where the
        // group lies —, clear of the line by inset_mm when that is said)
        // (inset_mm is measured from the line's even width — a pen's heavy side takes some of it —, along the balloons
        // and their tails: a drip's finger has the tone up to its line)
        clean::Plane edge;
        if (tone->inset_mm > 0) {
            edge = clean::ribbon_plane(dripping ? clean::edge_runs(body_shapes) : clean::edge_runs(shapes), width + tone->inset_mm / 25.4 * dpi, size.width, size.height);
            if (dripping) {
                for (std::size_t i = 0; i < count; ++i) edge.values[i] *= body_fill.values[i];
            }
        }
        out.tone = Image::frombytes("L", size, tone_bytes(*tone, ramp_over(whole_boxes, tone->dir, tone->radial), size, dpi, rx0, ry0, cover,
                                                              tone->inset_mm > 0 ? &edge.values : nullptr));
    } else if (kind == "tone_box") {  // (a light dot tone laid inside, under the words)
        clean::Plane edge = clean::ribbon_plane(dripping ? clean::edge_runs(body_shapes) : clean::edge_runs(shapes), core::py_max(2.0, static_cast<double>(px(1.0, dpi))),
                                                size.width, size.height);
        if (dripping) {
            for (std::size_t i = 0; i < count; ++i) edge.values[i] *= body_fill.values[i];
        }
        clean::Plane dots = clean::union_plane(tone_dots(size, dpi, rx0, ry0), size.width, size.height);
        for (std::size_t i = 0; i < count; ++i) dots.values[i] *= cover[i] * (1.0f - edge.values[i]);
        out.tone = as_image(dots);
    }
    if (const Json& blend = at(st, "fill_gradient"); out.fill && blend.is_object() && at(blend, "to").is_array() && at(blend, "to").size() >= 3 &&
                                                     std::all_of(at(blend, "to").begin(), at(blend, "to").begin() + 3, [](const Json& c) { return c.is_number(); })) {
        // (fukidashi@2: the fill a blend from its own colour to another — three numbers, each kept within 0..255; anything
        // else is no blend)
        out.blend = true;
        out.blend_to.clear();
        for (std::size_t c = 0; c < 3; ++c) {
            out.blend_to.push_back(static_cast<std::int64_t>(core::py_max(0.0, core::py_min(255.0, core::to_float(at(blend, "to")[c])))));
        }
        out.ramp = ramp_over(whole_boxes, setting(blend, "dir", 90.0, -360.0, 360.0), core::py_truthy(at(blend, "radial")));
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
    double paper[3] = {255.0, 255.0, 255.0};
    double line[3] = {0.0, 0.0, 0.0};
    double first_paper[3] = {255.0, 255.0, 255.0};  // (fukidashi@2, a blend: the colours its ramp begins and ends in)
    double last_paper[3] = {255.0, 255.0, 255.0};
    for (std::size_t c = 0; c < 3; ++c) {
        if (c < masks.paper.size()) paper[c] = static_cast<double>(std::clamp<std::int64_t>(masks.paper[c], 0, 255));
        if (c < masks.line_rgb.size()) line[c] = static_cast<double>(std::clamp<std::int64_t>(masks.line_rgb[c], 0, 255));
        first_paper[c] = paper[c];
        last_paper[c] = c < masks.blend_to.size() ? static_cast<double>(std::clamp<std::int64_t>(masks.blend_to[c], 0, 255)) : paper[c];
    }
    // (the fill's colour at a pixel of the part: the blend is worked out from the pixel's place in the group's region, so a
    // part of the page is the page cut there)
    const auto blended = [&](std::size_t i, double* colour) {
        const double t = masks.ramp.at(static_cast<double>(from.x0 + static_cast<int>(i % static_cast<std::size_t>(w))) + 0.5,
                                       static_cast<double>(from.y0 + static_cast<int>(i / static_cast<std::size_t>(w))) + 0.5);
        for (std::size_t k = 0; k < 3; ++k) colour[k] = first_paper[k] + (last_paper[k] - first_paper[k]) * t;
    };
    if (mode != "RGB" && mode != "RGBA") {
        // (any other kind of picture: the fill through what it covers inside the line, then the line)
        if (masks.fill) {
            const double k = masks.opacity;
            const Image inside = chops::subtract(cover_part, band_part).point([k](int v) { return static_cast<int>(static_cast<double>(v) * k + 0.5); });
            if (masks.blend) {
                std::string colours(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3, '\0');
                for (std::size_t i = 0; i < static_cast<std::size_t>(w) * static_cast<std::size_t>(h); ++i) {
                    double colour[3];
                    blended(i, colour);
                    for (std::size_t c = 0; c < 3; ++c) colours[i * 3 + c] = static_cast<char>(static_cast<std::uint8_t>(clamp01(colour[c] / 255.0) * 255.0 + 0.5));
                }
                image.paste(Image::frombytes("RGB", Size{w, h}, colours), to, &inside);
            } else {
                image.paste(Ink::tuple(masks.paper), to, &inside);
            }
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
    const auto count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < count; ++i) {
        const auto band8 = static_cast<unsigned char>(band[i]);
        const auto cover8 = static_cast<unsigned char>(cover[i]);
        if (band8 == 0 && cover8 == 0) continue;
        if (masks.blend && band8 != 255) blended(i, paper);  // (the fill's colour here)
        if (tone.empty() || tone[i] == '\0') {  // (most of a balloon: all line, or all fill — the colour itself)
            const bool all_line = band8 == 255;
            const bool all_fill = band8 == 0 && cover8 == 255 && masks.fill && masks.opacity >= 1.0 && !masks.blend;
            if (all_line || all_fill) {
                char* whole = &pixels[i * bands];
                const double* colour = all_line ? line : paper;
                for (std::size_t k = 0; k < 3; ++k) whole[k] = static_cast<char>(static_cast<std::uint8_t>(colour[k]));
                if (bands == 4) whole[3] = static_cast<char>(static_cast<std::uint8_t>(255));
                continue;
            }
            if (band8 == 0 && !masks.fill) continue;  // (inside a balloon without a fill: the page as it is)
        }
        const double b = static_cast<double>(band8) / 255.0;
        const double c = static_cast<double>(cover8) / 255.0;
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
            // (what is held: a byte a pixel for where the group lies, for its line and — a tone box — for its tone; the
            // planes of coverage they were worked out from are gone by now)
            const auto bytes = [](const Image& mask) { return mask.empty() ? std::int64_t{0} : static_cast<std::int64_t>(mask.width()) * mask.height(); };
            remembered_masks().put(*key, masks, bytes(masks->cover) + bytes(masks->band) + bytes(masks->tone) + 256);
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

// --- a group's frame, and its traces ----------------------------------------------------------------------------------

namespace {

// What a group's shapes are worked out from: each line's box (px), the tails drawn (a thought without one trails its
// bubbles down and away), and the region they need — the boxes, the tails' tips and bends, 4 mm round them (px of the
// page; not yet cut to the page).
struct GroupFrame {
    std::vector<std::array<std::int64_t, 4>> boxes;
    Tails tails;
    std::int64_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

GroupFrame group_frame(const std::vector<const core::StoryLine*>& lines, int dpi, const Panels* panels) {
    GroupFrame frame;
    std::vector<std::array<std::int64_t, 4>>& boxes = frame.boxes;
    for (const core::StoryLine* ln : lines) boxes.push_back(box_px(*ln, dpi));
    Tails& tails = frame.tails;
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
    frame.x0 = *std::min_element(xs.begin(), xs.end()) - margin;
    frame.y0 = *std::min_element(ys.begin(), ys.end()) - margin;
    frame.x1 = *std::max_element(xs.begin(), xs.end()) + margin;
    frame.y1 = *std::max_element(ys.begin(), ys.end()) + margin;
    return frame;
}

}  // namespace

namespace balloon {

std::vector<PenTrace> pen_trace(const std::vector<const core::StoryLine*>& group, int dpi) {
    std::vector<PenTrace> out;
    if (group.empty()) return out;
    const std::string kind = kind_of(*group.front());
    if (kind == "sfx" || kind == "none" || kind == "picture") return out;
    const Json st = style_of(*group.front());
    const double width = line_width(st, dpi);
    const std::optional<clean::Pen> pen = width > 0 ? pen_of(st, kind, width, dpi) : std::nullopt;
    if (!pen) return out;
    const GroupFrame frame = group_frame(group, dpi, nullptr);
    const Box region{static_cast<int>(std::max<std::int64_t>(0, frame.x0)), static_cast<int>(std::max<std::int64_t>(0, frame.y0)), static_cast<int>(frame.x1),
                     static_cast<int>(frame.y1)};
    const Built built = build_shapes(group, frame.boxes, frame.tails, region, dpi, width);
    const bool dashes = kind == "whisper" || core::py_truthy(at(st, "dash_mm"));
    clean::Pen with = *pen;
    with.chain = !dashes;
    with.corners = built.corners;
    std::vector<clean::Run> runs;
    if (dashes) {
        const double on = core::py_max(width, mm_px(at(st, "dash_mm"), dpi, 1.1 / 25.4 * dpi));
        runs = clean::edge_runs(clean::dashed(built.shapes, on, core::py_max(width * 1.2, on * 0.6)));
    } else {
        runs = clean::edge_runs(built.shapes);
    }
    const double mm = 25.4 / dpi;
    for (const clean::PenRun& run : clean::pen_runs(runs, built.shapes, with)) {
        PenTrace trace;
        trace.closed = run.closed;
        for (std::size_t i = 0; i < run.points.size(); ++i) {
            trace.points_mm.push_back({(run.points[i].x + region.x0) * mm, (run.points[i].y + region.y0) * mm});
            trace.weight.push_back(run.reach[i] / width);
            trace.stroke_start.push_back(run.stroke_start[i]);
        }
        out.push_back(std::move(trace));
    }
    return out;
}

std::vector<DripTrace> drip_trace(const core::StoryLine& line, int dpi) {
    std::vector<DripTrace> out;
    const std::string kind = kind_of(line);
    if (kind == "sfx" || kind == "none" || kind == "picture") return out;
    const std::vector<const core::StoryLine*> group{&line};
    const Json st = style_of(line);
    const GroupFrame frame = group_frame(group, dpi, nullptr);
    const Box region{static_cast<int>(std::max<std::int64_t>(0, frame.x0)), static_cast<int>(std::max<std::int64_t>(0, frame.y0)), static_cast<int>(frame.x1),
                     static_cast<int>(frame.y1)};
    const Built built = build_shapes(group, frame.boxes, frame.tails, region, dpi, line_width(st, dpi));
    const double mm = 25.4 / dpi;
    // (from the body's box — the line's box at this resolution — to the page)
    const double x0 = static_cast<double>(frame.boxes.front()[0]) * mm, y0 = static_cast<double>(frame.boxes.front()[1]) * mm;
    for (const DripDrawn& drip : built.drips) out.push_back(DripTrace{x0 + drip.x_mm, drip.half_mm, y0 + drip.tip_mm});
    return out;
}

TonePrint tone_print_range(double dpi, double lpi, bool lines) {
    TonePrint out;
    const double pitch = core::py_max(2.0, dpi / core::py_max(1.0, lpi));
    if (lines) {  // (a stripe, and the white between two, two pixels wide at the least)
        out.lightest = core::py_min(0.9, 2.0 / pitch);
        out.darkest = core::py_max(0.0, core::py_min(0.9, 1.0 - 2.0 / pitch));
        return out;
    }
    // (a dot two pixels across at the least; the white left between four dots — as wide as the circle that fits there,
    // less the 0.6 px the dots gain when the page is made black and white — two pixels at the least)
    out.lightest = core::py_min(0.9, core::kPi / (pitch * pitch));
    out.darkest = 0.0;
    for (int percent = 90; percent >= 0; --percent) {
        const double density = percent / 100.0;
        if (pitch * 1.4142135623730951 - 2 * pitch * dot_radius(density) - 0.6 >= 2.0) {
            out.darkest = density;
            break;
        }
    }
    return out;
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
    // (fukidashi@2: the letters of the group lie over the tone of its first line, when that draws one — ToneUnder)
    const ToneUnder tone_under(kind != "picture" && kind != "sfx" && kind != "none" && fill_tone_draws(st));
    if (kind == "picture") {
        for (const core::StoryLine* line : lines) letters(part, *line, dpi, show_speaker, font_path, stop, unported, speakers, parts);
        return;
    }
    if (kind != "sfx" && kind != "none" && balloons) {
        const GroupFrame frame = group_frame(lines, dpi, panels);
        const std::int64_t rx0 = std::max<std::int64_t>(0, frame.x0);
        const std::int64_t ry0 = std::max<std::int64_t>(0, frame.y0);
        const std::int64_t rx1 = std::min<std::int64_t>(part.page.width, frame.x1);
        const std::int64_t ry1 = std::min<std::int64_t>(part.page.height, frame.y1);
        if (rx1 > rx0 && ry1 > ry0) {
            const Box region{static_cast<int>(rx0), static_cast<int>(ry0), static_cast<int>(rx1), static_cast<int>(ry1)};
            paint_shapes(part, lines, frame.boxes, frame.tails, region, dpi, st);
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
    remembered_tone_tiles().clear();
    remembered_sheets().clear();
    remembered_stretches().clear();
}

std::int64_t balloon_cache_bytes() {
    return layout_cache_bytes() + remembered_masks().used() + remembered_tone_tiles().used() + remembered_sheets().used() + remembered_stretches().used();
}

}  // namespace genko::render::text
