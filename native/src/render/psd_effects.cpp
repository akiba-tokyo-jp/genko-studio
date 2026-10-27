// PC4 E6 (layer-styles@1, render/psd_effects.hpp): PhotoCraft v0.2.0 crates/io/src/effects_map.rs (BLEND_NAMES, the
// effects' keys parse_one / write_one, KINDS with their multi lists, contours, parse_lfx2, write_lfx2) and blocks.rs
// (colours, gradients), crates/doc/src/blend_if.rs (the blending ranges' byte order), MIT:
// native/third_party/photocraft/NOTICE.md — in the book's mm and its colours, through Genko's descriptor reader (its
// budget: render/psd_descriptor) and pattern items (render/psd_shapes).

#include "render/psd_effects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "core/error.hpp"
#include "core/layer_styles.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"

namespace genko::render::psd_effects {

using core::Json;
namespace d = psd::descriptor;

namespace {

constexpr std::array<std::pair<std::string_view, std::string_view>, 20> kBlends{{
    {"normal", "Nrml"},       {"multiply", "Mltp"},          {"screen", "Scrn"},    {"add", "linearDodge"},  {"overlay", "Ovrl"},
    {"darken", "Drkn"},       {"lighten", "Lghn"},           {"color_burn", "CBrn"}, {"color_dodge", "CDdg"}, {"linear_burn", "linearBurn"},
    {"soft_light", "SftL"},   {"hard_light", "HrdL"},        {"difference", "Dfrn"}, {"exclusion", "Xclu"},  {"subtract", "blendSubtraction"},
    {"divide", "blendDivide"}, {"hue", "H   "},              {"saturation", "Strt"}, {"color", "Clr "},       {"luminosity", "Lmns"},
}};

// Photoshop's modes Genko has not, by their ids (said when read).
const std::map<std::string, std::string, std::less<>>& missing_modes() {
    static const std::map<std::string, std::string, std::less<>> m{{"Dslv", "ディザ合成"},      {"darkerColor", "カラー比較（暗）"}, {"lighterColor", "カラー比較（明）"},
                                                                   {"vividLight", "ビビッドライト"}, {"linearLight", "リニアライト"},     {"pinLight", "ピンライト"},
                                                                   {"hardMix", "ハードミックス"},   {"passThrough", "通過"}};
    return m;
}

// The effects' kinds: Genko's, the descriptor's key and its list of several.
struct KindKeys {
    std::string_view kind, key, multi;
};
constexpr std::array<KindKeys, 10> kKinds{{{"drop_shadow", "DrSh", "dropShadowMulti"},
                                           {"inner_shadow", "IrSh", "innerShadowMulti"},
                                           {"outer_glow", "OrGl", ""},
                                           {"inner_glow", "IrGl", ""},
                                           {"bevel", "ebbl", ""},
                                           {"satin", "ChFX", ""},
                                           {"color_overlay", "SoFi", "solidFillMulti"},
                                           {"gradient_overlay", "GrFl", "gradientFillMulti"},
                                           {"pattern_overlay", "patternFill", ""},
                                           {"stroke", "FrFX", "frameFXMulti"}}};

double num(const Json& e, const char* key, double fallback) {
    const Json* v = core::get(e, key);
    return v != nullptr && v->is_number() ? v->get<double>() : fallback;
}
bool flag(const Json& e, const char* key, bool fallback) {
    const Json* v = core::get(e, key);
    return v != nullptr && v->is_boolean() ? v->get<bool>() : fallback;
}
std::string word(const Json& e, const char* key, const char* fallback) {
    const Json* v = core::get(e, key);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string(fallback);
}

// --- writing ---------------------------------------------------------------------------------------------------------

double px_per_mm(const psd_shapes::Canvas& canvas) {
    if (canvas.width_px > 0 && canvas.width_mm > 0) return canvas.width_px / canvas.width_mm;
    return std::max(canvas.dpi, 1.0) / 25.4;
}

d::Writer colour_of(const Json& rgb) {
    d::Writer c;
    const auto at = [&](std::size_t k) { return rgb.is_array() && rgb.size() > k && rgb[k].is_number() ? rgb[k].get<double>() : 0.0; };
    c.number("Rd  ", at(0));
    c.number("Grn ", at(1));
    c.number("Bl  ", at(2));
    return c;
}

void put_blend(d::Writer& w, std::string_view key, const std::string& mode) { w.enumerated(key, "BlnM", blend_id(mode)); }

d::Writer contour_of(const Json* contour) {
    d::Writer c;
    std::string name = "Linear";
    std::vector<std::pair<double, double>> points{{0, 0}, {1, 1}};
    if (contour != nullptr && contour->is_object()) {
        name = contour->value("name", std::string());
        points.clear();
        for (const Json& p : (*contour)["points"]) points.emplace_back(p[0].get<double>(), p[1].get<double>());
    }
    c.text("Nm  ", psd::utf16(name));
    std::vector<std::pair<std::string, d::Writer>> pts;
    for (const auto& [x, y] : points) {
        d::Writer p;
        p.number("Hrzn", x * 255.0);
        p.number("Vrtc", y * 255.0);
        pts.emplace_back("CrPt", std::move(p));
    }
    c.objects("Crv ", pts);
    return c;
}

void put_common(d::Writer& w, const Json& e, const char* blend) {
    w.boolean("enab", flag(e, "enabled", true));
    w.boolean("present", true);
    w.boolean("showInDialog", true);
    put_blend(w, "Md  ", word(e, "blend", blend));
}

// A gradient's keys (Grad and its placement).
void put_gradient(d::Writer& w, const Json& g) {
    d::Writer grad;
    grad.text("Nm  ", psd::utf16("Custom"));
    grad.enumerated("GrdF", "GrdF", "CstS");
    grad.number("Intr", 4096);
    std::vector<std::pair<std::string, d::Writer>> colours, opacities;
    for (const Json& s : g["stops"]) {
        d::Writer stop;
        stop.object("Clr ", colour_of(s[1]), "RGBC");
        stop.enumerated("Type", "Clry", "UsrS");
        stop.integer("Lctn", static_cast<std::int32_t>(std::lround(s[0].get<double>() * 4096)));
        stop.integer("Mdpn", 50);
        colours.emplace_back("Clrt", std::move(stop));
    }
    const Json transparency = g.contains("transparency") ? g["transparency"] : Json::array({Json::array({0, 1}), Json::array({1, 1})});
    for (const Json& s : transparency) {
        d::Writer stop;
        stop.unit("Opct", "#Prc", s[1].get<double>() * 100.0);
        stop.integer("Lctn", static_cast<std::int32_t>(std::lround(s[0].get<double>() * 4096)));
        stop.integer("Mdpn", 50);
        opacities.emplace_back("TrnS", std::move(stop));
    }
    grad.objects("Clrs", colours);
    grad.objects("Trns", opacities);
    w.object("Grad", grad, "Grdn");
    w.unit("Angl", "#Ang", num(g, "angle", 90));
    const std::string style = word(g, "style", "linear");
    w.enumerated("Type", "GrdT", style == "radial" ? "Rdl " : style == "angle" ? "Angl" : style == "reflected" ? "Rflc" : style == "diamond" ? "Dmnd" : "Lnr ");
    w.boolean("Rvrs", flag(g, "reverse", false));
    w.boolean("Algn", flag(g, "align", true));
    w.unit("Scl ", "#Prc", num(g, "scale", 1) * 100.0);
    d::Writer offset;
    const Json o = g.value("offset", Json::array({0, 0}));
    offset.unit("Hrzn", "#Prc", o[0].get<double>() * 100.0);
    offset.unit("Vrtc", "#Prc", o[1].get<double>() * 100.0);
    w.object("Ofst", offset, "Pnt ");
}

std::pair<std::string_view, d::Writer> write_one(const Json& e, const psd_shapes::Canvas& canvas) {
    const double k = px_per_mm(canvas);
    const std::string kind = word(e, "kind", "");
    const auto px = [&](const char* key) { return num(e, key, 0) * k; };
    d::Writer w;
    if (kind == "drop_shadow" || kind == "inner_shadow") {
        const bool inner = kind == "inner_shadow";
        put_common(w, e, "multiply");
        w.object("Clr ", colour_of(e["rgb"]), "RGBC");
        w.unit("Opct", "#Prc", num(e, "opacity", 0.75) * 100.0);
        w.boolean("uglg", flag(e, "global_light", true));
        w.unit("lagl", "#Ang", num(e, "angle", 120));
        w.unit("Dstn", "#Pxl", px("distance_mm"));
        w.unit("Ckmt", "#Pxl", num(e, inner ? "choke" : "spread", 0) * 100.0);
        w.unit("blur", "#Pxl", px("size_mm"));
        w.unit("Nose", "#Prc", num(e, "noise", 0) * 100.0);
        w.boolean("AntA", flag(e, "anti_alias", false));
        w.object("TrnS", contour_of(core::get(e, "contour")), "ShpC");
        if (!inner) w.boolean("layerConceals", flag(e, "knocks_out", true));
        return {inner ? "IrSh" : "DrSh", std::move(w)};
    }
    if (kind == "outer_glow" || kind == "inner_glow") {
        const bool inner = kind == "inner_glow";
        put_common(w, e, "screen");
        if (e.contains("gradient")) put_gradient(w, e["gradient"]);
        else w.object("Clr ", colour_of(e["rgb"]), "RGBC");
        w.unit("Opct", "#Prc", num(e, "opacity", 0.75) * 100.0);
        w.enumerated("GlwT", "BETE", word(e, "technique", "softer") == "precise" ? "PrBL" : "SfBL");
        w.unit("Ckmt", "#Pxl", num(e, inner ? "choke" : "spread", 0) * 100.0);
        w.unit("blur", "#Pxl", px("size_mm"));
        w.unit("Nose", "#Prc", num(e, "noise", 0) * 100.0);
        w.unit("ShdN", "#Prc", num(e, "jitter", 0) * 100.0);
        w.boolean("AntA", flag(e, "anti_alias", false));
        w.object("TrnS", contour_of(core::get(e, "contour")), "ShpC");
        w.unit("Inpr", "#Prc", num(e, "range", 0.5) * 100.0);
        if (inner) w.enumerated("glwS", "IGSr", word(e, "source", "edge") == "center" ? "SrcC" : "SrcE");
        return {inner ? "IrGl" : "OrGl", std::move(w)};
    }
    if (kind == "bevel") {
        const std::string style = word(e, "style", "inner"), technique = word(e, "technique", "smooth");
        const Json high = e.value("highlight", Json::object()), low = e.value("shadow", Json::object());
        w.boolean("enab", flag(e, "enabled", true));
        w.boolean("present", true);
        w.boolean("showInDialog", true);
        put_blend(w, "hglM", word(high, "blend", "screen"));
        w.object("hglC", colour_of(high.value("rgb", Json::array({255, 255, 255}))), "RGBC");
        w.unit("hglO", "#Prc", num(high, "opacity", 0.75) * 100.0);
        put_blend(w, "sdwM", word(low, "blend", "multiply"));
        w.object("sdwC", colour_of(low.value("rgb", Json::array({0, 0, 0}))), "RGBC");
        w.unit("sdwO", "#Prc", num(low, "opacity", 0.75) * 100.0);
        w.enumerated("bvlT", "bvlT", technique == "chisel_hard" ? "PrBL" : technique == "chisel_soft" ? "Slmt" : "SfBL");
        w.enumerated("bvlS", "BESl", style == "outer" ? "OtrB" : style == "emboss" ? "Embs" : style == "pillow" ? "PlEb" : style == "stroke" ? "strokeEmboss" : "InrB");
        w.boolean("uglg", flag(e, "global_light", true));
        w.unit("lagl", "#Ang", num(e, "angle", 120));
        w.unit("Lald", "#Ang", num(e, "altitude", 30));
        w.unit("srgR", "#Prc", num(e, "depth", 1) * 100.0);
        w.unit("blur", "#Pxl", px("size_mm"));
        w.enumerated("bvlD", "BESs", word(e, "direction", "up") == "down" ? "Out " : "In  ");
        w.object("TrnS", contour_of(core::get(e, "gloss_contour")), "ShpC");
        w.unit("Sftn", "#Pxl", px("soften_mm"));
        if (const Json* c = core::get(e, "contour"); c != nullptr && c->is_object()) {
            w.boolean("useShape", true);
            w.object("MpgS", contour_of(core::get(*c, "curve")), "ShpC");
            w.boolean("AntA", flag(*c, "anti_alias", false));
            w.unit("Inpr", "#Prc", num(*c, "range", 0.5) * 100.0);
        } else {
            w.boolean("useShape", false);
        }
        if (const Json* t = core::get(e, "texture"); t != nullptr && t->is_object()) {
            w.boolean("useTexture", true);
            w.boolean("InvT", flag(*t, "invert", false));
            w.unit("textureDepth", "#Prc", num(*t, "depth", 1) * 100.0);
            w.append(psd_shapes::pattern_items((*t)["pattern"], canvas));
        } else {
            w.boolean("useTexture", false);
        }
        return {"ebbl", std::move(w)};
    }
    if (kind == "satin") {
        put_common(w, e, "multiply");
        w.object("Clr ", colour_of(e["rgb"]), "RGBC");
        w.unit("Opct", "#Prc", num(e, "opacity", 0.5) * 100.0);
        w.boolean("AntA", flag(e, "anti_alias", true));
        w.boolean("Invr", flag(e, "invert", true));
        w.unit("lagl", "#Ang", num(e, "angle", 19));
        w.unit("Dstn", "#Pxl", px("distance_mm"));
        w.unit("blur", "#Pxl", px("size_mm"));
        w.object("MpgS", contour_of(core::get(e, "contour")), "ShpC");
        return {"ChFX", std::move(w)};
    }
    if (kind == "color_overlay") {
        put_common(w, e, "normal");
        w.unit("Opct", "#Prc", num(e, "opacity", 1) * 100.0);
        w.object("Clr ", colour_of(e["rgb"]), "RGBC");
        return {"SoFi", std::move(w)};
    }
    if (kind == "gradient_overlay") {
        put_common(w, e, "normal");
        w.unit("Opct", "#Prc", num(e, "opacity", 1) * 100.0);
        put_gradient(w, e["gradient"]);
        w.boolean("Dthr", flag(e, "dither", false));
        return {"GrFl", std::move(w)};
    }
    if (kind == "pattern_overlay") {
        put_common(w, e, "normal");
        w.unit("Opct", "#Prc", num(e, "opacity", 1) * 100.0);
        w.append(psd_shapes::pattern_items(e["pattern"], canvas));
        return {"patternFill", std::move(w)};
    }
    // stroke
    put_common(w, e, "normal");
    w.unit("Opct", "#Prc", num(e, "opacity", 1) * 100.0);
    const std::string position = word(e, "position", "outside");
    w.enumerated("Styl", "FStl", position == "inside" ? "InsF" : position == "center" ? "CtrF" : "OutF");
    w.unit("Sz  ", "#Pxl", px("size_mm"));
    if (e.contains("gradient")) {
        w.enumerated("PntT", "FrFl", "GrFl");
        put_gradient(w, e["gradient"]);
    } else if (e.contains("pattern")) {
        w.enumerated("PntT", "FrFl", "Ptrn");
        w.append(psd_shapes::pattern_items(e["pattern"], canvas));
    } else {
        w.enumerated("PntT", "FrFl", "SClr");
        w.object("Clr ", colour_of(e["rgb"]), "RGBC");
    }
    return {"FrFX", std::move(w)};
}

// --- reading ---------------------------------------------------------------------------------------------------------

std::optional<double> number_of(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    if (v == nullptr) return std::nullopt;
    const std::optional<double> n = v->numeric();
    return n && std::isfinite(*n) ? n : std::nullopt;
}
double number_or(const d::Object& o, std::string_view key, double fallback) { return number_of(o, key).value_or(fallback); }
std::optional<bool> bool_of(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    return v != nullptr && v->type == "bool" ? std::optional<bool>(v->boolean) : std::nullopt;
}
std::string enum_of(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    return v != nullptr ? v->enum_value : std::string();
}
const d::Object* object_of(const d::Object& o, std::string_view key) {
    const d::Value* v = o.get(key);
    return v != nullptr && v->object ? v->object.get() : nullptr;
}

int byte_of(double v) { return static_cast<int>(std::clamp<long>(std::lround(v), 0, 255)); }

// A colour of any of the descriptor's classes as the sRGB [r, g, b] it means.
Json colour_from(const d::Object* c) {
    if (c == nullptr) return Json::array({0, 0, 0});
    const auto n = [&](std::string_view key) { return number_or(*c, key, 0.0); };
    if (c->class_id == "CMYC") {
        const double cy = n("Cyn ") / 100.0, m = n("Mgnt") / 100.0, y = n("Ylw ") / 100.0, kk = n("Blck") / 100.0;
        return Json::array({byte_of(255.0 * (1 - cy) * (1 - kk)), byte_of(255.0 * (1 - m) * (1 - kk)), byte_of(255.0 * (1 - y) * (1 - kk))});
    }
    if (c->class_id == "Grsc") {
        const int v = byte_of(255.0 * (1.0 - n("Gry ") / 100.0));
        return Json::array({v, v, v});
    }
    if (c->class_id == "LbCl") {  // (D50 Lab → sRGB)
        const double L = n("Lmnc"), A = n("A   "), B = n("B   ");
        const double fy = (L + 16) / 116.0, fx = fy + A / 500.0, fz = fy - B / 200.0;
        const auto f = [](double t) { return t > 6.0 / 29.0 ? t * t * t : 3 * (6.0 / 29.0) * (6.0 / 29.0) * (t - 4.0 / 29.0); };
        const double X = 0.9642 * f(fx), Y = f(fy), Z = 0.8249 * f(fz);
        const double r = 3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z, g = -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z,
                     b = 0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z;
        const auto enc = [](double v) {
            v = std::clamp(v, 0.0, 1.0);
            return byte_of(255.0 * (v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055));
        };
        return Json::array({enc(r), enc(g), enc(b)});
    }
    if (c->class_id == "HSBC") {
        const double h = std::fmod(std::fmod(n("H   "), 360.0) + 360.0, 360.0) / 60.0, s = n("Strt") / 100.0, v = n("Brgh") / 100.0;
        const double cc = v * s, x = cc * (1 - std::fabs(std::fmod(h, 2.0) - 1)), m = v - cc;
        double r = 0, g = 0, b = 0;
        if (h < 1) r = cc, g = x; else if (h < 2) r = x, g = cc; else if (h < 3) g = cc, b = x;
        else if (h < 4) g = x, b = cc; else if (h < 5) r = x, b = cc; else r = cc, b = x;
        return Json::array({byte_of((r + m) * 255), byte_of((g + m) * 255), byte_of((b + m) * 255)});
    }
    double r = n("Rd  "), g = n("Grn "), b = n("Bl  ");
    if (c->get("redFloat") != nullptr) {  // (newer files: 0..1 floats)
        r = n("redFloat") * 255.0;
        g = n("greenFloat") * 255.0;
        b = n("blueFloat") * 255.0;
    }
    return Json::array({byte_of(r), byte_of(g), byte_of(b)});
}

// A contour (ShpC) as the style keeps one; null for the linear one.
Json contour_from(const d::Object* c) {
    if (c == nullptr) return nullptr;
    const d::Value* curve = c->get("Crv ");
    if (curve == nullptr || curve->list.size() < 2) return nullptr;
    Json points = Json::array();
    for (const d::Value& p : curve->list) {
        if (!p.object || points.size() >= 32) continue;
        points.push_back(Json::array({std::clamp(number_or(*p.object, "Hrzn", 0) / 255.0, 0.0, 1.0), std::clamp(number_or(*p.object, "Vrtc", 0) / 255.0, 0.0, 1.0)}));
    }
    std::string name;
    if (const d::Value* nm = c->get("Nm  ")) name = psd::utf8_of(nm->text);
    if (name.size() > 255) name.resize(255);
    return Json{{"name", name}, {"points", std::move(points)}};
}

class Reader {
public:
    Reader(const psd_shapes::Placement& at, std::vector<std::string>& notes, double scale) : at_(at), notes_(notes), scale_(scale) {}

