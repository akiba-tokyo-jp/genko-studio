// Python's genko/mesh3d.py (and the figure geometry of genko/threeops.py), expression by expression: every product,
// sum and conversion in the order Python evaluates it, numpy's matmul through core/linalg3, the math module through
// core/pymath (libm called as CPython calls it).

#include "core/mesh3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/persp3d.hpp"
#include "core/prim3d.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyvalue.hpp"

namespace genko::core::mesh3d {

using namespace genko::core::la;  // (the element-wise operators on Vec3)

const std::array<std::string_view, 17> kFigureJoints{"hip",     "spine",   "chest",   "neck",    "head",   "l_arm",
                                                     "r_arm",   "l_elbow", "r_elbow", "l_wrist", "r_wrist", "l_leg",
                                                     "r_leg",   "l_knee",  "r_knee",  "l_ankle", "r_ankle"};
const std::array<std::string_view, 6> kHandPoses{"open", "relaxed", "fist", "point", "peace", "grip"};
const std::array<std::string_view, 5> kFingers{"thumb", "index", "middle", "ring", "little"};

namespace {

using la::matmul;
using la::matvec;

double py_max(double a, double b) { return b > a ? b : a; }
double py_min(double a, double b) { return b < a ? b : a; }

const Json& json_of(const char* text) {
    // (a table written as Python writes its literals: 1.0 stays a float, 0 an int)
    static std::mutex mutex;
    static std::map<const char*, Json> tables;
    std::lock_guard lock(mutex);
    auto it = tables.find(text);
    if (it == tables.end()) it = tables.emplace(text, parse_python_json(text)).first;
    return it->second;
}

const char* const kPresets = R"({
 "stand": {},
 "walk": {"l_leg": {"x": 0.45}, "l_knee": {"x": -0.15}, "r_leg": {"x": -0.35}, "r_knee": {"x": -0.45},
          "l_arm": {"x": -0.35, "z": 0.1}, "l_elbow": {"x": 0.25}, "r_arm": {"x": 0.35, "z": -0.1}, "r_elbow": {"x": 0.4}},
 "run": {"spine": {"x": 0.3}, "l_leg": {"x": 1.0}, "l_knee": {"x": -1.3}, "r_leg": {"x": -0.6}, "r_knee": {"x": -1.1},
         "l_arm": {"x": -0.8, "z": 0.15}, "l_elbow": {"x": 1.3}, "r_arm": {"x": 0.9, "z": -0.15}, "r_elbow": {"x": 1.4},
         "hands": {"l": "fist", "r": "fist"}},
 "sit": {"l_leg": {"x": 1.5}, "l_knee": {"x": -1.5}, "r_leg": {"x": 1.45}, "r_knee": {"x": -1.45},
         "l_arm": {"x": 0.3, "z": 0.1}, "l_elbow": {"x": 1.0}, "r_arm": {"x": 0.35, "z": -0.1}, "r_elbow": {"x": 1.0}},
 "point": {"r_arm": {"z": -1.5, "x": 0.2}, "r_elbow": {"z": -0.05}, "head": {"y": -0.3}, "hands": {"r": "point"}},
 "arms_up": {"l_arm": {"z": 2.8}, "r_arm": {"z": -2.8}, "l_elbow": {"z": 0.2}, "r_elbow": {"z": -0.2},
             "hands": {"l": "open", "r": "open"}},
 "think": {"r_arm": {"x": 0.6, "z": -0.2}, "r_elbow": {"x": 2.3}, "head": {"z": 0.15, "x": -0.1}, "l_arm": {"x": 0.3, "z": 0.3},
           "l_elbow": {"x": 1.4, "z": -0.8}, "hands": {"r": "relaxed"}},
 "kneel": {"l_leg": {"x": 1.4}, "l_knee": {"x": -1.4}, "r_leg": {"x": 0.1}, "r_knee": {"x": -1.55}, "r_ankle": {"x": 0.6}},
 "peace": {"r_arm": {"z": -0.4, "x": 0.9}, "r_elbow": {"x": 1.6, "z": 0.4}, "hands": {"r": "peace"}, "head": {"z": -0.1}}
})";

const char* const kSexBody = R"({"male": {"shoulders": 1.08, "hips": 0.95, "build": 1.05},
 "female": {"heads": 7.2, "shoulders": 0.86, "hips": 1.18, "build": 0.88}})";

// _CURLS: thumb, index, middle, ring, little
const std::pair<std::string_view, std::array<double, 5>> kCurls[] = {
    {"open", {0.0, 0.0, 0.0, 0.0, 0.0}},     {"relaxed", {0.2, 0.25, 0.3, 0.35, 0.4}},
    {"fist", {0.7, 1.0, 1.0, 1.0, 1.0}},     {"point", {0.6, 0.0, 1.0, 1.0, 1.0}},
    {"peace", {0.7, 0.0, 0.0, 1.0, 1.0}},    {"grip", {0.5, 0.6, 0.6, 0.6, 0.6}},
};

std::array<double, 5> curls_of(std::string_view name) {
    for (const auto& [key, curls] : kCurls) {
        if (key == name) return curls;
    }
    return kCurls[1].second;  // relaxed
}

std::string str_or(const Json* value, std::string_view fallback) {
    return value != nullptr && py_truthy(*value) ? py_str(*value) : std::string(fallback);
}

double clamp01(double c) { return py_max(0.0, py_min(1.0, c)); }

// A vertex index as numpy takes it (negative from the end); IndexError beyond.
std::size_t vertex(std::int64_t i, std::size_t n) {
    const auto size = static_cast<std::int64_t>(n);
    const std::int64_t at = i < 0 ? i + size : i;
    if (at < 0 || at >= size) {
        pyv::raise_index("index " + std::to_string(i) + " is out of bounds for axis 0 with size " + std::to_string(n));
    }
    return static_cast<std::size_t>(at);
}

// --- mesh pieces -------------------------------------------------------------------------------------------------

struct Builder {
    Mesh m;

    void add(const std::vector<Vec3>& verts, const std::vector<std::vector<std::int64_t>>& faces, int part, bool smooth) {
        const auto base = static_cast<std::int64_t>(m.v.size());
        m.v.insert(m.v.end(), verts.begin(), verts.end());
        for (const auto& face : faces) {
            std::vector<std::int64_t> f;
            f.reserve(face.size());
            for (const std::int64_t i : face) f.push_back(base + i);
            m.f.push_back(std::move(f));
            m.parts.push_back(part + (smooth ? kSmooth : 0));
        }
    }

    void ellipsoid(const Vec3& centre, const Vec3& radii, const Mat3* rot, int nu = 14, int nv = 9, int part = 0) {
        std::vector<Vec3> verts;
        std::vector<std::vector<std::int64_t>> faces;
        for (int j = 0; j <= nv; ++j) {
            const double phi = kPi * j / nv;
            for (int i = 0; i < nu; ++i) {
                const double th = 2 * kPi * i / nu;
                Vec3 p{radii[0] * py_sin(phi) * py_cos(th), -radii[1] * py_cos(phi), radii[2] * py_sin(phi) * py_sin(th)};
                if (rot != nullptr) p = matvec(*rot, p);
                verts.push_back(centre + p);
            }
        }
        for (int j = 0; j < nv; ++j) {
            for (int i = 0; i < nu; ++i) {
                const std::int64_t a = j * nu + i;
                const std::int64_t b = j * nu + (i + 1) % nu;
                faces.push_back({a, b, b + nu, a + nu});
            }
        }
        add(verts, faces, part, true);
    }

    // A limb from a to b: a tube with rounded ends.
    void capsule(const Vec3& a, const Vec3& b, double r, int n = 10, int part = 0) {
        const Vec3 axis = b - a;
        const double length = la::norm(axis);
        if (length < 1e-6) {
            ellipsoid(a, Vec3{r, r, r}, nullptr, 14, 9, part);
            return;
        }
        const Vec3 w = axis / length;
        Vec3 u = la::cross(w, Vec3{0.0, 0.0, 1.0});
        if (la::norm(u) < 1e-6) u = la::cross(w, Vec3{1.0, 0.0, 0.0});
        u = u / la::norm(u);
        const Vec3 v = la::cross(w, u);
        std::vector<std::pair<Vec3, double>> rings;
        const int caps = 3;
        for (int k = 0; k <= caps; ++k) {  // the start cap
            const double t = kPi / 2 * (1 - static_cast<double>(k) / caps);
            rings.emplace_back(a - w * r * py_sin(t), r * py_cos(t));
        }
        for (int k = 0; k <= caps; ++k) {  // the end cap
            const double t = kPi / 2 * k / caps;
            rings.emplace_back(b + w * r * py_sin(t), r * py_cos(t));
        }
        std::vector<Vec3> verts;
        std::vector<std::vector<std::int64_t>> faces;
        for (const auto& [centre, rad] : rings) {
            for (int i = 0; i < n; ++i) {
                const double th = 2 * kPi * i / n;
                verts.push_back(centre + (u * py_cos(th) + v * py_sin(th)) * rad);
            }
        }
        for (std::size_t j = 0; j + 1 < rings.size(); ++j) {
            for (int i = 0; i < n; ++i) {
                const std::int64_t p = static_cast<std::int64_t>(j) * n + i;
                const std::int64_t q = static_cast<std::int64_t>(j) * n + (i + 1) % n;
                faces.push_back({p, q, q + n, p + n});
            }
        }
        add(verts, faces, part, true);
    }

    void box(const Vec3& centre, const Vec3& size, const Mat3* rot, int part = 0) {
        const double w = size[0];
        const double h = size[1];
        const double d = size[2];
        std::vector<Vec3> corners;
        for (const int sx : {-1, 1}) {
            for (const int sy : {-1, 1}) {
                for (const int sz : {-1, 1}) {
                    const Vec3 p{sx * w / 2, sy * h / 2, sz * d / 2};
                    corners.push_back(centre + (rot != nullptr ? matvec(*rot, p) : p));
                }
            }
        }
        add(corners, {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}}, part, false);
    }
};

// A hand at the wrist: the palm and five fingers of two bones, curled by the pose. `frame` is the forearm's turn
// (its y points along the hand).
void hand(Builder& b, const Vec3& wrist, const Mat3& frame, double length, const Json& pose, int side, int part) {
    const std::array<double, 5> curls = hand_curls(pose);
    const double palm_len = length * 0.5;
    const double palm_w = length * 0.45;
    const double thick = length * 0.14;
    const Vec3 down = la::column(frame, 1);
    const Vec3 across = la::column(frame, 0);
    const Vec3 palm_c = wrist + down * palm_len / 2;
    b.box(palm_c, Vec3{palm_w, palm_len, thick}, &frame, part);
    for (int k = 0; k < 5; ++k) {
        const double curl = curls[static_cast<std::size_t>(k)];
        Vec3 base{};
        Mat3 direction{};
        if (k == 0) {  // the thumb: from the palm's side, toward the front
            base = wrist + down * palm_len * 0.3 + across * (palm_w / 2) * side;
            direction = matmul(rz(-0.7 * side), rx(0.4));
        } else {
            base = wrist + down * palm_len + across * (palm_w * (0.36 - 0.24 * (k - 1))) * side;
            direction = la::identity3();
        }
        const double seg = length * (k == 1 || k == 2 || k == 3 ? 0.22 : 0.18);
        Mat3 turn = matmul(frame, direction);
        Vec3 p = base;
        for (int bone = 0; bone < 2; ++bone) {
            const Mat3 bend = rx(-curl * 1.3);  // curling toward the palm (its front)
            turn = matmul(turn, bend);
            const Vec3 q = p + la::column(turn, 1) * seg;
            b.capsule(p, q, thick * 0.45, 6, part);
            p = q;
        }
    }
}

