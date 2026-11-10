// A layer's style in a .pcraft both ways (render/pcraft_styles.hpp).

#include "render/pcraft_styles.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

#include "core/error.hpp"
#include "core/layer_styles.hpp"
#include "core/pyops.hpp"

namespace genko::render::pcraft {

namespace {

using core::Json;

double clamped(const Json& v, std::string_view what, double lo, double hi) { return std::clamp(number(v, what), lo, hi); }
double unit_of(const Json& object, std::string_view key, std::string_view what) { return clamped(field(object, key, what), what, 0.0, 1.0); }
double r4(double v) { return std::round(v * 10000.0) / 10000.0; }
double r6(double v) { return std::round(v * 1e6) / 1e6; }

// --- reading -----------------------------------------------------------------------------------------------------------

struct Reader {
    const StyleRead& in;

    double mm(const Json& object, std::string_view key, std::string_view what) const {
        return r4(std::clamp(number(field(object, key, what), what) * in.mm, 0.0, core::kEffectMaxMm));
    }
    double angle(const Json& object, std::string_view key, std::string_view what) const { return clamped(field(object, key, what), what, -360.0, 360.0); }

    std::string blend(const Json& value) const {
        const std::string name = blend_from(value);
        if (const auto mapped = genko_blend(name)) return *mapped;
        in.say("効果の描画モード「" + name + "」は Genko に無いため「通常」にしました");
        return "normal";
    }

    // FxCommon: {"enabled", "blend", "opacity"} onto the effect.
    void common(const Json& c, Json& out) const {
        out["enabled"] = boolean(field(c, "enabled", "effect"), "effect.enabled");
        out["blend"] = blend(field(c, "blend", "effect"));
        out["opacity"] = unit_of(c, "opacity", "effect.opacity");
    }

    // A contour: "Linear" (none) or {"Custom": {"name", "points": [{"input", "output"}, …]}}.
    std::optional<Json> contour(const Json& value) const {
        const auto [kind, data] = variant(value, "contour");
        if (kind == "Linear") return std::nullopt;
        if (kind != "Custom" || data == nullptr) throw Unreadable("contour: not a kind this build knows");
        const Json& points = field(*data, "points", "contour");
        if (!points.is_array()) throw Unreadable("contour: points");
        Json list = Json::array();
        std::set<double> inputs;
        for (const Json& p : points) {
            const double x = r4(unit_of(p, "input", "contour.input")), y = r4(unit_of(p, "output", "contour.output"));
            inputs.insert(x);
            list.push_back(Json::array({x, y}));
        }
        if (list.size() > 32 || inputs.size() < 2) {
            in.say("2〜32 点でない輪郭（等高線）は Genko に無いため「線形」にしました");
            return std::nullopt;
        }
        const Json* name = optional_field(*data, "name");
        std::string text = name != nullptr && name->is_string() ? name->get<std::string>() : std::string();
        if (text.size() > 255) text = first_chars(text, 255, 255);
        return Json{{"name", text}, {"points", std::move(list)}};
    }

