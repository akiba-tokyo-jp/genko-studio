#include "core/pose_checks.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <tuple>

#include "core/figure_shape.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/pose_library.hpp"

namespace genko::core::pose_checks {

namespace {

using la::Mat3;
using namespace genko::core::la;  // (the element-wise operators on Vec3)

const std::vector<JointRange> kRanges = {
    // (radians; the middle and the left; the right is the mirror)
    {"hip", {-1.7, 1.7}, {-3.15, 3.15}, {-0.8, 0.8}},       // (the whole body about the pelvis: a lean, a back view)
    {"spine", {-1.0, 0.6}, {-0.8, 0.8}, {-0.6, 0.6}},
    {"chest", {-0.6, 0.5}, {-0.6, 0.6}, {-0.5, 0.5}},
    {"neck", {-0.8, 0.7}, {-0.9, 0.9}, {-0.5, 0.5}},
    {"head", {-0.8, 0.7}, {-1.0, 1.0}, {-0.5, 0.5}},
    {"l_elbow", {-0.1, 2.6}, {-1.8, 1.8}, {-0.9, 0.9}},
    {"l_wrist", {-0.6, 0.6}, {-0.6, 0.6}, {-1.4, 1.2}},   // (z: toward the palm −, the back +)
    {"l_knee", {-2.85, 0.05}, {-0.6, 0.6}, {-0.3, 0.3}},
    {"l_ankle", {-1.6, 0.75}, {-0.6, 0.6}, {-0.5, 0.5}},  // (x −: the foot in line with the shin, as in seiza — the foot is one piece)
};

// The shoulder and the hip turn every way: what they may reach is a cone about the bone's rest (down), widest forward,
// and a twist about the bone (their Euler angles are no measure: at 90° of flexion z no longer spreads the knees).
const std::vector<BallRange> kBalls = {
    // forward, outward, back, inward (the most the bone may rise from hanging down, radians), the twist (outward +) and
    // the rise it is measured to
    {"l_arm", 3.2, 3.15, 1.2, 1.6, {-1.6, 1.9}, 2.2},
    {"l_leg", 2.6, 1.3, 0.8, 0.6, {-0.9, 1.0}, 2.9},
};

const char* const kFloorContacts[] = {"l_foot", "r_foot", "l_knee", "r_knee", "l_hand", "r_hand", "seat", "pelvis"};
const char* const kContacts[] = {"l_foot", "r_foot", "l_knee", "r_knee", "l_hand", "r_hand", "seat", "pelvis", "back", "front", "side"};
const char* const kPoseKeys[] = {"id", "category", "ja", "en", "tags", "joints", "hands", "hold", "support", "seat_h", "contact", "symmetric"};
const char* const kPairKeys[] = {"id", "category", "ja", "en", "tags", "figures", "touch", "symmetric"};
const char* const kPersonKeys[] = {"joints", "hands", "hold", "support", "seat_h", "contact", "offset", "turn"};

bool has(const Json& list, std::string_view want) {
    return list.is_array() && std::any_of(list.begin(), list.end(), [&](const Json& v) { return v.is_string() && v.get_ref<const std::string&>() == want; });
}

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

bool places(double v, int n) {
    const double k = std::pow(10.0, n);
    return std::fabs(std::round(v * k) / k - v) <= 1e-12 * std::max(1.0, std::fabs(v));
}

bool ascii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

bool good_name(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

double dist_seg(const Vec3& p, const Vec3& a, const Vec3& b, double* t_out = nullptr) {
    const Vec3 ab = b - a;
    const double l2 = dot(ab, ab);
    double t = l2 > 0.0 ? dot(p - a, ab) / l2 : 0.0;
    t = std::max(0.0, std::min(1.0, t));
    if (t_out != nullptr) *t_out = t;
    return norm(p - (a + ab * t));
}

// The hand's reach from the wrist (u): shorter as the fingers close
double hand_length(const Json& hands, const char* side) {
    const Json* pose = dict_get(hands, side);
    static const Json kRelaxed = "relaxed";
    const mesh3d::HandShape shape = mesh3d::hand_shape(pose != nullptr && py_truthy(*pose) ? *pose : kRelaxed);
    const double mean = (shape.curls[1] + shape.curls[2] + shape.curls[3] + shape.curls[4]) / 4.0;
    return 0.8 * (1.0 - 0.45 * mean);
}

// The bones the surface's points are told apart by, and the limbs' tapered capsules (u: the body's own girths at each
// end, measured on figure_shape's body — a measure, not a threshold: taken again when the body's forms change, last
// with the anatomy pass of 2026-11-16)
struct Bone {
    std::string name;
    Vec3 a;
    Vec3 b;
    double ra;  // (radius at a, at b: u)
    double rb;
    int chain;  // −1 the body and the head; 0 l arm, 1 r arm, 2 l leg, 3 r leg
    double from = 0;  // (the part of the limb checked against the torso: from here to its end)
};

std::vector<Bone> bones_of(const mesh3d::Skeleton& sk, const Json& prim) {
    const double u = sk.unit;
    static const Json kNone = Json::object();
    const Json& hands = get_else(prim, "hands", kNone);
    std::vector<Bone> out;
    const Vec3& head_base = sk.point("head_base");
    const Vec3& head = sk.point("head");
    out.push_back({"torso_low", sk.point("pelvis"), sk.point("waist"), 0.55, 0.55, -1});
    out.push_back({"torso_high", sk.point("waist"), sk.point("neck"), 0.6, 0.6, -1});
    out.push_back({"neck", sk.point("neck"), head_base, 0.27, 0.27, -1});
    out.push_back({"head", head_base, head + (head - head_base) * 0.5, 0.48, 0.48, -1});
    for (int side = 0; side < 2; ++side) {
        const std::string s = side == 0 ? "l" : "r";
        const Vec3& wrist = sk.point(s + "_wrist");
        const Vec3 along = (sk.point(s + "_hand") - wrist) / norm(sk.point(s + "_hand") - wrist);
        const Mat3& foot = sk.turn(s + "_ankle");
        const Vec3& ankle = sk.point(s + "_ankle");
        out.push_back({s + "_upper", sk.point(s + "_shoulder"), sk.point(s + "_elbow"), 0.235, 0.165, side, 0.45});
        out.push_back({s + "_fore", sk.point(s + "_elbow"), wrist, 0.165, 0.1, side, 0.0});
        out.push_back({s + "_hand", wrist, wrist + along * (hand_length(hands, s.c_str()) * u), 0.06, 0.045, side, 0.0});  // (the palm's thickness)
        out.push_back({s + "_thigh", sk.point(s + "_hip"), sk.point(s + "_knee"), 0.4, 0.245, 2 + side, 0.55});
        out.push_back({s + "_shin", sk.point(s + "_knee"), ankle, 0.245, 0.142, 2 + side, 0.0});
        out.push_back({s + "_foot", ankle + matvec(foot, Vec3{0.0, 0.17 * u, 0.1 * u}), ankle + matvec(foot, Vec3{0.0, 0.25 * u, -0.85 * u}), 0.12, 0.09,
                       2 + side, 0.0});
    }
    return out;
}

// The bone each point of the surface belongs to (the nearest, less its radius)
std::vector<int> regions(const std::vector<Vec3>& v, const std::vector<Bone>& bones, double u) {
    std::vector<int> out(v.size(), 0);
    for (std::size_t i = 0; i < v.size(); ++i) {
        double best = std::numeric_limits<double>::infinity();
        for (std::size_t b = 0; b < bones.size(); ++b) {
            double t = 0;
            const double d = dist_seg(v[i], bones[b].a, bones[b].b, &t);
            const double r = (bones[b].ra + (bones[b].rb - bones[b].ra) * t) * u;
            if (d - r < best) {
                best = d - r;
                out[i] = static_cast<int>(b);
            }
        }
    }
    return out;
}

int bone_index(const std::vector<Bone>& bones, const std::string& name) {
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

// Whether point i of the surface is part of the contact `name`
bool in_contact(const std::string& name, const Vec3& p, int region, const std::vector<Bone>& bones, const mesh3d::Skeleton& sk) {
    const double u = sk.unit;
    if (name == "l_foot" || name == "r_foot") return region == bone_index(bones, name);
    if (name == "l_hand" || name == "r_hand") return region == bone_index(bones, name);
    if (name == "l_knee" || name == "r_knee") {
        const std::string s = name.substr(0, 1);
        if (region != bone_index(bones, s + "_thigh") && region != bone_index(bones, s + "_shin")) return false;
        return norm(p - sk.point(s + "_knee")) <= 0.45 * u;
    }
    if (name == "pelvis") return region == bone_index(bones, "torso_low");
    if (name == "seat") {
        if (region == bone_index(bones, "torso_low")) return true;
        if (region != bone_index(bones, "l_thigh") && region != bone_index(bones, "r_thigh")) return false;
        return norm(p - sk.point("pelvis")) <= 0.8 * u;  // (the thighs just below the buttocks)
    }
    return false;
}

// Andrew's monotone chain: the convex hull, counter-clockwise
std::vector<std::array<double, 2>> hull(std::vector<std::array<double, 2>> pts) {
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return pts;
    const auto cross2 = [](const std::array<double, 2>& o, const std::array<double, 2>& a, const std::array<double, 2>& b) {
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
    };
    std::vector<std::array<double, 2>> h(2 * pts.size());
    std::size_t k = 0;
    for (const auto& p : pts) {
        while (k >= 2 && cross2(h[k - 2], h[k - 1], p) <= 0) --k;
        h[k++] = p;
    }
    for (std::size_t i = pts.size() - 1, t = k + 1; i-- > 0;) {
        while (k >= t && cross2(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    h.resize(k - 1);
    return h;
}

// How far p lies outside the convex polygon (0 inside)
double outside(const std::vector<std::array<double, 2>>& poly, const std::array<double, 2>& p) {
    if (poly.empty()) return std::numeric_limits<double>::infinity();
    const auto seg = [](const std::array<double, 2>& a, const std::array<double, 2>& b, const std::array<double, 2>& q) {
        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        const double l2 = dx * dx + dy * dy;
        double t = l2 > 0 ? ((q[0] - a[0]) * dx + (q[1] - a[1]) * dy) / l2 : 0.0;
        t = std::max(0.0, std::min(1.0, t));
        return std::hypot(q[0] - (a[0] + dx * t), q[1] - (a[1] + dy * t));
    };
    if (poly.size() < 3) {
        double best = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < poly.size(); ++i) best = std::min(best, seg(poly[i], poly[(i + 1) % poly.size()], p));
        return best;
    }
    bool in = true;
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const auto& a = poly[i];
        const auto& b = poly[(i + 1) % poly.size()];
        if ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]) < 0) in = false;
        best = std::min(best, seg(a, b, p));
    }
    return in ? 0.0 : best;
}

void range_issue(std::vector<Issue>& out, const std::string& joint, char axis, double v) {
    const std::array<double, 2> r = range_of(joint, axis);
    if (!(r[0] <= v && v <= r[1])) {
        out.push_back({"range", joint + "." + std::string(1, axis) + " " + fmt(v) + " is outside [" + fmt(r[0]) + ", " + fmt(r[1]) + "]"});
    }
}

}  // namespace

const std::vector<JointRange>& joint_ranges() { return kRanges; }
const std::vector<BallRange>& ball_ranges() { return kBalls; }

std::array<double, 2> range_of(std::string_view joint, char axis) {
    const bool right = joint.size() > 2 && joint.substr(0, 2) == "r_";
    const std::string name = right ? "l_" + std::string(joint.substr(2)) : std::string(joint);
    for (const JointRange& r : kRanges) {
        if (r.joint != name) continue;
        const std::array<double, 2> v = axis == 'x' ? r.x : (axis == 'y' ? r.y : r.z);
        if (right && axis != 'x') return {-v[1], -v[0]};
        return v;
    }
    return {0.0, 0.0};
}

// --- (1) the file ----------------------------------------------------------------------------------------------------

// A person's keys of a pose (or of a pair's person): joints, hands, hold, support, seat_h, contact ("partner" a pair's own)
template <typename In>
void person_fields(const Json& p, const In& in, bool in_pair) {
    const Json& joints = p.value("joints", Json());
    if (!joints.is_object()) {
        in("joints is {joint: {x, y, z}}");
    } else {
        for (const auto& [name, axes] : joints.items()) {
            if (std::find(mesh3d::kFigureJoints.begin(), mesh3d::kFigureJoints.end(), name) == mesh3d::kFigureJoints.end()) in("unknown joint " + name);
            if (!axes.is_object() || axes.empty()) {
                in(name + " is {x, y, z}");
                continue;
            }
            for (const auto& [axis, v] : axes.items()) {
                if (axis != "x" && axis != "y" && axis != "z") in(name + ": unknown axis " + axis);
                if (!v.is_number_float() || !std::isfinite(v.get<double>()) || !places(v.get<double>(), 4)) in(name + "." + axis + " is a number to 4 places");
            }
        }
    }
    const Json& hands = p.value("hands", Json());
    if (!hands.is_object()) {
        in("hands is {l, r}");
    } else {
        for (const auto& [side, hand] : hands.items()) {
            if (side != "l" && side != "r") in("hands: unknown side " + side);
            if (hand.is_string()) {
                if (std::find(mesh3d::kHandPoses.begin(), mesh3d::kHandPoses.end(), hand.get<std::string>()) == mesh3d::kHandPoses.end()) in("hand " + hand.dump());
                continue;
            }
            if (!hand.is_object() || !hand.contains("pose") || !hand["pose"].is_string() ||
                std::find(mesh3d::kHandPoses.begin(), mesh3d::kHandPoses.end(), hand["pose"].get<std::string>()) == mesh3d::kHandPoses.end()) {
                in("a hand is a pose or {pose, curls, spread, thumb}");
                continue;
            }
            for (const auto& [k, v] : hand.items()) {
                if (k == "pose") continue;
                if (k == "curls") {
                    bool ok = v.is_array() && v.size() == 5;
                    for (const Json& c : ok ? v : Json::array()) ok = ok && c.is_number() && c.get<double>() >= 0.0 && c.get<double>() <= 1.0 && places(c.get<double>(), 3);
                    if (!ok) in("curls are five numbers 0..1 to 3 places");
                } else if (k == "spread" || k == "thumb") {
                    if (!v.is_number() || !(v.get<double>() >= 0.0 && v.get<double>() <= 1.0) || !places(v.get<double>(), 3)) in(k + " is 0..1 to 3 places");
                } else {
                    in("hand: unknown key " + k);
                }
            }
        }
    }
    if (p.contains("hold")) {
        const Json& hold = p["hold"];
        if (!hold.is_object() || hold.empty()) {
            in("hold is {l, r: prop or prop:way}");
        } else {
            for (const auto& [side, prop] : hold.items()) {
                if (side != "l" && side != "r") in("hold: unknown side " + side);
                if (!prop.is_string() || !pose_library::is_prop(pose_library::held_of(prop.get_ref<const std::string&>()).prop)) {
                    in("hold: unknown prop " + prop.dump());
                } else if (const std::string problem = pose_library::hold_problem(prop.get_ref<const std::string&>()); !problem.empty()) {
                    in("hold: " + problem);  // (a way that is not the prop's: SPEC FIGURE-02, figures@2)
                }
            }
        }
    }
    const std::string support = p.value("support", std::string());
    if (support != "floor" && support != "chair" && support != "none") in("support is floor, chair or none");
    if ((support == "chair") != p.contains("seat_h")) in("seat_h is a chair's (and only a chair's)");
    if (p.contains("seat_h") && (!p["seat_h"].is_number() || !places(p["seat_h"].get<double>(), 2))) in("seat_h is a number to 2 places");
    const Json& contact = p.value("contact", Json());
    if (!contact.is_array()) {
        in("contact is a list");
    } else {
        std::set<std::string> seen;
        for (const Json& c : contact) {
            if (!c.is_string() || (std::find(std::begin(kContacts), std::end(kContacts), c.get<std::string>()) == std::end(kContacts) &&
                                   !(in_pair && c.get<std::string>() == pose_library::kPartner))) {
                in("unknown contact " + c.dump());
                continue;
            }
            if (!seen.insert(c.get<std::string>()).second) in("contact " + c.get<std::string>() + " twice");
        }
        if (support != "none" && contact.empty()) in("a body that rests on something touches it");
        if (in_pair && support == "none" && !has(contact, pose_library::kPartner)) in("a person resting on nothing is held by the partner (contact partner)");
        if (support == "chair" && !has(contact, "seat")) in("a chair's seat is a contact");
    }
}

std::vector<Issue> check_file(const Json& file) {
    std::vector<Issue> out;
    const auto bad = [&](const std::string& what) { out.push_back({"file", what}); };
    if (!file.is_object()) return {{"file", "the file is not an object"}};
    for (const auto& [k, v] : file.items()) {
        if (k != "format" && k != "categories" && k != "poses") bad("unknown key " + k);
    }
    if (file.value("format", Json()) != Json(std::string(pose_library::kFormat))) bad("format is not genko.poses@1");
    std::vector<std::string> categories;
    if (!file.contains("categories") || !file["categories"].is_array() || file["categories"].empty()) {
        bad("no categories");
    } else {
        for (const Json& c : file["categories"]) {
            if (!c.is_object() || c.size() != 3 || !c.contains("id") || !c.contains("ja") || !c.contains("en")) {
                bad("a category is {id, ja, en}");
                continue;
            }
            const Json& id = c["id"];
            if (!id.is_string() || !good_name(id.get<std::string>())) bad("category id " + c["id"].dump());
            if (!c["ja"].is_string() || c["ja"].get<std::string>().empty() || ascii(c["ja"].get<std::string>())) bad("category ja " + c["ja"].dump());
            if (!c["en"].is_string() || c["en"].get<std::string>().empty() || !ascii(c["en"].get<std::string>())) bad("category en " + c["en"].dump());
            if (id.is_string()) {
                if (std::find(categories.begin(), categories.end(), id.get<std::string>()) != categories.end()) bad("category " + id.get<std::string>() + " twice");
                categories.push_back(id.get<std::string>());
            }
        }
    }
    if (!file.contains("poses") || !file["poses"].is_array()) return out.empty() ? std::vector<Issue>{{"file", "no poses"}} : out;
    std::set<std::string> ids;
    std::size_t last_category = 0;
    for (const Json& p : file["poses"]) {
        if (!p.is_object() || !p.contains("id") || !p["id"].is_string()) {
            bad("a pose without an id");
            continue;
        }
        const std::string id = p["id"].get<std::string>();
        const auto in = [&](const std::string& what) { bad(id + ": " + what); };
        const bool pair = pose_library::is_pair(p);
        for (const auto& [k, v] : p.items()) {
            const auto& keys = pair ? std::begin(kPairKeys) : std::begin(kPoseKeys);
            const auto& keys_end = pair ? std::end(kPairKeys) : std::end(kPoseKeys);
            if (std::find(keys, keys_end, k) == keys_end) in("unknown key " + k);
        }
        for (const char* k : {"category", "ja", "en", "tags", "symmetric"}) {
            if (!p.contains(k)) in(std::string("no ") + k);
        }
        for (const char* k : {"joints", "hands", "support", "contact"}) {
            if (!pair && !p.contains(k)) in(std::string("no ") + k);
        }
        if (pair && !p.contains("touch")) in("no touch");
        if (!ids.insert(id).second) in("twice");
        const std::string category = p.value("category", std::string());
        const auto at = std::find(categories.begin(), categories.end(), category);
        if (at == categories.end()) {
            in("unknown category " + category);
        } else {
            const auto n = static_cast<std::size_t>(at - categories.begin());
            if (n < last_category) in("out of its category's order");
            last_category = std::max(last_category, n);
        }
        if (id.rfind(category + "/", 0) != 0 || !good_name(id.substr(std::min(id.size(), category.size() + 1)))) in("the id is <category>/<a_z_0_9>");
        const Json& ja = p.value("ja", Json());
        const Json& en = p.value("en", Json());
        if (!ja.is_string() || ja.get<std::string>().empty() || ascii(ja.get<std::string>())) in("ja is a Japanese name");
        if (!en.is_string() || en.get<std::string>().empty() || !ascii(en.get<std::string>())) in("en is an English name");
        const Json& tags = p.value("tags", Json());
        bool tag_ja = false;
        bool tag_en = false;
        if (!tags.is_array()) {
            in("tags is a list");
        } else {
            std::set<std::string> seen;
            for (const Json& t : tags) {
                if (!t.is_string() || t.get<std::string>().empty()) {
                    in("a tag is a word");
                    continue;
                }
                if (!seen.insert(t.get<std::string>()).second) in("tag " + t.get<std::string>() + " twice");
                (ascii(t.get<std::string>()) ? tag_en : tag_ja) = true;
            }
            if (!tag_ja || !tag_en) in("tags in Japanese and in English");
        }
        if (pose_library::is_pair(p)) {
            const Json& figures = p["figures"];
            if (!figures.is_array() || figures.size() != 2) {
                in("figures is two people (a, b)");
            } else {
                for (std::size_t k = 0; k < 2; ++k) {
                    const Json& f = figures[k];
                    const std::string role = k == 0 ? "a" : "b";
                    const auto in_f = [&](const std::string& what) { in(role + ": " + what); };
                    if (!f.is_object()) {
                        in_f("a person is an object");
                        continue;
                    }
                    for (const auto& [key, v] : f.items()) {
                        if (std::find(std::begin(kPersonKeys), std::end(kPersonKeys), key) == std::end(kPersonKeys)) in_f("unknown key " + key);
                    }
                    for (const char* key : {"joints", "hands", "support", "contact"}) {
                        if (!f.contains(key)) in_f(std::string("no ") + key);
                    }
                    if (k == 0 && (f.contains("offset") || f.contains("turn"))) in_f("a is where the pair is: no offset or turn");
                    if (k == 1) {
                        const Json& o = f.value("offset", Json());
                        bool ok = o.is_array() && o.size() == 3;
                        for (const Json& v : ok ? o : Json::array()) ok = ok && v.is_number() && std::isfinite(v.get<double>()) && places(v.get<double>(), 4);
                        if (!ok) in_f("offset is [x, y, z] (u, to 4 places)");
                        const Json& t = f.value("turn", Json());
                        if (!t.is_number() || !std::isfinite(t.get<double>()) || !places(t.get<double>(), 4)) in_f("turn is a number (to 4 places)");
                    }
                    person_fields(f, in_f, true);
                }
            }
            const Json& touch = p.value("touch", Json());
            if (!touch.is_array()) {
                in("touch is a list of {a, b}");
            } else {
                for (const Json& t : touch) {
                    const auto part = [&](const char* role) {
                        const Json* v = t.is_object() ? get(t, role) : nullptr;
                        return v != nullptr && v->is_string() &&
                               std::find(std::begin(pose_library::kTouchParts), std::end(pose_library::kTouchParts), v->get<std::string>()) != std::end(pose_library::kTouchParts);
                    };
                    if (!t.is_object() || t.size() != 2 || !part("a") || !part("b")) in("a touch is {a: part, b: part} (" + t.dump() + ")");
                }
            }
        } else {
            person_fields(p, in, false);
        }
        if (!p.value("symmetric", Json()).is_boolean()) in("symmetric is true or false");
    }
    return out;
}

// --- (2) the ranges --------------------------------------------------------------------------------------------------

Ball ball_of(std::string_view joint, const Json& axes) {
    const bool right = joint.size() > 2 && joint.substr(0, 2) == "r_";
    const double side = right ? -1.0 : 1.0;
    Json total = axes.is_object() ? axes : Json::object();
    if (joint.ends_with("_arm")) total["z"] = side * 0.12 + total.value("z", 0.0);  // (the arms' rest, as figure_skeleton adds it)
    const Mat3 m = mesh3d::joint_matrix(total);
    const Vec3 d = la::column(m, 1);  // (the bone)
    const Vec3 r = la::column(m, 2);  // (and its back)
    Ball b;
    b.rise = std::acos(std::max(-1.0, std::min(1.0, d[1])));
    b.toward = std::atan2(side * d[0], -d[2]);
    // the twist: the bone's back against where the shortest swing from hanging down carries it
    Vec3 axis = la::cross(Vec3{0.0, 1.0, 0.0}, d);
    const double n = la::norm(axis);
    axis = n > 1e-12 ? axis / n : Vec3{1.0, 0.0, 0.0};
    const double c = std::cos(b.rise);
    const double s = std::sin(b.rise);
    const Vec3 z{0.0, 0.0, 1.0};
    const Vec3 r0 = z * c + la::cross(axis, z) * s + axis * (la::dot(axis, z) * (1.0 - c));
    b.twist = -side * std::atan2(la::dot(d, la::cross(r0, r)), la::dot(r0, r));
    if (joint.ends_with("_leg")) {
        // the hip's rotation about the femur, in the joint's own coordinate system: m = rx(flexion) rz(abduction) ry(twist)
        const double ab = std::asin(std::max(-1.0, std::min(1.0, d[0])));
        const double fl = std::atan2(-d[2], d[1]);
        const double ca = std::cos(fl);
        const double sa = std::sin(fl);
        const double cb = std::cos(ab);
        const double sb = std::sin(ab);
        const Vec3 x = la::column(m, 0);
        const double m00 = cb * x[0] - ca * sb * x[1] + sa * sb * x[2];  // ((rx rz)ᵀ m)[0][0]
        const double m20 = sa * x[1] + ca * x[2];                        // ((rx rz)ᵀ m)[2][0]
        b.twist = side * std::atan2(m20, m00);
    }
    return b;
}

double most_rise(const BallRange& r, double toward) {
    // (straight between forward 0, outward π/2, back ±π, inward −π/2)
    const double a = std::fabs(toward);
    const double half = 1.5707963267948966;
    if (toward >= 0) return a <= half ? r.forward + (r.out - r.forward) * a / half : r.out + (r.back - r.out) * (a - half) / half;
    return a <= half ? r.forward + (r.in - r.forward) * a / half : r.in + (r.back - r.in) * (a - half) / half;
}

std::vector<Issue> check_ranges(const Json& joints) {
    std::vector<Issue> out;
    if (!joints.is_object()) return out;
    for (const auto& [name, axes] : joints.items()) {
        if (!axes.is_object()) continue;
        const std::string base = name.size() > 2 && name.substr(0, 2) == "r_" ? "l_" + name.substr(2) : name;
        const auto ball = std::find_if(kBalls.begin(), kBalls.end(), [&](const BallRange& b) { return b.joint == base; });
        if (ball != kBalls.end()) {
            const Ball b = ball_of(name, axes);
            const double most = most_rise(*ball, b.toward);
            if (b.rise > most) out.push_back({"range", name + " rises " + fmt(b.rise) + " toward " + fmt(b.toward) + ", beyond " + fmt(most)});
            if (b.rise < ball->twist_rise && !(ball->twist[0] <= b.twist && b.twist <= ball->twist[1])) {
                out.push_back({"range", name + " twists " + fmt(b.twist) + ", outside [" + fmt(ball->twist[0]) + ", " + fmt(ball->twist[1]) + "]"});
            }
            continue;
        }
        for (const auto& [axis, v] : axes.items()) {
            if (axis.size() == 1 && v.is_number()) range_issue(out, name, axis[0], v.get<double>());
        }
    }
    return out;
}

// --- (3) the support ---------------------------------------------------------------------------------------------------

Json figure_of(const Json& pose, double height_mm, const Json& body) {
    Json prim = Json::object();
    prim["id"] = "pose";
    prim["kind"] = "figure";
    prim["pos"] = Json::array({0.0, 0.0, 0.0});
    prim["size"] = Json::array({height_mm / 2, height_mm, height_mm / 4});
    prim["rot"] = Json::array({0.0, 0.0, 0.0});
    prim["body"] = body.is_object() ? body : Json::object();
    prim["joints"] = pose.value("joints", Json::object());
    prim["hands"] = pose.value("hands", Json::object());
    return prim;
}

Measure measure(const Json& prim, const mesh3d::Model& model, const Json& pose) {
    Measure m;
    const mesh3d::Skeleton sk = mesh3d::figure_skeleton(prim);
    const double u = sk.unit;
    m.unit = u;
    m.triangles = model.smooth() ? model.tris.size() : model.mesh.f.size();
    const std::vector<Vec3>& v = model.mesh.v;
    if (v.empty()) return m;
    const std::vector<Bone> bones = bones_of(sk, prim);
    const std::vector<int> region = regions(v, bones, u);
    m.floor = -std::numeric_limits<double>::infinity();
    for (const Vec3& p : v) {
        m.floor = std::max(m.floor, p[1]);
        for (int a = 0; a < 3; ++a) m.reach = std::max(m.reach, std::fabs(p[static_cast<std::size_t>(a)]) / u);
    }
    for (const char* name : kFloorContacts) {
        double low = -std::numeric_limits<double>::infinity();
        Vec3 at{};
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i][1] > low && in_contact(name, v[i], region[i], bones, sk)) {
                low = v[i][1];
                at = v[i];
            }
        }
        m.gap[name] = (m.floor - low) / u;
        m.lowest[name] = at;
    }
    m.seat = m.gap["seat"];
    // the ribcage (the chest and the back: a body lying on the floor), the head and the upper arms (a body on its side rests
    // on the shoulder), the lowest of each
    for (const auto& [name, bone] : {std::pair<const char*, const char*>{"torso", "torso_high"}, {"head", "head"}, {"l_upper", "l_upper"}, {"r_upper", "r_upper"}}) {
        const int index = bone_index(bones, bone);
        double low = -std::numeric_limits<double>::infinity();
        Vec3 at{};
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i][1] > low && region[i] == index) {
                low = v[i][1];
                at = v[i];
            }
        }
        m.gap[name] = (m.floor - low) / u;
        m.lowest[name] = at;
    }
    {  // the face of the torso toward the floor
        const Mat3& chest = sk.turn("chest");
        const double front = -la::column(chest, 2)[1];  // (the front is −z: how far it faces down)
        const double side = std::fabs(la::column(chest, 0)[1]);
        m.down = side > std::fabs(front) ? "side" : (front > 0 ? "front" : "back");
    }
    // the centre of mass (each bone's share of the body at its middle) and the place the body rests on
    static const std::pair<const char*, double> kMass[] = {{"head", 0.081}, {"torso_low", 0.25}, {"torso_high", 0.25}, {"neck", 0.0},
                                                           {"l_upper", 0.028}, {"r_upper", 0.028}, {"l_fore", 0.016}, {"r_fore", 0.016},
                                                           {"l_hand", 0.006},  {"r_hand", 0.006},  {"l_thigh", 0.1},  {"r_thigh", 0.1},
                                                           {"l_shin", 0.0465}, {"r_shin", 0.0465}, {"l_foot", 0.0145}, {"r_foot", 0.0145}};
    Vec3 mass{0.0, 0.0, 0.0};
    double total = 0;
    for (const auto& [name, share] : kMass) {
        const Bone& b = bones[static_cast<std::size_t>(bone_index(bones, name))];
        mass = mass + (b.a + b.b) * (0.5 * share);
        total += share;
    }
    m.mass = mass / total;
    const std::string support = pose.value("support", std::string("floor"));
    const Json contact = pose.value("contact", Json::array());
    std::vector<std::array<double, 2>> patch;
    const double seat_low = m.floor - m.seat * u;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i][1] >= m.floor - kContactGap * u) patch.push_back({v[i][0], v[i][2]});
        if (support == "chair" && v[i][1] >= seat_low - kSeatBand * u && in_contact("seat", v[i], region[i], bones, sk)) patch.push_back({v[i][0], v[i][2]});
    }
    m.rest = patch;
    m.balance = outside(hull(std::move(patch)), {m.mass[0], m.mass[2]}) / u;
    return m;
}

