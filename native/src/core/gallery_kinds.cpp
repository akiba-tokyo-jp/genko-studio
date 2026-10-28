// PC4 節 D3 (SPEC.md COLOR-08): フィルターギャラリー's table (core/gallery_kinds.hpp) — PhotoCraft v0.2.0's
// algo/src/artistic.rs (39-91: the 47 effects, their settings, ranges and defaults; 221-232: NEON_DEFAULT) and
// engine/src/gallery_cmds.rs (32-79: an effect read from its settings, the stack's colours behind its own; 87-100: the
// catalogue), MIT: native/third_party/photocraft/NOTICE.md. Its formulas: render/fx_gallery.cpp.

#include "core/gallery_kinds.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/command_bus.hpp"
#include "core/filter_kinds.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

using Type = AdjustParam::Type;

AdjustParam num(std::string_view key, double lo, double hi, double fallback) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Number;
    p.lo = lo;
    p.hi = hi;
    p.fallback = fallback;
    return p;
}

AdjustParam pick(std::string_view key, std::vector<std::string_view> choices) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Choice;
    p.choices = std::move(choices);
    p.text = p.choices.front();  // (PhotoCraft: the first choice is the default)
    return p;
}

AdjustParam check(std::string_view key) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Bool;
    p.fallback = 0;
    return p;
}

AdjustParam colour(std::string_view key, std::array<int, 3> fallback, bool optional = false) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Rgb;
    p.numbers = {static_cast<double>(fallback[0]), static_cast<double>(fallback[1]), static_cast<double>(fallback[2])};
    p.lo = 0;
    p.hi = 255;
    p.optional = optional;
    return p;
}

// A setting read by the kind's own normalize, required (absent: null, which the normalize refuses).
AdjustParam required_custom(std::string_view key) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Custom;
    p.optional = false;
    return p;
}

// (the textures and lights PhotoCraft's texturizer offers, each list in its own order: its first the default)
std::vector<std::string_view> textures() { return {"canvas", "brick", "burlap", "sandstone"}; }
std::vector<std::string_view> lights_from_bottom() { return {"bottom", "bottomLeft", "left", "topLeft", "top", "topRight", "right", "bottomRight"}; }
std::vector<std::string_view> lights_from_top() { return {"top", "topRight", "right", "bottomRight", "bottom", "bottomLeft", "left", "topLeft"}; }
std::vector<std::string_view> directions() { return {"rightDiagonal", "horizontal", "leftDiagonal", "vertical"}; }

constexpr std::string_view kArtistic = "アーティスティック", kBrush = "ブラシストローク", kDistort = "変形", kSketch = "スケッチ", kStylize = "表現手法",
                           kTexture = "テクスチャ";