void head_mesh(Builder& b, const Vec3& centre, const Mat3& turn, double u, int part) {
    b.ellipsoid(centre, Vec3{0.42 * u, 0.55 * u, 0.5 * u}, &turn, 16, 11, part);
    // the jaw and chin, a little forward and down
    b.ellipsoid(centre + matvec(turn, Vec3{0.0, 0.28 * u, -0.12 * u}), Vec3{0.3 * u, 0.28 * u, 0.3 * u}, &turn, 12, 8, part);
}

// The guide lines on a face: the centre line (front, top to chin), the eye line, the nose.
std::vector<std::vector<Vec3>> head_marks(const Vec3& centre, const Mat3& turn, double u) {
    const double rx_ = 0.42 * u;
    const double ry_ = 0.55 * u;
    const double rz_ = 0.5 * u;
    const auto on = [&](double theta, double phi, double lift) {
        const Vec3 p = Vec3{rx_ * py_sin(phi) * py_sin(theta), -ry_ * py_cos(phi), -rz_ * py_sin(phi) * py_cos(theta)} * lift;
        return centre + matvec(turn, p);
    };
    std::vector<Vec3> centre_line;
    for (int k = 3; k < 21; ++k) centre_line.push_back(on(0.0, kPi * k / 20, 1.01));
    std::vector<Vec3> eye_line;
    for (int k = 0; k < 21; ++k) eye_line.push_back(on(kPi * (k / 20.0 - 0.5) * 0.9, kPi * 0.55, 1.01));
    std::vector<Vec3> nose{on(0, kPi * 0.62, 1.01), on(0, kPi * 0.66, 1.12), on(0, kPi * 0.7, 1.02)};
    return {centre_line, eye_line, nose};
}

// --- figures ------------------------------------------------------------------------------------------------------

std::shared_ptr<const Model> figure(const Json& prim) {
    const Skeleton sk = figure_skeleton(prim);
    const double u = sk.unit;
    const double build = py_max(0.5, py_min(1.8, pyv::to_float(sk.body.at("build"))));
    Builder b;
    const double t = u * 0.2 * build;  // limb radius
    const Mat3& chest_turn = sk.turn("chest");
    // the torso: chest, belly and pelvis as eggs; the neck; the head
    b.ellipsoid((sk.point("waist") + sk.point("neck")) / 2 + matvec(chest_turn, Vec3{0.0, 0.1 * u, 0.0}),
                Vec3{0.95 * u * py_max(0.6, pyv::to_float(sk.body.at("shoulders"))) * 0.85 * py_pow(build, 0.3), 0.85 * u,
                     0.5 * u * build},
                &chest_turn, 14, 9, 1);
    b.ellipsoid((sk.point("pelvis") + sk.point("waist")) / 2,
                Vec3{0.62 * u * py_pow(build, 0.3) * py_max(0.6, pyv::to_float(sk.body.at("hips"))), 0.75 * u, 0.42 * u * build},
                &sk.turn("spine"), 14, 9, 1);
    const Json* sex = pyv::find(sk.body, "sex");
    if (sex != nullptr && sex->is_string() && sex->get_ref<const std::string&>() == "female") {
        // (the chest's shape, and a narrower waist)
        const Vec3 chest = (sk.point("waist") + sk.point("neck")) / 2 + matvec(chest_turn, Vec3{0.0, 0.25 * u, 0.0});
        for (const int s : {1, -1}) {
            b.ellipsoid(chest + matvec(chest_turn, Vec3{s * 0.36 * u, 0.05 * u, -0.38 * u * build}), Vec3{0.3 * u, 0.28 * u, 0.26 * u},
                        &chest_turn, 10, 7, 1);
        }
    }
    b.capsule(sk.point("neck"), sk.point("head_base"), t * 0.8, 10, 1);
    head_mesh(b, sk.point("head"), sk.turn("head"), u, 2);
    static const Json kNoHands = Json::object();
    const Json& hands = pyv::get_or(prim, "hands", kNoHands);
    static const Json kRelaxed = "relaxed";
    for (const auto& [p, s] : {std::pair<const char*, int>{"l", 1}, {"r", -1}}) {
        const std::string side(p);
        b.capsule(sk.point(side + "_shoulder"), sk.point(side + "_elbow"), t * 1.05, 10, 3);
        b.capsule(sk.point(side + "_elbow"), sk.point(side + "_wrist"), t * 0.85, 10, 3);
        const Json* pose = pyv::get(hands, side);
        hand(b, sk.point(side + "_wrist"), sk.turn(side + "_wrist"), 0.8 * u, pose != nullptr && py_truthy(*pose) ? *pose : kRelaxed,
             s, 4);
        b.capsule(sk.point(side + "_hip"), sk.point(side + "_knee"), t * 1.45, 10, 5);
        b.capsule(sk.point(side + "_knee"), sk.point(side + "_ankle"), t * 1.1, 10, 5);
        const Vec3 foot_mid = (sk.point(side + "_ankle") + sk.point(side + "_toe")) / 2;
        const Mat3 foot = matmul(sk.turn(side + "_ankle"), rx(-1.35));
        b.box(foot_mid, Vec3{0.4 * u, 0.3 * u, 1.0 * u}, &foot, 6);
    }
    auto model = std::make_shared<Model>();
    model->mesh = std::move(b.m);
    model->marks = head_marks(sk.point("head"), sk.turn("head"), u);
    return model;
}

// _figure_cached: the last 64 figures, by their size, body, joints and hands
struct FigureCache {
    std::mutex mutex;
    std::list<std::pair<std::string, std::shared_ptr<const Model>>> items;  // most recent first
};

FigureCache& figure_cache() {
    static FigureCache cache;
    return cache;
}

std::shared_ptr<const Model> cached_figure(const Json& prim) {
    Json key_value = Json::object();
    for (const char* k : {"size", "body", "joints", "hands"}) {
        const Json* v = pyv::get(prim, k);
        key_value[k] = v != nullptr ? *v : Json();
    }
    const std::string key = dump_canonical(key_value);
    FigureCache& cache = figure_cache();
    {
        std::lock_guard lock(cache.mutex);
        for (auto it = cache.items.begin(); it != cache.items.end(); ++it) {
            if (it->first == key) {
                cache.items.splice(cache.items.begin(), cache.items, it);
                return it->second;
            }
        }
    }
    std::shared_ptr<const Model> made = figure(key_value);
    std::lock_guard lock(cache.mutex);
    cache.items.emplace_front(key, made);
    while (cache.items.size() > 64) cache.items.pop_back();
    return made;
}

// --- the older guides' surfaces ------------------------------------------------------------------------------------

std::shared_ptr<const Model> solid(const Json& prim) {
    const Vec3 size = prim3d::size_of(prim);
    const double w = size[0];
    const double h = size[1];
    const double d = size[2];
    const Json* kind_value = pyv::get(prim, "kind");
    const std::string kind = kind_value != nullptr && kind_value->is_string() ? kind_value->get<std::string>() : std::string();
    Builder b;
    if (kind == "box") {
        b.box(Vec3{0.0, 0.0, 0.0}, Vec3{w, h, d}, nullptr);
    } else if (kind == "cylinder") {
        const int n = 24;
        std::vector<Vec3> verts;
        for (const double y : {-h / 2, h / 2}) {
            for (int k = 0; k < n; ++k) verts.push_back(Vec3{w / 2 * py_cos(2 * kPi * k / n), y, d / 2 * py_sin(2 * kPi * k / n)});
        }
        std::vector<std::vector<std::int64_t>> faces;
        for (int k = 0; k < n; ++k) faces.push_back({k, (k + 1) % n, n + (k + 1) % n, n + k});
        std::vector<std::int64_t> top;
        std::vector<std::int64_t> bottom;
        for (int k = 0; k < n; ++k) top.push_back(k);
        for (int k = 2 * n - 1; k > n - 1; --k) bottom.push_back(k);
        faces.push_back(top);
        faces.push_back(bottom);
        b.add(verts, faces, 0, true);
    } else if (kind == "stairs") {
        const Json* steps_value = pyv::get(prim, "steps");
        const std::int64_t steps =
            std::max<std::int64_t>(2, std::min<std::int64_t>(30, steps_value != nullptr && py_truthy(*steps_value) ? pyv::to_int(*steps_value) : 6));
        const double rise = h / static_cast<double>(steps);
        const double run = d / static_cast<double>(steps);
        for (std::int64_t k = 0; k < steps; ++k) {  // each step a box from the ground up
            const double top = h / 2 - rise * static_cast<double>(k + 1);
            b.box(Vec3{0.0, (top + h / 2) / 2, -d / 2 + run * (static_cast<double>(k) + 0.5)}, Vec3{w, h / 2 - top, run}, nullptr);
        }
    } else if (kind == "floor") {
        b.add({Vec3{-w / 2, 0.0, -d / 2}, Vec3{w / 2, 0.0, -d / 2}, Vec3{w / 2, 0.0, d / 2}, Vec3{-w / 2, 0.0, d / 2}}, {{0, 1, 2, 3}}, 0,
              false);
    } else if (kind == "sphere") {
        b.ellipsoid(Vec3{0.0, 0.0, 0.0}, Vec3{w / 2, h / 2, d / 2}, nullptr, 24, 14);
    } else if (kind == "cone") {
        const int n = 24;
        std::vector<Vec3> verts{Vec3{0.0, -h / 2, 0.0}};
        for (int k = 0; k < n; ++k) verts.push_back(Vec3{w / 2 * py_cos(2 * kPi * k / n), h / 2, d / 2 * py_sin(2 * kPi * k / n)});
        std::vector<std::vector<std::int64_t>> faces;
        for (int k = 0; k < n; ++k) faces.push_back({0, 1 + (k + 1) % n, 1 + k});
        std::vector<std::int64_t> base;
        for (int k = 1; k <= n; ++k) base.push_back(k);
        faces.push_back(base);
        b.add(verts, faces, 0, true);
    } else if (kind == "prop") {
        const std::string name = str_or(pyv::get(prim, "prop"), "chair");
        const auto* boxes = prim3d::prop_boxes(name);
        if (boxes == nullptr) boxes = prim3d::prop_boxes("chair");
        for (const auto& [bx, by, bz, bw, bh, bd] : *boxes) {
            b.box(Vec3{(bx - 0.5) * w, (by - 0.5) * h, (bz - 0.5) * d}, Vec3{bw * w, bh * h, bd * d}, nullptr);
        }
    } else if (kind == "scene") {
        const double g = h / 2;
        const double top = -h / 2;
        const double back = d / 2;
        b.add({Vec3{-w / 2, g, -d / 2}, Vec3{w / 2, g, -d / 2}, Vec3{w / 2, g, back}, Vec3{-w / 2, g, back}}, {{0, 1, 2, 3}}, 0, false);
        const Json* scene = pyv::get(prim, "scene");
        if (!(scene != nullptr && pyv::eq(*scene, Json("street")))) {
            b.add({Vec3{-w / 2, top, back}, Vec3{w / 2, top, back}, Vec3{w / 2, g, back}, Vec3{-w / 2, g, back}}, {{0, 1, 2, 3}}, 0, false);
            for (const double x : {-w / 2, w / 2}) {
                b.add({Vec3{x, top, -d / 2}, Vec3{x, top, back}, Vec3{x, g, back}, Vec3{x, g, -d / 2}}, {{0, 1, 2, 3}}, 0, false);
            }
        }
    }
    auto model = std::make_shared<Model>();
    model->mesh = std::move(b.m);
    return model;
}

// np.asarray(light, dtype=float) for the light's direction (a list; its items numbers, booleans or numeric text)
std::vector<double> light_vector(const Json* light) {
    if (light == nullptr) return {-0.5, -0.7, -0.6};
    if (!light->is_array()) {
        if (light->is_number() || light->is_boolean() || light->is_string()) {
            throw Error("value", "matmul: Input operand 1 does not have enough dimensions (has 0, gufunc core with signature "
                                 "(n?,k),(k,m?)->(n?,m?) requires 1)");
        }
        throw Error("type", "float() argument must be a string or a real number, not '" + py_type_name(*light) + "'");
    }
    std::vector<double> out;
    for (const Json& v : *light) out.push_back(v.is_null() ? std::nan("") : pyv::to_float(v));
    return out;
}

