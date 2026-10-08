// Python's genko/effects.py. Every expression keeps Python's order of operations and the random numbers are drawn in
// Python's order (random.Random, core::PyRandom), so the lines are Python's to the last bit.

#include "render/effects.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include "core/frames.hpp"
#include "core/limits.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/pyrandom.hpp"
#include "core/stroke_geom.hpp"
#include "render/brushes.hpp"
#include "render/draw.hpp"
#include "render/page.hpp"
#include "render/page_internal.hpp"

namespace genko::render::effects {

namespace {

using core::Json;
using core::kPi;
using core::py_cos;  // (math.cos: ValueError for an infinity, as in Python)
using core::py_max;
using core::py_min;
using core::py_sin;

constexpr double kDegToRad = kPi / 180.0;  // math.radians
constexpr double kFadeMm = 3.0;            // FADE_MM: how long a cut line takes to thin out to nothing

Size size_of(const Box& b) { return Size{b.width(), b.height()}; }
bool intersects(const Box& a, const Box& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
Box intersection(const Box& a, const Box& b) {
    return Box{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}
Box shifted(const Box& b, int dx, int dy) { return Box{b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy}; }

double dist(const XY& a, const XY& b) { return core::py_dist(a.x, a.y, b.x, b.y); }
double r3(double v) { return core::py_round(v, 3); }

bool is_str(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

// params.get(key, fallback)
Json pget(const Json& params, const char* key, const Json& fallback = Json()) { return core::py_get(params, key, fallback); }
double fget(const Json& params, const char* key, const Json& fallback) { return core::to_float(pget(params, key, fallback)); }

// for x, y in pairs: (float(x), float(y)) — each item unpacked into two
XY pair_of(const Json& item) {
    if (item.is_array() || item.is_string() || item.is_object()) {
        const std::vector<Json> values = core::iterate(item);
        if (values.size() != 2) {
            throw core::PyValueError(values.size() > 2 ? "too many values to unpack (expected 2)"
                                                       : "not enough values to unpack (expected 2, got " + std::to_string(values.size()) + ")");
        }
        return XY{core::to_float(values[0]), core::to_float(values[1])};
    }
    throw core::PyTypeError("cannot unpack non-iterable " + core::py_type_name(item) + " object");
}

// (float(p[0]), float(p[1]))
XY xy_of(const Json& p) {
    const double x = core::to_float(core::subscript(p, 0));
    const double y = core::to_float(core::subscript(p, 1));
    return XY{x, y};
}

// effects.area(effect, page): the effect's panel outline (mm) and its box.
std::pair<std::vector<XY>, std::array<double, 4>> area(const Json& effect, const core::Page& page) {
    const core::Frame* frame = nullptr;
    const Json frame_id = core::py_get(effect, "frame_id");
    if (core::py_truthy(frame_id) && frame_id.is_string()) frame = page.find_frame(frame_id.get_ref<const std::string&>());
    std::vector<XY> outline;
    if (frame != nullptr) {
        for (const core::Point& p : core::outline(*frame)) outline.push_back(XY{p.x.value(), p.y.value()});
    } else {
        const core::Rect b = page.bleed_rect_mm();  // without a panel: the whole page out to the bleed
        outline = {XY{b.x.value(), b.y.value()}, XY{(b.x + b.width).value(), b.y.value()},
                   XY{(b.x + b.width).value(), (b.y + b.height).value()}, XY{b.x.value(), (b.y + b.height).value()}};
    }
    double x0 = outline.front().x, y0 = outline.front().y, x1 = x0, y1 = y0;
    for (const XY& p : outline) {
        x0 = py_min(x0, p.x);
        y0 = py_min(y0, p.y);
        x1 = py_max(x1, p.x);
        y1 = py_max(y1, p.y);
    }
    return {outline, {x0, y0, x1 - x0, y1 - y0}};
}

XY centre_of(const Json& params, const std::array<double, 4>& box) {
    const Json c = pget(params, "center");
    if (core::py_truthy(c)) return xy_of(c);
    return XY{box[0] + box[2] / 2, box[1] + box[3] / 2};
}

XY inner_of(const Json& params, const std::array<double, 4>& box) {
    if (core::py_truthy(pget(params, "inner"))) {
        const XY r = pair_of(params.at("inner"));
        return XY{py_max(0.5, r.x), py_max(0.5, r.y)};
    }
    const double share = fget(params, "clear", Json(0.4));  // old books: the clear middle as a share of the panel
    return XY{py_max(0.5, box[2] / 2 * share), py_max(0.5, box[3] / 2 * share)};
}

// _ray_to(shape, centre, angle): how far from `centre` a ray at `angle` meets the outline `shape`.
double ray_to(const std::vector<XY>& shape, const XY& centre, double angle) {
    const double dx = py_cos(angle);
    const double dy = py_sin(angle);
    std::optional<double> best;
    for (std::size_t i = 0; i < shape.size(); ++i) {
        const XY& a = shape[i];
        const XY& b = shape[(i + 1) % shape.size()];
        const double ex = b.x - a.x;
        const double ey = b.y - a.y;
        const double den = dx * ey - dy * ex;
        if (std::fabs(den) < 1e-9) continue;
        const double t = ((a.x - centre.x) * ey - (a.y - centre.y) * ex) / den;
        const double u = ((a.x - centre.x) * dy - (a.y - centre.y) * dx) / den;
        if (t > 0 && 0 <= u && u <= 1 && (!best || t < *best)) best = t;
    }
    return best.value_or(5.0);
}

// _taper(params, default): 入り抜き
std::string taper_of(const Json& params, const std::string& fallback) {
    const Json value = pget(params, "taper", Json(true));
    if ((value.is_boolean() && value.get<bool>()) || is_str(value, "True")) return fallback;
    if (core::py_equals(value, Json(false)) || value.is_null() || is_str(value, "") || is_str(value, "none")) return "";
    if (is_str(value, "in") || is_str(value, "out") || is_str(value, "both")) return value.get<std::string>();
    return fallback;
}

// _jitter(params, what): 乱れ by kind; each falls back to the one `jitter`.
double jitter_of(const Json& params, const std::string& what) {
    const Json value = pget(params, ("jitter_" + what).c_str(), pget(params, "jitter", Json(0.25)));
    return py_max(0.0, py_min(1.0, core::to_float(value)));
}

// _slot(i, count, params, r): まとまり
std::optional<double> slot_of(std::int64_t i, std::int64_t count, const Json& params, double r) {
    const std::int64_t size = core::to_int_held(core::py_or(pget(params, "bundle", Json(1)), Json(1)));
    if (size <= 1) return std::nullopt;
    const double gap = py_max(0.0, py_min(0.95, fget(params, "bundle_gap", Json(0.5))));
    const auto groups = core::py_trunc_held(std::ceil(static_cast<double>(count) / static_cast<double>(size)));
    const std::int64_t b = i / size;  // (i >= 0, size > 1: Python's divmod)
    const std::int64_t k = i % size;
    const double spread = (1 - gap) / static_cast<double>(size);
    return (static_cast<double>(b) + gap / 2 + (static_cast<double>(k) + 0.5) * spread +
            (r - 0.5) * spread * jitter_of(params, "position") * 2) /
           static_cast<double>(groups);
}

// _line(a, b, taper, n)
std::vector<std::array<double, 3>> line_of(const XY& a, const XY& b, const std::string& taper, int n = 6) {
    std::vector<std::array<double, 3>> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(n - 1);
        double p = 1.0;
        if (taper == "out") {
            p = 0.03 + 0.97 * t;
        } else if (taper == "in") {
            p = 1 - t * 0.97;
        } else if (taper == "both") {
            p = 0.03 + 0.97 * py_sin(kPi * t);
        }
        out.push_back({r3(a.x + (b.x - a.x) * t), r3(a.y + (b.y - a.y) * t), r3(p)});
    }
    return out;
}

std::int64_t count_of(const Json& params, const char* key, std::int64_t fallback) {
    return core::checked_count(core::to_int_held(pget(params, key, Json(fallback))), core::limits::kEffectLines);
}

// _speed_along(params, rng, box): 流線 along a curve.
std::vector<Line> speed_along(const Json& params, core::PyRandom& rng, const std::array<double, 4>& box) {
    std::vector<XY> raw;
    for (const Json& p : core::iterate(params.at("path"))) raw.push_back(xy_of(p));
    std::vector<XY> dense{raw.front()};
    std::vector<XY> ext;
    ext.push_back(raw.front());
    ext.insert(ext.end(), raw.begin(), raw.end());
    ext.push_back(raw.back());
    for (std::size_t i = 1; i + 2 < ext.size(); ++i) {  // (a Catmull-Rom curve through the points: no sharp corners)
        const XY& p0 = ext[i - 1];
        const XY& p1 = ext[i];
        const XY& p2 = ext[i + 1];
        const XY& p3 = ext[i + 2];
        const auto n = core::checked_count(std::max<std::int64_t>(2, core::py_trunc_held(dist(p1, p2) / 1.5)));
        for (std::int64_t k = 1; k <= n; ++k) {
            const double t = static_cast<double>(k) / static_cast<double>(n);
            const auto coord = [&](double q0, double q1, double q2, double q3) {
                return 0.5 * (2 * q1 + (-q0 + q2) * t + (2 * q0 - 5 * q1 + 4 * q2 - q3) * t * t +
                              (-q0 + 3 * q1 - 3 * q2 + q3) * core::py_pow(t, 3.0));
            };
            dense.push_back(XY{coord(p0.x, p1.x, p2.x, p3.x), coord(p0.y, p1.y, p2.y, p3.y)});
        }
    }
    std::vector<double> lengths{0.0};
    for (std::size_t i = 0; i + 1 < dense.size(); ++i) lengths.push_back(lengths.back() + dist(dense[i], dense[i + 1]));
    const double total = lengths.back() != 0.0 ? lengths.back() : 1.0;
    const std::int64_t count = count_of(params, "count", 40);
    const double width = fget(params, "width_mm", Json(0.5));
    const double share = fget(params, "length", Json(0.7));
    const double jitter = fget(params, "jitter", Json(0.25));
    const double spread = fget(params, "spread_mm", Json(py_min(box[2], box[3]) * 0.5));
    const Json taper_value = pget(params, "taper", Json(true));
    const bool taper = !(taper_value.is_boolean() && !taper_value.get<bool>());

    struct At {
        double x, y, nx, ny;
    };
    const auto at = [&](double s) {
        s = py_max(0.0, py_min(total, s));
        std::size_t k = lengths.size() - 2;
        for (std::size_t i = 0; i + 1 < lengths.size(); ++i) {
            if (lengths[i + 1] >= s) {
                k = i;
                break;
            }
        }
        double seg = lengths[k + 1] - lengths[k];
        if (seg == 0.0) seg = 1.0;
        const double t = (s - lengths[k]) / seg;
        const XY& a = dense[k];
        const XY& b = dense[k + 1];
        double d = core::py_hypot(b.x - a.x, b.y - a.y);
        if (d == 0.0) d = 1.0;
        return At{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, -(b.y - a.y) / d, (b.x - a.x) / d};
    };

    std::vector<Line> out;
    for (std::int64_t i = 0; i < count; ++i) {
        const double offset = spread * ((static_cast<double>(i) + rng.random()) / static_cast<double>(count) - 0.5);
        const double length = total * share * (1 - jitter * rng.random() * 0.8);
        const double start = (total - length) * rng.random();
        const auto steps = core::checked_count(std::max<std::int64_t>(8, core::py_trunc_held(length / 2)));
        Line line;
        for (std::int64_t k = 0; k < steps; ++k) {
            const double t = static_cast<double>(k) / static_cast<double>(steps - 1);
            const At q = at(start + length * t);
            const double p = taper ? 0.03 + 0.97 * py_sin(kPi * t) : 1.0;
            line.points.push_back({r3(q.x + q.nx * offset), r3(q.y + q.ny * offset), r3(p)});
        }
        line.width_mm = width * (0.5 + rng.random());
        out.push_back(std::move(line));
    }
    return out;
}

// --- keeping clear of faces (avoid) and inside a selection (within) ---------------------------------------------------

std::pair<std::vector<Shape>, std::optional<std::vector<XY>>> clearing(const Json& params) {
    std::vector<Shape> shapes;
    for (const Json& shape : core::iterate(core::py_or(pget(params, "avoid"), Json::array()))) {
        if (shape.is_object() && core::py_truthy(core::py_get(shape, "ellipse"))) {
            const std::vector<Json> values = core::iterate(shape.at("ellipse"));
            if (values.size() != 4) {
                throw core::PyValueError(values.size() > 4 ? "too many values to unpack (expected 4)"
                                                           : "not enough values to unpack (expected 4, got " + std::to_string(values.size()) + ")");
            }
            Shape s;
            s.ellipse = true;
            for (std::size_t i = 0; i < 4; ++i) s.box[i] = core::to_float(values[i]);
            if (s.box[2] > 0 && s.box[3] > 0) shapes.push_back(std::move(s));
        } else if (shape.is_object() && core::length(core::py_or(core::py_get(shape, "path"), Json::array())) >= 3) {
            Shape s;
            for (const Json& p : core::iterate(shape.at("path"))) s.path.push_back(xy_of(p));
            shapes.push_back(std::move(s));
        }
    }
    std::vector<XY> within;
    for (const Json& p : core::iterate(core::py_or(pget(params, "within"), Json::array()))) within.push_back(xy_of(p));
    if (within.size() >= 3) return {shapes, within};
    return {shapes, std::nullopt};
}

bool in_polygon(double x, double y, const std::vector<XY>& poly) {
    bool inside = false;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const XY& a = poly[i];
        const XY& b = poly[(i + 1) % poly.size()];
        const double dy = b.y - a.y;
        if ((a.y > y) != (b.y > y) && x < a.x + (b.x - a.x) * (y - a.y) / (dy != 0.0 ? dy : 1e-12)) inside = !inside;
    }
    return inside;
}

bool clear_at(double x, double y, const std::vector<Shape>& avoid, const std::optional<std::vector<XY>>& within) {
    if (within && !in_polygon(x, y, *within)) return false;
    for (const Shape& s : avoid) {
        if (s.ellipse) {
            const auto& [cx, cy, rx, ry] = s.box;
            if (core::py_pow((x - cx) / rx, 2.0) + core::py_pow((y - cy) / ry, 2.0) <= 1) return false;
        } else if (in_polygon(x, y, s.path)) {
            return false;
        }
    }
    return true;
}

// _keep_clear(lines, avoid, within): each line cut where it enters a shape to keep clear of (or leaves `within`), the
// part left thinning out toward the cut; pieces too short to read are dropped.
std::vector<Line> keep_clear(const std::vector<Line>& lines, const std::vector<Shape>& avoid,
                             const std::optional<std::vector<XY>>& within) {
    std::vector<Line> out;
    for (const Line& line : lines) {
        const auto& pts = line.points;
        std::vector<std::array<double, 3>> dense;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            const auto& a = pts[i];
            const auto& b = pts[i + 1];
            const auto n = core::checked_count(std::max<std::int64_t>(1, core::py_trunc_held(core::py_dist(a[0], a[1], b[0], b[1]) / 0.4)));
            for (std::int64_t k = 0; k < n; ++k) {
                const double t = static_cast<double>(k) / static_cast<double>(n);
                dense.push_back({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t});
            }
        }
        dense.push_back(pts.back());
        std::vector<bool> keep;
        keep.reserve(dense.size());
        for (const auto& p : dense) keep.push_back(clear_at(p[0], p[1], avoid, within));
        if (std::all_of(keep.begin(), keep.end(), [](bool v) { return v; })) {
            out.push_back(line);
            continue;
        }
        std::size_t i = 0;
        while (i < dense.size()) {
            if (!keep[i]) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j + 1 < dense.size() && keep[j + 1]) ++j;
            std::vector<std::array<double, 3>> piece(dense.begin() + static_cast<std::ptrdiff_t>(i),
                                                     dense.begin() + static_cast<std::ptrdiff_t>(j + 1));
            std::vector<double> parts;
            for (std::size_t q = 0; q + 1 < piece.size(); ++q) {
                parts.push_back(core::py_dist(piece[q][0], piece[q][1], piece[q + 1][0], piece[q + 1][1]));
            }
            const double length = core::py_float_sum(parts);
            if (length >= 1.5) {
                const double fade = py_min(kFadeMm, length * 0.45);
                const bool cuts[2][2] = {{i > 0, false}, {j < dense.size() - 1, true}};
                for (const auto& cut : cuts) {
                    if (!cut[0]) continue;
                    const bool cut_end = cut[1];
                    double walk = 0.0;
                    const std::size_t n = piece.size();
                    const auto at = [&](std::size_t q) -> std::array<double, 3>& { return cut_end ? piece[n - 1 - q] : piece[q]; };
                    std::array<double, 3> prev = at(0);
                    for (std::size_t q = 0; q < n; ++q) {
                        std::array<double, 3>& p = at(q);
                        walk += core::py_dist(prev[0], prev[1], p[0], p[1]);
                        prev = p;
                        if (walk >= fade) break;
                        p[2] = p[2] * (0.03 + 0.97 * walk / fade);
                    }
                }
                Line cut = line;
                cut.points.clear();
                for (const auto& p : piece) cut.points.push_back({r3(p[0]), r3(p[1]), r3(p[2])});
                out.push_back(std::move(cut));
            }
            i = j + 1;
        }
    }
    return out;
}

}  // namespace

