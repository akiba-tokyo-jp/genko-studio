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

double dot3(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
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
Vec3 into(const Mat3& m, const Vec3& v) {
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

// A stub of the round cone a (ra) → b (rb), kept from b to `keep` toward a and narrowed toward its cut end by `shrink`
// (at b it is the cone itself): where another grid's mesh of the same cone is a chord inside it, the stub's cut rim
// stays inside that mesh and no ring shows.
Piece stub_of(const Vec3& a, double ra, const Vec3& b, double rb, double keep, double shrink) {
    const Vec3 ab = sub(a, b);
    const double len = length(ab);
    const double t = len > 0.0 ? std::min(1.0, (keep * 1.25) / len) : 0.0;
    const Vec3 end = add(b, mul(ab, t));
    const double r_end = rb + (ra - rb) * t - shrink * 1.25;
    return cone(end, r_end, b, rb);
}

// --- the hand, in its own frame: a along the fingers, w toward the thumb, n out of the palm (a × w = n: a left hand)

struct Stub {
    bool on = false;
    Vec3 elbow{};  // (in the hand's frame)
    double r_elbow = 0;
    double r_wrist = 0;
    double cut = 0;  // kept within this far from the wrist toward the elbow (0: the whole round cone)
    double shrink = 0;  // (narrowed by this at the cut)
};

struct Finger {
    Vec3 base;  // the knuckle (MCP), in hand lengths
    double r;   // its radius there
    double len;
    double fan0;
    double fan1;  // the fan angle: fan0 + fan1 × spread
};

const Finger kFingers[4] = {
    {Vec3{0.54, 0.162, -0.005}, 0.047, 0.38, 0.035, 0.16},    // index
    {Vec3{0.555, 0.054, -0.005}, 0.048, 0.43, 0.008, 0.04},   // middle
    {Vec3{0.54, -0.054, -0.005}, 0.045, 0.40, -0.03, -0.10},  // ring
    {Vec3{0.505, -0.158, -0.005}, 0.040, 0.31, -0.075, -0.20},  // little
};

// Field of a hand `length` long. Parts: 0 the palm, 1 the thumb, 2..5 the fingers, 6 the stub of the forearm.
Field hand_field(double length, const HandShape& shape, const Stub& stub) {
    const double L = length;
    const auto at = [L](double a, double w, double n) { return Vec3{a * L, w * L, n * L}; };
    Field f;
    Part palm;
    palm.k = 0.05 * L;
    palm.pieces.push_back(box(at(0.37, 0.0, 0.0), kIdentity, at(0.18, 0.21, 0.066), 0.066 * L));
    palm.pieces.push_back(ellipsoid(at(0.19, 0.0, -0.004), kIdentity, at(0.18, 0.17, 0.08)));  // (the heel, narrowing to the wrist)
    palm.pieces.push_back(ellipsoid(at(0.2, 0.135, 0.045), kIdentity, at(0.16, 0.085, 0.075)));   // the ball of the thumb
    palm.pieces.push_back(ellipsoid(at(0.22, -0.15, 0.03), kIdentity, at(0.17, 0.065, 0.06)));    // and of the little finger
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
        f.joints.push_back(Joint{0, 1, at(0.22, 0.17, 0.03), 0.1 * L, 0.32 * L});  // (the web between thumb and index)
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
        f.joints.push_back(Joint{0, i + 2, at(g.base[0], g.base[1], g.base[2]), 0.025 * L, 0.09 * L});
    }
    Part wrist;
    if (stub.on) {
        wrist.pieces.push_back(stub_of(stub.elbow, stub.r_elbow, Vec3{0.0, 0.0, 0.0}, stub.r_wrist, stub.cut, stub.shrink));
        if (stub.cut > 0.0) {
            wrist.cut = true;
            wrist.cut_n = unit(stub.elbow);
            wrist.cut_d = stub.cut;
        }
    } else {  // (a hand on its own: a short rounded wrist)
        wrist.pieces.push_back(cone(at(-0.2, 0.0, 0.0), 0.16 * L, Vec3{0.0, 0.0, 0.0}, 0.15 * L));
    }
    f.parts.push_back(std::move(wrist));
    f.joints.push_back(Joint{0, 6, at(0.02, 0.0, 0.0), 0.06 * L, 0.22 * L});
    return f;
}

// --- the head: the skull, the face below and in front of it, the chin; no features --------------------------------

Part head_part(const Vec3& centre, const Mat3& turn, double u) {
    const auto at = [&](double x, double y, double z) { return add(centre, out_of(turn, Vec3{x * u, y * u, z * u})); };
    Part head;
    head.k = 0.12 * u;
    head.pieces.push_back(ellipsoid(at(0.0, -0.09, 0.05), turn, Vec3{0.4 * u, 0.45 * u, 0.5 * u}));
    head.pieces.push_back(ellipsoid(at(0.0, 0.17, -0.1), turn, Vec3{0.29 * u, 0.37 * u, 0.37 * u}));
    head.pieces.push_back(ellipsoid(at(0.0, 0.45, -0.22), turn, Vec3{0.15 * u, 0.12 * u, 0.13 * u}));
    // the relief of a neutral face: brow, nose, lips, the ears; the eyes' hollows (pairs joined first: see Part::at)
    head.detail_k = 0.08 * u;
    head.details.push_back(cone(at(0.0, 0.0, -0.45), 0.033 * u, at(0.0, 0.165, -0.54), 0.042 * u));
    head.details.push_back(ellipsoid(at(0.0, 0.175, -0.5), turn, Vec3{0.068 * u, 0.04 * u, 0.05 * u}));
    head.details.push_back(ellipsoid(at(0.0, 0.265, -0.425), turn, Vec3{0.075 * u, 0.03 * u, 0.05 * u}));
    head.details.push_back(ellipsoid(at(0.0, 0.318, -0.415), turn, Vec3{0.062 * u, 0.03 * u, 0.045 * u}));
    Piece ear = ellipsoid(at(0.4, 0.06, 0.07), turn, Vec3{0.045 * u, 0.12 * u, 0.075 * u});
    ear.paired = true;
    head.details.push_back(ear);
    head.details.push_back(ellipsoid(at(-0.4, 0.06, 0.07), turn, Vec3{0.045 * u, 0.12 * u, 0.075 * u}));
    head.hollow_k = 0.06 * u;
    Piece eye = ellipsoid(at(0.13, 0.04, -0.5), turn, Vec3{0.07 * u, 0.055 * u, 0.055 * u});
    eye.paired = true;
    head.hollows.push_back(eye);
    head.hollows.push_back(ellipsoid(at(-0.13, 0.04, -0.5), turn, Vec3{0.07 * u, 0.055 * u, 0.055 * u}));
    return head;
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
    double waist = 0.95;
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
    if (name == "male") return Sex{1.12, 1.05, 1.0, 0.86, 0.92, 1.08, 1.0, 1.0, 1.06, 1};
    if (name == "female") return Sex{0.9, 0.95, 0.82, 1.02, 1.12, 0.88, 1.08, 0.92, 0.9, 2};
    return Sex{};
}

// (figure_skeleton has put every number in: the op's own, the sex's, or the usual)
double body_number(const Json& body, const char* key, double lo, double hi) { return py_max(lo, py_min(hi, to_float(body.at(key)))); }

// Append b to a (b's points moved into the figure by `place`, its normals by `turn`; triangles flipped when `flip`).
template <typename Place, typename Turn>
void append(Smooth& a, const Smooth& b, Place place, Turn turn, bool flip) {
    const auto base = static_cast<std::int32_t>(a.v.size());
    for (std::size_t i = 0; i < b.v.size(); ++i) {
        a.v.push_back(place(b.v[i]));
        a.n.push_back(turn(b.n[i]));
        a.no_contour.push_back(b.no_contour[i]);
        a.part.push_back(b.part[i]);
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
        // (the ball's distance, squeezed: never more than the true distance)
        const Vec3 q = into(axes, sub(x, a));
        const double qx = q[0] / size[0];
        const double qy = q[1] / size[1];
        const double qz = q[2] / size[2];
        const double k0 = std::sqrt(qx * qx + qy * qy + qz * qz);
        return (k0 - 1.0) * std::min(size[0], std::min(size[1], size[2]));
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
    }
    return std::numeric_limits<double>::infinity();
}

double smin(double a, double b, double k) {
    const double m = std::min(a, b);
    if (!(k > 0.0)) return m;
    const double h = std::max(k - std::fabs(a - b), 0.0) / k;
    return m - h * h * k * 0.25;
}

double Part::at(const Vec3& x) const {
    double v = std::numeric_limits<double>::infinity();
    bool first = true;
    // (a pair is joined first: smin is the same either way round, so a mirrored body is the mirror's to the bit)
    const auto each = [&x](const std::vector<Piece>& list, double kk, auto&& take) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            double d = list[i].at(x);
            if (list[i].paired && i + 1 < list.size()) {
                d = smin(d, list[i + 1].at(x), kk);
                ++i;
            }
            take(d);
        }
    };
    each(pieces, k, [&](double d) {
        v = first ? d : smin(v, d, k);
        first = false;
    });
    each(details, detail_k, [&](double d) { v = smin(v, d, detail_k); });
    each(hollows, hollow_k, [&](double d) { v = -smin(-v, d, hollow_k); });
    if (cut) {
        const double g = dot3(cut_n, x) - cut_d;
        v = cut_k > 0.0 ? -smin(-v, -g, cut_k) : std::max(v, g);
    }
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
        const double w = (1.0 - t2) * (1.0 - t2);
        f = std::min(f, smin(d[j.p], d[j.c], j.k * w));
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
        std::vector<Piece> all = part.pieces;
        all.insert(all.end(), part.details.begin(), part.details.end());
        for (const Piece& p : all) {
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], p.centre[a] - p.reach - k);
                hi[a] = std::max(hi[a], p.centre[a] + p.reach + k);
            }
        }
    }
}