std::vector<GalleryEffectKind> make_effects() {
    return {
        // ---- アーティスティック (Artistic) ----
        {"colored_pencil", "coloredPencil", "色鉛筆", kArtistic, {num("pencil_width", 1, 24, 4), num("stroke_pressure", 0, 15, 8), num("paper_brightness", 0, 50, 25)}},
        {"cutout", "cutout", "カットアウト", kArtistic, {num("number_of_levels", 2, 8, 4), num("edge_simplicity", 0, 10, 4), num("edge_fidelity", 1, 3, 2)}},
        {"dry_brush", "dryBrush", "ドライブラシ", kArtistic, {num("brush_size", 0, 10, 2), num("brush_detail", 0, 10, 8), num("texture", 1, 3, 1)}},
        {"film_grain", "filmGrain", "粒状フィルム", kArtistic, {num("grain", 0, 20, 4), num("highlight_area", 0, 20, 0), num("intensity", 0, 10, 10)}},
        {"fresco", "fresco", "フレスコ", kArtistic, {num("brush_size", 0, 10, 2), num("brush_detail", 0, 10, 8), num("texture", 1, 3, 1)}},
        {"neon_glow", "neonGlow", "ネオン光彩", kArtistic, {num("glow_size", -24, 24, 5), num("glow_brightness", 0, 50, 15), colour("glow_color", kNeonGlowColour)}},
        {"paint_daubs", "paintDaubs", "塗料", kArtistic,
         {num("brush_size", 1, 50, 8), num("sharpness", 0, 40, 7), pick("brush_type", {"simple", "lightRough", "darkRough", "wideSharp", "wideBlurry", "sparkle"})}},
        {"palette_knife", "paletteKnife", "パレットナイフ", kArtistic, {num("stroke_size", 1, 50, 25), num("stroke_detail", 1, 3, 3), num("softness", 0, 10, 0)}},
        {"plastic_wrap", "plasticWrap", "ラップ", kArtistic, {num("highlight_strength", 0, 20, 15), num("detail", 1, 15, 9), num("smoothness", 1, 15, 7)}},
        {"poster_edges", "posterEdges", "ポスターエッジ", kArtistic, {num("edge_thickness", 0, 10, 2), num("edge_intensity", 0, 10, 1), num("posterization", 0, 6, 2)}},
        {"rough_pastels", "roughPastels", "粗いパステル画", kArtistic,
         {num("stroke_length", 0, 40, 6), num("stroke_detail", 1, 20, 4), pick("texture", textures()), num("scaling", 50, 200, 100), num("relief", 0, 50, 20),
          pick("light", lights_from_bottom()), check("invert")}},
        {"smudge_stick", "smudgeStick", "こする", kArtistic, {num("stroke_length", 0, 10, 2), num("highlight_area", 0, 20, 0), num("intensity", 0, 10, 10)}},
        {"sponge", "sponge", "スポンジ", kArtistic, {num("brush_size", 0, 10, 2), num("definition", 0, 25, 12), num("smoothness", 1, 15, 5)}},
        {"underpainting", "underpainting", "底塗り効果", kArtistic,
         {num("brush_size", 0, 40, 6), num("texture_coverage", 0, 40, 16), pick("texture", textures()), num("scaling", 50, 200, 100), num("relief", 0, 50, 4),
          pick("light", lights_from_top()), check("invert")}},
        {"watercolor", "watercolor", "水彩画", kArtistic, {num("brush_detail", 1, 14, 9), num("shadow_intensity", 0, 10, 1), num("texture", 1, 3, 1)}},
        // ---- ブラシストローク (Brush Strokes) ----
        {"accented_edges", "accentedEdges", "エッジの強調", kBrush, {num("edge_width", 1, 14, 2), num("edge_brightness", 0, 50, 38), num("smoothness", 1, 15, 5)}},
        {"angled_strokes", "angledStrokes", "ストローク（斜め）", kBrush, {num("direction_balance", 0, 100, 50), num("stroke_length", 3, 50, 15), num("sharpness", 0, 10, 3)}},
        {"crosshatch", "crosshatch", "網目", kBrush, {num("stroke_length", 3, 50, 9), num("sharpness", 0, 20, 6), num("strength", 1, 3, 1)}},
        {"dark_strokes", "darkStrokes", "ストローク（暗）", kBrush, {num("balance", 0, 10, 5), num("black_intensity", 0, 10, 6), num("white_intensity", 0, 10, 2)}},
        {"ink_outlines", "inkOutlines", "インク画（外形）", kBrush, {num("stroke_length", 1, 50, 4), num("dark_intensity", 0, 50, 20), num("light_intensity", 0, 50, 10)}},
        {"spatter", "spatter", "はね", kBrush, {num("spray_radius", 0, 25, 10), num("smoothness", 1, 15, 5)}},
        {"sprayed_strokes", "sprayedStrokes", "ストローク（スプレー）", kBrush,
         {num("stroke_length", 0, 20, 12), num("spray_radius", 0, 25, 7), pick("stroke_direction", directions())}},
        {"sumi_e", "sumiE", "墨絵", kBrush, {num("stroke_width", 3, 15, 10), num("stroke_pressure", 0, 15, 2), num("contrast", 0, 40, 16)}},
        // ---- 変形 (Distort) ----
        {"diffuse_glow", "diffuseGlow", "光彩拡散", kDistort, {num("graininess", 0, 10, 6), num("glow_amount", 0, 20, 10), num("clear_amount", 0, 20, 15)}},
        {"glass", "glass", "ガラス", kDistort,
         {num("distortion", 0, 20, 5), num("smoothness", 1, 15, 3), pick("texture", {"frosted", "blocks", "canvas", "tinyLens"}), num("scaling", 50, 200, 100),
          check("invert")}},
        {"ocean_ripple", "oceanRipple", "海の波紋", kDistort, {num("ripple_size", 1, 15, 9), num("ripple_magnitude", 0, 20, 9)}},
        // ---- スケッチ (Sketch: the foreground colour the ink, the background the paper) ----
        {"bas_relief", "basRelief", "浅浮彫り", kSketch, {num("detail", 1, 15, 13), num("smoothness", 1, 15, 3), pick("light", lights_from_bottom())}},
        {"chalk_charcoal", "chalkCharcoal", "チョーク・木炭画", kSketch, {num("charcoal_area", 0, 20, 6), num("chalk_area", 0, 20, 6), num("stroke_pressure", 0, 5, 1)}},
        {"charcoal", "charcoal", "木炭画", kSketch, {num("charcoal_thickness", 1, 7, 1), num("detail", 0, 5, 5), num("light_dark_balance", 0, 100, 50)}},
        {"chrome", "chrome", "クロム", kSketch, {num("detail", 0, 10, 4), num("smoothness", 0, 10, 7)}},
        {"conte_crayon", "conteCrayon", "コンテ画", kSketch,
         {num("foreground_level", 1, 15, 11), num("background_level", 1, 15, 7), pick("texture", textures()), num("scaling", 50, 200, 100), num("relief", 0, 50, 4),
          pick("light", lights_from_top()), check("invert")}},
        {"graphic_pen", "graphicPen", "グラフィックペン", kSketch,
         {num("stroke_length", 1, 15, 15), num("light_dark_balance", 0, 100, 50), pick("stroke_direction", directions())}},
        {"halftone_pattern", "halftonePattern", "ハーフトーンパターン", kSketch, {num("size", 1, 12, 1), num("contrast", 0, 50, 5), pick("pattern_type", {"dot", "circle", "line"})}},
        {"note_paper", "notePaper", "ノート用紙", kSketch, {num("image_balance", 0, 50, 25), num("graininess", 0, 20, 10), num("relief", 0, 25, 11)}},
        {"photocopy", "photocopy", "コピー", kSketch, {num("detail", 1, 24, 7), num("darkness", 1, 50, 8)}},
        {"plaster", "plaster", "漆喰", kSketch, {num("image_balance", 0, 50, 20), num("smoothness", 1, 15, 2), pick("light", lights_from_top())}},
        {"reticulation", "reticulation", "細網", kSketch, {num("density", 0, 50, 12), num("foreground_level", 0, 50, 40), num("background_level", 0, 50, 5)}},
        {"stamp", "stamp", "スタンプ", kSketch, {num("light_dark_balance", 0, 50, 25), num("smoothness", 1, 50, 5)}},
        {"torn_edges", "tornEdges", "ぎざぎざのエッジ", kSketch, {num("image_balance", 0, 50, 25), num("smoothness", 1, 15, 11), num("contrast", 1, 25, 17)}},
        {"water_paper", "waterPaper", "ウォーターペーパー", kSketch, {num("fiber_length", 3, 50, 15), num("brightness", 0, 100, 60), num("contrast", 0, 100, 80)}},
        // ---- 表現手法 (Stylize) ----
        {"glowing_edges", "glowingEdges", "エッジの光彩", kStylize, {num("edge_width", 1, 14, 2), num("edge_brightness", 0, 20, 6), num("smoothness", 1, 15, 5)}},
        // ---- テクスチャ (Texture) ----
        {"craquelure", "craquelure", "クラッキング", kTexture, {num("crack_spacing", 2, 100, 15), num("crack_depth", 0, 10, 6), num("crack_brightness", 0, 10, 9)}},
        {"grain", "grain", "粒状", kTexture,
         {num("intensity", 0, 100, 40), num("contrast", 0, 100, 50),
          pick("grain_type", {"regular", "soft", "sprinkles", "clumped", "contrasty", "enlarged", "stippled", "horizontal", "vertical", "speckle"})}},
        {"mosaic_tiles", "mosaicTiles", "モザイクタイル", kTexture, {num("tile_size", 2, 100, 12), num("grout_width", 1, 15, 3), num("lighten_grout", 0, 10, 9)}},
        {"patchwork", "patchwork", "パッチワーク", kTexture, {num("square_size", 0, 10, 4), num("relief", 0, 25, 8)}},
        {"stained_glass", "stainedGlass", "ステンドグラス", kTexture, {num("cell_size", 2, 50, 10), num("border_thickness", 1, 20, 4), num("light_intensity", 0, 10, 3)}},
        {"texturizer", "texturizer", "テクスチャライザー", kTexture,
         {pick("texture", textures()), num("scaling", 50, 200, 100), num("relief", 0, 50, 4), pick("light", lights_from_top()), check("invert")}},
    };
}

