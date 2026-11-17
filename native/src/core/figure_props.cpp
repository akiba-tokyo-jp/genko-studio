// What a figure's hands hold (SPEC FIGURE-02, `hold`; G5): small solids of flat faces set in the hand's own frame, as
// core/figure_shape puts the hand — a sword, a bat or an umbrella through the fist, a cup, a phone, a book, a ball
// against the palm, a pen between the thumb and the fingers, a sheet of paper at the fingertips, a bag hanging from
// the fist. A long prop and a bag may be held another way ("<prop>:<way>": core/pose_library.hpp) — a sword, a bat or
// an umbrella `forward` (tilted in the fist so that it carries on the forearm's line) or `down` (out of the little
// finger's side); a bag by a strap over the `shoulder` (the bag at the hip, the strap through the fist) or hanging from
// the crook of the `elbow`. Only + − × ÷, sqrt and the sines and cosines of fixed angles: the same bytes on every run
// and thread.

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>

#include "core/figure_shape.hpp"
#include "core/pynum.hpp"

namespace genko::core::figure_shape {

namespace {

constexpr double kPiHere = 3.141592653589793;

double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 add(const Vec3& a, const Vec3& b) { return Vec3{a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return Vec3{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 mul(const Vec3& a, double s) { return Vec3{a[0] * s, a[1] * s, a[2] * s}; }
Vec3 cross(const Vec3& a, const Vec3& b) { return Vec3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec3 unit(const Vec3& a) {
    const double n = std::sqrt(dot(a, a));
    return n > 0.0 ? Vec3{a[0] / n, a[1] / n, a[2] / n} : Vec3{0.0, 0.0, 0.0};
}
Vec3 rows_of(const Mat3& m, const Vec3& v) {  // m v
    return Vec3{m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}
Vec3 cols_of(const Mat3& m, const Vec3& v) {  // mᵀ v
    return Vec3{m[0][0] * v[0] + m[1][0] * v[1] + m[2][0] * v[2], m[0][1] * v[0] + m[1][1] * v[1] + m[2][1] * v[2],
                m[0][2] * v[0] + m[1][2] * v[1] + m[2][2] * v[2]};
}

// A solid of flat faces (convex pieces, each face's normal outward from its piece's middle).
struct Solid {
    std::vector<Vec3> v;
    struct Face {
        std::vector<int> at;
        bool round = false;  // (a face of a rounded piece: its edges with another such face draw only on the outline)
        int piece = 0;
    };
    std::vector<Face> faces;
    std::vector<Vec3> middle;  // (each piece's)
    int piece = -1;

    int begin() {
        middle.push_back(Vec3{0.0, 0.0, 0.0});
        return ++piece;
    }
    void end(int first_point) {
        Vec3 sum{0.0, 0.0, 0.0};
        for (std::size_t i = static_cast<std::size_t>(first_point); i < v.size(); ++i) sum = add(sum, v[i]);
        middle.back() = mul(sum, 1.0 / static_cast<double>(v.size() - static_cast<std::size_t>(first_point)));
    }
    void face(std::vector<int> at, bool round) { faces.push_back(Face{std::move(at), round, piece}); }

    // A box: its centre and its three half axes.
    void box(const Vec3& c, const Vec3& ea, const Vec3& eb, const Vec3& ec) {
        begin();
        const int base = static_cast<int>(v.size());
        for (const int sa : {-1, 1}) {
            for (const int sb : {-1, 1}) {
                for (const int sc : {-1, 1}) v.push_back(add(add(add(c, mul(ea, sa)), mul(eb, sb)), mul(ec, sc)));
            }
        }
        end(base);
        const int q[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
        for (const auto& f : q) face({base + f[0], base + f[1], base + f[2], base + f[3]}, false);
    }

    // A solid of turning about the axis from `c` along `d` (e1, e2 across it): rings (distance along d, radius) of n
    // corners, the sides between them; a ring of radius 0 is a point. Ends closed by flat faces.
    void lathe(const Vec3& c, const Vec3& d, const Vec3& e1, const Vec3& e2, const std::vector<std::pair<double, double>>& rings, int n,
               bool round) {
        begin();
        const int base = static_cast<int>(v.size());
        std::vector<int> first;  // (each ring's first point; a point ring has one)
        for (const auto& [along, r] : rings) {
            first.push_back(static_cast<int>(v.size()));
            const Vec3 o = add(c, mul(d, along));
            if (r <= 0.0) {
                v.push_back(o);
                continue;
            }
            for (int k = 0; k < n; ++k) {
                const double t = 2.0 * kPiHere * (static_cast<double>(k) + 0.5) / static_cast<double>(n);
                v.push_back(add(o, add(mul(e1, r * py_cos(t)), mul(e2, r * py_sin(t)))));
            }
        }
        end(base);
        const auto point = [&](std::size_t ring) { return rings[ring].second <= 0.0; };
        for (std::size_t i = 0; i + 1 < rings.size(); ++i) {
            for (int k = 0; k < n; ++k) {
                const int k1 = (k + 1) % n;
                const int a0 = point(i) ? first[i] : first[i] + k;
                const int a1 = point(i) ? first[i] : first[i] + k1;
                const int b0 = point(i + 1) ? first[i + 1] : first[i + 1] + k;
                const int b1 = point(i + 1) ? first[i + 1] : first[i + 1] + k1;
                if (point(i)) {
                    face({a0, b1, b0}, round);
                } else if (point(i + 1)) {
                    face({a0, a1, b0}, round);
                } else {
                    face({a0, a1, b1, b0}, round);
                }
            }
        }
        if (!point(0)) {
            std::vector<int> cap;
            for (int k = n - 1; k >= 0; --k) cap.push_back(first[0] + k);
            face(cap, false);
        }
        if (!point(rings.size() - 1)) {
            std::vector<int> cap;
            for (int k = 0; k < n; ++k) cap.push_back(first.back() + k);
            face(cap, false);
        }
    }

    // A ball: an icosahedron with each face cut in four, pushed onto the sphere (80 faces), rounded.
    void ball(const Vec3& c, double r) {
        begin();
        const int base = static_cast<int>(v.size());
        const double g = (1.0 + std::sqrt(5.0)) / 2.0;
        std::vector<Vec3> p = {{-1, g, 0}, {1, g, 0}, {-1, -g, 0}, {1, -g, 0}, {0, -1, g}, {0, 1, g},
                               {0, -1, -g}, {0, 1, -g}, {g, 0, -1}, {g, 0, 1}, {-g, 0, -1}, {-g, 0, 1}};
        for (Vec3& q : p) q = unit(q);
        const int t[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                              {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
                              {3, 8, 9},  {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
        std::map<std::pair<int, int>, int> halves;
        const auto half = [&](int a, int b) {
            const std::pair<int, int> key{std::min(a, b), std::max(a, b)};
            const auto it = halves.find(key);
            if (it != halves.end()) return it->second;
            p.push_back(unit(add(p[static_cast<std::size_t>(a)], p[static_cast<std::size_t>(b)])));
            const int i = static_cast<int>(p.size()) - 1;
            halves.emplace(key, i);
            return i;
        };
        std::vector<std::array<int, 3>> tris;
        for (const auto& f : t) {
            const int a = f[0], b = f[1], cc = f[2];
            const int ab = half(a, b), bc = half(b, cc), ca = half(cc, a);
            tris.push_back({a, ab, ca});
            tris.push_back({b, bc, ab});
            tris.push_back({cc, ca, bc});
            tris.push_back({ab, bc, ca});
        }
        for (const Vec3& q : p) v.push_back(add(c, mul(q, r)));
        end(base);
        for (const auto& f : tris) face({base + f[0], base + f[1], base + f[2]}, true);
    }

    // A band (a strap) along a line of points: `half_w` wide to each side along `across` (made square to the line at
    // each point), `half_t` thick. One section at each point, shared by the stretches before and after it; each
    // stretch a piece of its own, rounded (the band's lines are its outline alone: it is too thin for its far edges
    // to hide behind it); the two ends closed.
    void band(const std::vector<Vec3>& line, const Vec3& across, double half_w, double half_t) {
        if (line.size() < 2) return;
        const int base = static_cast<int>(v.size());
        for (std::size_t i = 0; i < line.size(); ++i) {
            const Vec3 along = unit(sub(line[std::min(i + 1, line.size() - 1)], line[i > 0 ? i - 1 : 0]));
            Vec3 w = sub(across, mul(along, dot(across, along)));
            if (dot(w, w) < 1e-12) w = cross(along, Vec3{along[1], along[2], along[0]});  // (the band runs along `across`: any side)
            w = unit(w);
            const Vec3 t = cross(along, w);
            for (const int sw : {-1, 1}) {
                for (const int st : {-1, 1}) v.push_back(add(line[i], add(mul(w, half_w * sw), mul(t, half_t * st))));
            }
        }
        const int n = static_cast<int>(line.size());
        for (int i = 0; i + 1 < n; ++i) {
            begin();
            const int a = base + 4 * i;
            const int b = a + 4;
            Vec3 sum{0.0, 0.0, 0.0};
            for (int k = a; k < a + 8; ++k) sum = add(sum, v[static_cast<std::size_t>(k)]);
            middle.back() = mul(sum, 1.0 / 8.0);
            face({a + 0, a + 2, b + 2, b + 0}, true);  // (the two flat sides)
            face({a + 1, b + 1, b + 3, a + 3}, true);
            face({a + 0, b + 0, b + 1, a + 1}, true);  // (the two edges)
            face({a + 2, a + 3, b + 3, b + 2}, true);
            if (i == 0) face({a + 0, a + 1, a + 3, a + 2}, false);
            if (i + 2 == n) face({b + 0, b + 2, b + 3, b + 1}, false);
        }
    }
};

// A sword, a bat or an umbrella's handle along d from c (e1 across its width, e2 across its thickness), its grip's
// middle at `shift` along d from c's.
void long_prop(Solid& s, std::string_view name, const Vec3& c0, const Vec3& d, const Vec3& e1, const Vec3& e2, double shift, bool open_up) {
    const Vec3 c = add(c0, mul(d, shift));
    if (name == "sword") {
        s.lathe(c, d, e1, e2, {{-0.42, 0.11}, {-0.32, 0.11}}, 8, false);  // (the pommel)
        s.lathe(c, d, e1, e2, {{-0.32, 0.075}, {0.42, 0.075}}, 8, true);  // (the hilt)
        s.box(add(c, mul(d, 0.47)), mul(e1, 0.34), mul(d, 0.05), mul(e2, 0.1));  // (the guard)
        s.begin();  // the blade: wide along e1, thin along e2, to a point
        const int base = static_cast<int>(s.v.size());
        const double sec[6][2] = {{0.11, 0.0}, {0.05, 0.03}, {-0.05, 0.03}, {-0.11, 0.0}, {-0.05, -0.03}, {0.05, -0.03}};
        for (const double at : {0.52, 4.3}) {
            const double k = at > 1.0 ? 0.85 : 1.0;
            for (const auto& q : sec) s.v.push_back(add(add(c, mul(d, at)), add(mul(e1, q[0] * k), mul(e2, q[1] * k))));
        }
        s.v.push_back(add(add(c, mul(d, 4.75)), mul(e1, -0.02)));
        s.end(base);
        for (int k = 0; k < 6; ++k) s.face({base + k, base + (k + 1) % 6, base + 6 + (k + 1) % 6, base + 6 + k}, false);
        for (int k = 0; k < 6; ++k) s.face({base + 6 + k, base + 6 + (k + 1) % 6, base + 12}, false);
        s.face({base + 5, base + 4, base + 3, base + 2, base + 1, base + 0}, false);
    } else if (name == "bat") {
        s.lathe(c, d, e1, e2, {{-0.46, 0.0}, {-0.44, 0.11}, {-0.36, 0.11}, {-0.32, 0.07}, {1.4, 0.085}, {3.0, 0.17}, {4.35, 0.2}, {4.5, 0.16}}, 12, true);
    } else if (open_up) {  // (an umbrella held up: open over the head, 8 panels)
        s.lathe(c, d, e1, e2, {{-0.38, 0.07}, {0.38, 0.07}}, 8, true);  // (the handle)
        s.lathe(c, d, e1, e2, {{0.38, 0.03}, {3.5, 0.03}}, 6, true);    // (the shaft)
        s.lathe(c, d, e1, e2, {{3.3, 0.0}, {3.38, 2.9}, {3.44, 2.9}, {4.3, 0.25}, {4.6, 0.0}}, 8, false);
    } else {  // (an umbrella closed)
        s.lathe(c, d, e1, e2, {{-0.38, 0.07}, {0.38, 0.07}, {0.38, 0.03}, {1.0, 0.03}, {1.6, 0.15}, {3.6, 0.06}, {4.1, 0.025}, {4.15, 0.0}}, 8, false);
    }
}

// A bag on the shoulder, in the figure's unit u: how far above the pelvis its top hangs and how far from the hip's
// side (or the thigh's) its near face is; where on the line from the neck's base to the shoulder the strap sits; the
// strap's half width and half thickness. A bag on the elbow: how far along the forearm it hangs and the handle's radius round it.
constexpr double kBagHipUp = 0.3;
constexpr double kBagHipGap = 0.03;
constexpr double kThighRadius = 0.3;  // (the thigh's half thickness at the hip, growing thinner by 0.2 u along the thigh)
constexpr double kStrapSeat = 0.62;
constexpr double kStrapSeatLeast = 0.45;  // (a fist before the chest moves the seat to where it is, between these)
constexpr double kStrapSeatMost = 0.8;
constexpr double kStrapHalfWidth = 0.065;
constexpr double kStrapHalfThick = 0.01;
constexpr double kCrookAlong = 0.3;
constexpr double kCrookRadius = 0.15;

// How far a long prop held `forward` is tilted in the fist, from the thumb's side toward the fingers: its handle lies
// across the palm from the heel of the hand to the first knuckle, and the wrist's own bend to the little finger's side
// (pose_checks' range: 0.6) brings it the rest of the way onto the forearm's line.
constexpr double kForwardTilt = 60.0 * kPiHere / 180.0;

// A long prop held another way: `forward` or `down` (through the fist, out of the little finger's side). Closed, when
// it is an umbrella. False for a way it does not have.
bool long_way(Solid& s, std::string_view name, std::string_view way) {
    const Vec3 A{1.0, 0.0, 0.0};
    const Vec3 W{0.0, 1.0, 0.0};
    const Vec3 N{0.0, 0.0, 1.0};
    const Vec3 grip{0.58, 0.0, 0.16};
    if (way == "forward") {
        const double c = py_cos(kForwardTilt);
        const double sn = py_sin(kForwardTilt);
        long_prop(s, name, grip, add(mul(W, c), mul(A, sn)), sub(mul(A, c), mul(W, sn)), N, 0.0, false);
        return true;
    }
    if (way == "down") {
        long_prop(s, name, grip, mul(W, -1.0), A, N, 0.0, false);
        return true;
    }
    return false;
}

// A hand's frame in the figure: a point of it (in hand lengths) in the figure's space and back, a direction back.
struct HandFrame {
    const Mat3& T;
    Vec3 wrist;
    double sg;
    double L;
    Vec3 out(const Vec3& p) const {
        const Vec3 c = mul(p, L);
        return add(wrist, rows_of(T, Vec3{-sg * c[2], c[0], -c[1]}));
    }
    Vec3 dir_in(const Vec3& d) const {
        const Vec3 x = cols_of(T, d);
        return Vec3{x[1], -x[2], -sg * x[0]};
    }
    Vec3 in(const Vec3& x) const { return mul(dir_in(sub(x, wrist)), 1.0 / L); }
};

// The convex outline of points of a plane (x, y), counter-clockwise from the lowest of the leftmost: which of them.
std::vector<int> outline_of(const std::vector<std::array<double, 2>>& p) {
    std::vector<int> order(p.size());
    for (std::size_t i = 0; i < p.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto& pa = p[static_cast<std::size_t>(a)];
        const auto& pb = p[static_cast<std::size_t>(b)];
        return pa[0] < pb[0] || (pa[0] == pb[0] && pa[1] < pb[1]);
    });
    const auto turn = [&](int o, int a, int b) {
        const auto& po = p[static_cast<std::size_t>(o)];
        const auto& pa = p[static_cast<std::size_t>(a)];
        const auto& pb = p[static_cast<std::size_t>(b)];
        return (pa[0] - po[0]) * (pb[1] - po[1]) - (pa[1] - po[1]) * (pb[0] - po[0]);
    };
    std::vector<int> hull;
    for (int pass = 0; pass < 2; ++pass) {
        const std::size_t start = hull.size();
        for (std::size_t k = 0; k < order.size(); ++k) {
            const int i = pass == 0 ? order[k] : order[order.size() - 1 - k];
            while (hull.size() >= start + 2 && turn(hull[hull.size() - 2], hull.back(), i) <= 0.0) hull.pop_back();
            hull.push_back(i);
        }
        hull.pop_back();
    }
    return hull;
}

// A bag on a strap over the shoulder of the hand's side: the bag at the hip, under the arm; the strap from the bag's
// top up the back, over the shoulder (between the neck and the arm), down the front and back to the bag — taut over the
// shoulder in an upright plane of the chest, round the torso's own surface (figure_shape's torso: whatever the body's
// build), straight from there down to the bag beside the hip (laid round the torso where that would cut through it) —
// and through the fist, which holds it (the plane goes through a fist held before the chest, between the neck and the
// arm; any other fist pulls the strap out of it).
void bag_on_shoulder(Solid& s, const mesh3d::Skeleton& sk, const std::string& side, const HandFrame& h) {
    const double u = sk.unit;
    const double L = h.L;
    const Vec3 dn{0.0, 1.0, 0.0};
    const Mat3& chest = sk.turn("chest");
    const Mat3& hip = sk.turn("hip");
    const Part torso = torso_part(sk);
    // where a ray from inside the torso leaves it by `clear` (steps of 0.05 u, then halved 24 times)
    const auto leaves_at = [&](const Vec3& from, const Vec3& dir, double clear) {
        double lo = 0.0;
        double hi = 4.0 * u;
        for (int k = 1; k <= 80; ++k) {
            const double t = 0.05 * u * k;
            if (torso.at(add(from, mul(dir, t))) > clear) {
                hi = t;
                lo = t - 0.05 * u;
                break;
            }
        }
        for (int k = 0; k < 24; ++k) {
            const double mid = (lo + hi) / 2.0;
            if (torso.at(add(from, mul(dir, mid))) > clear) {
                hi = mid;
            } else {
                lo = mid;
            }
        }
        return hi;
    };
    const auto leaves = [&](const Vec3& from, const Vec3& dir, double clear) { return add(from, mul(dir, leaves_at(from, dir, clear))); };
    // the bag: against the side of the hip, hanging by the figure's down, its wide faces fore and aft — as far out as
    // the torso's side reaches at three heights of it and the thigh's (its bone and the thigh's thickness there) where
    // the thigh is beside it
    const Vec3 side_x = rows_of(hip, Vec3{h.sg, 0.0, 0.0});
    Vec3 outward = sub(side_x, mul(dn, dot(side_x, dn)));
    if (dot(outward, outward) < 1e-6) outward = side_x;
    outward = unit(outward);
    const Vec3 fore = mul(cross(dn, outward), h.sg);
    const Vec3 base = add(sk.point("pelvis"), rows_of(hip, Vec3{0.0, -kBagHipUp * u, 0.0}));
    double reach = 0.0;
    for (const double below : {0.0, 0.25, 0.5}) {
        const Vec3 o = add(sk.point("pelvis"), rows_of(hip, Vec3{0.0, (below - kBagHipUp) * u, 0.0}));
        reach = std::max(reach, dot(sub(leaves(o, outward, 0.0), base), outward));
    }
    {
        const Vec3& joint = sk.point(side + "_hip");
        const Vec3 thigh = sub(sk.point(side + "_knee"), joint);
        for (const double t : {0.0, 0.15, 0.3, 0.45}) {
            const Vec3 p = add(joint, mul(thigh, t));
            const double under = dot(sub(p, base), dn);
            if (under < 0.0 || under > 1.16 * L || std::fabs(dot(sub(p, base), fore)) > 0.78 * L) continue;
            reach = std::max(reach, dot(sub(p, base), outward) + (kThighRadius - 0.2 * t) * u);
        }
    }
    const Vec3 top = add(base, mul(outward, reach + 0.22 * L + kBagHipGap * u));
    s.box(h.in(add(top, mul(dn, 0.58 * L))), mul(h.dir_in(fore), 0.78), mul(h.dir_in(dn), 0.58), mul(h.dir_in(outward), 0.22));
    // the strap's plane: upright and fore and aft in the chest, through the shoulder's seat — between the neck and
    // the arm, where the fist is when it is before the chest (as near it as the seat may be, otherwise)
    const Vec3 grip{0.58, 0.0, 0.16};
    const Vec3 fist_at = h.out(grip);
    const Vec3& neck = sk.point("neck");
    const Vec3 to_shoulder = sub(sk.point(side + "_shoulder"), neck);
    const Vec3 across = rows_of(chest, Vec3{h.sg, 0.0, 0.0});
    const Vec3 up = rows_of(chest, Vec3{0.0, -1.0, 0.0});
    const Vec3 f = rows_of(chest, Vec3{0.0, 0.0, -1.0});
    double seat = kStrapSeat;
    bool through = false;  // (whether the plane goes through the fist: the strap is taut through the hand)
    if (dot(sub(fist_at, neck), f) > 0.15 * u) {
        const double wide = dot(to_shoulder, across);
        const double want = wide > 0.0 ? dot(sub(fist_at, neck), across) / wide : kStrapSeat;
        seat = std::max(kStrapSeatLeast, std::min(kStrapSeatMost, want));
        through = seat == want;
    }
    const Vec3 from = add(add(neck, mul(to_shoulder, seat)), mul(up, -0.5 * u));
    const double half_w = kStrapHalfWidth * u;
    const double half_t = kStrapHalfThick * u;
    const double clear = half_t + 0.012 * u;
    std::vector<Vec3> at;  // the bag's two ends, the torso's surface in the plane, the fist
    at.push_back(add(add(top, mul(fore, -0.47 * L)), mul(dn, 0.03 * L)));
    at.push_back(add(add(top, mul(fore, 0.47 * L)), mul(dn, 0.03 * L)));
    for (int k = -12; k <= 12; ++k) {  // (clear of the torso across the strap's whole width: its middle and its two edges)
        const double t = 10.0 * k * kPiHere / 180.0;
        const Vec3 dir = add(mul(up, py_cos(t)), mul(f, py_sin(t)));
        double far = 0.0;
        for (const double edge : {-1.0, 0.0, 1.0}) far = std::max(far, leaves_at(add(from, mul(across, half_w * edge)), dir, clear));
        at.push_back(add(from, mul(dir, far)));
    }
    const int fist = static_cast<int>(at.size());
    at.push_back(fist_at);
    std::vector<std::array<double, 2>> flat;  // (each seen along the plane's own way: the bag's ends and a fist beside it too)
    for (const Vec3& p : at) flat.push_back({dot(sub(p, from), f), dot(sub(p, from), up)});
    // taut: the convex outline from the bag's back end over the shoulder to its front end
    const std::vector<int> hull = outline_of(flat);
    std::vector<int> path;
    {
        const auto back_at = std::find(hull.begin(), hull.end(), 0);
        const auto front_at = std::find(hull.begin(), hull.end(), 1);
        if (back_at != hull.end() && front_at != hull.end()) {
            // (counter-clockwise the outline runs from the front end over the top to the back end)
            for (auto it = front_at;; it = it + 1 == hull.end() ? hull.begin() : it + 1) {
                path.push_back(*it);
                if (it == back_at) break;
            }
            std::reverse(path.begin(), path.end());
        } else {
            path.push_back(0);
            for (int k = 2; k < fist; ++k) path.push_back(k);
            path.push_back(1);
        }
    }
    if (std::find(path.begin(), path.end(), fist) == path.end()) {  // (a fist inside the strap's line: the strap goes to it, below the last place it lies higher)
        std::size_t top_at = 0;
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (flat[static_cast<std::size_t>(path[i])][1] > flat[static_cast<std::size_t>(path[top_at])][1]) top_at = i;
        }
        std::size_t put = path.size() - 1;
        for (std::size_t i = top_at + 1; i + 1 < path.size(); ++i) {
            if (flat[static_cast<std::size_t>(path[i])][1] < flat[static_cast<std::size_t>(fist)][1]) {
                put = i;
                break;
            }
        }
        path.insert(path.begin() + static_cast<std::ptrdiff_t>(put), fist);
    }
    std::vector<Vec3> run;  // (the strap's line in the figure)
    std::size_t in_fist = 0;
    for (const int i : path) {
        if (i != fist) {
            run.push_back(at[static_cast<std::size_t>(i)]);
            continue;
        }
        // (through the fist as a handle goes: along the thumb's way, the end nearer the strap before it first)
        const Vec3 a = h.out(add(grip, Vec3{0.0, 0.3, 0.0}));
        const Vec3 b = h.out(sub(grip, Vec3{0.0, 0.3, 0.0}));
        const Vec3 before = run.empty() ? a : run.back();
        const bool a_first = dot(sub(a, before), sub(a, before)) <= dot(sub(b, before), sub(b, before));
        in_fist = run.size();
        run.push_back(a_first ? a : b);
        run.push_back(a_first ? b : a);
    }
    std::vector<Vec3> line;
    // (a stretch that leaves the plane — down to the bag beside the hip, or to a fist out of the plane and from it —
    // runs straight: where it would cut through the torso it is laid round it, each of fifteen places along it pushed out
    // from the upright line under the seat until the strap's whole width is clear)
    const auto stretch = [&](const Vec3& a, const Vec3& b) {
        for (int k = 1; k <= 15; ++k) {
            Vec3 p = add(a, mul(sub(b, a), k / 16.0));
            Vec3 out = sub(p, add(from, mul(up, dot(sub(p, from), up))));
            if (dot(out, out) < 1e-9 * u * u) continue;
            out = unit(out);
            const Vec3 side_way = cross(unit(sub(b, a)), out);
            bool moved = false;
            for (int step = 0; step < 40; ++step) {
                double least = 1e300;
                for (const double edge : {-1.0, 0.0, 1.0}) least = std::min(least, torso.at(add(p, mul(side_way, half_w * edge))));
                if (least > clear) break;
                p = add(p, mul(out, 0.02 * u));
                moved = true;
            }
            if (moved) line.push_back(h.in(p));
        }
    };
    for (std::size_t i = 0; i < run.size(); ++i) {
        Vec3 p = run[i];
        if (i == in_fist || i == in_fist + 1) {  // (the fist's two ends, when the hand is held against the body: out of it too)
            Vec3 out = sub(p, add(from, mul(up, dot(sub(p, from), up))));
            if (dot(out, out) > 1e-9 * u * u) {
                out = unit(out);
                for (int step = 0; step < 40; ++step) {
                    double least = 1e300;
                    for (const double edge : {-1.0, 0.0, 1.0}) least = std::min(least, torso.at(add(p, mul(across, half_w * edge))));
                    if (least > clear) break;
                    p = add(p, mul(out, 0.02 * u));
                }
            }
        }
        line.push_back(h.in(p));
        if (i + 1 == run.size() || i == in_fist) continue;  // (the last place; the stretch inside the fist)
        const bool by_bag = i == 0 || i + 2 == run.size();
        const bool by_fist = i + 1 == in_fist || i == in_fist + 1;
        if (by_bag || (by_fist && !through)) stretch(run[i], run[i + 1]);
    }
    s.band(line, h.dir_in(across), half_w / L, half_t / L);
}

// A bag hanging from the crook of the arm: its two handles over the forearm near the elbow, the bag below it by the
// figure's down, its wide faces across the forearm — beside the body where the body is under the forearm.
void bag_on_elbow(Solid& s, const mesh3d::Skeleton& sk, const std::string& side, const HandFrame& h) {
    const double u = sk.unit;
    const double L = h.L;
    const Vec3 dn{0.0, 1.0, 0.0};
    const Vec3& elbow = sk.point(side + "_elbow");
    const Vec3 fore = unit(sub(sk.point(side + "_wrist"), elbow));
    Vec3 deep = sub(fore, mul(dn, dot(fore, dn)));
    const double level = std::sqrt(dot(deep, deep));  // (1: the forearm level; 0: straight up or down)
    if (level < 1e-3) deep = rows_of(sk.turn("hip"), Vec3{0.0, 0.0, -1.0});
    deep = unit(sub(deep, mul(dn, dot(deep, dn))));
    const Vec3 across = cross(dn, deep);
    const double r = kCrookRadius * u;
    const double rise = r / std::max(0.5, level);  // (a sloping forearm is cut on the slant: the handle's arch is as much taller)
    const Vec3 anchor = add(elbow, mul(fore, kCrookAlong * u));
    // the bag hangs under the forearm — and beside the body, not in it: moved along its own width, away from the body's
    // middle, until its two wide faces are clear of the torso and of that side's thigh (0.04 u at a time, 0.64 u at most)
    Vec3 top = add(anchor, mul(dn, rise + 0.45 * L));
    {
        const Part torso = torso_part(sk);
        const Vec3& joint = sk.point(side + "_hip");
        const Vec3 thigh = sub(sk.point(side + "_knee"), joint);
        const Vec3 away = mul(across, dot(across, sub(anchor, sk.point("pelvis"))) >= 0.0 ? 1.0 : -1.0);
        const auto clear = [&](const Vec3& middle) {
            for (const double a : {-1.0, 0.0, 1.0}) {
                for (const double b : {-1.0, 0.0, 1.0}) {
                    for (const double c : {-1.0, 1.0}) {
                        const Vec3 p = add(middle, add(add(mul(across, 0.78 * L * a), mul(dn, 0.58 * L * b)), mul(deep, 0.22 * L * c)));
                        if (torso.at(p) < 0.02 * u) return false;
                        const double t = std::max(0.0, std::min(1.0, dot(sub(p, joint), thigh) / dot(thigh, thigh)));
                        const Vec3 off = sub(p, add(joint, mul(thigh, t)));
                        const double thick = (kThighRadius - 0.2 * t + 0.02) * u;
                        if (dot(off, off) < thick * thick) return false;
                    }
                }
            }
            return true;
        };
        for (int k = 0; k < 16 && !clear(add(top, mul(dn, 0.58 * L))); ++k) top = add(top, mul(away, 0.04 * u));
    }
    for (const double off : {-1.0, 1.0}) {
        const Vec3 o = mul(deep, 0.1 * L * off);
        std::vector<Vec3> line;
        line.push_back(h.in(add(add(top, o), add(mul(across, -0.38 * L), mul(dn, 0.03 * L)))));
        for (int k = 4; k >= 0; --k) {
            const double t = 45.0 * k * kPiHere / 180.0;
            line.push_back(h.in(add(add(anchor, o), add(mul(across, r * py_cos(t)), mul(dn, -rise * py_sin(t))))));
        }
        line.push_back(h.in(add(add(top, o), add(mul(across, 0.38 * L), mul(dn, 0.03 * L)))));
        s.band(line, h.dir_in(deep), 0.03, 0.012);
    }
    s.box(h.in(add(top, mul(dn, 0.58 * L))), mul(h.dir_in(across), 0.78), mul(h.dir_in(dn), 0.58), mul(h.dir_in(deep), 0.22));
}

// The prop in the hand's frame (a along the fingers, w toward the thumb, n out of the palm; in hand lengths), `down`
// the figure's own down in that frame: a bag hangs and a cup stands by it, and a long prop held by a palm laid on it
// (a pommel, a bat's knob, an umbrella's handle: the palm facing down) goes down from under the palm.
void make(Solid& s, std::string_view name, const Vec3& down) {
    const Vec3 A{1.0, 0.0, 0.0};
    const Vec3 W{0.0, 1.0, 0.0};
    const Vec3 N{0.0, 0.0, 1.0};
    const Vec3 grip{0.58, 0.0, 0.16};  // (the middle of a handle through the fist)
    const Vec3 dn = unit(down);
    const Vec3 up = mul(dn, -1.0);
    if (name == "sword" || name == "bat" || name == "umbrella") {
        const bool held_up = name == "umbrella" && dot(W, up) > 0.3;  // (an umbrella with the thumb up is held up, open)
        if (dot(N, dn) > 0.6 && !held_up) {  // (the palm laid on its top: the prop down from under it, closed)
            long_prop(s, name, Vec3{0.42, 0.0, 0.08}, N, A, W, 0.44, false);
            return;
        }
        if (name != "umbrella") {  // (through the fist, toward the thumb)
            long_prop(s, name, grip, W, A, N, 0.0, false);
            return;
        }
        const double up_w = dot(W, up);
        if (std::fabs(up_w) > 0.3) {  // (held up or hanging down through the fist: open when up)
            long_prop(s, name, grip, up_w > 0.0 ? W : mul(W, -1.0), A, N, 0.0, up_w > 0.0);
        } else {
            long_prop(s, name, grip, W, A, N, 0.0, false);
        }
    } else if (name == "bag") {  // (hanging from the fist by its handle)
        Vec3 across = sub(W, mul(dn, dot(W, dn)));
        if (dot(across, across) < 1e-6) across = sub(A, mul(dn, dot(A, dn)));
        across = unit(across);
        const Vec3 deep = cross(dn, across);
        const Vec3 top = add(grip, mul(dn, 0.55));
        for (const double side : {-1.0, 1.0}) {  // (the handle: two straps from the fist to the bag's top)
            const Vec3 foot = add(top, mul(across, 0.38 * side));
            const Vec3 mid = mul(add(grip, foot), 0.5);
            const Vec3 along = mul(sub(foot, grip), 0.5);
            const Vec3 off = unit(cross(along, deep));
            s.box(mid, along, mul(off, 0.025), mul(deep, 0.025));
        }
        s.box(add(top, mul(dn, 0.58)), mul(across, 0.78), mul(dn, 0.58), mul(deep, 0.22));
    } else if (name == "cup") {  // (upright: against the fingers, or on the palm when it faces up)
        const double r = 0.21;
        const double h = 0.56;
        const double on_palm = std::max(0.0, std::min(1.0, (dot(N, up) - 0.4) / 0.4));
        Vec3 side = sub(N, mul(up, dot(N, up)));
        side = dot(side, side) > 1e-6 ? unit(side) : N;
        const Vec3 held{0.48, 0.0, 0.07};
        const Vec3 c = add(held, add(mul(side, (r + 0.08) * (1.0 - on_palm)), mul(up, (h / 2 + 0.02) * on_palm)));
        Vec3 e1 = sub(A, mul(up, dot(A, up)));
        if (dot(e1, e1) < 1e-6) e1 = sub(W, mul(up, dot(W, up)));
        e1 = unit(e1);
        const Vec3 e2 = cross(up, e1);
        s.lathe(c, up, e1, e2, {{-h / 2, r * 0.8}, {-h / 2 + 0.03, r * 0.88}, {h / 2, r * 1.06}}, 12, true);
    } else if (name == "phone") {
        s.box(Vec3{0.42, 0.0, 0.12}, mul(A, 0.42), mul(W, 0.21), mul(N, 0.028));
    } else if (name == "paper") {  // (a sheet held at its corner, between the thumb and the fingers)
        s.box(Vec3{0.75 - 0.8, 0.12 - 0.57, 0.12}, mul(A, 0.8), mul(W, 0.57), mul(N, 0.006));
    } else if (name == "book") {
        s.box(Vec3{0.45, -0.1, 0.17}, mul(A, 0.62), mul(W, 0.45), mul(N, 0.09));
    } else if (name == "pen") {
        const Vec3 q{0.7, 0.16, 0.12};
        const Vec3 d = unit(Vec3{0.7, -0.25, 0.6});
        const Vec3 e1 = unit(cross(d, N));
        const Vec3 e2 = cross(d, e1);
        s.lathe(q, d, e1, e2, {{-0.55, 0.03}, {0.12, 0.03}, {0.24, 0.0}}, 6, false);
    } else if (name == "ball") {
        s.ball(Vec3{0.42, 0.0, 0.7}, 0.62);
    }
}

}  // namespace

double hand_length(const mesh3d::Skeleton& sk) { return 0.8 * sk.unit * hand_scale(sk.body); }

Props props(const mesh3d::Skeleton& sk, std::string_view left, std::string_view right) {
    Props out;
    const double L = hand_length(sk);
    for (int side = 0; side < 2; ++side) {
        const std::string_view held = side == 0 ? left : right;
        if (held.empty()) continue;
        const std::size_t colon = held.find(':');  // ("<prop>:<way>": core/pose_library.hpp)
        const std::string_view name = held.substr(0, colon);
        const std::string_view way = colon == std::string_view::npos ? std::string_view() : held.substr(colon + 1);
        const std::string s = side == 0 ? "l" : "r";
        const double sg = side == 0 ? 1.0 : -1.0;
        const Mat3& T = sk.turn(s + "_wrist");
        const Vec3& wrist = sk.point(s + "_wrist");
        // the hand's frame in the figure (as figure_shape puts the hand): (a, w, n) → (−side n, a, −w), turned by the wrist
        const auto local = [sg](const Vec3& c) { return Vec3{-sg * c[2], c[0], -c[1]}; };
        const auto back = [sg](const Vec3& x) { return Vec3{x[1], -x[2], -sg * x[0]}; };  // (its inverse)
        const Vec3 down = back(cols_of(T, Vec3{0.0, 1.0, 0.0}));
        Solid solid;
        if (colon == std::string_view::npos) {
            make(solid, name, down);
        } else if (name == "sword" || name == "bat" || name == "umbrella") {
            long_way(solid, name, way);
        } else if (name == "bag" && way == "shoulder") {
            bag_on_shoulder(solid, sk, s, HandFrame{T, wrist, sg, L});
        } else if (name == "bag" && way == "elbow") {
            bag_on_elbow(solid, sk, s, HandFrame{T, wrist, sg, L});
        }
        // each face its own points (flat: one normal), triangles fanned from its first point; the edges between faces
        const auto base = static_cast<std::int32_t>(out.mesh.v.size());
        std::map<std::pair<int, int>, std::vector<std::array<std::int32_t, 3>>> edge_faces;  // (point pair → [mesh a, mesh b, normal point])
        std::vector<bool> round_of;
        for (const Solid::Face& f : solid.faces) {
            Vec3 nl{0.0, 0.0, 0.0};  // (Newell's normal)
            Vec3 mid{0.0, 0.0, 0.0};
            for (std::size_t i = 0; i < f.at.size(); ++i) {
                const Vec3& p = solid.v[static_cast<std::size_t>(f.at[i])];
                const Vec3& q = solid.v[static_cast<std::size_t>(f.at[(i + 1) % f.at.size()])];
                nl = add(nl, Vec3{(p[1] - q[1]) * (p[2] + q[2]), (p[2] - q[2]) * (p[0] + q[0]), (p[0] - q[0]) * (p[1] + q[1])});
                mid = add(mid, p);
            }
            mid = mul(mid, 1.0 / static_cast<double>(f.at.size()));
            nl = unit(nl);
            if (dot(nl, sub(mid, solid.middle[static_cast<std::size_t>(f.piece)])) < 0.0) nl = mul(nl, -1.0);
            const Vec3 normal = rows_of(T, local(nl));
            const auto first = static_cast<std::int32_t>(out.mesh.v.size());
            for (const int i : f.at) {
                out.mesh.v.push_back(add(wrist, rows_of(T, local(mul(solid.v[static_cast<std::size_t>(i)], L)))));
                out.mesh.n.push_back(normal);
                out.mesh.no_contour.push_back(1);
                out.mesh.part.push_back(255);
            }
            const auto count = static_cast<std::int32_t>(f.at.size());
            for (std::int32_t k = 1; k + 1 < count; ++k) {
                if (side == 1) {
                    out.mesh.t.push_back({first, first + k + 1, first + k});
                } else {
                    out.mesh.t.push_back({first, first + k, first + k + 1});
                }
            }
            for (std::size_t i = 0; i < f.at.size(); ++i) {
                const int a = f.at[i];
                const int b = f.at[(i + 1) % f.at.size()];
                edge_faces[{std::min(a, b), std::max(a, b)}].push_back(
                    {first + static_cast<std::int32_t>(i), first + static_cast<std::int32_t>((i + 1) % f.at.size()), first});
            }
            round_of.push_back(f.round);
        }
        (void)base;
        // (the face each edge's entries came from: by their normal point's place among the faces' first points)
        std::map<std::int32_t, bool> round_at;
        {
            std::size_t fi = 0;
            std::int32_t at = base;
            for (const Solid::Face& f : solid.faces) {
                round_at[at] = round_of[fi++];
                at += static_cast<std::int32_t>(f.at.size());
            }
        }
        for (const auto& [key, list] : edge_faces) {
            if (list.empty()) continue;
            const auto& one = list[0];
            const std::int32_t other = list.size() > 1 ? list[1][2] : -1;
            const bool round = list.size() > 1 && round_at[one[2]] && round_at[other];
            out.edges.push_back({one[0], one[1], one[2], other, round ? 1 : 0});
        }
    }
    return out;
}

}  // namespace genko::core::figure_shape
