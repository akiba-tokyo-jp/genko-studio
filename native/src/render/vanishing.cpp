// PC4 節 D6: フィルター → 消点… (render/vanishing.hpp) — PhotoCraft v0.2.0's algo/src/vanishing.rs (the planes, the camera,
// tear_off, paste, clone_stroke) and engine/src/vp_cmds.rs (the scene from its settings, the op), MIT
// (native/third_party/photocraft/NOTICE.md); on Genko's page (mm), on every kind of a paint layer's pixels, the stamp laid as
// PC3's Clone Stamp lays its stroke (render/retouch_dabs).

#include "render/vanishing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/areas.hpp"
#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/vanishing.hpp"
#include "render/ops_pc3.hpp"
#include "render/fx.hpp"
#include "render/retouch_dabs.hpp"
#include "render/segment/segment.hpp"
#include "render/smart_decode.hpp"

namespace genko::render::vanishing {

using core::Json;
using core::OpError;
using image_ops::Pixels;

namespace {

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 mul(const V3& a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 norm(const V3& a) { return mul(a, 1.0 / std::max(std::sqrt(dot(a, a)), 1e-300)); }

// Where the lines (a, b) and (c, d) meet, in homogeneous coordinates.
V3 meet(const XY& a, const XY& b, const XY& c, const XY& d) {
    const V3 l1 = cross({a[0], a[1], 1.0}, {b[0], b[1], 1.0});
    const V3 l2 = cross({c[0], c[1], 1.0}, {d[0], d[1], 1.0});
    return cross(l1, l2);
}

Quad corners_of(const Json& list) {
    Quad q{};
    for (std::size_t i = 0; i < 4; ++i) q[i] = {list[i][0].get<double>(), list[i][1].get<double>()};
    return q;
}

Box meet_box(const Box& a, const Box& b) {
    Box out{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    if (out.x1 <= out.x0 || out.y1 <= out.y0) return Box{};
    return out;
}

Box join_box(const Box& a, const Box& b) {
    if (a.width() <= 0 || a.height() <= 0) return b;
    if (b.width() <= 0 || b.height() <= 0) return a;
    return Box{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

bool empty(const Box& b) { return b.width() <= 0 || b.height() <= 0; }

// The box (whole pixels) of points, one pixel of slack.
Box box_of(const std::vector<XY>& points) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const XY& p : points) {
        if (!std::isfinite(p[0]) || !std::isfinite(p[1])) continue;
        x0 = std::min(x0, p[0]);
        y0 = std::min(y0, p[1]);
        x1 = std::max(x1, p[0]);
        y1 = std::max(y1, p[1]);
    }
    if (x0 > x1) return Box{};
    const auto clamp = [](double v) { return static_cast<int>(std::clamp(v, -1e8, 1e8)); };
    return Box{clamp(std::floor(x0)) - 1, clamp(std::floor(y0)) - 1, clamp(std::ceil(x1)) + 1, clamp(std::ceil(y1)) + 1};
}

}  // namespace

// --- the planes (vanishing.rs) -------------------------------------------------------------------------------------------

std::optional<XY> Lifted::project(const V3& x) const {
    if (!(x[2] > 1e-9)) return std::nullopt;
    return XY{centre[0] + focal * x[0] / x[2], centre[1] + focal * x[1] / x[2]};
}

std::pair<double, double> Lifted::lengths() const {
    const auto l = [](const V3& a, const V3& b) { return std::sqrt(dot(sub(a, b), sub(a, b))); };
    return {(l(pts[0], pts[1]) + l(pts[3], pts[2])) / 2.0, (l(pts[0], pts[3]) + l(pts[1], pts[2])) / 2.0};
}

std::optional<resample::Homography> homography(const Quad& corners) { return resample::Homography::rect_to_quad({0.0, 0.0, 1.0, 1.0}, corners); }

std::optional<double> focal(const Quad& q, const XY& c) {
    const V3 v1 = meet(q[0], q[1], q[3], q[2]);
    const V3 v2 = meet(q[0], q[3], q[1], q[2]);
    if (std::fabs(v1[2]) < 1e-9 || std::fabs(v2[2]) < 1e-9) return std::nullopt;
    const XY a{v1[0] / v1[2] - c[0], v1[1] / v1[2] - c[1]}, b{v2[0] / v2[2] - c[0], v2[1] / v2[2] - c[1]};
    const double f2 = -(a[0] * b[0] + a[1] * b[1]);
    if (!(f2 > 1.0)) return std::nullopt;
    return std::sqrt(f2);
}

Lifted lift(const Quad& q, double f, const XY& c) {
    const auto ray = [&](const XY& p) { return V3{(p[0] - c[0]) / f, (p[1] - c[1]) / f, 1.0}; };
    // (the vanishing directions, or the edges' own when they are parallel on the page)
    const auto dir = [&](const XY& a, const XY& b, const XY& cc, const XY& d) {
        const V3 v = meet(a, b, cc, d);
        if (std::fabs(v[2]) < 1e-9 * std::max(std::fabs(v[0]) + std::fabs(v[1]), 1.0)) return norm({v[0], v[1], 0.0});
        return norm(ray({v[0] / v[2], v[1] / v[2]}));
    };
    const V3 d1 = dir(q[0], q[1], q[3], q[2]);
    const V3 d2 = dir(q[0], q[3], q[1], q[2]);
    V3 n = norm(cross(d1, d2));
    if (n[2] < 0.0) n = mul(n, -1.0);
    Lifted out;
    out.focal = f;
    out.centre = c;
    for (std::size_t i = 0; i < 4; ++i) {
        const V3 r = ray(q[i]);
        out.pts[i] = mul(r, 1.0 / std::max(std::fabs(dot(n, r)), 1e-9));
    }
    return out;
}

Scene Scene::make(const Quad& first, const XY& centre, std::optional<double> given, double fallback) {
    Scene out;
    out.planes = {first};
    out.centre = centre;
    if (given && *given > 1.0) out.focal = *given;
    else out.focal = vanishing::focal(first, centre).value_or(fallback);
    return out;
}

Lifted Scene::lifted(std::size_t i) const { return lift(planes[i], focal, centre); }

std::optional<std::size_t> Scene::tear_off(std::size_t from, Edge edge, double angle, double depth) {
    const Lifted lf = lifted(from);
    const auto& p = lf.pts;
    // (the edge's ends a → b, clockwise, and the parent's way inwards)
    std::size_t ia = 0, ib = 1, ic = 3;
    switch (edge) {
        case Edge::Top: ia = 0, ib = 1, ic = 3; break;
        case Edge::Right: ia = 1, ib = 2, ic = 0; break;
        case Edge::Bottom: ia = 2, ib = 3, ic = 1; break;
        case Edge::Left: ia = 3, ib = 0, ic = 2; break;
    }
    const V3 a = p[ia], b = p[ib];
    const V3 axis = norm(sub(b, a));
    const V3 inward = sub(p[ic], p[ia]);
    const double len = std::sqrt(dot(inward, inward)) * std::max(depth, 0.05);
    // (the new plane goes on outwards, turned about the edge by 180° − angle; of the two ways to turn, the one facing the
    // camera with the parent before it — a room's corner, as Photoshop tears planes off)
    const V3 out = mul(norm(inward), -1.0);
    const double th = (180.0 - angle) * std::numbers::pi / 180.0;
    std::optional<std::pair<double, Quad>> best;
    for (const double sgn : {1.0, -1.0}) {
        const double s = std::sin(sgn * th), co = std::cos(sgn * th);
        const V3 rot = add(add(mul(out, co), mul(cross(axis, out), s)), mul(axis, dot(axis, out) * (1.0 - co)));
        const V3 a2 = add(a, mul(rot, len)), b2 = add(b, mul(rot, len));
        const auto pa = lf.project(a), pb = lf.project(b), pa2 = lf.project(a2), pb2 = lf.project(b2);
        if (!pa || !pb || !pa2 || !pb2) continue;
        V3 n2 = norm(cross(sub(b, a), sub(a2, a)));
        if (dot(n2, a) > 0.0) n2 = mul(n2, -1.0);
        const double score = dot(n2, inward);
        if (!best || score > best->first) best = std::make_pair(score, Quad{*pa, *pb, *pa2, *pb2});
    }
    if (!best) return std::nullopt;
    const auto& [pa, pb, pa2, pb2] = best->second;
    // (clockwise from the new plane's top left, the shared edge first or last)
    Quad corners{};
    switch (edge) {
        case Edge::Top: corners = {pa2, pb2, pb, pa}; break;
        case Edge::Right: corners = {pa, pa2, pb2, pb}; break;
        case Edge::Bottom: corners = {pb, pa, pa2, pb2}; break;
        case Edge::Left: corners = {pb2, pb, pa, pa2}; break;
    }
    planes.push_back(corners);
    return planes.size() - 1;
}

std::optional<Frame> Scene::frame(std::size_t i) const {
    const auto h = homography(planes[i]);
    if (!h) return std::nullopt;
    const auto [lu, lv] = lifted(i).lengths();
    return Frame{*h, lu, lv};
}

std::optional<std::size_t> Scene::plane_at(const XY& p) const {
    for (std::size_t k = planes.size(); k-- > 0;) {
        const auto h = homography(planes[k]);
        const auto inv = h ? h->inverse() : std::nullopt;
        if (!inv) continue;
        const auto [u, v] = inv->apply(p[0], p[1]);
        if (u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0) return k;
    }
    return std::nullopt;
}

Scene scene_of(const Json& normal, double width_mm, double height_mm) {
    const Json& planes = normal["planes"];
    const XY centre{width_mm / 2.0, height_mm / 2.0};
    std::optional<double> given;
    if (const Json* f = core::get(normal, "focal_mm"); f != nullptr && f->is_number()) given = f->get<double>();
    Scene scene = Scene::make(corners_of(planes[0]["corners_mm"]), centre, given, std::max(width_mm, height_mm));
    for (std::size_t i = 1; i < planes.size(); ++i) {
        const Json& plane = planes[i];
        if (plane.contains("corners_mm")) {
            scene.planes.push_back(corners_of(plane["corners_mm"]));
            continue;
        }
        const std::string e = plane["edge"].get<std::string>();
        const Edge edge = e == "right" ? Edge::Right : e == "bottom" ? Edge::Bottom : e == "left" ? Edge::Left : Edge::Top;
        if (!scene.tear_off(plane["from"].get<std::size_t>(), edge, plane["angle"].get<double>(), plane["depth"].get<double>()))
            throw OpError("vanishing_point: the plane " + std::to_string(i) + " would lie behind the camera");
    }
    for (std::size_t i = 0; i < scene.planes.size(); ++i) {
        const auto h = homography(scene.planes[i]);
        if (!h || !h->inverse()) throw OpError("vanishing_point: the plane " + std::to_string(i) + " is degenerate");
        for (const XY& p : scene.planes[i]) {
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || std::fabs(p[0]) > 1e6 || std::fabs(p[1]) > 1e6)
                throw OpError("vanishing_point: the plane " + std::to_string(i) + " reaches too far (it would lie behind the camera)");
        }
    }
    return scene;
}

// --- the perspective stamp (vanishing.rs clone_stroke, laid as retouch_dabs lays a stroke) --------------------------------

namespace {

// A place on the page (mm) as its plane's metric coordinates.
XY metric(const resample::Homography& inv, const Frame& f, const XY& p) {
    const auto [u, v] = inv.apply(p[0], p[1]);
    return {u * f.lu, v * f.lv};
}

}  // namespace

Stamp stamp_coverage(const Scene& scene, const XY& source_mm, const std::vector<XY>& points_mm, double size_px, double hardness, double per_mm,
                     const Box& grid) {
    Stamp out;
    if (points_mm.empty() || !(per_mm > 0)) return out;
    const auto ps = scene.plane_at(source_mm), pd = scene.plane_at(points_mm.front());
    if (!ps || !pd) return out;
    const auto fs = scene.frame(*ps), fd = scene.frame(*pd);
    if (!fs || !fd) return out;
    const auto ds = fs->h.inverse(), dd = fd->h.inverse();
    if (!ds || !dd) return out;
    const XY ms = metric(*ds, *fs, source_mm), md0 = metric(*dd, *fd, points_mm.front());
    out.from = ps;
    out.to = pd;
    out.offset = {ms[0] - md0[0], ms[1] - md0[1]};
    // (the brush's radius in the plane's metric, from its size on the grid at the first dab: the size is of the working
    // 200 dpi)
    const double radius_px = std::max(size_px * per_mm * 25.4 / 200.0 / 2.0, 0.5);
    const double radius_mm = radius_px / per_mm;
    const XY probe = metric(*dd, *fd, {points_mm.front()[0] + radius_mm, points_mm.front()[1]});
    const double r_m = std::max(std::hypot(probe[0] - md0[0], probe[1] - md0[1]), 1e-9);
    // (the dabs along the stroke on the grid, a quarter of the first dab's diameter apart)
    retouch::Brush brush;
    brush.size = static_cast<float>(radius_px * 2.0);
    brush.spacing = 0.25f;
    brush.pressure_size = false;
    std::vector<retouch::StrokePoint> on_grid;
    for (const XY& p : points_mm) on_grid.push_back(retouch::StrokePoint{p[0] * per_mm, p[1] * per_mm, 1.0f});
    struct Dab {
        XY mc;
        Box area;
    };
    std::vector<Dab> dabs;
    // (a footprint at most 4096 pixels of the working 200 dpi a side: one reaching toward the horizon left out, counted)
    const double most = 4096.0 * std::max(1.0, per_mm * 25.4 / 200.0);
    for (const retouch::Dab& d : retouch::dabs(brush, on_grid)) {
        segment::check_stop();
        const XY mc = metric(*dd, *fd, {d.x / per_mm, d.y / per_mm});
        // (its footprint on the page: a metric circle's image, sixteen points round it)
        std::vector<XY> rim;
        for (int k = 0; k < 16; ++k) {
            const double a = k / 16.0 * 2.0 * std::numbers::pi;
            const auto [x, y] = fd->h.apply((mc[0] + r_m * std::cos(a)) / fd->lu, (mc[1] + r_m * std::sin(a)) / fd->lv);
            rim.push_back({x * per_mm, y * per_mm});
        }
        const Box area = box_of(rim);
        if (empty(area)) continue;
        if (area.width() > most || area.height() > most) {
            ++out.skipped;
            continue;
        }
        const Box inside = meet_box(area, grid);
        if (empty(inside)) continue;
        dabs.push_back({mc, inside});
        out.box = join_box(out.box, inside);
    }
    if (dabs.empty()) return out;
    const auto w = std::size_t(out.box.width());
    out.cov.assign(w * std::size_t(out.box.height()), 0.0f);
    const auto hard = static_cast<float>(std::clamp(hardness, 0.0, 1.0));
    const auto rad = static_cast<float>(radius_px);
    for (const Dab& d : dabs) {
        segment::check_stop();
        bool laid = false;
        for (int y = d.area.y0; y < d.area.y1; ++y)
            for (int x = d.area.x0; x < d.area.x1; ++x) {
                const XY m = metric(*dd, *fd, {(x + 0.5) / per_mm, (y + 0.5) / per_mm});
                const double dist = std::hypot(m[0] - d.mc[0], m[1] - d.mc[1]) / r_m;
                const float v = retouch::dab_coverage(static_cast<float>(dist) * rad, rad, hard);
                if (!(v > 0.0f)) continue;
                float& c = out.cov[std::size_t(y - out.box.y0) * w + std::size_t(x - out.box.x0)];
                c = c + v * (1.0f - c);  // (built up as a stroke of full flow)
                laid = true;
            }
        if (laid) ++out.dabs;
    }
    return out;
}

std::optional<XY> stamp_source(const Scene& scene, const Stamp& stamp, const XY& at_px, double per_mm) {
    if (!stamp.from || !stamp.to) return std::nullopt;
    const auto fs = scene.frame(*stamp.from), fd = scene.frame(*stamp.to);
    if (!fs || !fd) return std::nullopt;
    const auto dd = fd->h.inverse();
    if (!dd) return std::nullopt;
    const XY m = metric(*dd, *fd, {at_px[0] / per_mm, at_px[1] / per_mm});
    const XY sm{m[0] + stamp.offset[0], m[1] + stamp.offset[1]};
    const auto [x, y] = fs->h.apply(sm[0] / fs->lu, sm[1] / fs->lv);
    if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
    return XY{x * per_mm, y * per_mm};
}

// --- pasting (vanishing.rs paste) ----------------------------------------------------------------------------------------

Box paste_box(const Scene& scene, std::size_t i, double per_mm, const Box& grid) {
    std::vector<XY> quad;
    for (const XY& p : scene.planes[i]) quad.push_back({p[0] * per_mm, p[1] * per_mm});
    return meet_box(box_of(quad), grid);
}

std::optional<resample::Plane> pasted(const Scene& scene, std::size_t i, const resample::Plane& picture, const XY& at, double width, double per_mm,
                                      const Box& out) {
    const auto f = scene.frame(i);
    if (!f || picture.width <= 0 || picture.height <= 0 || empty(out)) return std::nullopt;
    const double sw = picture.width;
    // (the picture's pixels → the plane's (u, v): its width `width` of the plane's, its height kept in the plane's metric)
    const double su = width / sw, sv = width * f->lu / f->lv / sw;
    const resample::Homography to_plane{{su, 0.0, at[0], 0.0, sv, at[1], 0.0, 0.0, 1.0}};
    const resample::Homography grid{{per_mm, 0.0, 0.0, 0.0, per_mm, 0.0, 0.0, 0.0, 1.0}};
    const resample::Homography on_page = grid.mul(f->h);
    const resample::Homography full = on_page.mul(to_plane);
    resample::Plane made = resample::project(picture, full, out.width(), out.height(), resample::Interp::Bicubic, out.x0, out.y0);
    // (clipped to the plane)
    const auto inv = on_page.inverse();
    if (!inv) return std::nullopt;
    bool any = false;
    const auto n = std::size_t(made.channels);
    for (int y = 0; y < made.height; ++y)
        for (int x = 0; x < made.width; ++x) {
            float* px = made.at(x, y);
            if (!(px[n - 1] > 0.0f)) continue;
            const auto [u, v] = inv->apply(out.x0 + x + 0.5, out.y0 + y + 0.5);
            if (!(u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0)) {
                std::fill(px, px + n, 0.0f);
                continue;
            }
            any = true;
        }
    if (!any) return std::nullopt;
    return made;
}

// --- the op (vp_cmds.rs vanishing_point) -----------------------------------------------------------------------------------

namespace {

constexpr std::size_t kMostPastes = 16;
constexpr std::size_t kMostStrokes = 64;
constexpr std::size_t kMostPoints = 4096;
constexpr std::size_t kMostData = 32u * 1024 * 1024;

bool finite_number(const Json& v) { return v.is_number() && !v.is_boolean() && std::isfinite(v.get<double>()); }

XY point_of(const Json& v, const std::string& where) {
    if (!v.is_array() || v.size() != 2 || !finite_number(v[0]) || !finite_number(v[1])) throw OpError(where + " must be a place [x, y]");
    return {v[0].get<double>(), v[1].get<double>()};
}

double number_of(const Json& item, const char* key, double fallback, double lo, double hi, const std::string& where) {
    const Json* v = core::get(item, key);
    if (v == nullptr || v->is_null()) return fallback;
    if (!finite_number(*v)) throw OpError(where + "." + key + " must be a number");
    return std::clamp(v->get<double>(), lo, hi);
}

// A picture as the target's kind of pixels (an 8-bit picture or another kind's as the colours it means, an ink layer's by
// the page's rules).
Pixels as_kind(const Pixels& src, const Pixels& like) {
    if (src.kind() == like.kind() && src.colours() == like.colours() && (src.kind() != Pixels::Kind::Inks || src.ink == like.ink)) return src;
    const std::vector<image_ops::Rgba> rgba = image_ops::rgba_of(src);
    if (like.kind() == Pixels::Kind::Inks) {
        Pixels out(Pixels::Kind::Inks, src.width(), src.height(), like.colours());
        out.ink = like.ink;
        image_ops::from_rgba(out, rgba);
        return out;
    }
    Pixels out(like.kind(), src.width(), src.height(), 3);
    out.floating = like.floating;
    for (std::size_t i = 0; i < out.size(); ++i) std::copy(rgba[i].begin(), rgba[i].end(), out.at(i));
    return out;
}

// The pixels that show (alpha above 0) as a plane of their own box (none: nothing shows).
std::optional<resample::Plane> shown_plane(const Pixels& p) {
    int x0 = static_cast<int>(p.width()), y0 = static_cast<int>(p.height()), x1 = 0, y1 = 0;
    const unsigned n = p.all();
    for (std::uint32_t y = 0; y < p.height(); ++y)
        for (std::uint32_t x = 0; x < p.width(); ++x) {
            if (!(p.at(std::size_t(y) * p.width() + x)[n - 1] > 0.0f)) continue;
            x0 = std::min(x0, int(x));
            y0 = std::min(y0, int(y));
            x1 = std::max(x1, int(x) + 1);
            y1 = std::max(y1, int(y) + 1);
        }
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    resample::Plane out(x1 - x0, y1 - y0, static_cast<int>(n), true);
    out.hdr = p.kind() == Pixels::Kind::Precise && p.floating;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) std::copy_n(p.at(std::size_t(y) * p.width() + std::size_t(x)), n, out.at(x - x0, y - y0));
    return out;
}

// The bilinear sample of pixels over `box` at (u, v) — grid pixels, centres at whole numbers — premultiplied, clear past
// them, into `out` (straight).
void sample(const Pixels& p, const Box& box, double u, double v, float* out) {
    const unsigned n = p.all(), a = n - 1;
    std::vector<double> acc(n, 0.0);
    const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
    const double fx = u - ix, fy = v - iy;
    const auto add = [&](int x, int y, double w) {
        if (w <= 0.0 || x < box.x0 || y < box.y0 || x >= box.x1 || y >= box.y1) return;
        const float* q = p.at(std::size_t(y - box.y0) * std::size_t(box.width()) + std::size_t(x - box.x0));
        for (unsigned c = 0; c < a; ++c) acc[c] += static_cast<double>(q[c]) * q[a] * w;
        acc[a] += static_cast<double>(q[a]) * w;
    };
    add(ix, iy, (1 - fx) * (1 - fy));
    add(ix + 1, iy, fx * (1 - fy));
    add(ix, iy + 1, (1 - fx) * fy);
    add(ix + 1, iy + 1, fx * fy);
    const double al = std::clamp(acc[a], 0.0, 1.0);
    for (unsigned c = 0; c < a; ++c) out[c] = al > 1e-6 ? static_cast<float>(acc[c] / al) : 0.0f;
    out[a] = static_cast<float>(al);
}

struct PasteItem {
    std::size_t plane = 0;
    Pixels picture;
    XY at{0.25, 0.25};
    double width = 0.5;
};

struct CloneItem {
    XY source{0, 0};
    std::vector<XY> points;
    double size = 40, hardness = 0.5, opacity = 1;
};

}  // namespace

void vanishing_point(core::OpContext& c) {
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    core::Document& doc = c.doc;
    const Json& op = c.op;
    static const char* const kKeys[] = {"op", "page", "id", "new_layer", "planes", "focal_mm", "keep_planes", "paste", "clone", "area", "kind"};
    for (const auto& [key, value] : op.items()) {
        if (std::find(std::begin(kKeys), std::end(kKeys), key) == std::end(kKeys))
            throw OpError("vanishing_point has no setting " + core::py_repr_str(key) + " (page, id, new_layer, planes, focal_mm, keep_planes, paste, clone, area)");
    }
    if (const Json* kind = core::get(op, "kind"); kind != nullptr && !kind->is_null() && *kind != Json("vanishing_point"))
        throw OpError("vanishing_point has no kind (the フィルター menu's: vanishing_point)");
    const std::size_t at = core::require_page(doc, op);
    const Json* keep_given = core::get(op, "keep_planes");
    const bool keep = keep_given == nullptr || keep_given->is_null() || core::py_truthy(*keep_given);
    // what it is asked to lay (none: the planes alone — no layer needed)
    const auto asked = [&](const char* key) {
        const Json* list = core::get(op, key);
        return list != nullptr && !list->is_null() && !(list->is_array() && list->empty());
    };
    const bool draws = asked("paste") || asked("clone");
    // the planes taken away ("planes": [] — 消点's window with every plane deleted): the page's gone, nothing laid
    if (const Json* planes = core::get(op, "planes"); planes != nullptr && planes->is_array() && planes->empty()) {
        if (draws) throw OpError("vanishing_point: no planes to paste or stamp on (planes is empty)");
        bool removed = false;
        if (keep && core::page_vanishing(doc.page(at)) != nullptr) {
            core::Page& page = doc.edit_page(at);
            page.extra.erase(std::string(core::kVanishingKey));
            removed = true;
        }
        c.report = Json{{"id", nullptr}, {"new_layer", false}, {"planes", Json::array()}, {"focal_mm", nullptr}, {"pasted", 0},
                        {"dabs", 0},     {"box_mm", nullptr}, {"removed", removed}};
        return;
    }
    // the planes: given, else the page's
    Json normal;
    if (const Json* planes = core::get(op, "planes"); planes != nullptr && !planes->is_null()) {
        Json given{{"planes", *planes}};
        if (const Json* f = core::get(op, "focal_mm"); f != nullptr && !f->is_null()) given["focal_mm"] = *f;
        normal = core::read_vanishing(given);
    } else if (const Json* kept = core::page_vanishing(doc.page(at))) {
        Json given = *kept;
        if (const Json* f = core::get(op, "focal_mm"); f != nullptr && !f->is_null()) given["focal_mm"] = *f;
        normal = core::read_vanishing(given);
    } else {
        throw OpError("vanishing_point: the page has no planes yet (give planes: the first {\"corners_mm\": [[x, y] × 4]})");
    }
    const double width_mm = doc.page(at).spec.width_mm.value(), height_mm = doc.page(at).spec.height_mm.value();
    const Scene scene = scene_of(normal, width_mm, height_mm);
    const bool new_layer = draws && core::truthy_at(op, "new_layer");
    std::string id;
    if (const Json* given = core::get(op, "id"); given != nullptr && !given->is_null()) {
        if (!given->is_string() || given->get_ref<const std::string&>().empty()) throw OpError("id must be a layer's id");
        id = given->get<std::string>();
    }
    if (draws && id.empty() && !new_layer) throw OpError("vanishing_point needs the paint layer it lays on (id), or a new layer (new_layer)");
    // the pastes and strokes read (their pictures read before anything changes)
    std::vector<PasteItem> pastes;
    std::vector<CloneItem> clones;
    if (const Json* list = core::get(op, "paste"); list != nullptr && !list->is_null()) {
        if (!list->is_array() || list->size() > kMostPastes)
            throw OpError("paste must be a list of at most " + std::to_string(kMostPastes) + " pictures {\"plane\", \"layer\" | \"data\", \"at\": [u, v], \"width\"}");
        for (std::size_t k = 0; k < list->size(); ++k) {
            const Json& item = (*list)[k];
            const std::string where = "paste[" + std::to_string(k) + "]";
            if (!item.is_object()) throw OpError(where + " must be an object");
            for (const auto& [key, value] : item.items()) {
                if (key != "plane" && key != "layer" && key != "data" && key != "at" && key != "width")
                    throw OpError(where + " has no setting " + core::py_repr_str(key) + " (plane, layer, data, at, width)");
            }
            PasteItem made{0, Pixels(Pixels::Kind::Eight, 0, 0, 3)};
            if (const Json* plane = core::get(item, "plane"); plane != nullptr && !plane->is_null()) {
                if (!plane->is_number_integer() || plane->get<std::int64_t>() < 0 || plane->get<std::int64_t>() >= static_cast<std::int64_t>(scene.planes.size()))
                    throw OpError(where + ".plane must be one of the planes (0 to " + std::to_string(scene.planes.size() - 1) + ")");
                made.plane = plane->get<std::size_t>();
            }
            if (const Json* a = core::get(item, "at"); a != nullptr && !a->is_null()) made.at = point_of(*a, where + ".at");
            made.at = {std::clamp(made.at[0], -10.0, 10.0), std::clamp(made.at[1], -10.0, 10.0)};
            made.width = number_of(item, "width", 0.5, 0.01, 10.0, where);
            const Json* layer = core::get(item, "layer");
            const Json* data = core::get(item, "data");
            if (data != nullptr && !data->is_null()) {
                if (!data->is_string() || data->get_ref<const std::string&>().size() / 4 * 3 > kMostData + 3)
                    throw OpError(where + ".data must be the base64 of a PNG file (32 MiB at most)");
                std::string bytes;
                try {
                    bytes = core::a2b_base64(data->get_ref<const std::string&>());
                } catch (const core::Error&) {
                    throw OpError(where + ".data must be the base64 of a PNG file (32 MiB at most)");
                }
                if (bytes.size() > kMostData) throw OpError(where + ".data must be the base64 of a PNG file (32 MiB at most)");
                Image picture;
                try {
                    picture = smart::eight_bit_picture(bytes).convert("RGBA");
                } catch (const core::Error& error) {
                    if (error.code() == "memory") throw;
                    throw OpError(where + ".data cannot be read as a picture (" + std::string(error.what()) + ")");
                }
                made.picture = image_ops::from_image(picture);
            } else if (layer != nullptr && layer->is_string()) {
                const core::Page& page = doc.page(at);
                const core::Layer* found = nullptr;
                for (const core::Layer& l : page.layers)
                    if (l.id == layer->get<std::string>()) found = &l;
                if (found == nullptr) throw OpError(where + ".layer: no layer " + layer->get<std::string>() + " on the page");
                made.picture = image_ops::shown_layer_pixels(doc, page, *found);
            } else {
                throw OpError(where + " needs its picture: a layer of the page (layer) or a PNG's data (data)");
            }
            pastes.push_back(std::move(made));
        }
    }
    if (const Json* list = core::get(op, "clone"); list != nullptr && !list->is_null()) {
        if (!list->is_array() || list->size() > kMostStrokes)
            throw OpError("clone must be a list of at most " + std::to_string(kMostStrokes) + " strokes {\"source\": [x, y], \"points\": [[x, y]…] (mm), \"size_px\", \"hardness\", \"opacity\"}");
        for (std::size_t k = 0; k < list->size(); ++k) {
            const Json& item = (*list)[k];
            const std::string where = "clone[" + std::to_string(k) + "]";
            if (!item.is_object()) throw OpError(where + " must be an object");
            for (const auto& [key, value] : item.items()) {
                if (key != "source" && key != "points" && key != "size_px" && key != "hardness" && key != "opacity")
                    throw OpError(where + " has no setting " + core::py_repr_str(key) + " (source, points, size_px, hardness, opacity)");
            }
            CloneItem made;
            const Json* source = core::get(item, "source");
            if (source == nullptr) throw OpError(where + ".source must be a place [x, y] (mm)");
            // (its places within the page's size round the page: a stroke from outside reaching far is refused, P2-4)
            const auto near_page = [&](const XY& p) {
                return p[0] >= -width_mm && p[0] <= 2.0 * width_mm && p[1] >= -height_mm && p[1] <= 2.0 * height_mm;
            };
            made.source = point_of(*source, where + ".source");
            if (!near_page(made.source)) throw OpError(where + ".source must lie near the page (within its width and height round it)");
            const Json* points = core::get(item, "points");
            if (points == nullptr || !points->is_array() || points->empty() || points->size() > kMostPoints)
                throw OpError(where + ".points must be a list of 1 to " + std::to_string(kMostPoints) + " places [x, y] (mm)");
            for (const Json& p : *points) {
                made.points.push_back(point_of(p, where + ".points"));
                if (!near_page(made.points.back())) throw OpError(where + ".points must lie near the page (within its width and height round it)");
            }
            made.size = number_of(item, "size_px", 40, 1, 2000, where);
            made.hardness = number_of(item, "hardness", 50, 0, 100, where) / 100.0;
            made.opacity = number_of(item, "opacity", 100, 0, 100, where) / 100.0;
            clones.push_back(std::move(made));
        }
    }
    // the planes as the report gives them, and kept on the page
    const auto planes_of_scene = [&scene] {
        Json planes = Json::array();
        for (std::size_t i = 0; i < scene.planes.size(); ++i) {
            const auto [lu, lv] = scene.lifted(i).lengths();
            Json corners = Json::array();
            for (const XY& p : scene.planes[i]) corners.push_back(Json::array({p[0], p[1]}));
            planes.push_back(Json{{"corners_mm", std::move(corners)}, {"aspect", lv > 0 ? lu / lv : 0.0}});
        }
        return planes;
    };
    const auto keep_planes = [&] {
        if (!keep) return;
        core::Page& page = doc.edit_page(at);
        if (!page.extra.is_object()) page.extra = Json::object();
        page.extra[std::string(core::kVanishingKey)] = normal;
    };
    if (!draws) {  // (the planes alone: no layer named or made, P3-15)
        keep_planes();
        c.report = Json{{"id", id.empty() ? Json(nullptr) : Json(id)}, {"new_layer", false}, {"planes", planes_of_scene()}, {"focal_mm", scene.focal},
                        {"pasted", 0}, {"dabs", 0}, {"box_mm", nullptr}};
        return;
    }
    const std::optional<Json> area = core::truthy_at(op, "area") ? std::optional<Json>(core::op_area(op)) : std::nullopt;
    // the layer: the one named, or a new one above it (or on top)
    if (new_layer) {
        core::Page& page = doc.edit_page(at);
        core::Layer layer;
        layer.id = core::new_id();
        layer.role = core::LayerRole::User;
        layer.kind = core::LayerKind::Strokes;
        layer.title = "消点";
        layer.exportable = true;
        layer.panel_each = true;
        std::size_t index = page.layers.size();
        if (!id.empty()) {
            bool found = false;
            for (std::size_t i = 0; i < page.layers.size(); ++i) {
                if (page.layers[i].id != id) continue;
                index = i + 1;
                layer.parent_id = page.layers[i].parent_id;
                found = true;
            }
            if (!found) throw OpError("no layer " + id + " on the page");
        }
        id = layer.id;
        page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
    }
    image_ops::Window window;
    const Pixels like = image_ops::layer_pixels(c, at, id, Box{0, 0, 1, 1}, &window);  // (refused: no such paint layer, locked)
    const Box grid{0, 0, window.grid.width, window.grid.height};
    if (empty(grid)) throw OpError("the layer " + id + " has no pixels to work on");
    const double per_mm = window.grid.width / width_mm;
    // the selection's coverage over the grid (none: everywhere)
    std::optional<std::vector<float>> cover;
    Box within = grid;
    const image_ops::Held cover_held(area ? std::uint64_t(grid.x1) * std::uint64_t(grid.y1) * sizeof(float) : 0);
    if (area) {
        cover = image_ops::coverage(doc.page(at), *area, static_cast<std::uint32_t>(grid.x1), static_cast<std::uint32_t>(grid.y1), like.kind() == Pixels::Kind::Eight);
        int x0 = grid.x1, y0 = grid.y1, x1 = 0, y1 = 0;
        for (int y = 0; y < grid.y1; ++y)
            for (int x = 0; x < grid.x1; ++x) {
                if (!((*cover)[std::size_t(y) * std::size_t(grid.x1) + std::size_t(x)] > 0.0f)) continue;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1);
                y1 = std::max(y1, y + 1);
            }
        within = meet_box(Box{x0, y0, x1, y1}, grid);
    }
    // what each lays reaches (and what a stroke copies from): the box worked
    Box work;
    std::vector<Box> paste_boxes;
    for (const PasteItem& p : pastes) {
        paste_boxes.push_back(meet_box(paste_box(scene, p.plane, per_mm, grid), within));
        work = join_box(work, paste_boxes.back());
    }
    std::vector<Stamp> stamps;
    std::vector<std::unique_ptr<image_ops::Held>> stamps_held;
    std::size_t skipped = 0;
    for (const CloneItem& k : clones) {
        segment::check_stop();
        Stamp st = stamp_coverage(scene, k.source, k.points, k.size, k.hardness, per_mm, grid);
        skipped += st.skipped;
        stamps_held.push_back(std::make_unique<image_ops::Held>(std::uint64_t(st.cov.size()) * sizeof(float)));
        if (st.dabs > 0) {
            st.box = meet_box(st.box, within);
            if (!empty(st.box)) {
                work = join_box(work, st.box);
                // (where it copies from: the box's rim and middle sent to the source, two pixels round them)
                std::vector<XY> from;
                const Box& b = st.box;
                for (int k2 = 0; k2 <= 16; ++k2) {
                    const double t = k2 / 16.0;
                    for (const XY& q : {XY{b.x0 + t * b.width(), double(b.y0)}, XY{b.x0 + t * b.width(), double(b.y1)}, XY{double(b.x0), b.y0 + t * b.height()},
                                        XY{double(b.x1), b.y0 + t * b.height()}})
                        if (const auto s = stamp_source(scene, st, q, per_mm)) from.push_back(*s);
                }
                Box source = box_of(from);
                source = meet_box(Box{source.x0 - 2, source.y0 - 2, source.x1 + 2, source.y1 + 2}, grid);
                work = join_box(work, source);
            }
        }
        stamps.push_back(std::move(st));
    }
    work = meet_box(work, grid);
    std::size_t pasted_count = 0, dab_count = 0;
    Box damaged;
    if (!empty(work)) {
        image_ops::Window part;
        Pixels target = image_ops::layer_pixels(c, at, id, work, &part);
        const Box box = part.box;
        const unsigned n = target.colours();
        const auto bw = std::size_t(box.width());
        const auto cover_at = [&](int x, int y) { return cover ? (*cover)[std::size_t(y) * std::size_t(grid.x1) + std::size_t(x)] : 1.0f; };
        // (how far it is: each paste and stroke a share)
        const double shares = static_cast<double>(pastes.size() + clones.size());
        std::size_t done = 0;
        // the pastes, in their order, over the layer
        for (std::size_t k = 0; k < pastes.size(); ++k) {
            segment::check_stop();
            fx::ProgressScope::put(static_cast<double>(done++) / shares);
            const Box pb = meet_box(paste_boxes[k], box);
            if (empty(pb)) continue;
            const image_ops::Held picture_held(std::uint64_t(pastes[k].picture.size()) * (n + 1) * sizeof(float) * 2);
            const auto picture = shown_plane(as_kind(pastes[k].picture, like));
            if (!picture) continue;
            const image_ops::Held made_held(std::uint64_t(pb.width()) * std::uint64_t(pb.height()) * (n + 1) * sizeof(float));
            const auto made = pasted(scene, pastes[k].plane, *picture, pastes[k].at, pastes[k].width, per_mm, pb);
            if (!made) continue;
            segment::check_stop();
            bool laid = false;
            for (int y = pb.y0; y < pb.y1; ++y)
                for (int x = pb.x0; x < pb.x1; ++x) {
                    const float* px = made->at(x - pb.x0, y - pb.y0);
                    const float k2 = cover_at(x, y);
                    if (!(px[n] > 0.0f) || !(k2 > 0.0f)) continue;
                    retouch::over(target.at(std::size_t(y - box.y0) * bw + std::size_t(x - box.x0)), px, n, k2, false);
                    damaged = join_box(damaged, Box{x, y, x + 1, y + 1});
                    laid = true;
                }
            if (laid) ++pasted_count;
        }
        // the strokes, each copying from what is there before it (a new layer: the page as it shows, what was pasted on it
        // over that)
        // (a new layer's strokes copy from the page as it shows under it — drawn once: the new layer is empty in the book
        // until the end — with what is laid on the layer so far over it)
        std::optional<Pixels> under;
        const image_ops::Held work_held(clones.empty() ? 0 : std::uint64_t(box.width()) * std::uint64_t(box.height()) * (n + 1) * sizeof(float) * (new_layer ? 3 : 2));
        for (std::size_t k = 0; k < clones.size(); ++k) {
            segment::check_stop();
            fx::ProgressScope::put(static_cast<double>(done++) / shares);
            const Stamp& st = stamps[k];
            if (st.dabs == 0 || empty(st.box)) continue;
            Pixels before = [&] {
                if (!new_layer) return target;
                if (!under) under = page_shown_over(c, at, id, box);
                Pixels shown = *under;
                for (std::size_t i = 0; i < shown.size(); ++i) retouch::over(shown.at(i), target.at(i), n, 1.0f, false);
                return shown;
            }();
            Pixels paint(like.kind(), box.width() > 0 ? std::uint32_t(box.width()) : 0u, std::uint32_t(std::max(box.height(), 0)), like.colours());
            paint.floating = like.floating;
            paint.ink = like.ink;
            const Box sb = meet_box(st.box, box);
            for (int y = sb.y0; y < sb.y1; ++y) {
                if ((y - sb.y0) % 64 == 0) segment::check_stop();
                for (int x = sb.x0; x < sb.x1; ++x) {
                    if (!(st.cov[std::size_t(y - st.box.y0) * std::size_t(st.box.width()) + std::size_t(x - st.box.x0)] > 0.0f)) continue;
                    const auto q = stamp_source(scene, st, {x + 0.5, y + 0.5}, per_mm);
                    if (!q) continue;
                    sample(before, box, (*q)[0] - 0.5, (*q)[1] - 0.5, paint.at(std::size_t(y - box.y0) * bw + std::size_t(x - box.x0)));
                }
            }
            retouch::Coverage cov;
            cov.box = st.box;
            cov.cov = st.cov;
            std::optional<std::vector<float>> chosen;
            const image_ops::Held chosen_held(cover ? std::uint64_t(bw) * std::uint64_t(box.height()) * sizeof(float) : 0);
            if (cover) {
                chosen.emplace(bw * std::size_t(box.height()));
                for (int y = box.y0; y < box.y1; ++y)
                    for (int x = box.x0; x < box.x1; ++x) (*chosen)[std::size_t(y - box.y0) * bw + std::size_t(x - box.x0)] = cover_at(x, y);
            }
            const Box changed = retouch::apply_coverage(target, box, cov, static_cast<float>(clones[k].opacity), chosen ? &*chosen : nullptr, false, paint, "normal");
            if (!empty(changed)) {
                damaged = join_box(damaged, changed);
                dab_count += st.dabs;
            }
        }
        segment::check_stop();
        fx::ProgressScope::put(1.0);
        if (!empty(damaged)) image_ops::put_layer_pixels(doc, at, id, target, box);
    }
    keep_planes();
    if (skipped > 0)
        c.notes.push_back("vanishing_point: " + std::to_string(skipped) + " dabs reached toward the horizon too far to lay and were left out");
    Json box_mm = nullptr;
    if (!empty(damaged))
        box_mm = Json::array({damaged.x0 / per_mm, damaged.y0 / per_mm, damaged.width() / per_mm, damaged.height() / per_mm});
    c.report = Json{{"id", id}, {"new_layer", new_layer}, {"planes", planes_of_scene()}, {"focal_mm", scene.focal}, {"pasted", pasted_count},
                    {"dabs", dab_count}, {"box_mm", std::move(box_mm)}};
    if (skipped > 0) (*c.report)["skipped_dabs"] = skipped;
}

}  // namespace genko::render::vanishing
