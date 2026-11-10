// The 3D ops: Python's genko/threeops.py (add_figure, pose_figure, add_head, add_hand, import_model, set_camera,
// set_light, render_prims) and the 3D branches of genko/ops.py's _apply_one (add_mannequin, pose_mannequin,
// add_prim3d, add_scene, edit_prim, delete_prim, trace_prims, ruler_from_3d, camera_from_ruler), check by check and in
// the same order, with the same messages. (The page locks and strict_gates are the CommandBus's checks, before the op;
// the layer an edit draws on, a brush's kind and a layer's screen are core/ops_util's and core/brushes', as for every
// op.)
//
// Where Python stores what would break the book, the C++ build refuses the op instead (docs/cpp-migration/SPEC.md §2:
// known bugs are not carried over): a number that is not finite (a book never holds NaN or an infinity: refused once the
// op has done everything else, "<key> must be a finite number", as the other ops do), a mannequin that could not be
// drawn (values that are not numbers, a negative height), a perspective grid outside 0..60 lines, and the model files of
// import_model that Python reads into an unusable mesh (see core/mesh3d.hpp).

#include "render/ops_3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/base64.hpp"
#include "core/brushes.hpp"
#include "core/error.hpp"
#include "core/frames.hpp"
#include "core/ids.hpp"
#include "core/mannequin.hpp"
#include "core/mesh3d.hpp"
#include "core/ops_util.hpp"
#include "core/persp3d.hpp"
#include "core/prim3d.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/image.hpp"
#include "render/png.hpp"