std::vector<Issue> check_support(const Json& pose, const Measure& m) {
    std::vector<Issue> out;
    const std::string support = pose.value("support", std::string());
    const Json contact = pose.value("contact", Json::array());
    const std::string category = pose.value("category", std::string());
    const auto gap = [&](const char* name) { return m.gap.at(name); };
    if (support == "floor") {
        for (const char* name : kFloorContacts) {
            if (has(contact, name)) {
                if (gap(name) > kContactGap) out.push_back({"support", std::string(name) + " is " + fmt(gap(name)) + " u above the floor"});
            } else if (gap(name) < kUnlistedGap) {
                const bool seat = std::string_view(name) == "seat" || std::string_view(name) == "pelvis";
                if (!(seat && (has(contact, "seat") || has(contact, "pelvis")))) out.push_back({"support", std::string(name) + " touches the floor but is not a contact"});
            }
        }
    } else if (support == "chair") {
        const bool feet = has(contact, "l_foot") || has(contact, "r_foot");
        for (const char* name : {"l_foot", "r_foot"}) {
            if (has(contact, name) && gap(name) > kContactGap) out.push_back({"support", std::string(name) + " is " + fmt(gap(name)) + " u above the floor"});
        }
        const double seat_h = pose.value("seat_h", 0.0);
        if (!(kSeatLow <= seat_h && seat_h <= kSeatHigh)) out.push_back({"support", "seat_h " + fmt(seat_h) + " is not a seat's"});
        if (feet && std::fabs(m.seat - seat_h) > kSeatGap) out.push_back({"support", "the seat is " + fmt(m.seat) + " u high, not " + fmt(seat_h)});
        for (const char* name : {"l_knee", "r_knee", "l_hand", "r_hand"}) {
            if (feet && !has(contact, name) && gap(name) < kUnlistedGap) out.push_back({"support", std::string(name) + " touches the floor but is not a contact"});
        }
    }
    const bool faces = has(contact, "back") || has(contact, "front") || has(contact, "side");
    // (on the floor, back, front or side with the ribcage on it is lying there; off it, a lean on a wall)
    const bool lying = support == "floor" && faces && m.gap.at("torso") <= kContactGap;
    if (lying) {
        for (const char* face : {"back", "front", "side"}) {
            if (has(contact, face) && m.down != face) out.push_back({"support", "the body lies on its " + m.down + ", not its " + face});
        }
    }
    if (support == "floor" && !faces && m.gap.at("torso") < kUnlistedGap) {
        out.push_back({"support", "the torso touches the floor but is not a contact (back, front or side)"});
    }
    // (a lean on a wall, or on the partner of a pair: the pair's balance is the two together, check_pair)
    const bool leans = (faces && !lying) || has(contact, pose_library::kPartner);
    if (support != "none" && !leans && category != "walk" && category != "run" && m.balance > kBalance) {
        out.push_back({"balance", "the centre of mass is " + fmt(m.balance) + " u beyond what the body rests on"});
    }
    return out;
}

