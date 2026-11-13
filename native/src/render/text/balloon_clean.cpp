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

Contour cloud(const Box& box, int bumps) {
    const int n = std::clamp(bumps, 5, 60);
    const double rx = box.rx();
    const double ry = box.ry();
    // (the valleys: at equal distances round an ellipse inside the box, a bump's middle at the top)
    const double keep = 0.86;
    const Contour base = four_fold(quarter_ellipse(rx * keep, ry * keep), 0.0, 0.0);
    const std::vector<double> lengths = lengths_of(base);
    const double total = lengths.back();
    const double bulge = 0.3;  // (a scallop's height against its width)
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
    const double cx = box.cx();
    const double y0 = box.y0;
    const double high = box.y1 - box.y0;
    return two_fold(half, [=](double x, double y, bool mirrored) {
        const double px = x / reach * rx;
        return P{mirrored ? cx - px : cx + px, y0 + (y - top) / (foot - top) * high};
    });
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

Tail tail(const Contour& guide, P centre, const TailSpec& spec) {
    Tail out;
    const std::vector<double> lengths = lengths_of(guide);
    const double total = lengths.back();
    const bool clockwise = shoelace(guide) > 0;
    const bool bent = !spec.bends.empty();
    const bool notch = spec.kind == "inward";
    const P target = bent ? spec.bends.front() : (spec.has_via ? spec.via : spec.tip);
    // (it leaves the outline on the line from the balloon's middle to what it points at: a tail points from the middle
    // of its balloon at the speaker)
    const double at = facing(guide, lengths, centre, target);
    const double half = std::max(1.0, std::min(spec.base, total * 0.3) / 2);
    const P left = point_at(guide, lengths, at - half);
    const P right = point_at(guide, lengths, at + half);
    out.left = left;
    out.right = right;
    const P middle = lerp(left, right, 0.5);
    const double half_chord = std::max(1e-6, distance(left, right) / 2);
    const P chord = unit(sub(right, left));
    const P into = clockwise ? inward(chord) : mul(inward(chord), -1.0);  // (into the balloon, across the base)
    const P side0 = unit(sub(left, middle));                              // (from the base's middle to its left end)

    Contour left_side;   // (from the base's left end to the tip, the tip not included)
    Contour right_side;  // (the same on the right)
    const auto offset_sign = [&](P normal) { return dot(normal, side0) >= 0 ? 1.0 : -1.0; };
    if (bent || spec.bolt) {
        Contour path{middle};
        if (bent) {
            path.insert(path.end(), spec.bends.begin(), spec.bends.end());
        } else {  // (a lightning bolt: the middle line steps aside twice)
            const P along = sub(spec.tip, middle);
            const P across = inward(unit(along));
            path.push_back(add(lerp(middle, spec.tip, 0.38), mul(across, half_chord * 0.9)));
            path.push_back(add(lerp(middle, spec.tip, 0.62), mul(across, -half_chord * 0.6)));
        }
        path.push_back(spec.tip);
        std::vector<double> along{0.0};
        for (std::size_t i = 0; i + 1 < path.size(); ++i) along.push_back(along.back() + distance(path[i], path[i + 1]));
        const double length = std::max(1e-6, along.back());
        const double sign = offset_sign(inward(unit(sub(path[1], path[0]))));
        left_side.push_back(left);
        right_side.push_back(right);
        for (std::size_t i = 1; i + 1 < path.size(); ++i) {
            const P n0 = inward(unit(sub(path[i], path[i - 1])));
            const P n1 = inward(unit(sub(path[i + 1], path[i])));
            const P mitre = unit(add(n0, n1));
            const double scale = std::min(2.5, 1 / std::max(0.2, dot(mitre, n0)));
            const double width = half_chord * (1 - along[i] / length) * scale * sign;
            left_side.push_back(add(path[i], mul(mitre, width)));
            right_side.push_back(sub(path[i], mul(mitre, width)));
        }
    } else {
        P control = spec.has_via ? spec.via : lerp(middle, spec.tip, 0.5);
        const bool bow = spec.kind == "wedge" && !spec.has_via;
        if (bow) {  // (a drawn tail bows a little, away from the balloon's middle: a comma, not a needle)
            const P d = sub(spec.tip, middle);
            const double side = cross(d, sub(middle, centre)) >= 0 ? 1.0 : -1.0;
            control = {control.x - d.y * 0.16 * side, control.y + d.x * 0.16 * side};
        }
        const double length = distance(middle, control) + distance(control, spec.tip);
        const bool zigzag = spec.kind == "zigzag";
        const int steps = zigzag ? 16 : std::clamp(static_cast<int>(std::ceil(length / 5)), 24, 160);
        // (across the tail: the way its width lies where it bends, on the side its base's left end is)
        const P run = sub(spec.tip, middle);
        P across = inward(unit(hyp(run.x, run.y) > 1e-9 ? run : sub(control, middle)));
        if (dot(across, side0) < 0) across = mul(across, -1.0);
        if (zigzag) {  // (its sides step in and out about its middle line, full at the base and to a point)
            const P d0 = sub(control, middle);
            const double sign = offset_sign(inward(unit(hyp(d0.x, d0.y) > 1e-9 ? d0 : run)));
            for (int i = 0; i < steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                const double u = 1 - t;
                const P here{u * u * middle.x + 2 * u * t * control.x + t * t * spec.tip.x, u * u * middle.y + 2 * u * t * control.y + t * t * spec.tip.y};
                const P d{2 * u * (control.x - middle.x) + 2 * t * (spec.tip.x - control.x), 2 * u * (control.y - middle.y) + 2 * t * (spec.tip.y - control.y)};
                const double width = half_chord * u * (i % 2 != 0 ? 1.6 : 0.55);
                const double blend = std::min(1.0, t / 0.4);
                const double eased = blend * blend * (3 - 2 * blend);
                const P over = unit(lerp(side0, mul(inward(unit(d)), sign), eased));
                if (i == 0) {
                    left_side.push_back(left);
                    right_side.push_back(right);
                } else {
                    left_side.push_back(add(here, mul(over, width)));
                    right_side.push_back(sub(here, mul(over, width)));
                }
            }
        } else {
            // each side one curve from its end of the base to the tip, bending about the middle line's bend — no
            // wave in it: full at the base, then (a bowed tail) quickly to a point, or (a straight one) evenly
            const double belly = half_chord * (bow ? 0.31 : 0.5);
            const P bend_left = add(control, mul(across, belly));
            const P bend_right = sub(control, mul(across, belly));
            for (int i = 0; i < steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                const double u = 1 - t;
                left_side.push_back(i == 0 ? left : P{u * u * left.x + 2 * u * t * bend_left.x + t * t * spec.tip.x, u * u * left.y + 2 * u * t * bend_left.y + t * t * spec.tip.y});
                right_side.push_back(i == 0 ? right : P{u * u * right.x + 2 * u * t * bend_right.x + t * t * spec.tip.x, u * u * right.y + 2 * u * t * bend_right.y + t * t * spec.tip.y});
            }
        }
    }
    // the shape: the base's two ends reach into the balloon (out of it, for a notch), not part of the outline
    const double depth = std::max(1.0, spec.depth);
    const P away = mul(into, notch ? -depth : depth);
    Shape& shape = out.shape;
    shape.cut = notch;
    shape.points.push_back(add(left, away));
    for (const P& p : left_side) shape.points.push_back(p);
    shape.points.push_back(spec.tip);
    for (std::size_t i = right_side.size(); i-- > 0;) shape.points.push_back(right_side[i]);
    shape.points.push_back(add(right, away));
    shape.stroke.assign(shape.points.size(), 1);
    shape.stroke.front() = 0;                          // (from inside the balloon to the base's left end)
    shape.stroke[shape.points.size() - 2] = 0;         // (from the base's right end back inside)
    shape.stroke.back() = 0;                           // (across, inside)

    // the rounded joins: a patch in each notch between the balloon's outline and the tail's side
    const bool joined = spec.fillet > 0.5 && !notch && !bent && !spec.bolt && spec.kind != "zigzag";
    if (joined) {
        const double inset = std::max(1.5, spec.fillet * 0.35);
        const auto join = [&](const Contour& side, P corner, double from, double way) {
            double side_length = 0.0;
            for (std::size_t i = 0; i + 1 < side.size(); ++i) side_length += distance(side[i], side[i + 1]);
            // (the point `reach` along the tail's side from the corner, and the side's direction there)
            const auto along_side = [&](double reach, P& direction) {
                double walked = 0.0;
                direction = unit(sub(side.size() > 1 ? side[1] : spec.tip, side[0]));
                for (std::size_t i = 0; i + 1 < side.size(); ++i) {
                    const double span = distance(side[i], side[i + 1]);
                    direction = unit(sub(side[i + 1], side[i]));
                    if (walked + span >= reach) return lerp(side[i], side[i + 1], span > 0 ? (reach - walked) / span : 0.0);
                    walked += span;
                }
                return side.back();
            };
            // (a sharp notch — a tail leaning along the balloon — is rounded further than a blunt one)
            P unused;
            const double sharp = sharpness(unit(sub(point_at(guide, lengths, from + way * spec.fillet), corner)),
                                           unit(sub(along_side(std::min(spec.fillet, side_length * 0.4), unused), corner)));
            const double reach_side = std::min(spec.fillet * sharp, side_length * 0.45);
            const double reach_body = std::min(spec.fillet * sharp, total * 0.12);
            if (reach_side < 0.5 || reach_body < 0.5) return;
            P side_direction;
            const P on_side = along_side(reach_side, side_direction);
            const P on_body = point_at(guide, lengths, from + way * reach_body);
            const P body_direction = direction_at(guide, lengths, from + way * reach_body);
            const P body_in = clockwise ? inward(body_direction) : mul(inward(body_direction), -1.0);
            // (the tail's inside, across its side: toward its other side)
            P tail_in = inward(side_direction);
            if (dot(tail_in, sub(middle, corner)) < 0) tail_in = mul(tail_in, -1.0);
            out.joins.push_back(patch(corner, on_body, body_in, mul(body_direction, -way), on_side, tail_in, mul(side_direction, -1.0), inset));
        };
        join(left_side, left, at - half, -1.0);
        join(right_side, right, at + half, 1.0);
    }
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
