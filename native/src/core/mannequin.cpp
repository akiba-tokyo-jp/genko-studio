// Python's genko/mannequin.py, expression by expression (the math module through core/pymath).

#include "core/mannequin.hpp"

#include <cmath>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyvalue.hpp"

namespace genko::core::mannequin {

const std::array<std::string_view, 16> kJoints{"hip",     "spine",   "neck",  "head",  "l_arm",  "r_arm",   "l_elbow", "r_elbow",
                                               "l_wrist", "r_wrist", "l_leg", "r_leg", "l_knee", "r_knee",  "l_ankle", "r_ankle"};

namespace {

double py_max(double a, double b) { return b > a ? b : a; }

constexpr std::string_view kHandles[][3] = {
    {"chest", "pelvis", "spine"},       {"head", "neck", "head"},          {"l_elbow", "l_shoulder", "l_arm"},
    {"r_elbow", "r_shoulder", "r_arm"}, {"l_hand", "l_elbow", "l_elbow"},   {"r_hand", "r_elbow", "r_elbow"},
    {"l_fingers", "l_hand", "l_wrist"}, {"r_fingers", "r_hand", "r_wrist"}, {"l_knee", "l_hip", "l_leg"},
    {"r_knee", "r_hip", "r_leg"},       {"l_ankle", "l_knee", "l_knee"},    {"r_ankle", "r_knee", "r_knee"},
    {"l_toe", "l_ankle", "l_ankle"},    {"r_toe", "r_ankle", "r_ankle"},
};

// {**default_joints(), **(prim.get("joints") or {})}
Json joints_of(const Json& prim) {
    Json joints = default_joints();
    static const Json kNone = Json::object();
    const Json& own = pyv::get_or(prim, "joints", kNone);
    if (!own.is_object()) throw Error("type", "'" + py_type_name(own) + "' object is not a mapping");
    for (const auto& [k, v] : own.items()) joints[k] = v;
    return joints;
}

// _joint: (yaw, pitch)
std::pair<double, double> joint(const Json& joints, std::string_view name) {
    const Json* slot_value = pyv::get(joints, name);
    static const Json kNone = Json::object();
    const Json& slot = slot_value != nullptr && py_truthy(*slot_value) ? *slot_value : kNone;
    const Json* yaw = pyv::get(slot, "yaw");
    const Json* pitch = pyv::get(slot, "pitch");
    const double y = yaw != nullptr ? pyv::to_float(*yaw) : 0.0;
    const double p = pitch != nullptr ? pyv::to_float(*pitch) : 0.0;
    return {y, p};
}

// [x, y, z…] + [0, 0, 0] of prim["rot"]
Json rot_items(const Json& prim) {
    static const Json kNone = Json::array({0, 0, 0});
    Json rot = py_list(pyv::get_or(prim, "rot", kNone));
    for (int i = 0; i < 3; ++i) rot.push_back(0);
    return rot;
}

std::string names_of_presets() {
    std::string out;
    for (const auto& [k, v] : presets().items()) out += (out.empty() ? "" : ", ") + k;
    return out;
}

}  // namespace

const Json& presets() {
    static const Json table = parse_python_json(R"({
 "stand": {},
 "walk": {"rot": [0, 1.5707963267948966, 0], "l_leg": {"yaw": 0.4}, "l_knee": {"yaw": -0.1}, "r_leg": {"yaw": -0.35},
          "r_knee": {"yaw": -0.5}, "l_arm": {"yaw": -0.4}, "l_elbow": {"yaw": 0.2}, "r_arm": {"yaw": 0.35}, "r_elbow": {"yaw": 0.45}},
 "run": {"rot": [0, 1.5707963267948966, 0], "spine": {"yaw": -0.3}, "l_leg": {"yaw": 0.9}, "l_knee": {"yaw": -1.2},
         "r_leg": {"yaw": -0.6}, "r_knee": {"yaw": -1.1}, "l_arm": {"yaw": -0.8}, "l_elbow": {"yaw": 1.2}, "r_arm": {"yaw": 0.9},
         "r_elbow": {"yaw": 1.3}},
 "sit": {"rot": [0, 1.5707963267948966, 0], "l_leg": {"yaw": 1.5}, "l_knee": {"yaw": -1.5}, "r_leg": {"yaw": 1.45},
         "r_knee": {"yaw": -1.45}, "l_arm": {"yaw": 0.3}, "l_elbow": {"yaw": 0.9}, "r_arm": {"yaw": 0.35}, "r_elbow": {"yaw": 0.9}},
 "point": {"r_arm": {"yaw": -1.55}, "r_elbow": {"yaw": 0.05}, "head": {"yaw": -0.15}},
 "look_back": {"rot": [0, 2.6, 0], "neck": {"yaw": 0.35}, "head": {"yaw": 0.3}},
 "arms_up": {"l_arm": {"yaw": 2.8}, "r_arm": {"yaw": -2.8}, "l_elbow": {"yaw": 0.2}, "r_elbow": {"yaw": -0.2}}
})");
    return table;
}

