#include "core/ops_util.hpp"

#include <cmath>

#include "core/ids.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace {

void collect_ids(const Frame& frame, std::vector<std::string>& out) {
    out.push_back(frame.id);
    for (const Frame& child : frame.children) collect_ids(child, out);
}

Json opt_string(const std::optional<std::string>& value) { return value ? Json(*value) : Json(nullptr); }
Json opt_num(const std::optional<Num>& value) { return value ? value->json() : Json(nullptr); }
Json opt_json(const std::optional<Json>& value) { return value ? *value : Json(nullptr); }
bool truthy(const std::optional<Json>& value) { return value && py_truthy(*value); }

}  // namespace

// models.stroke_to_dict
Json stroke_to_dict(const Stroke& stroke) {
    Json out = Json::object();
    out["id"] = stroke.id;
    Json points = Json::array();
    for (const PointF& p : stroke.points) points.push_back(Json::array({p.x, p.y}));
    out["points"] = std::move(points);
    Json pressure = Json::array();
    for (const double v : stroke.pressure) pressure.push_back(v);
    out["pressure"] = std::move(pressure);
    out["width_mm"] = stroke.width_mm;
    out["kind"] = stroke.kind;
    if (stroke.rgb) out["rgb"] = ints_json(*stroke.rgb);
    if (stroke.color_rgb) out["color_rgb"] = *stroke.color_rgb;
    if (stroke.opacity != 1.0) out["opacity"] = stroke.opacity;
    return out;
}

LayerRole role_from(const Json& value) {
    if (value.is_string()) {
        if (const auto role = layer_role_from(value.get_ref<const std::string&>())) return *role;
    }
    throw PyValueError(py_repr(value) + " is not a valid LayerRole");
}

std::size_t layer_by_id(const Page& page, std::string_view layer_id) {
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (page.layers[i].id == layer_id) return i;
    }
    throw OpError("no layer " + std::string(layer_id));
}

std::size_t layer_for_role(Page& page, LayerRole role) {
    Layer& layer = page.layer_for(role);
    return static_cast<std::size_t>(&layer - page.layers.data());
}

Layer& paint_target(Page& page, const Json& op) {
    Layer* target = nullptr;
    if (truthy_at(op, "layer_id")) {
        target = &page.layers[layer_by_id(page, py_str(op["layer_id"]))];
    } else {
        const Json* role = get(op, "layer");
        target = &page.layers[layer_for_role(page, role_from(Json(role != nullptr && py_truthy(*role) ? py_str(*role) : "ink")))];
    }
    if (target->locked) throw OpError("the layer is locked");
    if (target->kind != LayerKind::Strokes && target->kind != LayerKind::Raster && target->kind != LayerKind::Tone) {
        throw OpError("this layer cannot be painted on (choose a pen, paint or tone layer)");
    }
    return *target;
}

const Frame& frame_or_fail(const Page& page, const Json& frame_id) {
    const Frame* frame = page.find_frame(py_str(frame_id));
    if (frame == nullptr) throw OpError("no panel " + py_str(frame_id));
    return *frame;
}

std::size_t ruler_index(const Page& page, const Json& ruler_id) {
    for (std::size_t i = 0; i < page.rulers.size(); ++i) {
        if (py_equals(py_get(page.rulers[i], "id"), ruler_id)) return i;
    }
    throw OpError("no ruler " + py_str(ruler_id));
}

