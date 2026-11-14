// fukidashi@1 (SPEC BALLOON-01): the balloons' outlines as polygons of real pixels and their coverage. See the header.

#include "render/text/balloon_clean.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "core/vector.hpp"
#include "render/libm_float.hpp"
#include "render/path_raster.hpp"

namespace genko::render::text::clean {

namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kTolerance = 0.05;  // (an outline lies within this of its curve, px)
constexpr double kMitre = 4.0;       // (a corner's mitre reaches at most this many times the ribbon's reach)

double hyp(double x, double y) { return std::sqrt(x * x + y * y); }
P add(P a, P b) { return {a.x + b.x, a.y + b.y}; }
P sub(P a, P b) { return {a.x - b.x, a.y - b.y}; }
P mul(P a, double k) { return {a.x * k, a.y * k}; }
double dot(P a, P b) { return a.x * b.x + a.y * b.y; }
double cross(P a, P b) { return a.x * b.y - a.y * b.x; }
P unit(P a) {
    const double n = hyp(a.x, a.y);
    if (n < 1e-12) return {1.0, 0.0};
    return {a.x / n, a.y / n};
}
P lerp(P a, P b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
// (for an outline that runs clockwise on the page, the side its inside lies on)
P inward(P direction) { return {-direction.y, direction.x}; }

void push(Contour& out, P p) {
    if (!out.empty() && std::fabs(out.back().x - p.x) < 1e-9 && std::fabs(out.back().y - p.y) < 1e-9) return;
    out.push_back(p);
}

// A quarter of the unit circle as (sin a, cos a), a from 0 to a right angle, in 2^depth chords: each middle the
// normalized sum of its neighbours (no table of angles, no library sine).
std::vector<P> unit_quarter(int depth) {
    std::vector<P> points{{0.0, 1.0}, {1.0, 0.0}};
    for (int d = 0; d < depth; ++d) {
        std::vector<P> next;
        next.reserve(points.size() * 2 - 1);
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            next.push_back(points[i]);
            next.push_back(unit(add(points[i], points[i + 1])));
        }
        next.push_back(points.back());
        points = std::move(next);
    }
    return points;
}

// How many times a quarter circle of `radius` is halved so that its chords lie within the tolerance.
int quarter_depth(double radius) {
    int depth = 2;
    while (depth < 11) {
        const double angle = (kPi / 2) / static_cast<double>(1 << depth);
        if (radius * angle * angle / 8 <= kTolerance) break;
        ++depth;
    }
    return depth;
}

// A quarter of an outline that is its own mirror image both ways — from its top (0, −ry) clockwise to its right
// (rx, 0), about its middle — made the whole outline about (cx, cy).
Contour four_fold(const std::vector<P>& quarter, double cx, double cy) {
    const std::size_t n = quarter.size() - 1;
    Contour out;
    out.reserve(4 * n);
    for (std::size_t i = 0; i < n; ++i) out.push_back({cx + quarter[i].x, cy + quarter[i].y});
    for (std::size_t i = n; i > 0; --i) out.push_back({cx + quarter[i].x, cy - quarter[i].y});
    for (std::size_t i = 0; i < n; ++i) out.push_back({cx - quarter[i].x, cy - quarter[i].y});
    for (std::size_t i = n; i > 0; --i) out.push_back({cx - quarter[i].x, cy + quarter[i].y});
    return out;
}

// The right half of an outline that is its own mirror image left to right — from its top (0, y) clockwise to its
// bottom (0, y'), about the middle line x = 0 — made the whole outline, placed by `place`.
template <typename Place>
Contour two_fold(const std::vector<P>& half, Place place) {
    Contour out;
    out.reserve(half.size() * 2);
    for (const P& p : half) out.push_back(place(p.x, p.y, false));
    for (std::size_t i = half.size() - 1; i-- > 1;) out.push_back(place(half[i].x, half[i].y, true));
    return out;
}

std::vector<P> quarter_ellipse(double rx, double ry) {
    const std::vector<P> circle = unit_quarter(quarter_depth(std::max(rx, ry)));
    std::vector<P> out;
    out.reserve(circle.size());
    for (const P& u : circle) out.push_back({rx * u.x, -ry * u.y});
    return out;
}

// The arc from `a` to `b` bulging `sagitta` to the side `out` (a unit vector across the chord): the points after `a`,
// `b` the last. Less than half a circle.
void arc_to(Contour& points, P a, P b, P out, double sagitta) {
    const double chord = distance(a, b);
    if (sagitta <= kTolerance / 4 || chord < 1e-9) {
        push(points, b);
        return;
    }
    const double radius = (chord * chord / 4 + sagitta * sagitta) / (2 * sagitta);
    const P middle = lerp(a, b, 0.5);
    const P centre = sub(middle, mul(out, radius - sagitta));
    std::vector<P> arc{a, b};
    for (int depth = 0; depth < 10; ++depth) {
        const double c = distance(arc[0], arc[1]);
        const double sag = radius - std::sqrt(std::max(0.0, radius * radius - c * c / 4));
        if (sag <= kTolerance) break;
        std::vector<P> next;
        next.reserve(arc.size() * 2 - 1);
        for (std::size_t i = 0; i + 1 < arc.size(); ++i) {
            next.push_back(arc[i]);
            const P m = sub(lerp(arc[i], arc[i + 1], 0.5), centre);
            next.push_back(add(centre, mul(unit(m), radius)));
        }
        next.push_back(arc.back());
        arc = std::move(next);
    }
    for (std::size_t i = 1; i < arc.size(); ++i) push(points, arc[i]);
}

// A corner rounded: from `corner − in·reach` to `corner + out·reach` as the curve whose control point is the corner
// (tangent to both sides), the points after its start.
void round_corner(Contour& points, P from, P corner, P to, int pieces, double until = 1.0) {
    for (int j = 1; j <= pieces; ++j) {
        const double t = until * static_cast<double>(j) / static_cast<double>(pieces);
        const double u = 1 - t;
        push(points, {u * u * from.x + 2 * u * t * corner.x + t * t * to.x, u * u * from.y + 2 * u * t * corner.y + t * t * to.y});
    }
}

int corner_pieces(P from, P corner, P to) {
    const double reach = distance(from, corner) + distance(corner, to);
    return std::clamp(static_cast<int>(std::ceil(std::sqrt(reach) * 1.5)), 4, 48);
}

// Where two segments cross: how far along each (0..1), when they do.
bool crossing(P a, P b, P c, P d, double& t, double& u) {
    const P r = sub(b, a);
    const P s = sub(d, c);
    const double den = cross(r, s);
    if (std::fabs(den) < 1e-12) return false;
    const P ac = sub(c, a);
    t = cross(ac, s) / den;
    u = cross(ac, r) / den;
    const double eps = 1e-9;
    return t >= -eps && t <= 1 + eps && u >= -eps && u <= 1 + eps;
}

struct Bounds {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

Bounds bounds_of(const Contour& points) {
    Bounds b{points.front().x, points.front().y, points.front().x, points.front().y};
    for (const P& p : points) {
        b.x0 = std::min(b.x0, p.x);
        b.y0 = std::min(b.y0, p.y);
        b.x1 = std::max(b.x1, p.x);
        b.y1 = std::max(b.y1, p.y);
    }
    return b;
}

// The coverage of a rasterizer over the plane (what it does not reach left 0).
Plane rendered(const path_raster::Rasterizer& raster, int width, int height) {
    Plane plane;
    plane.width = width;
    plane.height = height;
    plane.values.assign(static_cast<std::size_t>(std::max(0, width)) * static_cast<std::size_t>(std::max(0, height)), 0.0f);
    const auto reach = raster.pixel_bounds();
    if (!reach || width <= 0 || height <= 0) return plane;
    const path_raster::PixelBox box{std::max(0, reach->x0), std::max(0, reach->y0), std::min(width, reach->x1), std::min(height, reach->y1)};
    if (box.empty()) return plane;
    const std::vector<float> part = raster.render(box);
    const auto bw = static_cast<std::size_t>(box.width());
    for (int y = box.y0; y < box.y1; ++y) {
        std::copy_n(part.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y - box.y0) * bw), bw,
                    plane.values.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(box.x0)));
    }
    return plane;
}

std::vector<path_raster::XY> as_xy(const Contour& points) {
    std::vector<path_raster::XY> out;
    out.reserve(points.size());
    for (const P& p : points) out.emplace_back(p.x, p.y);
    return out;
}

// How much further a join reaches into a sharp notch than into a blunt one, so that its curve is about as round
// either way: the cotangent of half the angle between the notch's two sides (unit vectors from its corner), kept
// between 0.6 and 2.5.
double sharpness(P along_a, P along_b) {
    const double cosine = std::clamp(dot(along_a, along_b), -0.98, 0.98);
    return std::clamp(std::sqrt((1 + cosine) / (1 - cosine)), 0.6, 2.5);
}

// A patch that rounds the notch at `corner`, where two outlines meet: from `a` on the one (its inside toward `a_in`,
// running toward the corner along `a_to`) round to `b` on the other (`b_in`, `b_to` the same), a curve that leaves
// both along their own directions; the patch reaches a little inside both so that the corner itself is covered. Only
// the curve is part of the outline.
Shape patch(P corner, P a, P a_in, P a_to, P b, P b_in, P b_to, double inset) {
    Shape shape;
    const P a_inner = add(a, mul(a_in, inset));
    const P b_inner = add(b, mul(b_in, inset));
    const P behind = add(corner, mul(unit(add(a_in, b_in)), inset * 1.5));
    shape.points.push_back(a_inner);
    shape.points.push_back(a);
    const std::size_t first = shape.points.size() - 1;
    // (the handles no longer than the shorter way to the corner: the curve never loops)
    const double handle = std::min(distance(a, corner), distance(b, corner)) * 2 / 3;
    const P c1 = add(a, mul(a_to, handle));
    const P c2 = add(b, mul(b_to, handle));
    const int pieces = corner_pieces(a, corner, b);
    for (int j = 1; j <= pieces; ++j) {
        const double t = static_cast<double>(j) / pieces;
        const double u = 1 - t;
        const double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        push(shape.points, {w0 * a.x + w1 * c1.x + w2 * c2.x + w3 * b.x, w0 * a.y + w1 * c1.y + w2 * c2.y + w3 * b.y});
    }
    const std::size_t last = shape.points.size() - 1;
    shape.points.push_back(b_inner);
    shape.points.push_back(behind);
    shape.stroke.assign(shape.points.size(), 0);
    for (std::size_t i = first; i < last; ++i) shape.stroke[i] = 1;
    return shape;
}

}  // namespace

// --- measuring --------------------------------------------------------------------------------------------------------

double distance(P a, P b) { return hyp(a.x - b.x, a.y - b.y); }

std::vector<double> lengths_of(const Contour& closed) {
    std::vector<double> lengths(closed.size() + 1, 0.0);
    for (std::size_t i = 0; i < closed.size(); ++i) lengths[i + 1] = lengths[i] + distance(closed[i], closed[(i + 1) % closed.size()]);
    return lengths;
}

namespace {

// The segment `s` lies on, and how far along it (0..1).
std::pair<std::size_t, double> place_of(const std::vector<double>& lengths, double s) {
    const double total = lengths.back();
    if (!(total > 0)) return {0, 0.0};
    s = std::fmod(s, total);
    if (s < 0) s += total;
    std::size_t i = static_cast<std::size_t>(std::upper_bound(lengths.begin(), lengths.end(), s) - lengths.begin());
    i = i == 0 ? 0 : i - 1;
    if (i >= lengths.size() - 1) i = lengths.size() - 2;
    const double span = lengths[i + 1] - lengths[i];
    return {i, span > 0 ? (s - lengths[i]) / span : 0.0};
}

}  // namespace

