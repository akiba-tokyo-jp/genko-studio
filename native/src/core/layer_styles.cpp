// layer-styles@1 (PC4 E6, core/layer_styles.hpp). The effects' settings and defaults after PhotoCraft v0.2.0's
// crates/doc/src/effects.rs (the model), blend_if.rs (Blend If's ranges) and engine/src/layer_style.rs (effect_from_params:
// what a command leaves unsaid), MIT: native/third_party/photocraft/NOTICE.md — their pixels in mm here.

#include "core/layer_styles.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <vector>

#include "core/error.hpp"
#include "core/patterns.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace {

[[noreturn]] void bad(const std::string& message) { throw Error("value", "layer style: " + message); }

// A number as the normal form keeps it: 4 decimals, a whole one as an integer.
Json num(double v) {
    v = py_round(v, 4);
    if (v == 0) v = 0;  // (no −0)
    if (v == std::floor(v) && std::fabs(v) < 1e15) return Json(static_cast<std::int64_t>(v));
    return Json(v);
}

double number_of(const Json& v, const std::string& what, double lo, double hi) {
    if (!v.is_number() || v.is_boolean()) bad(what + " must be a number");
    const double d = v.get<double>();
    if (!std::isfinite(d) || d < lo || d > hi) bad(what + " must be between " + py_float_repr(lo) + " and " + py_float_repr(hi));
    return d;
}

bool bool_of(const Json& v, const std::string& what) {
    if (!v.is_boolean()) bad(what + " must be true or false");
    return v.get<bool>();
}

std::string word_of(const Json& v, const std::string& what, std::initializer_list<std::string_view> words) {
    std::string list;
    for (std::string_view w : words) list += (list.empty() ? "" : ", ") + std::string(w);
    if (!v.is_string()) bad(what + " must be one of " + list);
    const std::string& s = v.get_ref<const std::string&>();
    if (std::find(words.begin(), words.end(), s) == words.end()) bad(what + " must be one of " + list);
    return s;
}

// The blend modes a layer takes (core::blend_mode's).
constexpr std::string_view kModes[] = {"normal", "multiply", "screen",     "add",       "overlay",  "darken",     "lighten",
                                       "color_burn", "color_dodge", "linear_burn", "soft_light", "hard_light", "difference", "exclusion",
                                       "subtract", "divide", "hue", "saturation", "color", "luminosity"};

std::string blend_of(const Json& v, const std::string& what) {
    if (!v.is_string() || std::find(std::begin(kModes), std::end(kModes), v.get<std::string>()) == std::end(kModes)) {
        bad(what + " must be a blend mode (normal, multiply, screen, …)");
    }
    return v.get<std::string>();
}

Json rgb_of(const Json& v, const std::string& what) {
    if (v.is_string()) {
        std::string text = v.get<std::string>();
        if (!text.empty() && text.front() == '#') text.erase(0, 1);
        if (text.size() != 6 || !std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; })) {
            bad(what + " must be [r, g, b] or \"#rrggbb\"");
        }
        Json out = Json::array();
        for (std::size_t k = 0; k < 3; ++k) out.push_back(std::stoi(text.substr(k * 2, 2), nullptr, 16));
        return out;
    }
    if (!v.is_array() || v.size() != 3) bad(what + " must be [r, g, b] (0..255)");
    Json out = Json::array();
    for (const Json& c : v) {
        if (!c.is_number_integer() || c.get<std::int64_t>() < 0 || c.get<std::int64_t>() > 255) bad(what + " must be [r, g, b] (whole numbers 0..255)");
        out.push_back(c.get<std::int64_t>());
    }
    return out;
}