// --- (4) inside ------------------------------------------------------------------------------------------------------

Inside inside(const Json& prim) {
    Inside out;
    const mesh3d::Skeleton sk = mesh3d::figure_skeleton(prim);
    const double u = sk.unit;
    const figure_shape::Part torso = figure_shape::torso_part(sk);
    const figure_shape::Part head = figure_shape::head_part(sk);
    const std::vector<Bone> bones = bones_of(sk, prim);
    constexpr int kSamples = 9;
    const auto sample = [](const Bone& b, double from, int i) {
        const double t = from + (1.0 - from) * static_cast<double>(i) / (kSamples - 1);
        return std::pair<Vec3, double>{b.a + (b.b - b.a) * t, b.ra + (b.rb - b.ra) * t};
    };
    for (const Bone& b : bones) {
        if (b.chain < 0) continue;
        for (int i = 0; i < kSamples; ++i) {
            const auto [p, r] = sample(b, b.from, i);
            const double in_torso = -torso.at(p) / (r * u);
            if (in_torso > out.torso) {
                out.torso = in_torso;
                out.torso_limb = b.name;
            }
            const auto [q, rq] = sample(b, std::min(b.from, 0.2), i);
            const double in_head = -head.at(q) / (rq * u);
            if (in_head > out.head) {
                out.head = in_head;
                out.head_limb = b.name;
            }
        }
    }
    for (std::size_t i = 0; i < bones.size(); ++i) {
        for (std::size_t j = i + 1; j < bones.size(); ++j) {
            if (bones[i].chain < 0 || bones[j].chain < 0 || bones[i].chain == bones[j].chain) continue;
            if (bones[i].name.ends_with("_hand") && bones[j].name.ends_with("_hand")) continue;  // (hands clasp and interlace)
            for (int a = 0; a < kSamples; ++a) {
                const auto [p, rp] = sample(bones[i], 0.0, a);
                for (int b = 0; b < kSamples; ++b) {
                    const auto [q, rq] = sample(bones[j], 0.0, b);
                    const double over = ((rp + rq) * u - norm(p - q)) / (std::min(rp, rq) * u);
                    if (over > out.overlap) {
                        out.overlap = over;
                        out.overlap_limbs = bones[i].name + " and " + bones[j].name;
                    }
                }
            }
        }
    }
    return out;
}

