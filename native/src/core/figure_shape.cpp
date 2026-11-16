// The smooth figure (SPEC FIGURE-01): the field of its body, head and hands, built from figure_skeleton's joints and
// turns, and meshed by core/surface_nets. Only + − × ÷, sqrt, min and max here (and the turns' sines and cosines).

#include "core/figure_shape.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "core/json.hpp"
#include "core/pynum.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/surface_nets.hpp"

namespace genko::core::figure_shape {

namespace {

using la::matvec;

inline double dot3(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
// (smin, for the folds here)
inline double blend_min(double a, double b, double k) {
    const double m = std::min(a, b);
    if (!(k > 0.0)) return m;
    const double h = std::max(k - std::fabs(a - b), 0.0) / k;
    return m - h * h * h * k * (1.0 / 6.0);
}
Vec3 add(const Vec3& a, const Vec3& b) { return Vec3{a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return Vec3{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 mul(const Vec3& a, double s) { return Vec3{a[0] * s, a[1] * s, a[2] * s}; }
double length(const Vec3& a) { return std::sqrt(dot3(a, a)); }
Vec3 unit(const Vec3& a) {
    const double n = length(a);
    return n > 0.0 ? Vec3{a[0] / n, a[1] / n, a[2] / n} : Vec3{0.0, 0.0, 0.0};
}
double sign(double v) { return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0); }
// m^T v: the point in the frame whose axes are m's columns
inline Vec3 into(const Mat3& m, const Vec3& v) {
    return Vec3{m[0][0] * v[0] + m[1][0] * v[1] + m[2][0] * v[2], m[0][1] * v[0] + m[1][1] * v[1] + m[2][1] * v[2],
                m[0][2] * v[0] + m[1][2] * v[1] + m[2][2] * v[2]};
}
// m v (plain products, for the shapes' own frames)
Vec3 out_of(const Mat3& m, const Vec3& v) {
    return Vec3{m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

const Mat3 kIdentity{Vec3{1.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0}, Vec3{0.0, 0.0, 1.0}};

double clamp(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

// A piece carried into another frame: `to` maps a point of the figure there, `turn` a direction (a turn, or a turn
// and a mirror).
template <typename To, typename Turn>
Piece carried(Piece p, To to, Turn turn) {
    p.a = to(p.a);
    p.b = to(p.b);
    p.centre = to(p.centre);
    if (p.kind != Piece::Kind::Cone) {
        Mat3 m{};
        for (int c = 0; c < 3; ++c) {
            const Vec3 col = turn(Vec3{p.axes[0][static_cast<std::size_t>(c)], p.axes[1][static_cast<std::size_t>(c)], p.axes[2][static_cast<std::size_t>(c)]});
            for (int r = 0; r < 3; ++r) m[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = col[static_cast<std::size_t>(r)];
        }
        p.axes = m;
    }
    return p;
}

Piece with_blend(Piece p, double blend) {
    p.blend = blend;
    return p;
}

Piece carved(Piece p, double blend) {
    p.blend = blend;
    p.carve = true;
    return p;
}

void pair(std::vector<Piece>& list, Piece one, Piece other, double blend = -1.0) {
    one.paired = true;
    one.blend = blend;
    other.blend = blend;
    list.push_back(std::move(one));
    list.push_back(std::move(other));
}

// The stations `from`..`to` of a run (or of its slopes), each y moved by `by`: a run of sections that goes through a
// joint is one smooth run cut in two there, each half in its own bone's frame (so nothing shows at the joint held
// straight).
std::vector<Station> stations_of(const std::vector<Station>& all, std::size_t from, std::size_t to, double by) {
    std::vector<Station> some;
    for (std::size_t i = from; i <= to; ++i) {
        some.push_back(all[i]);
        some.back().y = some.back().y + by;
    }
    return some;
}

// How far the turn b is from the turn a: the sine of half the angle between them (0: the same).
double bend_of(const Mat3& a, const Mat3& b) {
    double trace = 0.0;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) trace = trace + a[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] * b[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
    }
    return std::sqrt(std::max(0.0, (3.0 - trace) * 0.25));
}

// --- the hand, in its own frame: a along the fingers, w toward the thumb, n out of the palm (a × w = n: a left hand)

// The end of the forearm a hand is meshed with (the body's own forearm, in the hand's frame): kept within `cut` of
// the wrist and thinned by `shrink` there, so its rim stays inside the body's mesh of the same forearm.
struct Stub {
    bool on = false;
    std::vector<Piece> pieces;  // (the forearm's, in the hand's frame)
    double k = 0;               // (and the width they are joined by)
    Vec3 toward{};              // (the unit vector from the wrist to the elbow, in the hand's frame)
    double cut = 0;
    double shrink = 0;
    Vec3 wrist{};               // (the wrist's radii along a, w and n, in the hand's frame's units)
};

struct Finger {
    Vec3 base;  // the knuckle (MCP), in hand lengths
    double r;   // its radius there
    double len;
    double fan0;
    double fan1;  // the fan angle: fan0 + fan1 × spread
    Vec3 root;    // where its bone of the palm starts, near the wrist
    double r_root;
    double r_head;  // (the bone's radius at the knuckle)
};

const Finger kFingers[4] = {
    {Vec3{0.54, 0.162, -0.005}, 0.047, 0.38, 0.035, 0.16, Vec3{0.11, 0.075, 0.0}, 0.074, 0.06},      // index
    {Vec3{0.555, 0.054, -0.005}, 0.048, 0.43, 0.008, 0.04, Vec3{0.11, 0.027, 0.0}, 0.077, 0.061},    // middle
    {Vec3{0.54, -0.054, -0.005}, 0.045, 0.40, -0.03, -0.10, Vec3{0.11, -0.03, 0.0}, 0.075, 0.057},   // ring
    {Vec3{0.505, -0.158, -0.005}, 0.040, 0.31, -0.075, -0.20, Vec3{0.11, -0.085, 0.0}, 0.068, 0.05},  // little
};

// Field of a hand `length` long. Parts: 0 the palm, 1 the thumb, 2..5 the fingers, 6 the stub of the forearm.
Field hand_field(double length, const HandShape& shape, const Stub& stub) {
    const double L = length;
    const auto at = [L](double a, double w, double n) { return Vec3{a * L, w * L, n * L}; };
    Field f;
    Part palm;
    // (the palm is its four bones fanning from the wrist to the knuckles — wider at the knuckles, a web between them —,
    // the pads at the base of the thumb and of the little finger, and the wrist's end)
    palm.k = 0.05 * L;
    for (const Finger& g : kFingers) {
        palm.pieces.push_back(cone(at(g.root[0], g.root[1], g.root[2]), g.r_root * L, at(g.base[0] - 0.02, g.base[1], 0.0), g.r_head * L));
    }
    palm.pieces.push_back(ellipsoid(at(0.23, 0.125, 0.04), kIdentity, at(0.15, 0.085, 0.07)));   // the ball of the thumb
    palm.pieces.push_back(ellipsoid(at(0.24, -0.135, 0.03), kIdentity, at(0.16, 0.06, 0.06)));   // and of the little finger
    // (the wrist: a little more than the forearm's own end, which the body's mesh carries into the hand)
    const Vec3 wrist = stub.on ? stub.wrist : at(0.21, 0.135, 0.092);
    palm.pieces.push_back(ellipsoid(Vec3{0.0, 0.0, 0.0}, kIdentity, Vec3{wrist[0] + 0.006 * L, wrist[1] + 0.006 * L, wrist[2] + 0.006 * L}));
    f.parts.push_back(std::move(palm));

    const double spread = clamp(shape.spread, 0.0, 1.0);
    const double opp = clamp(shape.thumb, 0.0, 1.0);
    {  // the thumb, from the wrist's side of the palm: three bones
        const double c = clamp(shape.curls[0], 0.0, 1.0);
        // (beside the palm and a little in front of it at 0; across the palm's front at 1; bending toward the palm)
        const Vec3 d = unit(Vec3{0.85 + (0.84 - 0.85) * opp, 0.42 + (-0.24 - 0.42) * opp, 0.25 + (0.49 - 0.25) * opp});
        Vec3 bend{0.0, -0.7 + 0.3 * opp, 0.7 - 1.6 * opp};
        bend = unit(sub(bend, mul(d, dot3(bend, d))));
        const double lengths[3] = {0.24, 0.16, 0.12};
        const double radii[4] = {0.07, 0.056, 0.05, 0.045};
        const double turns[3] = {0.1 * c, 0.35 * c, 0.6 * c};
        Part thumb;
        Vec3 p = at(0.12, 0.14, 0.05);
        double phi = 0.0;
        for (int k = 0; k < 3; ++k) {
            phi = phi + turns[k];
            const Vec3 dir = add(mul(d, py_cos(phi)), mul(bend, py_sin(phi)));
            const Vec3 q = add(p, mul(dir, lengths[k] * L));
            thumb.pieces.push_back(cone(p, radii[k] * L, q, radii[k + 1] * L));
            p = q;
        }
        f.parts.push_back(std::move(thumb));
        f.joints.push_back(Joint{0, 1, at(0.22, 0.17, 0.03), 0.12 * L, 0.32 * L, 0.0});  // (the web between thumb and index)
    }
    for (int i = 0; i < 4; ++i) {  // the fingers: three bones each, curled toward the palm (+n)
        const Finger& g = kFingers[i];
        const double c = clamp(shape.curls[static_cast<std::size_t>(i + 1)], 0.0, 1.0);
        const double fan = g.fan0 + g.fan1 * spread;
        const Vec3 d{py_cos(fan), py_sin(fan), 0.0};
        const Vec3 n{0.0, 0.0, 1.0};
        const double parts[3] = {0.47, 0.30, 0.23};
        const double radii[4] = {g.r, 0.9 * g.r, 0.8 * g.r, 0.72 * g.r};
        const double turns[3] = {1.6 * c, 1.85 * c, 1.2 * c};
        Part finger;
        Vec3 p = at(g.base[0], g.base[1], g.base[2]);
        double phi = 0.0;
        for (int k = 0; k < 3; ++k) {
            phi = phi + turns[k];
            const Vec3 dir = add(mul(d, py_cos(phi)), mul(n, py_sin(phi)));
            const double seg = g.len * parts[k] - (k == 2 ? radii[3] : 0.0);
            const Vec3 q = add(p, mul(dir, seg * L));
            finger.pieces.push_back(cone(p, radii[k] * L, q, radii[k + 1] * L));
            p = q;
        }
        f.parts.push_back(std::move(finger));
        f.joints.push_back(Joint{0, i + 2, at(g.base[0], g.base[1], g.base[2]), 0.035 * L, 0.09 * L, 0.0});
    }
    Part wrist_part;
    if (stub.on) {
        wrist_part.pieces = stub.pieces;
        wrist_part.k = stub.k;
        wrist_part.cut = true;
        wrist_part.cut_n = stub.toward;
        wrist_part.cut_d = stub.cut;
        wrist_part.fade = stub.shrink / stub.cut;
        wrist_part.fade_n = stub.toward;
        wrist_part.fade_d = -0.5 * stub.cut;  // (half the shrink already at the wrist: under the body's coarser mesh of the same forearm)
    } else {  // (a hand on its own: a short rounded wrist)
        wrist_part.k = 0.06 * L;
        wrist_part.pieces.push_back(ellipsoid(Vec3{0.0, 0.0, 0.0}, kIdentity, wrist));
        wrist_part.pieces.push_back(ellipsoid(at(-0.1, 0.0, 0.0), kIdentity, at(0.2, 0.125, 0.105)));
    }
    f.parts.push_back(std::move(wrist_part));
    f.joints.push_back(Joint{0, 6, at(0.04, 0.0, 0.0), 0.08 * L, 0.3 * L, 0.0});
    return f;
}

// --- the head: an egg of a skull, the face in front of and below it, the jaw and the chin; a calm face in shallow
// relief (the brow, the eyes' shallow sockets and their lids, the nose and its bridge, closed lips), small ears flat
// against the head between the eyes' and the nose's height ------------------------------------------------------------

constexpr std::size_t kPlainHead = 5;  // (the head's first pieces: the skull, the face, the chin and the jaw's corners, without the relief)

Part head_part(const Vec3& centre, const Mat3& turn, double u) {
    const auto at = [&](double x, double y, double z) { return add(centre, out_of(turn, Vec3{x * u, y * u, z * u})); };
    const auto r = [u](double x, double y, double z) { return Vec3{x * u, y * u, z * u}; };
    Part head;
    head.k = 0.16 * u;
    std::vector<Piece>& p = head.pieces;
    p.push_back(ellipsoid(at(0.0, -0.11, 0.055), turn, r(0.375, 0.43, 0.445)));  // the skull
    p.push_back(ellipsoid(at(0.0, 0.14, -0.1), turn, r(0.29, 0.39, 0.335)));     // the face
    p.push_back(with_blend(ellipsoid(at(0.0, 0.4, -0.235), turn, r(0.16, 0.125, 0.135)), 0.14 * u));  // the chin
    pair(p, ellipsoid(at(0.185, 0.27, -0.03), turn, r(0.095, 0.14, 0.16)), ellipsoid(at(-0.185, 0.27, -0.03), turn, r(0.095, 0.14, 0.16)), 0.14 * u);  // the jaw's corners
    p.push_back(with_blend(ellipsoid(at(0.0, -0.03, -0.24), turn, r(0.3, 0.12, 0.2)), 0.1 * u));      // the brow
    // the eyes: shallow sockets under the brow, beside the nose's bridge
    pair(p, carved(ellipsoid(at(0.15, 0.06, -0.5), turn, r(0.1, 0.07, 0.085)), 0.1 * u), carved(ellipsoid(at(-0.15, 0.06, -0.5), turn, r(0.1, 0.07, 0.085)), 0.1 * u),
         0.1 * u);
    pair(p, ellipsoid(at(0.15, 0.062, -0.34), turn, r(0.075, 0.05, 0.075)), ellipsoid(at(-0.15, 0.062, -0.34), turn, r(0.075, 0.05, 0.075)), 0.05 * u);  // their lids
    // the nose: its bridge down to the tip, the wings
    p.push_back(with_blend(cone(at(0.0, 0.01, -0.405), 0.036 * u, at(0.0, 0.165, -0.475), 0.042 * u), 0.07 * u));
    p.push_back(with_blend(ellipsoid(at(0.0, 0.185, -0.44), turn, r(0.062, 0.04, 0.055)), 0.05 * u));
    // the mouth: the round of the teeth under it, the two lips closed on each other
    p.push_back(with_blend(ellipsoid(at(0.0, 0.3, -0.285), turn, r(0.15, 0.1, 0.13)), 0.08 * u));
    p.push_back(with_blend(ellipsoid(at(0.0, 0.286, -0.36), turn, r(0.075, 0.03, 0.05)), 0.03 * u));
    p.push_back(with_blend(ellipsoid(at(0.0, 0.32, -0.355), turn, r(0.062, 0.03, 0.05)), 0.03 * u));
    pair(p, ellipsoid(at(0.318, 0.1, 0.07), turn, r(0.035, 0.095, 0.06)), ellipsoid(at(-0.318, 0.1, 0.07), turn, r(0.035, 0.095, 0.06)), 0.05 * u);  // the ears
    return head;
}

// The directions a head's outline is found by: at each of its points (part 0 of the mesh) the plain head's own
// direction of growth — the skull, the face and the chin without the relief.
void plain_outline(Smooth& mesh, const Part& head, double u) {
    Part plain;
    plain.k = head.k;
    plain.pieces.assign(head.pieces.begin(), head.pieces.begin() + static_cast<std::ptrdiff_t>(kPlainHead));
    const double e = 0.01 * u;
    mesh.outline = mesh.n;
    for (std::size_t i = 0; i < mesh.v.size(); ++i) {
        if (mesh.part[i] != 0) continue;
        const Vec3& x = mesh.v[i];
        const Vec3 g{plain.at(Vec3{x[0] + e, x[1], x[2]}) - plain.at(Vec3{x[0] - e, x[1], x[2]}), plain.at(Vec3{x[0], x[1] + e, x[2]}) - plain.at(Vec3{x[0], x[1] - e, x[2]}),
                     plain.at(Vec3{x[0], x[1], x[2] + e}) - plain.at(Vec3{x[0], x[1], x[2] - e})};
        const double len = length(g);
        if (len > 0.0) mesh.outline[i] = Vec3{g[0] / len, g[1] / len, g[2] / len};
    }
}

// The face's guide lines (mesh3d's head_marks: the centre line, the eye line, the nose), each point carried along its
// ray from the head's centre to the surface (bisection), then lifted as before.
std::vector<std::vector<Vec3>> marks_on(const Part& head, const Vec3& centre, const Mat3& turn, double u) {
    const double rx_ = 0.42 * u;
    const double ry_ = 0.55 * u;
    const double rz_ = 0.5 * u;
    const auto on = [&](double theta, double phi, double lift) {
        const Vec3 dir = out_of(turn, Vec3{rx_ * py_sin(phi) * py_sin(theta), -ry_ * py_cos(phi), -rz_ * py_sin(phi) * py_cos(theta)});
        double lo = 0.2;  // (inside)
        double hi = 2.0;  // (outside)
        for (int i = 0; i < 48; ++i) {
            const double mid = (lo + hi) * 0.5;
            if (head.at(add(centre, mul(dir, mid))) < 0.0) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return add(centre, mul(dir, (lo + hi) * 0.5 * lift));
    };
    std::vector<Vec3> centre_line;
    for (int k = 3; k < 21; ++k) centre_line.push_back(on(0.0, kPi * k / 20, 1.01));
    std::vector<Vec3> eye_line;
    for (int k = 0; k < 21; ++k) eye_line.push_back(on(kPi * (k / 20.0 - 0.5) * 0.9, kPi * 0.55, 1.01));
    std::vector<Vec3> nose{on(0, kPi * 0.62, 1.01), on(0, kPi * 0.66, 1.03), on(0, kPi * 0.7, 1.01)};  // (over the nose's relief now)
    return {centre_line, eye_line, nose};
}

// --- the body --------------------------------------------------------------------------------------------------------

struct Sex {
    double rib_w = 1.0;
    double rib_d = 1.0;
    double waist = 1.0;
    double pelvis = 1.0;
    double glute = 1.0;
    double limb = 1.0;
    double thigh = 1.0;
    double hand = 1.0;
    double neck = 1.0;
    int chest = 0;  // 1 pectorals, 2 breasts
};

Sex sex_of(const Json& body) {
    const Json* sex = get(body, "sex");
    const std::string name = sex != nullptr && sex->is_string() ? sex->get<std::string>() : std::string();
    if (name == "male") return Sex{1.02, 1.04, 1.04, 0.97, 0.95, 1.08, 1.0, 1.0, 1.06, 1};
    if (name == "female") return Sex{1.05, 0.97, 1.02, 1.0, 1.08, 1.0, 1.06, 0.92, 0.92, 2};
    return Sex{};
}

// (figure_skeleton has put every number in: the op's own, the sex's, or the usual)
double body_number(const Json& body, const char* key, double lo, double hi) { return py_max(lo, py_min(hi, to_float(body.at(key)))); }

// The body's numbers, as the pieces use them (lengths in u).
struct Dims {
    double u = 1;
    double shoulders = 1;
    double hips = 1;
    double build = 1;
    double legs = 1;
    Sex sx;
    double g = 1;     // girth: build ^ 0.75
    double gs = 1;    // (its root: the feet and the neck follow the build a little)
    double dd = 1;    // depth of the torso
    double hw = 0.5;  // the hip joints' half spacing
    double arm = 1;   // the arms' girth
    double leg = 1;   // the thighs'
};

Dims dims_of(const mesh3d::Skeleton& sk) {
    const Json& body = sk.body;
    Dims d;
    d.u = sk.unit;
    d.shoulders = body_number(body, "shoulders", 0.6, 1.5);
    d.hips = body_number(body, "hips", 0.6, 1.6);
    d.build = body_number(body, "build", 0.5, 1.8);
    d.legs = body_number(body, "legs", 0.6, 1.5);
    d.sx = sex_of(body);
    const double root = std::sqrt(d.build);
    d.g = root * std::sqrt(root);
    d.gs = std::sqrt(d.g);
    d.dd = d.build;
    d.hw = 0.5 * d.hips;
    d.arm = d.g * d.sx.limb;
    d.leg = d.g * d.sx.thigh;
    return d;
}

// Append b to a (b's points moved into the figure by `place`, its normals by `turn`; triangles flipped when `flip`).
template <typename Place, typename Turn>
void append(Smooth& a, const Smooth& b, Place place, Turn turn, bool flip) {
    const auto base = static_cast<std::int32_t>(a.v.size());
    if (!b.outline.empty() && a.outline.empty()) a.outline = a.n;
    for (std::size_t i = 0; i < b.v.size(); ++i) {
        a.v.push_back(place(b.v[i]));
        a.n.push_back(turn(b.n[i]));
        a.no_contour.push_back(b.no_contour[i]);
        a.part.push_back(b.part[i]);
        if (!a.outline.empty() || !b.outline.empty()) a.outline.push_back(turn(b.outline.empty() ? b.n[i] : b.outline[i]));
    }
    for (const auto& t : b.t) {
        if (flip) {
            a.t.push_back({base + t[0], base + t[2], base + t[1]});
        } else {
            a.t.push_back({base + t[0], base + t[1], base + t[2]});
        }
    }
}

}  // namespace

double hand_scale(const Json& body) { return sex_of(body).hand; }

// --- pieces -----------------------------------------------------------------------------------------------------------

Piece cone(const Vec3& a, double ra, const Vec3& b, double rb) {
    Piece p;
    p.kind = Piece::Kind::Cone;
    p.a = a;
    p.b = b;
    p.ra = ra;
    p.rb = rb;
    p.centre = mul(add(a, b), 0.5);
    p.reach = length(sub(b, a)) * 0.5 + std::max(ra, rb);
    return p;
}

Piece ellipsoid(const Vec3& centre, const Mat3& axes, const Vec3& radii) {
    Piece p;
    p.kind = Piece::Kind::Ellipsoid;
    p.a = centre;
    p.axes = axes;
    p.size = radii;
    p.centre = centre;
    p.reach = std::max(radii[0], std::max(radii[1], radii[2]));
    p.steep = p.reach / std::min(radii[0], std::min(radii[1], radii[2]));
    return p;
}

Piece box(const Vec3& centre, const Mat3& axes, const Vec3& half, double round) {
    Piece p;
    p.kind = Piece::Kind::Box;
    p.a = centre;
    p.axes = axes;
    p.size = half;
    p.round = std::min(round, std::min(half[0], std::min(half[1], half[2])));
    p.centre = centre;
    p.reach = length(half);
    return p;
}

std::vector<Station> run_slopes(const std::vector<Station>& stations) {
    const std::size_t n = stations.size();
    std::vector<Station> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i].y = stations[i].y;
    if (n < 3) return out;
    // (the clamped spline's own equations, h[i] m[i−1] + 2 (h[i−1] + h[i]) m[i] + h[i−1] m[i+1] = 3 (h[i] s[i−1] + h[i−1] s[i]),
    // with m 0 at both ends: one sweep down, one back)
    for (int which = 0; which < 3; ++which) {
        const auto v = [&](std::size_t i) { return which == 0 ? stations[i].rx : (which == 1 ? stations[i].rz : stations[i].zc); };
        std::vector<double> upper(n, 0.0);
        std::vector<double> right(n, 0.0);
        for (std::size_t i = 1; i + 1 < n; ++i) {
            const double h0 = stations[i].y - stations[i - 1].y;
            const double h1 = stations[i + 1].y - stations[i].y;
            const double rhs = 3.0 * (h1 * (v(i) - v(i - 1)) / h0 + h0 * (v(i + 1) - v(i)) / h1);
            const double pivot = 2.0 * (h0 + h1) - h1 * upper[i - 1];
            upper[i] = h0 / pivot;
            right[i] = (rhs - h1 * right[i - 1]) / pivot;
        }
        std::vector<double> m(n, 0.0);
        for (std::size_t i = n - 2; i >= 1; --i) m[i] = right[i] - upper[i] * m[i + 1];
        for (std::size_t i = 0; i < n; ++i) (which == 0 ? out[i].rx : (which == 1 ? out[i].rz : out[i].zc)) = m[i];
    }
    return out;
}

// (a loft's numbers: the count n; the n stations' y; then for each of the n − 1 spans 1 / its length and the cubics'
// four coefficients of rx, rz and zc, in the span's own 0..1)
Piece loft(const Vec3& origin, const Mat3& axes, const std::vector<Station>& stations, double cap0, double cap1, const std::vector<Station>* slopes) {
    Piece p;
    p.kind = Piece::Kind::Loft;
    p.a = origin;
    p.axes = axes;
    p.ra = cap0;
    p.rb = cap1;
    const std::size_t n = stations.size();
    p.loft.push_back(static_cast<double>(n));
    for (const Station& st : stations) p.loft.push_back(st.y);
    const std::vector<Station> own = slopes == nullptr ? run_slopes(stations) : *slopes;
    const auto of = [](const Station& st, int which) { return which == 0 ? st.rx : (which == 1 ? st.rz : st.zc); };
    const auto value = [&](std::size_t i, int which) { return of(stations[i], which); };
    const auto slope = [&](std::size_t i, int which) { return of(own[i], which); };
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const double span = stations[i + 1].y - stations[i].y;
        p.loft.push_back(1.0 / span);
        for (int which = 0; which < 3; ++which) {
            const double v0 = value(i, which);
            const double v1 = value(i + 1, which);
            const double m0 = slope(i, which) * span;
            const double m1 = slope(i + 1, which) * span;
            p.loft.push_back(v0);
            p.loft.push_back(m0);
            p.loft.push_back(3.0 * (v1 - v0) - 2.0 * m0 - m1);
            p.loft.push_back(2.0 * (v0 - v1) + m0 + m1);
        }
    }
    double widest = 0.0;
    double ratio = 1.0;
    for (const Station& st : stations) {
        widest = std::max(widest, std::max(st.rx, st.rz) + std::fabs(st.zc));
        ratio = std::min(ratio, std::min(st.rx, st.rz) / std::max(st.rx, st.rz));
        ratio = std::min(ratio, std::min(cap0, cap1) / std::max(st.rx, st.rz));
    }
    const double y0 = stations.front().y;
    const double y1 = stations.back().y;
    p.centre = add(origin, out_of(axes, Vec3{0.0, (y0 + y1) * 0.5, 0.0}));
    p.reach = (y1 - y0) * 0.5 + std::max(cap0, cap1) + 1.1 * widest;
    p.steep = 1.0 / (0.6 * std::min(1.0, ratio));
    return p;
}

namespace {

// The first-order distance to the ellipsoid of radii r about 0 (the true distance at its surface, never more than it).
inline double ellipsoid_at(double x, double y, double z, double rx, double ry, double rz) {
    const double qx = x / rx;
    const double qy = y / ry;
    const double qz = z / rz;
    const double k0 = std::sqrt(qx * qx + qy * qy + qz * qz);
    if (!(k0 > 0.0)) return -std::min(rx, std::min(ry, rz));
    const double gx = qx / rx;
    const double gy = qy / ry;
    const double gz = qz / rz;
    return (k0 - 1.0) * k0 / std::sqrt(gx * gx + gy * gy + gz * gz);
}

}  // namespace

double Piece::at(const Vec3& x) const {
    switch (kind) {
    case Kind::Cone: {
        // (the exact distance to a round cone: two balls and the cone that touches both)
        const Vec3 ba = sub(b, a);
        const double l2 = dot3(ba, ba);
        const double rr = ra - rb;
        const double a2 = l2 - rr * rr;
        if (!(l2 > 0.0) || !(a2 > 0.0)) {  // (one ball holds the other)
            return std::min(length(sub(x, a)) - ra, length(sub(x, b)) - rb);
        }
        const double il2 = 1.0 / l2;
        const Vec3 pa = sub(x, a);
        const double y = dot3(pa, ba);
        const double z = y - l2;
        const Vec3 q = sub(mul(pa, l2), mul(ba, y));
        const double x2 = dot3(q, q);
        const double y2 = y * y * l2;
        const double z2 = z * z * l2;
        const double k = sign(rr) * rr * rr * x2;
        if (sign(z) * a2 * z2 > k) return std::sqrt(x2 + z2) * il2 - rb;
        if (sign(y) * a2 * y2 < k) return std::sqrt(x2 + y2) * il2 - ra;
        return (std::sqrt(x2 * a2 * il2) + y * rr) * il2 - ra;
    }
    case Kind::Ellipsoid: {
        const Vec3 q = into(axes, sub(x, a));
        return ellipsoid_at(q[0], q[1], q[2], size[0], size[1], size[2]);
    }
    case Kind::Box: {
        const Vec3 q = into(axes, sub(x, a));
        const double qx = std::fabs(q[0]) - (size[0] - round);
        const double qy = std::fabs(q[1]) - (size[1] - round);
        const double qz = std::fabs(q[2]) - (size[2] - round);
        const double ox = std::max(qx, 0.0);
        const double oy = std::max(qy, 0.0);
        const double oz = std::max(qz, 0.0);
        return std::sqrt(ox * ox + oy * oy + oz * oz) + std::min(std::max(qx, std::max(qy, qz)), 0.0) - round;
    }
    case Kind::Loft: {
        const Vec3 q = into(axes, sub(x, a));
        const double* data = loft.data();
        const int n = static_cast<int>(data[0]);
        const double* ys = data + 1;
        const double* spans = ys + n;
        // (past an end: the dome that closes it, its section going on as it slopes at the end — so the form it
        // continues into, sloping the same way there, holds it)
        if (!(q[1] > ys[0])) {
            const double* c = spans;
            const double t = (q[1] - ys[0]) * c[0];
            const double rx = std::max(c[1] + c[2] * t, 0.3 * c[1]);
            const double rz = std::max(c[5] + c[6] * t, 0.3 * c[5]);
            return ellipsoid_at(q[0], q[1] - ys[0], q[2] - (c[9] + c[10] * t), rx, ra, rz);
        }
        if (!(q[1] < ys[n - 1])) {
            const double* c = spans + 13 * (n - 2);
            const double t = (q[1] - ys[n - 1]) * c[0];
            const double rx1 = c[1] + c[2] + c[3] + c[4];
            const double rz1 = c[5] + c[6] + c[7] + c[8];
            const double rx = std::max(rx1 + (c[2] + 2.0 * c[3] + 3.0 * c[4]) * t, 0.3 * rx1);
            const double rz = std::max(rz1 + (c[6] + 2.0 * c[7] + 3.0 * c[8]) * t, 0.3 * rz1);
            const double zc = c[9] + c[10] + c[11] + c[12] + (c[10] + 2.0 * c[11] + 3.0 * c[12]) * t;
            return ellipsoid_at(q[0], q[1] - ys[n - 1], q[2] - zc, rx, rb, rz);
        }
        int i = 0;
        while (i + 2 < n && !(q[1] < ys[i + 1])) ++i;
        const double* c = spans + 13 * i;
        const double inv = c[0];
        const double s = (q[1] - ys[i]) * inv;
        const double rx = c[1] + s * (c[2] + s * (c[3] + s * c[4]));
        const double rz = c[5] + s * (c[6] + s * (c[7] + s * c[8]));
        const double zc = c[9] + s * (c[10] + s * (c[11] + s * c[12]));
        const double drx = (c[2] + s * (2.0 * c[3] + 3.0 * s * c[4])) * inv;
        const double drz = (c[6] + s * (2.0 * c[7] + 3.0 * s * c[8])) * inv;
        const double dzc = (c[10] + s * (2.0 * c[11] + 3.0 * s * c[12])) * inv;
        const double X = q[0] / rx;
        const double Z = (q[2] - zc) / rz;
        const double e = std::sqrt(X * X + Z * Z);
        if (!(e > 0.0)) return -std::min(rx, rz);
        // (the slope of the section's size where the ray from its middle through x leaves it: first order at the surface)
        const double ux = X / e;
        const double uz = Z / e;
        const double along = ux * ux * drx / rx + uz * uz * drz / rz + uz * dzc / rz;
        const double gx = ux / rx;
        const double gz = uz / rz;
        return (e - 1.0) / std::sqrt(gx * gx + gz * gz + along * along);
    }
    }
    return std::numeric_limits<double>::infinity();
}

double smin(double a, double b, double k) { return blend_min(a, b, k); }

namespace {

// Whether the piece's value at x is `least` or more for certain (x that far past the ball that holds it).
inline bool beyond(const Piece& p, const Vec3& x, double least) {
    if (!(least >= 0.0)) return false;
    const double r = p.reach + least * p.steep;
    const double dx = x[0] - p.centre[0];
    const double dy = x[1] - p.centre[1];
    const double dz = x[2] - p.centre[2];
    return dx * dx + dy * dy + dz * dz >= r * r;
}

}  // namespace

double Part::at(const Vec3& x) const {
    // (a pair is joined first: smin is the same either way round, so a mirrored body is the mirror's to the bit. A
    // piece too far away to change the value — the smooth minimum of v and d is v where d ≥ v + k — is not worked out:
    // the same number, sooner)
    const Piece* const all = pieces.data();
    const std::size_t count = pieces.size();
    if (count == 0) return std::numeric_limits<double>::infinity();
    // the point in the rest space of a trunk that bends, and how much less the field counts there
    Vec3 q = x;
    double less = 1.0;
    if (bend.on) {
        const Vec3 qh = into(bend.hip, sub(x, bend.pelvis));
        const Vec3 qs = into(bend.spine, sub(x, bend.pelvis));
        Vec3 qc = into(bend.chest, sub(x, bend.waist_at));
        qc[1] = qc[1] - bend.waist;
        const double t1 = clamp((-0.5 * (qh[1] + qs[1]) - bend.from[0]) / bend.over[0], 0.0, 1.0);
        const double t2 = clamp((-0.5 * (qs[1] + qc[1]) - bend.from[1]) / bend.over[1], 0.0, 1.0);
        const double s1 = t1 * t1 * (3.0 - 2.0 * t1);
        const double s2 = t2 * t2 * (3.0 - 2.0 * t2);
        const Vec3 hs = sub(qs, qh);
        const Vec3 sc = sub(qc, qs);
        q = add(add(qh, mul(hs, s1)), mul(sc, s2));
        const double steeper = 6.0 * t1 * (1.0 - t1) / bend.over[0] * length(hs) + 6.0 * t2 * (1.0 - t2) / bend.over[1] * length(sc);
        less = 1.0 / (1.0 + steeper);
    }
    const auto value = [&](const Piece& p) { return p.rest ? less * p.at(q) : p.at(x); };
    const auto far = [&](const Piece& p, double least) { return p.rest ? beyond(p, q, least / less) : beyond(p, x, least); };
    double v;
    std::size_t i = 0;
    {
        const Piece& p = all[0];
        v = value(p);
        if (p.paired && count > 1) {
            v = blend_min(v, value(all[1]), p.blend >= 0.0 ? p.blend : k);
            i = 2;
        } else {
            i = 1;
        }
    }
    for (; i < count; ++i) {
        const Piece& p = all[i];
        const double kk = p.blend >= 0.0 ? p.blend : k;
        const bool two = p.paired && i + 1 < count;
        const double least = p.carve ? kk - v : v + (two ? 1.25 * kk : kk);
        if (far(p, least) && (!two || far(all[i + 1], least))) {
            if (two) ++i;
            continue;
        }
        double d = value(p);
        if (two) {
            d = blend_min(d, value(all[i + 1]), kk);
            ++i;
        }
        v = p.carve ? -blend_min(-v, d, kk) : blend_min(v, d, kk);
    }
    if (cut) {
        const double g = dot3(cut_n, x) - cut_d;
        v = cut_k > 0.0 ? -blend_min(-v, -g, cut_k) : std::max(v, g);
    }
    if (fade > 0.0) v = v + fade * std::max(0.0, dot3(fade_n, x) - fade_d);
    v = v + inset;
    return v;
}

double Field::at(const Vec3& x, const std::uint8_t* use, double* d, int* nearest) const {
    double f = std::numeric_limits<double>::infinity();
    int best = -1;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (use != nullptr && use[i] == 0) {
            d[i] = std::numeric_limits<double>::infinity();
            continue;
        }
        d[i] = parts[i].at(x);
        if (d[i] < f || best < 0) {
            f = d[i];
            best = static_cast<int>(i);
        }
    }
    for (const Joint& j : joints) {
        if (use != nullptr && (use[j.p] == 0 || use[j.c] == 0)) continue;
        const Vec3 r = sub(x, j.at);
        const double t2 = dot3(r, r) / (j.radius * j.radius);
        if (!(t2 < 1.0)) continue;
        double w = 1.0;
        if (t2 > j.flat * j.flat) {  // (past the flat middle: (1 − s²)², s from 0 at its rim to 1 at the radius)
            const double s = (std::sqrt(t2) - j.flat) / (1.0 - j.flat);
            w = (1.0 - s * s) * (1.0 - s * s);
        }
        f = std::min(f, blend_min(d[j.p], d[j.c], j.k * w));
    }
    if (nearest != nullptr) *nearest = best;
    return f;
}

double Field::blend_of(int part) const {
    double k = 0.0;
    for (const Joint& j : joints) {
        if (j.p == part || j.c == part) k = std::max(k, j.k);
    }
    return k;
}

void Field::bounds(Vec3& lo, Vec3& hi) const {
    const double inf = std::numeric_limits<double>::infinity();
    lo = Vec3{inf, inf, inf};
    hi = Vec3{-inf, -inf, -inf};
    double k = 0.0;
    for (const Joint& j : joints) k = std::max(k, j.k);
    for (const Part& part : parts) {
        for (const Piece& p : part.pieces) {
            if (p.carve) continue;
            const double kk = std::max(k, std::max(part.k, p.blend));
            // (a piece of a bending trunk: where each of its three turns would put it)
            std::vector<Vec3> centres{p.centre};
            if (p.rest && part.bend.on) {
                Vec3 above = p.centre;
                above[1] = above[1] + part.bend.waist;
                centres = {add(part.bend.pelvis, out_of(part.bend.hip, p.centre)), add(part.bend.pelvis, out_of(part.bend.spine, p.centre)),
                           add(part.bend.waist_at, out_of(part.bend.chest, above))};
            }
            for (const Vec3& c : centres) {
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], c[a] - p.reach - kk);
                    hi[a] = std::max(hi[a], c[a] + p.reach + kk);
                }
            }
        }
    }
}

namespace {

// The torso's pieces and what the limbs share with it (the balls at the shoulders).
struct Torso {
    Part part;
    double r_shoulder = 0;  // (the ball at each shoulder, which the upper arm starts as)
};

Torso torso_of(const mesh3d::Skeleton& sk, const Dims& d) {
    const double u = d.u;
    const double g = d.g;
    const double dd = d.dd;
    const Sex& sx = d.sx;
    const Vec3& P0 = sk.point("pelvis");
    const Vec3& W = sk.point("waist");
    const Vec3& N = sk.point("neck");
    const Mat3& H = sk.turn("hip");
    const Mat3& S = sk.turn("spine");
    const Mat3& C = sk.turn("chest");
    const auto off = [u](const Vec3& o, const Mat3& m, double x, double y, double z) { return add(o, out_of(m, Vec3{x * u, y * u, z * u})); };
    const auto radii = [u](double x, double y, double z) { return Vec3{x * u, y * u, z * u}; };
    const auto st = [u](double y, double rx, double rz, double zc) { return Station{y * u, rx * u, rz * u, zc * u}; };
    // (half widths: the pelvis spans the hip joints and what the build puts around them; the ribcage follows the
    // shoulders; the waist both. Depths follow the build)
    const double r_pelvis = d.hw + 0.35 * d.leg * sx.pelvis;
    const double r_chest = (0.4 + 0.4 * d.shoulders) * g * sx.rib_w;
    const double r_waist = 0.63 * g * sx.waist * (0.55 + 0.225 * (d.hips + d.shoulders));
    const double r_ribs = 0.3 * r_waist + 0.64 * r_chest;  // (the lowest ribs, where the chest turns on the waist)
    const double d_chest = dd * sx.rib_d;
    const double sh = d.shoulders;
    Torso out;
    Part& p = out.part;
    p.k = 0.12 * u;
    // The trunk's sections from the neck's base down to the hips, as one run: the top of the ribcage, under the
    // armpits, the fullest ribs, the lowest ribs (where the chest turns on the waist), the waist, above the hips, the
    // hips (where the spine turns on the pelvis).
    const Station top = st(-2.74, 0.58 * r_chest, 0.33 * d_chest, 0.1 * dd);
    const Station armpit = st(-2.38, 0.9 * r_chest, 0.5 * d_chest, 0.03 * dd);
    const Station ribs = st(-1.92, r_chest, 0.55 * d_chest, -0.015 * dd);
    const Station low = st(-1.3, r_ribs, 0.47 * d_chest, -0.035 * dd);
    const Station waist = st(-0.92, r_waist, 0.4 * dd, -0.035 * dd);
    const Station flank = st(-0.4, 0.42 * r_waist + 0.58 * r_pelvis, 0.42 * dd, -0.025 * dd);
    const Station hips = st(0.0, r_pelvis, 0.43 * dd, -0.02 * dd);
    // (and the pelvis: from the hips' section it narrows between the thighs to the crotch — the hips' sides are the
    // thighs' own tops)
    const Station groin = st(0.2, 0.62 * d.hw + 0.14 * g, 0.385 * dd, 0.025 * dd);
    const Station crotch = st(0.35, 0.12 * g + 0.2 * (d.hw - 0.5), 0.2 * dd, 0.09 * dd);
    // The trunk is this one run, set in the body standing straight; it bends with the spine (Bend): rigid with the
    // pelvis below the pelvis joint, with the chest above the waist joint and what is 0.35 u over it, and the two
    // turns mixed over the 0.7 u above each joint. No form meets another at a joint, so nothing shows there, straight
    // or bent.
    p.bend.on = true;
    p.bend.pelvis = P0;
    p.bend.waist_at = W;
    p.bend.hip = H;
    p.bend.spine = S;
    p.bend.chest = C;
    p.bend.waist = 1.3 * u;
    p.bend.from[0] = 0.0;
    p.bend.over[0] = 0.7 * u;
    p.bend.from[1] = 0.95 * u;
    p.bend.over[1] = 0.7 * u;
    Piece trunk = loft(Vec3{0.0, 0.0, 0.0}, kIdentity, {top, armpit, ribs, low, waist, flank, hips, groin, crotch}, 0.22 * u, 0.07 * u);
    trunk.rest = true;
    p.pieces.push_back(std::move(trunk));
    // the buttocks
    const double gx = 0.52 * d.hw + 0.02 * g;
    pair(p.pieces, ellipsoid(off(P0, H, gx, 0.1, 0.2 * dd), H, radii(0.34 * g * sx.glute, 0.42, 0.37 * dd * sx.glute)),
         ellipsoid(off(P0, H, -gx, 0.1, 0.2 * dd), H, radii(0.34 * g * sx.glute, 0.42, 0.37 * dd * sx.glute)), 0.22 * u);
    // the shoulders: the trapezius sloping from the neck to each, the ball of each (the upper arm starts as the same
    // ball, so the deltoid runs into the chest and the back however the arm is raised)
    out.r_shoulder = 0.245 * u * d.arm;
    const Vec3& ls = sk.point("l_shoulder");
    const Vec3& rs = sk.point("r_shoulder");
    pair(p.pieces, cone(off(N, C, 0.14, -0.16, 0.1), 0.15 * u * d.gs, off(ls, C, -0.12, -0.03, 0.03), 0.16 * u * g),
         cone(off(N, C, -0.14, -0.16, 0.1), 0.15 * u * d.gs, off(rs, C, 0.12, -0.03, 0.03), 0.16 * u * g), 0.3 * u);
    pair(p.pieces, ellipsoid(ls, C, Vec3{out.r_shoulder, out.r_shoulder, out.r_shoulder}), ellipsoid(rs, C, Vec3{out.r_shoulder, out.r_shoulder, out.r_shoulder}),
         0.25 * u);
    // the collarbones: a low ridge from the pit of the neck out to each shoulder
    {
        // (set under the chest's front between its top section and the one under the armpits: 0.012 u above it)
        const double front_top = 0.1 * dd - 0.33 * d_chest;
        const double front_armpit = 0.03 * dd - 0.5 * d_chest;
        const double z = front_top + 0.39 * (front_armpit - front_top) + 0.068 + (sx.chest == 2 ? 0.024 : 0.0);
        const double out_z = sx.chest == 2 ? -0.01 : -0.04;
        pair(p.pieces, cone(off(N, C, 0.1, 0.15, z), 0.07 * u, off(ls, C, -0.18, -0.06, out_z), 0.07 * u),
             cone(off(N, C, -0.1, 0.15, z), 0.07 * u, off(rs, C, 0.18, -0.06, out_z), 0.07 * u), 0.16 * u);
    }
    // (a low form on the trunk is a full ellipsoid set deep in it, only its crown above the surface: its edge soft
    // where the two surfaces part slowly, firmer where they part fast — a pectoral's lower edge)
    // the shoulder blades
    pair(p.pieces, ellipsoid(off(W, C, 0.4 * sh * g, -0.98, 0.1 * d_chest), C, radii(0.33 * g, 0.4, 0.41 * d_chest)),
         ellipsoid(off(W, C, -0.4 * sh * g, -0.98, 0.1 * d_chest), C, radii(0.33 * g, 0.4, 0.41 * d_chest)), 0.12 * u);
    // the chest: the pectorals (a man's fuller), or the breasts on the ribcage
    if (sx.chest == 2) {
        pair(p.pieces, ellipsoid(off(W, C, 0.33 * sh * g + 0.04, -0.66, -0.4 * d_chest), C, radii(0.27 * g, 0.29 * g, 0.26 * g)),
             ellipsoid(off(W, C, -0.33 * sh * g - 0.04, -0.66, -0.4 * d_chest), C, radii(0.27 * g, 0.29 * g, 0.26 * g)), 0.3 * u);
    } else {
        const double full = sx.chest == 1 ? 1.0 : 0.4;
        const double z = (0.04 - 0.018 * full) * d_chest;
        pair(p.pieces, ellipsoid(off(W, C, 0.38 * sh * g, -0.95, z), C, radii(0.43 * sh * g, 0.44, 0.5 * d_chest)),
             ellipsoid(off(W, C, -0.38 * sh * g, -0.95, z), C, radii(0.43 * sh * g, 0.44, 0.5 * d_chest)), 0.1 * u);
    }
    // the furrow of the spine down the back (shallow), along the chest and along the waist
    {
        Piece furrow_high = carved(cone(Vec3{0.0, -2.25 * u, 0.6 * d_chest * u}, 0.04 * u, Vec3{0.0, -1.3 * u, 0.495 * d_chest * u}, 0.055 * u), 0.14 * u);
        Piece furrow_low = carved(cone(Vec3{0.0, -1.3 * u, 0.495 * d_chest * u}, 0.055 * u, Vec3{0.0, -0.5 * u, 0.455 * dd * u}, 0.045 * u), 0.14 * u);
        furrow_high.rest = true;
        furrow_low.rest = true;
        pair(p.pieces, furrow_high, furrow_low, 0.14 * u);
        p.pieces[p.pieces.size() - 2].carve = true;
        p.pieces[p.pieces.size() - 1].carve = true;
    }
    return out;
}

}  // namespace

Part torso_part(const mesh3d::Skeleton& sk) { return torso_of(sk, dims_of(sk)).part; }

Part head_part(const mesh3d::Skeleton& sk) { return head_part(sk.point("head"), sk.turn("head"), sk.unit); }

// --- the meshes ----------------------------------------------------------------------------------------------------

FigureMesh figure(const mesh3d::Skeleton& sk, const HandShape& left, const HandShape& right) {
    const Dims dm = dims_of(sk);
    const double u = dm.u;
    const double g = dm.g;
    const double gs = dm.gs;
    const double legs = dm.legs;
    const Sex& sx = dm.sx;

    const Vec3& N = sk.point("neck");
    const Vec3& HB = sk.point("head_base");
    const Vec3& Hc = sk.point("head");
    const Mat3& C = sk.turn("chest");
    const Mat3& Hd = sk.turn("head");
    const auto off = [u](const Vec3& o, const Mat3& m, double x, double y, double z) { return add(o, out_of(m, Vec3{x * u, y * u, z * u})); };
    const auto radii = [u](double x, double y, double z) { return Vec3{x * u, y * u, z * u}; };
    const auto st = [u](double y, double rx, double rz, double zc) { return Station{y * u, rx * u, rz * u, zc * u}; };

    Field f;
    enum { kTorso, kNeck };
    // (sizes in u, the skeleton's unit: a head is 1.1 u tall; an adult's widths, girths and depths as such)
    Torso torso = torso_of(sk, dm);
    f.parts.push_back(std::move(torso.part));  // 0 the torso as one smooth body
    const Vec3 neck_low = off(N, C, 0.0, 0.1, 0.05);
    const Vec3 neck_top = off(HB, Hd, 0.0, -0.05, 0.1);
    const double r_neck_low = 0.3 * u * gs * sx.neck;
    const double r_neck_top = 0.25 * u * gs * sx.neck;
    {  // 1 the neck (the head is meshed on its own, finer grid, with the neck's top: below)
        Part p;
        p.pieces.push_back(cone(neck_low, r_neck_low, neck_top, r_neck_top));
        f.parts.push_back(std::move(p));
    }
    f.joints.push_back(Joint{kTorso, kNeck, off(N, C, 0.0, 0.1, 0.03), 0.3 * u, 0.6 * u, 0.0});

    struct Arm {
        int fore;
        Vec3 elbow;
        Vec3 wrist;
        std::vector<Piece> end;  // (the forearm's pieces: the hand's stub is the same)
        double k;                // (and the width they are joined by)
        Vec3 wrist_radii;        // (in the wrist's frame: thick, along, wide)
    };
    Arm arms[2];
    for (int side = 0; side < 2; ++side) {
        const std::string s = side == 0 ? "l" : "r";
        const double sg = side == 0 ? 1.0 : -1.0;
        const double limb = dm.arm;
        const Vec3& shoulder = sk.point(s + "_shoulder");
        const Vec3& elbow = sk.point(s + "_elbow");
        const Vec3& wrist = sk.point(s + "_wrist");
        const Mat3& arm_r = sk.turn(s + "_arm");
        const Mat3& fore_r = sk.turn(s + "_elbow");
        const Mat3& hand_r = sk.turn(s + "_wrist");
        const double r_elbow = 0.165 * limb;
        // (the arm's sections from the shoulder to the wrist, one run through the elbow)
        const std::vector<Station> arm_run{st(0.0, 0.235 * limb, 0.245 * limb, 0.0),  st(0.55, 0.205 * limb, 0.235 * limb, -0.01), st(1.15, 0.172 * limb, 0.19 * limb, 0.0),
                                           st(1.5, r_elbow, 1.04 * r_elbow, 0.01),      st(1.82, 0.172 * limb, 0.19 * limb, 0.0),     st(2.4, 0.118 * limb, 0.13 * limb, 0.0),
                                           st(2.8, 0.088 * limb, 0.092 * limb, 0.0)};
        const std::vector<Station> arm_slopes = run_slopes(arm_run);
        {  // the upper arm: from the shoulder's ball, fullest at the biceps, to the elbow; the deltoid on its outer side
            Part p;
            p.k = 0.1 * u;
            const std::vector<Station> slopes = stations_of(arm_slopes, 0, 3, 0.0);
            p.pieces.push_back(loft(shoulder, arm_r, stations_of(arm_run, 0, 3, 0.0), 0.235 * u * limb, r_elbow * u, &slopes));
            p.pieces.push_back(ellipsoid(off(shoulder, arm_r, sg * 0.045 * limb, 0.2, 0.0), arm_r, radii(0.225 * limb, 0.38, 0.25 * limb)));
            f.parts.push_back(std::move(p));
        }
        const int upper = static_cast<int>(f.parts.size()) - 1;
        const Vec3 wrist_radii = radii(0.074 * limb, 0.17, 0.108 * limb);
        {  // the forearm: fullest below the elbow, narrow to the wrist; the wrist flat, turned as the hand is
            Part p;
            p.k = 0.08 * u;
            const std::vector<Station> slopes = stations_of(arm_slopes, 3, 6, 0.0);
            p.pieces.push_back(loft(elbow, fore_r, stations_of(arm_run, 3, 6, -1.5 * u), r_elbow * u, 0.08 * u * limb, &slopes));
            p.pieces.push_back(ellipsoid(wrist, hand_r, wrist_radii));
            f.parts.push_back(std::move(p));
        }
        const int fore = upper + 1;
        f.joints.push_back(Joint{kTorso, upper, shoulder, 0.2 * u, 1.1 * u, 0.55});
        f.joints.push_back(Joint{upper, fore, elbow, 0.07 * u * std::min(1.0, bend_of(arm_r, fore_r) / 0.3), 0.3 * u, 0.3});
        arms[side] = Arm{fore, elbow, wrist, f.parts[static_cast<std::size_t>(fore)].pieces, f.parts[static_cast<std::size_t>(fore)].k, wrist_radii};

        const Vec3& hip = sk.point(s + "_hip");
        const Vec3& knee = sk.point(s + "_knee");
        const Vec3& ankle = sk.point(s + "_ankle");
        const Mat3& thigh_r = sk.turn(s + "_leg");
        const Mat3& shin_r = sk.turn(s + "_knee");
        const Mat3& foot_r = sk.turn(s + "_ankle");
        const double leg = dm.leg;
        const double r_knee = 0.245 * g;
        const double r_ankle = 0.142 * gs;
        const double med = -sg;  // (toward the other leg)
        // (the leg's sections from the hip to the ankle, one run through the knee: thick at the top, the knee a little
        // forward, the shin straight in front and the calf behind, narrow to the ankle)
        const std::vector<Station> leg_run{st(0.0, 0.39 * leg, 0.41 * leg, -0.01 * leg),      st(0.85 * legs, 0.335 * leg, 0.365 * leg, -0.025),
                                           st(1.6 * legs, 0.262 * g, 0.275 * g, -0.012),      st(2.0 * legs, r_knee, 1.05 * r_knee, -0.03 * g),
                                           st(2.25 * legs, 0.228 * g, 0.24 * g, -0.005 * g),  st(2.6 * legs, 0.232 * g, 0.258 * g, 0.05 * g),
                                           st(3.3 * legs, 0.165 * g, 0.175 * g, 0.02 * g),    st(3.95 * legs, r_ankle, 1.08 * r_ankle, 0.01)};
        const std::vector<Station> leg_slopes = run_slopes(leg_run);
        {  // the thigh: a ball in the hip
            Part p;
            p.k = 0.1 * u;
            const std::vector<Station> slopes = stations_of(leg_slopes, 0, 3, 0.0);
            p.pieces.push_back(loft(hip, thigh_r, stations_of(leg_run, 0, 3, 0.0), 0.36 * u * leg, r_knee * u, &slopes));
            // (the inner thigh: the two meet under the crotch, when the build fills the room between the hips)
            const double inner = std::max(0.0, std::min(dm.hw - 0.3 * leg, 0.26 * leg));
            p.pieces.push_back(with_blend(cone(off(hip, thigh_r, med * inner, 0.2, 0.01), 0.3 * u * leg, off(hip, thigh_r, med * 0.03, 1.35 * legs, 0.0), 0.2 * u * leg), 0.25 * u));
            f.parts.push_back(std::move(p));
        }
        const int thigh = static_cast<int>(f.parts.size()) - 1;
        {  // the shin
            Part p;
            p.k = 0.08 * u;
            const std::vector<Station> slopes = stations_of(leg_slopes, 3, 7, 0.0);
            p.pieces.push_back(loft(knee, shin_r, stations_of(leg_run, 3, 7, -2.0 * legs * u), r_knee * u, r_ankle * u, &slopes));
            f.parts.push_back(std::move(p));
        }
        const int shin = thigh + 1;
        {  // the foot, a run of sections from the heel to the toes (wide and low at the ball, a wedge at the toes), the
            // instep rising to the ankle, the big toe's side longer; the sole flat (each section reaches 0.04 u under the
            // sole's plane, the rounding of its cut: the sole is the plane itself)
            Part p;
            p.k = 0.1 * u;
            Mat3 along{};  // (the run goes forward: its width the foot's x, its height the foot's down)
            for (std::size_t r = 0; r < 3; ++r) {
                along[r][0] = foot_r[r][0];
                along[r][1] = -foot_r[r][2];
                along[r][2] = foot_r[r][1];
            }
            p.pieces.push_back(loft(ankle, along,
                                    {st(-0.17, 0.12 * gs, 0.145, 0.2), st(0.03, 0.15 * gs, 0.17, 0.175), st(0.36, 0.165 * gs, 0.135, 0.21), st(0.66, 0.205 * gs, 0.105, 0.24),
                                     st(0.9, 0.175 * gs, 0.075, 0.265)},
                                    0.09 * u, 0.09 * u));
            p.pieces.push_back(with_blend(cone(ankle, r_ankle * u, off(ankle, foot_r, 0.0, 0.1, -0.32), 0.1 * u * gs), 0.16 * u));  // the instep
            p.pieces.push_back(ellipsoid(off(ankle, foot_r, med * 0.095 * gs, 0.265, -0.9), foot_r, radii(0.085 * gs, 0.078, 0.12)));  // the big toe
            p.cut = true;
            p.cut_n = la::column(foot_r, 1);
            p.cut_d = dot3(p.cut_n, ankle) + 0.30 * u;
            p.cut_k = 0.04 * u;
            f.parts.push_back(std::move(p));
        }
        const int foot = shin + 1;
        f.joints.push_back(Joint{kTorso, thigh, hip, 0.16 * u, 1.0 * u, 0.4});
        f.joints.push_back(Joint{thigh, shin, knee, 0.09 * u * std::min(1.0, bend_of(thigh_r, shin_r) / 0.3), 0.4 * u, 0.3});
        f.joints.push_back(Joint{shin, foot, ankle, 0.07 * u, 0.3 * u, 0.0});
    }

    FigureMesh out;
    out.mesh = surface_nets::mesh(f, u * std::max(1.0, std::sqrt(g)) / kBodyCells);  // (a heavier body: coarser, the same count)
    // (the forearm's outline stops short of the wrist: the hand's own takes over there)
    for (std::size_t i = 0; i < out.mesh.v.size(); ++i) {
        for (const Arm& arm : arms) {
            if (out.mesh.part[i] != arm.fore) continue;
            const Vec3 e = unit(sub(arm.elbow, arm.wrist));
            if (dot3(sub(out.mesh.v[i], arm.wrist), e) < 0.06 * u) out.mesh.no_contour[i] = 1;
        }
    }
    {  // the head and the neck's top, on a grid of u / kHeadCells: the face's relief needs it
        Field hf;
        hf.parts.push_back(head_part(Hc, Hd, u));
        Part stub;
        // (the neck's own cone, 0.004 u thinner — it stays under the body's coarser mesh of the same neck, which is the
        // neck one sees —, kept down to 0.3 u below the neck's top. The head runs into it at the nape; under the jaw the
        // two meet in a plain crease, which the body's neck crosses cleanly)
        stub.pieces.push_back(cone(neck_low, r_neck_low, neck_top, r_neck_top));
        const Vec3 down = unit(sub(neck_low, neck_top));
        stub.cut = true;
        stub.cut_n = down;
        stub.cut_d = dot3(down, neck_top) + 0.3 * u;
        stub.inset = 0.004 * u;
        hf.parts.push_back(std::move(stub));
        hf.joints.push_back(Joint{0, 1, off(HB, Hd, 0.0, -0.05, 0.22), 0.25 * u, 0.5 * u, 0.0});  // (at the nape: the chin stays clear of the neck)
        Smooth hm = surface_nets::mesh(hf, u / kHeadCells);
        // (the outline of the neck changes from the body's to the head's 0.12 u below the neck's top)
        for (std::size_t i = 0; i < hm.v.size(); ++i) {
            if (hm.part[i] == 1 && dot3(sub(hm.v[i], neck_top), down) > 0.12 * u) hm.no_contour[i] = 1;
        }
        for (std::size_t i = 0; i < out.mesh.v.size(); ++i) {
            if (out.mesh.part[i] == kNeck && dot3(sub(out.mesh.v[i], neck_top), down) < 0.12 * u) out.mesh.no_contour[i] = 1;
        }
        plain_outline(hm, hf.parts[0], u);
        append(out.mesh, hm, [](const Vec3& c) { return c; }, [](const Vec3& c) { return c; }, false);
        out.marks = marks_on(hf.parts[0], Hc, Hd, u);
    }

    for (int side = 0; side < 2; ++side) {
        const std::string s = side == 0 ? "l" : "r";
        const double sg = side == 0 ? 1.0 : -1.0;
        const Arm& arm = arms[side];
        const Mat3& T = sk.turn(s + "_wrist");
        // the hand's frame in the figure: (a, w, n) → (−side n, a, −w), turned by the wrist
        const auto local = [sg](const Vec3& c) { return Vec3{-sg * c[2], c[0], -c[1]}; };
        const auto to_hand = [&](const Vec3& v) {  // (a direction of the figure in the hand's frame)
            const Vec3 t = into(T, v);
            return Vec3{t[1], -t[2], -sg * t[0]};
        };
        const Vec3& wrist = arm.wrist;
        Stub stub;
        stub.on = true;
        for (const Piece& piece : arm.end) {
            stub.pieces.push_back(carried(piece, [&](const Vec3& v) { return to_hand(sub(v, wrist)); }, to_hand));
        }
        stub.k = arm.k;
        stub.toward = unit(to_hand(sub(arm.elbow, wrist)));
        stub.cut = 0.15 * u;
        stub.shrink = 0.01 * u;
        stub.wrist = Vec3{arm.wrist_radii[1], arm.wrist_radii[2], arm.wrist_radii[0]};
        const double hand_len = 0.8 * u * sx.hand;
        const Field hf = hand_field(hand_len, side == 0 ? left : right, stub);
        Smooth hm = surface_nets::mesh(hf, hand_len / kHandCells);
        for (std::size_t i = 0; i < hm.v.size(); ++i) {
            if (hm.part[i] == 6 && dot3(hm.v[i], stub.toward) > 0.06 * u) hm.no_contour[i] = 1;
        }
        append(out.mesh, hm, [&](const Vec3& c) { return add(wrist, out_of(T, local(c))); }, [&](const Vec3& c) { return out_of(T, local(c)); },
               side == 1);
    }
    return out;
}

FigureMesh head(double u) {
    Field f;
    f.parts.push_back(head_part(Vec3{0.0, 0.0, 0.0}, kIdentity, u));
    FigureMesh out;
    out.mesh = surface_nets::mesh(f, u / kHeadCells);
    plain_outline(out.mesh, f.parts[0], u);
    out.marks = marks_on(f.parts[0], Vec3{0.0, 0.0, 0.0}, kIdentity, u);
    return out;
}

Smooth hand(double length, const HandShape& shape, int side) {
    const Field f = hand_field(length, shape, Stub{});
    const Smooth hm = surface_nets::mesh(f, length / kHandCells);
    const double sg = side >= 0 ? 1.0 : -1.0;
    const double half = length / 2;
    // (a, w, n) → (−side w, a − length / 2, n): the palm away from the viewer
    Smooth out;
    append(out, hm, [&](const Vec3& c) { return Vec3{-sg * c[1], c[0] - half, c[2]}; }, [&](const Vec3& c) { return Vec3{-sg * c[1], c[0], c[2]}; },
           sg < 0);
    return out;
}

}  // namespace genko::core::figure_shape