// --- the meshes ----------------------------------------------------------------------------------------------------

FigureMesh figure(const mesh3d::Skeleton& sk, const HandShape& left, const HandShape& right) {
    const double u = sk.unit;
    const Json& body = sk.body;
    const double shoulders = body_number(body, "shoulders", 0.6, 1.5);
    const double hips = body_number(body, "hips", 0.6, 1.6);
    const double build = body_number(body, "build", 0.5, 1.8);
    const double legs = body_number(body, "legs", 0.6, 1.5);
    const Sex sx = sex_of(body);
    const double root = std::sqrt(build);
    const double g = root * std::sqrt(root);  // girth: build ^ 0.75
    const double dd = build;                  // depth of the torso
    const double gs = std::sqrt(g);           // (the feet and the neck follow the build a little)

    const Vec3& P0 = sk.point("pelvis");
    const Vec3& W = sk.point("waist");
    const Vec3& N = sk.point("neck");
    const Vec3& HB = sk.point("head_base");
    const Vec3& Hc = sk.point("head");
    const Mat3& H = sk.turn("hip");
    const Mat3& S = sk.turn("spine");
    const Mat3& C = sk.turn("chest");
    const Mat3& Hd = sk.turn("head");
    const auto off = [u](const Vec3& o, const Mat3& m, double x, double y, double z) { return add(o, out_of(m, Vec3{x * u, y * u, z * u})); };
    const auto radii = [u](double x, double y, double z) { return Vec3{x * u, y * u, z * u}; };

    Field f;
    enum { kPelvis, kBelly, kChest, kNeck };
    // (sizes in u, the skeleton's unit: a head is 1.1 u tall; an adult's widths, girths and depths as such)
    const auto pair = [](Part& p, Piece one, Piece other) {  // (a left and right pair: joined first, the same either way)
        one.paired = true;
        p.pieces.push_back(one);
        p.pieces.push_back(other);
    };
    {  // 0 the pelvis and the buttocks
        Part p;
        p.k = 0.16 * u;
        p.pieces.push_back(ellipsoid(off(P0, H, 0.0, 0.02, 0.02), H, radii((0.42 + 0.3 * hips) * g * sx.pelvis, 0.5, 0.44 * dd)));
        p.pieces.push_back(ellipsoid(off(P0, H, 0.0, 0.4, 0.0), H, radii(0.17 * g * sx.pelvis, 0.26, 0.27 * dd)));  // (between the legs)
        Piece glute[2];
        for (int i = 0; i < 2; ++i) {
            const double s = i == 0 ? 1.0 : -1.0;
            glute[i] = ellipsoid(off(P0, H, s * 0.26 * hips, 0.22, 0.2 * dd), H, radii(0.33 * g * sx.glute, 0.36, 0.28 * dd * sx.glute));
        }
        pair(p, glute[0], glute[1]);
        f.parts.push_back(std::move(p));
    }
    {  // 1 the belly
        Part p;
        p.pieces.push_back(ellipsoid(off(P0, S, 0.0, -0.8, 0.0), S,
                                     radii(0.62 * g * sx.waist * (0.55 + 0.225 * (hips + shoulders)), 0.8, 0.43 * dd)));
        f.parts.push_back(std::move(p));
    }
    {  // 2 the chest: the ribs, the shoulders' line, the trapezius, and the pectorals or the breasts
        Part p;
        p.k = 0.16 * u;
        p.pieces.push_back(ellipsoid(off(W, C, 0.0, -0.8, 0.04), C, radii((0.36 + 0.38 * shoulders) * g * sx.rib_w, 0.82, 0.48 * dd * sx.rib_d)));
        const double sw = 0.95 * clamp(shoulders, 0.6, 1.5);
        p.pieces.push_back(ellipsoid(off(N, C, 0.0, 0.3, 0.05), C, radii(sw * 0.86, 0.25 * g, 0.3 * dd * sx.rib_d)));
        pair(p, cone(off(N, C, 0.0, 0.05, 0.1), 0.2 * u * g, off(sk.point("l_shoulder"), C, 0.0, -0.02, 0.05), 0.15 * u * g),
             cone(off(N, C, 0.0, 0.05, 0.1), 0.2 * u * g, off(sk.point("r_shoulder"), C, 0.0, -0.02, 0.05), 0.15 * u * g));
        if (sx.chest == 1) {
            pair(p, ellipsoid(off(W, C, 0.3 * shoulders, -1.06, -0.33 * dd), C, radii(0.3 * g, 0.22, 0.15 * dd)),
                 ellipsoid(off(W, C, -0.3 * shoulders, -1.06, -0.33 * dd), C, radii(0.3 * g, 0.22, 0.15 * dd)));
        } else if (sx.chest == 2) {
            pair(p, ellipsoid(off(W, C, 0.27 * shoulders, -0.88, -0.36 * dd), C, radii(0.21 * g, 0.2 * g, 0.19 * g)),
                 ellipsoid(off(W, C, -0.27 * shoulders, -0.88, -0.36 * dd), C, radii(0.21 * g, 0.2 * g, 0.19 * g)));
        }
        f.parts.push_back(std::move(p));
    }
    const Vec3 neck_low = off(N, C, 0.0, 0.1, 0.04);
    const Vec3 neck_top = off(HB, Hd, 0.0, -0.05, 0.06);
    const double r_neck_low = 0.25 * u * gs * sx.neck;
    const double r_neck_top = 0.21 * u * gs * sx.neck;
    {  // 3 the neck (the head is meshed on its own, finer grid, with the neck's top: below)
        Part p;
        p.pieces.push_back(cone(neck_low, r_neck_low, neck_top, r_neck_top));
        f.parts.push_back(std::move(p));
    }
    f.joints.push_back(Joint{kPelvis, kBelly, off(P0, S, 0.0, -0.45, 0.0), 0.45 * u, 1.1 * u});
    f.joints.push_back(Joint{kBelly, kChest, off(W, C, 0.0, -0.25, 0.0), 0.45 * u, 1.1 * u});
    f.joints.push_back(Joint{kChest, kNeck, off(N, C, 0.0, 0.05, 0.03), 0.16 * u, 0.45 * u});

    struct Arm {
        int fore;
        Vec3 elbow;
        Vec3 wrist;
        double r_elbow;
        double r_wrist;
    };
    Arm arms[2];
    for (int side = 0; side < 2; ++side) {
        const std::string s = side == 0 ? "l" : "r";
        const double sg = side == 0 ? 1.0 : -1.0;
        const double limb = g * sx.limb;
        const Vec3& shoulder = sk.point(s + "_shoulder");
        const Vec3& elbow = sk.point(s + "_elbow");
        const Vec3& wrist = sk.point(s + "_wrist");
        const Mat3& arm_r = sk.turn(s + "_arm");
        const Mat3& fore_r = sk.turn(s + "_elbow");
        const double r_shoulder = 0.2 * u * limb;
        const double r_elbow = 0.145 * u * limb;
        const double r_wrist = 0.12 * u * limb;
        {  // the upper arm and its deltoid
            Part p;
            p.k = 0.1 * u;
            p.pieces.push_back(cone(shoulder, r_shoulder, elbow, r_elbow));
            p.pieces.push_back(ellipsoid(off(shoulder, arm_r, 0.0, 0.16, 0.0), arm_r, radii(0.25 * limb, 0.38, 0.24 * limb)));
            f.parts.push_back(std::move(p));
        }
        const int upper = static_cast<int>(f.parts.size()) - 1;
        {  // the forearm, fuller below the elbow
            Part p;
            p.k = 0.1 * u;
            p.pieces.push_back(cone(elbow, r_elbow, wrist, r_wrist));
            p.pieces.push_back(ellipsoid(off(elbow, fore_r, 0.0, 0.36, 0.0), fore_r, radii(0.165 * limb, 0.42, 0.15 * limb)));
            f.parts.push_back(std::move(p));
        }
        const int fore = upper + 1;
        f.joints.push_back(Joint{kChest, upper, shoulder, 0.16 * u, 0.5 * u});
        f.joints.push_back(Joint{upper, fore, elbow, 0.05 * u, 0.22 * u});
        arms[side] = Arm{fore, elbow, wrist, r_elbow, r_wrist};

        const Vec3& hip = sk.point(s + "_hip");
        const Vec3& knee = sk.point(s + "_knee");
        const Vec3& ankle = sk.point(s + "_ankle");
        const Mat3& thigh_r = sk.turn(s + "_leg");
        const Mat3& shin_r = sk.turn(s + "_knee");
        const Mat3& foot_r = sk.turn(s + "_ankle");
        const double r_knee = 0.2 * u * g;
        const double r_ankle = 0.12 * u * g;
        {  // the thigh
            Part p;
            p.k = 0.14 * u;
            p.pieces.push_back(cone(hip, 0.39 * u * g * sx.thigh, knee, r_knee));
            p.pieces.push_back(ellipsoid(off(hip, thigh_r, 0.0, 0.8 * legs, -0.05), thigh_r, radii(0.31 * g * sx.thigh, 0.8 * legs, 0.31 * g * sx.thigh)));
            f.parts.push_back(std::move(p));
        }
        const int thigh = static_cast<int>(f.parts.size()) - 1;
        {  // the shin and the calf
            Part p;
            p.k = 0.12 * u;
            p.pieces.push_back(cone(knee, r_knee, ankle, r_ankle));
            p.pieces.push_back(ellipsoid(off(knee, shin_r, 0.0, 0.62 * legs, 0.06), shin_r, radii(0.205 * g, 0.62 * legs, 0.21 * g)));
            f.parts.push_back(std::move(p));
        }
        const int shin = thigh + 1;
        {  // the foot: heel, instep, the ball, the toes (the big toe's side longer); the sole flat
            Part p;
            p.k = 0.12 * u;
            const double med = -sg;  // (toward the other foot)
            p.pieces.push_back(cone(off(ankle, foot_r, 0.0, 0.0, 0.0), r_ankle, off(ankle, foot_r, 0.0, 0.17, 0.1), 0.135 * u * gs));
            p.pieces.push_back(cone(off(ankle, foot_r, 0.0, 0.17, 0.1), 0.135 * u * gs, off(ankle, foot_r, med * 0.03, 0.2, -0.56), 0.11 * u * gs));
            p.pieces.push_back(ellipsoid(off(ankle, foot_r, med * 0.02, 0.2, -0.5), foot_r, radii(0.21 * gs, 0.11, 0.3)));
            p.pieces.push_back(ellipsoid(off(ankle, foot_r, med * 0.045, 0.245, -0.8), foot_r, radii(0.19 * gs, 0.07, 0.15)));
            p.cut = true;
            p.cut_n = la::column(foot_r, 1);
            p.cut_d = dot3(p.cut_n, ankle) + 0.30 * u;
            p.cut_k = 0.04 * u;
            f.parts.push_back(std::move(p));
        }
        const int foot = shin + 1;
        f.joints.push_back(Joint{kPelvis, thigh, hip, 0.3 * u, 0.8 * u});
        f.joints.push_back(Joint{thigh, shin, knee, 0.05 * u, 0.25 * u});
        f.joints.push_back(Joint{shin, foot, ankle, 0.08 * u, 0.3 * u});
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
        stub.pieces.push_back(stub_of(neck_low, r_neck_low, neck_top, r_neck_top, 0.3 * u, 0.01 * u));
        const Vec3 down = unit(sub(neck_low, neck_top));
        stub.cut = true;  // (kept down to 0.3 u below the neck's top: inside the body's own neck below that)
        stub.cut_n = down;
        stub.cut_d = dot3(down, neck_top) + 0.3 * u;
        hf.parts.push_back(std::move(stub));
        hf.joints.push_back(Joint{0, 1, HB, 0.12 * u, 0.4 * u});
        Smooth hm = surface_nets::mesh(hf, u / kHeadCells);
        // (the outline of the neck changes from the body's to the head's 0.12 u below the neck's top)
        for (std::size_t i = 0; i < hm.v.size(); ++i) {
            if (hm.part[i] == 1 && dot3(sub(hm.v[i], neck_top), down) > 0.12 * u) hm.no_contour[i] = 1;
        }
        for (std::size_t i = 0; i < out.mesh.v.size(); ++i) {
            if (out.mesh.part[i] == kNeck && dot3(sub(out.mesh.v[i], neck_top), down) < 0.12 * u) out.mesh.no_contour[i] = 1;
        }
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
        const Vec3 lc = into(T, sub(arm.elbow, arm.wrist));
        Stub stub;
        stub.on = true;
        stub.elbow = Vec3{lc[1], -lc[2], -sg * lc[0]};
        stub.r_elbow = arm.r_elbow;
        stub.r_wrist = arm.r_wrist;
        stub.cut = 0.15 * u;
        stub.shrink = 0.01 * u;
        const double hand_len = 0.8 * u * sx.hand;
        const Field hf = hand_field(hand_len, side == 0 ? left : right, stub);
        Smooth hm = surface_nets::mesh(hf, hand_len / kHandCells);
        const Vec3 e = unit(stub.elbow);
        for (std::size_t i = 0; i < hm.v.size(); ++i) {
            if (hm.part[i] == 6 && dot3(hm.v[i], e) > 0.06 * u) hm.no_contour[i] = 1;
        }
        const Vec3& wrist = arm.wrist;
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