std::vector<Issue> check_inside(const Json& prim) {
    const Inside in = inside(prim);
    std::vector<Issue> out;
    if (in.torso > kInside) out.push_back({"inside", in.torso_limb + " is " + fmt(in.torso) + " of its radius inside the torso"});
    if (in.head > kInside) out.push_back({"inside", in.head_limb + " is " + fmt(in.head) + " of its radius inside the head"});
    if (in.overlap > kOverlap) out.push_back({"overlap", in.overlap_limbs + " overlap by " + fmt(in.overlap) + " of the thinner's radius"});
    return out;
}

// --- (5) the mirror --------------------------------------------------------------------------------------------------

std::vector<Issue> check_mirror(const Json& pose) {
    std::vector<Issue> out;
    const Json once = pose_library::mirrored(pose);
    if (pose_library::mirrored(once) != pose) out.push_back({"mirror", "the mirror of the mirror is not the pose"});
    const mesh3d::Skeleton a = mesh3d::figure_skeleton(figure_of(pose));
    const mesh3d::Skeleton b = mesh3d::figure_skeleton(figure_of(once));
    const auto other = [](const std::string& name) {
        if (name.size() > 2 && name[1] == '_' && (name[0] == 'l' || name[0] == 'r')) return std::string(1, name[0] == 'l' ? 'r' : 'l') + name.substr(1);
        return name;
    };
    double worst = 0;
    for (const auto& [name, p] : a.points) {
        const Vec3& q = b.point(other(name));
        worst = std::max({worst, std::fabs(p[0] + q[0]), std::fabs(p[1] - q[1]), std::fabs(p[2] - q[2])});
    }
    if (worst > 1e-9 * a.unit) out.push_back({"mirror", "the mirror's skeleton is " + std::to_string(worst / a.unit) + " u off the reflection"});
    if (pose.value("symmetric", false)) {
        const auto same = [](const Json& x, const Json& y) { return nlohmann::json(x) == nlohmann::json(y); };  // (keys in any order)
        for (const char* key : {"joints", "hands", "hold", "contact"}) {
            const Json mine = pose.value(key, Json());
            Json theirs = once.value(key, Json());
            if (std::string_view(key) == "contact" && mine.is_array()) {
                std::multiset<std::string> x;
                std::multiset<std::string> y;
                for (const Json& c : mine) x.insert(c.get<std::string>());
                for (const Json& c : theirs) y.insert(c.get<std::string>());
                if (x != y) out.push_back({"mirror", "a symmetric pose's contacts are not their own mirror"});
                continue;
            }
            if (!same(mine, theirs)) out.push_back({"mirror", std::string("a symmetric pose's ") + key + " are not their own mirror"});
        }
    }
    return out;
}