P point_at(const Contour& closed, const std::vector<double>& lengths, double s) {
    const auto [i, t] = place_of(lengths, s);
    return lerp(closed[i], closed[(i + 1) % closed.size()], t);
}

P direction_at(const Contour& closed, const std::vector<double>& lengths, double s) {
    const auto [i, t] = place_of(lengths, s);
    (void)t;
    return unit(sub(closed[(i + 1) % closed.size()], closed[i]));
}

bool inside(const Contour& closed, P point) {
    int winding = 0;
    const std::size_t n = closed.size();
    for (std::size_t i = 0; i < n; ++i) {
        const P a = closed[i];
        const P b = closed[(i + 1) % n];
        if (a.y <= point.y) {
            if (b.y > point.y && cross(sub(b, a), sub(point, a)) > 0) ++winding;
        } else if (b.y <= point.y && cross(sub(b, a), sub(point, a)) < 0) {
            --winding;
        }
    }
    return winding != 0;
}

double shoelace(const Contour& closed) {
    double sum = 0.0;
    const std::size_t n = closed.size();
    for (std::size_t i = 0; i < n; ++i) sum += cross(closed[i], closed[(i + 1) % n]);
    return sum;
}

double facing(const Contour& closed, const std::vector<double>& lengths, P from, P toward) {
    const P ray = sub(toward, from);
    if (hyp(ray.x, ray.y) < 1e-9) return 0.0;
    double best_t = -1.0;
    double best_s = 0.0;
    const std::size_t n = closed.size();
    for (std::size_t i = 0; i < n; ++i) {
        const P a = closed[i];
        const P b = closed[(i + 1) % n];
        const P seg = sub(b, a);
        const double den = cross(ray, seg);
        if (std::fabs(den) < 1e-12) continue;
        const P fa = sub(a, from);
        const double t = cross(fa, seg) / den;  // (along the ray)
        const double u = cross(fa, ray) / den;  // (along the segment)
        if (t <= 0 || u < 0 || u > 1) continue;
        if (t > best_t) {
            best_t = t;
            best_s = lengths[i] + (lengths[i + 1] - lengths[i]) * u;
        }
    }
    return best_s;
}

// --- the bodies -------------------------------------------------------------------------------------------------------

Contour ellipse(const Box& box) { return four_fold(quarter_ellipse(box.rx(), box.ry()), box.cx(), box.cy()); }

Contour circle(P centre, double radius) { return four_fold(quarter_ellipse(radius, radius), centre.x, centre.y); }

Contour rounded(const Box& box, double radius) {
    const double rx = box.rx();
    const double ry = box.ry();
    const double r = std::max(0.0, std::min(radius, std::min(rx, ry)));
    Contour quarter;
    quarter.push_back({0.0, -ry});
    if (r <= 0) {
        quarter.push_back({rx, -ry});
    } else {
        push(quarter, {rx - r, -ry});
        const std::vector<P> arc = unit_quarter(quarter_depth(r));
        for (std::size_t i = 1; i + 1 < arc.size(); ++i) push(quarter, {rx - r + r * arc[i].x, -ry + r - r * arc[i].y});
        push(quarter, {rx, -ry + r});
    }
    push(quarter, {rx, 0.0});
    // (a capsule's cap ends on the middle line: the quarter still runs from its top to its right)
    if (quarter.size() < 2) quarter = {{0.0, -ry}, {rx, 0.0}};
    return four_fold(quarter, box.cx(), box.cy());
}

Contour squircle(const Box& box) {
    const double rx = box.rx();
    const double ry = box.ry();
    const int n = std::clamp(static_cast<int>(std::ceil(std::sqrt(std::max(rx, ry)) * 5)), 32, 512);
    const double turn = 0.8408964152537145;  // (2^(−1/4): where |x| = |y| on the unit squircle)
    const auto fourth_root = [](double v) { return std::sqrt(std::sqrt(std::max(0.0, v))); };
    Contour quarter;
    for (int i = 0; i <= n; ++i) {
        const double u = turn * static_cast<double>(i) / static_cast<double>(n);
        const double u2 = u * u;
        quarter.push_back({rx * u, -ry * (i == n ? turn : fourth_root(1 - u2 * u2))});
    }
    for (int i = n - 1; i >= 0; --i) {
        const double v = turn * static_cast<double>(i) / static_cast<double>(n);
        const double v2 = v * v;
        quarter.push_back({rx * fourth_root(1 - v2 * v2), -ry * v});
    }
    quarter.front() = {0.0, -ry};
    quarter.back() = {rx, 0.0};
    return four_fold(quarter, box.cx(), box.cy());
}

Contour polygon(const Box& box, int sides, double radius) {
    const int n = std::clamp(sides, 5, 12);
    const bool odd = n % 2 != 0;
    // (the corners of the right half, a flat side up, on the unit circle: x to the right, y down)
    std::vector<P> corners;
    const int count = odd ? (n + 1) / 2 : n / 2;
    for (int k = 0; k < count; ++k) {
        const float angle = static_cast<float>(-kPi / 2 + kPi / n + 2 * kPi * k / n);
        corners.push_back({static_cast<double>(libm::cosf(angle)), static_cast<double>(libm::sinf(angle))});
    }
    if (odd) corners.back() = {0.0, 1.0};  // (the corner at the foot, on the middle line)
    double reach = 0.0;
    for (const P& c : corners) reach = std::max(reach, c.x);
    const double top = corners.front().y;
    const double foot = odd ? 1.0 : -top;
    const double rx = box.rx();
    const double ry = box.ry();
    for (P& c : corners) c = {c.x / reach * rx, -ry + (c.y - top) / (foot - top) * 2 * ry};
    Contour half;
    half.push_back({0.0, -ry});
    for (std::size_t k = 0; k < corners.size(); ++k) {
        const P here = corners[k];
        const P before = k == 0 ? P{-here.x, here.y} : corners[k - 1];
        const bool at_foot = odd && k + 1 == corners.size();
        const P after = k + 1 < corners.size() ? corners[k + 1] : (odd ? P{-corners[k - 1].x, corners[k - 1].y} : P{-here.x, here.y});
        const P in = unit(sub(here, before));
        const P out = unit(sub(after, here));
        const double cosine = std::clamp(dot(in, out), -0.999, 0.999);
        const double tangent = std::sqrt((1 - cosine) / (1 + cosine));  // (of half the turn)
        const double reach_here = std::min(radius * tangent, 0.4 * std::min(distance(before, here), distance(here, after)));
        if (reach_here < 0.25) {
            push(half, here);
            continue;
        }
        const P from = sub(here, mul(in, reach_here));
        const P to = add(here, mul(out, reach_here));
        push(half, from);
        round_corner(half, from, here, to, corner_pieces(from, here, to), at_foot ? 0.5 : 1.0);
        if (at_foot) half.back().x = 0.0;
    }
    if (!odd) push(half, {0.0, ry});
    const double cx = box.cx();
    const double cy = box.cy();
    return two_fold(half, [cx, cy](double x, double y, bool mirrored) { return P{mirrored ? cx - x : cx + x, cy + y}; });
}

