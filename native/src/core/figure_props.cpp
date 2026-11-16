// What a figure's hands hold (SPEC FIGURE-02, `hold`; G5): small solids of flat faces set in the hand's own frame, as
// core/figure_shape puts the hand — a sword, a bat or an umbrella through the fist, a cup, a phone, a book, a ball
// against the palm, a pen between the thumb and the fingers, a sheet of paper at the fingertips, a bag hanging from
// the fist. Only + − × ÷, sqrt and the sines and cosines of fixed angles: the same bytes on every run and thread.

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
        const std::string_view name = side == 0 ? left : right;
        if (name.empty()) continue;
        const std::string s = side == 0 ? "l" : "r";
        const double sg = side == 0 ? 1.0 : -1.0;
        const Mat3& T = sk.turn(s + "_wrist");
        const Vec3& wrist = sk.point(s + "_wrist");
        // the hand's frame in the figure (as figure_shape puts the hand): (a, w, n) → (−side n, a, −w), turned by the wrist
        const auto local = [sg](const Vec3& c) { return Vec3{-sg * c[2], c[0], -c[1]}; };
        const auto back = [sg](const Vec3& x) { return Vec3{x[1], -x[2], -sg * x[0]}; };  // (its inverse)
        const Vec3 down = back(cols_of(T, Vec3{0.0, 1.0, 0.0}));
        Solid solid;
        make(solid, name, down);
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