// --- (6) drawn ---------------------------------------------------------------------------------------------------------

std::vector<Issue> check_render(const Measure& m, std::size_t lines) {
    std::vector<Issue> out;
    if (m.triangles == 0) out.push_back({"render", "no surface"});
    if (m.triangles > figure_shape::kMaxTriangles) out.push_back({"render", std::to_string(m.triangles) + " triangles"});
    if (m.reach > kFrame) out.push_back({"render", "reaches " + fmt(m.reach) + " u from the pelvis"});
    if (lines == 0) out.push_back({"render", "no lines"});
    return out;
}

Report check_pose(const Json& pose, const Json& body) {
    Report r;
    r.id = pose.value("id", std::string());
    const Json prim = figure_of(pose, 120.0, body);
    const auto model = mesh3d::model_of(prim);
    r.measured = measure(prim, *model, pose);
    const auto lines = mesh3d::prim_lines(prim, nullptr, 40.0);
    r.inside = inside(prim);
    for (auto list : {check_ranges(pose.value("joints", Json::object())), check_support(pose, r.measured), check_inside(prim), check_mirror(pose),
                      check_render(r.measured, lines ? lines->size() : 0)}) {
        r.issues.insert(r.issues.end(), list.begin(), list.end());
    }
    return r;
}

// --- the ops and the pictures ------------------------------------------------------------------------------------------

