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

// --- the head: an egg of a skull, the face in front of and below it, the chin and the jaw's corners, cut flat under
// the jaw (the jaw's line); a mannequin's face: the brow's ridge, the eyes as smooth shallow hollows under it, the nose
// and its bridge, closed lips; small ears flat against the head between the eyes' and the nose's height -------------

constexpr double kHeadNormalSteps = 1.0;  // (the head's light: its relief is a few cells across — surface_nets::mesh)
constexpr std::size_t kPlainHead = 6;  // (the head's first pieces: the skull, the face, the chin, the jaw's corners and the cut under them, without the relief)

Part head_part(const Vec3& centre, const Mat3& turn, double u) {
    const auto at = [&](double x, double y, double z) { return add(centre, out_of(turn, Vec3{x * u, y * u, z * u})); };
    const auto r = [u](double x, double y, double z) { return Vec3{x * u, y * u, z * u}; };
    Part head;
    head.k = 0.16 * u;
    std::vector<Piece>& p = head.pieces;
    p.push_back(ellipsoid(at(0.0, -0.11, 0.055), turn, r(0.375, 0.43, 0.445)));  // the skull
    p.push_back(ellipsoid(at(0.0, 0.14, -0.1), turn, r(0.29, 0.39, 0.335)));     // the face
    p.push_back(with_blend(ellipsoid(at(0.0, 0.41, -0.26), turn, r(0.13, 0.125, 0.12)), 0.12 * u));  // the chin
    pair(p, ellipsoid(at(0.165, 0.28, -0.03), turn, r(0.09, 0.14, 0.16)), ellipsoid(at(-0.165, 0.28, -0.03), turn, r(0.09, 0.14, 0.16)), 0.14 * u);  // the jaw's corners
    // The line of the jaw: what is under a broad, nearly flat form — from below the chin back and up to under the
    // jaw's corners — is taken away, so the cheek and the chin turn under along one clear edge (round over 0.05 u)
    // to the shallow dish between the jaw and the neck: the jaw reads from the front, three quarters and the side,
    // and the chin stands over the neck. The form falls away at its ends, so it leaves the head with no step behind
    // the jaw or at the nape.
    {
        const Vec3 down{0.0, 0.912, 0.41};     // (the form's own: down and a little back)
        const Vec3 along{0.0, -0.41, 0.912};
        const double thick = 0.25;
        const Vec3 on{0.0, 0.525 - 0.41 * 0.12, -0.27 + 0.912 * 0.12};  // (where its top is, under the middle of the jaw)
        Mat3 m{};
        m[0] = Vec3{1.0, down[0], along[0]};
        m[1] = Vec3{0.0, down[1], along[1]};
        m[2] = Vec3{0.0, down[2], along[2]};
        Mat3 world{};
        for (std::size_t col = 0; col < 3; ++col) {
            const Vec3 v = out_of(turn, Vec3{m[0][col], m[1][col], m[2][col]});
            for (std::size_t row = 0; row < 3; ++row) world[row][col] = v[row];
        }
        Piece under = ellipsoid(at(on[0] + down[0] * thick, on[1] + down[1] * thick, on[2] + down[2] * thick), world, r(1.2, thick, 0.45));
        under.carve = true;
        p.push_back(with_blend(std::move(under), 0.05 * u));
    }
    // the eyes: a smooth, shallow hollow under the brow on each side of the nose's bridge, taken out of the face — a
    // broad dish (its walls slope gently: no rim on the cheek), so the eye is a soft shadow under the brow
    {
        Piece one = ellipsoid(at(0.155, 0.068, -0.465), turn, r(0.14, 0.095, 0.1));
        Piece other = ellipsoid(at(-0.155, 0.068, -0.465), turn, r(0.14, 0.095, 0.1));
        one.carve = true;
        other.carve = true;
        pair(p, std::move(one), std::move(other), 0.08 * u);
    }
    // the brow: a ridge over the eyes, a little proud of the forehead (set after the hollows: its round underside
    // runs down into them whole, with no thin edge left of it)
    p.push_back(with_blend(ellipsoid(at(0.0, -0.055, -0.265), turn, r(0.29, 0.09, 0.19)), 0.12 * u));
    // the nose: its bridge from inside the brow down to the tip, and the tip (set after the hollows: it stands clear
    // between them)
    p.push_back(with_blend(cone(at(0.0, -0.045, -0.39), 0.05 * u, at(0.0, 0.165, -0.478), 0.044 * u), 0.08 * u));
    p.push_back(with_blend(ellipsoid(at(0.0, 0.188, -0.45), turn, r(0.072, 0.05, 0.06)), 0.06 * u));
    // the mouth: the round of the teeth, the closed lips on it as one soft form, the hollow between the lips and the chin
    p.push_back(with_blend(ellipsoid(at(0.0, 0.3, -0.285), turn, r(0.15, 0.1, 0.125)), 0.08 * u));
    p.push_back(swell(at(0.0, 0.3, -0.4), turn, r(0.16, 0.08, 0.09), 0.014 * u));
    p.push_back(swell(at(0.0, 0.39, -0.37), turn, r(0.13, 0.07, 0.09), -0.012 * u));
    pair(p, ellipsoid(at(0.318, 0.1, 0.07), turn, r(0.035, 0.095, 0.06)), ellipsoid(at(-0.318, 0.1, 0.07), turn, r(0.035, 0.095, 0.06)), 0.06 * u);  // the ears
    {  // (and the hollow of each)
        Piece one = swell(at(0.35, 0.1, 0.062), turn, r(0.05, 0.062, 0.036), -0.013 * u);
        one.paired = true;
        p.push_back(std::move(one));
        p.push_back(swell(at(-0.35, 0.1, 0.062), turn, r(0.05, 0.062, 0.036), -0.013 * u));
    }
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

// The directions of the light on a part that thins toward a seam (Part::fade), where `where` says so: those of the
// same field with the part unthinned — the seam's two meshes then take the light as one surface.
template <typename Where>
void plain_normals(Smooth& mesh, Field field, const std::vector<int>& parts, double u, Where where) {
    for (const int part : parts) field.parts[static_cast<std::size_t>(part)].fade = 0.0;
    std::vector<double> d(field.parts.size());
    const double e = 0.02 * u;
    const auto at = [&](double x, double y, double z) { return field.at(Vec3{x, y, z}, nullptr, d.data()); };
    for (std::size_t i = 0; i < mesh.v.size(); ++i) {
        if (std::find(parts.begin(), parts.end(), static_cast<int>(mesh.part[i])) == parts.end()) continue;
        const Vec3& x = mesh.v[i];
        if (!where(x)) continue;
        const Vec3 g{at(x[0] + e, x[1], x[2]) - at(x[0] - e, x[1], x[2]), at(x[0], x[1] + e, x[2]) - at(x[0], x[1] - e, x[2]), at(x[0], x[1], x[2] + e) - at(x[0], x[1], x[2] - e)};
        const double len = length(g);
        if (len > 0.0) mesh.n[i] = Vec3{g[0] / len, g[1] / len, g[2] / len};
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
    double crest = 0.13;  // (how much narrower than the thighs' tops the crest of the hips may be)
};

Sex sex_of(const Json& body) {
    const Json* sex = get(body, "sex");
    const std::string name = sex != nullptr && sex->is_string() ? sex->get<std::string>() : std::string();
    if (name == "male") return Sex{1.02, 1.04, 1.04, 0.97, 0.95, 1.08, 1.0, 1.0, 1.06, 1};
    if (name == "female") return Sex{1.05, 0.97, 1.02, 1.0, 1.08, 1.0, 1.06, 0.92, 0.96, 2, 0.1};
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

Piece swell(const Vec3& centre, const Mat3& axes, const Vec3& radii, double amount, bool flat) {
    Piece p;
    p.kind = Piece::Kind::Swell;
    p.round = flat ? 1.0 : 0.0;
    p.a = centre;
    p.axes = axes;
    p.size = radii;
    p.ra = amount;
    p.centre = centre;
    p.reach = std::max(radii[0], std::max(radii[1], radii[2]));
    return p;
}

Piece slab(const Vec3& centre, const Mat3& axes, const Vec3& radii, double amount, const Vec3* back, double arch, double arch_round) {
    Piece p = swell(centre, axes, radii, amount);
    p.round = 2.0;
    p.b = back != nullptr ? *back : radii;
    p.arch = arch;
    p.arch_round = arch_round;
    // (the ball that holds it: its axes may slant and its edge fall away, so the box's corner is further out than the
    // radii say — twice the longest, and how far the arch falls, is more than any slant here reaches)
    p.reach = 2.0 * std::max(p.reach, std::max(p.b[0], std::max(p.b[1], p.b[2]))) + std::fabs(arch) * std::max(radii[0], p.b[0]);
    return p;
}

std::vector<Station> run_slopes(const std::vector<Station>& stations, const Station* first, const Station* last) {
    const std::size_t n = stations.size();
    std::vector<Station> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i].y = stations[i].y;
    if (n == 0) return out;
    const auto of = [](const Station* st, int which) { return st == nullptr ? 0.0 : (which == 0 ? st->rx : (which == 1 ? st->rz : st->zc)); };
    if (n < 3) {
        for (int which = 0; which < 3; ++which) {
            (which == 0 ? out[0].rx : (which == 1 ? out[0].rz : out[0].zc)) = of(first, which);
            (which == 0 ? out[n - 1].rx : (which == 1 ? out[n - 1].rz : out[n - 1].zc)) = of(n == 1 ? first : last, which);
        }
        return out;
    }
    // (the clamped spline's own equations, h[i] m[i−1] + 2 (h[i−1] + h[i]) m[i] + h[i−1] m[i+1] = 3 (h[i] s[i−1] + h[i−1] s[i]),
    // with m given at both ends (0, or `first` and `last`): one sweep down, one back)
    for (int which = 0; which < 3; ++which) {
        const auto v = [&](std::size_t i) { return which == 0 ? stations[i].rx : (which == 1 ? stations[i].rz : stations[i].zc); };
        std::vector<double> upper(n, 0.0);
        std::vector<double> right(n, 0.0);
        right[0] = of(first, which);
        for (std::size_t i = 1; i + 1 < n; ++i) {
            const double h0 = stations[i].y - stations[i - 1].y;
            const double h1 = stations[i + 1].y - stations[i].y;
            const double rhs = 3.0 * (h1 * (v(i) - v(i - 1)) / h0 + h0 * (v(i + 1) - v(i)) / h1);
            const double pivot = 2.0 * (h0 + h1) - h1 * upper[i - 1];
            upper[i] = h0 / pivot;
            right[i] = (rhs - h1 * right[i - 1]) / pivot;
        }
        std::vector<double> m(n, 0.0);
        m[0] = of(first, which);
        m[n - 1] = of(last, which);
        for (std::size_t i = n - 2; i >= 1; --i) m[i] = right[i] - upper[i] * m[i + 1];
        for (std::size_t i = 0; i < n; ++i) (which == 0 ? out[i].rx : (which == 1 ? out[i].rz : out[i].zc)) = m[i];
    }
    return out;
}

// (a loft's numbers: the count n; the n stations' y; then for each of the n − 1 spans 1 / its length and the cubics'
// four coefficients of rx, rz and zc, in the span's own 0..1)
Piece loft(const Vec3& origin, const Mat3& axes, const std::vector<Station>& stations, double cap0, double cap1, const std::vector<Station>* slopes, double full0,
           double full1) {
    Piece p;
    p.kind = Piece::Kind::Loft;
    p.size = Vec3{full0, full1, 0.0};
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
    // (the ball that holds it: about the middle of its whole length, domes and all; a dome that leans reaches further
    // across)
    const double y0 = stations.front().y - cap0;
    const double y1 = stations.back().y + cap1;
    const double half = (y1 - y0) * 0.5;
    const double across = 1.1 * widest + std::max(std::fabs(own.front().zc) * cap0, std::fabs(own.back().zc) * cap1);
    p.centre = add(origin, out_of(axes, Vec3{0.0, (y0 + y1) * 0.5, 0.0}));
    p.reach = std::sqrt(half * half + across * across);
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

// The same for the dome that closes a loft's end: half an ellipsoid (rx, ry, rz), fuller by `full` (its profile
// √(1 − (1 − full) t² − full t⁴)), whose middle moves by dzc for each unit of y (x, y, z from the middle at y = 0). At
// y = 0 it is the loft's own value where the loft's section is flat there but for its middle's slope.
inline double dome_at(double x, double y, double z, double rx, double ry, double rz, double full, double dzc) {
    const double qx = x / rx;
    const double qy = y / ry;
    const double qz = z / rz;
    const double y2 = qy * qy;
    const double k0 = std::sqrt(qx * qx + qz * qz + y2 * ((1.0 - full) + full * y2));
    if (!(k0 > 0.0)) return -std::min(rx, std::min(ry, rz));
    const double gx = qx / rx;
    const double gy = qy * ((1.0 - full) + 2.0 * full * y2) / ry - qz * dzc / rz;
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
            // (a flat end — a run's own — may lean and be fuller; an end the form goes on through, at a joint, is the
            // plain half ellipsoid)
            if (c[2] == 0.0 && c[6] == 0.0) return dome_at(q[0], q[1] - ys[0], q[2] - (c[9] + c[10] * t), rx, ra, rz, size[0], c[10] * c[0]);
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
            const double drx = c[2] + 2.0 * c[3] + 3.0 * c[4];
            const double drz = c[6] + 2.0 * c[7] + 3.0 * c[8];
            if (drx == 0.0 && drz == 0.0) return dome_at(q[0], q[1] - ys[n - 1], q[2] - zc, rx, rb, rz, size[1], (c[10] + 2.0 * c[11] + 3.0 * c[12]) * c[0]);
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
    case Kind::Swell: break;  // (no solid: Part::at lifts the surface by it)
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

// How far a swell lifts the surface at x: its amount × (1 − r²)³ within its ellipsoid (a flat one: (1 − r⁴)³; a slab:
// (1 − (x⁴ + y⁴ + z⁴))³ within its rounded box).
inline double lift_of(const Piece& p, const Vec3& x) {
    const Vec3 r = sub(x, p.a);
    if (!(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] < p.reach * p.reach)) return 0.0;  // (outside the ball that holds it)
    const Vec3 q = into(p.axes, r);
    if (p.round > 1.5) {
        // (each side of the middle has its own radius: 1 − q⁴ is flat enough at the middle for the two to meet unseen)
        const double qx = q[0] / (q[0] < 0.0 ? p.b[0] : p.size[0]);
        const double along = p.arch != 0.0 ? q[1] - p.arch * (std::sqrt(q[0] * q[0] + p.arch_round * p.arch_round) - p.arch_round) : q[1];
        const double qy = along / (along < 0.0 ? p.b[1] : p.size[1]);
        const double qz = q[2] / (q[2] < 0.0 ? p.b[2] : p.size[2]);
        const double x2 = qx * qx;
        const double y2 = qy * qy;
        const double z2 = qz * qz;
        const double t = 1.0 - (x2 * x2 + y2 * y2 + z2 * z2);
        return t > 0.0 ? p.ra * (t * t * t) : 0.0;
    }
    const double qx = q[0] / p.size[0];
    const double qy = q[1] / p.size[1];
    const double qz = q[2] / p.size[2];
    const double r2 = qx * qx + qy * qy + qz * qz;
    const double t = p.round > 0.0 ? 1.0 - r2 * r2 : 1.0 - r2;
    return t > 0.0 ? p.ra * (t * t * t) : 0.0;
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
        // (the mix rises as t³ (10 − 15 t + 6 t²): its slope and its curvature both begin and end at nothing, so
        // the bent trunk's curve has no step where a stretch begins)
        const double s1 = t1 * t1 * t1 * (10.0 + t1 * (6.0 * t1 - 15.0));
        const double s2 = t2 * t2 * t2 * (10.0 + t2 * (6.0 * t2 - 15.0));
        const Vec3 hs = sub(qs, qh);
        const Vec3 sc = sub(qc, qs);
        q = add(add(qh, mul(hs, s1)), mul(sc, s2));
        const double steeper =
            30.0 * t1 * t1 * (1.0 - t1) * (1.0 - t1) / bend.over[0] * length(hs) + 30.0 * t2 * t2 * (1.0 - t2) * (1.0 - t2) / bend.over[1] * length(sc);
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
        const bool two = p.paired && i + 1 < count;
        if (p.kind == Piece::Kind::Swell) {  // (a left and right pair: summed first, the same either way round)
            double lift = lift_of(p, p.rest ? q : x);
            if (two) {
                ++i;
                lift = lift + lift_of(all[i], all[i].rest ? q : x);
            }
            v = v - lift;
            continue;
        }
        const double kk = p.blend >= 0.0 ? p.blend : k;
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
        const Part& part = parts[i];
        if (part.cut && best >= 0) {
            // (a cut part is never nearer than its cutting plane: past the plane by more than what is nearest so far
            // and the widest blend it takes part in, it cannot change the field — the plane's distance stands for it,
            // and the part is not worked out: the same field, sooner. The head's mesh carries the torso this way.)
            const double g = dot3(part.cut_n, x) - part.cut_d;
            if (g >= f + 1.25 * blend_of(static_cast<int>(i))) {
                d[i] = g;
                continue;
            }
        }
        d[i] = part.at(x);
        if (d[i] < f || best < 0) {
            f = d[i];
            best = static_cast<int>(i);
        }
    }
    // (how much of a width is left at x: all of it within flat × radius of the place, (1 − s²)² past that, s from 0
    // there to 1 at the radius)
    const auto share = [&x](const Vec3& at, double radius, double flat) {
        const Vec3 r = sub(x, at);
        const double t2 = dot3(r, r) / (radius * radius);
        if (!(t2 < 1.0)) return 0.0;
        if (!(t2 > flat * flat)) return 1.0;
        const double s = (std::sqrt(t2) - flat) / (1.0 - flat);
        return (1.0 - s * s) * (1.0 - s * s);
    };
    for (const Joint& j : joints) {
        if (use != nullptr && (use[j.p] == 0 || use[j.c] == 0)) continue;
        double k = j.k * share(j.at, j.radius, j.flat);
        if (j.more_k > 0.0) k = k + j.more_k * share(j.more, j.more_radius, j.more_flat);
        if (!(k > 0.0)) continue;
        f = std::min(f, blend_min(d[j.p], d[j.c], k));
    }
    if (nearest != nullptr) *nearest = best;
    return f;
}

double Field::blend_of(int part) const {
    double k = 0.0;
    for (const Joint& j : joints) {
        if (j.p == part || j.c == part) k = std::max(k, j.k + j.more_k);
    }
    return k;
}

void Field::bounds(Vec3& lo, Vec3& hi) const {
    const double inf = std::numeric_limits<double>::infinity();
    lo = Vec3{inf, inf, inf};
    hi = Vec3{-inf, -inf, -inf};
    double k = 0.0;
    for (const Joint& j : joints) k = std::max(k, j.k + j.more_k);
    for (const Part& part : parts) {
        Vec3 plo{inf, inf, inf};
        Vec3 phi{-inf, -inf, -inf};
        for (const Piece& p : part.pieces) {
            if (p.carve || p.kind == Piece::Kind::Swell) continue;
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
                for (std::size_t a = 0; a < 3; ++a) {
                    plo[a] = std::min(plo[a], c[a] - p.reach - kk);
                    phi[a] = std::max(phi[a], c[a] + p.reach + kk);
                }
            }
        }
        if (!(plo[0] <= phi[0])) continue;
        if (part.cut) {
            // (a cut part: only what is on the kept side of its plane — the box of its pieces, as far along each axis
            // as the plane lets a point of the box go; the cut's rounding and the blends reach a little past it)
            const double past = part.cut_d + part.cut_k + k;
            Vec3 clo = plo;
            Vec3 chi = phi;
            for (std::size_t a = 0; a < 3; ++a) {
                const double n = part.cut_n[a];
                if (!(std::fabs(n) > 1e-6)) continue;
                double rest = 0.0;  // (the least the other two axes can add to n · x in the box)
                for (std::size_t o = 0; o < 3; ++o) {
                    if (o != a) rest = rest + std::min(part.cut_n[o] * plo[o], part.cut_n[o] * phi[o]);
                }
                const double at = (past - rest) / n;
                if (n > 0.0) {
                    chi[a] = std::min(chi[a], at);
                } else {
                    clo[a] = std::max(clo[a], at);
                }
            }
            plo = clo;
            phi = chi;
            if (!(plo[0] <= phi[0] && plo[1] <= phi[1] && plo[2] <= phi[2])) continue;
        }
        for (std::size_t a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], plo[a]);
            hi[a] = std::max(hi[a], phi[a]);
        }
    }
}