namespace {

// Scallops of equal length round an ellipse (`keep` of the box) — a cloud's bumps, a flower's petals —, each `bulge`
// high against its width, a scallop's middle at the top; stretched to touch the box's four sides. `valleys_out`: where
// the scallops meet, on the page.
Contour scalloped(const Box& box, int n, double keep, double bulge, Contour* valleys_out = nullptr) {
    const double rx = box.rx();
    const double ry = box.ry();
    // (the valleys: at equal distances round an ellipse inside the box, a bump's middle at the top)
    const Contour base = four_fold(quarter_ellipse(rx * keep, ry * keep), 0.0, 0.0);
    const std::vector<double> lengths = lengths_of(base);
    const double total = lengths.back();
    std::vector<P> valleys;
    const bool odd = n % 2 != 0;
    const int count = odd ? (n + 1) / 2 : n / 2;
    for (int j = 0; j < count; ++j) {
        valleys.push_back(point_at(base, lengths, total * (static_cast<double>(j) + 0.5) / static_cast<double>(n)));
    }
    if (odd) valleys.back() = {0.0, ry * keep};
    const auto scallop = [bulge](Contour& points, P a, P b) {
        const P along = unit(sub(b, a));
        arc_to(points, a, b, {along.y, -along.x}, distance(a, b) * bulge);
    };
    Contour half;
    {  // (the right half of the top bump, from its top)
        const P a{-valleys.front().x, valleys.front().y};
        Contour whole_bump{a};
        scallop(whole_bump, a, valleys.front());
        const std::size_t middle = whole_bump.size() / 2;
        half.push_back({0.0, whole_bump[middle].y});
        for (std::size_t i = middle + 1; i < whole_bump.size(); ++i) push(half, whole_bump[i]);
    }
    for (std::size_t j = 0; j + 1 < valleys.size(); ++j) scallop(half, valleys[j], valleys[j + 1]);
    if (!odd) {  // (the right half of the bottom bump, to its foot)
        const P b{-valleys.back().x, valleys.back().y};
        Contour whole_bump{valleys.back()};
        scallop(whole_bump, valleys.back(), b);
        const std::size_t middle = whole_bump.size() / 2;
        for (std::size_t i = 1; i < middle; ++i) push(half, whole_bump[i]);
        half.push_back({0.0, whole_bump[middle].y});
    }
    // (stretched to touch the box's four sides)
    double reach = 0.0, top = half.front().y, foot = half.front().y;
    for (const P& p : half) {
        reach = std::max(reach, p.x);
        top = std::min(top, p.y);
        foot = std::max(foot, p.y);
    }
    if (!(reach > 0) || !(foot > top)) return {};  // (a box of no size: nothing)
    const double cx = box.cx();
    const double y0 = box.y0;
    const double high = box.y1 - box.y0;
    const auto place = [=](double x, double y, bool mirrored) {
        const double px = x / reach * rx;
        return P{mirrored ? cx - px : cx + px, y0 + (y - top) / (foot - top) * high};
    };
    if (valleys_out != nullptr) {  // (all the way round, clockwise from the first right of the top)
        valleys_out->clear();
        for (const P& v : valleys) valleys_out->push_back(place(v.x, v.y, false));
        for (std::size_t j = valleys.size() - (odd ? 1 : 0); j-- > 0;) valleys_out->push_back(place(valleys[j].x, valleys[j].y, true));
    }
    return two_fold(half, place);
}

// The right half of an outline given by its corners — the first and the last on the middle line x = 0 —, each corner
// rounded: `reach[i]` of both its sides taken by a curve whose control point is the corner (at most 0.4 of the shorter
// side; under a quarter of a pixel: the corner as it is). The corners on the middle line are rounded with their mirror
// image: the half begins and ends on the middle of their curves.
std::vector<P> rounded_half(const std::vector<P>& corners, const std::vector<double>& reach) {
    std::vector<P> half;
    const std::size_t n = corners.size();
    if (n < 2) return corners;
    for (std::size_t k = 0; k < n; ++k) {
        const P here = corners[k];
        const P before = k == 0 ? P{-corners[1].x, corners[1].y} : corners[k - 1];
        const P after = k + 1 < n ? corners[k + 1] : P{-corners[n - 2].x, corners[n - 2].y};
        const double taken = std::min(reach[k], 0.4 * std::min(distance(before, here), distance(here, after)));
        if (taken < 0.25) {
            push(half, here);
            continue;
        }
        const P from = sub(here, mul(unit(sub(here, before)), taken));
        const P to = add(here, mul(unit(sub(after, here)), taken));
        const int pieces = corner_pieces(from, here, to) * 2;  // (an even number: its middle is a point of it)
        const int first = k == 0 ? pieces / 2 : 0;
        const int last = k + 1 == n ? pieces / 2 : pieces;
        for (int j = first; j <= last; ++j) {
            const double t = static_cast<double>(j) / static_cast<double>(pieces);
            const double u = 1 - t;
            P p{u * u * from.x + 2 * u * t * here.x + t * t * to.x, u * u * from.y + 2 * u * t * here.y + t * t * to.y};
            if ((k == 0 && j == first) || (k + 1 == n && j == last)) p.x = 0.0;  // (on the middle line)
            push(half, p);
        }
    }
    return half;
}

// A half outline about the middle line (x = 0) made the whole outline, stretched to touch the box's four sides.
Contour fitted_two_fold(const std::vector<P>& half, const Box& box) {
    if (half.size() < 2) return {};
    double reach = 0.0, top = half.front().y, foot = half.front().y;
    for (const P& p : half) {
        reach = std::max(reach, p.x);
        top = std::min(top, p.y);
        foot = std::max(foot, p.y);
    }
    if (!(reach > 0) || !(foot > top)) return {};
    const double cx = box.cx();
    const double rx = box.rx();
    const double y0 = box.y0;
    const double high = box.y1 - box.y0;
    return two_fold(half, [=](double x, double y, bool mirrored) {
        const double px = x / reach * rx;
        return P{mirrored ? cx - px : cx + px, y0 + (y - top) / (foot - top) * high};
    });
}

// How much of a corner's two sides a rounding of `radius` takes: radius × the tangent of half the corner's turn.
double corner_reach(P before, P here, P after, double radius) {
    const double cosine = std::clamp(dot(unit(sub(here, before)), unit(sub(after, here))), -0.999, 0.999);
    return radius * std::sqrt((1 - cosine) / (1 + cosine));
}

// A flower's petals: scallops round an ellipse of 0.6 of the box, each 0.46 of its width high — a little less than half a
// circle (arc_to draws less than half a circle).
constexpr double kPetalKeep = 0.6;
constexpr double kPetalBulge = 0.46;

// A star's corners on the unit circle — its points at 1, its valleys at `inner`, the first point straight up, the
// right half from the top to the foot (2 × the half-steps: n + 1 corners) — and how they are laid in a box: x by
// 1 / reach of the half width, y from `top` .. `foot` to the box's height.
struct StarFrame {
    std::vector<P> corners;
    double reach = 1.0;
    double top = -1.0;
    double foot = 1.0;
};

StarFrame star_frame(int points, double inner) {
    StarFrame frame;
    const int n = std::clamp(points, 4, 12);
    const double in = std::clamp(inner, 0.2, 0.95);
    for (int j = 0; j <= n; ++j) {
        const double r = j % 2 == 0 ? 1.0 : in;
        if (j == 0) {
            frame.corners.push_back({0.0, -r});
        } else if (j == n) {
            frame.corners.push_back({0.0, r});
        } else {
            const float angle = static_cast<float>(-kPi / 2 + kPi * j / n);
            frame.corners.push_back({r * static_cast<double>(libm::cosf(angle)), r * static_cast<double>(libm::sinf(angle))});
        }
    }
    frame.reach = 0.0;
    frame.top = frame.corners.front().y;
    frame.foot = frame.corners.front().y;
    for (const P& c : frame.corners) {
        frame.reach = std::max(frame.reach, c.x);
        frame.top = std::min(frame.top, c.y);
        frame.foot = std::max(frame.foot, c.y);
    }
    return frame;
}

// (a star's corner laid in the box, about the box's middle)
P star_placed(const StarFrame& frame, const Box& box, P unit_point) {
    return {unit_point.x / frame.reach * box.rx(), -box.ry() + (unit_point.y - frame.top) / (frame.foot - frame.top) * 2 * box.ry()};
}

// A heart's right half about its middle line, in a square of side 2 (y down): a round lobe — the circle about
// (0.485, −0.5) of radius 0.515, from the cleft where it meets its mirror image, over its top and its right, to 20°
// below its middle — and from there a curve that leaves the lobe along it and comes to the point at the foot (0, 1) at
// 42° from the middle line. `scale_x`, `scale_y`: the size it is made for (how finely its curves are cut).
std::vector<P> heart_half(double scale_x, double scale_y, double* cleft_y = nullptr) {
    const double cx = 0.485, cy = -0.5, r = 0.515;
    const double cleft = cy - std::sqrt(r * r - cx * cx);
    if (cleft_y != nullptr) *cleft_y = cleft;
    const int depth = quarter_depth(std::max(scale_x, scale_y) * r) + 1;
    // (unit vectors from the lobe's middle, clockwise on the page: the cleft, straight up, to the right, 20° below)
    const P to_cleft = unit({-cx, cleft - cy});
    const double cos20 = 0.9396926207859084, sin20 = 0.3420201433256687;
    const P stops[4] = {to_cleft, {0.0, -1.0}, {1.0, 0.0}, {cos20, sin20}};
    std::vector<P> half;
    half.push_back({0.0, cleft});
    for (int piece = 0; piece < 3; ++piece) {
        std::vector<P> arc{stops[piece], stops[piece + 1]};
        for (int d = 0; d < depth; ++d) {
            std::vector<P> next;
            next.reserve(arc.size() * 2 - 1);
            for (std::size_t i = 0; i + 1 < arc.size(); ++i) {
                next.push_back(arc[i]);
                next.push_back(unit(add(arc[i], arc[i + 1])));
            }
            next.push_back(arc.back());
            arc = std::move(next);
        }
        for (std::size_t i = 1; i < arc.size(); ++i) push(half, {cx + r * arc[i].x, cy + r * arc[i].y});
    }
    const P from{cx + r * cos20, cy + r * sin20};
    const P tip{0.0, 1.0};
    const double sin42 = 0.6691306063588582, cos42 = 0.7431448254773942;
    const double handle = distance(from, tip) * 0.4;
    const P c1 = add(from, mul({-sin20, cos20}, handle));
    const P c2{sin42 * handle, 1.0 - cos42 * handle};
    const int pieces = std::clamp(static_cast<int>(std::ceil(std::sqrt(std::max(scale_x, scale_y)) * 3)), 12, 96);
    for (int j = 1; j <= pieces; ++j) {
        const double t = static_cast<double>(j) / pieces;
        const double u = 1 - t;
        const double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        push(half, {w0 * from.x + w1 * c1.x + w2 * c2.x + w3 * tip.x, w0 * from.y + w1 * c1.y + w2 * c2.y + w3 * tip.y});
    }
    half.back() = tip;
    return half;
}

}  // namespace

Contour cloud(const Box& box, int bumps) {
    const int n = std::clamp(bumps, 5, 60);
    return scalloped(box, n, 0.86, 0.3);  // (a scallop's height against its width: 0.3)
}

// --- fukidashi@2: the symbols --------------------------------------------------------------------------------------------

Contour diamond(const Box& box, double radius) {
    const double rx = box.rx();
    const double ry = box.ry();
    if (!(rx > 0) || !(ry > 0)) return {};
    const std::vector<P> corners{{0.0, -ry}, {rx, 0.0}, {0.0, ry}};
    const double r = radius >= 0 ? radius : std::min(rx, ry) * 0.08;
    std::vector<double> reach;
    reach.push_back(corner_reach({-rx, 0.0}, corners[0], corners[1], r));
    reach.push_back(corner_reach(corners[0], corners[1], corners[2], r));
    reach.push_back(corner_reach(corners[1], corners[2], {-rx, 0.0}, r));
    return fitted_two_fold(rounded_half(corners, reach), box);
}

Contour star(const Box& box, int points, double inner, double radius) {
    const double rx = box.rx();
    const double ry = box.ry();
    if (!(rx > 0) || !(ry > 0)) return {};
    const StarFrame frame = star_frame(points, inner);
    std::vector<P> corners;
    for (const P& c : frame.corners) corners.push_back(star_placed(frame, box, c));
    corners.front().x = 0.0;
    corners.back().x = 0.0;
    const double least = std::min(rx, ry);
    const std::size_t n = corners.size();
    std::vector<double> reach(n, 0.0);
    for (std::size_t k = 0; k < n; ++k) {
        const P here = corners[k];
        const P before = k == 0 ? P{-corners[1].x, corners[1].y} : corners[k - 1];
        const P after = k + 1 < n ? corners[k + 1] : P{-corners[n - 2].x, corners[n - 2].y};
        const bool point = k % 2 == 0;
        if (radius >= 0) {
            reach[k] = corner_reach(before, here, after, point ? radius : radius / 2);
        } else if (point) {
            // (its own: the point cut short by 0.035 of the narrow half — the curve's top lies half the way from the
            // corner to the middle of where it begins and ends)
            const double cosine = std::clamp(dot(unit(sub(here, before)), unit(sub(after, here))), -0.999, 0.999);
            const double half_sine = std::sqrt((1 - cosine) / 2);  // (the sine of half the turn: the cosine of half the point's angle)
            reach[k] = 2 * least * 0.035 / std::max(0.2, half_sine);
        } else {
            reach[k] = corner_reach(before, here, after, least * 0.05);
        }
    }
    return fitted_two_fold(rounded_half(corners, reach), box);
}

P star_centre(const Box& box, int points, double inner) {
    const StarFrame frame = star_frame(points, inner);
    const P at = star_placed(frame, box, {0.0, 0.0});
    return {box.cx() + at.x, box.cy() + at.y};
}

Contour star_guide(const Box& box, int points, double inner) {
    const StarFrame frame = star_frame(points, inner);
    const int n = std::clamp(points, 4, 12);
    const double in = std::clamp(inner, 0.2, 0.95);
    // (inside the chords between its valleys: their middles lie at inner × cos(π / n))
    const double keep = in * static_cast<double>(libm::cosf(static_cast<float>(kPi / n))) * 0.97;
    const P centre = star_centre(box, points, inner);
    const double a = keep / frame.reach * box.rx();
    const double b = keep / (frame.foot - frame.top) * 2 * box.ry();
    return ellipse(Box{centre.x - a, centre.y - b, centre.x + a, centre.y + b});
}

Contour flower(const Box& box, int petals) { return scalloped(box, std::clamp(petals, 4, 12), kPetalKeep, kPetalBulge); }

Contour flower_guide(const Box& box, int petals) {
    Contour valleys;
    if (scalloped(box, std::clamp(petals, 4, 12), kPetalKeep, kPetalBulge, &valleys).empty() || valleys.empty()) return {};
    const double rx = box.rx(), ry = box.ry();
    double keep = 1.0;
    for (const P& v : valleys) keep = std::min(keep, hyp((v.x - box.cx()) / rx, (v.y - box.cy()) / ry));
    return scaled_about(ellipse(box), {box.cx(), box.cy()}, keep * 0.97);
}