    // A gradient: {"stops": [[t, colour]], "opacity_stops": [[t, a]], "style", "angle", "scale", "reverse", "align", "offset"}.
    std::optional<Json> gradient(const Json& g) const {
        const Json& stops = field(g, "stops", "gradient");
        if (!stops.is_array() || stops.empty() || stops.size() > 16) {
            in.say("色の分岐点が 1〜16 でないグラデーションの効果は Genko に無いため外しました");
            return std::nullopt;
        }
        Json list = Json::array();
        for (const Json& s : stops) {
            if (!s.is_array() || s.size() != 2) throw Unreadable("gradient: a stop is [position, colour]");
            list.push_back(Json::array({r4(clamped(s[0], "gradient", 0.0, 1.0)), in.rgb(s[1])}));
        }
        if (list.size() == 1) list.push_back(Json::array({1, list[0][1]}));
        Json out{{"stops", std::move(list)}};
        if (const Json* t = optional_field(g, "opacity_stops"); t != nullptr && t->is_array() && !t->empty()) {
            if (t->size() > 16) {
                in.say("不透明度の分岐点が 16 を超えるグラデーションの効果は、その不透明度を外しました");
            } else {
                Json alpha = Json::array();
                for (const Json& s : *t) {
                    if (!s.is_array() || s.size() != 2) throw Unreadable("gradient: an opacity stop is [position, opacity]");
                    alpha.push_back(Json::array({r4(clamped(s[0], "gradient", 0.0, 1.0)), r4(clamped(s[1], "gradient", 0.0, 1.0))}));
                }
                if (alpha.size() == 1) alpha.push_back(Json::array({1, alpha[0][1]}));
                out["transparency"] = std::move(alpha);
            }
        }
        const std::string style = text(field(g, "style", "gradient"), "gradient.style");
        if (style == "Linear") out["style"] = "linear";
        else if (style == "Radial") out["style"] = "radial";
        else if (style == "Angle") out["style"] = "angle";
        else if (style == "Reflected") out["style"] = "reflected";
        else if (style == "Diamond") out["style"] = "diamond";
        else throw Unreadable("gradient: not a style this build knows");
        out["angle"] = angle(g, "angle", "gradient.angle");
        out["scale"] = r4(clamped(field(g, "scale", "gradient"), "gradient.scale", 0.1, 1.5));
        out["reverse"] = boolean(field(g, "reverse", "gradient"), "gradient.reverse");
        out["align"] = boolean(field(g, "align", "gradient"), "gradient.align");
        const Json& offset = field(g, "offset", "gradient");
        if (!offset.is_array() || offset.size() != 2) throw Unreadable("gradient: offset is 2 numbers");
        out["offset"] = Json::array({r4(clamped(offset[0], "gradient.offset", -1.0, 1.0)), r4(clamped(offset[1], "gradient.offset", -1.0, 1.0))});
        return out;
    }

    std::optional<Json> pattern(const Json& d, bool placed) const {
        const Json* id = optional_field(d, "id");
        const std::string name = text(field(d, "name", "pattern"), "pattern.name");
        std::array<double, 2> phase{0, 0};
        double rotation = 0;
        bool link = true;
        if (placed) {  // (a pattern overlay's and a bevel's texture: its angle, link and phase)
            if (const Json* p = optional_field(d, "phase")) {
                if (!p->is_array() || p->size() != 2) throw Unreadable("pattern: phase is 2 numbers");
                phase = {number((*p)[0], "pattern.phase"), number((*p)[1], "pattern.phase")};
            }
            if (const Json* a = optional_field(d, "angle")) rotation = std::clamp(number(*a, "pattern.angle"), -360.0, 360.0);
            if (const Json* l = optional_field(d, "link")) link = boolean(*l, "pattern.link");
        }
        auto fill = in.pattern(id != nullptr && id->is_string() ? id->get<std::string>() : std::string(), name,
                               number(field(d, "scale", "pattern"), "pattern.scale"), rotation, link, phase);
        if (!fill) in.say("原稿に無いパターンの効果は外しました");
        return fill;
    }

    // A paint (FxPaint) into `out` as its kind takes it: "rgb", "gradient" or (a stroke's) "pattern". False: left out.
    bool paint(const Json& value, Json& out, bool patterns) const {
        const auto [kind, data] = variant(value, "paint");
        if (data == nullptr) throw Unreadable("paint: no data");
        if (kind == "Color") {
            out["rgb"] = in.rgb(*data);
            return true;
        }
        if (kind == "Gradient") {
            auto g = gradient(*data);
            if (!g) return false;
            out["gradient"] = std::move(*g);
            return true;
        }
        if (kind == "Pattern") {
            if (!patterns) {
                in.say("パターンで塗る光彩は Genko に無いため外しました");
                return false;
            }
            auto p = pattern(*data, false);
            if (!p) return false;
            out["pattern"] = std::move(*p);
            return true;
        }
        throw Unreadable("paint: not a kind this build knows");
    }

    std::optional<Json> shadow(const Json& d, bool inner) const {
        Json out{{"kind", inner ? "inner_shadow" : "drop_shadow"}};
        common(field(d, "common", "shadow"), out);
        out["rgb"] = in.rgb(field(d, "color", "shadow"));
        out["angle"] = angle(d, "angle", "shadow.angle");
        out["global_light"] = boolean(field(d, "use_global_light", "shadow"), "shadow.use_global_light");
        out["distance_mm"] = mm(d, "distance", "shadow.distance");
        out[inner ? "choke" : "spread"] = unit_of(d, "spread", "shadow.spread");
        out["size_mm"] = mm(d, "size", "shadow.size");
        out["noise"] = unit_of(d, "noise", "shadow.noise");
        out["anti_alias"] = boolean(field(d, "anti_alias", "shadow"), "shadow.anti_alias");
        if (auto c = contour(field(d, "contour", "shadow"))) out["contour"] = std::move(*c);
        if (!inner) out["knocks_out"] = boolean(field(d, "knocks_out", "shadow"), "shadow.knocks_out");
        return out;
    }