Json screen_spec(const Json& raw) {
    if (!raw.is_object()) throw OpError("screen is {pattern, lpi, angle}");
    const Json* pattern_value = get(raw, "pattern");
    const std::string pattern = pattern_value != nullptr && py_truthy(*pattern_value) ? py_str(*pattern_value) : "dot";
    if (pattern != "dot" && pattern != "line" && pattern != "cross" && pattern != "noise") {
        throw OpError("screen pattern must be dot, line, cross or noise");
    }
    const double lpi = to_float(get_or(raw, "lpi", Json(60)));
    if (!(10 <= lpi && lpi <= 150)) throw OpError("lpi must be between 10 and 150");
    const Json* shape_value = get(raw, "shape");
    const std::string shape = shape_value != nullptr && py_truthy(*shape_value) ? py_str(*shape_value) : "round";
    if (shape != "round" && shape != "square" && shape != "diamond" && shape != "ellipse") {
        throw OpError("screen shape must be one of round, square, diamond, ellipse");
    }
    Json spec = Json::object();
    spec["pattern"] = pattern;
    spec["lpi"] = lpi;
    spec["angle"] = py_fmod(to_float(get_or(raw, "angle", Json(45))), 180);
    spec["black"] = py_clamp(to_float(get_or(raw, "black", Json(0.1))), 0.0, 0.9);
    spec["white"] = py_clamp(to_float(get_or(raw, "white", Json(0.95))), 0.1, 1.0);
    if (shape != "round") spec["shape"] = shape;
    if (truthy_at(raw, "offset_mm")) {
        Json offset = Json::array();
        try {
            for (const Json& v : iterate(raw["offset_mm"])) offset.push_back(to_float(v));
        } catch (const PyTypeError&) {
            throw OpError("offset_mm is [x, y] in mm");
        } catch (const PyValueError&) {
            throw OpError("offset_mm is [x, y] in mm");
        }
        Json two = Json::array();
        for (std::size_t i = 0; i < offset.size() && i < 2; ++i) two.push_back(offset[i]);
        spec["offset_mm"] = std::move(two);
    }
    return spec;
}

void require_finite(const Json& value, const std::string& what) {
    if (value.is_number_float() && !std::isfinite(value.get<double>())) throw OpError(what + " must be a finite number");
    if (value.is_object()) {
        for (const auto& [key, item] : value.items()) require_finite(item, what.empty() ? key : what + "." + key);
    } else if (value.is_array()) {
        for (const Json& item : value) require_finite(item, what);
    }
}

LayerRole stroke_role(const std::string& layer_name) {
    if (layer_name == "ink") return LayerRole::Ink;
    if (layer_name == "name") return LayerRole::Name;
    return role_from(Json(layer_name));
}

bool gated(const Document& doc) { return doc.strict_gates || py_truthy(doc.studio); }

std::vector<std::int64_t> rgb3(const Json& value, std::string_view what) {
    std::vector<std::int64_t> rgb;
    try {
        for (const Json& v : iterate(value)) rgb.push_back(to_int(v));
    } catch (const PyTypeError&) {
        throw OpError(std::string(what) + " is [r, g, b]");
    } catch (const PyValueError&) {
        throw OpError(std::string(what) + " is [r, g, b]");
    }
    if (rgb.size() > 3) rgb.resize(3);
    if (rgb.size() != 3) throw OpError(std::string(what) + " is [r, g, b]");
    for (const auto v : rgb) {
        if (v < 0 || v > 255) throw OpError(std::string(what) + " is [r, g, b]");
    }
    return rgb;
}

std::string blend_mode(const Json& value) {
    const std::string mode = py_truthy(value) ? py_str(value) : std::string("normal");
    static const char* const kModes[] = {"normal",   "multiply",   "screen",     "add",       "overlay",
                                         "darken",   "lighten",    "color_burn", "color_dodge", "linear_burn",
                                         "soft_light", "hard_light", "difference", "exclusion", "subtract",
                                         "divide",   "hue",        "saturation", "color",     "luminosity"};
    for (const char* known : kModes) {
        if (mode == known) return mode;
    }
    throw OpError("unknown blend mode " + mode);
}

Json ints_json(const std::vector<std::int64_t>& values) {
    Json out = Json::array();
    for (const auto v : values) out.push_back(v);
    return out;
}