Json default_joints() {
    Json base = Json::object();
    for (const std::string_view name : kJoints) base[std::string(name)] = Json::object({{"yaw", 0.0}, {"pitch", 0.0}});
    base["l_arm"]["yaw"] = 0.25;
    base["r_arm"]["yaw"] = -0.25;
    base["l_leg"]["yaw"] = 0.08;
    base["r_leg"]["yaw"] = -0.08;
    return base;
}

void apply_preset(Json& prim, std::string_view name) {
    const Json* preset = pyv::find(presets(), name);
    if (preset == nullptr) throw Error("value", "preset must be one of " + names_of_presets());
    Json joints = default_joints();
    for (const auto& [key, values] : preset->items()) {
        if (key == "rot") {
            prim["rot"] = values;
        } else {
            for (const auto& [k, v] : values.items()) joints[key][k] = v;
        }
    }
    prim["joints"] = std::move(joints);
    prim["preset"] = std::string(name);
}

const Point2& Bone::point(std::string_view name) const {
    for (const auto& [key, p] : points) {
        if (key == name) return p;
    }
    throw OpKeyError(py_repr_str(name));
}

Bone skeleton(const Json& prim) {
    static const Json kDefaultPos = Json::array({100, 160, 0});
    const Json& pos_value = pyv::get_or(prim, "pos", kDefaultPos);
    Json first = pos_value.is_array() ? pyv::slice(pos_value, 0, 2)
                                      : (pos_value.is_string() ? pyv::slice(py_list(pos_value), 0, 2) : Json());
    if (first.is_null()) {
        throw Error("type", pos_value.is_object() ? "unhashable type: 'slice'"
                                                  : "'" + py_type_name(pos_value) + "' object is not subscriptable");
    }
    std::vector<double> xy;
    for (const Json& v : first) xy.push_back(pyv::to_float(v));
    if (xy.size() != 2) throw Error("value", "not enough values to unpack (expected 2, got " + std::to_string(xy.size()) + ")");
    const double x0 = xy[0];
    const double y0 = xy[1];
    static const Json kDefaultSize = Json::array({40, 80, 20});
    const Json& size = pyv::get_or(prim, "size", kDefaultSize);
    double height = pyv::to_float(size.is_array() ? pyv::at(size, 1) : size);
    if (height == 0.0) height = 80.0;
    const Json rot = rot_items(prim);
    const double tip = pyv::to_float(rot[0]);
    const double turn = pyv::to_float(rot[1]);
    const double lean = pyv::to_float(rot[2]);
    const Json joints = joints_of(prim);
    const double unit = height / 8.0;  // one head
    const double narrow = std::fabs(py_cos(turn));  // widths as seen when the figure turns
    const double squash = py_max(0.2, std::fabs(py_cos(tip)));
    const int facing = py_cos(turn) >= 0 ? 1 : -1;

    const auto step = [&](const Point2& point, double angle, double length, double pitch) {
        // angle 0 points down the page; positive angles turn counter-clockwise (toward +x at the bottom)
        length *= py_max(0.15, std::fabs(py_cos(pitch)));
        const double a = angle + lean;
        return Point2{point[0] + py_sin(a) * length, point[1] + py_cos(a) * length * squash};
    };

    Bone bone;
    const auto [hip_yaw, hip_pitch_unused] = joint(joints, "hip");
    (void)hip_pitch_unused;
    const auto [spine_yaw, spine_pitch] = joint(joints, "spine");
    const Point2 pelvis{x0, y0};
    const Point2 chest = step(pelvis, kPi + spine_yaw + hip_yaw, unit * 3.0, spine_pitch);
    bone.segments.push_back(Segment{pelvis, chest, "body"});
    const auto [neck_yaw, neck_pitch] = joint(joints, "neck");
    const Point2 neck_top = step(chest, kPi + spine_yaw + hip_yaw + neck_yaw, unit * 0.45, neck_pitch);
    bone.segments.push_back(Segment{chest, neck_top, "body"});
    const auto [head_yaw, head_pitch] = joint(joints, "head");
    const double head_r = unit * 0.5;
    const Point2 head_c = step(neck_top, kPi + spine_yaw + hip_yaw + neck_yaw + head_yaw, head_r, head_pitch);
    bone.points = {{"pelvis", pelvis}, {"chest", chest}, {"neck", neck_top}, {"head", head_c}};
    const double shoulder_half = unit * 0.9 * narrow;
    const double hip_half = unit * 0.55 * narrow;
    const double body_angle = spine_yaw + hip_yaw;
    const Point2 across{py_cos(body_angle + lean), -py_sin(body_angle + lean) * squash};
    for (const auto& [side, sign] : {std::pair<const char*, int>{"left", 1}, {"right", -1}}) {
        const std::string prefix = std::string(side) == "left" ? "l" : "r";
        // the figure's left is on the viewer's right (+x) when it faces the viewer
        const int s = sign * facing;
        const Point2 shoulder{chest[0] + across[0] * shoulder_half * s, chest[1] + across[1] * shoulder_half * s};
        bone.segments.push_back(Segment{chest, shoulder, "body"});
        const auto [arm_yaw, arm_pitch] = joint(joints, prefix + "_arm");
        const auto [elbow_yaw, elbow_pitch] = joint(joints, prefix + "_elbow");
        const double wrist_yaw = joint(joints, prefix + "_wrist").first;
        const Point2 elbow = step(shoulder, body_angle + arm_yaw * facing, unit * 1.45, arm_pitch);
        const Point2 hand = step(elbow, body_angle + (arm_yaw + elbow_yaw) * facing, unit * 1.3, elbow_pitch);
        const Point2 fingers = step(hand, body_angle + (arm_yaw + elbow_yaw + wrist_yaw) * facing, unit * 0.5, 0.0);
        bone.segments.push_back(Segment{shoulder, elbow, side});
        bone.segments.push_back(Segment{elbow, hand, side});
        bone.segments.push_back(Segment{hand, fingers, side});
        bone.points.emplace_back(prefix + "_shoulder", shoulder);
        bone.points.emplace_back(prefix + "_elbow", elbow);
        bone.points.emplace_back(prefix + "_hand", hand);
        bone.points.emplace_back(prefix + "_fingers", fingers);
        const Point2 hip{pelvis[0] + across[0] * hip_half * s, pelvis[1] + across[1] * hip_half * s};
        bone.segments.push_back(Segment{pelvis, hip, "body"});
        const auto [leg_yaw, leg_pitch] = joint(joints, prefix + "_leg");
        const auto [knee_yaw, knee_pitch] = joint(joints, prefix + "_knee");
        const double ankle_yaw = joint(joints, prefix + "_ankle").first;
        const Point2 knee = step(hip, hip_yaw + leg_yaw * facing, unit * 2.0, leg_pitch);
        const Point2 ankle = step(knee, hip_yaw + (leg_yaw + knee_yaw) * facing, unit * 2.0, knee_pitch);
        const double toe_angle = hip_yaw + (leg_yaw + knee_yaw) * facing - (kPi / 2 - ankle_yaw) * facing;
        const Point2 toe = step(ankle, toe_angle, unit * 0.6, 0.0);
        bone.segments.push_back(Segment{hip, knee, side});
        bone.segments.push_back(Segment{knee, ankle, side});
        bone.segments.push_back(Segment{ankle, toe, side});
        bone.points.emplace_back(prefix + "_hip", hip);
        bone.points.emplace_back(prefix + "_knee", knee);
        bone.points.emplace_back(prefix + "_ankle", ankle);
        bone.points.emplace_back(prefix + "_toe", toe);
    }
    std::vector<double> xs;
    std::vector<double> ys;
    for (const Segment& s : bone.segments) {
        xs.push_back(s.a[0]);
        xs.push_back(s.b[0]);
        ys.push_back(s.a[1]);
        ys.push_back(s.b[1]);
    }
    xs.push_back(head_c[0] - head_r);
    xs.push_back(head_c[0] + head_r);
    ys.push_back(head_c[1] - head_r);
    ys.push_back(head_c[1] + head_r);
    // (Python's min and max: the first of the smallest)
    const auto low = [](const std::vector<double>& v) {
        double m = v[0];
        for (const double x : v) {
            if (x < m) m = x;
        }
        return m;
    };
    const auto high = [](const std::vector<double>& v) {
        double m = v[0];
        for (const double x : v) {
            if (x > m) m = x;
        }
        return m;
    };
    bone.bbox = {low(xs), low(ys), high(xs) - low(xs), high(ys) - low(ys)};
    bone.head_c = head_c;
    bone.head_r = head_r;
    bone.facing = facing;
    return bone;
}