std::pair<std::vector<XY>, std::array<double, 4>> panel_area(const Json& effect, const core::Page& page) { return area(effect, page); }

XY centre(const Json& params, const std::array<double, 4>& box) { return centre_of(params, box); }

XY inner_size(const Json& params, const std::array<double, 4>& box) { return inner_of(params, box); }

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> k{"focus", "speed", "uni_flash", "beta_flash", "white"};
    return k;
}

void validate(const Json& kind, const Json& params) {
    const auto& all = kinds();
    if (!kind.is_string() || std::find(all.begin(), all.end(), kind.get<std::string>()) == all.end()) {
        std::string names;
        for (const auto& k : all) names += (names.empty() ? "" : ", ") + k;
        throw core::PyValueError("kind must be one of " + names);
    }
    for (const Json& shape : core::iterate(core::py_or(pget(params, "avoid"), Json::array()))) {
        bool good = false;
        if (shape.is_object()) {
            const Json ellipse = core::py_get(shape, "ellipse");
            const Json path = core::py_get(shape, "path");
            good = (ellipse.is_array() && ellipse.size() == 4) || (path.is_array() && path.size() >= 3);
        }
        if (!good) throw core::PyValueError("avoid is a list of {\"ellipse\": [cx, cy, rx, ry]} or {\"path\": [[x, y], …]} (3 points or more)");
    }
    if (!pget(params, "within").is_null() && core::length(core::py_or(pget(params, "within"), Json::array())) < 3) {
        throw core::PyValueError("within is a shape of 3 points or more");
    }
    if (core::to_int_held(core::py_or(pget(params, "count", Json(1)), Json(1))) > 2000 ||
        core::to_int_held(core::py_or(pget(params, "spikes", Json(1)), Json(1))) > 2000) {
        throw core::PyValueError("too many lines (at most 2000)");
    }
    const Json taper = pget(params, "taper");
    const bool taper_ok = taper.is_null() || core::py_equals(taper, Json(true)) || core::py_equals(taper, Json(false)) ||
                          is_str(taper, "True") || is_str(taper, "") || is_str(taper, "none") || is_str(taper, "in") ||
                          is_str(taper, "out") || is_str(taper, "both");
    if (!taper_ok) throw core::PyValueError("taper is in, out, both or false");
    const Json bundle = pget(params, "bundle");
    if (!bundle.is_null()) {
        const std::int64_t n = core::to_int_held(bundle);
        if (!(1 <= n && n <= 50)) throw core::PyValueError("bundle is 1 to 50 lines");
    }
    for (const char* key : {"jitter", "depth", "length", "jitter_length", "jitter_position", "jitter_width"}) {
        if (params.is_object() && params.contains(key)) {
            const double v = core::to_float(params[key]);
            if (!(0 <= v && v <= 1)) throw core::PyValueError(std::string(key) + " is 0 to 1");
        }
    }
}