// --- triangles ----------------------------------------------------------------------------------------------------

// _fill_triangle over a w × h picture, for the pixels of out's window
void fill_triangle(const std::array<Point2, 3>& p, const std::array<double, 3>& z, float value, int w, int h, Raster& out) {
    // (numpy's min and max: a NaN wins)
    const auto low = [](double a, double b) { return la::np_minimum(a, b); };
    const auto high = [](double a, double b) { return la::np_maximum(a, b); };
    const double min_x = low(low(p[0][0], p[1][0]), p[2][0]);
    const double min_y = low(low(p[0][1], p[1][1]), p[2][1]);
    const double max_x = high(high(p[0][0], p[1][0]), p[2][0]);
    const double max_y = high(high(p[0][1], p[1][1]), p[2][1]);
    const double fx0 = py_max(0.0, la::py_floor_int(min_x));
    const double fy0 = py_max(0.0, la::py_floor_int(min_y));
    const double fx1 = py_min(static_cast<double>(w - 1), la::py_ceil_int(max_x));
    const double fy1 = py_min(static_cast<double>(h - 1), la::py_ceil_int(max_y));
    if (fx1 < fx0 || fy1 < fy0) return;
    // (only the window's pixels: each pixel's value depends on nothing but the triangle)
    const int x0 = std::max(static_cast<int>(fx0), out.x0);
    const int y0 = std::max(static_cast<int>(fy0), out.y0);
    const int x1 = std::min(static_cast<int>(fx1), out.x0 + out.width - 1);
    const int y1 = std::min(static_cast<int>(fy1), out.y0 + out.height - 1);
    if (x1 < x0 || y1 < y0) return;
    const double ax = p[0][0];
    const double ay = p[0][1];
    const double bx = p[1][0];
    const double by = p[1][1];
    const double cx = p[2][0];
    const double cy = p[2][1];
    const double den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (std::fabs(den) < 1e-9) return;
    const double a1 = by - cy;
    const double b1 = cx - bx;
    const double a2 = cy - ay;
    const double b2 = ax - cx;
    for (int yi = y0; yi <= y1; ++yi) {
        const double gy = static_cast<double>(yi) + 0.5;
        const double dy = gy - cy;
        for (int xi = x0; xi <= x1; ++xi) {
            const double gx = static_cast<double>(xi) + 0.5;
            const double dx = gx - cx;
            const double l1 = (a1 * dx + b1 * dy) / den;
            const double l2 = (a2 * dx + b2 * dy) / den;
            const double l3 = 1 - l1 - l2;
            if (!(l1 >= -1e-6 && l2 >= -1e-6 && l3 >= -1e-6)) continue;
            const double depth = l1 * z[0] + l2 * z[1] + l3 * z[2];
            const std::size_t at = static_cast<std::size_t>(yi - out.y0) * static_cast<std::size_t>(out.width) +
                                   static_cast<std::size_t>(xi - out.x0);
            if (depth < static_cast<double>(out.zbuf[at])) {
                out.zbuf[at] = static_cast<float>(depth);
                out.shade[at] = value;
            }
        }
    }
}

// _turned: the prim's points turned as they are seen (its own turn, then the camera's)
std::vector<Vec3> turned(const Json& prim, std::span<const Vec3> verts, const Json* camera) {
    static const Json kZeroRot = Json::array({0, 0, 0});
    std::vector<Vec3> out = la::rows_mt(verts, prim_rotation(pyv::get_or(prim, "rot", kZeroRot)));
    if (camera != nullptr) {
        const Json* tip = pyv::get(*camera, "tip");
        const Json* turn = pyv::get(*camera, "turn");
        const Json* roll = pyv::get(*camera, "roll");
        const Json view = Json::array({tip != nullptr ? *tip : Json(0), turn != nullptr ? *turn : Json(0), roll != nullptr ? *roll : Json(0)});
        out = la::rows_mt(out, prim_rotation(view));
    }
    return out;
}

std::vector<Vec3> normals(const std::vector<Vec3>& verts3, const std::vector<std::vector<std::int64_t>>& faces) {
    std::vector<Vec3> out;
    out.reserve(faces.size());
    for (const auto& face : faces) {
        if (face.size() < 2) pyv::raise_index("tuple index out of range");
        const Vec3& a = verts3[vertex(face[0], verts3.size())];
        const Vec3& b = verts3[vertex(face[1], verts3.size())];
        const Vec3& c = verts3[vertex(face[face.size() > 2 ? 2 : 1], verts3.size())];
        const Vec3 n = la::cross(b - a, c - a);
        const double norm = la::norm(n);
        out.push_back(norm > 1e-9 ? n / norm : Vec3{0.0, 0.0, 0.0});
    }
    return out;
}

const Json* truthy(const Json* camera) { return camera != nullptr && py_truthy(*camera) ? camera : nullptr; }

struct EdgeHash {
    std::size_t operator()(const std::pair<std::int64_t, std::int64_t>& e) const {
        return std::hash<std::int64_t>{}(e.first) * 0x9E3779B97F4A7C15ULL ^ std::hash<std::int64_t>{}(e.second);
    }
};

std::vector<Line2> join_pieces(std::vector<Line2> pieces) {
    std::vector<Line2> out;
    for (Line2& piece : pieces) {
        if (!out.empty() && py_dist(out.back().back()[0], out.back().back()[1], piece[0][0], piece[0][1]) < 0.05) {
            out.back().insert(out.back().end(), piece.begin() + 1, piece.end());
        } else {
            out.push_back(std::move(piece));
        }
    }
    return out;
}

struct LinesCache {
    std::mutex mutex;
    std::list<std::pair<std::string, std::shared_ptr<const std::vector<Line2>>>> items;
};

LinesCache& lines_cache() {
    static LinesCache cache;
    return cache;
}

}  // namespace

// --- tables and small helpers ----------------------------------------------------------------------------------------

bool is_mesh_kind(const Json& kind) { return pyv::is_one_of(kind, {"figure", "head", "hand", "mesh"}); }

const Json& figure_presets() { return json_of(kPresets); }
const Json& sex_body() { return json_of(kSexBody); }

std::array<double, 5> hand_curls(const Json& value) {
    if (value.is_array() && value.size() == 5) {
        std::array<double, 5> out{};
        for (std::size_t i = 0; i < 5; ++i) out[i] = clamp01(pyv::to_float(value[i]));
        return out;
    }
    if (value.is_object()) {
        std::array<double, 5> base = curls_of(str_or(pyv::get(value, "pose"), "relaxed"));
        const Json* curls = pyv::get(value, "curls");
        if (curls != nullptr && curls->is_array() && curls->size() == 5) {
            for (std::size_t i = 0; i < 5; ++i) base[i] = pyv::to_float((*curls)[i]);
        } else if (curls != nullptr && curls->is_object()) {
            for (const auto& [name, c] : curls->items()) {
                for (std::size_t i = 0; i < kFingers.size(); ++i) {
                    if (kFingers[i] == name) base[i] = pyv::to_float(c);
                }
            }
        }
        for (double& c : base) c = clamp01(c);
        return base;
    }
    return curls_of(str_or(&value, "relaxed"));
}

Mat3 rx(double t) {
    const double c = py_cos(t);
    const double s = py_sin(t);
    return Mat3{Vec3{1.0, 0.0, 0.0}, Vec3{0.0, c, s}, Vec3{0.0, -s, c}};
}

Mat3 ry(double t) {
    const double c = py_cos(t);
    const double s = py_sin(t);
    return Mat3{Vec3{c, 0.0, -s}, Vec3{0.0, 1.0, 0.0}, Vec3{s, 0.0, c}};
}

Mat3 rz(double t) {
    const double c = py_cos(t);
    const double s = py_sin(t);
    return Mat3{Vec3{c, s, 0.0}, Vec3{-s, c, 0.0}, Vec3{0.0, 0.0, 1.0}};
}

Mat3 prim_rotation(const Json& rot) {
    // tip, turn, lean = (list(rot or [0, 0, 0]) + [0, 0, 0])[:3]
    Json items = py_truthy(rot) ? py_list(rot) : Json::array({0, 0, 0});
    for (int i = 0; i < 3; ++i) items.push_back(0);
    const double turn = pyv::real(items[1]);
    const Mat3 m_turn{Vec3{py_cos(turn), 0.0, -py_sin(turn)}, Vec3{0.0, 1.0, 0.0}, Vec3{py_sin(turn), 0.0, py_cos(turn)}};
    const double tip = pyv::real(items[0]);
    const Mat3 m_tip{Vec3{1.0, 0.0, 0.0}, Vec3{0.0, py_cos(tip), -py_sin(tip)}, Vec3{0.0, py_sin(tip), py_cos(tip)}};
    const double lean = pyv::real(items[2]);
    const Mat3 m_lean{Vec3{py_cos(lean), -py_sin(lean), 0.0}, Vec3{py_sin(lean), py_cos(lean), 0.0}, Vec3{0.0, 0.0, 1.0}};
    return matmul(matmul(m_lean, m_tip), m_turn);
}

Mat3 joint_matrix(const Json& joint) {
    static const Json kNone = Json::object();
    const Json& j = py_truthy(joint) ? joint : kNone;
    const auto axis = [&](const char* key) {
        const Json* v = pyv::get(j, key);
        return v != nullptr ? pyv::to_float(*v) : 0.0;
    };
    const Mat3 z = rz(axis("z"));
    const Mat3 x = rx(axis("x"));
    const Mat3 y = ry(axis("y"));
    return matmul(matmul(z, x), y);
}

const Vec3& Skeleton::point(std::string_view name) const {
    for (const auto& [key, p] : points) {
        if (key == name) return p;
    }
    throw OpKeyError(py_repr_str(name));
}

const Mat3& Skeleton::turn(std::string_view name) const {
    for (const auto& [key, m] : turns) {
        if (key == name) return m;
    }
    throw OpKeyError(py_repr_str(name));
}

