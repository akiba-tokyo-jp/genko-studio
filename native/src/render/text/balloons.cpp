// genko/balloons.py, the balloon side: tails_of, _ellipse_point, _wobbly, _uneven, _outline, _shape, _electric,
// _edge_point, _smooth_closed, _polyline_tail, _tail_polygon, _fade_mask, _erode, _turned, _draw_turned,
// _thought_trail, draw_group, _paint_shapes, _dotted_edge, _corner_marks, _tone_dots, _swell, _cuts_mask, _dashes,
// _flash_lines and draw_lines. (The letters, _paint_text and what it calls: lettering.cpp.)

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
#include "render/draw.hpp"
#include "render/npcompat.hpp"
#include "render/page.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"
#include "render/text/memo.hpp"

namespace genko::render::text {

using core::Json;
using balloon::Box4;
using balloon::Pt;

namespace {

constexpr double kTau = 6.283185307179586;  // math.tau
constexpr double kHandPower = 2.6;          // HAND_POWER
constexpr double kUnevenMost = 0.04;        // UNEVEN_MOST
const std::vector<std::int64_t> kOutline{20, 20, 20};   // OUTLINE
const std::vector<std::int64_t> kPaper{255, 255, 255};  // PAPER

bool one_of(std::string_view kind, std::initializer_list<std::string_view> kinds) {
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}
// SQUARE, (*SQUARE, "rounded"), NO_TAIL, UNEVEN
bool square(std::string_view kind) { return one_of(kind, {"box", "narration", "dotted_box", "tone_box", "fancy_box"}); }
bool square_or_rounded(std::string_view kind) { return square(kind) || kind == "rounded"; }
bool no_tail(std::string_view kind) {
    return one_of(kind, {"narration", "sfx", "none", "flash", "picture", "dotted_box", "tone_box", "fancy_box"});
}
bool uneven_kind(std::string_view kind) { return one_of(kind, {"speech", "thought", "whisper"}); }

// st[key] / st.get(key): the value, or null when it is not there
const Json& at(const Json& st, std::string_view key) {
    static const Json none;
    if (!st.is_object()) return none;
    const auto it = st.find(key);
    return it == st.end() ? none : *it;
}
// st.get(key, True)
bool get_true(const Json& st, std::string_view key) {
    if (!st.is_object() || !st.contains(key)) return true;
    return core::py_truthy(at(st, key));
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

std::vector<PointD> drawn(const std::vector<Pt>& points) {
    std::vector<PointD> out;
    out.reserve(points.size());
    for (const Pt& p : points) out.push_back(PointD{p[0], p[1]});
    return out;
}

BoxF box_f(const Box4& b) { return BoxF{b[0], b[1], b[2], b[3]}; }

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

// _shape(draw, kind, box, st, seed)
void shape(Draw& draw, std::string_view kind, const Box4& box, const Json& st, std::string_view seed) {
    const auto [x0, y0, x1, y1] = box;
    const double cx = (x0 + x1) / 2;
    const double cy = (y0 + y1) / 2;
    const double rx = (x1 - x0) / 2;
    const double ry = (y1 - y0) / 2;
    const double wobble = float_or(at(st, "wobble"), 0);
    if (wobble > 0) {
        if (const auto points = balloon::outline(kind, box)) {
            draw.polygon(drawn(balloon::wobbly(*points, wobble, core::py_min(rx, ry) * 2, seed)), Ink(255));
            return;
        }
    }
    if (square(kind)) {
        draw.rectangle(box_f(box), Ink(255));
    } else if (kind == "rounded") {
        draw.rounded_rectangle(box_f(box), core::py_min(rx, ry) * 0.6, Ink(255));
    } else if (kind == "cloud") {
        double bump = core::py_max(2.0, core::py_min(rx, ry) * 0.3);
        const double k = 1 - bump / core::py_max(1.0, core::py_min(rx, ry));
        const double perimeter = core::kPi * (rx + ry) * k;
        std::int64_t n = int_or_zero(at(st, "bumps"));
        if (n == 0) n = std::max<std::int64_t>(8, core::py_trunc_int(perimeter / (bump * 1.4)));
        // (a set number of bumps: each as big as its share of the edge)
        if (core::py_truthy(at(st, "bumps"))) bump = core::py_max(2.0, perimeter / static_cast<double>(n) / 1.4);
        draw.ellipse(BoxF{cx - rx * k, cy - ry * k, cx + rx * k, cy + ry * k}, Ink(255));
        for (std::int64_t i = 0; i < n; ++i) {
            const Pt b = balloon::ellipse_point(cx, cy, rx * k, ry * k, 2 * core::kPi * static_cast<double>(i) / static_cast<double>(n));
            draw.ellipse(BoxF{b[0] - bump, b[1] - bump, b[0] + bump, b[1] + bump}, Ink(255));
        }
    } else if (kind == "shout") {
        std::int64_t spikes = int_or_zero(at(st, "spikes"));
        if (spikes == 0) spikes = std::max<std::int64_t>(12, core::py_trunc_int((rx + ry) / core::py_max(4.0, core::py_min(rx, ry) / 3)));
        const double depth = core::py_max(0.05, core::py_min(0.6, float_or(at(st, "spike_depth"), 0.2)));
        const double jitter = core::py_max(0.0, core::py_min(1.0, float_or(at(st, "spike_jitter"), 0)));
        core::PyRandom rng = core::PyRandom::from_str(seed.empty() ? std::string_view("shout") : seed);
        std::vector<Pt> points;
        for (std::int64_t i = 0; i < spikes * 2; ++i) {
            const double t = core::kPi * static_cast<double>(i) / static_cast<double>(spikes);
            const double k = i % 2 == 0 ? 1.0 + (rng.uniform(-0.5, 0.5) * depth * 2 * jitter) : 1.0 - depth;
            points.push_back(balloon::ellipse_point(cx, cy, rx * k, ry * k, t));
        }
        draw.polygon(drawn(points), Ink(255));
    } else if (kind == "electric") {
        draw.polygon(drawn(balloon::electric(box, st)), Ink(255));
    } else if (uneven_kind(kind) && get_true(st, "hand")) {
        draw.polygon(drawn(balloon::uneven(box, seed)), Ink(255));
    } else {  // speech, thought, whisper, flash
        draw.ellipse(box_f(box), Ink(255));
    }
}

// _fade_mask(size, fades): 255 everywhere but near each fading tail's tip, falling to 0 at the tip (numpy float64:
// np.hypot is libm's hypot, the minimum over the tails, then (keep * 255).astype("uint8")).
Image fade_mask(Size size, const std::vector<std::pair<Box4, Pt>>& fades) {
    const auto w = static_cast<std::size_t>(size.width);
    const auto h = static_cast<std::size_t>(size.height);
    std::vector<double> keep(w * h, 1.0);
    for (const auto& [box, tip] : fades) {
        const double cx = (box[0] + box[2]) / 2;
        const double cy = (box[1] + box[3]) / 2;
        const double reach =
            core::py_max(1.0, core::py_dist(cx, cy, tip[0], tip[1]) - core::py_min(box[2] - box[0], box[3] - box[1]) / 2) * 0.7;
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const double d = np::hypot(static_cast<double>(x) - tip[0], static_cast<double>(y) - tip[1]);
                double v = d / reach;
                v = v < 0 ? 0.0 : (v > 1 ? 1.0 : v);  // np.clip(…, 0, 1)
                double& k = keep[y * w + x];
                if (v < k) k = v;  // np.minimum
            }
        }
    }
    std::string bytes(w * h, '\0');
    for (std::size_t i = 0; i < keep.size(); ++i) bytes[i] = static_cast<char>(static_cast<std::uint8_t>(keep[i] * 255));
    return Image::frombytes("L", size, bytes);
}

// _erode(mask, amount)
Image erode(Image mask, std::int64_t amount) {
    if (amount <= 0) return mask;
    if (amount <= 3) {
        for (std::int64_t i = 0; i < amount; ++i) mask = mask.filter(Filter::min_filter(3));
        return mask;
    }
    // an isotropic erosion: blur and keep what stays nearly solid (Φ(2) ≈ 0.977)
    return mask.filter(Filter::gaussian_blur(static_cast<double>(amount) / 2)).point([](int v) { return v >= 249 ? 255 : 0; });
}

// _dotted_edge(size, boxes, width): round dots along each box's edge, about three line-widths apart.
Image dotted_edge(Size size, const std::vector<Box4>& boxes, std::int64_t width) {
    Image out = Image::create("L", size, 0);
    Draw draw(out);
    const double r = core::py_max(1.0, static_cast<double>(width) * 0.9);
    const double step = core::py_max(3.0, static_cast<double>(width) * 3.2);
    for (const Box4& b : boxes) {
        const double x0 = b[0] + r, y0 = b[1] + r, x1 = b[2] - r, y1 = b[3] - r;
        const Pt edges[4][2] = {{{x0, y0}, {x1, y0}}, {{x1, y0}, {x1, y1}}, {{x1, y1}, {x0, y1}}, {{x0, y1}, {x0, y0}}};
        for (const auto& edge : edges) {
            const Pt a = edge[0];
            const Pt e = edge[1];
            const double length = core::py_dist(a[0], a[1], e[0], e[1]);
            const std::int64_t n = std::max<std::int64_t>(1, core::py_round_int(length / step));
            for (std::int64_t i = 0; i < n; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(n);
                const double cx = a[0] + (e[0] - a[0]) * t;
                const double cy = a[1] + (e[1] - a[1]) * t;
                draw.ellipse(BoxF{cx - r, cy - r, cx + r, cy + r}, Ink(255));
            }
        }
    }
    return out;
}

// _corner_marks(size, boxes, width): a small filled diamond on each corner of each box (a 飾り枠's corners).
Image corner_marks(Size size, const std::vector<Box4>& boxes, std::int64_t width) {
    Image out = Image::create("L", size, 0);
    Draw draw(out);
    const double d = static_cast<double>(width) * 3.5;
    for (const Box4& b : boxes) {
        for (const Pt c : {Pt{b[0], b[1]}, Pt{b[2], b[1]}, Pt{b[2], b[3]}, Pt{b[0], b[3]}}) {
            draw.polygon(drawn({{c[0], c[1] - d}, {c[0] + d, c[1]}, {c[0], c[1] + d}, {c[0] - d, c[1]}}), Ink(255));
        }
    }
    return out;
}

// _tone_dots(size, dpi, origin): a light dot tone (about 10 %), dots a millimetre apart, lined up with the page.
Image tone_dots(Size size, int dpi, std::int64_t ox, std::int64_t oy) {
    Image out = Image::create("L", size, 0);
    Draw draw(out);
    const double pitch = core::py_max(3.0, dpi / 25.4 * 1.0);
    const double r = core::py_max(0.5, pitch * 0.17);
    double y = -core::py_fmod(static_cast<double>(oy), pitch);
    std::int64_t row = 0;
    while (y < size.height + pitch) {
        double x = -core::py_fmod(static_cast<double>(ox), pitch) + (row % 2 != 0 ? pitch / 2 : 0);
        while (x < size.width + pitch) {
            draw.ellipse(BoxF{x - r, y - r, x + r, y + r}, Ink(255));
            x += pitch;
        }
        y += pitch;
        ++row;
    }
    return out;
}

// _swell(size, seed, wave): where a pen's line swells (white) and where it stays thin (black), soft blobs `wave` apart.
Image swell(Size size, std::string_view seed, double wave) {
    core::PyRandom rng = core::PyRandom::from_str("swell:" + std::string(seed.empty() ? std::string_view("genko") : seed));
    const std::int64_t cw = std::max<std::int64_t>(2, core::py_round_int(size.width / core::py_max(1.0, wave))) + 1;
    const std::int64_t ch = std::max<std::int64_t>(2, core::py_round_int(size.height / core::py_max(1.0, wave))) + 1;
    static const std::vector<int> choices{0, 0, 90, 255, 255};
    std::string cells(static_cast<std::size_t>(cw * ch), '\0');
    for (char& c : cells) c = static_cast<char>(rng.choice(choices));
    const Image small = Image::frombytes("L", Size{static_cast<int>(cw), static_cast<int>(ch)}, cells);
    return small.resize(size, Resample::Bicubic);
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

// _dashes(size, boxes, scale): angular stripes around each balloon's centre (a whisper's dashed outline).
Image dashes(Size size, const std::vector<Box4>& boxes, std::int64_t scale) {
    Image out = Image::create("L", size, 0);
    Draw draw(out);
    for (const auto& [x0, y0, x1, y1] : boxes) {
        const double cx = (x0 + x1) / 2;
        const double cy = (y0 + y1) / 2;
        const double r = core::py_max(x1 - x0, y1 - y0) * 2;
        const std::int64_t n = std::max<std::int64_t>(16, core::py_trunc_int((x1 - x0 + y1 - y0) / static_cast<double>(6 * scale)));
        for (std::int64_t i = 0; i < n * 2; i += 2) {
            const double a0 = core::kPi * static_cast<double>(i) / static_cast<double>(n);
            const double a1 = core::kPi * static_cast<double>(i + 1) / static_cast<double>(n);
            draw.polygon(drawn({{cx, cy},
                                {cx + r * core::py_cos(a0), cy + r * core::py_sin(a0)},
                                {cx + r * core::py_cos(a1), cy + r * core::py_sin(a1)}}),
                         Ink(255));
        }
    }
    return out;
}

// _flash_lines(size, boxes, width): radiating lines from just inside the balloon to outside it.
Image flash_lines(Size size, const std::vector<Box4>& boxes, std::int64_t width) {
    Image out = Image::create("L", size, 0);
    Draw draw(out);
    for (const auto& [x0, y0, x1, y1] : boxes) {
        const double cx = (x0 + x1) / 2;
        const double cy = (y0 + y1) / 2;
        const double rx = (x1 - x0) / 2;
        const double ry = (y1 - y0) / 2;
        const std::int64_t n = std::max<std::int64_t>(40, core::py_trunc_int((rx + ry) / 2));
        for (std::int64_t i = 0; i < n; ++i) {
            const double t = 2 * core::kPi * static_cast<double>(i) / static_cast<double>(n) + static_cast<double>(i % 3) * 0.013;
            const double inner = 0.93 + 0.04 * static_cast<double>((i * 7) % 5) / 5;
            const Pt a = balloon::ellipse_point(cx, cy, rx * inner, ry * inner, t);
            const Pt b = balloon::ellipse_point(cx, cy, rx * 1.18, ry * 1.18, t);
            draw.line(drawn({a, b}), Ink(255), static_cast<int>(std::max<std::int64_t>(1, width / 2)));
        }
    }
    return out;
}

// --- one group's shapes (_paint_shapes) ------------------------------------------------------------------------------

using Tails = std::vector<std::pair<const core::StoryLine*, Json>>;

// What _paint_shapes lays on the page over its region: the fill's mask (fill_opacity applied) and colour, a
// tone_box's dots, the outline's mask and colour. Each mask is the region's size.
struct Masks {
    bool fill = false;
    std::vector<std::int64_t> paper;
    Image shapes;
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

Masks shape_masks(const std::vector<const core::StoryLine*>& lines, const std::vector<std::array<std::int64_t, 4>>& boxes,
                  const Tails& tails, const Box& region, int dpi, const Json& st) {
    const std::int64_t rx0 = region.x0, ry0 = region.y0, rx1 = region.x1, ry1 = region.y1;
    const std::int64_t scale = dpi < 300 ? 2 : 1;  // draw small renders at twice the size for smooth edges
    const Size size{static_cast<int>((rx1 - rx0) * scale), static_cast<int>((ry1 - ry0) * scale)};
    Image mask = Image::create("L", size, 0);
    Image bubbles = Image::create("L", size, 0);
    std::vector<std::pair<Box4, Pt>> fades;
    const auto local = [&](const std::array<std::int64_t, 4>& b) {
        return Box4{static_cast<double>((b[0] - rx0) * scale), static_cast<double>((b[1] - ry0) * scale),
                    static_cast<double>((b[2] - rx0) * scale), static_cast<double>((b[3] - ry0) * scale)};
    };
    const auto at_px = [&](const Json& point) {
        return Pt{static_cast<double>((px(number(core::subscript(point, 0)), dpi) - rx0) * scale),
                  static_cast<double>((px(number(core::subscript(point, 1)), dpi) - ry0) * scale)};
    };
    {
        Draw draw(mask);
        Draw bdraw(bubbles);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const core::StoryLine& line = *lines[i];
            const Json lst = style_of(line);
            if (line.path && !line.path->empty()) {  // drawn by hand
                std::vector<Pt> outline;
                for (const core::Point& p : *line.path) {
                    outline.push_back(Pt{static_cast<double>((px(p.x.value(), dpi) - rx0) * scale),
                                         static_cast<double>((px(p.y.value(), dpi) - ry0) * scale)});
                }
                if (core::py_truthy(at(lst, "path_curve"))) outline = balloon::smooth_closed(outline);
                draw.polygon(drawn(outline), Ink(255));
            } else {
                shape(draw, kind_of(line), local(boxes[i]), lst, line.id);
            }
        }
        for (const auto& [line, tail] : tails) {
            const std::size_t index = static_cast<std::size_t>(std::find(lines.begin(), lines.end(), line) - lines.begin());
            const Box4 box = local(boxes[index]);
            const Pt tip = at_px(core::subscript(tail, "to"));
            const Json* via_value = core::dict_get(tail, "via");
            const std::optional<Pt> via = via_value != nullptr && core::py_truthy(*via_value) ? std::optional<Pt>(at_px(*via_value)) : std::nullopt;
            const double shortest = core::py_min(box[2] - box[0], box[3] - box[1]);
            const Json* kind_value = core::dict_get(tail, "kind");
            const std::string tail_style = core::py_str(kind_value != nullptr && core::py_truthy(*kind_value) ? *kind_value : Json("wedge"));
            // (a letterer's tail is broad where it leaves the balloon: about a third of its narrow side)
            const Json* width_value = core::dict_get(tail, "width_mm");
            const double base = width_value != nullptr && core::py_truthy(*width_value)
                                    ? static_cast<double>(px(core::to_float(core::subscript(tail, "width_mm")), dpi) * scale)
                                    : core::py_max(4.0, shortest / (tail_style == "wedge" ? 3.0 : 4.0));
            const std::string kind = kind_of(*line);
            const Json* vias = core::dict_get(tail, "vias");
            if (vias != nullptr && core::py_truthy(*vias)) {  // 折れ線のしっぽ
                std::vector<Pt> bends;
                for (const Json& v : core::iterate(*vias)) bends.push_back(at_px(v));
                draw.polygon(drawn(balloon::polyline_tail(kind, box, tip, bends, base)), Ink(255));
                continue;
            }
            if (kind == "thought" || tail_style == "bubbles") {
                // bubbles toward the speaker instead of a tail
                const double cx = (box[0] + box[2]) / 2;
                const double cy = (box[1] + box[3]) / 2;
                const double rx = (box[2] - box[0]) / 2;
                const double ry = (box[3] - box[1]) / 2;
                const double ang = core::py_atan2((tip[1] - cy) / core::py_max(ry, 1.0), (tip[0] - cx) / core::py_max(rx, 1.0));
                const Pt e = balloon::ellipse_point(cx, cy, rx, ry, ang);
                const double fracs[3] = {0.3, 0.6, 0.88};
                for (int k = 0; k < 3; ++k) {
                    const double r = core::py_max(2.0, core::py_min(rx, ry) * (0.2 - k * 0.05));
                    const double bx = e[0] + (tip[0] - e[0]) * fracs[k];
                    const double by = e[1] + (tip[1] - e[1]) * fracs[k];
                    bdraw.ellipse(BoxF{bx - r, by - r, bx + r, by + r}, Ink(255));
                }
                continue;
            }
            draw.polygon(drawn(balloon::tail_polygon(kind, box, tip, via, base, tail_style)), Ink(255));
            if (tail_style == "fade") fades.emplace_back(box, tip);
        }
    }
    const Json& border = at(st, "border_mm");
    const std::int64_t width = px(core::to_float(border.is_null() ? Json(0.35) : border), dpi) * scale;
    const std::string kind = kind_of(*lines.front());
    Image shapes = chops::lighter(mask, bubbles);
    const Image inside = chops::lighter(erode(mask, width), erode(bubbles, std::max<std::int64_t>(1, width * 2 / 3)));
    Image band = chops::subtract(shapes, inside);
    if (get_true(st, "hand") && kind != "whisper" && kind != "flash" && width >= 2) {
        // a pen's line: thinner here, fuller there, as the letterer's hand went round (from 0.6 to 1.5 of the width)
        const Image thin = chops::subtract(
            shapes, chops::lighter(erode(mask, std::max<std::int64_t>(1, core::py_round_int(static_cast<double>(width) * 0.6))),
                                   erode(bubbles, std::max<std::int64_t>(1, width * 2 / 5))));
        const Image full = chops::subtract(
            shapes, chops::lighter(erode(mask, std::max<std::int64_t>(2, core::py_round_int(static_cast<double>(width) * 1.5))), inside));
        const Image swelling = swell(size, lines.front()->id, static_cast<double>(std::max(size.width, size.height)) / 3);
        band = chops::lighter(thin, chops::multiply(full, swelling));
    }
    if (core::py_truthy(at(st, "double"))) {  // a second line inside the first
        const Image inner = erode(inside, std::max<std::int64_t>(2, width * 2));
        band = chops::lighter(band, chops::subtract(inner, erode(inner, std::max<std::int64_t>(1, width))));
    }
    if (!fades.empty()) {  // 消えるしっぽ: its outline and fill thin away toward the tip
        const Image keep = fade_mask(size, fades);
        band = chops::multiply(band, keep);
        shapes = chops::lighter(chops::multiply(shapes, keep), inside);
    }
    std::vector<Box4> locals;
    for (const auto& b : boxes) locals.push_back(local(b));
    if (kind == "whisper") band = chops::multiply(band, dashes(size, locals, scale));
    if (kind == "dotted_box") band = dotted_edge(size, locals, std::max<std::int64_t>(2, width));  // (round dots, not a line)
    if (kind == "fancy_box") {  // (a double line, and a small diamond on each corner)
        const Image inner = erode(inside, std::max<std::int64_t>(2, width * 2));
        band = chops::lighter(band, chops::subtract(inner, erode(inner, std::max<std::int64_t>(1, width))));
        band = chops::lighter(band, corner_marks(size, locals, std::max<std::int64_t>(2, width)));
    }
    if (kind == "flash") band = flash_lines(size, locals, width);
    if (const auto cut = cuts_mask(lines, size, rx0, ry0, dpi, scale)) {
        // フキダシ消しゴム: those parts of the balloon (its fill and its line) are gone
        const Image keep = chops::invert(*cut);
        shapes = chops::multiply(shapes, keep);
        band = chops::multiply(band, keep);
    }
    if (scale > 1) {
        const Size full{static_cast<int>(rx1 - rx0), static_cast<int>(ry1 - ry0)};
        shapes = shapes.resize(full, Resample::Lanczos);
        band = band.resize(full, Resample::Lanczos);
    }
    Masks out;
    out.fill = at(st, "fill") != Json("none");
    if (out.fill) {
        out.paper = colour_of(at(st, "fill_rgb"), kPaper);
        const Json& cover = at(st, "fill_opacity");
        if (!cover.is_null() && core::to_float(cover) < 1) {
            const double k = core::py_max(0.0, core::to_float(cover));
            shapes = shapes.point([k](int v) { return static_cast<int>(core::py_trunc_int(v * k)); });
        }
    }
    if (kind == "tone_box") {  // (a light dot tone laid inside, under the words)
        const Image inner = erode(shapes, std::max<std::int64_t>(2, px(1.0, dpi)));
        out.tone = chops::multiply(inner, tone_dots(inner.size(), dpi, rx0, ry0));
    }
    out.line_rgb = colour_of(at(st, "line_rgb"), kOutline);
    out.shapes = std::move(shapes);
    out.band = std::move(band);
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
    // (Python: area = image.crop(region); colours pasted through the masks; image.paste(area, region's corner). Each
    // pixel is its own: the part where the region and the image meet is painted the same.)
    const Box meet{std::max(region.x0, here.x0), std::max(region.y0, here.y0), std::min(region.x1, here.x1), std::min(region.y1, here.y1)};
    const Box from{meet.x0 - region.x0, meet.y0 - region.y0, meet.x1 - region.x0, meet.y1 - region.y0};
    const Box to{meet.x0 - part.origin.x, meet.y0 - part.origin.y, meet.x1 - part.origin.x, meet.y1 - part.origin.y};
    const bool all = from == Box{0, 0, region.width(), region.height()};
    if (masks->fill) {
        const Image shapes = all ? masks->shapes : masks->shapes.crop(from);
        part.image->paste(Ink::tuple(masks->paper), to, &shapes);
    }
    if (!masks->tone.empty()) {
        const Image tone = all ? masks->tone : masks->tone.crop(from);
        part.image->paste(Ink::tuple(masks->line_rgb), to, &tone);
    }
    const Image band = all ? masks->band : masks->band.crop(from);
    part.image->paste(Ink::tuple(masks->line_rgb), to, &band);
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
    return Json::array({ln.id, ln.text, ln.speaker, ln.frame_id ? Json(*ln.frame_id) : Json(), ln.x_mm.json(), ln.y_mm.json(),
                        ln.w_mm.json(), ln.h_mm.json(), ln.balloon,
                        ln.tail ? Json::array({ln.tail->x.json(), ln.tail->y.json()}) : Json(), ln.wrap, Json(ln.ruby_runs), path,
                        Json(ln.emphasis_runs), style_runs, detail::keyed_style(ln.style), Json(ln.tails)});
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
             std::stop_token stop, const Unported* unported, SpeakerFonts* speakers) {
    try {
        paint_text(*part.image, line, dpi, show_speaker, font_path, std::move(stop), part.origin, speakers);
    } catch (const NotYetPorted& e) {
        if (unported == nullptr || !*unported) throw;
        (*unported)(e);
    } catch (const core::PyUncaught& e) {
        if (unported == nullptr || !*unported || !needs_raqm(e)) throw;
        (*unported)(NotYetPorted("text_features"));
    }
}

// _draw_turned(image, lines, dpi, show_speaker, font_path, degrees): drawn upright on its own sheet, turned about its
// centre and laid on the page; the tails turned back first, so they still point where they were aimed.
void draw_turned(const PagePart& part, const std::vector<const core::StoryLine*>& lines, int dpi, bool show_speaker,
                 const std::optional<std::string>& font_path, double degrees, std::stop_token stop, SpeakerFonts* speakers) {
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
    const std::int64_t size = px(2 * reach, dpi);
    const std::int64_t at_x = core::py_round_int(left / 25.4 * dpi);
    const std::int64_t at_y = core::py_round_int(top / 25.4 * dpi);
    if (overlaps(Box{static_cast<int>(at_x), static_cast<int>(at_y), static_cast<int>(at_x + size), static_cast<int>(at_y + size)},
                 page_box(part))) {
        Json group = Json::array();
        for (const core::StoryLine* ln : lines) group.push_back(line_json(*ln));
        const std::optional<std::string> key =
            detail::memo_key(Json::array({"turned", dpi, font_path ? Json(*font_path) : Json(), group}));
        std::shared_ptr<const Sheet> sheet = key ? remembered_sheets().get(*key) : nullptr;
        if (!sheet) {
            Image upright_sheet = Image::create("RGBA", Size{static_cast<int>(size), static_cast<int>(size)}, Ink{0, 0, 0, 0});
            std::vector<core::StoryLine> twins;
            twins.reserve(lines.size());
            for (const core::StoryLine* ln : lines) {
                core::StoryLine twin = *ln;
                twin.style = Json::object();
                if (ln->style.is_object()) {
                    for (const auto& [k, v] : ln->style.items()) {
                        if (k != "rotate_deg") twin.style[k] = v;
                    }
                }
                twin.x_mm = core::Num(ln->x_mm.value() - left);
                twin.y_mm = core::Num(ln->y_mm.value() - top);
                if (ln->path && !ln->path->empty()) {
                    std::vector<core::Point> moved;
                    for (const core::Point& p : *ln->path) moved.push_back(core::Point{core::Num(p.x.value() - left), core::Num(p.y.value() - top)});
                    twin.path = std::move(moved);
                } else {
                    twin.path.reset();
                }
                std::vector<Json> tails;
                for (const Json& tail : balloon::tails_of(*ln)) {
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
                twins.push_back(std::move(twin));
            }
            std::vector<const core::StoryLine*> upright;
            for (const core::StoryLine& twin : twins) upright.push_back(&twin);
            draw_group(whole_page(upright_sheet), upright, dpi, false, font_path, nullptr, stop);
            const double middle = static_cast<double>(size) / 2;
            auto made = std::make_shared<Sheet>();
            made->turned = upright_sheet.rotate(-degrees, Resample::Bicubic, false, std::pair<double, double>{middle, middle});
            sheet = made;
            if (key) remembered_sheets().put(*key, sheet, size * size * 4);
        }
        part.image->paste(sheet->turned, Point{static_cast<int>(at_x - part.origin.x), static_cast<int>(at_y - part.origin.y)},
                          &sheet->turned);
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

Pt ellipse_point(double cx, double cy, double rx, double ry, double t) { return {cx + rx * core::py_cos(t), cy + ry * core::py_sin(t)}; }

std::vector<Pt> wobbly(const std::vector<Pt>& points, double amount, double size, std::string_view seed) {
    core::PyRandom rng = core::PyRandom::from_str(seed.empty() ? std::string_view("genko") : seed);
    struct Wave {
        double f, ph, a;
    };
    std::array<Wave, 3> waves{};
    for (Wave& w : waves) {
        w.f = rng.uniform(2, 5);
        w.ph = rng.uniform(0, kTau);
        w.a = rng.uniform(0.5, 1.0);
    }
    std::vector<double> xs;
    std::vector<double> ys;
    for (const Pt& p : points) {
        xs.push_back(p[0]);
        ys.push_back(p[1]);
    }
    const auto n = static_cast<double>(points.size());
    const double cx = core::py_float_sum(xs) / n;
    const double cy = core::py_float_sum(ys) / n;
    std::vector<Pt> out;
    for (std::size_t k = 0; k < points.size(); ++k) {
        const double t = kTau * static_cast<double>(k) / n;
        std::vector<double> terms;
        for (const Wave& w : waves) terms.push_back(w.a * core::py_sin(w.f * t + w.ph));
        const double push = core::py_float_sum(terms) / 3;
        const auto [x, y] = points[k];
        double d = core::py_hypot(x - cx, y - cy);
        if (d == 0) d = 1.0;
        const double shift = push * amount * size * 0.05;
        out.push_back({x + (x - cx) / d * shift, y + (y - cy) / d * shift});
    }
    return out;
}

std::vector<Pt> uneven(const Box4& box, std::string_view seed, int n) {
    const auto [x0, y0, x1, y1] = box;
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    core::PyRandom rng = core::PyRandom::from_str("uneven:" + std::string(seed.empty() ? std::string_view("genko") : seed));
    const double a1 = rng.uniform(0.012, 0.03);
    const double p1 = rng.uniform(0, kTau);
    const double a2 = rng.uniform(0.015, 0.035);
    const double p2 = rng.uniform(0, kTau);
    const double a3 = rng.uniform(0.0, 0.01);
    const double p3 = rng.uniform(0, kTau);
    const auto count = static_cast<double>(n);
    std::vector<double> drops;
    for (int k = 0; k < n; ++k) {
        const auto kk = static_cast<double>(k);
        drops.push_back(a1 * (1 + core::py_cos(kTau * kk / count - p1)) + a2 * (1 + core::py_cos(2 * kTau * kk / count - p2)) +
                        a3 * (1 + core::py_cos(3 * kTau * kk / count - p3)));
    }
    double most = drops.empty() ? 0.0 : drops.front();
    for (const double d : drops) most = core::py_max(most, d);
    if (most == 0) most = 1.0;
    const double squeeze = core::py_min(1.0, kUnevenMost / most);  // (never more than UNEVEN_MOST in from the box)
    std::vector<Pt> points;
    for (int k = 0; k < n; ++k) {
        const double t = kTau * static_cast<double>(k) / count;
        const double r = 1 - drops[static_cast<std::size_t>(k)] * squeeze;
        const double c = core::py_cos(t);
        const double s = core::py_sin(t);  // (a fuller oval than an ellipse: HAND_POWER)
        const double ex = std::copysign(core::py_pow(std::fabs(c), 2 / kHandPower), c);
        const double ey = std::copysign(core::py_pow(std::fabs(s), 2 / kHandPower), s);
        points.push_back({cx + rx * r * ex, cy + ry * r * ey});
    }
    return points;
}

std::optional<std::vector<Pt>> outline(std::string_view kind, const Box4& box, int n) {
    const auto [x0, y0, x1, y1] = box;
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    if (one_of(kind, {"speech", "thought", "whisper", "flash"})) {
        std::vector<Pt> out;
        for (int k = 0; k < n; ++k) out.push_back(ellipse_point(cx, cy, rx, ry, kTau * k / n));
        return out;
    }
    if (square_or_rounded(kind)) {
        const int per = n / 4;
        std::vector<Pt> pts;
        const Pt edges[4][2] = {{{x0, y0}, {x1, y0}}, {{x1, y0}, {x1, y1}}, {{x1, y1}, {x0, y1}}, {{x0, y1}, {x0, y0}}};
        for (const auto& edge : edges) {
            const auto [ax, ay] = edge[0];
            const auto [bx, by] = edge[1];
            for (int i = 0; i < per; ++i) pts.push_back({ax + (bx - ax) * i / per, ay + (by - ay) * i / per});
        }
        return pts;
    }
    return std::nullopt;
}

std::vector<Pt> electric(const Box4& box, const Json& st) {
    const auto [x0, y0, x1, y1] = box;
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    std::int64_t teeth = int_or_zero(at(st, "spikes"));
    if (teeth == 0) teeth = std::max<std::int64_t>(14, core::py_trunc_int((rx + ry) / core::py_max(3.0, core::py_min(rx, ry) / 4)));
    const double depth = core::py_max(0.04, core::py_min(0.4, float_or(at(st, "spike_depth"), 0.12)));
    std::vector<Pt> points;
    for (std::int64_t i = 0; i < teeth * 2; ++i) {
        // (the inner corners lag: a sawtooth, not a star)
        const double t = core::kPi * (static_cast<double>(i) + (i % 2 != 0 ? 0.35 : 0)) / static_cast<double>(teeth);
        const double c = core::py_cos(t);
        const double s = core::py_sin(t);
        // a superellipse of power 4: flat sides, round corners
        const double k = i % 2 == 0 ? 1.0 : 1.0 - depth;
        points.push_back({cx + rx * k * std::copysign(core::py_pow(std::fabs(c), 0.5), c),
                          cy + ry * k * std::copysign(core::py_pow(std::fabs(s), 0.5), s)});
    }
    return points;
}

std::pair<Pt, Pt> edge_point(std::string_view kind, const Box4& box, Pt toward, double spread) {
    const auto [x0, y0, x1, y1] = box;
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    double rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    const auto [tx, ty] = toward;
    if (square_or_rounded(kind)) {
        if (std::fabs(tx - cx) / core::py_max(rx, 1.0) > std::fabs(ty - cy) / core::py_max(ry, 1.0)) {
            const double ex = cx + (tx > cx ? rx : -rx) * 0.9;
            const double my = core::py_max(y0 + spread, core::py_min(y1 - spread, ty));
            return {Pt{ex, my - spread / 2}, Pt{ex, my + spread / 2}};
        }
        const double ey = cy + (ty > cy ? ry : -ry) * 0.9;
        const double mx = core::py_max(x0 + spread, core::py_min(x1 - spread, tx));
        return {Pt{mx - spread / 2, ey}, Pt{mx + spread / 2, ey}};
    }
    const double t = core::py_atan2((ty - cy) / core::py_max(ry, 1.0), (tx - cx) / core::py_max(rx, 1.0));
    const double k = kind == "shout" ? 0.72 : kind == "electric" ? 0.8 : 0.9;  // start inside (below the spikes' valleys)
    rx = rx * k / 0.9;
    ry = ry * k / 0.9;
    double lo = 0.0;
    double hi = core::kPi / 2;
    for (int i = 0; i < 24; ++i) {  // the half-angle whose chord is `spread`
        const double mid = (lo + hi) / 2;
        const Pt a = ellipse_point(cx, cy, rx * 0.9, ry * 0.9, t - mid);
        const Pt b = ellipse_point(cx, cy, rx * 0.9, ry * 0.9, t + mid);
        if (core::py_dist(a[0], a[1], b[0], b[1]) < spread) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return {ellipse_point(cx, cy, rx * 0.9, ry * 0.9, t - lo), ellipse_point(cx, cy, rx * 0.9, ry * 0.9, t + lo)};
}

std::vector<Pt> smooth_closed(const std::vector<Pt>& points, int per) {
    const std::size_t n = points.size();
    if (n < 3) return points;
    std::vector<Pt> out;
    for (std::size_t i = 0; i < n; ++i) {
        const Pt& p0 = points[(i + n - 1) % n];
        const Pt& p1 = points[i];
        const Pt& p2 = points[(i + 1) % n];
        const Pt& p3 = points[(i + 2) % n];
        for (int s = 0; s < per; ++s) {
            const double t = static_cast<double>(s) / per;
            const double t2 = t * t;
            const double t3 = t * t * t;
            Pt p{};
            for (int k = 0; k < 2; ++k) {
                p[k] = 0.5 * (2 * p1[k] + (-p0[k] + p2[k]) * t + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t2 +
                              (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * t3);
            }
            out.push_back(p);
        }
    }
    return out;
}

std::vector<Pt> polyline_tail(std::string_view kind, const Box4& box, Pt tip, const std::vector<Pt>& vias, double base) {
    const auto [p1, p2] = edge_point(kind, box, vias.front(), base);
    std::vector<Pt> path{Pt{(p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2}};
    path.insert(path.end(), vias.begin(), vias.end());
    path.push_back(tip);
    std::vector<double> lengths{0.0};
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        lengths.push_back(lengths.back() + core::py_dist(path[i][0], path[i][1], path[i + 1][0], path[i + 1][1]));
    }
    double total = lengths.back();
    if (total == 0) total = 1.0;
    const double half0 = core::py_dist(p1[0], p1[1], p2[0], p2[1]) / 2;
    std::vector<Pt> left;
    std::vector<Pt> right;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const Pt& p = path[i];
        const Pt& a = path[i == 0 ? 0 : i - 1];
        const Pt& b = path[std::min(path.size() - 1, i + 1)];
        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        double n = core::py_hypot(dx, dy);
        if (n == 0) n = 1.0;
        const double half = half0 * (1 - lengths[i] / total);
        left.push_back({p[0] - dy / n * half, p[1] + dx / n * half});
        right.push_back({p[0] + dy / n * half, p[1] - dx / n * half});
    }
    left.insert(left.end(), right.rbegin(), right.rend());
    return left;
}

std::vector<Pt> tail_polygon(std::string_view kind, const Box4& box, Pt tip, const std::optional<Pt>& via, double base,
                             std::string_view style) {
    const auto [p1, p2] = edge_point(kind, box, via ? *via : tip, base);
    const double mx = (p1[0] + p2[0]) / 2;
    const double my = (p1[1] + p2[1]) / 2;
    double cx = via ? (*via)[0] : (mx + tip[0]) / 2;
    double cy = via ? (*via)[1] : (my + tip[1]) / 2;
    const bool bow = style == "wedge" && !via && kind != "electric";
    if (bow) {  // (a drawn tail bows a little, away from the balloon's middle: a comma, not a needle)
        const double bx = (box[0] + box[2]) / 2;
        const double by = (box[1] + box[3]) / 2;
        const double dx = tip[0] - mx;
        const double dy = tip[1] - my;
        const double side = (dx * (my - by) - dy * (mx - bx)) >= 0 ? 1.0 : -1.0;
        const double nx = cx - dy * 0.16 * side;
        const double ny = cy + dx * 0.16 * side;
        cx = nx;
        cy = ny;
    }
    const double chord = core::py_dist(p1[0], p1[1], p2[0], p2[1]);
    std::vector<Pt> left;
    std::vector<Pt> right;
    const int steps = 16;
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        double x = core::py_pow(1 - t, 2) * mx + 2 * (1 - t) * t * cx + core::py_pow(t, 2) * tip[0];
        double y = core::py_pow(1 - t, 2) * my + 2 * (1 - t) * t * cy + core::py_pow(t, 2) * tip[1];
        const double dx = 2 * (1 - t) * (cx - mx) + 2 * t * (tip[0] - cx);
        const double dy = 2 * (1 - t) * (cy - my) + 2 * t * (tip[1] - cy);
        double n = core::py_hypot(dx, dy);
        if (n == 0) n = 1.0;
        // (full at the base, then quickly to a point)
        double half = core::py_dist(p1[0], p1[1], p2[0], p2[1]) / 2 * (bow ? core::py_pow(1 - t, 1.3) : (1 - t));
        if (style == "zigzag" && 0 < i && i < steps) half *= i % 2 != 0 ? 1.6 : 0.55;
        if (kind == "electric" && 0 < i && i < steps) {
            // a lightning tail: the centre line jumps sideways at a few steps
            const double jump = i % 4 == 0 ? ((i / 4) % 2 != 0 ? 1 : -1) * chord * 0.9 * (1 - t) : 0.0;
            const double nx = x - dy / n * jump;
            const double ny = y + dx / n * jump;
            x = nx;
            y = ny;
        }
        left.push_back({x - dy / n * half, y + dx / n * half});
        right.push_back({x + dy / n * half, y - dx / n * half});
    }
    left.insert(left.end(), right.rbegin(), right.rend());
    return left;
}

Pt turned(Pt point, Pt centre, double degrees) {
    const double a = degrees * (core::kPi / 180.0);  // math.radians
    const double dx = point[0] - centre[0];
    const double dy = point[1] - centre[1];
    return {centre[0] + dx * core::py_cos(a) - dy * core::py_sin(a), centre[1] + dx * core::py_sin(a) + dy * core::py_cos(a)};
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
                SpeakerFonts* speakers) {
    if (lines.empty()) return;
    std::optional<SpeakerFonts> own;
    if (speakers == nullptr) speakers = &own.emplace(stop);
    const core::StoryLine& first = *lines.front();
    const std::string kind = kind_of(first);
    const Json st = style_of(first);
    const Json& rotate = at(st, "rotate_deg");
    if (core::py_truthy(rotate) && std::fabs(core::to_float(rotate)) > 0.01) {
        // (its sheet is remembered as a whole: what it cannot draw leaves the whole group out, in draw_lines)
        draw_turned(part, lines, dpi, show_speaker, font_path, core::to_float(rotate), stop, speakers);
        return;
    }
    const Box here = page_box(part);
    const Box page_all{0, 0, part.page.width, part.page.height};
    for (const core::StoryLine* ln : lines) {  // 画像のフキダシ: the picture stretched over the box
        if (ln->balloon != "picture") continue;
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
        for (const core::StoryLine* line : lines) letters(part, *line, dpi, show_speaker, font_path, stop, unported, speakers);
        return;
    }
    if (kind != "sfx" && kind != "none") {
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
    for (const core::StoryLine* line : lines) letters(part, *line, dpi, show_speaker, font_path, stop, unported, speakers);
}

void draw_lines(const PagePart& part, const std::vector<const core::StoryLine*>& lines, int dpi, const std::optional<std::string>& font_path,
                bool show_speaker, const Panels* panels, std::stop_token stop, const Unported* unported) {
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
            draw_group(part, group, dpi, show_speaker, font_path, panels, stop, unported, &speakers);
        } catch (const NotYetPorted& e) {
            if (unported == nullptr || !*unported) throw;
            (*unported)(e);
        } catch (const core::PyUncaught& e) {  // (a turned balloon's letters: its sheet drawn whole or not at all)
            if (unported == nullptr || !*unported || !needs_raqm(e)) throw;
            (*unported)(NotYetPorted("text_features"));
        }
    }
}

void clear_balloon_cache() {
    clear_layout_cache();
    remembered_masks().clear();
    remembered_sheets().clear();
    remembered_stretches().clear();
}

}  // namespace genko::render::text