namespace {

constexpr std::size_t kTrunkAndTrapezius = 3;  // (the torso's first pieces: the trunk and the two sides of the trapezius)

// The torso's pieces, and where the fold of each armpit is (on the side of the chest under the shoulder; left, right).
struct Torso {
    Part part;
    Vec3 armpit[2]{};
};

Torso torso_of(const mesh3d::Skeleton& sk, const Dims& d) {
    const double u = d.u;
    const double g = d.g;
    const double dd = d.dd;
    const Sex& sx = d.sx;
    const Vec3& P0 = sk.point("pelvis");
    const Vec3& W = sk.point("waist");
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
    // The trunk's sections as one run from the fullest ribs down to the crest of the hips: the lowest ribs (where the
    // chest turns on the waist), the waist, the crest. Above the ribs the ribcage closes as a dome that leans back to
    // the neck's base; below the crest the pelvis closes as a dome between the thighs, down to the crotch — the hips'
    // sides and the fronts beside it are the thighs' own tops, which start above the hip joints (figure()). So the fold
    // of the groin runs from the hip bone down and in to the crotch, and no line crosses the front.
    const Station ribs = st(-1.92, r_chest, 0.55 * d_chest, -0.015 * dd);
    const Station low = st(-1.3, r_ribs, 0.47 * d_chest, -0.035 * dd);
    const Station waist = st(-0.92, r_waist, 0.4 * dd, -0.035 * dd);
    // (the crest is never much narrower than the thighs' tops beside the hip joints — wide hips flare from the waist
    // to the crest and on to the trochanters in one curve, with no step out to the thigh)
    const double r_crest = std::max(0.42 * r_waist + 0.58 * r_pelvis, d.hw + 0.395 * d.leg - sx.crest * g);
    const Station crest = st(-0.3, r_crest, 0.425 * dd, -0.025 * dd);
    const Station lean{0.0, 0.0, 0.0, -0.14 * dd};  // (the ribcage's dome: its middle goes back by this for each u up)
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
    const std::vector<Station> run{ribs, low, waist, crest};
    const std::vector<Station> slopes = run_slopes(run, &lean);
    Piece trunk = loft(Vec3{0.0, 0.0, 0.0}, kIdentity, run, 1.0 * u, 0.8 * u, &slopes);
    trunk.rest = true;
    p.pieces.push_back(std::move(trunk));
    // the shoulders: the trapezius sloping from the neck out to each (the ball of the shoulder is the upper arm's own
    // top, which the torso runs into at the joint: two copies of it, one in each part, would swell where they blend)
    const Vec3& ls = sk.point("l_shoulder");
    const Vec3& rs = sk.point("r_shoulder");
    const auto rest = [u](double x, double y, double z) { return Vec3{x * u, y * u, z * u}; };
    // (a frame whose first axis runs along `dir`, its third level: for a form laid along a bone)
    const auto frame_along = [](const Vec3& dir) {
        const Vec3 e0 = unit(dir);
        const Vec3 e2 = unit(Vec3{-e0[2], 0.0, e0[0]});
        const Vec3 e1{e2[1] * e0[2] - e2[2] * e0[1], e2[2] * e0[0] - e2[0] * e0[2], e2[0] * e0[1] - e2[1] * e0[0]};
        Mat3 m{};
        for (std::size_t r = 0; r < 3; ++r) m[r] = Vec3{e0[r], e1[r], e2[r]};
        return m;
    };
    // (the shoulders in the body standing straight)
    const auto shoulder_rest = [&](const Vec3& at) {
        Vec3 q = into(C, sub(at, W));
        q[1] = q[1] - 1.3 * u;
        return q;
    };
    const Vec3 lsr = shoulder_rest(ls);
    const Vec3 rsr = shoulder_rest(rs);
    for (int side = 0; side < 2; ++side) {
        const Vec3& at = side == 0 ? lsr : rsr;
        out.armpit[side] = add(W, out_of(C, Vec3{(side == 0 ? 0.98 : -0.98) * r_chest * u, at[1] + 0.42 * u + 1.3 * u, -0.02 * u}));
    }
    {
        const auto trapezius = [&](const Vec3& shoulder, double side) {
            // (a woman's begins higher on the neck: her narrower neck does not read long)
            const Vec3 inner = rest(side * 0.1, sx.chest == 2 ? -2.98 : -2.86, 0.1);
            const Vec3 outer = add(shoulder, rest(-side * 0.1, -0.03, 0.03));
            Piece e = ellipsoid(mul(add(inner, outer), 0.5), frame_along(sub(outer, inner)), Vec3{0.5 * length(sub(outer, inner)) + 0.1 * u, 0.17 * u * g, 0.2 * u * g});
            e.rest = true;
            return e;
        };
        pair(p.pieces, trapezius(lsr, 1.0), trapezius(rsr, -1.0), 0.3 * u);
    }
    // the buttocks
    const double gx = 0.52 * d.hw + 0.02 * g;
    pair(p.pieces, ellipsoid(off(P0, H, gx, 0.1, 0.2 * dd), H, radii(0.34 * g * sx.glute, 0.42, 0.37 * dd * sx.glute)),
         ellipsoid(off(P0, H, -gx, 0.1, 0.2 * dd), H, radii(0.34 * g * sx.glute, 0.42, 0.37 * dd * sx.glute)), 0.22 * u);
    // (the pectoral's outer part, from the chest to the arm: the chest runs into the shoulder with no groove)
    {
        const auto tail = [&](const Vec3& shoulder) {
            Piece e = ellipsoid(Vec3{0.66 * shoulder[0], -2.42 * u, -0.16 * d_chest * u}, kIdentity, radii(0.3 * g, 0.27, 0.15 * g));
            e.rest = true;
            return e;
        };
        pair(p.pieces, tail(lsr), tail(rsr), 0.3 * u);
    }
    // a woman's breasts: each a drop — narrow where it leaves the chest under the collarbone, full below
    if (sx.chest == 2) {
        const double bx = 0.33 * sh * g + 0.04;
        pair(p.pieces, cone(off(W, C, bx - 0.03, -1.05, -0.17 * d_chest), 0.11 * u * g, off(W, C, bx, -0.66, -0.4 * d_chest), 0.265 * u * g),
             cone(off(W, C, -bx + 0.03, -1.05, -0.17 * d_chest), 0.11 * u * g, off(W, C, -bx, -0.66, -0.4 * d_chest), 0.265 * u * g), 0.3 * u);
    }
    // The low relief of the trunk, as swells of its surface (no edge anywhere), set in the body standing straight:
    const auto relief = [&p](Piece one) {
        one.rest = true;
        p.pieces.push_back(std::move(one));
    };
    const auto relief_pair = [&p](Piece one, Piece other) {
        one.rest = true;
        other.rest = true;
        one.paired = true;
        p.pieces.push_back(std::move(one));
        p.pieces.push_back(std::move(other));
    };
    // the shoulder blades
    {
        const double bx = 0.37 * sh * g;
        const Vec3 size = radii(0.3 * g, 0.36, 0.25);
        relief_pair(swell(rest(bx, -2.2, 0.47 * d_chest), kIdentity, size, 0.03 * u * g), swell(rest(-bx, -2.2, 0.47 * d_chest), kIdentity, size, 0.03 * u * g));
    }
    // the furrow of the spine, deepest at the small of the back, fading out toward the neck and toward the pelvis
    relief(swell(rest(0.0, -1.45, 0.46 * dd), kIdentity, radii(0.15, 1.2, 0.3), -0.022 * u));
    // the collarbones: a low ridge from the pit of the neck out toward each shoulder, fading at both ends
    {
        const double sx_ = 0.9 * (0.4 + 0.4 * sh) * g;  // (about where the shoulder's ball begins)
        const auto bone = [&](double side) {
            const Vec3 inner = rest(side * 0.04, -2.75, 0.1 * dd - 0.33 * d_chest - 0.02);
            const Vec3 outer = rest(side * sx_, -2.71, -0.1);
            return swell(mul(add(inner, outer), 0.5), frame_along(sub(outer, inner)), Vec3{0.52 * length(sub(outer, inner)), 0.12 * u, 0.22 * u}, 0.02 * u);
        };
        relief_pair(bone(1.0), bone(-1.0));
    }
    // the chest: the pectorals (a man's fuller): each a slab from beside the breastbone (the two leave a shallow
    // hollow down the middle) out into the front of the shoulder. Its lower edge is a straight soft step that rises
    // outward to the fold of the armpit, its inner one runs down beside the breastbone, the corner between them is
    // round; outward it goes on past the shoulder, and upward it thins as the chest leans back to the collarbone —
    // a slab of muscle from the breastbone into the shoulder, with no round outline and no upper edge
    if (sx.chest != 2) {
        const double full = sx.chest == 1 ? 1.0 : 0.4;
        const double wide = sh * g;
        const auto pectoral = [&](double side) {
            Mat3 m{};
            m[0] = Vec3{side, side * 0.34, 0.0};  // (columns: across; down and a little outward — square to the lower edge; forward)
            m[1] = Vec3{0.0, 0.94, 0.0};
            m[2] = Vec3{0.0, 0.0, 1.0};
            const Vec3 back = radii(0.66 * wide, 0.54, 0.6);
            return slab(rest(side * 0.7 * wide, -2.6, -0.47 * d_chest), m, radii(0.66 * wide, 0.54, 0.45), 0.045 * u * full, &back);
        };
        relief_pair(pectoral(1.0), pectoral(-1.0));
    }
    // The belly's relief. (The front of the trunk at a height, between its sections.)
    const auto front = [&](double y) {
        const double ys[4] = {-1.92, -1.3, -0.92, -0.3};
        const double zs[4] = {-0.015 * dd - 0.55 * d_chest, -0.035 * dd - 0.47 * d_chest, -0.435 * dd, -0.45 * dd};
        if (y <= ys[0]) return zs[0];
        for (int i = 0; i < 3; ++i) {
            if (y <= ys[i + 1]) return zs[i] + (zs[i + 1] - zs[i]) * (y - ys[i]) / (ys[i + 1] - ys[i]);
        }
        return zs[3];
    };
    const double gb = std::max(0.85, g);  // (across: the belly's forms keep to the grid on a thin body)
    // the lower edge of the ribs: the belly sunk under it — a slab taken away, whose upper edge (a soft step) is an
    // arch from the breastbone's end down and out toward each flank; downward it fades over the whole belly, too
    // slowly to show, and it is of the front only (gone before the flank: the body's side is as it was). Sunk, not
    // the ribs raised: the trunk is nowhere larger for it.
    {
        const double amount = (sx.chest == 1 ? 0.03 : (sx.chest == 2 ? 0.018 : 0.024)) * u;
        const Vec3 back = radii(1.3 * gb, 0.8, 0.62);
        relief(slab(rest(0.0, -1.0, front(-1.2) + 0.1), kIdentity, radii(1.3 * gb, 1.1, 0.35), -amount, &back, 0.5, 0.2 * u));
    }
    // the line down the middle, from the breastbone's end to the navel, fading at both ends
    relief(swell(rest(0.0, -1.3, front(-1.3)), kIdentity, radii(0.24, 0.6, 0.3), -0.012 * u));
    // the navel: a soft dimple (seven of the grid's cells across: a smaller one is drawn as a few flat faces)
    relief(swell(rest(0.0, -0.78, front(-0.78)), kIdentity, radii(0.22, 0.22, 0.25), -0.02 * u));
    // a hint of the belly's muscle: a long low form on each side of the middle line, from under the ribs to the navel,
    // and two lower pads along it (a man's; fainter on the body that is neither; a woman's belly stays smooth)
    if (sx.chest != 2) {
        const double amount = (sx.chest == 1 ? 1.0 : 0.55) * u;
        const double bx = 0.24 * gb;
        relief_pair(swell(rest(bx, -1.34, front(-1.34)), kIdentity, radii(0.25 * gb, 0.66, 0.3), 0.01 * amount),
                    swell(rest(-bx, -1.34, front(-1.34)), kIdentity, radii(0.25 * gb, 0.66, 0.3), 0.01 * amount));
        for (const double y : {-1.6, -1.08}) {
            const Vec3 size = radii(0.25 * gb, 0.27, 0.25);
            relief_pair(swell(rest(bx, y, front(y)), kIdentity, size, 0.013 * amount), swell(rest(-bx, y, front(y)), kIdentity, size, 0.013 * amount));
        }
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
    const Vec3 armpits[2] = {torso.armpit[0], torso.armpit[1]};
    f.parts.push_back(std::move(torso.part));  // 0 the torso as one smooth body
    const Vec3 neck_low = off(N, C, 0.0, 0.1, 0.05);
    const Vec3 neck_top = off(HB, Hd, 0.0, -0.05, 0.1);
    const Vec3 down = unit(sub(neck_low, neck_top));
    // The neck: a run of sections from its base (widest: it runs into the trapezius) up into the skull (the back of the
    // head runs into it high, as the neck's muscles hold the skull), narrowest two thirds up to the skull's base. The body's mesh draws it up to 0.2 u below its top and the head's own, finer mesh from there up,
    // with the jaw (below): each thins fast past that ring (by half of how far past), so the two cross steeply — a
    // clean ring, where two meshes of one surface laid on each other would show through each other in patches — and
    // the directions of the light there are the unthinned neck's on both (plain_normals), so nothing shows.
    const double neck_len = length(sub(neck_top, neck_low));
    const double nk = gs * sx.neck;
    Piece neck;
    {
        const Vec3 up = mul(down, -1.0);
        const Vec3 cx = la::column(C, 0);
        const Vec3 e0 = unit(sub(cx, mul(up, dot3(cx, up))));
        const Vec3 e2{e0[1] * up[2] - e0[2] * up[1], e0[2] * up[0] - e0[0] * up[2], e0[0] * up[1] - e0[1] * up[0]};
        Mat3 m{};
        for (std::size_t r = 0; r < 3; ++r) m[r] = Vec3{e0[r], up[r], e2[r]};
        neck = loft(neck_low, m,
                    {Station{0.0, 0.33 * u * nk, 0.3 * u * nk, 0.0}, Station{0.62 * neck_len, 0.218 * u * nk, 0.228 * u * nk, 0.012 * u},
                     Station{neck_len, 0.212 * u * nk, 0.218 * u * nk, 0.02 * u}, Station{neck_len + 0.2 * u, 0.21 * u * nk, 0.215 * u * nk, 0.0}},
                    0.3 * u, 0.3 * u);
    }
    const double ring = dot3(down, neck_top) + 0.2 * u;  // (where down · x is this)
    {  // 1 the neck
        Part p;
        p.pieces.push_back(neck);
        p.fade = 0.5;
        p.fade_n = mul(down, -1.0);
        p.fade_d = -(ring + 0.01 * u);
        f.parts.push_back(std::move(p));
    }
    const Joint neck_joint{kTorso, kNeck, off(N, C, 0.0, 0.1, 0.03), 0.3 * u, 0.6 * u, 0.5};
    f.joints.push_back(neck_joint);

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
        // (the shoulder's join, wider in the fold of the armpit under it: the arm's underside runs into the side of
        // the chest in a round hollow, not a corner — the shoulder itself, above and in front, keeps its width)
        f.joints.push_back(Joint{kTorso, upper, shoulder, 0.3 * u, 1.1 * u, 0.55, armpits[side], 0.25 * u, 0.6 * u, 0.25});
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
        const std::vector<Station> leg_run{st(0.1, 0.395 * leg, 0.41 * leg, -0.01 * leg),     st(0.85 * legs, 0.335 * leg, 0.365 * leg, -0.025),
                                           st(1.6 * legs, 0.262 * g, 0.275 * g, -0.012),      st(2.0 * legs, r_knee, 1.05 * r_knee, -0.03 * g),
                                           st(2.25 * legs, 0.228 * g, 0.24 * g, -0.005 * g),  st(2.6 * legs, 0.232 * g, 0.258 * g, 0.05 * g),
                                           st(3.3 * legs, 0.165 * g, 0.175 * g, 0.02 * g),    st(3.95 * legs, r_ankle, 1.08 * r_ankle, 0.01)};
        const std::vector<Station> leg_slopes = run_slopes(leg_run);
        {  // the thigh: widest just under the hip joint, and above it a dome up to the crest of the pelvis — the hip's
            // own side, and its front beside the belly
            Part p;
            p.k = 0.1 * u;
            const std::vector<Station> slopes = stations_of(leg_slopes, 0, 3, 0.0);
            p.pieces.push_back(loft(hip, thigh_r, stations_of(leg_run, 0, 3, 0.0), 0.5 * u * leg, r_knee * u, &slopes, 0.7));
            // The inner thigh. With the usual hips (and narrower) a long form beside the thigh's own, a little toward
            // the other leg. With wide hips — more room between the legs than that fills — it gives way (by `wide`,
            // 0..1) to a cone from just under the crotch, where it is fullest, down to above the knee, where it ends
            // inside the thigh: the inner outline is one straight taper from the crotch to the knee, with no swell
            // at mid-thigh. The two legs never come nearer than three of the grid's cells (a thinner slit between two
            // forms would be drawn ragged).
            const double inner = std::max(0.0, std::min(dm.hw - 0.3 * leg, 0.26 * leg));
            const double usual = 0.6 * inner + 0.02;
            const double wide = std::min(1.0, std::max(0.0, dm.hw - 0.3 * leg - 0.075 - usual) / 0.05);
            if (wide < 1.0) {
                p.pieces.push_back(
                    with_blend(ellipsoid(off(hip, thigh_r, med * usual * (1.0 - wide), 0.72 * legs, 0.01), thigh_r, radii(0.3 * leg, 0.82 * legs, 0.3 * leg)), 0.25 * u));
            }
            if (wide > 0.0) {
                const double top = 0.395 * leg + wide * std::min(std::max(0.0, dm.hw - 0.09 - 0.395 * leg), 0.15 * leg);  // (how far it reaches toward the other leg)
                p.pieces.push_back(with_blend(cone(off(hip, thigh_r, med * (top - 0.3 * leg), 0.3, 0.01), 0.3 * u * leg, off(hip, thigh_r, med * 0.09 * g, 1.7 * legs, 0.0), 0.15 * u * g),
                                              0.25 * u));
            }
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
        f.joints.push_back(Joint{kTorso, thigh, hip, 0.3 * u, 1.15 * u, 0.5});
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
    // (the neck's directions of the light above the ring: the unthinned neck's)
    plain_normals(out.mesh, f, {kNeck}, u, [&](const Vec3& x) { return dot3(down, x) < ring + 0.03 * u; });
    {  // the head and the neck's top, on a grid of u / kHeadCells: the face's relief needs it
        Field hf;
        hf.parts.push_back(head_part(Hc, Hd, u));
        Part stub;
        // (the neck's own run from its top down to 0.37 u below it, thinning under the ring. The head runs into it at
        // the nape, and the jaw into it by a narrower blend: an overhang with a soft fold under it)
        stub.pieces.push_back(neck);
        stub.cut = true;
        stub.cut_n = down;
        stub.cut_d = ring + 0.14 * u;
        stub.fade = 0.5;
        stub.fade_n = down;
        stub.fade_d = ring - 0.01 * u;
        hf.parts.push_back(std::move(stub));
        // (and the torso where the neck's base runs into it, thinned and cut the same way: the neck at the ring is
        // the body's own)
        Part under = f.parts[kTorso];
        under.pieces.resize(kTrunkAndTrapezius);  // (what the neck's base runs into: the trunk and the trapezius)
        under.cut = true;
        under.cut_n = down;
        under.cut_d = ring + 0.14 * u;
        under.fade = 0.5;
        under.fade_n = down;
        under.fade_d = ring - 0.01 * u;
        hf.parts.push_back(std::move(under));
        // (the head runs into the neck at the nape, and more widely under the jaw, whose shallow underside meets the
        // neck in a round fold — one joint with its second place under the jaw, a little above the neck's top so
        // that it is gone at the ring, where the neck is the body's own)
        hf.joints.push_back(Joint{0, 1, off(HB, Hd, 0.0, -0.05, 0.22), 0.24 * u, 0.5 * u, 0.0, off(HB, Hd, 0.0, -0.12, -0.08), 0.18 * u, 0.36 * u, 0.5});
        hf.joints.push_back(Joint{2, 1, neck_joint.at, neck_joint.k, neck_joint.radius, neck_joint.flat});
        Smooth hm = surface_nets::mesh(hf, u / kHeadCells, kHeadNormalSteps);
        // (from two of the head's cells above where the thinning begins: the head's wider slope reaches that far)
        plain_normals(hm, hf, {1, 2}, u, [&](const Vec3& x) { return dot3(down, x) > ring - 0.08 * u; });
        // (the outline of the neck changes from the body's to the head's at the ring)
        for (std::size_t i = 0; i < hm.v.size(); ++i) {
            if (hm.part[i] != 0 && dot3(down, hm.v[i]) > ring) hm.no_contour[i] = 1;
        }
        for (std::size_t i = 0; i < out.mesh.v.size(); ++i) {
            if (out.mesh.part[i] == kNeck && dot3(down, out.mesh.v[i]) < ring) out.mesh.no_contour[i] = 1;
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
    out.mesh = surface_nets::mesh(f, u / kHeadCells, kHeadNormalSteps);
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