    std::optional<Json> glow(const Json& d, bool inner) const {
        Json out{{"kind", inner ? "inner_glow" : "outer_glow"}};
        common(field(d, "common", "glow"), out);
        if (!paint(field(d, "paint", "glow"), out, false)) return std::nullopt;
        const std::string technique = text(field(d, "technique", "glow"), "glow.technique");
        if (technique != "Softer" && technique != "Precise") throw Unreadable("glow: technique");
        out["technique"] = technique == "Softer" ? "softer" : "precise";
        if (inner) {
            const std::string source = text(field(d, "source", "glow"), "glow.source");
            if (source != "Edge" && source != "Center") throw Unreadable("glow: source");
            out["source"] = source == "Edge" ? "edge" : "center";
        }
        out[inner ? "choke" : "spread"] = unit_of(d, "spread", "glow.spread");
        out["size_mm"] = mm(d, "size", "glow.size");
        out["range"] = r4(clamped(field(d, "range", "glow"), "glow.range", 0.01, 1.0));
        out["jitter"] = unit_of(d, "jitter", "glow.jitter");
        out["noise"] = unit_of(d, "noise", "glow.noise");
        out["anti_alias"] = boolean(field(d, "anti_alias", "glow"), "glow.anti_alias");
        if (auto c = contour(field(d, "contour", "glow"))) out["contour"] = std::move(*c);
        return out;
    }

    Json tone(const Json& common_value, const Json& colour) const {
        Json out = Json::object();
        Json c;
        common(common_value, c);
        out["blend"] = c["blend"];
        out["opacity"] = c["opacity"];
        if (!c["enabled"].get<bool>()) {
            in.say("ベベルとエンボスの切ったハイライト・シャドウは、不透明度 0 にしました");
            out["opacity"] = 0;
        }
        out["rgb"] = in.rgb(colour);
        return out;
    }

    std::optional<Json> bevel(const Json& d) const {
        Json out{{"kind", "bevel"}};
        out["enabled"] = boolean(field(d, "enabled", "bevel"), "bevel.enabled");
        const std::string style = text(field(d, "style", "bevel"), "bevel.style");
        if (style == "InnerBevel") out["style"] = "inner";
        else if (style == "OuterBevel") out["style"] = "outer";
        else if (style == "Emboss") out["style"] = "emboss";
        else if (style == "PillowEmboss") out["style"] = "pillow";
        else if (style == "StrokeEmboss") out["style"] = "stroke";
        else throw Unreadable("bevel: style");
        const std::string technique = text(field(d, "technique", "bevel"), "bevel.technique");
        if (technique == "Smooth") out["technique"] = "smooth";
        else if (technique == "ChiselHard") out["technique"] = "chisel_hard";
        else if (technique == "ChiselSoft") out["technique"] = "chisel_soft";
        else throw Unreadable("bevel: technique");
        out["depth"] = r4(clamped(field(d, "depth", "bevel"), "bevel.depth", 0.01, 10.0));
        out["direction"] = boolean(field(d, "up", "bevel"), "bevel.up") ? "up" : "down";
        out["size_mm"] = mm(d, "size", "bevel.size");
        out["soften_mm"] = mm(d, "soften", "bevel.soften");
        out["angle"] = angle(d, "angle", "bevel.angle");
        out["altitude"] = clamped(field(d, "altitude", "bevel"), "bevel.altitude", 0.0, 90.0);
        out["global_light"] = boolean(field(d, "use_global_light", "bevel"), "bevel.use_global_light");
        out["highlight"] = tone(field(d, "highlight", "bevel"), field(d, "highlight_color", "bevel"));
        out["shadow"] = tone(field(d, "shadow", "bevel"), field(d, "shadow_color", "bevel"));
        if (auto c = contour(field(d, "gloss_contour", "bevel"))) out["gloss_contour"] = std::move(*c);
        if (const Json* bc = optional_field(d, "contour")) {
            Json element = Json::object();
            if (auto c = contour(field(*bc, "contour", "bevel.contour"))) element["curve"] = std::move(*c);
            element["range"] = r4(clamped(field(*bc, "range", "bevel.contour"), "bevel.contour.range", 0.01, 1.0));
            element["anti_alias"] = boolean(field(*bc, "anti_alias", "bevel.contour"), "bevel.contour.anti_alias");
            out["contour"] = std::move(element);
        }
        if (const Json* t = optional_field(d, "texture")) {
            if (auto p = pattern(*t, true)) {
                out["texture"] = Json{{"pattern", std::move(*p)},
                                      {"depth", r4(clamped(field(*t, "depth", "bevel.texture"), "bevel.texture.depth", -10.0, 10.0))},
                                      {"invert", boolean(field(*t, "invert", "bevel.texture"), "bevel.texture.invert")}};
            }
        }
        return out;
    }