Contour heart(const Box& box, double radius) {
    const double rx = box.rx();
    const double ry = box.ry();
    if (!(rx > 0) || !(ry > 0)) return {};
    std::vector<P> half = heart_half(rx, ry);
    // (laid in the box — its lobes' top at the box's top, its point at the foot —, then its point rounded)
    double reach = 0.0, top = half.front().y;
    for (const P& p : half) {
        reach = std::max(reach, p.x);
        top = std::min(top, p.y);
    }
    for (P& p : half) p = {p.x / reach * rx, -ry + (p.y - top) / (1.0 - top) * 2 * ry};
    const double r = radius >= 0 ? radius : std::min(rx, ry) * 0.05;
    if (r > 0 && half.size() > 3) {
        // (the point: the last stretch of its side up to `taken` from it, then a curve to the middle line)
        const P tip = half.back();
        const P toward = unit(sub(tip, half[half.size() - 2]));
        const double cosine = std::clamp(toward.x * toward.x - toward.y * toward.y, -0.999, 0.999);  // (the side against its mirror image)
        const double taken = std::min(r * std::sqrt((1 - cosine) / (1 + cosine)), 0.2 * ry);
        while (half.size() > 2 && distance(half.back(), tip) < taken) half.pop_back();
        const P from = half.back();
        const P to{-from.x, from.y};
        const int pieces = corner_pieces(from, tip, to) * 2;
        for (int j = 1; j <= pieces / 2; ++j) {
            const double t = static_cast<double>(j) / static_cast<double>(pieces);
            const double u = 1 - t;
            push(half, {u * u * from.x + 2 * u * t * tip.x + t * t * to.x, u * u * from.y + 2 * u * t * tip.y + t * t * to.y});
        }
        half.back().x = 0.0;
    }
    return fitted_two_fold(half, box);
}

Contour symbol_room(std::string_view kind, const Box& box, int points, double inner) {
    const double rx = box.rx();
    const double ry = box.ry();
    const double cx = box.cx();
    const double cy = box.cy();
    if (!(rx > 0) || !(ry > 0)) return {};
    if (kind == "diamond") return {{cx, cy - ry}, {cx + rx, cy}, {cx, cy + ry}, {cx - rx, cy}};
    if (kind == "star") {
        const StarFrame frame = star_frame(points, inner);
        Contour room;
        const std::size_t n = frame.corners.size();
        for (std::size_t k = 1; k < n; k += 2) {  // (the valleys of the right half, then their mirror images back up)
            const P at = star_placed(frame, box, frame.corners[k]);
            room.push_back({cx + (k + 1 == n ? 0.0 : at.x), cy + at.y});
        }
        for (std::size_t k = (n % 2 == 0 ? n - 3 : n - 2); k < n; k -= 2) {
            const P at = star_placed(frame, box, frame.corners[k]);
            room.push_back({cx - at.x, cy + at.y});
            if (k < 2) break;
        }
        return room;
    }
    if (kind == "flower") {
        Contour valleys;
        (void)scalloped(box, std::clamp(points, 4, 12), kPetalKeep, kPetalBulge, &valleys);
        return valleys;
    }
    if (kind == "heart") {
        double cleft = 0.0;
        std::vector<P> half = heart_half(rx, ry, &cleft);
        double reach = 0.0, top = half.front().y;
        for (const P& p : half) {
            reach = std::max(reach, p.x);
            top = std::min(top, p.y);
        }
        // (below the cleft's level: the lobes from where they come down past it, the sides, the point)
        Contour right;
        bool below = false;
        for (std::size_t i = 1; i < half.size(); ++i) {
            const bool down = half[i].y > half[i - 1].y;
            if (!below && down && half[i].y >= cleft) {
                const double t = (cleft - half[i - 1].y) / (half[i].y - half[i - 1].y);
                right.push_back({half[i - 1].x + (half[i].x - half[i - 1].x) * t, cleft});
                below = true;
            }
            if (below) right.push_back(half[i]);
        }
        Contour room;
        const auto place = [&](P p, bool mirrored) {
            const double px = p.x / reach * rx;
            return P{mirrored ? cx - px : cx + px, cy - ry + (p.y - top) / (1.0 - top) * 2 * ry};
        };
        // (every eighth point is enough for a room)
        const std::size_t step = std::max<std::size_t>(1, right.size() / 24);
        Contour kept;
        for (std::size_t i = 0; i < right.size(); i += step) kept.push_back(right[i]);
        if (kept.empty() || kept.back().y < 1.0) kept.push_back({0.0, 1.0});
        for (const P& p : kept) room.push_back(place(p, false));
        for (std::size_t i = kept.size() - 1; i-- > 0;) room.push_back(place(kept[i], true));
        return room;
    }
    return {};
}

Contour burst(const Box& box, int spikes, double depth, double alt, const std::vector<double>& jitter) {
    int n = std::max(3, spikes);
    if (alt > 0 && n % 2 != 0) ++n;  // (long and short by turns: an even number)
    const double rx = box.rx();
    const double ry = box.ry();
    const Contour base = four_fold(quarter_ellipse(rx, ry), 0.0, 0.0);
    const std::vector<double> lengths = lengths_of(base);
    const double total = lengths.back();
    const auto tip = [&](int i) {
        double reach = 1.0;
        if (alt > 0 && i % 2 != 0) reach -= depth * alt;
        if (static_cast<std::size_t>(i) < jitter.size()) reach += jitter[static_cast<std::size_t>(i)];
        return mul(point_at(base, lengths, total * static_cast<double>(i) / static_cast<double>(n)), reach);
    };
    const auto valley = [&](int i) {
        return mul(point_at(base, lengths, total * (static_cast<double>(i) + 0.5) / static_cast<double>(n)), 1 - depth);
    };
    const double cx = box.cx();
    const double cy = box.cy();
    if (!jitter.empty()) {  // (each point its own reach: not a mirror image)
        Contour out;
        for (int i = 0; i < n; ++i) {
            out.push_back(add(tip(i), {cx, cy}));
            out.push_back(add(valley(i), {cx, cy}));
        }
        return out;
    }
    Contour half;
    const bool odd = n % 2 != 0;
    half.push_back({0.0, -ry});
    for (int i = 0; 2 * i + 1 <= n; ++i) {
        if (i > 0) half.push_back(tip(i));
        if (2 * i + 1 == n) {
            half.push_back({0.0, ry * (1 - depth)});  // (an odd number: a valley at the foot)
        } else {
            half.push_back(valley(i));
        }
    }
    if (!odd) half.push_back({0.0, ry * (alt > 0 && (n / 2) % 2 != 0 ? 1 - depth * alt : 1.0)});
    return two_fold(half, [cx, cy](double x, double y, bool mirrored) { return P{mirrored ? cx - x : cx + x, cy + y}; });
}

Contour sawtooth(const Box& box, int teeth, double depth) {
    const int n = std::max(3, teeth);
    Box about{-box.rx(), -box.ry(), box.rx(), box.ry()};
    const Contour base = squircle(about);
    const std::vector<double> lengths = lengths_of(base);
    const double total = lengths.back();
    Contour out;
    const double cx = box.cx();
    const double cy = box.cy();
    for (int i = 0; i < n; ++i) {
        const P outer = point_at(base, lengths, total * static_cast<double>(i) / static_cast<double>(n));
        // (the inner corners lag: a sawtooth, not a star)
        const P inner = mul(point_at(base, lengths, total * (static_cast<double>(i) + 0.675) / static_cast<double>(n)), 1 - depth);
        out.push_back({cx + outer.x, cy + outer.y});
        out.push_back({cx + inner.x, cy + inner.y});
    }
    return out;
}

Contour smooth_closed(const Contour& points, double tolerance) {
    const std::size_t n = points.size();
    if (n < 3) return points;
    Contour out;
    for (std::size_t i = 0; i < n; ++i) {
        const P& p0 = points[(i + n - 1) % n];
        const P& p1 = points[i];
        const P& p2 = points[(i + 1) % n];
        const P& p3 = points[(i + 2) % n];
        const double span = distance(p1, p2);
        const int per = std::clamp(static_cast<int>(std::ceil(std::sqrt(span / std::max(tolerance, 1e-3)) * 0.6)), 6, 96);
        for (int s = 0; s < per; ++s) {
            const double t = static_cast<double>(s) / per;
            const double t2 = t * t;
            const double t3 = t2 * t;
            const auto at = [&](double a, double b, double c, double d) {
                return 0.5 * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3);
            };
            push(out, {at(p0.x, p1.x, p2.x, p3.x), at(p0.y, p1.y, p2.y, p3.y)});
        }
    }
    return out;
}

Contour waved(const Contour& closed, double amplitude, double wavelength) {
    const std::vector<double> lengths = lengths_of(closed);
    const double total = lengths.back();
    if (!(amplitude > 0) || !(total > 0) || !(wavelength > 0)) return closed;
    const auto waves = static_cast<std::int64_t>(std::max(4.0, std::floor(total / wavelength + 0.5)));
    const std::int64_t per = 24;  // (points a wave)
    const std::int64_t count = waves * per;
    const bool clockwise = shoelace(closed) > 0;  // (y down: a positive sum runs clockwise on the page)
    Contour out;
    out.reserve(static_cast<std::size_t>(count));
    const double step = total / static_cast<double>(count);
    for (std::int64_t i = 0; i < count; ++i) {
        const double s = step * static_cast<double>(i);
        const P here = point_at(closed, lengths, s);
        const P along = unit(sub(point_at(closed, lengths, s + step / 2), point_at(closed, lengths, s - step / 2)));
        const P in = clockwise ? inward(along) : mul(inward(along), -1.0);
        // (a crest where the outline begins — its top — so a balloon that is its own mirror image stays one)
        const float phase = static_cast<float>(2 * kPi * static_cast<double>(i % per) / static_cast<double>(per));
        const double push_out = amplitude * static_cast<double>(libm::cosf(phase));
        out.push_back(sub(here, mul(in, push_out)));
    }
    return out;
}

Contour scaled_about(const Contour& closed, P centre, double factor) {
    Contour out;
    out.reserve(closed.size());
    for (const P& p : closed) out.push_back({centre.x + (p.x - centre.x) * factor, centre.y + (p.y - centre.y) * factor});
    return out;
}

// --- shapes and their tails ------------------------------------------------------------------------------------------

Shape whole(Contour points, bool cut, int owner) {
    Shape shape;
    shape.stroke.assign(points.size(), 1);
    shape.points = std::move(points);
    shape.cut = cut;
    shape.owner = owner;
    return shape;
}