Json number_json(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) return Json(static_cast<std::int64_t>(v));
    return Json(v);
}

Json setting_json(const AdjustParam& p) {
    Json out{{"key", std::string(p.key)}};
    switch (p.type) {
        case Type::Choice: {
            Json choices = Json::array();
            for (const std::string_view c : p.choices) choices.push_back(std::string(c));
            out["type"] = "choice";
            out["choices"] = std::move(choices);
            out["default"] = std::string(p.text);
            break;
        }
        case Type::Bool:
            out["type"] = "bool";
            out["default"] = p.fallback != 0;
            break;
        case Type::Rgb: {
            Json fallback = Json::array();
            for (const double v : p.numbers) fallback.push_back(number_json(v));
            out["type"] = "rgb";
            out["default"] = std::move(fallback);
            break;
        }
        default:
            out["type"] = "number";
            out["default"] = number_json(p.fallback);
            out["min"] = number_json(p.lo);
            out["max"] = number_json(p.hi);
            break;
    }
    return out;
}

constexpr std::array<std::string_view, 5> kEffectKeys{"filter", "params", "visible", "foreground", "background"};

}  // namespace

const std::vector<GalleryEffectKind>& gallery_effects() {
    static const std::vector<GalleryEffectKind> all = make_effects();
    return all;
}

