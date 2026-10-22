#include "core/brush_dabs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

#include "core/error.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"

// PhotoCraft v0.2.0 crates/paint/src/brush.rs (lines 1-451: the brush model and its defaults), MIT:
// native/third_party/photocraft/NOTICE.md.

namespace genko::core {

namespace {

[[noreturn]] void bad(const std::string& message) { throw Error("value", "dabs " + message); }

bool is_ref(const std::string& ref) {
    if (ref.size() != 71 || ref.compare(0, 7, "sha256:") != 0) return false;
    return std::all_of(ref.begin() + 7, ref.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

constexpr std::array<const char*, 8> kControls{"off", "fade", "pressure", "tilt", "wheel", "rotation", "initial_direction", "direction"};
constexpr std::array<const char*, 10> kMaskModes{"multiply", "subtract", "darken", "overlay", "color_dodge",
                                                 "color_burn", "linear_burn", "hard_mix", "linear_height", "height"};
constexpr std::array<const char*, 4> kStyles{"noise", "canvas", "paper", "dots"};

// --- reading -----------------------------------------------------------------------------------------------------------

// The keys of an object, each one known (`at` names it in the errors).
void known_keys(const Json& data, std::initializer_list<const char*> keys, const std::string& at) {
    if (!data.is_object()) bad(at + " must be an object");
    for (const auto& [key, value] : data.items()) {
        if (std::none_of(keys.begin(), keys.end(), [&](const char* k) { return key == k; })) bad(at + " has an unknown setting: " + key);
    }
}

double number(const Json& data, const char* key, double fallback, double lo, double hi, const std::string& at) {
    const Json* value = get(data, key);
    if (value == nullptr) return fallback;
    const double v = value->is_number() ? value->get<double>() : std::nan("");
    if (!(std::isfinite(v) && lo <= v && v <= hi)) {
        bad(at + key + " must be a number between " + py_format_g(lo) + " and " + py_format_g(hi));
    }
    return v;
}

std::int64_t whole(const Json& data, const char* key, std::int64_t fallback, std::int64_t lo, std::int64_t hi, const std::string& at) {
    const Json* value = get(data, key);
    if (value == nullptr) return fallback;
    const bool ok = value->is_number_integer() &&
                    (value->is_number_unsigned() ? value->get<std::uint64_t>() <= static_cast<std::uint64_t>(hi)
                                                 : value->get<std::int64_t>() >= lo && value->get<std::int64_t>() <= hi);
    if (!ok || value->get<std::int64_t>() < lo) bad(at + key + " must be a whole number between " + std::to_string(lo) + " and " + std::to_string(hi));
    return value->get<std::int64_t>();
}

bool flag(const Json& data, const char* key, bool fallback, const std::string& at) {
    const Json* value = get(data, key);
    if (value == nullptr) return fallback;
    if (!value->is_boolean()) bad(at + key + " must be true or false");
    return value->get<bool>();
}

template <std::size_t N>
std::size_t choice(const Json& data, const char* key, std::size_t fallback, const std::array<const char*, N>& names, const std::string& at) {
    const Json* value = get(data, key);
    if (value == nullptr) return fallback;
    for (std::size_t i = 0; i < N; ++i) {
        if (value->is_string() && value->get_ref<const std::string&>() == names[i]) return i;
    }
    std::string listed;
    for (const char* n : names) listed += (listed.empty() ? "" : ", ") + std::string(n);
    bad(at + key + " must be one of " + listed);
}

// A tip: "round" (the computed one) or {"asset": ref}.
std::string tip_of(const Json& data, const char* key, const std::string& at) {
    const Json* value = get(data, key);
    if (value == nullptr || (value->is_string() && value->get_ref<const std::string&>() == "round")) return {};
    if (value->is_object()) {
        known_keys(*value, {"asset"}, at + key);
        const Json* asset = get(*value, "asset");
        if (asset != nullptr && asset->is_string() && is_ref(asset->get<std::string>())) return asset->get<std::string>();
    }
    bad(at + key + " must be \"round\" or {\"asset\": an asset ref (sha256:<64 hex>)}");
}

DabDynamic dynamic(const Json& data, const char* key, double most_jitter, const std::string& at) {
    DabDynamic out;
    const Json* value = get(data, key);
    if (value == nullptr) return out;
    const std::string here = at + key + ".";
    known_keys(*value, {"jitter", "control", "fade_steps", "minimum"}, at + key);
    out.jitter = number(*value, "jitter", out.jitter, 0.0, most_jitter, here);
    out.control = static_cast<DabControl>(choice(*value, "control", 0, kControls, here));
    out.fade_steps = whole(*value, "fade_steps", out.fade_steps, 1, 9999, here);
    out.minimum = number(*value, "minimum", out.minimum, 0.0, 1.0, here);
    return out;
}

DabMaskMode mask_mode(const Json& data, const std::string& at) {
    return static_cast<DabMaskMode>(choice(data, "mode", 0, kMaskModes, at));
}

DabPattern pattern_of(const Json& data, const std::string& at) {
    DabPattern out;
    const Json* value = get(data, "pattern");
    if (value == nullptr) return out;
    if (value->is_object() && value->contains("asset")) {
        known_keys(*value, {"asset"}, at + "pattern");
        const Json& asset = (*value)["asset"];
        if (!asset.is_string() || !is_ref(asset.get<std::string>())) bad(at + "pattern.asset must be an asset ref (sha256:<64 hex>)");
        out.asset = asset.get<std::string>();
        return out;
    }
    known_keys(*value, {"procedural", "size", "seed"}, at + "pattern");
    const std::string here = at + "pattern.";
    out.procedural = kStyles[choice(*value, "procedural", 2, kStyles, here)];
    out.size = whole(*value, "size", out.size, 1, kMaxPatternEdge, here);
    out.seed = whole(*value, "seed", out.seed, 0, kMaxDabSeed, here);
    return out;
}

// --- writing (only what differs) ---------------------------------------------------------------------------------------

template <typename T>
void put(bool every, Json& out, const char* key, const T& mine, const T& plain) {
    if (every || !(mine == plain)) out[key] = mine;
}

Json dynamic_json(bool every, const DabDynamic& d) {
    const DabDynamic plain;
    Json out = Json::object();
    put(every, out, "jitter", d.jitter, plain.jitter);
    if (every || d.control != plain.control) out["control"] = kControls[static_cast<std::size_t>(d.control)];
    put(every, out, "fade_steps", d.fade_steps, plain.fade_steps);
    put(every, out, "minimum", d.minimum, plain.minimum);
    return out;
}

void put_dynamic(bool every, Json& out, const char* key, const DabDynamic& mine) {
    if (every || !(mine == DabDynamic{})) out[key] = dynamic_json(every, mine);
}

void put_mode(bool every, Json& out, DabMaskMode mode) {
    if (every || mode != DabMaskMode::Multiply) out["mode"] = kMaskModes[static_cast<std::size_t>(mode)];
}

void put_tip(bool every, Json& out, const char* key, const std::string& tip) {
    if (!tip.empty()) {
        out[key] = Json{{"asset", tip}};
    } else if (every) {
        out[key] = "round";
    }
}

Json pattern_json(bool every, const DabPattern& p) {
    if (!p.asset.empty()) return Json{{"asset", p.asset}};
    const DabPattern plain;
    Json out = Json::object();
    put(every, out, "procedural", p.procedural, plain.procedural);
    put(every, out, "size", p.size, plain.size);
    put(every, out, "seed", p.seed, plain.seed);
    return out;
}

const Json* custom_brushes(const Document& doc) { return doc.brush_custom.is_object() ? &doc.brush_custom : nullptr; }

}  // namespace

Json dabs_to_json(const DabSettings& s, bool every) {
    const DabSettings plain;
    Json out = Json::object();
    put(every, out, "size_mm", s.size_mm, plain.size_mm);
    put(every, out, "hardness", s.hardness, plain.hardness);
    put(every, out, "spacing", s.spacing, plain.spacing);
    put(every, out, "angle", s.angle, plain.angle);
    put(every, out, "roundness", s.roundness, plain.roundness);
    put(every, out, "flip_x", s.flip_x, plain.flip_x);
    put(every, out, "flip_y", s.flip_y, plain.flip_y);
    put(every, out, "aliased", s.aliased, plain.aliased);
    put(every, out, "opacity", s.opacity, plain.opacity);
    put(every, out, "flow", s.flow, plain.flow);
    put(every, out, "pressure_size", s.pressure_size, plain.pressure_size);
    put(every, out, "pressure_opacity", s.pressure_opacity, plain.pressure_opacity);
    put(every, out, "erase", s.erase, plain.erase);
    put_tip(every, out, "tip", s.tip);
    if (every || !(s.shape == plain.shape)) {
        Json shape = Json::object();
        put(every, shape, "enabled", s.shape.enabled, plain.shape.enabled);
        put_dynamic(every, shape, "size", s.shape.size);
        put_dynamic(every, shape, "angle", s.shape.angle);
        put_dynamic(every, shape, "roundness", s.shape.roundness);
        put(every, shape, "flip_x_jitter", s.shape.flip_x_jitter, plain.shape.flip_x_jitter);
        put(every, shape, "flip_y_jitter", s.shape.flip_y_jitter, plain.shape.flip_y_jitter);
        out["shape"] = std::move(shape);
    }
    if (every || !(s.scatter == plain.scatter)) {
        Json scatter = Json::object();
        put(every, scatter, "enabled", s.scatter.enabled, plain.scatter.enabled);
        put_dynamic(every, scatter, "scatter", s.scatter.scatter);
        put(every, scatter, "both_axes", s.scatter.both_axes, plain.scatter.both_axes);
        put(every, scatter, "count", s.scatter.count, plain.scatter.count);
        put_dynamic(every, scatter, "count_jitter", s.scatter.count_jitter);
        out["scatter"] = std::move(scatter);
    }
    if (every || !(s.texture == plain.texture)) {
        const DabTexture& t = s.texture;
        Json texture = Json::object();
        put(every, texture, "enabled", t.enabled, plain.texture.enabled);
        if (every || !(t.pattern == plain.texture.pattern)) texture["pattern"] = pattern_json(every, t.pattern);
        put(every, texture, "invert", t.invert, plain.texture.invert);
        put(every, texture, "scale", t.scale, plain.texture.scale);
        put(every, texture, "brightness", t.brightness, plain.texture.brightness);
        put(every, texture, "contrast", t.contrast, plain.texture.contrast);
        put(every, texture, "each_tip", t.each_tip, plain.texture.each_tip);
        put_mode(every, texture, t.mode);
        put(every, texture, "depth", t.depth, plain.texture.depth);
        put_dynamic(every, texture, "depth_jitter", t.depth_jitter);
        out["texture"] = std::move(texture);
    }
    if (every || !(s.dual == plain.dual)) {
        const DabDual& d = s.dual;
        Json dual = Json::object();
        put(every, dual, "enabled", d.enabled, plain.dual.enabled);
        put_mode(every, dual, d.mode);
        put_tip(every, dual, "tip", d.tip);
        put(every, dual, "size_mm", d.size_mm, plain.dual.size_mm);
        put(every, dual, "hardness", d.hardness, plain.dual.hardness);
        put(every, dual, "roundness", d.roundness, plain.dual.roundness);
        put(every, dual, "angle", d.angle, plain.dual.angle);
        put(every, dual, "spacing", d.spacing, plain.dual.spacing);
        put(every, dual, "scatter", d.scatter, plain.dual.scatter);
        put(every, dual, "both_axes", d.both_axes, plain.dual.both_axes);
        put(every, dual, "count", d.count, plain.dual.count);
        put(every, dual, "flip", d.flip, plain.dual.flip);
        out["dual"] = std::move(dual);
    }
    if (every || !(s.color == plain.color)) {
        const DabColor& c = s.color;
        Json color = Json::object();
        put(every, color, "enabled", c.enabled, plain.color.enabled);
        put(every, color, "per_tip", c.per_tip, plain.color.per_tip);
        put_dynamic(every, color, "fg_bg", c.fg_bg);
        put(every, color, "hue_jitter", c.hue_jitter, plain.color.hue_jitter);
        put(every, color, "saturation_jitter", c.saturation_jitter, plain.color.saturation_jitter);
        put(every, color, "brightness_jitter", c.brightness_jitter, plain.color.brightness_jitter);
        put(every, color, "purity", c.purity, plain.color.purity);
        out["color"] = std::move(color);
    }
    if (every || !(s.transfer == plain.transfer)) {
        Json transfer = Json::object();
        put(every, transfer, "enabled", s.transfer.enabled, plain.transfer.enabled);
        put_dynamic(every, transfer, "opacity", s.transfer.opacity);
        put_dynamic(every, transfer, "flow", s.transfer.flow);
        out["transfer"] = std::move(transfer);
    }
    if (every || !(s.pose == plain.pose)) {
        const DabPose& p = s.pose;
        Json pose = Json::object();
        put(every, pose, "enabled", p.enabled, plain.pose.enabled);
        put(every, pose, "tilt_x", p.tilt_x, plain.pose.tilt_x);
        put(every, pose, "tilt_y", p.tilt_y, plain.pose.tilt_y);
        put(every, pose, "rotation", p.rotation, plain.pose.rotation);
        put(every, pose, "pressure", p.pressure, plain.pose.pressure);
        put(every, pose, "override_tilt", p.override_tilt, plain.pose.override_tilt);
        put(every, pose, "override_rotation", p.override_rotation, plain.pose.override_rotation);
        put(every, pose, "override_pressure", p.override_pressure, plain.pose.override_pressure);
        out["pose"] = std::move(pose);
    }
    put(every, out, "noise", s.noise, plain.noise);
    put(every, out, "wet_edges", s.wet_edges, plain.wet_edges);
    put(every, out, "build_up", s.build_up, plain.build_up);
    put(every, out, "build_up_rate", s.build_up_rate, plain.build_up_rate);
    put(every, out, "protect_texture", s.protect_texture, plain.protect_texture);
    put(every, out, "seed", s.seed, plain.seed);
    return out;
}

DabSettings dabs_from_json(const Json& data) {
    known_keys(data, {"size_mm", "hardness", "spacing", "angle", "roundness", "flip_x", "flip_y", "aliased", "opacity", "flow",
                      "pressure_size", "pressure_opacity", "erase", "tip", "shape", "scatter", "texture", "dual", "color", "transfer",
                      "pose", "noise", "wet_edges", "build_up", "build_up_rate", "protect_texture", "seed"},
               "settings");
    DabSettings s;
    s.size_mm = number(data, "size_mm", s.size_mm, 0.01, 500.0, "");
    s.hardness = number(data, "hardness", s.hardness, 0.0, 1.0, "");
    s.spacing = number(data, "spacing", s.spacing, 0.01, 10.0, "");
    s.angle = number(data, "angle", s.angle, -360.0, 360.0, "");
    s.roundness = number(data, "roundness", s.roundness, 0.0, 1.0, "");
    s.flip_x = flag(data, "flip_x", s.flip_x, "");
    s.flip_y = flag(data, "flip_y", s.flip_y, "");
    s.aliased = flag(data, "aliased", s.aliased, "");
    s.opacity = number(data, "opacity", s.opacity, 0.0, 1.0, "");
    s.flow = number(data, "flow", s.flow, 0.0, 1.0, "");
    s.pressure_size = flag(data, "pressure_size", s.pressure_size, "");
    s.pressure_opacity = flag(data, "pressure_opacity", s.pressure_opacity, "");
    s.erase = flag(data, "erase", s.erase, "");
    s.tip = tip_of(data, "tip", "");
    if (const Json* shape = get(data, "shape")) {
        known_keys(*shape, {"enabled", "size", "angle", "roundness", "flip_x_jitter", "flip_y_jitter"}, "shape");
        s.shape.enabled = flag(*shape, "enabled", false, "shape.");
        s.shape.size = dynamic(*shape, "size", 1.0, "shape.");
        s.shape.angle = dynamic(*shape, "angle", 1.0, "shape.");
        s.shape.roundness = dynamic(*shape, "roundness", 1.0, "shape.");
        s.shape.flip_x_jitter = flag(*shape, "flip_x_jitter", false, "shape.");
        s.shape.flip_y_jitter = flag(*shape, "flip_y_jitter", false, "shape.");
    }
    if (const Json* scatter = get(data, "scatter")) {
        known_keys(*scatter, {"enabled", "scatter", "both_axes", "count", "count_jitter"}, "scatter");
        s.scatter.enabled = flag(*scatter, "enabled", false, "scatter.");
        s.scatter.scatter = dynamic(*scatter, "scatter", 10.0, "scatter.");
        s.scatter.both_axes = flag(*scatter, "both_axes", false, "scatter.");
        s.scatter.count = whole(*scatter, "count", 1, 1, 16, "scatter.");
        s.scatter.count_jitter = dynamic(*scatter, "count_jitter", 1.0, "scatter.");
    }
    if (const Json* texture = get(data, "texture")) {
        known_keys(*texture, {"enabled", "pattern", "invert", "scale", "brightness", "contrast", "each_tip", "mode", "depth", "depth_jitter"}, "texture");
        DabTexture& t = s.texture;
        t.enabled = flag(*texture, "enabled", t.enabled, "texture.");
        t.pattern = pattern_of(*texture, "texture.");
        t.invert = flag(*texture, "invert", t.invert, "texture.");
        t.scale = number(*texture, "scale", t.scale, 0.01, 10.0, "texture.");
        t.brightness = number(*texture, "brightness", t.brightness, -1.0, 1.0, "texture.");
        t.contrast = number(*texture, "contrast", t.contrast, -1.0, 1.0, "texture.");
        t.each_tip = flag(*texture, "each_tip", t.each_tip, "texture.");
        t.mode = mask_mode(*texture, "texture.");
        t.depth = number(*texture, "depth", t.depth, 0.0, 1.0, "texture.");
        t.depth_jitter = dynamic(*texture, "depth_jitter", 1.0, "texture.");
    }
    if (const Json* dual = get(data, "dual")) {
        known_keys(*dual, {"enabled", "mode", "tip", "size_mm", "hardness", "roundness", "angle", "spacing", "scatter", "both_axes", "count", "flip"}, "dual");
        DabDual& d = s.dual;
        d.enabled = flag(*dual, "enabled", d.enabled, "dual.");
        d.mode = mask_mode(*dual, "dual.");
        d.tip = tip_of(*dual, "tip", "dual.");
        d.size_mm = number(*dual, "size_mm", d.size_mm, 0.01, 500.0, "dual.");
        d.hardness = number(*dual, "hardness", d.hardness, 0.0, 1.0, "dual.");
        d.roundness = number(*dual, "roundness", d.roundness, 0.0, 1.0, "dual.");
        d.angle = number(*dual, "angle", d.angle, -360.0, 360.0, "dual.");
        d.spacing = number(*dual, "spacing", d.spacing, 0.01, 10.0, "dual.");
        d.scatter = number(*dual, "scatter", d.scatter, 0.0, 10.0, "dual.");
        d.both_axes = flag(*dual, "both_axes", d.both_axes, "dual.");
        d.count = whole(*dual, "count", d.count, 1, 16, "dual.");
        d.flip = flag(*dual, "flip", d.flip, "dual.");
    }
    if (const Json* color = get(data, "color")) {
        known_keys(*color, {"enabled", "per_tip", "fg_bg", "hue_jitter", "saturation_jitter", "brightness_jitter", "purity"}, "color");
        DabColor& c = s.color;
        c.enabled = flag(*color, "enabled", c.enabled, "color.");
        c.per_tip = flag(*color, "per_tip", c.per_tip, "color.");
        c.fg_bg = dynamic(*color, "fg_bg", 1.0, "color.");
        c.hue_jitter = number(*color, "hue_jitter", c.hue_jitter, 0.0, 1.0, "color.");
        c.saturation_jitter = number(*color, "saturation_jitter", c.saturation_jitter, 0.0, 1.0, "color.");
        c.brightness_jitter = number(*color, "brightness_jitter", c.brightness_jitter, 0.0, 1.0, "color.");
        c.purity = number(*color, "purity", c.purity, -1.0, 1.0, "color.");
    }
    if (const Json* transfer = get(data, "transfer")) {
        known_keys(*transfer, {"enabled", "opacity", "flow"}, "transfer");
        s.transfer.enabled = flag(*transfer, "enabled", false, "transfer.");
        s.transfer.opacity = dynamic(*transfer, "opacity", 1.0, "transfer.");
        s.transfer.flow = dynamic(*transfer, "flow", 1.0, "transfer.");
    }
    if (const Json* pose = get(data, "pose")) {
        known_keys(*pose, {"enabled", "tilt_x", "tilt_y", "rotation", "pressure", "override_tilt", "override_rotation", "override_pressure"}, "pose");
        DabPose& p = s.pose;
        p.enabled = flag(*pose, "enabled", p.enabled, "pose.");
        p.tilt_x = number(*pose, "tilt_x", p.tilt_x, -90.0, 90.0, "pose.");
        p.tilt_y = number(*pose, "tilt_y", p.tilt_y, -90.0, 90.0, "pose.");
        p.rotation = number(*pose, "rotation", p.rotation, 0.0, 360.0, "pose.");
        p.pressure = number(*pose, "pressure", p.pressure, 0.0, 1.0, "pose.");
        p.override_tilt = flag(*pose, "override_tilt", p.override_tilt, "pose.");
        p.override_rotation = flag(*pose, "override_rotation", p.override_rotation, "pose.");
        p.override_pressure = flag(*pose, "override_pressure", p.override_pressure, "pose.");
    }
    s.noise = flag(data, "noise", s.noise, "");
    s.wet_edges = flag(data, "wet_edges", s.wet_edges, "");
    s.build_up = flag(data, "build_up", s.build_up, "");
    s.build_up_rate = number(data, "build_up_rate", s.build_up_rate, 1.0, 1000.0, "");
    s.protect_texture = flag(data, "protect_texture", s.protect_texture, "");
    s.seed = whole(data, "seed", s.seed, 0, kMaxDabSeed, "");
    return s;
}

std::vector<std::string> dab_assets(const DabSettings& s) {
    std::vector<std::string> out;
    for (const std::string* ref : {&s.tip, &s.dual.tip, &s.texture.pattern.asset}) {
        if (!ref->empty() && std::find(out.begin(), out.end(), *ref) == out.end()) out.push_back(*ref);
    }
    return out;
}

bool is_dab_brush(const Json& brush) {
    const Json* engine = get(brush, "engine");
    return engine != nullptr && engine->is_string() && engine->get_ref<const std::string&>() == kDabEngine;
}

std::vector<std::string> dab_asset_refs(const Json& brush_custom) {
    std::vector<std::string> out;
    if (!brush_custom.is_object()) return out;
    for (const auto& [key, brush] : brush_custom.items()) {
        if (!is_dab_brush(brush) || !brush.contains("dabs")) continue;
        try {
            for (const std::string& ref : dab_assets(dabs_from_json(brush["dabs"]))) {
                if (std::find(out.begin(), out.end(), ref) == out.end()) out.push_back(ref);
            }
        } catch (const Error&) {
            // (the reader reports it)
        }
    }
    return out;
}

void validate_stroke_inputs(const Stroke& stroke) {
    const std::size_t n = stroke.points.size();
    const auto each = [](const std::vector<double>& values, double lo, double hi, const char* what) {
        for (const double v : values) {
            if (!(std::isfinite(v) && lo <= v && v <= hi)) throw Error("format", std::string("a line's ") + what);
        }
    };
    if (!stroke.tilt.empty()) {
        if (stroke.tilt.size() != 2 * n) throw Error("format", "a line's tilt is an x and a y for each of its points");
        each(stroke.tilt, -90.0, 90.0, "tilt is between -90 and 90 degrees");
    }
    if (!stroke.time_ms.empty()) {
        if (stroke.time_ms.size() != n) throw Error("format", "a line's time_ms is one time for each of its points");
        each(stroke.time_ms, 0.0, 1e12, "time_ms is a time of 0 or more (ms)");
    }
    if (!stroke.wheel.empty()) {
        if (stroke.wheel.size() != n) throw Error("format", "a line's wheel is one value for each of its points");
        each(stroke.wheel, 0.0, 1.0, "wheel is between 0 and 1");
    }
    if (stroke.flow && !(std::isfinite(*stroke.flow) && *stroke.flow >= 0.0 && *stroke.flow <= 1.0)) {
        throw Error("format", "a line's flow is between 0 and 1");
    }
    if (stroke.background_rgb && (stroke.background_rgb->size() != 3 ||
                                  std::any_of(stroke.background_rgb->begin(), stroke.background_rgb->end(), [](std::int64_t v) { return v < 0 || v > 255; }))) {
        throw Error("format", "a line's background_rgb is three whole numbers 0..255");
    }
}

bool inputs_fit(const std::vector<double>& values, std::size_t points, std::size_t per_point) {
    return !values.empty() && values.size() == points * per_point;
}

std::vector<double> resample_inputs(const std::vector<double>& values, std::size_t per, std::size_t count, int digits) {
    if (values.empty() || count == 0 || per == 0) return {};
    const std::size_t given = values.size() / per;
    if (given == 0) return {};
    std::vector<double> out;
    out.reserve(count * per);
    for (std::size_t i = 0; i < count; ++i) {
        const double t = count == 1 || given == 1 ? 0.0 : static_cast<double>(i) * static_cast<double>(given - 1) / static_cast<double>(count - 1);
        const auto j = std::min(static_cast<std::size_t>(std::floor(t)), given - 1);
        const std::size_t k = std::min(j + 1, given - 1);
        const double f = t - static_cast<double>(j);
        for (std::size_t c = 0; c < per; ++c) {
            const double a = values[j * per + c], b = values[k * per + c];
            out.push_back(py_round(a + (b - a) * f, digits));
        }
    }
    return out;
}

std::vector<double> lengths_along(const std::vector<PointF>& points) {
    std::vector<double> out;
    out.reserve(points.size());
    double total = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (i > 0) total += std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
        out.push_back(total);
    }
    return out;
}

std::vector<double> lengths_along(const PenPoints& points) {
    std::vector<PointF> xy;
    xy.reserve(points.size());
    for (const PenPoint& p : points) xy.push_back(PointF{p.x, p.y});
    return lengths_along(xy);
}

void carry_inputs(const Stroke& from, Stroke& to, const std::vector<double>* positions) {
    to.flow = from.flow;
    to.background_rgb = from.background_rgb;
    to.tilt.clear();
    to.time_ms.clear();
    to.wheel.clear();
    const std::size_t n = from.points.size(), m = to.points.size();
    if (n == 0 || m == 0) return;
    struct Input {
        const std::vector<double>& values;
        std::vector<double>& into;
        std::size_t per;
        int digits;
    };
    const Input inputs[] = {{from.tilt, to.tilt, 2, 1}, {from.time_ms, to.time_ms, 1, 1}, {from.wheel, to.wheel, 1, 3}};
    if (positions == nullptr) {  // (the same points, from the first)
        if (m > n) return;
        for (const Input& in : inputs) {
            if (inputs_fit(in.values, n, in.per)) in.into.assign(in.values.begin(), in.values.begin() + static_cast<std::ptrdiff_t>(m * in.per));
        }
        return;
    }
    if (positions->size() != m) return;
    const std::vector<double> along = lengths_along(from.points);
    for (const Input& in : inputs) {
        if (!inputs_fit(in.values, n, in.per)) continue;
        in.into.reserve(m * in.per);
        std::size_t k = 0;  // (the segment the last place was on: the places mostly go on along the line)
        for (const double s : *positions) {
            if (k > 0 && s < along[k]) k = 0;
            while (k + 2 < n && along[k + 1] < s) ++k;
            const std::size_t j = n == 1 ? 0 : k + 1;
            const double span = along[j] - along[k];
            const double t = span > 0 ? std::clamp((s - along[k]) / span, 0.0, 1.0) : 0.0;
            for (std::size_t c = 0; c < in.per; ++c) {
                const double a = in.values[k * in.per + c], b = in.values[j * in.per + c];
                in.into.push_back(py_round(a + (b - a) * t, in.digits));
            }
        }
    }
}

bool has_stroke_inputs(const Stroke& stroke) {
    const std::size_t n = stroke.points.size();
    return inputs_fit(stroke.tilt, n, 2) || inputs_fit(stroke.time_ms, n, 1) || inputs_fit(stroke.wheel, n, 1) || stroke.flow.has_value() ||
           stroke.background_rgb.has_value();
}

bool uses_brushes(const Document& doc) {
    if (doc.brush_kind || doc.brush_background_rgb) return true;
    if (const Json* custom = custom_brushes(doc)) {
        for (const auto& [key, brush] : custom->items()) {
            if (brush.is_object() && (brush.contains("engine") || brush.contains("dabs"))) return true;
        }
    }
    const auto page_uses = [](const Page& page) {
        for (const Layer& layer : page.layers) {
            if (!layer.strokes) continue;
            for (const StrokePtr& s : layer.strokes->items) {
                if (s && has_stroke_inputs(*s)) return true;
            }
        }
        return false;
    };
    for (const PagePtr& page : doc.pages) {
        if (page && page_uses(*page)) return true;
    }
    for (const auto& [id, contents] : doc.smart_contents) {
        if (contents && contents->page && page_uses(*contents->page)) return true;
    }
    return false;
}

std::optional<std::string> check_tip_png(std::string_view png, std::uint32_t edge) {
    // (the signature, then IHDR: width, height, bit depth, colour type)
    static constexpr unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (png.size() < 33 || !std::equal(std::begin(kSignature), std::end(kSignature), reinterpret_cast<const unsigned char*>(png.data())) ||
        png.substr(12, 4) != "IHDR") {
        return std::string("it is not a PNG picture");
    }
    const auto be32 = [&](std::size_t at) {
        const auto* b = reinterpret_cast<const unsigned char*>(png.data() + at);
        return (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) | std::uint32_t(b[3]);
    };
    const std::uint32_t width = be32(16), height = be32(20);
    const unsigned depth = static_cast<unsigned char>(png[24]), type = static_cast<unsigned char>(png[25]);
    if (type != 0 || (depth != 8 && depth != 16)) return std::string("it is not a grey picture of 8 or 16 bits");
    if (width == 0 || height == 0 || width > edge || height > edge) {
        return "it is not 1 to " + std::to_string(edge) + " pixels a side";
    }
    return std::nullopt;
}

void validate_brushes(const Document& doc) {
    if (doc.brush_kind) {
        const auto chars = ruby::code_points(*doc.brush_kind).size();
        if (chars < 1 || chars > 40) throw Error("value", "the brush state's kind is 1 to 40 characters");
    }
    if (doc.brush_background_rgb) {
        const auto& rgb = *doc.brush_background_rgb;
        if (rgb.size() != 3 || std::any_of(rgb.begin(), rgb.end(), [](std::int64_t v) { return v < 0 || v > 255; })) {
            throw Error("value", "the brush state's background_rgb is three whole numbers 0..255");
        }
    }
    const Json* custom = custom_brushes(doc);
    if (custom == nullptr) return;
    for (const auto& [key, brush] : custom->items()) {
        if (!brush.is_object() || (!brush.contains("engine") && !brush.contains("dabs"))) continue;
        if (!is_dab_brush(brush)) throw Error("value", "the brush " + key + "'s engine is not one this build draws (\"dabs\")");
        if (!brush.contains("dabs")) throw Error("value", "the dab brush " + key + " has no dabs");
        DabSettings settings;
        try {
            settings = dabs_from_json(brush["dabs"]);
        } catch (const Error& error) {
            throw Error("value", "the brush " + key + "'s " + error.what());
        }
        for (const std::string& ref : dab_assets(settings)) {
            const auto held = doc.brush_tips.find(ref);
            if (held == doc.brush_tips.end() || !held->second) throw Error("value", "the picture " + ref + " of the brush " + key + " is not held");
            // (a picture a texture uses: the texture's bound, the smaller, though a tip uses it too)
            const std::uint32_t edge = ref == settings.texture.pattern.asset ? kMaxPatternEdge : kMaxTipEdge;
            if (const auto why = check_tip_png(*held->second, edge)) throw Error("value", "the picture " + ref + " of the brush " + key + ": " + *why);
        }
    }
}

}  // namespace genko::core