double lowest(const Json& prim) {
    const auto model = mesh3d::model_of(prim);
    static const Json kNone = Json::array({0, 0, 0});
    const Mat3 turn = mesh3d::prim_rotation(get_else(prim, "rot", kNone));
    double low = -std::numeric_limits<double>::infinity();
    const std::size_t body = model->body_points();  // (the body's own: not what its hands hold)
    for (std::size_t i = 0; i < body; ++i) low = std::max(low, matvec(turn, model->mesh.v[i])[1]);
    return std::isfinite(low) ? low : 0.0;
}

std::string thumbnail(const Json& prim, double turn, int width, int height) {
    Json seen = prim;
    seen["pos"] = Json::array({0.0, 0.0, 0.0});
    seen["rot"] = Json::array({0.0, turn, 0.0});
    const mesh3d::Skeleton sk = mesh3d::figure_skeleton(seen);
    const double u = sk.unit;
    const double floor = lowest(seen);
    const double tall = 13.0 * u;
    const double dpi = height / (tall / 25.4);
    const double box_w = tall * width / height;
    const std::vector<Json> prims{seen};
    const mesh3d::Raster r = mesh3d::raster(prims, width, height, dpi, nullptr, nullptr, 0.35, -box_w / 2, floor + 0.9 * u - tall);
    std::string out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), static_cast<char>(255));
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (!r.alpha(i)) continue;
        const float s = std::max(0.0f, std::min(1.0f, r.shade[i]));
        out[i] = static_cast<char>(static_cast<int>(s * 254.0f + 0.5f));
    }
    return out;
}

}  // namespace genko::core::pose_checks