// A contour: {"name", "points"} (2..32 points 0..1, sorted, one of each input); null for the linear one.
Json contour_of(const Json& v, const std::string& what) {
    if (v.is_null()) return nullptr;
    if (!v.is_object()) bad(what + " must be {\"name\", \"points\": [[in, out], …]}");
    for (const auto& [key, value] : v.items()) {
        if (key != "name" && key != "points") bad(what + " has an unknown key: " + key);
    }
    std::string name;
    if (const Json* n = get(v, "name"); n != nullptr && !n->is_null()) {
        if (!n->is_string() || n->get<std::string>().size() > 255) bad(what + "'s name must be text (at most 255 bytes)");
        name = n->get<std::string>();
    }
    const Json* points = get(v, "points");
    if (points == nullptr || !points->is_array() || points->size() < 2 || points->size() > 32) bad(what + "'s points must be 2 to 32 [in, out] (0..1)");
    std::vector<std::pair<double, double>> pts;
    for (const Json& p : *points) {
        if (!p.is_array() || p.size() != 2) bad(what + "'s points must be 2 to 32 [in, out] (0..1)");
        pts.emplace_back(py_round(number_of(p[0], what + "'s point", 0, 1), 4), py_round(number_of(p[1], what + "'s point", 0, 1), 4));
    }
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<double, double>> kept;
    for (const auto& p : pts) {
        if (kept.empty() || std::fabs(p.first - kept.back().first) > 1e-6) kept.push_back(p);
    }
    if (kept.size() < 2) bad(what + " needs two points of different inputs");
    const bool linear = kept.size() == 2 && kept[0] == std::pair<double, double>{0, 0} && kept[1] == std::pair<double, double>{1, 1};
    if (linear && (name.empty() || name == "Linear" || name == "線形")) return nullptr;
    Json list = Json::array();
    for (const auto& [x, y] : kept) list.push_back(Json::array({num(x), num(y)}));
    return Json{{"name", name}, {"points", std::move(list)}};
}

// The stops of a gradient: [[place 0..1, colour], …] (2..16) sorted by place.
Json stops_of(const Json& v, const std::string& what) {
    if (!v.is_array() || v.size() < 2 || v.size() > 16) bad(what + " must be 2 to 16 [place 0..1, [r, g, b]]");
    std::vector<std::pair<double, Json>> stops;
    for (const Json& s : v) {
        if (!s.is_array() || s.size() != 2) bad(what + " must be 2 to 16 [place 0..1, [r, g, b]]");
        stops.emplace_back(py_round(number_of(s[0], what + "'s place", 0, 1), 4), rgb_of(s[1], what + "'s colour"));
    }
    std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    Json out = Json::array();
    for (auto& [t, c] : stops) out.push_back(Json::array({num(t), std::move(c)}));
    return out;
}

Json transparency_of(const Json& v, const std::string& what) {
    if (!v.is_array() || v.size() < 2 || v.size() > 16) bad(what + " must be 2 to 16 [place 0..1, opacity 0..1]");
    std::vector<std::pair<double, double>> stops;
    for (const Json& s : v) {
        if (!s.is_array() || s.size() != 2) bad(what + " must be 2 to 16 [place 0..1, opacity 0..1]");
        stops.emplace_back(py_round(number_of(s[0], what + "'s place", 0, 1), 4), py_round(number_of(s[1], what + "'s opacity", 0, 1), 4));
    }
    std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    Json out = Json::array();
    for (const auto& [t, a] : stops) out.push_back(Json::array({num(t), num(a)}));
    return out;
}

const Json& default_gradient() {
    static const Json g{{"stops", Json::array({Json::array({0, Json::array({0, 0, 0})}), Json::array({1, Json::array({255, 255, 255})})})},
                        {"style", "linear"},
                        {"angle", 90},
                        {"scale", 1},
                        {"reverse", false},
                        {"align", true},
                        {"offset", Json::array({0, 0})}};
    return g;
}

// A gradient: `raw`'s keys over `base`'s (a gradient's normal form).
Json gradient_of(const Json& raw, const Json& base, const std::string& what) {
    if (!raw.is_object()) bad(what + " must be {\"stops\", \"style\", \"angle\", …}");
    static const char* const kKeys[] = {"stops", "transparency", "style", "angle", "scale", "reverse", "align", "offset"};
    for (const auto& [key, value] : raw.items()) {
        if (std::none_of(std::begin(kKeys), std::end(kKeys), [&](const char* k) { return key == k; })) bad(what + " has an unknown key: " + key);
    }
    const auto at = [&](const char* key) -> const Json* {
        if (const Json* v = get(raw, key)) return v;
        return get(base, key);
    };
    Json out = Json::object();
    out["stops"] = stops_of(*at("stops"), what + "'s stops");
    if (const Json* t = at("transparency"); t != nullptr && !t->is_null()) out["transparency"] = transparency_of(*t, what + "'s transparency");
    out["style"] = word_of(*at("style"), what + "'s style", {"linear", "radial", "angle", "reflected", "diamond"});
    out["angle"] = num(number_of(*at("angle"), what + "'s angle", -360, 360));
    out["scale"] = num(number_of(*at("scale"), what + "'s scale", 0.1, 1.5));
    out["reverse"] = bool_of(*at("reverse"), what + "'s reverse");
    out["align"] = bool_of(*at("align"), what + "'s align");
    const Json& offset = *at("offset");
    if (!offset.is_array() || offset.size() != 2) bad(what + "'s offset must be [x, y] (−1..1 of the box)");
    out["offset"] = Json::array({num(number_of(offset[0], what + "'s offset", -1, 1)), num(number_of(offset[1], what + "'s offset", -1, 1))});
    return out;
}