namespace genko::render {

namespace {

using core::get;
using core::get_else;
using core::Json;
using core::Num;
using core::OpContext;
using core::OpError;
using core::Page;
using core::py_max;
using core::py_min;
namespace mesh3d = core::mesh3d;
namespace prim3d = core::prim3d;

constexpr int kWorkingDpi = 200;  // raster.WORKING_DPI

bool truthy(const Json* value) { return value != nullptr && core::py_truthy(*value); }

std::string join(std::span<const std::string_view> names) {
    std::string out;
    for (const std::string_view n : names) out += (out.empty() ? "" : ", ") + std::string(n);
    return out;
}

Json doubles(const std::vector<double>& values) {
    Json out = Json::array();
    for (const double v : values) out.push_back(v);
    return out;
}

// threeops._vec: [x, y, z] as floats (n of them; two at least)
Json vec(const Json& value, std::size_t n, const std::string& what) {
    std::vector<double> out;
    try {
        for (const Json& v : core::py_list(value)) out.push_back(core::to_float(v));
    } catch (const core::Error&) {
        throw OpError(what + " is [x, y, z]");
    }
    if (out.size() > n) out.resize(n);
    if (out.size() < 2) throw OpError(what + " is [x, y, z]");
    while (out.size() < n) out.push_back(0.0);
    return doubles(out);
}

// A number an op keeps under another name (height_mm as the size): OpError "<key> must be a finite number".
void require_finite_value(double value, const std::string& key) {
    if (!std::isfinite(value)) throw OpError(key + " must be a finite number");
}

// Whether every number in a value is finite.
bool all_finite(const Json& value) {
    if (value.is_number_float()) return std::isfinite(value.get<double>());
    if (value.is_array() || value.is_object()) {
        return std::all_of(value.begin(), value.end(), [](const Json& item) { return all_finite(item); });
    }
    return true;
}

// ops._vec3: [x, y, z] rounded to 4 places
Json vec3(const Json& value) {
    Json items = core::py_list(value);
    for (int i = 0; i < 3; ++i) items.push_back(0.0);
    std::vector<double> out;
    for (std::size_t i = 0; i < 3; ++i) out.push_back(core::py_round(core::to_float(items[i]), 4));
    return doubles(out);
}

// str(op.get("id") or new_id())
std::string id_of(const Json& op) {
    const Json* id = get(op, "id");
    return truthy(id) ? core::py_str(*id) : core::new_id();
}

// --- figures (threeops) -------------------------------------------------------------------------------------------

const std::pair<std::string_view, std::pair<double, double>> kBodyLimits[] = {
    {"heads", {4.0, 10.0}}, {"shoulders", {0.6, 1.5}}, {"hips", {0.6, 1.6}}, {"build", {0.5, 1.8}}, {"legs", {0.6, 1.5}}};

std::string hand_poses_message() { return "hand poses are " + join(mesh3d::kHandPoses) + " (for l and r)"; }

Json body_of(const Json& raw) {
    if (!raw.is_object()) throw OpError("body is {heads, shoulders, hips, build, legs}");
    Json out = Json::object();
    for (const auto& [key, value] : raw.items()) {
        if (key == "sex") {  // (男女: the build before the numbers)
            if (!(value.is_null() || core::is_one_of(value, {"", "male", "female"}))) throw OpError("body sex is male or female");
            out["sex"] = core::py_truthy(value) ? core::py_str(value) : std::string();
            continue;
        }
        const std::pair<double, double>* limits = nullptr;
        for (const auto& [name, range] : kBodyLimits) {
            if (name == key) limits = &range;
        }
        if (limits == nullptr) throw OpError("unknown body key " + key + " (heads, shoulders, hips, build, legs)");
        const double v = core::to_float(value);
        if (!(limits->first <= v && v <= limits->second)) {
            throw OpError("body " + key + " must be between " + core::py_format_g(limits->first) + " and " + core::py_format_g(limits->second));
        }
        out[key] = v;
    }
    return out;
}

// _hand_pose: a pose name, or each finger's curl
Json hand_pose(const Json& pose) {
    if (pose.is_string() && core::is_one_of(pose, {"open", "relaxed", "fist", "point", "peace", "grip"})) return pose;
    if (pose.is_array() && pose.size() == 5) {
        Json curls = Json::array();
        for (const Json& c : pose) curls.push_back(core::py_round(py_max(0.0, py_min(1.0, core::to_float(c))), 3));
        return Json::object({{"pose", "open"}, {"curls", std::move(curls)}});
    }
    if (pose.is_object()) {
        const Json* base_value = core::dict_get(pose, "pose");
        const std::string base = truthy(base_value) ? core::py_str(*base_value) : std::string("relaxed");
        const Json* curls = core::dict_get(pose, "curls");
        bool valid = curls == nullptr || curls->is_null() || (curls->is_array() && curls->size() == 5);
        if (!valid && curls->is_object()) {
            valid = true;
            for (const auto& [k, v] : curls->items()) {
                if (std::find(mesh3d::kFingers.begin(), mesh3d::kFingers.end(), k) == mesh3d::kFingers.end()) valid = false;
            }
        }
        if (!core::is_one_of(Json(base), {"open", "relaxed", "fist", "point", "peace", "grip"}) || !valid) {
            throw OpError("a hand is a pose (" + join(mesh3d::kHandPoses) + ") or its fingers' curls (" + join(mesh3d::kFingers) +
                          ": 0 straight to 1 closed)");
        }
        Json rounded = Json::array();
        for (const double c : mesh3d::hand_curls(pose)) rounded.push_back(core::py_round(c, 3));
        return Json::object({{"pose", base}, {"curls", std::move(rounded)}});
    }
    throw OpError(hand_poses_message());
}

Json hands_of(const Json& raw) {
    if (!raw.is_object()) throw OpError("hands is {l: pose, r: pose}");
    Json out = Json::object();
    for (const auto& [side, pose] : raw.items()) {
        if (side != "l" && side != "r") throw OpError(hand_poses_message());
        out[side] = hand_pose(pose);
    }
    return out;
}

Json joints_of(const Json& raw) {
    if (!raw.is_object()) throw OpError("joints is {name: {x, y, z}}");
    Json out = Json::object();
    for (const auto& [name, values] : raw.items()) {
        if (std::find(mesh3d::kFigureJoints.begin(), mesh3d::kFigureJoints.end(), name) == mesh3d::kFigureJoints.end()) {
            throw OpError("unknown joint " + name);
        }
        bool axes = values.is_object();
        if (axes) {
            for (const auto& [k, v] : values.items()) axes = axes && (k == "x" || k == "y" || k == "z");
        }
        if (!axes) throw OpError("each joint is {x, y, z} in radians");
        Json joint = Json::object();
        for (const auto& [k, v] : values.items()) joint[k] = core::py_round(core::to_float(v), 4);
        out[name] = std::move(joint);
    }
    return out;
}

void figure_preset(Json& prim, const std::string& name) {
    const Json* data = get(mesh3d::figure_presets(), name);
    if (data == nullptr) {
        std::string names;
        for (const auto& [k, v] : mesh3d::figure_presets().items()) names += (names.empty() ? "" : ", ") + k;
        throw OpError("preset must be one of " + names);
    }
    Json joints = Json::object();
    for (const auto& [k, v] : data->items()) {
        if (k != "hands") joints[k] = core::py_dict(v);
    }
    prim["joints"] = std::move(joints);
    const Json* hands = get(*data, "hands");
    prim["hands"] = truthy(hands) ? core::py_dict(*hands) : Json::object();
    prim["preset"] = name;
}

// threeops._find: the first prim with the id, of one of the kinds
std::size_t find_prim(const Page& page, const Json& prim_id, std::initializer_list<std::string_view> kinds, const std::string& label) {
    for (std::size_t i = 0; i < page.prims.size(); ++i) {
        const Json* id = core::dict_get(page.prims[i], "id");
        if (!core::py_equals(id != nullptr ? *id : Json(), prim_id)) continue;
        const Json* kind = core::dict_get(page.prims[i], "kind");
        if (kinds.size() > 0 && !(kind != nullptr && core::is_one_of(*kind, kinds))) break;
        return i;
    }
    throw OpError("no " + label + " " + core::py_str(prim_id));
}

// ops._guide_frame: the panel a 3D guide stays inside (the one named, else the one under its centre)
std::optional<std::string> guide_frame(const Page& page, const Json& op, const Json& pos) {
    const std::vector<const core::Frame*> leaves = page.leaf_frames();
    const Json* frame_id = get(op, "frame_id");
    if (truthy(frame_id)) {
        const bool known = frame_id->is_string() && std::any_of(leaves.begin(), leaves.end(), [&](const core::Frame* f) {
                               return f->id == frame_id->get_ref<const std::string&>();
                           });
        if (!known) throw OpError("no frame " + core::py_str(*frame_id));
        return core::py_str(*frame_id);
    }
    if (frame_id != nullptr && frame_id->is_boolean() && !frame_id->get<bool>()) return std::nullopt;
    for (const core::Frame* leaf : leaves) {
        if (core::contains(*leaf, Num(core::to_float(pos[0])), Num(core::to_float(pos[1])))) return leaf->id;
    }
    return std::nullopt;
}

// threeops._frame
void attach_frame(const Page& page, const Json& op, Json& prim) {
    if (!truthy(get(op, "frame_id"))) return;
    if (const auto frame = guide_frame(page, op, prim["pos"])) prim["frame_id"] = *frame;
}

const Json* page_camera(const Page& page) { return get(page.extra, "camera"); }

// coerce_stroke([(round(x, 3), round(y, 3)) …]) with the op's kind, width and colour
core::StrokePtr traced_stroke(const prim3d::Line2& line, const std::string& kind, const Json& op) {
    auto stroke = std::make_shared<core::Stroke>();
    for (const auto& p : line) stroke->points.push_back(core::PointF{core::py_round(p[0], 3), core::py_round(p[1], 3)});
    stroke->id = core::new_id();
    stroke->kind = kind;
    const Json* width = get(op, "width_mm");
    stroke->width_mm = truthy(width) ? core::to_float(*width) : 0.3;
    const Json* rgb = get(op, "rgb");
    if (truthy(rgb)) {
        std::vector<std::int64_t> values;
        for (const Json& v : core::py_list(*rgb)) values.push_back(core::to_int(v));
        stroke->rgb = std::move(values);
    }
    return stroke;
}

// The traced lines are refused when a number of theirs is not finite (Python adds them to the layer): checked once the
// op has done everything else.
void require_finite_lines(const std::vector<core::StrokePtr>& strokes) {
    for (const core::StrokePtr& stroke : strokes) {
        for (const core::PointF& p : stroke->points) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw OpError("the 3D gives a line that is not on the page (not finite)");
        }
        if (!std::isfinite(stroke->width_mm)) throw OpError("width_mm must be a finite number");
    }
}

void add_strokes(core::Layer& target, std::vector<core::StrokePtr> more) {
    if (more.empty()) return;
    std::vector<core::StrokePtr> items = target.strokes ? target.strokes->items : std::vector<core::StrokePtr>{};
    items.insert(items.end(), more.begin(), more.end());
    target.strokes = core::make_strokes(std::move(items));
}

// set(op.get("ids") or []) and `p.get("id") in ids`
std::vector<Json> ids_of(const Json& op) {
    static const Json kNone = Json::array();
    std::vector<Json> out;
    for (const Json& id : core::py_list(get_else(op, "ids", kNone))) {
        core::require_hashable(id);
        out.push_back(id);
    }
    return out;
}

bool chosen(const Json& prim, const std::vector<Json>& ids) {
    if (ids.empty()) return true;
    const Json* id = core::dict_get(prim, "id");
    const Json value = id != nullptr ? *id : Json();
    core::require_hashable(value);
    return std::any_of(ids.begin(), ids.end(), [&](const Json& want) { return core::py_equals(value, want); });
}

// --- the ops ------------------------------------------------------------------------------------------------------

void add_figure(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    const Json* height_value = get(c.op, "height_mm");
    const double height = truthy(height_value) ? core::to_float(*height_value) : 90.0;
    if (!(10 <= height && height <= 400)) throw OpError("height_mm must be between 10 and 400");
    static const Json kPos = Json::array({100, 160, 0});
    static const Json kRot = Json::array({0, 0, 0});
    static const Json kNone = Json::object();
    Json prim = Json::object();
    prim["id"] = id_of(c.op);
    prim["kind"] = "figure";
    prim["pos"] = vec(get_else(c.op, "pos", kPos), 3, "pos");
    prim["size"] = Json::array({height / 2, height, height / 4});
    prim["rot"] = vec(get_else(c.op, "rot", kRot), 3, "rot");
    prim["body"] = body_of(get_else(c.op, "body", kNone));
    prim["joints"] = Json::object();
    prim["hands"] = Json::object();
    if (const Json* focal = get(c.op, "focal_mm"); truthy(focal)) prim["focal_mm"] = py_max(20.0, core::to_float(*focal));
    if (const Json* preset = get(c.op, "preset"); truthy(preset)) figure_preset(prim, core::py_str(*preset));
    if (const Json* joints = get(c.op, "joints"); truthy(joints)) {
        const Json given = joints_of(*joints);
        for (const auto& [k, v] : given.items()) prim["joints"][k] = v;
    }
    if (const Json* hands = get(c.op, "hands"); truthy(hands)) {
        const Json given = hands_of(*hands);
        for (const auto& [k, v] : given.items()) prim["hands"][k] = v;
    }
    Page& page = c.doc.edit_page(pi);
    for (const Json& p : page.prims) {
        const Json* id = core::dict_get(p, "id");
        if (core::py_equals(id != nullptr ? *id : Json(), prim["id"])) throw OpError("3D " + prim["id"].get<std::string>() + " exists");
    }
    attach_frame(page, c.op, prim);
    core::require_finite(prim);
    page.prims.push_back(std::move(prim));
}

// owner.setdefault(key, {}): what is there (made when nothing is)
Json& setdefault(Json& owner, const std::string& key) {
    if (!owner.is_object()) core::raise_attribute_error(owner, "setdefault");
    if (!owner.contains(key)) owner[key] = Json::object();
    return owner[key];
}

// owner.setdefault(key, {}) where the value is used as a dict next (Python fails where it is something else)
Json& dict_at(Json& owner, const std::string& key) {
    Json& value = setdefault(owner, key);
    if (!value.is_object()) core::raise_attribute_error(value, "setdefault");
    return value;
}

void merge_into(Json& slot, const Json& values) {
    if (!slot.is_object()) core::raise_attribute_error(slot, "update");
    for (const auto& [k, v] : values.items()) slot[k] = v;
}

void pose_figure(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(pi);
    const Json* id = get(c.op, "id");
    const std::size_t at = find_prim(page, id != nullptr ? *id : Json(), {"figure", "hand"}, "figure/hand");
    Json prim = page.prims[at];
    if (core::py_equals(prim["kind"], Json("hand"))) {
        if (const Json* pose = get(c.op, "pose"); truthy(pose)) {
            if (!core::is_one_of(*pose, {"open", "relaxed", "fist", "point", "peace", "grip"})) throw OpError(hand_poses_message());
            prim["pose"] = core::py_str(*pose);
            prim.erase("curls");
        }
        if (const Json* curls = get(c.op, "curls"); curls != nullptr && !curls->is_null()) {
            const Json* pose = core::dict_get(prim, "pose");
            prim["curls"] = hand_pose(Json::object({{"pose", truthy(pose) ? *pose : Json("relaxed")}, {"curls", *curls}}))["curls"];
        }
        for (const char* key : {"rot", "pos"}) {
            if (const Json* v = get(c.op, key); v != nullptr && !v->is_null()) prim[key] = vec(*v, 3, key);
        }
        core::require_finite(prim);
        page.prims[at] = std::move(prim);
        return;
    }
    if (const Json* preset = get(c.op, "preset"); truthy(preset)) figure_preset(prim, core::py_str(*preset));
    if (const Json* set = get(c.op, "set_joints")) {
        static const Json kNone = Json::object();
        prim["joints"] = joints_of(core::py_truthy(*set) ? *set : kNone);
    }
    if (const Json* joints = get(c.op, "joints"); truthy(joints)) {
        Json& mine = setdefault(prim, "joints");
        const Json given = joints_of(*joints);
        for (const auto& [joint, values] : given.items()) merge_into(dict_at(mine, joint), values);
    }
    if (const Json* body = get(c.op, "body"); truthy(body)) {
        const Json* own = core::dict_get(prim, "body");
        Json merged = Json::object();
        if (truthy(own)) {
            if (!own->is_object()) throw core::PyTypeError("'" + core::py_type_name(*own) + "' object is not a mapping");
            merged = *own;
        }
        const Json given = body_of(*body);
        for (const auto& [k, v] : given.items()) merged[k] = v;
        prim["body"] = std::move(merged);
    }
    if (const Json* hands = get(c.op, "hands"); truthy(hands)) {
        const Json* own = core::dict_get(prim, "hands");
        Json merged = Json::object();
        if (truthy(own)) {
            if (!own->is_object()) throw core::PyTypeError("'" + core::py_type_name(*own) + "' object is not a mapping");
            merged = *own;
        }
        const Json given = hands_of(*hands);
        for (const auto& [k, v] : given.items()) merged[k] = v;
        prim["hands"] = std::move(merged);
    }
    for (const char* key : {"rot", "pos"}) {
        if (const Json* v = get(c.op, key); v != nullptr && !v->is_null()) prim[key] = vec(*v, 3, key);
    }
    std::optional<double> height;
    if (const Json* height_value = get(c.op, "height_mm"); truthy(height_value)) {
        height = core::to_float(*height_value);
        prim["size"] = Json::array({*height / 2, *height, *height / 4});
    }
    if (const Json* drag_value = get(c.op, "drag"); truthy(drag_value)) {
        const Json drag = core::py_dict(*drag_value);
        static const Json kOrigin = Json::array({0, 0});
        const Json to = vec(get_else(drag, "to", kOrigin), 2, "to");
        const mesh3d::Point2 target{to[0].get<double>(), to[1].get<double>()};
        const Json* camera = page_camera(page);
        const Json* handle = get(drag, "handle");
        const std::string handle_name = handle != nullptr ? core::py_str(*handle) : std::string("None");
        const Json change = truthy(get(drag, "ik")) ? mesh3d::reach(prim, handle_name, target, camera)
                                                    : mesh3d::drag_joint(prim, handle_name, target, camera);
        if (!all_finite(change)) throw OpError("to gives a pose that is not finite");  // (Python would keep it)
        if (change.contains("pos")) prim["pos"] = change["pos"];
        if (const Json* joints = get(change, "joints")) {
            for (const auto& [joint, values] : joints->items()) merge_into(dict_at(dict_at(prim, "joints"), joint), values);
        }
        prim.erase("preset");
    }
    if (height) require_finite_value(*height, "height_mm");
    core::require_finite(prim);
    page.prims[at] = std::move(prim);
}

void add_head_or_hand(OpContext& c, bool head) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    const Json* size_value = get(c.op, "size_mm");
    const double size = truthy(size_value) ? core::to_float(*size_value) : (head ? 30.0 : 25.0);
    static const Json kPos = Json::array({100, 100, 0});
    static const Json kRot = Json::array({0, 0, 0});
    Json prim = Json::object();
    prim["id"] = id_of(c.op);
    prim["kind"] = head ? "head" : "hand";
    prim["pos"] = vec(get_else(c.op, "pos", kPos), 3, "pos");
    prim["size"] = Json::array({size, size, size});
    prim["rot"] = vec(get_else(c.op, "rot", kRot), 3, "rot");
    if (!head) {
        const Json* side_value = get(c.op, "side");
        const std::string side = truthy(side_value) ? core::py_str(*side_value) : std::string("r");
        if (side != "l" && side != "r") throw OpError("side is l or r");
        const Json* pose_value = get(c.op, "pose");
        const std::string pose = truthy(pose_value) ? core::py_str(*pose_value) : std::string("relaxed");
        if (!core::is_one_of(Json(pose), {"open", "relaxed", "fist", "point", "peace", "grip"})) throw OpError(hand_poses_message());
        prim["side"] = side;
        prim["pose"] = pose;
    }
    Page& page = c.doc.edit_page(pi);
    attach_frame(page, c.op, prim);
    require_finite_value(size, "size_mm");
    core::require_finite(prim);
    page.prims.push_back(std::move(prim));
}