Json layer_to_dict(const Layer& layer) {
    const bool placed = layer.kind == LayerKind::Placed;
    Json out = Json::object();
    out["id"] = layer.id;
    out["role"] = to_string(layer.role);
    out["kind"] = to_string(layer.kind);
    out["visible"] = layer.visible;
    out["exportable"] = layer.exportable;
    Json strokes = Json::array();
    for (const StrokePtr& stroke : layer.strokes->items) strokes.push_back(stroke_to_dict(*stroke));
    out["strokes"] = std::move(strokes);
    out["raster_relpath"] = opt_string(layer.raster_relpath);
    out["fill_rgb"] = layer.fill_rgb && !layer.fill_rgb->empty() ? nums_json(*layer.fill_rgb) : Json(nullptr);
    out["lpi"] = opt_num(layer.lpi);
    out["density"] = opt_num(layer.density);
    if (layer.region) {
        Json region = Json::array();
        for (const Point& p : *layer.region) region.push_back(Json::array({p.x.json(), p.y.json()}));
        out["region"] = std::move(region);
    } else {
        out["region"] = nullptr;
    }
    out["opacity"] = layer.opacity;
    out["material_id"] = opt_string(layer.material_id);
    out["angle"] = layer.angle;
    out["title"] = layer.title;
    out["blend"] = layer.blend;
    out["clip"] = layer.clip;
    out["lock_alpha"] = layer.lock_alpha;
    out["parent_id"] = layer.parent_id;
    if (!layer.patches.empty()) {
        Json patches = Json::array();
        for (const Patch& patch : layer.patches) {
            Json item = patch.attrs;
            if (patch.asset) item["asset"] = *patch.asset;
            for (const auto& [key, value] : patch.after_asset.items()) item[key] = value;
            patches.push_back(std::move(item));
        }
        out["patches"] = std::move(patches);
    }
    if (layer.locked) out["locked"] = true;
    if (!layer.panel_clip) out["panel_clip"] = false;
    if (layer.panel_each) out["panel_each"] = true;
    if (truthy(layer.tone)) out["tone"] = *layer.tone;
    if (layer.mask) out["mask"] = Json::object({{"enabled", layer.mask->enabled}});
    if (layer.color && !layer.color->empty()) out["color"] = ints_json(*layer.color);
    if (layer.reference) out["reference"] = true;
    if (truthy(layer.fill)) out["fill"] = *layer.fill;
    if (truthy(layer.adjust)) out["adjust"] = *layer.adjust;
    if (truthy(layer.effect)) out["effect"] = *layer.effect;
    if (layer.color_prints) out["color_prints"] = true;
    if (truthy(layer.screen)) out["screen"] = *layer.screen;
    if (truthy(layer.source) && !placed) out["source"] = *layer.source;
    if (placed) {
        out["asset"] = opt_string(layer.asset);
        out["frame_id"] = opt_string(layer.frame_id);
        out["placement_mm"] = layer.placement_mm ? rect_to_json(*layer.placement_mm) : Json(nullptr);
        out["fit"] = layer.fit;
        out["clip_to"] = layer.clip_to;
        out["source"] = opt_json(layer.source);
        out["finish"] = opt_json(layer.finish);
    }
    return out;
}

std::vector<std::string> frame_ids(const Page& page) {
    std::vector<std::string> out;
    for (const Frame& frame : page.frames) collect_ids(frame, out);
    return out;
}

void FrameIdMap::set(const std::string& from, const std::string& to) {
    for (auto& [key, value] : items_) {
        if (key == from) {  // (a dict: the key keeps its place and takes the new value)
            value = to;
            return;
        }
    }
    items_.emplace_back(from, to);
}

const std::string* FrameIdMap::get(const std::string& from) const {
    for (const auto& [key, value] : items_) {
        if (key == from) return &value;
    }
    return nullptr;
}

void refresh_frame_ids(Frame& frame, FrameIdMap& map) {
    const std::string old = frame.id;
    frame.id = new_id();
    map.set(old, frame.id);
    for (Frame& child : frame.children) refresh_frame_ids(child, map);
}

}  // namespace genko::core