Skeleton figure_skeleton(const Json& prim) {
    static const Json kDefaultSize = Json::array({40, 80, 20});
    static const Json kNone = Json::object();
    const Json& size = pyv::get_or(prim, "size", kDefaultSize);
    double height = pyv::to_float(size.is_array() ? pyv::at(size, 1) : size);
    if (height == 0.0) height = 80.0;  // (float(…) or 80.0)
    const Json own = py_dict(pyv::get_or(prim, "body", kNone));
    Json body = Json::object({{"heads", 7.5}, {"shoulders", 1.0}, {"hips", 1.0}, {"build", 1.0}, {"legs", 1.0}});
    if (const Json* sb = pyv::find(sex_body(), str_or(pyv::get(own, "sex"), ""))) {
        for (const auto& [k, v] : sb->items()) body[k] = v;
    }
    for (const auto& [k, v] : own.items()) body[k] = v;
    const double u = height / py_max(4.0, py_min(10.0, pyv::to_float(body["heads"])));
    const Json& joints = pyv::get_or(prim, "joints", kNone);
    const double legs = py_max(0.6, py_min(1.5, pyv::to_float(body["legs"])));

    const auto j = [&](const std::string& name) {
        Json base = Json::object();
        if (name == "l_arm") base["z"] = 0.12;
        if (name == "r_arm") base["z"] = -0.12;
        const Json* given = pyv::get(joints, name);
        if (given != nullptr && py_truthy(*given)) {
            if (!given->is_object()) pyv::raise_attribute(*given, "items");
            for (const auto& [axis, value] : given->items()) {
                const Json* before = pyv::find(base, axis);
                const double b = before != nullptr ? before->get<double>() : 0.0;
                base[axis] = b + pyv::to_float(value);
            }
        }
        return joint_matrix(base);
    };

    Skeleton sk;
    const Mat3 hip_r = j("hip");
    const Vec3 pelvis{0.0, 0.0, 0.0};
    sk.points.emplace_back("pelvis", pelvis);
    const Mat3 spine_r = matmul(hip_r, j("spine"));
    const Vec3 waist = pelvis + matvec(spine_r, Vec3{0.0, -1.3 * u, 0.0});
    const Mat3 chest_r = matmul(spine_r, j("chest"));
    const Vec3 neck_base = waist + matvec(chest_r, Vec3{0.0, -1.45 * u, 0.0});
    const Mat3 neck_r = matmul(chest_r, j("neck"));
    const Vec3 head_base = neck_base + matvec(neck_r, Vec3{0.0, -0.45 * u, 0.0});
    const Mat3 head_r = matmul(neck_r, j("head"));
    const Vec3 head_c = head_base + matvec(head_r, Vec3{0.0, -0.55 * u, 0.0});
    sk.points.emplace_back("waist", waist);
    sk.points.emplace_back("neck", neck_base);
    sk.points.emplace_back("head_base", head_base);
    sk.points.emplace_back("head", head_c);
    sk.turns.emplace_back("hip", hip_r);
    sk.turns.emplace_back("spine", spine_r);
    sk.turns.emplace_back("chest", chest_r);
    sk.turns.emplace_back("neck", neck_r);
    sk.turns.emplace_back("head", head_r);
    const double shoulder_w = 0.95 * u * py_max(0.6, py_min(1.5, pyv::to_float(body["shoulders"])));
    const double hip_w = 0.5 * u * py_max(0.6, py_min(1.6, pyv::to_float(body["hips"])));
    for (const auto& [p, s] : {std::pair<const char*, int>{"l", 1}, {"r", -1}}) {
        const std::string side(p);
        const Vec3 shoulder = neck_base + matvec(chest_r, Vec3{s * shoulder_w, 0.2 * u, 0.0});
        const Mat3 arm_r = matmul(chest_r, j(side + "_arm"));
        const Vec3 elbow = shoulder + matvec(arm_r, Vec3{0.0, 1.5 * u, 0.0});
        const Mat3 fore_r = matmul(arm_r, j(side + "_elbow"));
        const Vec3 wrist = elbow + matvec(fore_r, Vec3{0.0, 1.3 * u, 0.0});
        const Mat3 hand_r = matmul(fore_r, j(side + "_wrist"));
        const Vec3 hip = pelvis + matvec(hip_r, Vec3{s * hip_w, 0.15 * u, 0.0});
        const Mat3 thigh_r = matmul(hip_r, j(side + "_leg"));
        const Vec3 knee = hip + matvec(thigh_r, Vec3{0.0, 2.0 * u * legs, 0.0});
        const Mat3 shin_r = matmul(thigh_r, j(side + "_knee"));
        const Vec3 ankle = knee + matvec(shin_r, Vec3{0.0, 1.95 * u * legs, 0.0});
        const Mat3 foot_r = matmul(shin_r, j(side + "_ankle"));
        const Vec3 toe = ankle + matvec(foot_r, Vec3{0.0, 0.25 * u, -0.85 * u});
        sk.points.emplace_back(side + "_shoulder", shoulder);
        sk.points.emplace_back(side + "_elbow", elbow);
        sk.points.emplace_back(side + "_wrist", wrist);
        sk.points.emplace_back(side + "_hip", hip);
        sk.points.emplace_back(side + "_knee", knee);
        sk.points.emplace_back(side + "_ankle", ankle);
        sk.points.emplace_back(side + "_toe", toe);
        sk.points.emplace_back(side + "_hand", wrist + matvec(hand_r, Vec3{0.0, 0.45 * u, 0.0}));
        sk.turns.emplace_back(side + "_arm", arm_r);
        sk.turns.emplace_back(side + "_elbow", fore_r);
        sk.turns.emplace_back(side + "_wrist", hand_r);
        sk.turns.emplace_back(side + "_leg", thigh_r);
        sk.turns.emplace_back(side + "_knee", shin_r);
        sk.turns.emplace_back(side + "_ankle", foot_r);
    }
    sk.unit = u;
    sk.body = std::move(body);
    return sk;
}

std::shared_ptr<const Model> model_of(const Json& prim) {
    const Json* kind_value = pyv::get(prim, "kind");
    const Json kind = kind_value != nullptr ? *kind_value : Json();
    static const Json kDefaultSize = Json::array({40, 40, 40});
    const Json& size = pyv::get_or(prim, "size", kDefaultSize);
    if (pyv::eq(kind, Json("figure"))) return cached_figure(prim);
    if (pyv::eq(kind, Json("head"))) {
        double height = pyv::to_float(size.is_array() ? pyv::at(size, 1) : size);
        if (height == 0.0) height = 30.0;
        const double u = height / 1.1;
        Builder b;
        head_mesh(b, Vec3{0.0, 0.0, 0.0}, la::identity3(), u, 2);
        auto model = std::make_shared<Model>();
        model->mesh = std::move(b.m);
        model->marks = head_marks(Vec3{0.0, 0.0, 0.0}, la::identity3(), u);
        return model;
    }
    if (pyv::eq(kind, Json("hand"))) {
        double length = pyv::to_float(size.is_array() ? pyv::at(size, 1) : size);
        if (length == 0.0) length = 20.0;
        Builder b;
        const Json* side_value = pyv::get(prim, "side");
        const int side = pyv::eq(side_value != nullptr ? *side_value : Json("r"), Json("l")) ? 1 : -1;
        const Json* pose_value = pyv::get(prim, "pose");
        const Json pose = pose_value != nullptr && py_truthy(*pose_value) ? *pose_value : Json("relaxed");
        const Json* curls = pyv::get(prim, "curls");
        const Json shape = curls != nullptr && py_truthy(*curls) ? Json::object({{"pose", pose}, {"curls", *curls}}) : pose;
        hand(b, Vec3{0.0, -length / 2, 0.0}, la::identity3(), length, shape, side, 4);
        auto model = std::make_shared<Model>();
        model->mesh = std::move(b.m);
        return model;
    }
    if (pyv::is_one_of(kind, {"box", "cylinder", "stairs", "floor", "scene", "sphere", "cone", "prop"})) return solid(prim);
    if (pyv::eq(kind, Json("mesh"))) {
        static const Json kNone = Json::object();
        const Json& data = pyv::get_or(prim, "mesh", kNone);
        static const Json kEmpty = Json::array();
        const Json& flat = pyv::get_or(data, "v", kEmpty);
        std::vector<double> values;
        for (const Json& c : py_list(flat)) values.push_back(c.is_null() ? std::nan("") : pyv::to_float(c));
        if (values.size() % 3 != 0) {
            throw Error("value", "cannot reshape array of size " + std::to_string(values.size()) + " into shape (3)");
        }
        Json dims = size.is_array() ? size : Json::array({size, size, size});
        if (size.is_array()) {
            const Json last = pyv::at(size, -1);
            for (int i = 0; i < 3; ++i) dims.push_back(last);
        }
        const double w = pyv::to_float(dims[0]);
        const double h = pyv::to_float(dims[1]);
        const double d = pyv::to_float(dims[2]);
        auto model = std::make_shared<Model>();
        for (std::size_t i = 0; i + 2 < values.size(); i += 3) {
            model->mesh.v.push_back(Vec3{values[i] * w, values[i + 1] * h, values[i + 2] * d});
        }
        for (const Json& face : py_list(pyv::get_or(data, "f", kEmpty))) {
            std::vector<std::int64_t> f;
            for (const Json& i : py_list(face)) f.push_back(pyv::to_int(i));
            model->mesh.f.push_back(std::move(f));
            model->mesh.parts.push_back(0);
        }
        return model;
    }
    return std::make_shared<Model>();
}

const Json* camera_of(const Json& prim, const Json* page_camera) {
    if (page_camera != nullptr && py_truthy(*page_camera)) return page_camera;
    return truthy(pyv::find(prim, "camera"));
}

std::vector<Vec3> face_normals(const Json& prim, const Mesh& mesh, const Json* camera) {
    return normals(turned(prim, mesh.v, truthy(camera)), mesh.f);
}

OnPage to_page(const Json& prim, std::span<const Vec3> local, const Json* camera) {
    camera = truthy(camera);
    static const Json kDefaultPos = Json::array({100, 150, 0});
    Json pos = py_list(pyv::get_or(prim, "pos", kDefaultPos));
    for (int i = 0; i < 3; ++i) pos.push_back(0);
    const Json cx = pos[0];
    const Json cy = pos[1];
    const Json cz = pos[2];
    static const Json kZeroRot = Json::array({0, 0, 0});
    const std::vector<Vec3> turned_pts = la::rows_mt(local, prim_rotation(pyv::get_or(prim, "rot", kZeroRot)));
    OnPage out;
    out.pts.reserve(local.size());
    out.depth.reserve(local.size());
    if (camera != nullptr) {
        const Vec3 at{pyv::to_float(cx), pyv::to_float(cy), pyv::to_float(cz)};
        const Json* target_value = pyv::get(*camera, "target");
        const Json target = target_value != nullptr && py_truthy(*target_value) ? *target_value : Json::array({cx, cy});
        const double tx = pyv::to_float(pyv::at(target, 0));
        const double ty = pyv::to_float(pyv::at(target, 1));
        const Json* tip = pyv::get(*camera, "tip");
        const Json* turn = pyv::get(*camera, "turn");
        const Json* roll = pyv::get(*camera, "roll");
        const Mat3 view = prim_rotation(
            Json::array({tip != nullptr ? *tip : Json(0), turn != nullptr ? *turn : Json(0), roll != nullptr ? *roll : Json(0)}));
        std::vector<Vec3> rel(turned_pts.size());
        for (std::size_t i = 0; i < turned_pts.size(); ++i) rel[i] = (turned_pts[i] + at) - Vec3{tx, ty, 0.0};
        rel = la::rows_mt(rel, view);
        const Json* focal_value = pyv::get(*camera, "focal_mm");
        double focal = 400.0;
        if (focal_value != nullptr && py_truthy(*focal_value)) {
            focal = pyv::to_float(*focal_value);
        } else if (const Json* own = pyv::get(prim, "focal_mm"); own != nullptr && py_truthy(*own)) {
            focal = pyv::to_float(*own);
        }
        for (const Vec3& r : rel) {
            const double scale = focal / la::np_maximum(focal * 0.2, focal + r[2]);
            out.pts.push_back(Point2{tx + r[0] * scale, ty + r[1] * scale});
            out.depth.push_back(r[2]);
        }
        return out;
    }
    const Json* focal_value = pyv::get(prim, "focal_mm");
    const double focal = focal_value == nullptr ? 400.0 : (py_truthy(*focal_value) ? pyv::to_float(*focal_value) : 400.0);
    const double z = focal + pyv::to_float(cz);
    const double x = pyv::to_float(cx);
    const double y = pyv::to_float(cy);
    const double zc = pyv::to_float(cz);
    for (const Vec3& t : turned_pts) {
        const double scale = focal / la::np_maximum(focal * 0.2, z + t[2]);
        out.pts.push_back(Point2{x + t[0] * scale, y + t[1] * scale});
        out.depth.push_back(t[2] + zc);
    }
    return out;
}

Seen seen(const Json& prim, const Json* camera) {
    Seen out;
    out.model = model_of(prim);
    if (out.model->mesh.v.empty()) return out;
    out.at = to_page(prim, out.model->mesh.v, camera);
    for (const auto& line : out.model->marks) {
        if (!line.empty()) out.guides.push_back(to_page(prim, line, camera));
    }
    return out;
}

bool Raster::alpha(std::size_t i) const { return std::isfinite(zbuf[i]); }