void add_head(OpContext& c) { add_head_or_hand(c, true); }
void add_hand(OpContext& c) { add_head_or_hand(c, false); }

// What a model file's reading raised, as import_model reports it.
[[noreturn]] void model_error(const std::string& message, bool own) {
    throw OpError(own ? message : "the model cannot be read (" + message + ")");
}

void import_model(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    const Json* text = get(c.op, "obj");
    Json mesh;
    try {
        if (const Json* glb = get(c.op, "glb"); truthy(glb)) {  // a .glb / .vrm (base64), or a .gltf's text
            mesh = mesh3d::read_gltf(core::a2b_base64(core::py_str(*glb)));
        } else if (const Json* gltf = get(c.op, "gltf"); truthy(gltf)) {
            mesh = mesh3d::read_gltf(core::py_str(*gltf));
        } else {
            if (text == nullptr || !text->is_string() || core::is_blank(text->get_ref<const std::string&>())) {
                throw OpError("obj is the model's OBJ text");
            }
            mesh = mesh3d::read_obj(text->get_ref<const std::string&>());
        }
    } catch (const mesh3d::ObjError& error) {
        model_error(error.what(), true);
    } catch (const OpError&) {
        throw;
    } catch (const core::OpKeyError& error) {
        model_error(error.what(), false);
    } catch (const core::PyUncaught& error) {
        // (except (ObjError, ValueError, IndexError, KeyError, TypeError): the others go through, as in Python)
        if (error.type() != "IndexError") throw;
        model_error(error.what(), false);
    } catch (const core::Error& error) {
        model_error(error.what(), false);
    }
    const Json* size_value = get(c.op, "size_mm");
    const double longest = truthy(size_value) ? core::to_float(*size_value) : 60.0;
    const Json ratio = mesh["ratio"];
    mesh.erase("ratio");
    static const Json kPos = Json::array({100, 150, 0});
    static const Json kRot = Json::array({0.2, 0.5, 0});
    Json prim = Json::object();
    prim["id"] = id_of(c.op);
    prim["kind"] = "mesh";
    prim["pos"] = vec(get_else(c.op, "pos", kPos), 3, "pos");
    prim["size"] = Json::array({longest, longest, longest});
    prim["rot"] = vec(get_else(c.op, "rot", kRot), 3, "rot");
    prim["mesh"] = std::move(mesh);
    const Json* name = get(c.op, "name");
    prim["title"] = truthy(name) ? core::py_str(*name) : std::string("モデル");
    prim["ratio"] = ratio;
    Page& page = c.doc.edit_page(pi);
    attach_frame(page, c.op, prim);
    require_finite_value(longest, "size_mm");
    core::require_finite(prim);
    page.prims.push_back(std::move(prim));
}