Json pattern_of(const Json& v, const std::string& what) {
    try {
        return pattern_fill_spec(v);
    } catch (const Error& error) {
        bad(what + ": " + error.what());
    }
}

// One kind's settings: its keys in the order written, with their defaults.
struct KindDef {
    std::string_view kind;
    Json defaults;  // (every key but "kind", in order)
};

Json shadow_defaults(bool inner) {
    Json d{{"enabled", true}, {"blend", "multiply"}, {"opacity", 0.75},  {"rgb", Json::array({0, 0, 0})}, {"angle", 120},
           {"global_light", true}, {"distance_mm", 0.5}};
    d[inner ? "choke" : "spread"] = 0;
    d["size_mm"] = 0.5;
    d["noise"] = 0;
    d["anti_alias"] = false;
    if (!inner) d["knocks_out"] = true;
    return d;
}

Json glow_defaults(bool inner) {
    Json d{{"enabled", true}, {"blend", "screen"}, {"opacity", 0.75}, {"rgb", Json::array({255, 255, 190})}, {"technique", "softer"}};
    if (inner) d["source"] = "edge";
    d[inner ? "choke" : "spread"] = 0;
    d["size_mm"] = 0.5;
    d["range"] = 0.5;
    d["jitter"] = 0;
    d["noise"] = 0;
    d["anti_alias"] = false;
    return d;
}

const std::vector<KindDef>& kinds() {
    static const std::vector<KindDef> all = [] {
        std::vector<KindDef> out;
        out.push_back({"bevel",
                       Json{{"enabled", true},
                            {"style", "inner"},
                            {"technique", "smooth"},
                            {"depth", 1},
                            {"direction", "up"},
                            {"size_mm", 0.5},
                            {"soften_mm", 0},
                            {"angle", 120},
                            {"altitude", 30},
                            {"global_light", true},
                            {"highlight", Json{{"blend", "screen"}, {"opacity", 0.75}, {"rgb", Json::array({255, 255, 255})}}},
                            {"shadow", Json{{"blend", "multiply"}, {"opacity", 0.75}, {"rgb", Json::array({0, 0, 0})}}}}});
        out.push_back({"stroke", Json{{"enabled", true},
                                      {"blend", "normal"},
                                      {"opacity", 1},
                                      {"size_mm", 0.3},
                                      {"position", "outside"},
                                      {"rgb", Json::array({255, 0, 0})}}});
        out.push_back({"inner_shadow", shadow_defaults(true)});
        out.push_back({"inner_glow", glow_defaults(true)});
        out.push_back({"satin", Json{{"enabled", true},
                                     {"blend", "multiply"},
                                     {"opacity", 0.5},
                                     {"rgb", Json::array({0, 0, 0})},
                                     {"angle", 19},
                                     {"distance_mm", 1},
                                     {"size_mm", 1.2},
                                     {"invert", true},
                                     {"anti_alias", true}}});
        out.push_back({"color_overlay", Json{{"enabled", true}, {"blend", "normal"}, {"opacity", 1}, {"rgb", Json::array({255, 0, 0})}}});
        out.push_back({"gradient_overlay", Json{{"enabled", true}, {"blend", "normal"}, {"opacity", 1}, {"gradient", default_gradient()}, {"dither", false}}});
        out.push_back({"pattern_overlay", Json{{"enabled", true}, {"blend", "normal"}, {"opacity", 1}, {"pattern", nullptr}}});
        out.push_back({"outer_glow", glow_defaults(false)});
        out.push_back({"drop_shadow", shadow_defaults(false)});
        return out;
    }();
    return all;
}