bool Raster::any() const {
    return std::any_of(zbuf.begin(), zbuf.end(), [](float z) { return std::isfinite(z); });
}

Raster raster(std::span<const Json> prims, int width, int height, double dpi, const Json* camera, const Json* light,
              double ambient, double box_x, double box_y, const Window* window) {
    if (width <= 0 || height <= 0) throw Error("value", "negative dimensions are not allowed");
    Raster out;
    if (window != nullptr) {
        out.x0 = std::max(0, window->x0);
        out.y0 = std::max(0, window->y0);
        out.width = std::max(0, std::min(width, window->x1) - out.x0);
        out.height = std::max(0, std::min(height, window->y1) - out.y0);
    } else {
        out.width = width;
        out.height = height;
    }
    if (static_cast<std::int64_t>(out.width) * out.height > kMaxRasterPixels) {
        throw Error("image_too_large", "the 3D is too large to draw at this resolution");
    }
    out.zbuf.assign(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height), std::numeric_limits<float>::infinity());
    out.shade.assign(out.zbuf.size(), 0.0f);
    const double k = dpi / 25.4;
    std::vector<double> lit = light_vector(light);
    {
        double sq = 0.0;
        if (lit.size() == 3) {
            sq = la::dot(Vec3{lit[0], lit[1], lit[2]}, Vec3{lit[0], lit[1], lit[2]});
        } else {
            for (const double v : lit) sq = sq + v * v;
            sq = 0.0 + sq;
        }
        double norm = std::sqrt(sq);
        if (norm == 0.0) norm = 1.0;  // (or 1.0)
        for (double& v : lit) v = v / norm;
    }
    camera = truthy(camera);
    for (const Json& prim : prims) {
        const std::shared_ptr<const Model> model = model_of(prim);
        const Mesh& mesh = model->mesh;
        if (mesh.f.empty()) continue;
        const OnPage at = to_page(prim, mesh.v, camera);
        const std::vector<Vec3> n_all = normals(turned(prim, mesh.v, camera), mesh.f);
        std::vector<Point2> px(at.pts.size());
        for (std::size_t i = 0; i < at.pts.size(); ++i) px[i] = Point2{(at.pts[i][0] - box_x) * k, (at.pts[i][1] - box_y) * k};
        for (std::size_t fi = 0; fi < mesh.f.size(); ++fi) {
            Vec3 n = n_all[fi];
            // (a face turned away still shows its inside from the other side: light it by the side seen)
            if (n[2] > 0) n = -n;
            if (lit.size() != 3) {
                throw Error("value", "matmul: Input operand 1 has a mismatch in its core dimension 0, with gufunc signature "
                                     "(n?,k),(k,m?)->(n?,m?) (size " + std::to_string(lit.size()) + " is different from 3)");
            }
            const double d = la::dot(n, Vec3{lit[0], lit[1], lit[2]});
            const double value = ambient + (1 - ambient) * (d > 0.0 ? d : 0.0);
            const auto& face = mesh.f[fi];
            for (std::size_t t = 1; t + 1 < face.size(); ++t) {
                const std::size_t a = vertex(face[0], px.size());
                const std::size_t b = vertex(face[t], px.size());
                const std::size_t c = vertex(face[t + 1], px.size());
                fill_triangle({px[a], px[b], px[c]}, {at.depth[a], at.depth[b], at.depth[c]}, static_cast<float>(value), width, height, out);
            }
        }
    }
    return out;
}

std::vector<Line2> lines(std::span<const Json> prims, const Json* camera, double dpi, double crease_deg) {
    camera = truthy(camera);
    std::vector<Json> meshy;
    for (const Json& p : prims) {
        const Json* kind = pyv::get(p, "kind");
        if (kind != nullptr && is_mesh_kind(*kind)) meshy.push_back(p);
    }
    if (meshy.empty()) return {};
    struct Seg {
        Point2 a;
        Point2 b;
        double za;
        double zb;
    };
    std::vector<Seg> segs;
    for (const Json& prim : meshy) {
        const Seen s = seen(prim, camera);
        if (s.model->mesh.v.empty() || s.model->mesh.f.empty()) continue;  // (seen gives no faces without points)
        const Mesh& mesh = s.model->mesh;
        const std::vector<Vec3> n_all = normals(turned(prim, mesh.v, camera), mesh.f);
        std::vector<bool> facing(n_all.size());
        for (std::size_t i = 0; i < n_all.size(); ++i) facing[i] = n_all[i][2] < 0;  // toward the viewer
        // the faces of each edge, the edges in the order they are first met (Python's dict)
        std::vector<std::pair<std::int64_t, std::int64_t>> order;
        std::vector<std::vector<std::size_t>> faces_of;
        std::unordered_map<std::pair<std::int64_t, std::int64_t>, std::size_t, EdgeHash> index;
        index.reserve(mesh.f.size() * 2);
        for (std::size_t fi = 0; fi < mesh.f.size(); ++fi) {
            const auto& face = mesh.f[fi];
            for (std::size_t i = 0; i < face.size(); ++i) {
                const std::int64_t a = face[i];
                const std::int64_t b = face[(i + 1) % face.size()];
                const std::pair<std::int64_t, std::int64_t> key{std::min(a, b), std::max(a, b)};
                const auto [it, added] = index.emplace(key, order.size());
                if (added) {
                    order.push_back(key);
                    faces_of.emplace_back();
                }
                faces_of[it->second].push_back(fi);
            }
        }
        const double cos_crease = py_cos(crease_deg * (kPi / 180.0));
        for (std::size_t e = 0; e < order.size(); ++e) {
            const auto& fs = faces_of[e];
            bool keep = false;
            if (fs.size() == 1) {
                keep = mesh.parts[fs[0]] < kSmooth;  // (an open edge; a rounded piece's poles are not edges)
            } else {
                const std::size_t f0 = fs[0];
                const std::size_t f1 = fs[1];
                const bool rounded = mesh.parts[f0] >= kSmooth && mesh.parts[f1] >= kSmooth;
                keep = facing[f0] != facing[f1] || (!rounded && la::dot(n_all[f0], n_all[f1]) < cos_crease);
            }
            if (keep) {
                const std::size_t a = vertex(order[e].first, s.at.pts.size());
                const std::size_t b = vertex(order[e].second, s.at.pts.size());
                segs.push_back(Seg{s.at.pts[a], s.at.pts[b], s.at.depth[a], s.at.depth[b]});
            }
        }
        for (const OnPage& g : s.guides) {
            for (std::size_t i = 0; i + 1 < g.pts.size(); ++i) segs.push_back(Seg{g.pts[i], g.pts[i + 1], g.depth[i] - 0.3, g.depth[i + 1] - 0.3});
        }
    }
    if (segs.empty()) return {};
    // the box of every end (numpy's min and max: a NaN wins), 2 mm round
    double x0 = segs[0].a[0];
    double y0 = segs[0].a[1];
    double x1 = x0;
    double y1 = y0;
    const auto take = [&](const Point2& p) {
        x0 = la::np_minimum(x0, p[0]);
        y0 = la::np_minimum(y0, p[1]);
        x1 = la::np_maximum(x1, p[0]);
        y1 = la::np_maximum(y1, p[1]);
    };
    for (const Seg& s : segs) take(s.a);
    for (const Seg& s : segs) take(s.b);
    x0 = x0 - 2;
    y0 = y0 - 2;
    x1 = x1 + 2;
    y1 = y1 + 2;
    const double k = dpi / 25.4;
    const auto int_of = [](double v) {
        if (std::isnan(v)) throw Error("value", "cannot convert float NaN to integer");
        if (std::isinf(v)) throw Error("overflow", "cannot convert float infinity to integer");
        return std::trunc(v);
    };
    const double fw = py_max(1.0, int_of((x1 - x0) * k) + 2);
    const double fh = py_max(1.0, int_of((y1 - y0) * k) + 2);
    // (a depth picture for the lines, at their own small resolution: one this big is a 3D far too large to trace)
    if (fw * fh > 64'000'000.0) throw Error("image_too_large", "the 3D is too large to draw at this resolution");
    const Raster depth = raster(meshy, static_cast<int>(fw), static_cast<int>(fh), dpi, camera, nullptr, 0.35, x0, y0);
    const double tolerance = py_max(0.8, 2.5 * 25.4 / dpi);  // (a pixel's worth of depth, and the rim of rounded parts)
    const float tolerance_f = static_cast<float>(tolerance);
    std::vector<Line2> out;
    for (const Seg& s : segs) {
        const double length = la::np_hypot(s.b[0] - s.a[0], s.b[1] - s.a[1]);
        const double fn = py_max(2.0, int_of(length * k * 1.5));
        const auto n = static_cast<std::int64_t>(fn);
        Line2 run;
        for (std::int64_t i = 0; i <= n; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(n);
            const Point2 p{s.a[0] + (s.b[0] - s.a[0]) * t, s.a[1] + (s.b[1] - s.a[1]) * t};
            const double z = s.za + (s.zb - s.za) * t;
            const double fpx = int_of((p[0] - x0) * k);
            const double fpy = int_of((p[1] - y0) * k);
            // zbuf[max(0, py - 1):py + 2, max(0, px - 1):px + 2] (Python's slices: a negative end counts from the end)
            const auto slice = [](double from, double to, int size) {
                const auto norm = [size](double v) {
                    if (v < 0) v += size;
                    return v < 0 ? 0.0 : (v > size ? static_cast<double>(size) : v);
                };
                return std::pair<int, int>{static_cast<int>(norm(from)), static_cast<int>(norm(to))};
            };
            const auto [r0, r1] = slice(py_max(0.0, fpy - 1), fpy + 2, depth.height);
            const auto [c0, c1] = slice(py_max(0.0, fpx - 1), fpx + 2, depth.width);
            bool visible = r1 <= r0 || c1 <= c0;  // (an empty slice)
            for (int r = r0; r < r1 && !visible; ++r) {
                for (int c = c0; c < c1; ++c) {
                    const float near = depth.zbuf[static_cast<std::size_t>(r) * static_cast<std::size_t>(depth.width) + static_cast<std::size_t>(c)];
                    // seen when some pixel beside it is empty or no nearer than it (the rim of a rounded part)
                    if (!std::isfinite(near) || z <= static_cast<double>(near + tolerance_f)) {
                        visible = true;
                        break;
                    }
                }
            }
            if (visible) {
                run.push_back(p);
            } else if (run.size() >= 2) {
                out.push_back(std::move(run));
                run.clear();
            } else {
                run.clear();
            }
        }
        if (run.size() >= 2) out.push_back(std::move(run));
    }
    return join_pieces(std::move(out));
}

std::shared_ptr<const std::vector<Line2>> prim_lines(const Json& prim, const Json* camera, double dpi) {
    camera = truthy(camera);
    Json own = Json::object();
    if (prim.is_object()) {
        for (const auto& [k, v] : prim.items()) {
            if (k != "camera") own[k] = v;
        }
    } else {
        pyv::raise_attribute(prim, "items");
    }
    Json key_value = Json::object({{"camera", camera != nullptr ? *camera : Json()}, {"prim", own}});
    char dpi_text[32];
    std::snprintf(dpi_text, sizeof dpi_text, "%a", dpi);
    const std::string key = dump_canonical(key_value) + "@" + dpi_text;
    LinesCache& cache = lines_cache();
    {
        std::lock_guard lock(cache.mutex);
        for (auto it = cache.items.begin(); it != cache.items.end(); ++it) {
            if (it->first == key) {
                cache.items.splice(cache.items.begin(), cache.items, it);
                return it->second;
            }
        }
    }
    const std::vector<Json> one{own};
    auto made = std::make_shared<const std::vector<Line2>>(lines(one, camera, dpi));
    std::lock_guard lock(cache.mutex);
    cache.items.emplace_front(key, made);
    while (cache.items.size() > 128) cache.items.pop_back();
    return made;
}