    std::optional<Json> effect(const Json& item) const {
        const auto [kind, data] = variant(item, "effect");
        if (data == nullptr) throw Unreadable("effect: no data");
        const Json& d = *data;
        if (kind == "DropShadow" || kind == "InnerShadow") return shadow(d, kind == "InnerShadow");
        if (kind == "OuterGlow" || kind == "InnerGlow") return glow(d, kind == "InnerGlow");
        if (kind == "BevelEmboss") return bevel(d);
        if (kind == "Stroke") {
            Json out{{"kind", "stroke"}};
            common(field(d, "common", "stroke"), out);
            out["size_mm"] = mm(d, "size", "stroke.size");
            const std::string position = text(field(d, "position", "stroke"), "stroke.position");
            if (position != "Outside" && position != "Inside" && position != "Center") throw Unreadable("stroke: position");
            out["position"] = position == "Outside" ? "outside" : position == "Inside" ? "inside" : "center";
            if (!paint(field(d, "paint", "stroke"), out, true)) return std::nullopt;
            return out;
        }
        if (kind == "Satin") {
            Json out{{"kind", "satin"}};
            common(field(d, "common", "satin"), out);
            out["rgb"] = in.rgb(field(d, "color", "satin"));
            out["angle"] = angle(d, "angle", "satin.angle");
            out["distance_mm"] = mm(d, "distance", "satin.distance");
            out["size_mm"] = mm(d, "size", "satin.size");
            out["invert"] = boolean(field(d, "invert", "satin"), "satin.invert");
            out["anti_alias"] = boolean(field(d, "anti_alias", "satin"), "satin.anti_alias");
            if (auto c = contour(field(d, "contour", "satin"))) out["contour"] = std::move(*c);
            return out;
        }
        if (kind == "ColorOverlay") {
            Json out{{"kind", "color_overlay"}};
            common(field(d, "common", "color overlay"), out);
            out["rgb"] = in.rgb(field(d, "color", "color overlay"));
            return out;
        }
        if (kind == "GradientOverlay") {
            Json out{{"kind", "gradient_overlay"}};
            common(field(d, "common", "gradient overlay"), out);
            auto g = gradient(field(d, "gradient", "gradient overlay"));
            if (!g) return std::nullopt;
            out["gradient"] = std::move(*g);
            out["dither"] = boolean(field(d, "dither", "gradient overlay"), "gradient overlay.dither");
            return out;
        }
        if (kind == "PatternOverlay") {
            Json out{{"kind", "pattern_overlay"}};
            common(field(d, "common", "pattern overlay"), out);
            auto p = pattern(d, true);
            if (!p) return std::nullopt;
            out["pattern"] = std::move(*p);
            return out;
        }
        in.say("この版の Genko が知らない種類のレイヤー効果（" + kind + "）を外しました");
        return std::nullopt;
    }
};

// Blend If's channels in the document's mode (after the composite grey); a grey document's one channel is the composite's.
std::vector<std::string> channels_of(Mode mode) {
    switch (mode) {
    case Mode::Grayscale:
    case Mode::Bitmap:
    case Mode::Duotone:
        return {};
    case Mode::Cmyk:
        return {"cyan", "magenta", "yellow", "black"};
    case Mode::Lab:
        return {"lightness", "a", "b"};
    default:
        return {"red", "green", "blue"};
    }
}

bool grey_mode(Mode mode) { return mode == Mode::Grayscale || mode == Mode::Bitmap || mode == Mode::Duotone; }

// One BlendRange {"black": [lo, hi], "white": [lo, hi]} as Genko's [black low, black high, white low, white high]: crossed
// halves as upstream weighs them (a step at the black's low, at the white's high).
Json range_from(const Json& r) {
    const auto two = [&](const char* key) {
        const Json& v = field(r, key, "blend_if");
        if (!v.is_array() || v.size() != 2) throw Unreadable("blend_if: a point is 2 numbers");
        return std::array<std::int64_t, 2>{std::clamp<std::int64_t>(whole(v[0], "blend_if"), 0, 255), std::clamp<std::int64_t>(whole(v[1], "blend_if"), 0, 255)};
    };
    auto black = two("black"), white = two("white");
    if (black[1] < black[0]) black[1] = black[0];
    if (white[0] > white[1]) white[0] = white[1];
    return Json::array({black[0], black[1], white[0], white[1]});
}

const Json kFull = Json::array({0, 0, 255, 255});

Json blend_if_from(const Json& blend_if, Mode mode, const StyleRead& in) {
    Json out = Json::array();
    const Json* ranges = optional_field(blend_if, "ranges");
    if (ranges == nullptr) return out;
    if (!ranges->is_array()) throw Unreadable("blend_if: ranges");
    const std::vector<std::string> names = channels_of(mode);
    bool past = false, twice = false;
    std::optional<std::pair<Json, Json>> grey;
    for (std::size_t i = 0; i < ranges->size(); ++i) {
        const Json& pair = (*ranges)[i];
        if (!pair.is_array() || pair.size() != 2) throw Unreadable("blend_if: an entry is [this layer, underlying]");
        Json self = range_from(pair[0]), under = range_from(pair[1]);
        if (self == kFull && under == kFull) continue;
        if (i == 0 || (i == 1 && grey_mode(mode))) {  // (a grey document's channel is its composite)
            if (grey && (grey->first != self || grey->second != under)) {
                twice = true;
                continue;
            }
            grey = std::make_pair(self, under);
            continue;
        }
        if (i - 1 >= names.size()) {
            past = true;
            continue;
        }
        out.push_back(Json{{"channel", names[i - 1]}, {"this", std::move(self)}, {"under", std::move(under)}});
    }
    if (grey) out.insert(out.begin(), Json{{"channel", "gray"}, {"this", grey->first}, {"under", grey->second}});
    if (twice) in.say("グレーの原稿の合成とグレーのチャンネルで違うブレンド条件は、合成のものにしました");
    if (past) in.say("原稿の色モードに無いチャンネルのブレンド条件は外しました");
    return out;
}

}  // namespace

bool has_upstream_effects(const Json& layer) {
    const Json* effects = optional_field(layer, "effects");
    if (effects == nullptr) return false;
    const Json* items = optional_field(*effects, "items");
    return (items != nullptr && items->is_array() && !items->empty()) || optional_field(*effects, "psd_raw") != nullptr;
}

bool has_upstream_blend_if(const Json& layer) {
    const Json* blend_if = optional_field(layer, "blend_if");
    if (blend_if == nullptr) return false;
    const Json* ranges = blend_if->is_object() ? optional_field(*blend_if, "ranges") : nullptr;
    if (ranges == nullptr || !ranges->is_array()) return !blend_if->is_object();
    for (const Json& pair : *ranges) {
        if (!pair.is_array()) return true;
        for (const Json& r : pair) {
            try {
                if (range_from(r) != kFull) return true;
            } catch (const Unreadable&) {
                return true;
            }
        }
    }
    return false;
}

Json style_from_upstream(const Json& layer, const StyleRead& in) {
    const Reader reader{in};
    Json style = Json::object();
    Json effects = Json::array();
    if (const Json* fx = optional_field(layer, "effects")) {
        if (const Json* items = optional_field(*fx, "items")) {
            if (!items->is_array()) throw Unreadable("effects: items");
            for (const Json& item : *items) {
                if (effects.size() >= core::kMaxEffects) {
                    in.say("レイヤー効果が " + std::to_string(core::kMaxEffects) + " を超えるため、残りは外しました");
                    break;
                }
                std::optional<Json> made = reader.effect(item);
                if (!made) continue;
                try {
                    effects.push_back(core::layer_effect_spec(*made));
                } catch (const core::Error& error) {
                    in.say(std::string("読めないレイヤー効果を外しました（") + error.what() + "）");
                }
            }
        }
        const Json* enabled = optional_field(*fx, "enabled");
        if (enabled != nullptr && !boolean(*enabled, "effects.enabled") && !effects.empty()) style["enabled"] = false;
        if (optional_field(*fx, "psd_raw") != nullptr) in.say("レイヤー効果の PSD の元の設定は Genko に無いため外しました（効果は読みました）");
        if (optional_field(*fx, "reference") != nullptr) in.say("レイヤー効果の基準点は Genko に無いため、レイヤーの左上にしました");
    }
    style["effects"] = std::move(effects);
    if (const Json* fill = optional_field(layer, "fill_opacity")) style["fill_opacity"] = r4(std::clamp(number(*fill, "layer.fill_opacity"), 0.0, 1.0));
    if (const Json* blend_if = optional_field(layer, "blend_if")) {
        if (Json made = blend_if_from(*blend_if, in.mode, in); !made.empty()) style["blend_if"] = std::move(made);
    }
    return core::layer_style_spec(style);
}

// --- writing -----------------------------------------------------------------------------------------------------------

namespace {

struct Writer {
    const StyleWrite& out;