Json pose_to(const Json& prim, std::string_view handle, const Json& target) {
    if (handle == "pelvis") {
        static const Json kDefault = Json::array({100, 160, 0});
        Json pos = py_list(pyv::get_or(prim, "pos", kDefault));
        for (int i = 0; i < 3; ++i) pos.push_back(0);
        const double x = pyv::to_float(pyv::at(target, 0));
        const double y = pyv::to_float(pyv::at(target, 1));
        return Json::object({{"pos", Json::array({py_round(x, 3), py_round(y, 3), pos[2]})}});
    }
    const std::string_view* found = nullptr;
    for (const auto& h : kHandles) {
        if (h[0] == handle) found = h;
    }
    if (found == nullptr) {
        std::string names;
        for (const auto& h : kHandles) names += (names.empty() ? "" : ", ") + std::string(h[0]);
        throw Error("value", "handle must be pelvis or one of " + names);
    }
    const Bone bone = skeleton(prim);
    const std::string_view base_name = found[1];
    const std::string joint_name(found[2]);
    const Point2 base = bone.point(base_name);
    const Json rot = rot_items(prim);
    const double lean = pyv::to_float(rot[2]);
    const double squash = py_max(0.2, std::fabs(py_cos(pyv::to_float(rot[0]))));
    const int facing = bone.facing;
    const double dx = pyv::to_float(pyv::at(target, 0)) - base[0];
    const double dy = pyv::to_float(pyv::at(target, 1)) - base[1];
    if (py_hypot(dx, dy) < 1e-6) return Json::object();
    const double a = py_atan2(dx, dy / squash) - lean;  // the drawn angle (0 = down the page)
    const Json joints = joints_of(prim);
    const auto y = [&](const std::string& name) { return joint(joints, name).first; };
    // (Python reads every joint's yaw first)
    for (const std::string_view name : kJoints) (void)y(std::string(name));
    const double body = y("spine") + y("hip");
    const auto wrap = [](double v) { return py_atan2(py_sin(v), py_cos(v)); };
    const std::string p(1, handle[0]);
    const auto ends = [&](std::string_view suffix) { return handle.size() >= suffix.size() && handle.substr(handle.size() - suffix.size()) == suffix; };
    double value = 0.0;
    if (handle == "chest") {
        value = wrap(a - kPi - y("hip"));
    } else if (handle == "head") {
        value = wrap(a - kPi - body - y("neck"));
    } else if (ends("_elbow")) {
        value = wrap((a - body) * facing);
    } else if (ends("_hand")) {
        value = wrap((a - body) * facing - y(p + "_arm"));
    } else if (ends("_fingers")) {
        value = wrap((a - body) * facing - y(p + "_arm") - y(p + "_elbow"));
    } else if (ends("_knee")) {
        value = wrap((a - y("hip")) * facing);
    } else if (ends("_ankle")) {
        value = wrap((a - y("hip")) * facing - y(p + "_leg"));
    } else {  // toe
        value = wrap((a - y("hip")) * facing - y(p + "_leg") - y(p + "_knee") + kPi / 2);
    }
    return Json::object({{"joints", Json::object({{joint_name, Json::object({{"yaw", py_round(value, 4)}})}})}});
}

bool in_rect(const Json& prim, const std::array<double, 4>& rect) {
    const auto [x, y, w, h] = skeleton(prim).bbox;
    const double cx = x + w / 2;
    const double cy = y + h / 2;
    const auto [rx, ry, rw, rh] = rect;
    return rx <= cx && cx <= rx + rw && ry <= cy && cy <= ry + rh;
}

}  // namespace genko::core::mannequin