namespace {

// Where a line, walked from its first point, first meets the closed outline: on which of its segments and how far
// along it, the point, and how far along the outline that is.
struct Meeting {
    bool found = false;
    std::size_t segment = 0;
    double along = 0.0;
    P at;
    double s = 0.0;
};

Meeting first_meeting(const Contour& line, const Contour& outline, const std::vector<double>& lengths) {
    Meeting out;
    if (line.size() < 2 || outline.size() < 3) return out;
    const Bounds reach = bounds_of(outline);
    for (std::size_t i = 0; i + 1 < line.size() && !out.found; ++i) {
        const P a = line[i];
        const P b = line[i + 1];
        if (std::max(a.x, b.x) < reach.x0 || std::min(a.x, b.x) > reach.x1 || std::max(a.y, b.y) < reach.y0 || std::min(a.y, b.y) > reach.y1) continue;
        double best = 2.0;
        for (std::size_t j = 0; j < outline.size(); ++j) {
            double t = 0.0, u = 0.0;
            if (!crossing(a, b, outline[j], outline[(j + 1) % outline.size()], t, u) || t >= best) continue;
            best = t;
            out.s = lengths[j] + (lengths[j + 1] - lengths[j]) * std::clamp(u, 0.0, 1.0);
        }
        if (best <= 1.0) {
            out.found = true;
            out.segment = i;
            out.along = std::clamp(best, 0.0, 1.0);
            out.at = lerp(a, b, out.along);
        }
    }
    return out;
}

// How far a join may reach along the closed outline from `s` (forward for way > 0, back otherwise), up to `reach`: it
// stops short of the outline's next corner (a turn of more than about 20°).
double smooth_reach(const Contour& outline, const std::vector<double>& lengths, double s, double way, double reach) {
    const std::size_t n = outline.size();
    const auto [index, t] = place_of(lengths, s);
    const auto span = [&](std::size_t k) { return lengths[k + 1] - lengths[k]; };
    const auto direction = [&](std::size_t k) { return unit(sub(outline[(k + 1) % n], outline[k])); };
    std::size_t k = index;
    double walked = way > 0 ? (1 - t) * span(k) : t * span(k);
    for (std::size_t guard = 0; walked < reach && guard < n; ++guard) {
        const std::size_t next = way > 0 ? (k + 1) % n : (k + n - 1) % n;
        if (span(next) > 1e-9 && span(k) > 1e-9 && dot(direction(k), direction(next)) < 0.94) return walked * 0.9;
        walked += span(next);
        k = next;
    }
    return std::min(reach, walked);
}

// How far an open line may be followed from its place (`segment`, `along`) before its next corner (a turn of more than
// about 20°) or its end, up to `reach`.
double corner_free(const Contour& line, std::size_t segment, double along, double reach) {
    double walked = distance(lerp(line[segment], line[segment + 1], along), line[segment + 1]);
    P direction = unit(sub(line[segment + 1], line[segment]));
    for (std::size_t i = segment + 1; i + 1 < line.size() && walked < reach; ++i) {
        const double span = distance(line[i], line[i + 1]);
        if (span < 1e-9) continue;
        const P next = unit(sub(line[i + 1], line[i]));
        if (dot(direction, next) < 0.94) return walked * 0.9;
        direction = next;
        walked += span;
    }
    return std::min(reach, walked);
}

// The point `reach` along an open line from its place, and the line's direction there.
P point_along(const Contour& line, std::size_t segment, double along, double reach, P& direction) {
    P here = lerp(line[segment], line[segment + 1], along);
    direction = unit(sub(line[segment + 1], line[segment]));
    for (std::size_t i = segment; i + 1 < line.size(); ++i) {
        const double span = distance(here, line[i + 1]);
        if (distance(line[i], line[i + 1]) > 1e-9) direction = unit(sub(line[i + 1], line[i]));
        if (span >= reach) return lerp(here, line[i + 1], span > 0 ? reach / span : 0.0);
        reach -= span;
        here = line[i + 1];
    }
    return here;
}

}  // namespace

Tail tail(const Contour& outline, const Contour& guide, P centre, const TailSpec& spec) {
    Tail out;
    if (outline.size() < 3 || guide.size() < 3) return out;
    const std::vector<double> guide_lengths = lengths_of(guide);
    const std::vector<double> lengths = lengths_of(outline);
    const bool bent = !spec.bends.empty();
    const bool notch = spec.kind == "inward";
    // its root: where the line from the balloon's middle to what it points at leaves the outline — a tail points from
    // the middle of its balloon at the speaker — and the way out of the balloon there, across the outline
    const P target = bent ? spec.bends.front() : (spec.has_via ? spec.via : spec.tip);
    const double at = facing(guide, guide_lengths, centre, target);
    const P root = point_at(guide, guide_lengths, at);
    const double asked = std::max(0.5, std::min(spec.base, guide_lengths.back() * 0.25) / 2);
    double half = asked;
    // (across the chord under the tail's base, half its width either side of the root: the same for a root on a corner
    // of the outline's many short edges as for one in the middle of an edge, and a mirror image for a mirrored tail)
    const P along_outline = unit(sub(point_at(guide, guide_lengths, at + asked), point_at(guide, guide_lengths, at - asked)));
    const P out_of = shoelace(guide) > 0 ? mul(inward(along_outline), -1.0) : inward(along_outline);
    const P normal = notch ? mul(out_of, -1.0) : out_of;  // (a notch runs into the balloon)
    const double depth = std::max(1.0, spec.depth);
    out.left = root;
    out.right = root;
    if (distance(root, spec.tip) < 1e-6) return out;

    // As wide as asked where it leaves the balloon: its sides run straight on into the balloon, wider there, and the
    // outline falls away from the root — so it is made, measured across where its sides meet the outline, and made again
    // that much slimmer (up to five times: within a hundredth of what was asked).
    Contour left_side, right_side;
    Meeting left, right;
    for (int pass = 0; pass < 6; ++pass) {
        Contour middle;
        std::vector<double> wide;
        left_side.clear();
        right_side.clear();
        // its middle line, from inside the balloon (a notch: from outside it) through the root to the tip, and how wide
        // the tail is at each of its points: as wide as asked at the root, slimming to the tip — and inside the balloon as
        // much wider as it slims outside, so that its sides pass through the outline without a corner there
        const auto within = [&](double slimming, double length) { return std::min(2.0, 1 + slimming * depth / std::max(1e-6, length)); };
        if (bent || spec.bolt) {  // (折れ線のしっぽ, a lightning bolt: straight between its bends)
            Contour path{root};
            if (bent) {
                path.insert(path.end(), spec.bends.begin(), spec.bends.end());
            } else {  // (the middle line steps aside twice, the same way for every bolt)
                const P across = inward(unit(sub(spec.tip, root)));
                path.push_back(add(lerp(root, spec.tip, 0.38), mul(across, half * 0.9)));
                path.push_back(add(lerp(root, spec.tip, 0.62), mul(across, -half * 0.6)));
            }
            path.push_back(spec.tip);
            std::vector<double> run{0.0};
            for (std::size_t i = 0; i + 1 < path.size(); ++i) run.push_back(run.back() + distance(path[i], path[i + 1]));
            const double length = std::max(1e-6, run.back());
            middle.push_back(sub(root, mul(unit(sub(path[1], path[0])), depth)));
            wide.push_back(half * within(1.0, length));
            for (std::size_t i = 0; i < path.size(); ++i) {
                middle.push_back(path[i]);
                wide.push_back(half * (1 - run[i] / length));
            }
        } else if (spec.kind == "straight") {
            const P along = unit(sub(spec.tip, root));
            middle.push_back(sub(root, mul(along, depth)));
            wide.push_back(half * within(1.0, distance(root, spec.tip)));
            const int steps = 16;
            for (int i = 0; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                middle.push_back(lerp(root, spec.tip, t));
                wide.push_back(half * (1 - t));
            }
        } else {
            // a curve that leaves the balloon straight across its outline and bends to the tip (through `via`, when the
            // tail has one): no bend at all when the tip lies straight out, the same bend for the same place
            P c1, c2;
            if (spec.has_via) {
                c1 = add(root, mul(normal, distance(root, spec.via) * 2 / 3));
                c2 = lerp(spec.tip, spec.via, 2.0 / 3);
            } else {
                const double reach = distance(root, spec.tip);
                const double forward = std::max(dot(sub(spec.tip, root), normal), reach * 0.2);
                const P bend = add(root, mul(normal, forward * 0.5));
                c1 = lerp(root, bend, 2.0 / 3);
                c2 = lerp(spec.tip, bend, 2.0 / 3);
            }
            const bool zigzag = spec.kind == "zigzag";
            const double length = distance(root, c1) + distance(c1, c2) + distance(c2, spec.tip);
            const int steps = zigzag ? 16 : std::clamp(static_cast<int>(std::ceil(length / 4)), 24, 200);
            middle.push_back(sub(root, mul(normal, depth)));
            wide.push_back(half * within(zigzag ? 1.0 : 1.1, length));
            // (a zigzag toward its tip: from the first neck too slim to show white between its two lines, it runs to its
            // point without stepping out again — a step out past such a neck would be a small closed "eye" of white.
            // spec.line 0 — the width not said, or no line — leaves every step)
            bool closing = false;
            for (int i = 0; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                const double u = 1 - t;
                const double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
                middle.push_back({w0 * root.x + w1 * c1.x + w2 * c2.x + w3 * spec.tip.x, w0 * root.y + w1 * c1.y + w2 * c2.y + w3 * spec.tip.y});
                // (full at the root, slimming evenly to a point; a zigzag's sides step in and out)
                double width = half * static_cast<double>(libm::powf(static_cast<float>(u), 1.1f));
                if (zigzag && i > 0) {
                    const double neck = half * u * 0.55;
                    // (a neck whose white is slimmer than the line itself — its two lines 2 × line of its 2 × neck — reads as closed)
                    if (!closing && i % 2 == 0 && spec.line > 0 && neck < spec.line * 1.5) closing = true;
                    width = closing ? neck : half * u * (i % 2 != 0 ? 1.6 : 0.55);
                }
                wide.push_back(i == steps ? 0.0 : width);
            }
        }
        // its two sides: the middle line's points moved across it by the width there (a bend's corner mitred)
        const std::size_t count = middle.size();
        for (std::size_t i = 0; i + 1 < count; ++i) {
            const P before = unit(sub(middle[i], middle[i == 0 ? 0 : i - 1]));
            const P after = unit(sub(middle[i + 1], middle[i]));
            const P run = i == 0 ? after : unit(add(before, after));
            const double cosine = i == 0 ? 1.0 : dot(run, after);
            const double scale = std::min(2.5, 1 / std::max(0.4, cosine));
            const P across = mul(inward(run), wide[i] * scale);
            left_side.push_back(add(middle[i], across));
            right_side.push_back(sub(middle[i], across));
        }
        left_side.push_back(spec.tip);
        right_side.push_back(spec.tip);
        // where its sides leave the balloon's outline: the two ends of its base
        left = first_meeting(left_side, outline, lengths);
        right = first_meeting(right_side, outline, lengths);
        if (!left.found || !right.found) break;
        const double across = distance(left.at, right.at);
        if (across < 1e-6 || std::fabs(across - 2 * asked) <= 0.01 * asked) break;
        half = std::clamp(half * (2 * asked / across), asked * 0.5, asked);
    }

    Shape& shape = out.shape;
    shape.cut = notch;
    for (const P& p : left_side) push(shape.points, p);
    for (std::size_t i = right_side.size() - 1; i-- > 0;) push(shape.points, right_side[i]);
    shape.stroke.assign(shape.points.size(), 1);
    shape.stroke.back() = 0;  // (across its base, inside the balloon: not part of the outline)

    if (left.found) out.left = left.at;
    if (right.found) out.right = right.at;
    if (notch || !(spec.fillet > 0.5) || inside(outline, spec.tip)) return out;  // (a tip inside its balloon: nothing to join)

    // the joins: the notch between the outline and each side rounded a little, from the outline round to the side,
    // leaving both along their own directions. The outline itself is as it was everywhere else.
    const double total = lengths.back();
    const auto join = [&](const Contour& side, const Contour& other, const Meeting& met, const Meeting& far_side) {
        if (!met.found) return;
        // (the way along the outline that leads away from the tail)
        double way = 1.0;
        if (far_side.found) {
            double gap = std::fmod(met.s - far_side.s, total);
            if (gap > total / 2) gap -= total;
            if (gap < -total / 2) gap += total;
            way = gap < 0 ? -1.0 : 1.0;
        }
        const double on_outline = smooth_reach(outline, lengths, met.s, way, spec.fillet);
        const double on_tail = corner_free(side, met.segment, met.along, spec.fillet);
        P side_direction;
        const P on_side = point_along(side, met.segment, met.along, on_tail, side_direction);
        if (on_outline < 0.5 || on_tail < 0.5) return;
        const P on_body = point_at(outline, lengths, met.s + way * on_outline);
        const P body_direction = direction_at(outline, lengths, met.s + way * on_outline);
        const P body_in = shoelace(outline) > 0 ? inward(body_direction) : mul(inward(body_direction), -1.0);
        // (the tail's inside, across its side: toward its other side)
        P tail_in = inward(side_direction);
        const std::size_t opposite = std::min(met.segment + 1, other.size() - 1);
        if (dot(tail_in, sub(other[opposite], side[std::min(met.segment + 1, side.size() - 1)])) < 0) tail_in = mul(tail_in, -1.0);
        out.joins.push_back(patch(met.at, on_body, body_in, mul(body_direction, -way), on_side, tail_in, mul(side_direction, -1.0),
                                  std::max(1.0, std::min(on_outline, on_tail) * 0.35)));
    };
    join(left_side, right_side, left, right);
    join(right_side, left_side, right, left);
    return out;
}