void clear_caches() {
    {
        FigureCache& cache = figure_cache();
        std::lock_guard lock(cache.mutex);
        cache.items.clear();
    }
    LinesCache& cache = lines_cache();
    std::lock_guard lock(cache.mutex);
    cache.items.clear();
}

// --- OBJ ------------------------------------------------------------------------------------------------------------

ObjError::ObjError(const std::string& message) : Error("obj", message) {}

namespace {

// One code point of UTF-8 text at `at` (and its length).
std::pair<std::uint32_t, std::size_t> code_point(std::string_view s, std::size_t at) {
    const auto c = static_cast<unsigned char>(s[at]);
    std::size_t n = 1;
    std::uint32_t cp = c;
    if (c >= 0xF0) {
        n = 4;
        cp = c & 0x07u;
    } else if (c >= 0xE0) {
        n = 3;
        cp = c & 0x0Fu;
    } else if (c >= 0xC0) {
        n = 2;
        cp = c & 0x1Fu;
    }
    for (std::size_t k = 1; k < n && at + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[at + k]) & 0x3Fu);
    return {cp, std::min(n, s.size() - at)};
}

// str.splitlines's line boundaries
bool line_break(std::uint32_t cp) {
    return cp == '\n' || cp == '\r' || cp == 0x0B || cp == 0x0C || cp == 0x1C || cp == 0x1D || cp == 0x1E || cp == 0x85 ||
           cp == 0x2028 || cp == 0x2029;
}

// str.split's whitespace (Py_UNICODE_ISSPACE)
bool space(std::uint32_t cp) {
    return (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x20) || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto [cp, n] = code_point(text, at);
        if (line_break(cp)) {
            out.push_back(text.substr(start, at - start));
            at += n;
            if (cp == '\r' && at < text.size() && text[at] == '\n') ++at;
            start = at;
            continue;
        }
        at += n;
    }
    if (start < text.size()) out.push_back(text.substr(start));
    return out;
}

std::vector<std::string_view> split_words(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    std::size_t start = std::string_view::npos;
    while (at < line.size()) {
        const auto [cp, n] = code_point(line, at);
        if (space(cp)) {
            if (start != std::string_view::npos) out.push_back(line.substr(start, at - start));
            start = std::string_view::npos;
        } else if (start == std::string_view::npos) {
            start = at;
        }
        at += n;
    }
    if (start != std::string_view::npos) out.push_back(line.substr(start));
    return out;
}

double text_float(std::string_view word) { return pyv::to_float(Json(std::string(word))); }

// A corner index of an "f" line (int(token.split("/")[0])); an int too large for 64 bits points nowhere.
std::optional<std::int64_t> text_int(std::string_view word) {
    const std::string head(word.substr(0, word.find('/')));
    try {
        return py_int(Json(head));
    } catch (const Error& error) {
        if (std::string_view(error.what()).find("too large") != std::string_view::npos) return std::nullopt;
        throw Error("value", error.what());
    }
}

// The model fitted into a unit box centred on 0 (read_obj, read_gltf): {"v", "f", "ratio"}.
Json fitted(const std::vector<Vec3>& verts, const std::vector<std::vector<std::int64_t>>& faces) {
    Vec3 lo = verts[0];
    Vec3 hi = verts[0];
    for (const Vec3& v : verts) {
        for (int c = 0; c < 3; ++c) {
            lo[c] = la::np_minimum(lo[c], v[c]);
            hi[c] = la::np_maximum(hi[c], v[c]);
        }
    }
    const Vec3 extent = hi - lo;
    double span = la::np_maximum(la::np_maximum(extent[0], extent[1]), extent[2]);
    if (span == 0.0) span = 1.0;  // (or 1.0)
    const Vec3 middle = (lo + hi) / 2;
    Json v = Json::array();
    for (const Vec3& p : verts) {
        for (int c = 0; c < 3; ++c) v.push_back(py_round((p[c] - middle[c]) / span, 5));
    }
    Json f = Json::array();
    for (const auto& face : faces) {
        Json item = Json::array();
        for (const std::int64_t i : face) item.push_back(i);
        f.push_back(std::move(item));
    }
    Json ratio = Json::array();
    for (int c = 0; c < 3; ++c) ratio.push_back(py_round(extent[c] / span, 4));
    return Json::object({{"v", std::move(v)}, {"f", std::move(f)}, {"ratio", std::move(ratio)}});
}

// The checks the C++ build adds once Python's own have passed (a model that cannot be drawn, or that would make the
// book enormous, is not taken in).
void check_model(const std::vector<Vec3>& verts, const std::vector<std::vector<std::int64_t>>& faces, bool check_corners) {
    if (static_cast<std::int64_t>(verts.size()) > kMaxVertices) {
        throw ObjError("the model has too many corners (at most " + std::to_string(kMaxVertices) + ")");
    }
    std::int64_t corners = 0;
    for (const auto& face : faces) {
        corners += static_cast<std::int64_t>(face.size());
        if (check_corners) {
            for (const std::int64_t i : face) {
                if (i < 0 || i >= static_cast<std::int64_t>(verts.size())) throw ObjError("the model file points at corners it does not have");
            }
        }
    }
    if (corners > kMaxCorners) throw ObjError("the model's faces have too many corners (at most " + std::to_string(kMaxCorners) + ")");
    for (const Vec3& v : verts) {
        for (const double c : v) {
            if (!std::isfinite(c)) throw ObjError("the model has a corner that is not a finite number");
        }
    }
}

}  // namespace

Json read_obj(std::string_view text) {
    std::vector<Vec3> verts;
    std::vector<std::vector<std::int64_t>> faces;
    const std::int64_t nowhere = std::numeric_limits<std::int64_t>::max();
    for (const std::string_view raw : split_lines(text)) {
        const std::vector<std::string_view> parts = split_words(raw);
        if (parts.empty()) continue;
        if (parts[0] == "v" && parts.size() >= 4) {
            const double x = text_float(parts[1]);
            const double y = text_float(parts[2]);
            const double z = text_float(parts[3]);
            verts.push_back(Vec3{x, -y, -z});
        } else if (parts[0] == "f" && parts.size() >= 4) {
            std::vector<std::int64_t> idx;
            for (std::size_t k = 1; k < parts.size(); ++k) {
                const std::optional<std::int64_t> i = text_int(parts[k]);
                if (!i) {
                    idx.push_back(nowhere);
                } else {
                    idx.push_back(*i > 0 ? *i - 1 : static_cast<std::int64_t>(verts.size()) + *i);
                }
            }
            faces.push_back(std::move(idx));
        }
    }
    if (verts.empty() || faces.empty()) throw ObjError("the OBJ file has no faces");
    if (static_cast<std::int64_t>(faces.size()) > kMaxFaces) {
        throw ObjError("the model has too many faces (at most " + std::to_string(kMaxFaces) + ")");
    }
    for (const auto& face : faces) {
        for (const std::int64_t i : face) {
            if (!(0 <= i && i < static_cast<std::int64_t>(verts.size()))) throw ObjError("the OBJ file points at corners it does not have");
        }
    }
    check_model(verts, faces, false);
    return fitted(verts, faces);
}

// --- glTF / GLB (VRM is a GLB) ---------------------------------------------------------------------------------------

namespace {

std::uint32_t u32(std::string_view data, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(data[at])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[at + 1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[at + 2])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(data[at + 3])) << 24;
}

// json.loads(bytes.decode("utf-8"))
Json json_text(std::string_view bytes) {
    if (auto problem = utf8_error(bytes)) throw Error("value", *problem);
    try {
        return parse_python_json(bytes);
    } catch (const Error& error) {
        throw Error("value", error.what());
    }
}

// container[key] for a JSON list or dict, as Python indexes them.
const Json& item(const Json& container, const Json& key) {
    if (container.is_array()) {
        if (!(key.is_number_integer() || key.is_number_unsigned() || key.is_boolean())) {
            throw Error("type", "list indices must be integers or slices, not " + py_type_name(key));
        }
        const std::int64_t i = key.is_boolean() ? (key.get<bool>() ? 1 : 0) : py_int(key);
        const auto n = static_cast<std::int64_t>(container.size());
        const std::int64_t at = i < 0 ? i + n : i;
        if (at < 0 || at >= n) pyv::raise_index();
        return container[static_cast<std::size_t>(at)];
    }
    if (container.is_object()) {
        if (key.is_array() || key.is_object()) throw Error("type", "unhashable type: '" + py_type_name(key) + "'");
        if (key.is_string()) {
            const auto it = container.find(key.get_ref<const std::string&>());
            if (it != container.end()) return *it;
        }
        throw OpKeyError(py_repr(key));
    }
    if (container.is_string()) throw Error("type", "string indices must be integers, not '" + py_type_name(key) + "'");
    throw Error("type", "'" + py_type_name(container) + "' object is not subscriptable");
}

// container["key"]
const Json& field(const Json& container, const char* key) { return item(container, Json(key)); }

// `key in container`
bool contains(const Json& container, std::string_view key) {
    if (container.is_object()) return container.contains(key);
    if (container.is_array()) {
        return std::any_of(container.begin(), container.end(), [&](const Json& v) { return pyv::eq(v, Json(std::string(key))); });
    }
    if (container.is_string()) return container.get_ref<const std::string&>().find(key) != std::string::npos;
    throw Error("type", "argument of type '" + py_type_name(container) + "' is not iterable");
}

struct Component {
    char format;
    int size;
};

Component component(const Json& type) {
    if (type.is_array() || type.is_object()) throw Error("type", "unhashable type: '" + py_type_name(type) + "'");
    if (type.is_number() && !type.is_boolean()) {
        const double v = type.get<double>();
        for (const auto& [code, c] : {std::pair<double, Component>{5120, {'b', 1}}, {5121, {'B', 1}}, {5122, {'h', 2}},
                                      {5123, {'H', 2}}, {5125, {'I', 4}}, {5126, {'f', 4}}}) {
            if (v == code) return c;
        }
    }
    throw OpKeyError(py_repr(type));
}

int width_of(const Json& type) {
    if (type.is_array() || type.is_object()) throw Error("type", "unhashable type: '" + py_type_name(type) + "'");
    if (type.is_string()) {
        const std::string& t = type.get_ref<const std::string&>();
        if (t == "SCALAR") return 1;
        if (t == "VEC2") return 2;
        if (t == "VEC3") return 3;
        if (t == "VEC4") return 4;
    }
    throw OpKeyError(py_repr(type));
}

double element(const std::string& raw, std::size_t at, Component c) {
    const auto byte = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<unsigned char>(raw[at + k])); };
    switch (c.format) {
        case 'b': return static_cast<double>(static_cast<std::int8_t>(byte(0)));
        case 'B': return static_cast<double>(byte(0));
        case 'h': return static_cast<double>(static_cast<std::int16_t>(byte(0) | byte(1) << 8));
        case 'H': return static_cast<double>(byte(0) | byte(1) << 8);
        case 'I': return static_cast<double>(byte(0) | byte(1) << 8 | byte(2) << 16 | byte(3) << 24);
        default: {
            const std::uint32_t bits = byte(0) | byte(1) << 8 | byte(2) << 16 | byte(3) << 24;
            float f = 0.0f;
            std::memcpy(&f, &bits, sizeof f);
            return static_cast<double>(f);
        }
    }
}

struct Accessor {
    std::int64_t count = 0;
    int width = 0;
    std::vector<double> values;  // count × width, row by row
};