    Json colour(const Json& rgb) const {
        double c[3] = {0, 0, 0};
        if (rgb.is_array() && rgb.size() == 3) {
            for (std::size_t i = 0; i < 3; ++i) c[i] = rgb[i].is_number() ? rgb[i].get<double>() / 255.0 : 0.0;
        }
        return to_json(rgb_color(c[0], c[1], c[2], 1.0));
    }
    double px(const Json& e, const char* key) const { return r6(e.value(key, 0.0) * out.px); }
    static double d(const Json& e, const char* key, double fallback = 0.0) {
        const Json* v = core::get(e, key);
        return v != nullptr && v->is_number() ? v->get<double>() : fallback;
    }
    static bool b(const Json& e, const char* key, bool fallback = false) {
        const Json* v = core::get(e, key);
        return v != nullptr && v->is_boolean() ? v->get<bool>() : fallback;
    }
    static Json common(const Json& e) {
        return Json{{"enabled", b(e, "enabled", true)}, {"blend", upstream_blend(e.value("blend", std::string("normal")))}, {"opacity", d(e, "opacity", 1.0)}};
    }
    static Json contour(const Json* c) {
        if (c == nullptr || !c->is_object()) return "Linear";
        Json points = Json::array();
        for (const Json& p : c->value("points", Json::array())) points.push_back(Json{{"input", p[0].get<double>()}, {"output", p[1].get<double>()}});
        return Json{{"Custom", Json{{"name", c->value("name", std::string())}, {"points", std::move(points)}}}};
    }
    Json gradient(const Json& g) const {
        Json stops = Json::array();
        for (const Json& s : g.value("stops", Json::array())) stops.push_back(Json::array({s[0].get<double>(), colour(s[1])}));
        Json alpha = Json::array();
        for (const Json& s : g.value("transparency", Json::array())) alpha.push_back(Json::array({s[0].get<double>(), s[1].get<double>()}));
        const std::string style = g.value("style", std::string("linear"));
        const std::string upstream = style == "radial" ? "Radial" : style == "angle" ? "Angle" : style == "reflected" ? "Reflected" : style == "diamond" ? "Diamond" : "Linear";
        const Json offset = g.value("offset", Json::array({0, 0}));
        return Json{{"stops", std::move(stops)}, {"opacity_stops", std::move(alpha)}, {"style", upstream}, {"angle", d(g, "angle", 90)},
                    {"scale", d(g, "scale", 1)},     {"reverse", b(g, "reverse")},      {"align", b(g, "align", true)},
                    {"offset", Json::array({offset[0].get<double>(), offset[1].get<double>()})}};
    }
    std::optional<Json> pattern(const Json& fill) const {
        auto p = out.pattern(fill);
        if (!p) out.say("本に無いパターンのレイヤー効果は書きませんでした");
        return p;
    }
    // A stroke's or a glow's paint (FxPaint).
    std::optional<Json> paint(const Json& e) const {
        if (const Json* g = core::get(e, "gradient"); g != nullptr && g->is_object()) return Json{{"Gradient", gradient(*g)}};
        if (const Json* p = core::get(e, "pattern"); p != nullptr && p->is_object()) {
            auto made = pattern(*p);
            if (!made) return std::nullopt;
            const Json phase = (*made)["phase"];
            if ((*made)["angle"].get<double>() != 0.0 || !(*made)["link"].get<bool>() || phase[0].get<double>() != 0.0 || phase[1].get<double>() != 0.0)
                out.say("線のパターンの角度・位置・レイヤーとのリンクは PhotoCraft に無いため外しました");
            return Json{{"Pattern", Json{{"name", (*made)["name"]}, {"id", (*made)["id"]}, {"scale", (*made)["scale"]}}}};
        }
        return Json{{"Color", colour(e.value("rgb", Json::array({0, 0, 0})))}};
    }