void set_camera(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(pi);
    if (truthy(get(c.op, "off"))) {
        page.extra.erase("camera");
        return;
    }
    const Json* own = get(page.extra, "camera");
    Json camera = core::py_dict(truthy(own) ? *own : Json::object());
    for (const char* key : {"turn", "tip", "roll"}) {
        if (const Json* v = get(c.op, key); v != nullptr && !v->is_null()) camera[key] = core::py_round(core::to_float(*v), 4);
    }
    if (const Json* focal_value = get(c.op, "focal_mm"); focal_value != nullptr && !focal_value->is_null()) {
        const double focal = core::to_float(*focal_value);
        if (!(20 <= focal && focal <= 5000)) throw OpError("focal_mm must be between 20 and 5000");
        camera["focal_mm"] = focal;
    }
    if (const Json* target = get(c.op, "target")) camera["target"] = core::py_truthy(*target) ? vec(*target, 2, "target") : Json();
    if (!camera.contains("target")) camera["target"] = Json::array({(page.spec.width_mm / Num(2)).json(), (page.spec.height_mm / Num(2)).json()});
    core::require_finite(camera);
    page.extra["camera"] = std::move(camera);
}

void set_light(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(pi);
    const Json* own = get(page.extra, "light");
    Json light = core::py_dict(truthy(own) ? *own : Json::object());
    if (const Json* dir = get(c.op, "dir"); dir != nullptr && !dir->is_null()) {
        const Json d = vec(*dir, 3, "dir");
        if (core::py_hypot(d[0].get<double>(), d[1].get<double>(), d[2].get<double>()) < 1e-6) throw OpError("dir must point somewhere");
        light["dir"] = d;
    }
    if (const Json* ambient = get(c.op, "ambient"); ambient != nullptr && !ambient->is_null()) {
        light["ambient"] = py_max(0.0, py_min(1.0, core::to_float(*ambient)));
    }
    core::require_finite(light);
    page.extra["light"] = std::move(light);
}