    std::string blend(const d::Object& o, std::string_view key, const char* fallback) {
        const std::string id = enum_of(o, key);
        if (id.empty()) return fallback;
        if (auto m = blend_of_id(id)) return *m;
        const auto missing = missing_modes().find(id);
        note("効果の描画モード「" + (missing != missing_modes().end() ? missing->second : id) + "」は Genko に無いため「通常」にしました");
        return "normal";
    }
    double mm(const d::Object& o, std::string_view key, double fallback_px) {
        return core::py_round(std::clamp(number_or(o, key, fallback_px) * scale_ * at_.mm_x, 0.0, core::kEffectMaxMm), 4);
    }
    static double pct(const d::Object& o, std::string_view key, double fallback, double lo = 0, double hi = 1) {
        return core::py_round(std::clamp(number_or(o, key, fallback * 100.0) / 100.0, lo, hi), 4);
    }
    static double angle(const d::Object& o, std::string_view key, double fallback) { return std::clamp(number_or(o, key, fallback), -360.0, 360.0); }

    Json gradient(const d::Object& o) {
        Json g = Json::object();
        Json stops = Json::array(), transparency = Json::array();
        if (const d::Object* grad = object_of(o, "Grad")) {
            if (const d::Value* clrs = grad->get("Clrs")) {
                for (const d::Value& s : clrs->list) {
                    if (!s.object || stops.size() >= 16) continue;
                    const std::string type = enum_of(*s.object, "Type");
                    Json colour = colour_from(object_of(*s.object, "Clr "));
                    if (type == "FrgC") colour = Json::array({0, 0, 0});        // (the 描画色 of Photoshop's: black, said)
                    if (type == "BckC") colour = Json::array({255, 255, 255});  // (the 背景色: white)
                    if (type == "FrgC" || type == "BckC") note("効果のグラデーションの描画色・背景色の分岐点は黒・白にしました");
                    stops.push_back(Json::array({std::clamp(number_or(*s.object, "Lctn", 0) / 4096.0, 0.0, 1.0), std::move(colour)}));
                }
            }
            if (const d::Value* trns = grad->get("Trns")) {
                for (const d::Value& s : trns->list) {
                    if (!s.object || transparency.size() >= 16) continue;
                    transparency.push_back(Json::array({std::clamp(number_or(*s.object, "Lctn", 0) / 4096.0, 0.0, 1.0),
                                                        std::clamp(number_or(*s.object, "Opct", 100) / 100.0, 0.0, 1.0)}));
                }
            }
            if (grad->get("ClNs") != nullptr || enum_of(*grad, "GrdF") == "ClNs") note("ノイズのグラデーションは近い形にしました");
        }
        if (stops.size() < 2) stops = Json::array({Json::array({0, Json::array({0, 0, 0})}), Json::array({1, Json::array({255, 255, 255})})});
        g["stops"] = std::move(stops);
        bool opaque = true;
        for (const Json& t : transparency) opaque = opaque && t[1].get<double>() >= 1.0;
        if (transparency.size() >= 2 && !opaque) g["transparency"] = std::move(transparency);
        const std::string type = enum_of(o, "Type");
        g["style"] = type == "Rdl " ? "radial" : type == "Angl" ? "angle" : type == "Rflc" ? "reflected" : type == "Dmnd" ? "diamond" : "linear";
        g["angle"] = angle(o, "Angl", 90);
        g["scale"] = pct(o, "Scl ", 1, 0.1, 1.5);
        g["reverse"] = bool_of(o, "Rvrs").value_or(false);
        g["align"] = bool_of(o, "Algn").value_or(true);
        Json offset = Json::array({0, 0});
        if (const d::Object* ofst = object_of(o, "Ofst")) offset = Json::array({pct(*ofst, "Hrzn", 0, -1, 1), pct(*ofst, "Vrtc", 0, -1, 1)});
        g["offset"] = std::move(offset);
        return g;
    }