const GalleryEffectKind* gallery_effect(std::string_view filter) {
    for (const GalleryEffectKind& each : gallery_effects()) {
        if (each.filter == filter) return &each;
    }
    return nullptr;
}

const std::vector<std::string_view>& gallery_categories() {
    static const std::vector<std::string_view> all{kArtistic, kBrush, kDistort, kSketch, kStylize, kTexture};
    return all;
}

Json read_gallery(Json read) {
    static const std::vector<AdjustParam> colours{colour("foreground", {0, 0, 0}, true), colour("background", {255, 255, 255}, true)};
    const std::string where = "adjust filter_gallery.effects";
    const std::string shape = " must be a list of 1 to " + std::to_string(kGalleryMostEffects) + " effects {filter, params, visible}";
    const auto given = read.find("effects");
    if (given == read.end() || given->is_null()) throw OpError(where + " is required: a list of effects {filter, params, visible} (genko inspect --filters)");
    if (!given->is_array() || given->empty() || given->size() > kGalleryMostEffects) throw OpError(where + shape);
    Json effects = Json::array();
    for (std::size_t i = 0; i < given->size(); ++i) {
        const Json& item = (*given)[i];
        const std::string here = where + "[" + std::to_string(i) + "]";
        if (!item.is_object()) throw OpError(here + " must be an effect {filter, params, visible}");
        for (const auto& [key, value] : item.items()) {
            if (std::find(kEffectKeys.begin(), kEffectKeys.end(), key) == kEffectKeys.end()) throw OpError(here + " has no setting " + py_repr_str(key));
        }
        const Json* filter = get(item, "filter");
        if (filter == nullptr || !filter->is_string()) throw OpError(here + ".filter must be the name of one of the gallery's effects (genko inspect --filters)");
        const std::string& name = filter->get_ref<const std::string&>();
        const GalleryEffectKind* kind = gallery_effect(name);
        if (kind == nullptr) throw OpError(here + ".filter: the gallery has no effect " + py_repr_str(name));
        const Json* params = get(item, "params");
        Json out{{"filter", name},
                 {"params", read_params(kind->params, params != nullptr && !params->is_null() ? *params : Json::object(), ReadMode::Op, here)},
                 {"visible", true}};
        if (const Json* visible = get(item, "visible"); visible != nullptr && !visible->is_null()) {
            if (!visible->is_boolean()) throw OpError(here + ".visible must be true or false");
            out["visible"] = *visible;
        }
        Json own = Json::object();
        for (const char* key : {"foreground", "background"}) {
            if (const Json* c = get(item, key); c != nullptr && !c->is_null()) own[key] = *c;
        }
        const Json read_colours = read_params(colours, own, ReadMode::Op, here);
        for (const auto& [key, value] : read_colours.items()) out[key] = value;
        effects.push_back(std::move(out));
    }
    read["effects"] = std::move(effects);
    return read;
}

Json gallery_catalogue() {
    Json out = Json::array();
    for (const GalleryEffectKind& each : gallery_effects()) {
        Json settings = Json::array();
        for (const AdjustParam& p : each.params) settings.push_back(setting_json(p));
        out.push_back(Json{{"filter", std::string(each.filter)},
                           {"upstream", std::string(each.upstream)},
                           {"command", "filter.gallery." + std::string(each.upstream)},
                           {"name", std::string(each.name)},
                           {"category", std::string(each.category)},
                           {"settings", std::move(settings)}});
    }
    return out;
}

// フィルター → フィルターギャラリー… (the kind of D3; its effects' formulas render/fx_gallery.cpp, its window
// app/filter_gallery_window.cpp)
std::vector<FilterKind> gallery_kinds() {
    FilterKind out;
    out.kind.name = "filter_gallery";
    out.kind.group = "PC4";
    out.kind.layer = false;
    out.kind.filter = true;
    out.kind.local = true;
    out.kind.smart = true;
    out.kind.params = {required_custom("effects"), colour("foreground", {0, 0, 0}), colour("background", {255, 255, 255})};
    out.kind.hooks.normalize = [](Json read, const Page*, LookupTables*) { return read_gallery(std::move(read)); };
    out.info.kind = "filter_gallery";
    out.info.upstream = "filter.filterGallery";
    out.info.name = "フィルターギャラリー";
    out.info.submenu = {};  // (in the フィルター menu itself)
    out.info.colours = {"foreground", "background"};
    out.info.inks = FilterInks::Meaning;  // (each effect reads and lays colours: on a page of inks, the colours they mean)
    return {std::move(out)};
}

}  // namespace genko::core