void render_prims(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(pi);
    core::Layer& target = core::paint_target(page, c.op);
    const std::vector<Json> ids = ids_of(c.op);
    const Json* camera_value = page_camera(page);
    std::vector<Json> picked;           // the prims, each as with_camera gives it (for the lines)
    std::vector<const Json*> cameras;   // the camera each is seen with
    // camera_of may return a pointer inside a prim: keep this copy alive until every line has been traced.
    const Json prims = core::py_list(page.prims);
    for (const Json& p : prims) {
        if (!chosen(p, ids)) continue;
        const Json* kind = core::dict_get(p, "kind");
        if (kind != nullptr && core::py_equals(*kind, Json("mannequin"))) continue;
        picked.push_back(p);
        cameras.push_back(mesh3d::camera_of(p, camera_value));
    }
    if (picked.empty()) throw OpError("no 3D figure or box to trace");
    const Json* surfaces = get(c.op, "surfaces");
    if (surfaces == nullptr || core::py_truthy(*surfaces)) {
        const Json* own = get(page.extra, "light");
        Json light = core::py_dict(truthy(own) ? *own : Json::object());
        if (const Json* dir = get(c.op, "light"); dir != nullptr && !dir->is_null()) light["dir"] = vec(*dir, 3, "light");
        const Json* ambient_value = get(c.op, "ambient");
        if (ambient_value == nullptr) ambient_value = get(light, "ambient");
        const double ambient = ambient_value != nullptr ? core::to_float(*ambient_value) : 0.35;
        const double dpi = kWorkingDpi;
        const double fw = std::nearbyint(page.spec.width_mm.value() / 25.4 * dpi);
        const double fh = std::nearbyint(page.spec.height_mm.value() / 25.4 * dpi);
        if (!std::isfinite(fw) || !std::isfinite(fh) || fw > 1e9 || fh > 1e9) throw core::PyValueError("cannot convert float to integer");
        const int sw = static_cast<int>(fw);
        const int sh = static_cast<int>(fh);
        // (the prims without the camera they carry; the page's camera for all of them, as Python's raster has it)
        std::vector<Json> bare;
        for (const Json& p : picked) {
            Json own_prim = Json::object();
            for (const auto& [k, v] : p.items()) {
                if (k != "camera") own_prim[k] = v;
            }
            bare.push_back(std::move(own_prim));
        }
        const Json* dir = get(light, "dir");
        mesh3d::Raster r;
        if (sw > 0 && sh > 0) r = mesh3d::raster(bare, sw, sh, dpi, camera_value, dir, ambient);
        if (!r.any()) throw OpError("the 3D has no surfaces to shade here");
        // grey = np.where(alpha, np.clip(shade * 255, 0, 255), 0).astype("uint8"): float32 throughout
        std::string bytes(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh) * 4, '\0');
        for (std::size_t i = 0; i < r.zbuf.size(); ++i) {
            if (!r.alpha(i)) continue;
            const float v = r.shade[i] * 255.0f;
            const float clipped = std::isnan(v) ? v : std::min(std::max(v, 0.0f), 255.0f);
            const auto grey = static_cast<unsigned char>(std::isfinite(clipped) ? static_cast<int>(clipped) : 0);
            bytes[i * 4] = static_cast<char>(grey);
            bytes[i * 4 + 1] = static_cast<char>(grey);
            bytes[i * 4 + 2] = static_cast<char>(grey);
            bytes[i * 4 + 3] = static_cast<char>(255);
        }
        const Image picture = Image::frombytes("RGBA", Size{sw, sh}, bytes);
        Image base = target.raster_png && !target.raster_png->empty() ? open_image(*target.raster_png, kPillowOpenLimits).convert("RGBA")
                                                                       : Image::create("RGBA", Size{sw, sh}, Ink{0, 0, 0, 0});
        base = base.convert("RGBA").resize(Size{sw, sh});
        base.alpha_composite(picture);
        target.raster_png = std::make_shared<const std::string>(write_png(base));
        target.raster_memo = {};
        // layer.raster_relpath = f"pages/{page.index:03d}/{layer.role.value}.png"
        if (!page.index.is_int()) throw core::PyValueError("Unknown format code 'd' for object of type 'float'");
        char rel[64];
        std::snprintf(rel, sizeof rel, "pages/%03lld/", static_cast<long long>(page.index.int_value()));
        target.raster_relpath = std::string(rel) + std::string(core::to_string(target.role)) + ".png";
        if (const Json* tone = get(c.op, "tone"); truthy(tone)) {
            Json raw = Json::object({{"pattern", "dot"}});
            const Json given = core::py_dict(*tone);
            for (const auto& [k, v] : given.items()) raw[k] = v;
            target.screen = core::screen_spec(raw);
        }
    }
    std::vector<core::StrokePtr> made;
    const Json* lines = get(c.op, "lines");
    if (lines == nullptr || core::py_truthy(*lines)) {
        static const Json kMili = "mili";
        const std::string kind = core::brush_kind(get_else(c.op, "kind", kMili), c.doc);
        for (std::size_t i = 0; i < picked.size(); ++i) {
            for (const prim3d::Line2& line : prim3d::trace(picked[i], cameras[i])) made.push_back(traced_stroke(line, kind, c.op));
        }
    }
    if (target.screen) core::require_finite(*target.screen, "tone");
    require_finite_lines(made);
    add_strokes(target, std::move(made));
}