Geometry geometry(const Json& effect, const core::Page& page) {
    const Json kind = core::py_get(effect, "kind");
    const Json params = core::py_dict(core::py_or(core::py_get(effect, "params"), Json::object()));
    const auto [outline, box] = area(effect, page);
    const double x = box[0], y = box[1], w = box[2], h = box[3];
    core::PyRandom rng = core::PyRandom::from_str(core::py_str(pget(params, "seed", core::py_get(effect, "id"))));
    Geometry geo;
    for (const Json& v : core::iterate(core::py_or(pget(params, "rgb"), Json::array({15, 15, 15})))) geo.rgb.push_back(core::to_int_held(v));
    const double jitter = fget(params, "jitter", Json(0.25));
    if (is_str(kind, "focus")) {
        const XY c = centre_of(params, box);
        const XY inner = inner_of(params, box);
        const std::int64_t count = count_of(params, "count", 90);
        const double width = fget(params, "width_mm", Json(0.8));
        const double outer = core::py_hypot(w, h) + core::py_hypot(c.x - (x + w / 2), c.y - (y + h / 2));
        const std::string taper = taper_of(params, "in");
        std::vector<XY> shape;
        for (const Json& p : core::iterate(core::py_or(pget(params, "inner_path"), Json::array()))) shape.push_back(xy_of(p));
        const double twist = fget(params, "twist", Json(0)) * kDegToRad;
        const bool own = params.contains("jitter_length") || params.contains("jitter_position") || params.contains("jitter_width");
        for (std::int64_t i = 0; i < count; ++i) {
            const double r = rng.random();
            const auto slot = slot_of(i, count, params, r);
            const double di = static_cast<double>(i);
            double a = 0.0;
            if (slot) {
                a = 2 * kPi * *slot;
            } else if (own) {
                a = 2 * kPi * (di + 0.5 + (r - 0.5) * jitter_of(params, "position") * 2) / static_cast<double>(count);
            } else {
                a = 2 * kPi * (di + r * 0.7) / static_cast<double>(count);
            }
            const double jl = jitter_of(params, "length");
            const double stop = 1 + jl * rng.random() * 1.2;
            XY inner_pt;
            if (shape.size() >= 3) {
                const double reach = ray_to(shape, c, a);
                inner_pt = XY{c.x + reach * stop * py_cos(a), c.y + reach * stop * py_sin(a)};
            } else {
                inner_pt = XY{c.x + inner.x * stop * py_cos(a), c.y + inner.y * stop * py_sin(a)};
            }
            XY outer_pt{c.x + outer * py_cos(a), c.y + outer * py_sin(a)};
            if (core::py_truthy(pget(params, "length_mm"))) {  // (線の長さ: from where it stops, straight out from the middle)
                double away = dist(c, inner_pt);
                if (away == 0.0) away = 1.0;
                const double ux = (inner_pt.x - c.x) / away;
                const double uy = (inner_pt.y - c.y) / away;
                const double length = core::to_float(params.at("length_mm"));
                outer_pt = XY{inner_pt.x + ux * length, inner_pt.y + uy * length};
            }
            Line line;
            line.points = line_of(outer_pt, inner_pt, taper, twist != 0.0 ? 16 : 6);
            if (twist != 0.0) {  // a swirl: each point turned about the centre, less toward the middle
                const std::size_t n = line.points.size();
                for (std::size_t k = 0; k < n; ++k) {
                    auto& p = line.points[k];
                    const double turn = twist * core::py_pow(1 - static_cast<double>(k) / static_cast<double>(n - 1), 2.0);
                    const double dx = p[0] - c.x;
                    const double dy = p[1] - c.y;
                    p[0] = r3(c.x + dx * py_cos(turn) - dy * py_sin(turn));
                    p[1] = r3(c.y + dx * py_sin(turn) + dy * py_cos(turn));
                }
            }
            const double spread = rng.random();
            const double thick = own ? width * (1 + jitter_of(params, "width") * (spread * 2 - 1) * 1.6) : width * (0.6 + spread * 0.8);
            line.width_mm = py_max(0.02, thick);
            geo.lines.push_back(std::move(line));
        }
    } else if (is_str(kind, "speed") && core::length(core::py_or(pget(params, "path"), Json::array())) >= 2) {
        geo.lines = speed_along(params, rng, box);
    } else if (is_str(kind, "speed")) {
        const double angle = fget(params, "angle", Json(0)) * kDegToRad;
        const XY d{py_cos(angle), py_sin(angle)};
        const XY n{-d.y, d.x};
        std::int64_t count = count_of(params, "count", 40);
        const double width = fget(params, "width_mm", Json(0.5));
        const double share = fget(params, "length", Json(0.7));
        const double curve = fget(params, "curve", Json(0));
        const double cx = x + w / 2;
        const double cy = y + h / 2;
        std::vector<double> across, along;
        for (const XY& p : outline) {
            const double qx = p.x - cx;
            const double qy = p.y - cy;
            across.push_back(qx * n.x + qy * n.y);
            along.push_back(qx * d.x + qy * d.y);
        }
        double lo = across.front(), hi = across.front(), a0 = along.front(), a1 = along.front();
        for (const double v : across) {
            lo = py_min(lo, v);
            hi = py_max(hi, v);
        }
        for (const double v : along) {
            a0 = py_min(a0, v);
            a1 = py_max(a1, v);
        }
        const double span = a1 - a0;
        const std::string taper = taper_of(params, "both");
        if (core::py_truthy(pget(params, "spacing_mm"))) {  // (線の間隔 instead of how many)
            count = std::max<std::int64_t>(
                2, std::min<std::int64_t>(2000, core::py_trunc_held((hi - lo) / py_max(0.2, core::to_float(params.at("spacing_mm"))))));
        }
        const bool own = params.contains("jitter_length") || params.contains("jitter_position") || params.contains("jitter_width");
        for (std::int64_t i = 0; i < count; ++i) {
            const double r = rng.random();
            const auto slot = slot_of(i, count, params, r);
            const double di = static_cast<double>(i);
            double offset = 0.0;
            if (slot) {
                offset = lo + (hi - lo) * *slot;
            } else if (own) {
                offset = lo + (hi - lo) * (di + 0.5 + (r - 0.5) * jitter_of(params, "position") * 2) / static_cast<double>(count);
            } else {
                offset = lo + (hi - lo) * (di + r) / static_cast<double>(count);
            }
            const double jl = jitter_of(params, "length");
            const double length = span * share * (1 - jl * rng.random() * 0.8);
            const double start = a0 - span * 0.05 + (span * 1.1 - length) * rng.random();
            Line line;
            const int steps = curve != 0.0 ? 24 : 8;
            for (int k = 0; k < steps; ++k) {
                const double t = static_cast<double>(k) / static_cast<double>(steps - 1);
                const double along_t = start + length * t;
                const double bow = curve * 4 * t * (1 - t);
                const double px = cx + d.x * along_t + n.x * (offset + bow);
                const double py = cy + d.y * along_t + n.y * (offset + bow);
                double p = 1.0;
                if (taper == "both") {
                    p = 0.03 + 0.97 * py_sin(kPi * t);
                } else if (taper == "in") {
                    p = 1 - 0.97 * t;
                } else if (taper == "out") {
                    p = 0.03 + 0.97 * t;
                }
                line.points.push_back({r3(px), r3(py), r3(p)});
            }
            const double spread = rng.random();
            const double thick = own ? width * (1 + jitter_of(params, "width") * (spread * 2 - 1) * 1.6) : width * (0.5 + spread);
            line.width_mm = py_max(0.02, thick);
            geo.lines.push_back(std::move(line));
        }
    } else if (is_str(kind, "uni_flash")) {
        const XY c = centre_of(params, box);
        const XY inner = inner_of(params, box);
        const std::int64_t count = count_of(params, "count", 140);
        const double width = fget(params, "width_mm", Json(0.35));
        const double length = fget(params, "length_mm", Json(py_max(8.0, py_min(w, h) * 0.18)));
        for (std::int64_t i = 0; i < count; ++i) {
            const double a = 2 * kPi * (static_cast<double>(i) + rng.random() * 0.8) / static_cast<double>(count);
            const double start = 1 + jitter * (rng.random() - 0.5) * 0.3;
            const double size = length * (1 - jitter * rng.random() * 0.7);
            const XY p0{c.x + inner.x * start * py_cos(a), c.y + inner.y * start * py_sin(a)};
            const double k = size / py_max(1e-6, core::py_hypot(inner.x * py_cos(a), inner.y * py_sin(a)));
            const XY p1{p0.x + inner.x * k * py_cos(a), p0.y + inner.y * k * py_sin(a)};
            Line line;
            line.points = line_of(p0, p1, "both");
            line.width_mm = width * (0.7 + rng.random() * 0.6);
            geo.lines.push_back(std::move(line));
        }
    } else if (is_str(kind, "beta_flash")) {
        const XY c = centre_of(params, box);
        const XY inner = inner_of(params, box);
        const std::int64_t spikes = count_of(params, "spikes", 70);
        const double depth = fget(params, "depth", Json(0.45));
        geo.fills.push_back(Fill{outline, geo.rgb});
        Fill star;
        const double reach = core::py_hypot(w, h) / 2;
        for (std::int64_t i = 0; i < spikes * 2; ++i) {
            const double a = kPi * static_cast<double>(i) / static_cast<double>(spikes);
            double r = 0.0;
            if (i % 2 != 0) {  // a white spike's tip, out in the black
                r = 1 + (reach / py_max(inner.x, inner.y) - 1) * depth * (0.55 + rng.random() * 0.9 * (0.5 + jitter));
            } else {
                r = 1 + jitter * 0.15 * rng.random();
            }
            star.points.push_back(XY{r3(c.x + inner.x * r * py_cos(a)), r3(c.y + inner.y * r * py_sin(a))});
        }
        star.rgb = {255, 255, 255};
        geo.fills.push_back(std::move(star));
    } else if (is_str(kind, "white")) {
        geo.fills.push_back(Fill{outline, {255, 255, 255}});
    }
    auto [avoid, within] = clearing(params);
    if (!avoid.empty() || within) geo.lines = keep_clear(geo.lines, avoid, within);
    geo.outline = outline;
    geo.avoid = std::move(avoid);
    geo.within = std::move(within);
    return geo;
}