namespace genko::core::pose_checks {

// --- pairs (G5) ----------------------------------------------------------------------------------------------------

std::pair<Json, Json> partner_place(const Json& a, const Json& offset, double turn) {
    const mesh3d::Skeleton sk = mesh3d::figure_skeleton(a);
    const double u = sk.unit;
    static const Json kZero = Json::array({0, 0, 0});
    Json rot = py_list(get_else(a, "rot", kZero));
    for (int i = 0; i < 3; ++i) rot.push_back(0.0);
    const Mat3 r = mesh3d::prim_rotation(rot);
    const Vec3 o{to_float(offset.at(0)) * u, to_float(offset.at(1)) * u, to_float(offset.at(2)) * u};
    const Vec3 d = matvec(r, o);
    static const Json kPos = Json::array({100, 160, 0});
    Json pos = py_list(get_else(a, "pos", kPos));
    for (int i = 0; i < 3; ++i) pos.push_back(0.0);
    Json at = Json::array();
    for (int i = 0; i < 3; ++i) at.push_back(py_round(to_float(pos[static_cast<std::size_t>(i)]) + d[static_cast<std::size_t>(i)], 4));
    Json turned = Json::array({to_float(rot[0]), py_round(to_float(rot[1]) + turn, 4), to_float(rot[2])});
    return {at, turned};
}

std::array<Json, 2> pair_figures(const Json& pair, double height_mm, const Json& body, bool with_hold) {
    std::array<Json, 2> out;
    for (int k = 0; k < 2; ++k) {
        const Json person = pose_library::member(pair, k == 0 ? "a" : "b");
        out[static_cast<std::size_t>(k)] = figure_of(person, height_mm, body);
        if (with_hold && person.contains("hold")) out[static_cast<std::size_t>(k)]["hold"] = person["hold"];
    }
    const Json& b = pair.at("figures").at(1);
    auto [pos, rot] = partner_place(out[0], b.at("offset"), to_float(b.at("turn")));
    out[1]["pos"] = std::move(pos);
    out[1]["rot"] = std::move(rot);
    return out;
}

namespace {

// A person of a pair in the space of the pair (a's own): b's points turned and moved.
struct Placed {
    Json prim;
    mesh3d::Skeleton sk;
    Mat3 turn;  // (its own turn)
    Vec3 at;    // (its pelvis)
    std::vector<Bone> bones;  // (in the pair's space)
    figure_shape::Part torso;
    figure_shape::Part head;