Accessor accessor(const Json& doc, const std::vector<std::string>& buffers, const Json& index) {
    const Json& acc = item(field(doc, "accessors"), index);
    const Json& view = item(field(doc, "bufferViews"), field(acc, "bufferView"));
    const Component c = component(field(acc, "componentType"));
    const int width = width_of(field(acc, "type"));
    const auto get_int = [](const Json& obj, const char* key, std::int64_t fallback) {
        const Json* v = pyv::get(obj, key);
        return v != nullptr ? pyv::to_int(*v) : fallback;
    };
    const std::int64_t start = get_int(view, "byteOffset", 0) + get_int(acc, "byteOffset", 0);
    std::int64_t stride = get_int(view, "byteStride", 0);
    if (stride == 0) stride = c.size * width;
    const Json* buffer_index = pyv::get(view, "buffer");
    static const Json kFirst = 0;
    const Json buffer_list = [&] {  // (buffers[view.get("buffer", 0)], indexed as Python indexes a list)
        Json list = Json::array();
        for (std::size_t i = 0; i < buffers.size(); ++i) list.push_back(static_cast<std::int64_t>(i));
        return list;
    }();
    const std::size_t which = static_cast<std::size_t>(item(buffer_list, buffer_index != nullptr ? *buffer_index : kFirst).get<std::int64_t>());
    const std::string& data = buffers[which];
    const std::int64_t count = pyv::to_int(field(acc, "count"));
    Accessor out;
    out.width = width;
    // values.reshape(count, width): a negative count is numpy's unknown dimension (as many rows as the values make)
    const auto rows = [&](std::int64_t items) {
        if (count >= 0) return count;
        if (items % width != 0) {
            throw Error("value", "cannot reshape array of size " + std::to_string(items) + " into shape (" + std::to_string(width) + ")");
        }
        return items / width;
    };
    const auto length = static_cast<std::int64_t>(data.size());
    if (stride == static_cast<std::int64_t>(c.size) * width) {
        // np.frombuffer(raw, dtype, count=count * width, offset=start): a negative count reads to the end of the buffer
        if (count < std::numeric_limits<std::int64_t>::min() / width) {
            // (Python stops with an OverflowError that import_model does not catch: refused here)
            throw Error("overflow", "Python int too large to convert to C ssize_t");
        }
        if (start < 0 || start > length) {
            throw Error("value", "offset must be non-negative and no greater than buffer length (" + std::to_string(length) + ")");
        }
        std::int64_t items = 0;
        if (count < 0) {
            if ((length - start) % c.size != 0) throw Error("value", "buffer size must be a multiple of element size");
            items = (length - start) / c.size;
        } else {
            const std::int64_t room = (length - start) / c.size;
            if (count > room / width) throw Error("value", "buffer is smaller than requested size");
            items = count * width;
        }
        out.count = rows(items);
        out.values.reserve(static_cast<std::size_t>(items));
        for (std::int64_t i = 0; i < items; ++i) {
            out.values.push_back(element(data, static_cast<std::size_t>(start + i * c.size), c));
        }
        return out;
    }
    // (struct.unpack_from row by row: a row outside the buffer is an error in Python too; range(count) is empty for a
    // negative count)
    out.count = rows(0);
    for (std::int64_t i = 0; i < count; ++i) {
        const std::int64_t at = start + i * stride;
        if (at < 0 || at + static_cast<std::int64_t>(c.size) * width > length) {
            throw Error("value", "unpack_from requires a buffer of at least " + std::to_string(at + c.size * width) + " bytes");
        }
        for (int k = 0; k < width; ++k) out.values.push_back(element(data, static_cast<std::size_t>(at + k * c.size), c));
    }
    return out;
}

// A Python number for the quaternion's arithmetic (an int stays an int).
Num number(const Json& v) {
    if (v.is_boolean()) return Num(v.get<bool>() ? 1 : 0);
    if (const auto n = Num::from_json(v)) return *n;
    throw Error("type", v.is_string() || v.is_array() ? "can't multiply sequence by non-int of type '" + py_type_name(v) + "'"
                                                       : "unsupported operand type(s) for *: '" + py_type_name(v) + "' and '" +
                                                             py_type_name(v) + "'");
}

// np.array(values, dtype=float) of a flat list (numbers, booleans, numeric text)
std::vector<double> floats(const Json& list) {
    std::vector<double> out;
    for (const Json& v : py_list(list)) {
        if (v.is_array()) {
            for (const double x : floats(v)) out.push_back(x);
        } else {
            out.push_back(v.is_null() ? std::nan("") : pyv::to_float(v));
        }
    }
    return out;
}

la::Mat4 node_matrix(const Json& node) {
    if (const Json* matrix = pyv::get(node, "matrix"); matrix != nullptr && py_truthy(*matrix)) {
        const std::vector<double> values = floats(*matrix);
        if (values.size() != 16) {
            throw Error("value", "cannot reshape array of size " + std::to_string(values.size()) + " into shape (4,4)");
        }
        la::Mat4 m{};
        for (int r = 0; r < 4; ++r) {
            for (int col = 0; col < 4; ++col) m[r][col] = values[static_cast<std::size_t>(col * 4 + r)];  // (.T)
        }
        return m;
    }
    la::Mat4 m = la::identity4();
    static const Json kNoMove = Json::array({0, 0, 0});
    static const Json kNoTurn = Json::array({0, 0, 0, 1});
    static const Json kNoScale = Json::array({1, 1, 1});
    const Json t = pyv::get_or(node, "translation", kNoMove);
    const Json q = py_list(pyv::get_or(node, "rotation", kNoTurn));
    if (q.size() != 4) {
        throw Error("value", q.size() > 4 ? "too many values to unpack (expected 4)"
                                          : "not enough values to unpack (expected 4, got " + std::to_string(q.size()) + ")");
    }
    const Num x = number(q[0]);
    const Num y = number(q[1]);
    const Num z = number(q[2]);
    const Num w = number(q[3]);
    const Num one(1);
    const Num two(2);
    const Num rot[3][3] = {{one - two * (y * y + z * z), two * (x * y - z * w), two * (x * z + y * w)},
                           {two * (x * y + z * w), one - two * (x * x + z * z), two * (y * z - x * w)},
                           {two * (x * z - y * w), two * (y * z + x * w), one - two * (x * x + y * y)}};
    const Json s = py_list(pyv::get_or(node, "scale", kNoScale));
    if (s.size() != 3 && s.size() != 1) {
        throw Error("value", "operands could not be broadcast together with shapes (3,3) (" + std::to_string(s.size()) + ",) ");
    }
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 3; ++col) m[r][col] = (rot[r][col] * number(s[s.size() == 1 ? 0 : col])).value();
    }
    const std::vector<double> move = floats(t);
    if (move.size() != 3 && move.size() != 1) {
        throw Error("value", "could not broadcast input array from shape (" + std::to_string(move.size()) + ",) into shape (3,)");
    }
    for (int r = 0; r < 3; ++r) m[r][3] = move[move.size() == 1 ? 0 : static_cast<std::size_t>(r)];
    return m;
}

struct GltfReader {
    const Json& doc;
    const std::vector<std::string>& buffers;
    std::vector<Vec3> verts;
    std::vector<std::vector<std::int64_t>> faces;
    std::vector<std::int64_t> path;  // the nodes being visited (a node inside itself is a loop)
    std::int64_t visits = 0;

    void visit(const Json& index, const la::Mat4& parent) {
        const Json& nodes = field(doc, "nodes");
        const Json& node = item(nodes, index);
        const std::int64_t id = index.is_number() || index.is_boolean() ? py_int(index) : -1;
        const std::int64_t at = id < 0 && nodes.is_array() ? id + static_cast<std::int64_t>(nodes.size()) : id;
        if (std::find(path.begin(), path.end(), at) != path.end()) throw ObjError("the model's nodes contain themselves");
        if (path.size() >= 900) throw ObjError("the model's nodes are too deep");
        if (++visits > 100000) throw ObjError("the model has too many nodes");
        path.push_back(at);
        const la::Mat4 m = la::matmul(parent, node_matrix(node));
        const Json* mesh = pyv::get(node, "mesh");
        if (mesh != nullptr && !mesh->is_null()) {
            static const Json kNone = Json::array();
            const Json& primitives = pyv::get_or(item(field(doc, "meshes"), *mesh), "primitives", kNone);
            for (const Json& primitive : py_list(primitives)) {
                const Json* mode = pyv::get(primitive, "mode");
                static const Json kNoAttributes = Json::object();
                if (!pyv::eq(mode != nullptr ? *mode : Json(4), Json(4)) ||
                    !contains(pyv::get_or(primitive, "attributes", kNoAttributes), "POSITION")) {
                    continue;
                }
                const Accessor pos = accessor(doc, buffers, field(field(primitive, "attributes"), "POSITION"));
                if (pos.width + 1 != 4) {
                    throw Error("value", "matmul: Input operand 1 has a mismatch in its core dimension 0, with gufunc signature "
                                         "(n?,k),(k,m?)->(n?,m?) (size 4 is different from " + std::to_string(pos.width + 1) + ")");
                }
                std::vector<la::Vec4> rows;
                rows.reserve(static_cast<std::size_t>(pos.count));
                for (std::int64_t i = 0; i < pos.count; ++i) {
                    const auto r = static_cast<std::size_t>(i * 3);
                    rows.push_back(la::Vec4{pos.values[r], pos.values[r + 1], pos.values[r + 2], 1.0});
                }
                const std::vector<la::Vec4> world = la::rows_mt(rows, m);
                const auto base = static_cast<std::int64_t>(verts.size());
                std::vector<std::int64_t> idx;
                const Json* indices = pyv::get(primitive, "indices");
                if (indices != nullptr && !indices->is_null()) {
                    const Accessor ix = accessor(doc, buffers, *indices);
                    for (const double v : ix.values) {
                        // (astype(int): toward zero; what is not a number has no corner)
                        idx.push_back(std::isfinite(v) && std::fabs(v) < 9.2e18 ? static_cast<std::int64_t>(v)
                                                                                 : std::numeric_limits<std::int64_t>::min());
                    }
                } else {
                    for (std::int64_t i = 0; i < pos.count; ++i) idx.push_back(i);
                }
                for (const la::Vec4& p : world) verts.push_back(Vec3{p[0], p[1], p[2]});
                for (std::size_t k = 0; k + 2 < idx.size(); k += 3) faces.push_back({base + idx[k], base + idx[k + 1], base + idx[k + 2]});
                if (static_cast<std::int64_t>(faces.size()) > kMaxFaces) {
                    throw ObjError("the model has too many faces (at most " + std::to_string(kMaxFaces) + ")");
                }
                if (static_cast<std::int64_t>(verts.size()) > kMaxVertices) {
                    throw ObjError("the model has too many corners (at most " + std::to_string(kMaxVertices) + ")");
                }
            }
        }
        static const Json kNoChildren = Json::array();
        for (const Json& child : py_list(pyv::get_or(node, "children", kNoChildren))) visit(child, m);
        path.pop_back();
    }
};

}  // namespace