bool drawn(const Json& effect) {
    const Json kind = core::py_get(effect, "kind");
    const auto& all = kinds();
    if (!kind.is_string() || std::find(all.begin(), all.end(), kind.get<std::string>()) == all.end()) return false;
    return core::py_truthy(core::py_get(effect, "visible", Json(true)));
}

Image draw(Image image, const Json& effect, const core::Page& page, int dpi, Size size, const Box& box) {
    const Geometry geo = geometry(effect, page);
    const auto px = [&](const XY& p) { return detail::xy_point(p.x, p.y, dpi); };
    Image layer = Image::create("RGBA", size_of(box), Ink{0, 0, 0, 0});
    if (!geo.fills.empty()) {
        PageCanvas canvas(layer, box, size);
        for (const Fill& fill : geo.fills) {
            std::vector<PointD> pts;
            for (const XY& p : fill.points) pts.push_back(px(p));
            canvas.draw().polygon(pts, Ink::with_alpha(fill.rgb, 255));
        }
        canvas.commit();
    }
    const Image colour = Image::create("RGBA", size_of(box), Ink::with_alpha(geo.rgb, 255));
    Image cover = Image::create("L", size_of(box), Ink(0));
    for (const Line& line : geo.lines) {
        core::PenPoints points;
        points.reserve(line.points.size());
        for (const auto& p : line.points) points.push_back(core::PenPoint{p[0], p[1], p[2]});
        const auto reach = brushes::extent(size, points, dpi, line.width_mm, "fx");
        if (!reach || !intersects(*reach, box)) continue;  // (nothing of it in this box)
        const auto mark = brushes::draw(size, points, dpi, line.width_mm, "fx");
        if (!mark) continue;
        const Box where{mark->origin.x, mark->origin.y, mark->origin.x + mark->mask.width(), mark->origin.y + mark->mask.height()};
        const Box part = intersection(where, box);
        if (part.x1 <= part.x0 || part.y1 <= part.y0) continue;
        const Box local = shifted(part, -box.x0, -box.y0);
        cover.paste(chops::lighter(cover.crop(local), mark->mask.crop(shifted(part, -where.x0, -where.y0))), Point{local.x0, local.y0});
    }
    layer = composite(colour, layer, cover);
    Image clip = Image::create("L", size_of(box), Ink(0));
    {
        PageCanvas canvas(clip, box, size);
        std::vector<PointD> pts;
        for (const XY& p : geo.outline) pts.push_back(px(p));
        canvas.draw().polygon(pts, Ink(255));
        canvas.commit();
    }
    if (geo.within) {  // (a fill kept inside the selection and clear of the faces, as the lines are)
        Image inside = Image::create("L", size_of(box), Ink(0));
        PageCanvas canvas(inside, box, size);
        std::vector<PointD> pts;
        for (const XY& p : *geo.within) pts.push_back(px(p));
        canvas.draw().polygon(pts, Ink(255));
        canvas.commit();
        clip = chops::multiply(clip, inside);
    }
    if (!geo.avoid.empty()) {
        PageCanvas canvas(clip, box, size);
        for (const Shape& s : geo.avoid) {
            if (s.ellipse) {
                const auto& [cx, cy, rx, ry] = s.box;
                const PointD a = px(XY{cx - rx, cy - ry});
                const PointD b = px(XY{cx + rx, cy + ry});
                canvas.draw().ellipse(BoxF{a.x, a.y, b.x, b.y}, Ink(0));
            } else {
                std::vector<PointD> pts;
                for (const XY& p : s.path) pts.push_back(px(p));
                canvas.draw().polygon(pts, Ink(0));
            }
        }
        canvas.commit();
    }
    layer.putalpha(chops::multiply(layer.getchannel(3), clip));
    return alpha_composite(image, layer);
}

}  // namespace genko::render::effects