    Vec3 out(const Vec3& p) const { return matvec(turn, p) + at; }
    Vec3 in(const Vec3& p) const {  // (turnᵀ (p − at))
        const Vec3 d = p - at;
        return Vec3{turn[0][0] * d[0] + turn[1][0] * d[1] + turn[2][0] * d[2], turn[0][1] * d[0] + turn[1][1] * d[1] + turn[2][1] * d[2],
                    turn[0][2] * d[0] + turn[1][2] * d[1] + turn[2][2] * d[2]};
    }
};

Placed placed(const Json& prim) {
    Placed p{prim, mesh3d::figure_skeleton(prim), mesh3d::prim_rotation(prim.at("rot")), Vec3{to_float(prim["pos"][0]), to_float(prim["pos"][1]), to_float(prim["pos"][2])}, {}, {}, {}};
    p.bones = bones_of(p.sk, prim);
    for (Bone& b : p.bones) {
        b.a = p.out(b.a);
        b.b = p.out(b.b);
    }
    p.torso = figure_shape::torso_part(p.sk);
    p.head = figure_shape::head_part(p.sk);
    return p;
}

bool part_has(const std::string& part, const std::string& bone) {
    if (part == "torso") return bone == "torso_low" || bone == "torso_high" || bone == "neck";
    return part == bone;
}

// A touching part and its neighbour on the limb (a hand and its forearm: a hand held wraps the other's wrist)
bool near_part(const std::string& part, const std::string& bone) {
    if (part_has(part, bone)) return true;
    for (const char* side : {"l_", "r_"}) {
        const std::string s(side);
        if ((part == s + "hand" && bone == s + "fore") || (part == s + "fore" && bone == s + "hand")) return true;
    }
    return false;
}

constexpr int kPairSamples = 9;

std::pair<Vec3, double> sample_of(const Bone& b, double from, int i) {
    const double t = from + (1.0 - from) * static_cast<double>(i) / (kPairSamples - 1);
    return {b.a + (b.b - b.a) * t, b.ra + (b.rb - b.ra) * t};
}

// How far x (the pair's space) is from the surface of `part` of person q (u; < 0 inside it)
double from_surface(const Placed& q, const std::string& part, const Vec3& x) {
    const double u = q.sk.unit;
    if (part == "torso") return q.torso.at(q.in(x)) / u;
    if (part == "head") return q.head.at(q.in(x)) / u;
    for (const Bone& b : q.bones) {
        if (b.name != part) continue;
        double t = 0;
        const double d = dist_seg(x, b.a, b.b, &t);
        return d / u - (b.ra + (b.rb - b.ra) * t);
    }
    return std::numeric_limits<double>::infinity();
}

}  // namespace

PairReport check_pair(const Json& pair, const Json& body, bool full) {
    PairReport r;
    r.id = pair.value("id", std::string());
    const auto issue = [&](const std::string& check, const std::string& what) { r.issues.push_back({check, what}); };
    const std::array<Json, 2> prims = pair_figures(pair, 120.0, body);
    const char* const roles[] = {"a", "b"};
    std::array<std::vector<int>, 2> region;
    std::array<std::shared_ptr<const mesh3d::Model>, 2> models;
    for (int k = 0; k < 2; ++k) {
        const Json person = pose_library::member(pair, roles[k]);
        Json own = prims[static_cast<std::size_t>(k)];
        own["pos"] = Json::array({0.0, 0.0, 0.0});
        own["rot"] = Json::array({0.0, 0.0, 0.0});
        Report& one = k == 0 ? r.a : r.b;
        one.id = person.value("id", std::string());
        models[static_cast<std::size_t>(k)] = mesh3d::model_of(own);
        one.measured = measure(own, *models[static_cast<std::size_t>(k)], person);
        const auto lines = full ? mesh3d::prim_lines(own, nullptr, 40.0) : nullptr;
        one.inside = inside(own);
        for (auto list : {check_ranges(person.value("joints", Json::object())), check_support(person, one.measured), check_inside(own), check_mirror(person),
                          full ? check_render(one.measured, lines ? lines->size() : 0) : std::vector<Issue>{}}) {
            for (const Issue& i : list) issue(i.check, std::string(roles[k]) + ": " + i.what);
        }
        for (const char* sex : {"male", "female"}) {
            if (!full) break;
            for (const Issue& i : check_inside(figure_of(person, 120.0, Json::object({{"sex", sex}})))) issue(i.check, std::string(roles[k]) + " (" + sex + "): " + i.what);
        }
        const mesh3d::Skeleton sk = mesh3d::figure_skeleton(own);
        region[static_cast<std::size_t>(k)] = regions(models[static_cast<std::size_t>(k)]->mesh.v, bones_of(sk, own), sk.unit);
    }
    const Placed a = placed(prims[0]);
    const Placed b = placed(prims[1]);
    const double u = a.sk.unit;
    const Json& fa = pair.at("figures").at(0);
    const Json& fb = pair.at("figures").at(1);
    // the same floor: both on the floor (or a chair's) — their lowest points at one height
    const std::string sa = fa.value("support", std::string());
    const std::string sb = fb.value("support", std::string());
    if (sa != "none" && sb != "none") {
        const double floor_b = r.b.measured.floor + b.at[1];
        r.floor = std::fabs(floor_b - r.a.measured.floor) / u;
        if (r.floor > kContactGap) issue("pair", "b's floor is " + fmt(r.floor) + " u from a's");
    }
    // the two together over what they rest on, when one leans on or is held by the other
    const bool leaning = has(fa.value("contact", Json::array()), pose_library::kPartner) || has(fb.value("contact", Json::array()), pose_library::kPartner);
    if (leaning) {
        std::vector<std::array<double, 2>> rest;
        for (const auto& p : r.a.measured.rest) rest.push_back(p);
        for (const auto& p : r.b.measured.rest) {
            const Vec3 w = b.out(Vec3{p[0], 0.0, p[1]});
            rest.push_back({w[0], w[2]});
        }
        const Vec3 mb = b.out(r.b.measured.mass);
        const std::array<double, 2> mass{(r.a.measured.mass[0] + mb[0]) / 2, (r.a.measured.mass[2] + mb[2]) / 2};
        r.balance = outside(hull(std::move(rest)), mass) / u;
        if (r.balance > kBalance) issue("balance", "the pair's centre of mass is " + fmt(r.balance) + " u beyond what they rest on");
        if (!pair.value("touch", Json::array()).is_array() || pair.value("touch", Json::array()).empty()) issue("pair", "a person leans on the partner but they touch nowhere");
    }
    // the touches: each named part of one within kTouchGap of the other's (its surface points: the other's surface)
    std::set<std::pair<std::string, std::string>> exempt;
    for (const Json& t : pair.value("touch", Json::array())) {
        const std::string pa = t.value("a", std::string());
        const std::string pb = t.value("b", std::string());
        double gap = std::numeric_limits<double>::infinity();
        const auto scan = [&](const Placed& me, int k, const std::string& mine, const Placed& other, const std::string& theirs) {
            const mesh3d::Model& m = *models[static_cast<std::size_t>(k)];
            const std::vector<Bone> own_bones = bones_of(me.sk, me.prim);
            const std::size_t points = m.body_points();
            for (std::size_t i = 0; i < points; i += 3) {  // (every third point: the surface is fine enough)
                const std::string& bone = own_bones[static_cast<std::size_t>(region[static_cast<std::size_t>(k)][i])].name;
                if (!part_has(mine, bone)) continue;
                gap = std::min(gap, from_surface(other, theirs, me.out(m.mesh.v[i])));
            }
        };
        scan(a, 0, pa, b, pb);
        scan(b, 1, pb, a, pa);
        r.touch.push_back(gap);
        if (gap > kTouchGap) issue("touch", "a's " + pa + " and b's " + pb + " are " + fmt(gap) + " u apart");
        if (gap < -kTouchDepth) issue("touch", "a's " + pa + " is " + fmt(-gap) + " u inside b's " + pb);
        for (const Bone& x : a.bones) {
            for (const Bone& y : b.bones) {
                if (near_part(pa, x.name) && near_part(pb, y.name)) exempt.insert({x.name, y.name});
            }
        }
    }
    // between the two: no limb's axis in the other's torso or head deeper than kInside of its radius, no torso, neck or
    // head axis in it at all (r.between.torso: the worst, as a part of what is allowed) — but a limb named touching the
    // other's torso or head, which the touch holds to kTouchDepth instead
    std::set<std::pair<std::string, std::string>> laid;  // (role, bone)
    for (const Json& t : pair.value("touch", Json::array())) {
        const std::string pa = t.value("a", std::string());
        const std::string pb = t.value("b", std::string());
        for (const auto& [mine, theirs, role, bones] : {std::tuple<std::string, std::string, std::string, const std::vector<Bone>*>{pa, pb, "a", &a.bones},
                                                        {pb, pa, "b", &b.bones}}) {
            if (theirs != "torso" && theirs != "head") continue;
            for (const Bone& x : *bones) {
                if (x.chain >= 0 && near_part(mine, x.name)) laid.insert({role, x.name});
            }
        }
    }
    for (const auto& [me, other, label] : {std::tuple<const Placed*, const Placed*, const char*>{&a, &b, "a"}, {&b, &a, "b"}}) {
        for (const Bone& x : me->bones) {
            if (laid.count({label, x.name}) != 0) continue;
            for (int i = 0; i < kPairSamples; ++i) {
                const auto [p, rr] = sample_of(x, 0.0, i);
                const Vec3 q = other->in(p);
                const double depth = -std::min(other->torso.at(q), other->head.at(q)) / u;
                const bool limb = x.chain >= 0;
                const double worst = limb ? depth / (kInside * rr) : (depth > 0.0 ? 1.0 + depth : depth);
                if (worst > r.between.torso) {
                    r.between.torso = worst;
                    r.between.torso_limb = std::string(label) + "'s " + x.name;
                    r.between.head = depth;
                }
            }
        }
    }
    if (r.between.torso > 1.0) issue("inside", r.between.torso_limb + " is " + fmt(r.between.head) + " u inside the other's body");
    for (const Bone& x : a.bones) {
        for (const Bone& y : b.bones) {
            if (x.chain < 0 || y.chain < 0 || exempt.count({x.name, y.name}) != 0) continue;
            for (int i = 0; i < kPairSamples; ++i) {
                const auto [p, rp] = sample_of(x, 0.0, i);
                for (int j = 0; j < kPairSamples; ++j) {
                    const auto [q, rq] = sample_of(y, 0.0, j);
                    const double over = ((rp + rq) * u - norm(p - q)) / (std::min(rp, rq) * u);
                    if (over > r.between.overlap) {
                        r.between.overlap = over;
                        r.between.overlap_limbs = "a's " + x.name + " and b's " + y.name;
                    }
                }
            }
        }
    }
    if (r.between.overlap > kOverlap) issue("overlap", r.between.overlap_limbs + " overlap by " + fmt(r.between.overlap) + " of the thinner's radius");
    return r;
}

}  // namespace genko::core::pose_checks