const KindDef* kind_def(std::string_view kind) {
    for (const KindDef& k : kinds()) {
        if (k.kind == kind) return &k;
    }
    return nullptr;
}

// The keys an effect of the kind may hold beyond its defaults' (each optional: absent when off).
std::vector<std::string_view> optional_keys(std::string_view kind) {
    if (kind == "bevel") return {"gloss_contour", "contour", "texture"};
    if (kind == "drop_shadow" || kind == "inner_shadow" || kind == "outer_glow" || kind == "inner_glow" || kind == "satin") return {"contour"};
    return {};
}

// The paints an effect's colour slot takes ("rgb" its default).
std::vector<std::string_view> paints(std::string_view kind) {
    if (kind == "stroke") return {"rgb", "gradient", "pattern"};
    if (kind == "outer_glow" || kind == "inner_glow") return {"rgb", "gradient"};
    return {};
}

// A bevel's highlight or shadow: {blend, opacity, rgb}.
Json tone_of(const Json& raw, const Json& base, const std::string& what) {
    if (!raw.is_object()) bad(what + " must be {\"blend\", \"opacity\", \"rgb\"}");
    for (const auto& [key, value] : raw.items()) {
        if (key != "blend" && key != "opacity" && key != "rgb") bad(what + " has an unknown key: " + key);
    }
    const auto at = [&](const char* key) -> const Json& {
        if (const Json* v = get(raw, key)) return *v;
        return base.at(key);
    };
    return Json{{"blend", blend_of(at("blend"), what + "'s blend")},
                {"opacity", num(number_of(at("opacity"), what + "'s opacity", 0, 1))},
                {"rgb", rgb_of(at("rgb"), what + "'s rgb")}};
}

// One setting of an effect checked (its default giving its kind of value).
Json setting_of(std::string_view kind, std::string_view key, const Json& value, const Json& fallback, const std::string& what) {
    const std::string here = what + "'s " + std::string(key);
    if (key == "enabled" || key == "global_light" || key == "anti_alias" || key == "knocks_out" || key == "invert" || key == "dither") {
        return bool_of(value, here);
    }
    if (key == "blend") return blend_of(value, here);
    if (key == "opacity" || key == "spread" || key == "choke" || key == "jitter" || key == "noise") return num(number_of(value, here, 0, 1));
    if (key == "range") return num(number_of(value, here, 0.01, 1));
    if (key == "rgb") return rgb_of(value, here);
    if (key == "angle") return num(number_of(value, here, -360, 360));
    if (key == "altitude") return num(number_of(value, here, 0, 90));
    if (key == "size_mm" || key == "distance_mm" || key == "soften_mm") return num(number_of(value, here, 0, kEffectMaxMm));
    if (key == "depth") return num(number_of(value, here, 0.01, 10));
    if (key == "technique") {
        return kind == "bevel" ? word_of(value, here, {"smooth", "chisel_hard", "chisel_soft"}) : word_of(value, here, {"softer", "precise"});
    }
    if (key == "source") return word_of(value, here, {"edge", "center"});
    if (key == "style") return word_of(value, here, {"inner", "outer", "emboss", "pillow", "stroke"});
    if (key == "direction") return word_of(value, here, {"up", "down"});
    if (key == "position") return word_of(value, here, {"outside", "inside", "center"});
    if (key == "highlight" || key == "shadow") return tone_of(value, fallback, here);
    if (key == "gradient") return gradient_of(value, fallback.is_object() ? fallback : default_gradient(), here);
    if (key == "pattern") return pattern_of(value, here);
    bad(here + " is not a setting of " + std::string(kind));
}

// A bevel's contour element: {"curve"?, "range", "anti_alias"}.
Json bevel_contour_of(const Json& raw, const Json* base, const std::string& what) {
    if (!raw.is_object()) bad(what + " must be {\"curve\"?, \"range\", \"anti_alias\"}");
    for (const auto& [key, value] : raw.items()) {
        if (key != "curve" && key != "range" && key != "anti_alias") bad(what + " has an unknown key: " + key);
    }
    const auto at = [&](const char* key, const Json& fallback) -> const Json& {
        if (const Json* v = get(raw, key)) return *v;
        if (base != nullptr && base->is_object()) {
            if (const Json* v = get(*base, key)) return *v;
        }
        return fallback;
    };
    Json out = Json::object();
    if (Json curve = contour_of(at("curve", Json(nullptr)), what + "'s curve"); !curve.is_null()) out["curve"] = std::move(curve);
    out["range"] = num(number_of(at("range", Json(0.5)), what + "'s range", 0.01, 1));
    out["anti_alias"] = bool_of(at("anti_alias", Json(false)), what + "'s anti_alias");
    return out;
}