    std::optional<Json> pattern(const d::Object& o, const char* what) {
        std::optional<Json> spec = psd_shapes::pattern_of_items(o, at_);
        if (!spec) note(std::string(what) + "のパターンがファイルに無いため外しました");
        return spec;
    }

    std::optional<Json> one(std::string_view key, const d::Object& o) {
        Json e = Json::object();
        const bool enabled = bool_of(o, "enab").value_or(true);
        if (key == "DrSh" || key == "IrSh") {
            const bool inner = key == "IrSh";
            e = Json{{"kind", inner ? "inner_shadow" : "drop_shadow"},
                     {"enabled", enabled},
                     {"blend", blend(o, "Md  ", "multiply")},
                     {"opacity", pct(o, "Opct", 0.75)},
                     {"rgb", colour_from(object_of(o, "Clr "))},
                     {"angle", angle(o, "lagl", 120)},
                     {"global_light", bool_of(o, "uglg").value_or(true)},
                     {"distance_mm", mm(o, "Dstn", 5)}};
            e[inner ? "choke" : "spread"] = pct(o, "Ckmt", 0);
            e["size_mm"] = mm(o, "blur", 5);
            e["noise"] = pct(o, "Nose", 0);
            e["anti_alias"] = bool_of(o, "AntA").value_or(false);
            if (Json c = contour_from(object_of(o, "TrnS")); !c.is_null()) e["contour"] = std::move(c);
            if (!inner) e["knocks_out"] = bool_of(o, "layerConceals").value_or(true);
        } else if (key == "OrGl" || key == "IrGl") {
            const bool inner = key == "IrGl";
            e = Json{{"kind", inner ? "inner_glow" : "outer_glow"}, {"enabled", enabled}, {"blend", blend(o, "Md  ", "screen")}, {"opacity", pct(o, "Opct", 0.75)}};
            if (o.get("Grad") != nullptr) e["gradient"] = gradient(o);
            else e["rgb"] = colour_from(object_of(o, "Clr "));
            e["technique"] = enum_of(o, "GlwT") == "PrBL" ? "precise" : "softer";
            if (inner) e["source"] = enum_of(o, "glwS") == "SrcC" ? "center" : "edge";
            e[inner ? "choke" : "spread"] = pct(o, "Ckmt", 0);
            e["size_mm"] = mm(o, "blur", 5);
            e["range"] = pct(o, "Inpr", 0.5, 0.01, 1);
            e["jitter"] = pct(o, "ShdN", 0);
            e["noise"] = pct(o, "Nose", 0);
            e["anti_alias"] = bool_of(o, "AntA").value_or(false);
            if (Json c = contour_from(object_of(o, "TrnS")); !c.is_null()) e["contour"] = std::move(c);
        } else if (key == "ebbl") {
            const std::string style = enum_of(o, "bvlS"), technique = enum_of(o, "bvlT");
            e = Json{{"kind", "bevel"},
                     {"enabled", enabled},
                     {"style", style == "OtrB" ? "outer" : style == "Embs" ? "emboss" : style == "PlEb" ? "pillow" : style == "strokeEmboss" ? "stroke" : "inner"},
                     {"technique", technique == "PrBL" ? "chisel_hard" : technique == "Slmt" ? "chisel_soft" : "smooth"},
                     {"depth", pct(o, "srgR", 1, 0.01, 10)},
                     {"direction", enum_of(o, "bvlD") == "Out " ? "down" : "up"},
                     {"size_mm", mm(o, "blur", 5)},
                     {"soften_mm", mm(o, "Sftn", 0)},
                     {"angle", angle(o, "lagl", 120)},
                     {"altitude", std::clamp(number_or(o, "Lald", 30), 0.0, 90.0)},
                     {"global_light", bool_of(o, "uglg").value_or(true)},
                     {"highlight", Json{{"blend", blend(o, "hglM", "screen")}, {"opacity", pct(o, "hglO", 0.75)},
                                        {"rgb", o.get("hglC") != nullptr ? colour_from(object_of(o, "hglC")) : Json::array({255, 255, 255})}}},
                     {"shadow", Json{{"blend", blend(o, "sdwM", "multiply")}, {"opacity", pct(o, "sdwO", 0.75)}, {"rgb", colour_from(object_of(o, "sdwC"))}}}};
            if (Json c = contour_from(object_of(o, "TrnS")); !c.is_null()) e["gloss_contour"] = std::move(c);
            if (bool_of(o, "useShape").value_or(false)) {
                Json c = Json{{"range", pct(o, "Inpr", 0.5, 0.01, 1)}, {"anti_alias", bool_of(o, "AntA").value_or(false)}};
                if (Json curve = contour_from(object_of(o, "MpgS")); !curve.is_null()) c["curve"] = std::move(curve);
                e["contour"] = std::move(c);
            }
            if (bool_of(o, "useTexture").value_or(false)) {
                if (auto spec = pattern(o, "ベベルとエンボスのテクスチャ")) {
                    e["texture"] = Json{{"pattern", std::move(*spec)}, {"depth", pct(o, "textureDepth", 1, -10, 10)}, {"invert", bool_of(o, "InvT").value_or(false)}};
                }
            }
        } else if (key == "ChFX") {
            e = Json{{"kind", "satin"},
                     {"enabled", enabled},
                     {"blend", blend(o, "Md  ", "multiply")},
                     {"opacity", pct(o, "Opct", 0.5)},
                     {"rgb", colour_from(object_of(o, "Clr "))},
                     {"angle", angle(o, "lagl", 19)},
                     {"distance_mm", mm(o, "Dstn", 11)},
                     {"size_mm", mm(o, "blur", 14)},
                     {"invert", bool_of(o, "Invr").value_or(false)},
                     {"anti_alias", bool_of(o, "AntA").value_or(true)}};
            if (Json c = contour_from(object_of(o, "MpgS")); !c.is_null()) e["contour"] = std::move(c);
        } else if (key == "SoFi") {
            e = Json{{"kind", "color_overlay"}, {"enabled", enabled}, {"blend", blend(o, "Md  ", "normal")}, {"opacity", pct(o, "Opct", 1)}, {"rgb", colour_from(object_of(o, "Clr "))}};
        } else if (key == "GrFl") {
            e = Json{{"kind", "gradient_overlay"}, {"enabled", enabled}, {"blend", blend(o, "Md  ", "normal")}, {"opacity", pct(o, "Opct", 1)},
                     {"gradient", gradient(o)}, {"dither", bool_of(o, "Dthr").value_or(false)}};
        } else if (key == "patternFill") {
            const std::optional<Json> spec = pattern(o, "パターンオーバーレイ");
            if (!spec) return std::nullopt;
            e = Json{{"kind", "pattern_overlay"}, {"enabled", enabled}, {"blend", blend(o, "Md  ", "normal")}, {"opacity", pct(o, "Opct", 1)}, {"pattern", *spec}};
        } else if (key == "FrFX") {
            const std::string position = enum_of(o, "Styl");
            e = Json{{"kind", "stroke"},
                     {"enabled", enabled},
                     {"blend", blend(o, "Md  ", "normal")},
                     {"opacity", pct(o, "Opct", 1)},
                     {"size_mm", mm(o, "Sz  ", 3)},
                     {"position", position == "InsF" ? "inside" : position == "CtrF" ? "center" : "outside"}};
            const std::string paint = enum_of(o, "PntT");
            if (paint == "GrFl") {
                e["gradient"] = gradient(o);
            } else if (paint == "Ptrn") {
                const std::optional<Json> spec = pattern(o, "境界線");
                if (!spec) return std::nullopt;
                e["pattern"] = *spec;
            } else {
                e["rgb"] = colour_from(object_of(o, "Clr "));
            }
        } else {
            return std::nullopt;
        }
        try {
            return core::layer_effect_spec(e);
        } catch (const core::Error& error) {
            note(std::string("読めない効果を外しました（") + error.what() + "）");
            return std::nullopt;
        }
    }

private:
    void note(const std::string& text) {
        if (std::find(notes_.begin(), notes_.end(), text) == notes_.end() && notes_.size() < 64) notes_.push_back(text);
    }
    const psd_shapes::Placement& at_;
    std::vector<std::string>& notes_;
    double scale_;
};

}  // namespace

std::string blend_id(std::string_view mode) {
    for (const auto& [name, id] : kBlends)
        if (name == mode) return std::string(id);
    return "Nrml";
}

std::optional<std::string> blend_of_id(std::string_view id) {
    for (const auto& [name, key] : kBlends)
        if (key == id) return std::string(name);
    if (id == "Lddg") return std::string("add");  // (an older id of 覆い焼き（リニア）)
    if (id == "lbrn") return std::string("linear_burn");
    return std::nullopt;
}

d::Writer effects_descriptor(const Json& style, const psd_shapes::Canvas& canvas) {
    d::Writer out;
    out.unit("Scl ", "#Prc", 100.0);
    out.boolean("masterFXSwitch", style.value("enabled", true));
    const Json effects = style.value("effects", Json::array());
    for (const KindKeys& k : kKinds) {
        std::vector<std::pair<std::string, d::Writer>> of_kind;
        for (const Json& e : effects) {
            if (word(e, "kind", "") != k.kind) continue;
            auto [key, w] = write_one(e, canvas);
            of_kind.emplace_back(std::string(key), std::move(w));
        }
        if (of_kind.empty()) continue;
        if (of_kind.size() > 1 && !k.multi.empty()) {
            out.objects(k.multi, of_kind);
        } else {
            out.object(k.key, of_kind.front().second, k.key);  // (a kind without a list keeps its first)
        }
    }
    return out;
}

std::string lfx2(const Json& style, const psd_shapes::Canvas& canvas) {
    if (!style.is_object() || style.value("effects", Json::array()).empty()) return {};
    std::string out;
    d::put_u32(out, 0);
    d::put_u32(out, 16);
    return out + effects_descriptor(style, canvas).structure("null");
}

Json effects_of(const d::Object& o, const psd_shapes::Placement& placement, std::vector<std::string>& notes) {
    const double scale = std::clamp(number_or(o, "Scl ", 100.0) / 100.0, 0.01, 10.0);  // (効果の比率)
    Reader reader(placement, notes, scale);
    std::vector<std::pair<std::string_view, Json>> found;  // (Genko's kind, the effect)
    for (const KindKeys& k : kKinds) {
        const d::Value* many = k.multi.empty() ? nullptr : o.get(k.multi);
        if (many != nullptr && many->type == "VlLs") {
            for (const d::Value& item : many->list) {
                if (!item.object) continue;
                if (auto e = reader.one(k.key, *item.object)) found.emplace_back(k.kind, std::move(*e));
            }
        } else if (const d::Object* single = object_of(o, k.key)) {
            if (auto e = reader.one(k.key, *single)) found.emplace_back(k.kind, std::move(*e));
        }
    }
    // (in the order of Photoshop's レイヤースタイル window)
    Json effects = Json::array();
    for (const std::string_view kind : core::kEffectKinds)
        for (const auto& [k, e] : found)
            if (k == kind && effects.size() < core::kMaxEffects) effects.push_back(e);
    Json style = Json{{"effects", std::move(effects)}};
    if (const auto master = bool_of(o, "masterFXSwitch"); master && !*master) style["enabled"] = false;
    return style;
}

std::optional<Json> style_of(std::string_view payload, const psd_shapes::Placement& placement, std::vector<std::string>& notes) {
    try {
        d::Bytes b(payload);
        if (b.u32() != 0) throw d::Broken("an effects block of another version");
        if (b.u32() != 16) throw d::Broken("a descriptor version other than 16");
        const d::Object o = d::read_structure(b, 0);
        const Json style = core::layer_style_spec(effects_of(o, placement, notes));
        if (style.is_null()) return std::nullopt;
        return style;
    } catch (const std::exception& error) {
        notes.push_back(std::string("レイヤー効果を読めませんでした（") + error.what() + "）");
        return std::nullopt;
    }
}

std::string iopa(double fill) {
    std::string out(4, '\0');
    out[0] = static_cast<char>(std::clamp<long>(std::lround(fill * 255.0), 0, 255));
    return out;
}

std::optional<double> fill_of_iopa(std::string_view payload) {
    if (payload.empty()) return std::nullopt;
    return core::py_round(static_cast<unsigned char>(payload[0]) / 255.0, 4);
}

std::string ranges_of(const Json& blend_if, const std::vector<std::string>& names) {
    std::string out;
    const auto range = [&](const std::string& channel, const char* side) {
        std::array<int, 4> r{0, 0, 255, 255};
        if (blend_if.is_array()) {
            for (const Json& entry : blend_if) {
                if (entry.value("channel", std::string()) != channel) continue;
                const Json& v = entry[side];
                for (std::size_t k = 0; k < 4; ++k) r[k] = static_cast<int>(std::clamp<std::int64_t>(v[k].get<std::int64_t>(), 0, 255));
            }
        }
        for (const int v : r) out += static_cast<char>(v);
    };
    for (const std::string& channel : [&] {
             std::vector<std::string> all{"gray"};
             all.insert(all.end(), names.begin(), names.end());
             return all;
         }()) {
        range(channel, "this");
        range(channel, "under");
    }
    return out;
}

Json blend_if_of(std::string_view ranges, const std::vector<std::string>& names) {
    Json out = Json::array();
    std::vector<std::string> all{"gray"};
    all.insert(all.end(), names.begin(), names.end());
    for (std::size_t i = 0; i < all.size() && (i + 1) * 8 <= ranges.size(); ++i) {
        const auto r = [&](std::size_t at) {
            Json v = Json::array();
            for (std::size_t k = 0; k < 4; ++k) v.push_back(static_cast<unsigned char>(ranges[at + k]));
            if (v[0] > v[1]) v[1] = v[0];  // (crossed halves read as the low one: Photoshop's sliders cannot cross)
            if (v[2] > v[3]) v[2] = v[3];
            return v;
        };
        Json entry{{"channel", all[i]}, {"this", r(i * 8)}, {"under", r(i * 8 + 4)}};
        const Json full = Json::array({0, 0, 255, 255});
        if (entry["this"] != full || entry["under"] != full) out.push_back(std::move(entry));
    }
    return out;
}

std::string light_resource(double degrees) {
    std::string out;
    d::put_i32(out, static_cast<std::int32_t>(std::lround(degrees)));
    return out;
}

std::optional<double> light_of_resource(std::string_view payload) {
    if (payload.size() < 4) return std::nullopt;
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | static_cast<unsigned char>(payload[k]);
    return static_cast<double>(static_cast<std::int32_t>(v));
}

}  // namespace genko::render::psd_effects