// --- the older 3D ops (ops._apply_one) -----------------------------------------------------------------------------

// The values a mannequin is drawn from, when an op has put them there unchecked (Python stores them as given, and a
// page with one that is not a number can no longer be drawn): pos [x, y], rot [tip, turn, lean], the joints' yaw and
// pitch, a height above 0. `kind` is the prim's (pose_mannequin also reaches other prims: then their own renderer's
// rules apply).
void check_drawable(const Json& prim, bool mannequin, bool check_pos, bool check_rot, const Json* joints_given) {
    const auto number = [](const Json& v) {
        try {
            return std::isfinite(core::to_float(v));
        } catch (const core::Error&) {
            return false;
        }
    };
    const auto real = [](const Json& v) { return (v.is_number() || v.is_boolean()) && std::isfinite(core::to_float(v)); };
    if (check_pos) {
        const Json* pos = core::dict_get(prim, "pos");
        static const Json kDefault = Json::array({100, 160, 0});
        const Json items = core::py_list(truthy(pos) ? *pos : kDefault);
        const std::size_t needed = mannequin ? 2 : 3;
        bool ok = items.size() >= 2;
        for (std::size_t i = 0; ok && i < std::min(needed, items.size()); ++i) ok = number(items[i]);
        if (!ok) throw OpError("pos is [x, y, z]");
    }
    if (check_rot) {
        const Json* rot = core::dict_get(prim, "rot");
        static const Json kDefault = Json::array({0, 0, 0});
        const Json items = core::py_list(truthy(rot) ? *rot : kDefault);
        for (std::size_t i = 0; i < std::min<std::size_t>(3, items.size()); ++i) {
            if (!(mannequin ? number(items[i]) : real(items[i]))) throw OpError("rot is [tip, turn, lean]");
        }
    }
    if (joints_given != nullptr) {
        const Json* joints = core::dict_get(prim, "joints");
        if (joints != nullptr && !joints->is_object()) throw OpError("joints is {name: {yaw, pitch}}");
        for (const auto& [name, given] : joints_given->items()) {
            const Json& slot = (*joints)[name];
            if (!slot.is_object()) throw OpError("each joint is {yaw, pitch} in radians");
            for (const auto& [axis, value] : slot.items()) {
                const bool read = !mannequin || axis == "yaw" || axis == "pitch";
                if (read && !number(value)) throw OpError("each joint is {yaw, pitch} in radians");
            }
        }
    }
}

// A mannequin's height as an op gives it: finite and not below 0 (its head would be drawn inside out), checked once the
// op has done everything else.
void check_mannequin_height(double height) {
    require_finite_value(height, "height_mm");
    if (height < 0) throw OpError("a mannequin's height must be above 0");
}

void add_mannequin(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    const Json* height_value = get(c.op, "height_mm");
    const double height = truthy(height_value) ? core::to_float(*height_value) : 80.0;
    static const Json kPos = Json::array({100, 160, 0});
    static const Json kRot = Json::array({0, 0, 0});
    Json prim = Json::object();
    prim["id"] = id_of(c.op);
    prim["kind"] = "mannequin";
    prim["pos"] = core::py_list(get_else(c.op, "pos", kPos));
    prim["size"] = Json::array({height / 2, height, height / 4});
    prim["rot"] = core::py_list(get_else(c.op, "rot", kRot));
    prim["joints"] = core::mannequin::default_joints();
    if (const Json* preset = get(c.op, "preset"); truthy(preset)) {
        try {
            core::mannequin::apply_preset(prim, core::py_str(*preset));
        } catch (const core::Error& error) {
            throw OpError(error.what());
        }
    }
    check_mannequin_height(height);
    core::require_finite(prim);
    check_drawable(prim, true, true, true, nullptr);
    c.doc.edit_page(pi).prims.push_back(std::move(prim));
}

void pose_mannequin(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(pi);
    const Json* id_value = get(c.op, "id");
    const Json wanted = id_value != nullptr ? *id_value : Json();
    for (Json& stored : page.prims) {
        const Json* id = core::dict_get(stored, "id");
        if (!core::py_equals(id != nullptr ? *id : Json(), wanted)) continue;
        Json prim = stored;
        const Json* kind = core::dict_get(prim, "kind");
        const bool mannequin = kind != nullptr && core::py_equals(*kind, Json("mannequin"));
        if (!mannequin) throw OpError("3D " + core::py_str(wanted) + " is not a mannequin");
        const Json* preset = get(c.op, "preset");
        if (truthy(preset)) {
            try {
                core::mannequin::apply_preset(prim, core::py_str(*preset));
            } catch (const core::Error& error) {
                throw OpError(error.what());
            }
        }
        if (const Json* rot = get(c.op, "rot")) prim["rot"] = core::py_list(*rot);
        if (const Json* pos = get(c.op, "pos")) prim["pos"] = core::py_list(*pos);
        std::optional<double> height;
        if (const Json* height_value = get(c.op, "height_mm"); truthy(height_value)) {
            height = core::to_float(*height_value);
            prim["size"] = Json::array({*height / 2, *height, *height / 4});
        }
        Json touched = Json::object();
        if (const Json* joints = get(c.op, "joints")) {
            Json& mine = setdefault(prim, "joints");
            const Json given = core::py_dict(*joints);
            for (const auto& [name, values] : given.items()) {
                Json& slot = setdefault(mine, name);
                merge_into(slot, core::py_dict(values));
                touched[name] = true;
            }
        }
        const Json* drag_value = get(c.op, "drag");
        if (truthy(drag_value)) {
            const Json drag = core::py_dict(*drag_value);
            const Json* handle = get(drag, "handle");
            static const Json kOrigin = Json::array({0, 0});
            Json change;
            try {
                change = core::mannequin::pose_to(prim, handle != nullptr ? core::py_str(*handle) : std::string("None"),
                                                  get_else(drag, "to", kOrigin));
            } catch (const core::Error& error) {
                if (error.code() == "value") throw OpError(error.what());  // (ValueError: the op's own error)
                throw;
            }
            if (change.contains("pos")) {
                const Json& moved = change["pos"];
                if (!std::isfinite(moved[0].get<double>()) || !std::isfinite(moved[1].get<double>())) throw OpError("to is [x, y]");
                prim["pos"] = moved;
            }
            if (const Json* joints = get(change, "joints")) {
                for (const auto& [joint, values] : joints->items()) {
                    merge_into(dict_at(dict_at(prim, "joints"), joint), values);
                    touched[joint] = true;
                }
            }
        }
        if (truthy(drag_value) && !truthy(preset)) prim.erase("preset");  // (a hand-made pose is no longer the preset)
        if (height && mannequin) check_mannequin_height(*height);
        if (height) require_finite_value(*height, "height_mm");
        core::require_finite(prim);
        check_drawable(prim, mannequin, get(c.op, "pos") != nullptr, get(c.op, "rot") != nullptr || truthy(preset),
                       touched.empty() ? nullptr : &touched);
        stored = std::move(prim);
        return;
    }
    throw OpError("no mannequin " + core::py_str(wanted));
}