// A bevel's texture: {"pattern", "depth", "invert"}.
Json texture_of(const Json& raw, const Json* base, const std::string& what) {
    if (!raw.is_object()) bad(what + " must be {\"pattern\", \"depth\", \"invert\"}");
    for (const auto& [key, value] : raw.items()) {
        if (key != "pattern" && key != "depth" && key != "invert") bad(what + " has an unknown key: " + key);
    }
    const auto at = [&](const char* key) -> const Json* {
        if (const Json* v = get(raw, key)) return v;
        if (base != nullptr && base->is_object()) return get(*base, key);
        return nullptr;
    };
    const Json* pattern = at("pattern");
    if (pattern == nullptr || pattern->is_null()) bad(what + " needs its pattern");
    Json out = Json::object();
    out["pattern"] = pattern_of(*pattern, what + "'s pattern");
    const Json* depth = at("depth");
    out["depth"] = num(depth == nullptr ? 1.0 : number_of(*depth, what + "'s depth", -10, 10));
    const Json* invert = at("invert");
    out["invert"] = invert == nullptr ? false : bool_of(*invert, what + "'s invert");
    return out;
}

const std::vector<std::string_view>& blend_if_channels() {
    static const std::vector<std::string_view> all{"gray", "red", "green", "blue", "cyan", "magenta", "yellow", "black", "lightness", "a", "b"};
    return all;
}

Json range_of(const Json& v, const std::string& what) {
    if (!v.is_array() || v.size() != 4) bad(what + " must be [black low, black high, white low, white high] (0..255)");
    std::array<std::int64_t, 4> r{};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!v[i].is_number_integer() || v[i].get<std::int64_t>() < 0 || v[i].get<std::int64_t>() > 255) {
            bad(what + " must be [black low, black high, white low, white high] (whole numbers 0..255)");
        }
        r[i] = v[i].get<std::int64_t>();
    }
    if (r[0] > r[1] || r[2] > r[3]) bad(what + "'s black low is at most its black high, its white low at most its white high");
    return Json::array({r[0], r[1], r[2], r[3]});
}

Json blend_if_of(const Json& v) {
    if (v.is_null()) return Json::array();
    if (!v.is_array()) bad("blend_if must be [{\"channel\", \"this\", \"under\"}, …]");
    const Json full = Json::array({0, 0, 255, 255});
    std::map<std::size_t, Json> by;
    for (const Json& item : v) {
        if (!item.is_object()) bad("blend_if must be [{\"channel\", \"this\", \"under\"}, …]");
        for (const auto& [key, value] : item.items()) {
            if (key != "channel" && key != "this" && key != "under") bad("blend_if has an unknown key: " + key);
        }
        const Json* channel = get(item, "channel");
        const auto& names = blend_if_channels();
        const auto it = channel != nullptr && channel->is_string() ? std::find(names.begin(), names.end(), channel->get<std::string>()) : names.end();
        if (it == names.end()) bad("blend_if's channel must be gray, red, green, blue, cyan, magenta, yellow, black, lightness, a or b");
        const std::size_t index = static_cast<std::size_t>(it - names.begin());
        if (by.contains(index)) bad("blend_if names the channel " + std::string(*it) + " twice");
        const Json* self = get(item, "this");
        const Json* under = get(item, "under");
        Json entry = Json::object();
        entry["channel"] = std::string(*it);
        entry["this"] = self == nullptr || self->is_null() ? full : range_of(*self, "blend_if's this");
        entry["under"] = under == nullptr || under->is_null() ? full : range_of(*under, "blend_if's under");
        if (entry["this"] != full || entry["under"] != full) by[index] = std::move(entry);
    }
    Json out = Json::array();
    for (auto& [index, entry] : by) out.push_back(std::move(entry));
    return out;
}

}  // namespace