std::vector<Contour> bubble_trail(const Contour& guide, P centre, P tip, double radius) {
    const std::vector<double> lengths = lengths_of(guide);
    const P edge = point_at(guide, lengths, facing(guide, lengths, centre, tip));
    std::vector<Contour> out;
    const double shares[3] = {0.3, 0.6, 0.88};
    for (int k = 0; k < 3; ++k) {
        const double r = std::max(2.0, radius * (1.0 - k * 0.25));
        out.push_back(circle(lerp(edge, tip, shares[k]), r));
    }
    return out;
}

std::vector<Shape> joins_between(const Contour& a, const Contour& b, double reach) {
    std::vector<Shape> out;
    if (a.size() < 3 || b.size() < 3 || reach < 0.5) return out;
    const Bounds ba = bounds_of(a);
    const Bounds bb = bounds_of(b);
    if (ba.x1 < bb.x0 || bb.x1 < ba.x0 || ba.y1 < bb.y0 || bb.y1 < ba.y0) return out;
    const std::vector<double> la = lengths_of(a);
    const std::vector<double> lb = lengths_of(b);
    const bool a_clockwise = shoelace(a) > 0;
    const bool b_clockwise = shoelace(b) > 0;
    const double limit = std::min(reach, 0.2 * std::min(la.back(), lb.back()));
    const double inset = std::max(1.5, limit * 0.35);
    struct Cross {
        double sa, sb;
        P at;
    };
    std::vector<Cross> crossings;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const P a0 = a[i];
        const P a1 = a[(i + 1) % a.size()];
        if (std::max(a0.x, a1.x) < bb.x0 || std::min(a0.x, a1.x) > bb.x1 || std::max(a0.y, a1.y) < bb.y0 || std::min(a0.y, a1.y) > bb.y1) continue;
        for (std::size_t j = 0; j < b.size(); ++j) {
            double t = 0.0, u = 0.0;
            if (!crossing(a0, a1, b[j], b[(j + 1) % b.size()], t, u)) continue;
            if (t >= 1.0 || u >= 1.0) continue;  // (an end shared by two segments: counted once)
            crossings.push_back({la[i] + (la[i + 1] - la[i]) * std::max(0.0, t), lb[j] + (lb[j + 1] - lb[j]) * std::max(0.0, u), lerp(a0, a1, std::clamp(t, 0.0, 1.0))});
        }
    }
    for (const Cross& c : crossings) {
        // (the way along each outline that leaves the other)
        const double probe = 0.75;
        const double way_a = inside(b, point_at(a, la, c.sa + probe)) ? -1.0 : 1.0;
        const double way_b = inside(a, point_at(b, lb, c.sb + probe)) ? -1.0 : 1.0;
        const double sharp = sharpness(unit(sub(point_at(a, la, c.sa + way_a * limit), c.at)), unit(sub(point_at(b, lb, c.sb + way_b * limit), c.at)));
        const double span = std::min(limit * sharp, 0.2 * std::min(la.back(), lb.back()));
        const P on_a = point_at(a, la, c.sa + way_a * span);
        const P on_b = point_at(b, lb, c.sb + way_b * span);
        if (inside(b, on_a) || inside(a, on_b)) continue;  // (the outlines meet again before the join ends)
        const P da = direction_at(a, la, c.sa + way_a * span);
        const P db = direction_at(b, lb, c.sb + way_b * span);
        const P a_in = a_clockwise ? inward(da) : mul(inward(da), -1.0);
        const P b_in = b_clockwise ? inward(db) : mul(inward(db), -1.0);
        out.push_back(patch(c.at, on_a, a_in, mul(da, -way_a), on_b, b_in, mul(db, -way_b), inset));
    }
    return out;
}

Shape drip(double x, double root_y, double left_top, double right_top, double half, double length) {
    Shape shape;
    if (!(half > 0) || !(length > 0)) return shape;
    // its right side, from the root down, as (half width, how far down): then the round end, and the left side its
    // mirror image
    std::vector<P> side;
    side.push_back({half, 0.0});
    // (a side that changes width between two heights: leaving and arriving straight down)
    const auto eased = [&](double w0, double y0, double w1, double y1) {
        if (y1 - y0 < 0.5) return;
        const int steps = std::clamp(static_cast<int>(std::ceil((y1 - y0) / 2)), 6, 64);
        const double ym = (y0 + y1) / 2;
        for (int j = 1; j <= steps; ++j) {
            const double t = static_cast<double>(j) / steps;
            const double u = 1 - t;
            const double b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
            push(side, {(b0 + b1) * w0 + (b2 + b3) * w1, b0 * y0 + (b1 + b2) * ym + b3 * y1});
        }
    };
    double end = 0.0;     // (the round end's radius)
    double middle = 0.0;  // (and how far down its middle lies)
    if (length >= 3.2 * half) {
        // a neck of 0.6 of the root's width, then a drop of 0.9
        end = 0.9 * half;
        middle = length - end;
        const double neck = 0.6 * half;
        const double flare = std::min(1.3 * half, middle * 0.35);
        const double swell = std::max(flare, middle - 1.5 * half);
        eased(half, 0.0, neck, flare);
        if (swell > flare) push(side, {neck, swell});
        eased(neck, swell, end, middle);
    } else {
        end = std::min(0.85 * half, length);
        middle = length - end;
        eased(half, 0.0, end, middle);
    }
    if (side.back().x != end || side.back().y != middle) push(side, {end, middle});
    const std::vector<P> quarter = unit_quarter(std::max(3, quarter_depth(end)));  // ((sin a, cos a): from straight down the page's right to its foot)
    shape.points.push_back({x - half, left_top});
    shape.points.push_back({x + half, right_top});
    for (const P& p : side) push(shape.points, {x + p.x, root_y + p.y});
    for (std::size_t i = 1; i < quarter.size(); ++i) push(shape.points, {x + end * quarter[i].y, root_y + middle + end * quarter[i].x});
    shape.points.back().x = x;  // (its tip, on its line)
    for (std::size_t i = quarter.size() - 1; i-- > 0;) push(shape.points, {x - end * quarter[i].y, root_y + middle + end * quarter[i].x});
    for (std::size_t i = side.size(); i-- > 0;) push(shape.points, {x - side[i].x, root_y + side[i].y});
    shape.stroke.assign(shape.points.size(), 1);
    shape.stroke[0] = 0;  // (across its top, inside the balloon)
    return shape;
}

Shape neck(P from, P to, double width) {
    const P across = mul(inward(unit(sub(to, from))), width / 2);
    Shape shape;
    shape.points = {add(from, across), add(to, across), sub(to, across), sub(from, across)};
    shape.stroke = {1, 0, 1, 0};
    return shape;
}

// --- the coverage -----------------------------------------------------------------------------------------------------

Plane fill_plane(const std::vector<Shape>& shapes, int width, int height) {
    path_raster::Rasterizer raster;
    for (const bool cut : {false, true}) {
        for (const Shape& shape : shapes) {
            if (shape.cut != cut || shape.points.size() < 3) continue;
            raster.add_component({as_xy(shape.points)}, cut ? core::vector::PathOp::Subtract : core::vector::PathOp::Combine,
                                 core::vector::FillRule::NonZero);
        }
    }
    return rendered(raster, width, height);
}

std::vector<Run> edge_runs(const std::vector<Shape>& shapes) {
    std::vector<Bounds> bounds;
    bounds.reserve(shapes.size());
    for (const Shape& shape : shapes) bounds.push_back(shape.points.empty() ? Bounds{} : bounds_of(shape.points));
    std::vector<Run> runs;
    std::vector<double> cuts;
    for (std::size_t si = 0; si < shapes.size(); ++si) {
        const Shape& shape = shapes[si];
        const std::size_t n = shape.points.size();
        if (n < 2) continue;
        // each segment's pieces on the group's edge, as ranges of it (0..1)
        struct Piece {
            std::size_t segment;
            double from, to;
        };
        std::vector<Piece> pieces;
        for (std::size_t i = 0; i < n; ++i) {
            if (!shape.stroke[i]) continue;
            const P a = shape.points[i];
            const P b = shape.points[(i + 1) % n];
            if (distance(a, b) < 1e-9) continue;
            // (where the other shapes' outlines cross it)
            cuts.clear();
            cuts.push_back(0.0);
            cuts.push_back(1.0);
            for (std::size_t sj = 0; sj < shapes.size(); ++sj) {
                if (sj == si || shapes[sj].points.size() < 3) continue;
                const Bounds& bj = bounds[sj];
                if (std::max(a.x, b.x) < bj.x0 || std::min(a.x, b.x) > bj.x1 || std::max(a.y, b.y) < bj.y0 || std::min(a.y, b.y) > bj.y1) continue;
                const Contour& other = shapes[sj].points;
                for (std::size_t j = 0; j < other.size(); ++j) {
                    double t = 0.0, u = 0.0;
                    if (crossing(a, b, other[j], other[(j + 1) % other.size()], t, u) && t > 1e-7 && t < 1 - 1e-7) cuts.push_back(t);
                }
            }
            std::sort(cuts.begin(), cuts.end());
            for (std::size_t k = 0; k + 1 < cuts.size(); ++k) {
                if (cuts[k + 1] - cuts[k] < 1e-7) continue;
                const P middle = lerp(a, b, (cuts[k] + cuts[k + 1]) / 2);
                bool shown = true;
                bool in_owner = !shape.cut;
                for (std::size_t sj = 0; sj < shapes.size() && shown; ++sj) {
                    if (sj == si || shapes[sj].points.size() < 3) continue;
                    const Bounds& bj = bounds[sj];
                    const bool reach = middle.x >= bj.x0 && middle.x <= bj.x1 && middle.y >= bj.y0 && middle.y <= bj.y1;
                    const bool in = reach && inside(shapes[sj].points, middle);
                    if (shape.cut) {  // (a notch's side shows inside what it is cut from, outside the other notches)
                        if (shapes[sj].cut) {
                            if (in) shown = false;
                        } else if (in && (shape.owner < 0 || static_cast<std::size_t>(shape.owner) == sj)) {
                            in_owner = true;
                        }
                    } else if (in) {
                        shown = false;
                    }
                }
                if (!shown || !in_owner) continue;
                if (!pieces.empty() && pieces.back().segment == i && std::fabs(pieces.back().to - cuts[k]) < 1e-12) {
                    pieces.back().to = cuts[k + 1];
                } else {
                    pieces.push_back({i, cuts[k], cuts[k + 1]});
                }
            }
        }
        if (pieces.empty()) continue;
        // pieces that follow one another without a break are one run
        const auto follows = [&](const Piece& before, const Piece& after) {
            return before.to >= 1.0 && after.from <= 0.0 && (before.segment + 1) % n == after.segment;
        };
        std::vector<Run> made;
        for (std::size_t k = 0; k < pieces.size(); ++k) {
            const Piece& piece = pieces[k];
            const P a = shape.points[piece.segment];
            const P b = shape.points[(piece.segment + 1) % n];
            if (k > 0 && follows(pieces[k - 1], piece)) {
                made.back().points.push_back(lerp(a, b, piece.to));
                made.back().cut_last = piece.to < 1.0;
                continue;
            }
            Run run;
            run.points = {lerp(a, b, piece.from), lerp(a, b, piece.to)};
            run.cut_first = piece.from > 0.0;
            run.cut_last = piece.to < 1.0;
            run.shape = static_cast<int>(si);
            made.push_back(std::move(run));
        }
        if (made.size() == 1 && pieces.size() == n && follows(pieces.back(), pieces.front())) {
            made.front().closed = true;  // (the whole outline: no ends)
            made.front().points.pop_back();
        } else if (made.size() > 1 && follows(pieces.back(), pieces.front())) {
            // (the run that ends where the outline began goes on into the first)
            Run& last = made.back();
            Run& first = made.front();
            last.points.insert(last.points.end(), first.points.begin() + 1, first.points.end());
            last.cut_last = first.cut_last;
            made.erase(made.begin());
        }
        // (a stretch that is not part of the outline — a tail's base — ends a run without a cut: no round end)
        for (Run& run : made) runs.push_back(std::move(run));
    }
    return runs;
}