void add_prim3d(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    const Json* kind_value = get(c.op, "kind");
    const std::string kind = truthy(kind_value) ? core::py_str(*kind_value) : std::string("box");
    if (kind != "box" && kind != "cylinder" && kind != "stairs" && kind != "floor" && kind != "sphere" && kind != "cone" && kind != "prop") {
        throw OpError("kind must be box, cylinder, stairs, floor, sphere, cone or prop (figures: add_mannequin)");
    }
    static const Json kFloor = Json::array({160, 1, 160});
    static const Json kBox = Json::array({40, 40, 40});
    Json size = get_else(c.op, "size", kind == "floor" ? kFloor : kBox);
    if (!size.is_array()) {
        const double s = core::to_float(size);
        size = Json::array({s, s, s});
    }
    static const Json kPos = Json::array({100, 150, 0});
    static const Json kRot = Json::array({0.35, 0.6, 0});
    Json prim = Json::object();
    prim["id"] = id_of(c.op);
    prim["kind"] = kind;
    prim["pos"] = vec3(get_else(c.op, "pos", kPos));
    prim["size"] = vec3(size);
    prim["rot"] = vec3(get_else(c.op, "rot", kRot));
    if (const Json* focal = get(c.op, "focal_mm"); truthy(focal)) prim["focal_mm"] = py_max(20.0, core::to_float(*focal));
    Page& page = c.doc.edit_page(pi);
    if (truthy(get(c.op, "frame_id"))) {
        const auto frame = guide_frame(page, c.op, prim["pos"]);
        prim["frame_id"] = frame ? Json(*frame) : Json();
    }
    if (kind == "prop") {
        const Json* prop = get(c.op, "prop");
        prim["prop"] = truthy(prop) ? core::py_str(*prop) : std::string("chair");
        if (prim3d::prop_boxes(prim["prop"].get<std::string>()) == nullptr) throw OpError("prop must be one of " + join(prim3d::kProps));
    }
    if (kind == "stairs") {
        const Json* steps = get(c.op, "steps");
        prim["steps"] = std::max<std::int64_t>(2, std::min<std::int64_t>(30, truthy(steps) ? core::to_int(*steps) : 6));
    }
    if (kind == "floor") {
        const Json* lines = get(c.op, "lines");
        prim["lines"] = std::max<std::int64_t>(2, std::min<std::int64_t>(40, truthy(lines) ? core::to_int(*lines) : 8));
        if (!truthy(get(c.op, "rot"))) prim["rot"] = Json::array({-1.2, 0.5, 0});  // seen from above at a slant
    }
    core::require_finite(prim);
    page.prims.push_back(std::move(prim));
}

void add_scene(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    const Json* kind_value = get(c.op, "kind");
    const std::string kind = truthy(kind_value) ? core::py_str(*kind_value) : std::string("room");
    if (std::find(prim3d::kScenes.begin(), prim3d::kScenes.end(), kind) == prim3d::kScenes.end()) {
        throw OpError("scene kind must be one of " + join(prim3d::kScenes));
    }
    const Page& before = c.doc.page(pi);
    const Json usual = prim3d::scene_size(kind);
    Json size = get_else(c.op, "size", usual);
    if (!size.is_array()) {
        const double s = core::to_float(size);
        Json scaled = Json::array();
        for (const Json& v : usual) scaled.push_back(s * v.get<double>() / usual[0].get<double>());
        size = std::move(scaled);
    }
    const Json size3 = vec3(size);
    const Json* focal_value = get(c.op, "focal_mm");
    const double focal = py_max(20.0, truthy(focal_value) ? core::to_float(*focal_value) : 220.0);
    const auto [rot, near] = prim3d::scene_view(kind);
    // (depth: the scene's near end sits `near` × focal from the camera plane; inside for corridors)
    const double depth = size3[2].get<double>() / 2 + near * focal;
    Json prim = Json::object();
    prim["id"] = id_of(c.op);
    prim["kind"] = "scene";
    prim["scene"] = kind;
    const Json centre = Json::array({(before.spec.width_mm / Num(2)).json(), (before.spec.height_mm / Num(2)).json(), depth});
    prim["pos"] = vec3(get_else(c.op, "pos", centre));
    prim["size"] = size3;
    prim["rot"] = vec3(get_else(c.op, "rot", rot));
    prim["focal_mm"] = focal;
    Page& page = c.doc.edit_page(pi);
    if (const auto frame = guide_frame(page, c.op, prim["pos"])) prim["frame_id"] = *frame;
    // (the scene's depth, and so its pos, comes from the focal length and the size: those are named first)
    require_finite_value(focal, "focal_mm");
    core::require_finite(prim["size"], "size");
    core::require_finite(prim);
    page.prims.push_back(std::move(prim));
}

// ops._prim
std::size_t prim_at(const Page& page, const Json& prim_id) {
    for (std::size_t i = 0; i < page.prims.size(); ++i) {
        const Json* id = core::dict_get(page.prims[i], "id");
        if (core::py_equals(id != nullptr ? *id : Json(), prim_id)) return i;
    }
    throw OpError("no 3D figure or box " + core::py_str(prim_id));
}

void edit_prim(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(pi);
    const Json* id = get(c.op, "id");
    const std::size_t at = prim_at(page, id != nullptr ? *id : Json());
    Json prim = page.prims[at];
    for (const char* key : {"pos", "size", "rot"}) {
        if (const Json* v = get(c.op, key); v != nullptr && !v->is_null()) prim[key] = vec3(*v);
    }
    if (const Json* focal = get(c.op, "focal_mm"); truthy(focal)) prim["focal_mm"] = py_max(20.0, core::to_float(*focal));
    const Json* kind = core::dict_get(prim, "kind");
    if (kind != nullptr && core::py_equals(*kind, Json("mannequin")) && get(c.op, "size") != nullptr && !get(c.op, "size")->is_null()) {
        const double height = prim["size"][1].get<double>();
        if (height < 0) throw OpError("a mannequin's height must be above 0");
        prim["size"] = Json::array({height / 2, height, height / 4});
    }
    core::require_finite(prim);
    page.prims[at] = std::move(prim);
}