Json read_gltf(std::string_view data) {
    Json doc;
    std::vector<std::string> buffers;
    if (data.substr(0, 4) == "glTF") {
        if (data.size() < 20) throw ObjError("the model file is cut short");
        const std::uint32_t total = u32(data, 8);
        std::size_t at = 12;
        bool found = false;
        std::string blob;
        const std::size_t end = std::min<std::size_t>(total, data.size());
        while (at + 8 <= end) {
            const std::uint32_t length = u32(data, at);
            const std::uint32_t kind = u32(data, at + 4);
            const std::string_view chunk = data.substr(at + 8, length);
            if (kind == 0x4E4F534A) {
                doc = json_text(chunk);
                found = true;
            } else if (kind == 0x004E4942) {
                blob = std::string(chunk);
            }
            at += 8 + static_cast<std::size_t>(length);
        }
        if (!found || doc.is_null()) throw ObjError("the model file has no scene");
        buffers.push_back(std::move(blob));
    } else {
        try {
            doc = json_text(data);
        } catch (const Error&) {
            throw ObjError("the model file is not glTF");
        }
        if (!doc.is_object()) throw ObjError("the model file is not glTF");
        static const Json kNone = Json::array();
        for (const Json& buffer : py_list(pyv::get_or(doc, "buffers", kNone))) {
            const Json* uri_value = pyv::get(buffer, "uri");
            const std::string uri = uri_value != nullptr && py_truthy(*uri_value) ? py_str(*uri_value) : std::string();
            if (!uri.starts_with("data:")) throw ObjError("a .gltf must carry its data inside (or use .glb)");
            const std::size_t comma = uri.find(',');
            if (comma == std::string::npos) pyv::raise_index();
            try {
                buffers.push_back(a2b_base64(std::string_view(uri).substr(comma + 1)));
            } catch (const Error& error) {
                throw Error("value", error.what());
            }
        }
    }
    if (!doc.is_object()) throw ObjError("the model file is not glTF");
    GltfReader reader{doc, buffers, {}, {}, {}, 0};
    const Json* scenes = pyv::get(doc, "scenes");
    Json fallback = Json::array();
    if (!(scenes != nullptr && py_truthy(*scenes))) {
        const Json* nodes = pyv::get(doc, "nodes");
        Json all = Json::array();
        const std::size_t n = nodes != nullptr && py_truthy(*nodes) ? pyv::len(*nodes) : 0;
        for (std::size_t i = 0; i < n; ++i) all.push_back(static_cast<std::int64_t>(i));
        fallback.push_back(Json::object({{"nodes", all}}));
    }
    const Json& scene_list = scenes != nullptr && py_truthy(*scenes) ? *scenes : fallback;
    const Json* scene_index = pyv::get(doc, "scene");
    const Json which = pyv::to_int(scene_index != nullptr ? *scene_index : Json(0));
    const Json& scene = item(scene_list, which);
    static const Json kNoRoots = Json::array();
    for (const Json& root : py_list(pyv::get_or(scene, "nodes", kNoRoots))) reader.visit(root, la::identity4());
    if (reader.verts.empty() || reader.faces.empty()) throw ObjError("the OBJ file has no faces");
    std::vector<Vec3> v = reader.verts;
    for (Vec3& p : v) p = p * Vec3{1.0, -1.0, -1.0};  // (glTF: y up, z toward the viewer)
    check_model(v, reader.faces, true);
    return fitted(v, reader.faces);
}

// --- the figure's handles (threeops) ----------------------------------------------------------------------------------

namespace {

struct Handle {
    std::string_view name;
    std::string_view joint;
    std::string_view start;
    std::string_view end;
};

constexpr Handle kHandles[] = {
    {"neck", "spine", "pelvis", "neck"},           {"head", "neck", "neck", "head"},
    {"l_elbow", "l_arm", "l_shoulder", "l_elbow"}, {"r_elbow", "r_arm", "r_shoulder", "r_elbow"},
    {"l_wrist", "l_elbow", "l_elbow", "l_wrist"},  {"r_wrist", "r_elbow", "r_elbow", "r_wrist"},
    {"l_hand", "l_wrist", "l_wrist", "l_hand"},    {"r_hand", "r_wrist", "r_wrist", "r_hand"},
    {"l_knee", "l_leg", "l_hip", "l_knee"},        {"r_knee", "r_leg", "r_hip", "r_knee"},
    {"l_ankle", "l_knee", "l_knee", "l_ankle"},    {"r_ankle", "r_knee", "r_knee", "r_ankle"},
    {"l_toe", "l_ankle", "l_ankle", "l_toe"},      {"r_toe", "r_ankle", "r_ankle", "r_toe"},
};

struct Chain {
    std::string_view handle;
    std::string_view upper;
    std::string_view lower;
};

// IK_CHAINS: the upper joint turns about x and z, the lower about x
constexpr Chain kChains[] = {
    {"l_wrist", "l_arm", "l_elbow"}, {"l_hand", "l_arm", "l_elbow"}, {"r_wrist", "r_arm", "r_elbow"},
    {"r_hand", "r_arm", "r_elbow"},  {"l_ankle", "l_leg", "l_knee"}, {"l_toe", "l_leg", "l_knee"},
    {"r_ankle", "r_leg", "r_knee"},  {"r_toe", "r_leg", "r_knee"},
};

std::string names_of_handles() {
    std::string out;
    for (const Handle& h : kHandles) out += (out.empty() ? "" : ", ") + std::string(h.name);
    return out;
}

// {k: dict(v) for k, v in (prim.get("joints") or {}).items()}
Json copied_joints(const Json& prim) {
    static const Json kNone = Json::object();
    const Json& joints = pyv::get_or(prim, "joints", kNone);
    if (!joints.is_object()) pyv::raise_attribute(joints, "items");
    Json out = Json::object();
    for (const auto& [k, v] : joints.items()) out[k] = py_dict(v);
    return out;
}

Json with_joints(const Json& prim, Json joints) {
    Json out = prim;
    out["joints"] = std::move(joints);
    return out;
}

}  // namespace

Point2 page_of_joint(const Json& prim, std::string_view name, const Json* camera) {
    const Skeleton sk = figure_skeleton(prim);
    const Vec3 p = sk.point(name);
    return to_page(prim, std::span<const Vec3>(&p, 1), camera).pts[0];
}

Json drag_joint(const Json& prim, std::string_view handle, Point2 target, const Json* camera) {
    if (handle == "pelvis") {
        static const Json kDefault = Json::array({100, 160, 0});
        Json pos = py_list(pyv::get_or(prim, "pos", kDefault));
        for (int i = 0; i < 3; ++i) pos.push_back(0);
        return Json::object({{"pos", Json::array({py_round(target[0], 3), py_round(target[1], 3), pos[2]})}});
    }
    const Handle* h = nullptr;
    for (const Handle& item : kHandles) {
        if (item.name == handle) h = &item;
    }
    if (h == nullptr) throw OpError("handle must be pelvis or one of " + names_of_handles());
    const std::string joint(h->joint);
    const double goal_y = target[1] - page_of_joint(prim, h->start, camera)[1];
    const double goal_x = target[0] - page_of_joint(prim, h->start, camera)[0];
    const double goal = py_atan2(goal_y, goal_x);
    const auto angle = [&](const Json& p) {
        const Point2 a = page_of_joint(p, h->start, camera);
        const Point2 b = page_of_joint(p, h->end, camera);
        return py_atan2(b[1] - a[1], b[0] - a[0]);
    };
    const auto with_value = [&](const std::string& axis, double value) {
        Json joints = copied_joints(prim);
        if (!joints.contains(joint)) joints[joint] = Json::object();
        joints[joint][axis] = value;
        return with_joints(prim, std::move(joints));
    };
    static const Json kNone = Json::object();
    const Json& joints = pyv::get_or(prim, "joints", kNone);
    const Json* own = pyv::get(joints, joint);
    const Json current = py_dict(own != nullptr && py_truthy(*own) ? *own : kNone);
    const auto axis_value = [&](const std::string& axis) {
        const Json* v = pyv::get(current, axis);
        return v != nullptr ? pyv::to_float(*v) : 0.0;
    };
    std::string best_axis = "z";
    double best_slope = 0.0;
    for (const std::string axis : {"z", "x"}) {
        const double base = axis_value(axis);
        const double slope = la::py_remainder(angle(with_value(axis, base + 0.05)) - angle(with_value(axis, base)), la::kTau) / 0.05;
        if (std::fabs(slope) > std::fabs(best_slope)) {
            best_axis = axis;
            best_slope = slope;
        }
    }
    double value = axis_value(best_axis);
    for (int i = 0; i < 6; ++i) {
        const double now = angle(with_value(best_axis, value));
        const double miss = la::py_remainder(goal - now, la::kTau);
        if (std::fabs(miss) < 0.002) break;
        const double slope = la::py_remainder(angle(with_value(best_axis, value + 0.02)) - now, la::kTau) / 0.02;
        if (std::fabs(slope) < 1e-4) break;
        const double step = miss / slope;
        value += py_max(-0.8, py_min(0.8, step));
    }
    Json change = Json::object();
    change[best_axis] = py_round(la::py_remainder(value, la::kTau), 4);
    return Json::object({{"joints", Json::object({{joint, change}})}});
}

Json reach(const Json& prim, std::string_view handle, Point2 target, const Json* camera) {
    const Chain* chain = nullptr;
    for (const Chain& c : kChains) {
        if (c.handle == handle) chain = &c;
    }
    if (chain == nullptr) {
        std::string names;
        for (const Chain& c : kChains) names += (names.empty() ? "" : ", ") + std::string(c.handle);
        throw OpError("ik moves " + names);
    }
    const std::string upper(chain->upper);
    const std::string lower(chain->lower);
    const std::string end(chain->handle);
    const Json joints = copied_joints(prim);
    const auto axis_of = [&](const std::string& joint, const char* axis) {
        const Json* j = pyv::get(joints, joint);
        static const Json kNone = Json::object();
        const Json& slot = j != nullptr && py_truthy(*j) ? *j : kNone;
        const Json* v = pyv::get(slot, axis);
        return v != nullptr ? pyv::to_float(*v) : 0.0;
    };
    const persp3d::Vector start{axis_of(upper, "x"), axis_of(upper, "z"), axis_of(lower, "x")};
    const Point2 goal{target[0], target[1]};
    const double knee_sign = lower.ends_with("knee") ? -1.0 : 1.0;  // (knees bend backward, elbows forward)
    const auto pose = [&](const persp3d::Vector& v) {
        Json moved = joints;
        if (!moved.contains(upper)) moved[upper] = Json::object();
        moved[upper]["x"] = v[0];
        moved[upper]["z"] = v[1];
        if (!moved.contains(lower)) moved[lower] = Json::object();
        moved[lower]["x"] = v.back();
        return with_joints(prim, std::move(moved));
    };
    const auto cost = [&](const persp3d::Vector& v) {
        const Point2 at = page_of_joint(pose(v), end, camera);
        const double miss = la::norm2(at[0] - goal[0], at[1] - goal[1]);
        const double bend = -knee_sign * v.back();
        const double bent = bend > 0.0 ? bend : 0.0;  // (the elbow or knee bent the wrong way)
        double sum = 0.0;
        for (std::size_t i = 0; i < v.size(); ++i) {
            const double d = v[i] - start[i];
            sum = sum + d * d;
        }
        return miss + 0.02 * sum + 40.0 * bent;
    };
    persp3d::Vector best = persp3d::minimize(cost, start, 300);
    for (const persp3d::Vector& guess : {persp3d::Vector{start[0] + 0.6, start[1], start[2] + knee_sign * 0.8},
                                         persp3d::Vector{start[0] - 0.6, start[1], start[2] + knee_sign * 0.4}}) {
        const persp3d::Vector found = persp3d::minimize(cost, guess, 300);
        if (cost(found) < cost(best)) best = found;
    }
    Json out = Json::object();
    out[upper] = Json::object({{"x", py_round(la::py_remainder(best[0], la::kTau), 4)}, {"z", py_round(la::py_remainder(best[1], la::kTau), 4)}});
    out[lower] = Json::object({{"x", py_round(la::py_remainder(best.back(), la::kTau), 4)}});
    return Json::object({{"joints", std::move(out)}});
}

std::vector<std::pair<std::string, Point2>> figure_handles(const Json& prim, const Json* camera) {
    const Skeleton sk = figure_skeleton(prim);
    std::vector<std::string> names{"pelvis"};
    for (const Handle& h : kHandles) names.emplace_back(h.name);
    std::vector<Vec3> pts;
    for (const std::string& n : names) pts.push_back(sk.point(n));
    const OnPage at = to_page(prim, pts, camera);
    std::vector<std::pair<std::string, Point2>> out;
    for (std::size_t i = 0; i < names.size(); ++i) out.emplace_back(names[i], at.pts[i]);
    return out;
}

}  // namespace genko::core::mesh3d