std::vector<Shape> dashed(const std::vector<Shape>& shapes, double on, double off) {
    std::vector<Shape> out;
    out.reserve(shapes.size());
    for (const Shape& shape : shapes) {
        const std::size_t n = shape.points.size();
        const std::vector<double> lengths = lengths_of(shape.points);
        const double total = lengths.back();
        if (n < 2 || !(total > 0) || !(on > 0) || !(off > 0)) {
            out.push_back(shape);
            continue;
        }
        const double count = std::max(2.0, std::floor(total / (on + off) + 0.5));
        const double period = total / count;
        const double dash = period * on / (on + off);
        Shape cut = shape;
        cut.points.clear();
        cut.stroke.clear();
        for (std::size_t i = 0; i < n; ++i) {
            const P a = shape.points[i];
            const P b = shape.points[(i + 1) % n];
            const double from = lengths[i];
            const double to = lengths[i + 1];
            if (!shape.stroke[i] || !(to > from)) {
                cut.points.push_back(a);
                cut.stroke.push_back(0);
                continue;
            }
            // (dashes centred on 0, period, 2 × period, …: each edge at k × period ± dash / 2)
            std::vector<double> marks{from};
            const auto first = static_cast<std::int64_t>(std::floor(from / period)) - 1;
            const auto last = static_cast<std::int64_t>(std::ceil(to / period)) + 1;
            for (std::int64_t k = first; k <= last; ++k) {
                for (const double edge : {static_cast<double>(k) * period - dash / 2, static_cast<double>(k) * period + dash / 2}) {
                    if (edge > from + 1e-9 && edge < to - 1e-9) marks.push_back(edge);
                }
            }
            std::sort(marks.begin(), marks.end());
            marks.push_back(to);
            for (std::size_t m = 0; m + 1 < marks.size(); ++m) {
                const double middle = (marks[m] + marks[m + 1]) / 2;
                const double phase = std::fmod(middle + dash / 2, period);
                cut.points.push_back(lerp(a, b, (marks[m] - from) / (to - from)));
                cut.stroke.push_back(phase < dash ? 1 : 0);
            }
        }
        out.push_back(std::move(cut));
    }
    return out;
}

namespace {

// Whether a strip `reach` either side of a run would fold over itself: somewhere the run turns so tightly that the
// strip's inner edge would run backwards (what its two bends take of a segment, reach × tan of half each turn, is more
// than the segment). A bend too sharp for a mitre is not counted: it has its own round join.
bool folds(const Contour& points, bool closed, double reach) {
    const std::size_t n = points.size();
    if (n < 3) return false;
    std::vector<double> taken(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        if (!closed && (i == 0 || i + 1 == n)) continue;
        const P n0 = inward(unit(sub(points[i], points[(i + n - 1) % n])));
        const P n1 = inward(unit(sub(points[(i + 1) % n], points[i])));
        const P sum = add(n0, n1);
        const double length = hyp(sum.x, sum.y);
        const double cosine = length > 1e-9 ? dot(mul(sum, 1 / length), n0) : 0.0;
        if (cosine * kMitre < 1.0) continue;
        taken[i] = reach * std::sqrt(std::max(0.0, 1 - cosine * cosine)) / cosine;
    }
    const std::size_t segments = closed ? n : n - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        if (taken[i] + taken[(i + 1) % n] > distance(points[i], points[(i + 1) % n]) * 0.9) return true;
    }
    return false;
}

}  // namespace

Plane ribbon_plane(const std::vector<Run>& runs, double reach, int width, int height, bool round_ends) {
    std::vector<std::vector<path_raster::XY>> pieces;
    if (!(reach > 0)) return rendered(path_raster::Rasterizer(), width, height);
    std::vector<P> quarter;
    for (const P& u : unit_quarter(std::max(3, quarter_depth(reach)))) quarter.push_back({u.x, -u.y});
    const Contour ring = four_fold(quarter, 0.0, 0.0);
    const auto disc = [&](P at) {
        // (the same way round as the strips: see below)
        Contour points;
        for (const P& u : ring) points.push_back({at.x + u.x * reach, at.y + u.y * reach});
        if (shoelace(points) > 0) std::reverse(points.begin(), points.end());
        pieces.push_back(as_xy(points));
    };
    for (const Run& run : runs) {
        Contour points;
        for (const P& p : run.points) push(points, p);
        if (run.closed && points.size() > 2 && distance(points.front(), points.back()) < 1e-9) points.pop_back();
        const std::size_t n = points.size();
        if (n < 2) continue;
        if (folds(points, run.closed, reach)) {
            // (a run that turns tighter than the ribbon is wide — a bubble smaller than its line, a corner rounder than
            // the line is thick: a strip would fold over itself there and leave a hole. Each of its segments is its
            // own box and each bend's outer side its own wedge, all the same way round: their union has no fold.)
            const auto piece = [&](Contour made) {
                if (std::fabs(shoelace(made)) < 1e-12) return;
                if (shoelace(made) > 0) std::reverse(made.begin(), made.end());
                pieces.push_back(as_xy(made));
            };
            const std::size_t segments = run.closed ? n : n - 1;
            for (std::size_t i = 0; i < segments; ++i) {
                const P a = points[i];
                const P b = points[(i + 1) % n];
                const P across = mul(inward(unit(sub(b, a))), reach);
                piece({add(a, across), add(b, across), sub(b, across), sub(a, across)});
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (!run.closed && (i == 0 || i + 1 == n)) continue;
                const P here = points[i];
                const P to = unit(sub(points[(i + 1) % n], here));
                const P n0 = inward(unit(sub(here, points[(i + n - 1) % n])));
                const P n1 = inward(to);
                const P sum = add(n0, n1);
                const double length = hyp(sum.x, sum.y);
                const double cosine = length > 1e-9 ? dot(mul(sum, 1 / length), n0) : 0.0;
                if (cosine * kMitre < 1.0) {
                    disc(here);
                    continue;
                }
                const double side = dot(n0, to) > 0 ? -1.0 : 1.0;  // (the bend's outer side)
                piece({here, add(here, mul(n0, side * reach)), add(here, mul(sum, side * reach / (length * cosine))), add(here, mul(n1, side * reach))});
            }
            if (round_ends && !run.closed) {
                if (run.cut_first) disc(points.front());
                if (run.cut_last) disc(points.back());
            }
            continue;
        }
        // a strip along the run: its left edge forward, its right edge back — the same way round whatever way the
        // run turns, so the strips of a group are one union under the non-zero rule
        Contour left, right;
        for (std::size_t i = 0; i < n; ++i) {
            const bool first = i == 0 && !run.closed;
            const bool last = i + 1 == n && !run.closed;
            const P here = points[i];
            const P before = points[(i + n - 1) % n];
            const P after = points[(i + 1) % n];
            const P n0 = first ? inward(unit(sub(after, here))) : inward(unit(sub(here, before)));
            const P n1 = last ? n0 : inward(unit(sub(after, here)));
            const P sum = add(n0, n1);
            const double length = hyp(sum.x, sum.y);
            const double cosine = length > 1e-9 ? dot(mul(sum, 1 / length), n0) : 0.0;
            if (cosine * kMitre >= 1.0) {
                const P mitre = mul(sum, reach / (length * cosine));
                left.push_back(add(here, mitre));
                right.push_back(sub(here, mitre));
            } else {  // (too sharp for a mitre: both sides' ends, and a round join over the gap)
                left.push_back(add(here, mul(n0, reach)));
                left.push_back(add(here, mul(n1, reach)));
                right.push_back(sub(here, mul(n0, reach)));
                right.push_back(sub(here, mul(n1, reach)));
                disc(here);
            }
        }
        std::vector<path_raster::XY> strip;
        strip.reserve(left.size() + right.size() + 2);
        for (const P& p : left) strip.emplace_back(p.x, p.y);
        if (run.closed) {
            strip.emplace_back(left.front().x, left.front().y);
            strip.emplace_back(right.front().x, right.front().y);
        }
        for (std::size_t i = right.size(); i-- > 0;) strip.emplace_back(right[i].x, right[i].y);
        pieces.push_back(std::move(strip));
        if (round_ends && !run.closed) {
            if (run.cut_first) disc(points.front());
            if (run.cut_last) disc(points.back());
        }
    }
    path_raster::Rasterizer raster;
    raster.add_component(pieces, core::vector::PathOp::Combine, core::vector::FillRule::NonZero);
    return rendered(raster, width, height);
}

// --- fukidashi@2: the pen's line -----------------------------------------------------------------------------------------