Json default_effect(std::string_view kind) {
    const KindDef* def = kind_def(kind);
    if (def == nullptr) bad("an effect's kind must be one of bevel, stroke, inner_shadow, inner_glow, satin, color_overlay, gradient_overlay, pattern_overlay, outer_glow, drop_shadow");
    Json out = Json::object();
    out["kind"] = std::string(kind);
    for (const auto& [key, value] : def->defaults.items()) out[key] = value;
    return out;
}

Json layer_effect_spec(const Json& raw, const Json* base) {
    if (!raw.is_object()) bad("an effect must be {\"kind\", …}");
    const Json* kind_given = get(raw, "kind");
    if (kind_given == nullptr && base != nullptr) kind_given = get(*base, "kind");
    if (kind_given == nullptr || !kind_given->is_string() || kind_def(kind_given->get<std::string>()) == nullptr) {
        bad("an effect's kind must be one of bevel, stroke, inner_shadow, inner_glow, satin, color_overlay, gradient_overlay, pattern_overlay, outer_glow, "
            "drop_shadow");
    }
    const std::string kind = kind_given->get<std::string>();
    const KindDef& def = *kind_def(kind);
    if (base != nullptr && (!base->is_object() || base->value("kind", std::string()) != kind)) base = nullptr;
    const std::string what = kind;
    const std::vector<std::string_view> paint_keys = paints(kind);
    const std::vector<std::string_view> extra = optional_keys(kind);
    for (const auto& [key, value] : raw.items()) {
        const bool known = key == "kind" || def.defaults.contains(key) ||
                           std::find(paint_keys.begin(), paint_keys.end(), key) != paint_keys.end() ||
                           std::find(extra.begin(), extra.end(), key) != extra.end();
        if (!known) bad(what + " has an unknown setting: " + key);
    }
    // (the paint: the one given, else the base's, else the default's colour; two given is one too many)
    std::string paint = paint_keys.empty() ? std::string() : std::string("rgb");
    if (!paint_keys.empty()) {
        int given = 0;
        for (std::string_view p : paint_keys) {
            if (const Json* v = get(raw, p); v != nullptr && !v->is_null()) {
                paint = std::string(p);
                ++given;
            }
        }
        if (given > 1) bad(what + " is painted with one of rgb, gradient or pattern");
        if (given == 0 && base != nullptr) {
            for (std::string_view p : paint_keys) {
                if (base->contains(p)) paint = std::string(p);
            }
        }
    }
    Json out = Json::object();
    out["kind"] = kind;
    for (const auto& [key, fallback] : def.defaults.items()) {
        std::string slot = key;
        if (key == "rgb" && !paint_keys.empty()) slot = paint;
        const Json* given = get(raw, slot);
        const Json* from_base = base != nullptr ? get(*base, slot) : nullptr;
        const Json& value = given != nullptr && !given->is_null() ? *given : from_base != nullptr ? *from_base : fallback;
        if (value.is_null()) bad(what + " needs its " + slot);
        const Json& base_value = from_base != nullptr ? *from_base : fallback;
        out[slot] = setting_of(kind, slot, value, base_value, what);
    }
    for (std::string_view key : extra) {
        const Json* given = get(raw, key);
        const Json* from_base = base != nullptr ? get(*base, key) : nullptr;
        const Json* value = given != nullptr ? given : from_base;
        if (value == nullptr || value->is_null()) continue;
        if (key == "contour" && kind == "bevel") {
            out["contour"] = bevel_contour_of(*value, given != nullptr ? from_base : nullptr, what + "'s contour");
        } else if (key == "texture") {
            out["texture"] = texture_of(*value, given != nullptr ? from_base : nullptr, what + "'s texture");
        } else if (Json curve = contour_of(*value, what + "'s " + std::string(key)); !curve.is_null()) {
            out[std::string(key)] = std::move(curve);
        }
    }
    return out;
}