    std::optional<Json> effect(const Json& e) const {
        const std::string kind = e.value("kind", std::string());
        if (kind == "drop_shadow" || kind == "inner_shadow") {
            const bool inner = kind == "inner_shadow";
            return Json{{inner ? "InnerShadow" : "DropShadow",
                         Json{{"common", common(e)},
                              {"color", colour(e.value("rgb", Json::array({0, 0, 0})))},
                              {"angle", d(e, "angle", 120)},
                              {"use_global_light", b(e, "global_light", true)},
                              {"distance", px(e, "distance_mm")},
                              {"spread", d(e, inner ? "choke" : "spread")},
                              {"size", px(e, "size_mm")},
                              {"contour", contour(core::get(e, "contour"))},
                              {"anti_alias", b(e, "anti_alias")},
                              {"noise", d(e, "noise")},
                              {"knocks_out", !inner && b(e, "knocks_out", true)}}}};
        }
        if (kind == "outer_glow" || kind == "inner_glow") {
            const bool inner = kind == "inner_glow";
            auto p = paint(e);
            if (!p) return std::nullopt;
            return Json{{inner ? "InnerGlow" : "OuterGlow",
                         Json{{"common", common(e)},
                              {"paint", std::move(*p)},
                              {"technique", e.value("technique", std::string("softer")) == "precise" ? "Precise" : "Softer"},
                              {"spread", d(e, inner ? "choke" : "spread")},
                              {"size", px(e, "size_mm")},
                              {"contour", contour(core::get(e, "contour"))},
                              {"anti_alias", b(e, "anti_alias")},
                              {"range", d(e, "range", 0.5)},
                              {"jitter", d(e, "jitter")},
                              {"noise", d(e, "noise")},
                              {"source", e.value("source", std::string("edge")) == "center" ? "Center" : "Edge"}}}};
        }
        if (kind == "stroke") {
            auto p = paint(e);
            if (!p) return std::nullopt;
            const std::string position = e.value("position", std::string("outside"));
            return Json{{"Stroke", Json{{"common", common(e)},
                                        {"size", px(e, "size_mm")},
                                        {"position", position == "inside" ? "Inside" : position == "center" ? "Center" : "Outside"},
                                        {"paint", std::move(*p)}}}};
        }
        if (kind == "satin") {
            return Json{{"Satin", Json{{"common", common(e)},
                                       {"color", colour(e.value("rgb", Json::array({0, 0, 0})))},
                                       {"angle", d(e, "angle", 19)},
                                       {"distance", px(e, "distance_mm")},
                                       {"size", px(e, "size_mm")},
                                       {"contour", contour(core::get(e, "contour"))},
                                       {"anti_alias", b(e, "anti_alias")},
                                       {"invert", b(e, "invert")}}}};
        }
        if (kind == "color_overlay") return Json{{"ColorOverlay", Json{{"common", common(e)}, {"color", colour(e.value("rgb", Json::array({0, 0, 0})))}}}};
        if (kind == "gradient_overlay") {
            const Json* g = core::get(e, "gradient");
            return Json{{"GradientOverlay", Json{{"common", common(e)}, {"gradient", gradient(g != nullptr ? *g : Json::object())}, {"dither", b(e, "dither")}}}};
        }
        if (kind == "pattern_overlay") {
            const Json* fill = core::get(e, "pattern");
            auto p = fill != nullptr ? pattern(*fill) : std::nullopt;
            if (!p) return std::nullopt;
            Json body{{"common", common(e)}};
            for (const char* key : {"name", "id", "scale", "angle", "link", "phase"}) body[key] = (*p)[key];
            return Json{{"PatternOverlay", std::move(body)}};
        }
        if (kind == "bevel") {
            const std::string style = e.value("style", std::string("inner"));
            const std::string technique = e.value("technique", std::string("smooth"));
            const auto tone = [&](const char* key) {
                const Json* t = core::get(e, key);
                const Json part = t != nullptr && t->is_object() ? *t : Json::object();
                return Json{{"enabled", true}, {"blend", upstream_blend(part.value("blend", std::string("normal")))}, {"opacity", d(part, "opacity", 0.75)}};
            };
            const auto tone_colour = [&](const char* key) {
                const Json* t = core::get(e, key);
                return colour(t != nullptr && t->is_object() ? t->value("rgb", Json::array({0, 0, 0})) : Json::array({0, 0, 0}));
            };
            Json body{{"enabled", b(e, "enabled", true)},
                      {"style", style == "outer" ? "OuterBevel" : style == "emboss" ? "Emboss" : style == "pillow" ? "PillowEmboss" : style == "stroke" ? "StrokeEmboss" : "InnerBevel"},
                      {"technique", technique == "chisel_hard" ? "ChiselHard" : technique == "chisel_soft" ? "ChiselSoft" : "Smooth"},
                      {"depth", d(e, "depth", 1)},
                      {"up", e.value("direction", std::string("up")) != "down"},
                      {"size", px(e, "size_mm")},
                      {"soften", px(e, "soften_mm")},
                      {"angle", d(e, "angle", 120)},
                      {"altitude", d(e, "altitude", 30)},
                      {"use_global_light", b(e, "global_light", true)},
                      {"gloss_contour", contour(core::get(e, "gloss_contour"))},
                      {"highlight", tone("highlight")},
                      {"highlight_color", tone_colour("highlight")},
                      {"shadow", tone("shadow")},
                      {"shadow_color", tone_colour("shadow")},
                      {"contour", nullptr},
                      {"texture", nullptr}};
            if (const Json* c = core::get(e, "contour"); c != nullptr && c->is_object()) {
                body["contour"] = Json{{"contour", contour(core::get(*c, "curve"))}, {"range", d(*c, "range", 0.5)}, {"anti_alias", b(*c, "anti_alias")}};
            }
            if (const Json* t = core::get(e, "texture"); t != nullptr && t->is_object()) {
                const Json* fill = core::get(*t, "pattern");
                if (auto p = fill != nullptr ? pattern(*fill) : std::nullopt) {
                    body["texture"] = Json{{"name", (*p)["name"]}, {"id", (*p)["id"]}, {"scale", (*p)["scale"]}, {"depth", d(*t, "depth", 1)},
                                           {"invert", b(*t, "invert")}, {"link", (*p)["link"]}, {"phase", (*p)["phase"]}};
                } else {
                    out.say("ベベルとエンボスのテクスチャは、本に無いパターンのため外しました");
                }
            }
            return Json{{"BevelEmboss", std::move(body)}};
        }
        return std::nullopt;
    }
};

}  // namespace

Json effects_to_upstream(const Json& style, const StyleWrite& out) {
    const Writer writer{out};
    Json items = Json::array();
    if (const Json* effects = core::get(style, "effects"); effects != nullptr && effects->is_array()) {
        for (const Json& e : *effects) {
            if (auto made = writer.effect(e)) items.push_back(std::move(*made));
        }
    }
    const Json* enabled = core::get(style, "enabled");
    return Json{{"enabled", !(enabled != nullptr && enabled->is_boolean() && !enabled->get<bool>())}, {"items", std::move(items)}, {"psd_raw", nullptr}, {"reference", nullptr}};
}

Json blend_if_to_upstream(const Json& style, const StyleWrite& out) {
    const Json* blend_if = core::get(style, "blend_if");
    if (blend_if == nullptr || !blend_if->is_array() || blend_if->empty()) return nullptr;
    const std::vector<std::string> names = channels_of(out.mode);
    std::vector<std::pair<Json, Json>> ranges;
    bool missing = false;
    for (const Json& entry : *blend_if) {
        const std::string channel = entry.value("channel", std::string());
        std::size_t at = 0;
        if (channel != "gray") {
            const auto it = std::find(names.begin(), names.end(), channel);
            if (it == names.end()) {
                missing = true;
                continue;
            }
            at = static_cast<std::size_t>(it - names.begin()) + 1;
        }
        if (ranges.size() <= at) ranges.resize(at + 1, {kFull, kFull});
        ranges[at] = {entry["this"], entry["under"]};
    }
    if (missing) out.say("ページの色モードに無いチャンネルのブレンド条件は書きませんでした");
    if (ranges.empty()) return nullptr;
    const auto range = [](const Json& r) { return Json{{"black", Json::array({r[0], r[1]})}, {"white", Json::array({r[2], r[3]})}}; };
    Json list = Json::array();
    for (const auto& [self, under] : ranges) list.push_back(Json::array({range(self), range(under)}));
    return Json{{"ranges", std::move(list)}};
}

}  // namespace genko::render::pcraft