namespace {

constexpr double kPenJoin = 0.01;       // (two runs whose ends lie this close are one line, px)
constexpr double kPenCorner = 0.8660254037844387;  // (the cosine of 30°: a sharper turn is a corner, where a stroke ends)
constexpr double kPenThin = 0.15;       // (a stroke's ends: this share of its width)

double smoothstep(double x) {
    if (x <= 0) return 0.0;
    if (x >= 1) return 1.0;
    return x * x * (3 - 2 * x);
}

// A chain of runs as one stretch of line: its points in order (the union's inside on its right), closed or not.
struct Chain {
    Contour points;
    bool closed = false;
    bool cut_first = false;
    bool cut_last = false;
};

// The runs joined end to start into the outlines they are pieces of. Each run is first turned so that the group's
// inside lies on its right (a shape's outline runs clockwise on the page with its inside on the right; a shape that is
// cut out, or runs the other way, is followed backwards).
std::vector<Chain> chained(const std::vector<Run>& runs, const std::vector<Shape>& shapes, bool join) {
    std::vector<Chain> pieces;
    for (const Run& run : runs) {
        Chain piece;
        for (const P& p : run.points) push(piece.points, p);
        piece.closed = run.closed;
        piece.cut_first = run.cut_first;
        piece.cut_last = run.cut_last;
        if (piece.closed && piece.points.size() > 2 && distance(piece.points.front(), piece.points.back()) < 1e-9) piece.points.pop_back();
        if (piece.points.size() < 2) continue;
        bool backwards = false;
        if (run.shape >= 0 && static_cast<std::size_t>(run.shape) < shapes.size()) {
            const Shape& shape = shapes[static_cast<std::size_t>(run.shape)];
            backwards = (shoelace(shape.points) < 0) != shape.cut;
        }
        if (backwards) {
            std::reverse(piece.points.begin(), piece.points.end());
            std::swap(piece.cut_first, piece.cut_last);
        }
        pieces.push_back(std::move(piece));
    }
    if (!join) return pieces;
    // each open piece's end goes on into the piece that begins nearest to it, within kPenJoin
    const std::size_t n = pieces.size();
    const std::size_t none = n;
    std::vector<std::size_t> next(n, none), before(n, none);
    for (std::size_t i = 0; i < n; ++i) {
        if (pieces[i].closed) continue;
        std::size_t best = none;
        double least = kPenJoin;
        for (std::size_t j = 0; j < n; ++j) {
            if (pieces[j].closed || before[j] != none) continue;
            const double d = distance(pieces[i].points.back(), pieces[j].points.front());
            if (d <= least) {
                least = d;
                best = j;
            }
        }
        if (best != none) {
            next[i] = best;
            before[best] = i;
        }
    }
    std::vector<Chain> out;
    std::vector<char> used(n, 0);
    const auto follow = [&](std::size_t first) {
        Chain chain;
        chain.cut_first = pieces[first].cut_first;
        std::size_t at = first;
        while (true) {
            used[at] = 1;
            for (const P& p : pieces[at].points) push(chain.points, p);
            chain.cut_last = pieces[at].cut_last;
            const std::size_t to = next[at];
            if (to == none) break;
            if (to == first) {
                chain.closed = true;
                break;
            }
            if (used[to]) break;
            at = to;
        }
        if (chain.closed) {
            if (chain.points.size() > 2 && distance(chain.points.front(), chain.points.back()) < kPenJoin) chain.points.pop_back();
            chain.cut_first = chain.cut_last = false;
        }
        if (chain.points.size() >= 2) out.push_back(std::move(chain));
    };
    for (std::size_t i = 0; i < n; ++i) {
        if (pieces[i].closed) {
            used[i] = 1;
            out.push_back(std::move(pieces[i]));
        }
    }
    for (std::size_t i = 0; i < n; ++i) {  // (the lines that begin somewhere, then the ones that close on themselves)
        if (!used[i] && before[i] == none) follow(i);
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!used[i]) follow(i);
    }
    return out;
}

}  // namespace

std::vector<PenRun> pen_runs(const std::vector<Run>& runs, const std::vector<Shape>& shapes, const Pen& pen) {
    std::vector<PenRun> out;
    const double least = std::min(1.0, pen.width);  // (the line never opens: a pixel at the least — its width, when it is slimmer)
    const double step = std::max(2.0, pen.step);
    for (Chain& chain : chained(runs, shapes, pen.chain)) {
        Contour& points = chain.points;
        if (chain.closed) {  // (from its topmost point — the leftmost of them)
            std::size_t top = 0;
            for (std::size_t i = 1; i < points.size(); ++i) {
                if (points[i].y < points[top].y || (points[i].y == points[top].y && points[i].x < points[top].x)) top = i;
            }
            std::rotate(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(top), points.end());
        }
        // no stretch longer than the step: each cut into equal parts
        Contour fine;
        const std::size_t count = points.size();
        const std::size_t spans = chain.closed ? count : count - 1;
        for (std::size_t i = 0; i < spans; ++i) {
            const P a = points[i];
            const P b = points[(i + 1) % count];
            const int parts = std::max(1, static_cast<int>(std::ceil(distance(a, b) / step)));
            for (int k = 0; k < parts; ++k) push(fine, lerp(a, b, static_cast<double>(k) / parts));
        }
        if (!chain.closed) push(fine, points.back());
        if (chain.closed && fine.size() > 2 && distance(fine.front(), fine.back()) < 1e-9) fine.pop_back();
        const std::size_t n = fine.size();
        if (n < 2 || (chain.closed && n < 3)) continue;
        // how far along the line each point lies, the way the line runs at it, and whether it turns a corner there
        std::vector<double> along(n + 1, 0.0);
        const std::size_t segments = chain.closed ? n : n - 1;
        for (std::size_t i = 0; i < segments; ++i) along[i + 1] = along[i] + distance(fine[i], fine[(i + 1) % n]);
        const double total = along[segments];
        std::vector<P> outward(n);
        std::vector<char> corner(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            const bool first = !chain.closed && i == 0;
            const bool last = !chain.closed && i + 1 == n;
            const P in = first ? unit(sub(fine[1], fine[0])) : unit(sub(fine[i], fine[(i + n - 1) % n]));
            const P on = last ? in : unit(sub(fine[(i + 1) % n], fine[i]));
            const P way = unit(add(in, on));
            outward[i] = {way.y, -way.x};  // (the inside is on the line's right)
            corner[i] = (first || last || dot(in, on) < kPenCorner) ? 1 : 0;
        }
        // the strokes: where each begins (a place along the line), in order; the last ends where the first begins
        // (closed) or at the line's end
        std::vector<double> starts;
        double turned = 0.0;  // (a closed line is counted from here)
        if (pen.chain) {
            for (std::size_t i = 0; i < n; ++i) {
                if (corner[i]) starts.push_back(along[i]);
            }
            if (chain.closed && starts.empty()) {
                // (no corner: from where the line is lightest — its outward normal most against the heavy way)
                std::size_t light = 0;
                double least_dot = 2.0;
                for (std::size_t i = 0; i < n; ++i) {
                    const double d = dot(outward[i], pen.heavy);
                    if (d < least_dot - 1e-12) {
                        least_dot = d;
                        light = i;
                    }
                }
                const int strokes = std::clamp(pen.strokes, 1, 8);
                for (int k = 0; k < strokes; ++k) starts.push_back(std::fmod(along[light] + total * k / strokes, total));
                std::sort(starts.begin(), starts.end());
            }
            if (chain.closed && !starts.empty()) turned = starts.front();
        }
        PenRun run;
        run.closed = chain.closed;
        run.round_first = chain.cut_first;
        run.round_last = chain.cut_last;
        run.points = fine;
        run.reach.assign(n, pen.width);
        for (std::size_t i = 0; i < n; ++i) {
            double thin = 1.0;
            if (pen.chain && pen.taper > 0 && !starts.empty() && total > 0) {
                // (the stroke the point lies in: from the last start at or before it to the next)
                double s = along[i] - turned;
                if (chain.closed && s < 0) s += total;
                double from = 0.0, to = total;
                for (std::size_t k = starts.size(); k-- > 0;) {
                    const double at = starts[k] - turned;
                    if (at > s + 1e-9) to = at;
                }
                for (const double start : starts) {
                    const double at = start - turned;
                    if (at <= s + 1e-9 && at < to - 1e-9) from = at;  // (the line's last point ends the stroke before it)
                }
                const double length = to - from;
                if (length >= 6 * pen.width) {
                    const double end = std::min(s - from, to - s);
                    thin = kPenThin + (1 - kPenThin) * smoothstep(end / (pen.taper * length / 2));
                }
            }
            const double heavy = 1 + pen.swell * dot(outward[i], pen.heavy);
            run.reach[i] = std::max(least, pen.width * heavy * thin);
        }
        out.push_back(std::move(run));
    }
    return out;
}

Plane pen_plane(const std::vector<PenRun>& runs, int width, int height) {
    // each stretch between two points its own four-cornered piece — as wide at each end as the line is there —, each
    // bend's outer side a wedge (a disc where the bend is too sharp for one), all the same way round: their union has
    // no fold, however tightly the line turns or its width changes
    std::vector<std::vector<path_raster::XY>> pieces;
    const std::vector<P> quarter_unit = unit_quarter(4);
    std::vector<P> quarter;
    for (const P& u : quarter_unit) quarter.push_back({u.x, -u.y});
    const Contour ring = four_fold(quarter, 0.0, 0.0);
    const auto piece = [&](Contour made) {
        if (std::fabs(shoelace(made)) < 1e-12) return;
        if (shoelace(made) > 0) std::reverse(made.begin(), made.end());
        pieces.push_back(as_xy(made));
    };
    const auto disc = [&](P at, double reach) {
        Contour points;
        for (const P& u : ring) points.push_back({at.x + u.x * reach, at.y + u.y * reach});
        piece(std::move(points));
    };
    for (const PenRun& run : runs) {
        const Contour& points = run.points;
        const std::size_t n = points.size();
        if (n < 2 || run.reach.size() != n) continue;
        const std::size_t segments = run.closed ? n : n - 1;
        for (std::size_t i = 0; i < segments; ++i) {
            const std::size_t j = (i + 1) % n;
            const P a = points[i];
            const P b = points[j];
            if (distance(a, b) < 1e-9) continue;
            const P across = inward(unit(sub(b, a)));
            piece({add(a, mul(across, run.reach[i])), add(b, mul(across, run.reach[j])), sub(b, mul(across, run.reach[j])), sub(a, mul(across, run.reach[i]))});
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (!run.closed && (i == 0 || i + 1 == n)) continue;
            const P here = points[i];
            const double reach = run.reach[i];
            const P to = unit(sub(points[(i + 1) % n], here));
            const P n0 = inward(unit(sub(here, points[(i + n - 1) % n])));
            const P n1 = inward(to);
            const P sum = add(n0, n1);
            const double length = hyp(sum.x, sum.y);
            const double cosine = length > 1e-9 ? dot(mul(sum, 1 / length), n0) : 0.0;
            if (cosine > 1 - 1e-9) continue;  // (straight on: nothing to fill)
            if (cosine * kMitre < 1.0) {
                disc(here, reach);
                continue;
            }
            const double side = dot(n0, to) > 0 ? -1.0 : 1.0;  // (the bend's outer side)
            piece({here, add(here, mul(n0, side * reach)), add(here, mul(sum, side * reach / (length * cosine))), add(here, mul(n1, side * reach))});
        }
        if (!run.closed) {
            if (run.round_first) disc(points.front(), run.reach.front());
            if (run.round_last) disc(points.back(), run.reach.back());
        }
    }
    path_raster::Rasterizer raster;
    raster.add_component(pieces, core::vector::PathOp::Combine, core::vector::FillRule::NonZero);
    return rendered(raster, width, height);
}

Plane union_plane(const std::vector<Contour>& outlines, int width, int height) {
    std::vector<std::vector<path_raster::XY>> pieces;
    pieces.reserve(outlines.size());
    for (const Contour& outline : outlines) {
        if (outline.size() < 3) continue;
        Contour points = outline;
        if (shoelace(points) > 0) std::reverse(points.begin(), points.end());
        pieces.push_back(as_xy(points));
    }
    path_raster::Rasterizer raster;
    raster.add_component(pieces, core::vector::PathOp::Combine, core::vector::FillRule::NonZero);
    return rendered(raster, width, height);
}

}  // namespace genko::render::text::clean