Json layer_style_spec(const Json& raw) {
    if (raw.is_null()) return nullptr;
    if (!raw.is_object()) bad("a style must be {\"effects\", \"enabled\"?, \"fill_opacity\"?, \"blend_if\"?}");
    for (const auto& [key, value] : raw.items()) {
        if (key != "effects" && key != "enabled" && key != "fill_opacity" && key != "blend_if") bad("a style has an unknown key: " + key);
    }
    Json effects = Json::array();
    if (const Json* given = get(raw, "effects"); given != nullptr && !given->is_null()) {
        if (!given->is_array()) bad("effects must be a list of effects");
        if (given->size() > kMaxEffects) bad("a style holds at most " + std::to_string(kMaxEffects) + " effects");
        for (const Json& e : *given) effects.push_back(layer_effect_spec(e));
    }
    bool enabled = true;
    if (const Json* v = get(raw, "enabled"); v != nullptr && !v->is_null()) enabled = bool_of(*v, "enabled");
    double fill = 1.0;
    if (const Json* v = get(raw, "fill_opacity"); v != nullptr && !v->is_null()) fill = py_round(number_of(*v, "fill_opacity", 0, 1), 4);
    Json blend_if = blend_if_of(get_or(raw, "blend_if", Json(nullptr)));
    if (effects.empty() && fill == 1.0 && blend_if.empty()) return nullptr;
    Json out = Json::object();
    out["effects"] = std::move(effects);
    if (!enabled && !out["effects"].empty()) out["enabled"] = false;
    if (fill != 1.0) out["fill_opacity"] = num(fill);
    if (!blend_if.empty()) out["blend_if"] = std::move(blend_if);
    return out;
}

bool same_json_unordered(const Json& a, const Json& b) {
    if (a.is_object() && b.is_object()) {
        if (a.size() != b.size()) return false;
        for (const auto& [key, value] : a.items()) {
            const Json* other = get(b, key);
            if (other == nullptr || !same_json_unordered(value, *other)) return false;
        }
        return true;
    }
    if (a.is_array() && b.is_array()) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!same_json_unordered(a[i], b[i])) return false;
        return true;
    }
    return a == b;
}

bool layer_style_normal(const Json& style) {
    try {
        const Json normal = layer_style_spec(style);
        return !normal.is_null() && same_json_unordered(normal, style);
    } catch (const Error&) {
        return false;
    }
}

bool has_layer_style(const Layer& layer) { return layer.style && layer.style->is_object(); }

bool takes_layer_style(const Layer& layer) {
    if (layer.role == LayerRole::Tone) return false;
    switch (layer.kind) {
        case LayerKind::Raster:
        case LayerKind::Strokes:
        case LayerKind::Fill:
        case LayerKind::Placed:
        case LayerKind::Smart:
            return true;
        default:
            return false;
    }
}

bool style_shows_effects(const Json& style) {
    if (!style.is_object() || style.value("enabled", true) == false) return false;
    const Json* effects = get(style, "effects");
    if (effects == nullptr || !effects->is_array()) return false;
    return std::any_of(effects->begin(), effects->end(), [](const Json& e) { return e.is_object() && e.value("enabled", true); });
}

double style_fill_opacity(const Json& style) {
    const Json* v = get(style, "fill_opacity");
    return v != nullptr && v->is_number() ? std::clamp(v->get<double>(), 0.0, 1.0) : 1.0;
}

std::array<double, 2> global_light(const Document& doc) {
    return doc.global_light.value_or(std::array<double, 2>{kDefaultLightAngle, kDefaultLightAltitude});
}

std::array<double, 2> global_light_of(const Json& raw) {
    if (!raw.is_object()) bad("the global light must be {\"angle\", \"altitude\"}");
    for (const auto& [key, value] : raw.items()) {
        if (key != "angle" && key != "altitude") bad("the global light has an unknown key: " + key);
    }
    double angle = kDefaultLightAngle, altitude = kDefaultLightAltitude;
    if (const Json* v = get(raw, "angle"); v != nullptr && !v->is_null()) angle = number_of(*v, "the global light's angle", -360, 360);
    if (const Json* v = get(raw, "altitude"); v != nullptr && !v->is_null()) altitude = number_of(*v, "the global light's altitude", 0, 90);
    angle = std::fmod(angle, 360.0);
    if (angle > 180) angle -= 360;
    if (angle <= -180) angle += 360;
    return {py_round(angle, 4) == 0 ? 0.0 : py_round(angle, 4), py_round(altitude, 4)};
}

Json global_light_json(const std::array<double, 2>& light) { return Json{{"angle", num(light[0])}, {"altitude", num(light[1])}}; }

bool uses_layer_styles(const Document& doc) {
    if (doc.global_light) return true;
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        for (const Layer& layer : written.page->layers) {
            if (has_layer_style(layer)) return true;
        }
    }
    return false;
}