void delete_prim(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(pi);
    const Json* id = get(c.op, "id");
    const Json wanted = id != nullptr ? *id : Json();
    prim_at(page, wanted);
    Json kept_prims = Json::array();
    for (const Json& p : page.prims) {
        const Json* own = core::dict_get(p, "id");
        if (!core::py_equals(own != nullptr ? *own : Json(), wanted)) kept_prims.push_back(p);
    }
    page.prims = std::move(kept_prims);
}

void trace_prims(OpContext& c) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(pi);
    core::Layer& target = core::paint_target(page, c.op);
    const std::vector<Json> ids = ids_of(c.op);
    std::vector<std::size_t> picked;
    for (std::size_t i = 0; i < page.prims.size(); ++i) {
        if (chosen(page.prims[i], ids)) picked.push_back(i);
    }
    if (picked.empty()) throw OpError("no 3D figure or box to trace");
    static const Json kPencil = "pencil";
    const std::string kind = core::brush_kind(get_else(c.op, "kind", kPencil), c.doc);
    const Json* camera_value = page_camera(page);
    std::vector<core::StrokePtr> made;
    for (const std::size_t i : picked) {
        const Json& prim = page.prims[i];
        for (const prim3d::Line2& line : prim3d::join_lines(prim3d::trace(prim, mesh3d::camera_of(prim, camera_value)))) {
            made.push_back(traced_stroke(line, kind, c.op));
        }
    }
    require_finite_lines(made);
    add_strokes(target, std::move(made));
}

void ruler_or_camera(OpContext& c, bool ruler) {
    const std::size_t pi = core::require_page(c.doc, c.op);
    Page& page = c.doc.edit_page(pi);
    std::vector<std::size_t> prims;
    for (std::size_t i = 0; i < page.prims.size(); ++i) {
        const Json* kind = core::dict_get(page.prims[i], "kind");
        if (!(kind != nullptr && core::py_equals(*kind, Json("mannequin")))) prims.push_back(i);
    }
    const Json* prim_id = get(c.op, "prim_id");
    std::optional<std::size_t> at;
    if (truthy(prim_id)) {
        for (const std::size_t i : prims) {
            const Json* id = core::dict_get(page.prims[i], "id");
            if (core::py_equals(id != nullptr ? *id : Json(), *prim_id)) {
                at = i;
                break;
            }
        }
    } else if (!prims.empty()) {
        at = prims.front();
    }
    if (!at) throw OpError(truthy(prim_id) ? "no 3D " + core::py_str(*prim_id) : "no 3D on this page to match (prim_id)");
    const Json prim = page.prims[*at];
    const Json* camera = page_camera(page);
    if (ruler) {
        const std::optional<Json> made = core::persp3d::ruler_from(prim, camera);
        if (!made) throw OpError("the 3D is seen straight on: its lines stay parallel (no vanishing point)");
        const std::string ruler_id = id_of(c.op);
        for (const Json& r : core::py_list(page.rulers)) {
            const Json* id = core::dict_get(r, "id");
            if (core::py_equals(id != nullptr ? *id : Json(), Json(ruler_id))) throw OpError("ruler " + ruler_id + " already exists");
        }
        Json item = Json::object();
        item["id"] = ruler_id;
        for (const auto& [k, v] : made->items()) item[k] = v;
        item["active"] = true;
        item["visible"] = true;
        if (const Json* grid = get(c.op, "grid"); truthy(grid)) {
            const std::int64_t lines = core::to_int(*grid);
            if (!(0 <= lines && lines <= 60)) throw OpError("grid is 0 (none) to 60 lines");  // (rulers.validate)
            item["grid"] = lines;
        }
        core::require_finite(item);
        if (!page.rulers.is_array()) page.rulers = core::py_list(page.rulers);
        page.rulers.push_back(std::move(item));
        return;
    }
    const Json* ruler_id = get(c.op, "id");
    const Json wanted = ruler_id != nullptr ? *ruler_id : Json();
    const Json* found = nullptr;
    for (const Json& r : page.rulers) {
        const Json* id = core::dict_get(r, "id");
        if (core::py_equals(id != nullptr ? *id : Json(), wanted)) {
            found = &r;
            break;
        }
    }
    if (found == nullptr) throw OpError("no ruler " + core::py_str(wanted));
    const Json* kind = core::dict_get(*found, "kind");
    if (!(kind != nullptr && core::py_equals(*kind, Json("perspective")))) throw OpError("the camera follows a perspective ruler");
    const Json fallback = Json::object(
        {{"target", Json::array({(page.spec.width_mm / Num(2)).json(), (page.spec.height_mm / Num(2)).json()})}});
    Json made;
    try {
        made = core::persp3d::camera_for(*found, prim, truthy(camera) ? *camera : fallback);
    } catch (const core::Error& error) {
        if (error.code() == "value") throw OpError(error.what());  // (except ValueError: the op's own error)
        throw;
    }
    core::require_finite(made, "camera");
    page.extra["camera"] = std::move(made);
}

void ruler_from_3d(OpContext& c) { ruler_or_camera(c, true); }
void camera_from_ruler(OpContext& c) { ruler_or_camera(c, false); }

}  // namespace

void register_3d_ops(core::OpRegistry& registry) {
    registry.add("add_figure", add_figure);
    registry.add("pose_figure", pose_figure);
    registry.add("add_head", add_head);
    registry.add("add_hand", add_hand);
    registry.add("import_model", import_model);
    registry.add("set_camera", set_camera);
    registry.add("set_light", set_light);
    registry.add("render_prims", render_prims);
    registry.add("add_mannequin", add_mannequin);
    registry.add("pose_mannequin", pose_mannequin);
    registry.add("add_prim3d", add_prim3d);
    registry.add("add_scene", add_scene);
    registry.add("edit_prim", edit_prim);
    registry.add("delete_prim", delete_prim);
    registry.add("trace_prims", trace_prims);
    registry.add("ruler_from_3d", ruler_from_3d);
    registry.add("camera_from_ruler", camera_from_ruler);
}

}  // namespace genko::render