void validate_layer_styles(const Document& doc) {
    if (doc.global_light) {
        const auto& [angle, altitude] = *doc.global_light;
        if (!std::isfinite(angle) || !std::isfinite(altitude) || angle <= -180 || angle > 180 || altitude < 0 || altitude > 90) {
            bad("the global light's angle is −180..180 and its altitude 0..90");
        }
    }
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        for (const Layer& layer : written.page->layers) {
            if (!layer.style) continue;
            if (!takes_layer_style(layer)) bad("the layer " + layer.id + " (a folder, correction or tone layer) cannot have a style");
            if (!layer_style_normal(*layer.style)) {
                try {
                    (void)layer_style_spec(*layer.style);
                } catch (const Error& error) {
                    throw Error("value", "the layer " + layer.id + "'s " + error.what());
                }
                bad("the layer " + layer.id + "'s style is not in its normal form");
            }
        }
    }
}

void each_style_pattern(const Json& style, const std::function<void(const Json&, const std::string&)>& each, const std::string& where) {
    const Json* effects = get(style, "effects");
    if (effects == nullptr || !effects->is_array()) return;
    for (const Json& e : *effects) {
        if (!e.is_object()) continue;
        const std::string kind = e.value("kind", std::string());
        if (const Json* p = get(e, "pattern"); p != nullptr && p->is_object()) each(*p, kind + " of " + where);
        if (const Json* t = get(e, "texture"); t != nullptr && t->is_object()) {
            if (const Json* p = get(*t, "pattern"); p != nullptr && p->is_object()) each(*p, "the bevel's texture of " + where);
        }
    }
}

void change_style_patterns(Json& style, const std::function<void(Json& fill)>& change) {
    if (!style.is_object() || !style.contains("effects") || !style["effects"].is_array()) return;
    for (Json& e : style["effects"]) {
        if (!e.is_object()) continue;
        if (e.contains("pattern") && e["pattern"].is_object()) change(e["pattern"]);
        if (e.contains("texture") && e["texture"].is_object() && e["texture"].contains("pattern") && e["texture"]["pattern"].is_object())
            change(e["texture"]["pattern"]);
    }
}

bool repoint_style_patterns(Json& style, const std::map<std::string, std::string>& ids) {
    if (ids.empty()) return false;
    bool any = false;
    change_style_patterns(style, [&](Json& p) {
        if (!p.contains("id") || !p["id"].is_string()) return;
        const auto to = ids.find(p["id"].get<std::string>());
        if (to == ids.end() || to->first == to->second) return;
        p["id"] = to->second;
        any = true;
    });
    return any;
}

Json scaled_layer_style(const Json& style, double factor) {
    if (!std::isfinite(factor) || factor < 0.01 || factor > 10) bad("the scale must be 1..1000 %");
    Json out = style;
    if (!out.is_object() || !out.contains("effects")) return out;
    for (Json& e : out["effects"]) {
        for (const char* key : {"size_mm", "distance_mm", "soften_mm"}) {
            if (e.contains(key) && e[key].is_number()) e[key] = num(std::clamp(e[key].get<double>() * factor, 0.0, kEffectMaxMm));
        }
        const auto scale_pattern = [factor](Json& p) {
            if (p.is_object() && p.contains("scale") && p["scale"].is_number())
                p["scale"] = num(std::clamp(p["scale"].get<double>() * factor, kPatternScaleMin, kPatternScaleMax));
        };
        if (e.contains("pattern")) scale_pattern(e["pattern"]);
        if (e.contains("texture") && e["texture"].is_object() && e["texture"].contains("pattern")) scale_pattern(e["texture"]["pattern"]);
    }
    return layer_style_spec(out);
}

double style_reach_mm(const Json& style) {
    if (!style_shows_effects(style)) return 0;
    double reach = 0;
    for (const Json& e : style["effects"]) {
        if (!e.is_object() || !e.value("enabled", true)) continue;
        const std::string kind = e.value("kind", std::string());
        const double size = e.value("size_mm", 0.0), distance = e.value("distance_mm", 0.0), soften = e.value("soften_mm", 0.0);
        double r = 0;
        if (kind == "drop_shadow" || kind == "inner_shadow" || kind == "satin") r = distance + size;
        else if (kind == "outer_glow" || kind == "inner_glow" || kind == "stroke") r = size;
        else if (kind == "bevel") r = size + soften;
        reach = std::max(reach, r);
    }
    return reach;
}

}  // namespace genko::core
