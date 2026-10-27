#include "render/style_presets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>

#include "core/error.hpp"
#include "core/layer_styles.hpp"
#include "core/ops_util.hpp"
#include "core/patterns.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/asl.hpp"
#include "render/layer_styles.hpp"
#include "render/pat.hpp"
#include "render/pattern_library.hpp"

// PhotoCraft v0.2.0 crates/engine/src/presets/styles.rs (lines 18-60: the preset — its effects and the blending options
// saved with it —; 62-150: the built-in groups, their effects and settings; 237-290: the swatch, the style on a rounded
// square), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::style_presets {

using core::Json;

namespace {

[[noreturn]] void bad(const std::string& why) { throw core::Error("value", why); }

constexpr std::string_view kLibraryId = "lib-";

// --- the built-in groups -----------------------------------------------------------------------------------------------

// upstream's sizes: pixels, here of 150 dpi
Json mm(double px) { return core::py_round(px * 25.4 / 150.0, 2); }

Json rgb_of(std::string_view hex) {
    if (!hex.empty() && hex.front() == '#') hex.remove_prefix(1);
    Json out = Json::array();
    for (std::size_t k = 0; k < 3; ++k) out.push_back(std::stoi(std::string(hex.substr(k * 2, 2)), nullptr, 16));
    return out;
}

double number(const Json& p, const char* key, double fallback) {
    const auto it = p.find(key);
    return it != p.end() && it->is_number() ? it->get<double>() : fallback;
}

// An effect as upstream's layer.layerStyle.<kind> takes it (opacity, depth in %, sizes in pixels, colours "#rrggbb", an
// angle given its own: not the global light), in this build's form.
Json fx(std::string_view kind, const Json& p) {
    static const std::pair<std::string_view, std::string_view> kKinds[] = {
        {"dropShadow", "drop_shadow"},       {"innerShadow", "inner_shadow"}, {"outerGlow", "outer_glow"}, {"innerGlow", "inner_glow"},
        {"stroke", "stroke"},                {"colorOverlay", "color_overlay"}, {"gradientOverlay", "gradient_overlay"},
        {"satin", "satin"},                  {"bevelEmboss", "bevel"}};
    std::string own;
    for (const auto& [theirs, ours] : kKinds)
        if (theirs == kind) own = ours;
    Json e{{"kind", own}};
    if (p.contains("opacity")) e["opacity"] = number(p, "opacity", 100) / 100.0;
    if (p.contains("blend")) e["blend"] = p["blend"];
    if (p.contains("color")) e["rgb"] = rgb_of(p["color"].get<std::string>());
    if (own == "drop_shadow" || own == "inner_shadow") {
        e["distance_mm"] = mm(number(p, "distance", 5));
        e["size_mm"] = mm(number(p, "size", 5));
        if (p.contains("angle")) {
            e["angle"] = p["angle"];
            e["global_light"] = false;
        }
    } else if (own == "outer_glow" || own == "inner_glow") {
        e["size_mm"] = mm(number(p, "size", 5));
    } else if (own == "stroke") {
        e["size_mm"] = mm(number(p, "size", 3));
        if (p.contains("position")) e["position"] = p["position"];
    } else if (own == "satin") {
        e["distance_mm"] = mm(number(p, "distance", 11));
        e["size_mm"] = mm(number(p, "size", 14));
    } else if (own == "bevel") {
        e["style"] = p.value("style", std::string("inner"));
        e["size_mm"] = mm(number(p, "size", 5));
        e["depth"] = number(p, "depth", 100) / 100.0;
        if (p.contains("soften")) e["soften_mm"] = mm(number(p, "soften", 0));
    } else if (own == "gradient_overlay") {
        e["gradient"] = Json{{"stops", Json::array({Json::array({0, rgb_of(p["from"].get<std::string>())}), Json::array({1, rgb_of(p["to"].get<std::string>())})})},
                             {"angle", number(p, "angle", 90)}};
    }
    return core::layer_effect_spec(e);
}

Json preset(const char* name, std::initializer_list<Json> effects, std::optional<double> fill = std::nullopt) {
    Json item{{"name", name}, {"effects", Json::array()}};
    for (const Json& e : effects) item["effects"].push_back(e);
    if (fill) item["fill_opacity"] = *fill;
    return item_of(item, name);
}

std::vector<preset_store::Group> builtin() {
    using J = Json;
    std::vector<preset_store::Group> out;
    out.push_back({"基本",
                   {preset("デフォルトスタイル（なし）", {}),
                    preset("ドロップシャドウ", {fx("dropShadow", J{{"distance", 6}, {"size", 8}, {"opacity", 60}})}),
                    preset("やわらかいシャドウ", {fx("dropShadow", J{{"distance", 2}, {"size", 16}, {"opacity", 45}})}),
                    preset("黒の境界線", {fx("stroke", J{{"size", 3}, {"color", "#000000"}, {"position", "outside"}})}),
                    preset("白の境界線", {fx("stroke", J{{"size", 3}, {"color", "#ffffff"}, {"position", "outside"}})}),
                    preset("シャドウ（内側）", {fx("innerShadow", J{{"distance", 4}, {"size", 6}, {"opacity", 55}})}),
                    preset("光彩（外側）", {fx("outerGlow", J{{"size", 14}, {"color", "#ffe9a8"}, {"opacity", 80}})}),
                    preset("エンボス", {fx("bevelEmboss", J{{"style", "emboss"}, {"size", 4}, {"depth", 120}})})}});
    out.push_back({"テキスト効果",
                   {preset("ネオン（青）", {fx("colorOverlay", J{{"color", "#d8f4ff"}}),
                                           fx("innerGlow", J{{"color", "#3fb6ff"}, {"size", 6}, {"opacity", 90}, {"blend", "normal"}}),
                                           fx("outerGlow", J{{"color", "#1e90ff"}, {"size", 22}, {"opacity", 90}})}),
                    preset("ネオン（ピンク）", {fx("colorOverlay", J{{"color", "#ffe1f3"}}),
                                               fx("innerGlow", J{{"color", "#ff4fb4"}, {"size", 6}, {"opacity", 90}, {"blend", "normal"}}),
                                               fx("outerGlow", J{{"color", "#ff2d95"}, {"size", 22}, {"opacity", 90}})}),
                    preset("クローム", {fx("gradientOverlay", J{{"from", "#3a3f47"}, {"to", "#f4f6f8"}, {"angle", 90}}),
                                       fx("bevelEmboss", J{{"style", "inner"}, {"size", 6}, {"depth", 250}, {"soften", 1}}),
                                       fx("stroke", J{{"size", 1}, {"color", "#2a2d33"}, {"position", "outside"}})}),
                    preset("ゴールド", {fx("gradientOverlay", J{{"from", "#8a5a12"}, {"to", "#ffe08a"}, {"angle", 90}}),
                                       fx("bevelEmboss", J{{"style", "inner"}, {"size", 5}, {"depth", 180}}),
                                       fx("dropShadow", J{{"distance", 3}, {"size", 5}, {"opacity", 50}})}),
                    preset("コミックの縁取り", {fx("colorOverlay", J{{"color", "#ffd23f"}}),
                                               fx("stroke", J{{"size", 4}, {"color", "#111111"}, {"position", "outside"}}),
                                               fx("dropShadow", J{{"distance", 6}, {"size", 0}, {"opacity", 100}, {"angle", 135}})}),
                    preset("活版印刷", {fx("colorOverlay", J{{"color", "#5b6168"}}),
                                       fx("innerShadow", J{{"distance", 2}, {"size", 2}, {"opacity", 70}}),
                                       fx("dropShadow", J{{"distance", 1}, {"size", 0}, {"color", "#ffffff"}, {"opacity", 60}, {"blend", "screen"}, {"angle", 90}})}),
                    preset("中抜き", {fx("stroke", J{{"size", 2}, {"color", "#222222"}, {"position", "inside"}})}, 0.0)}});
    out.push_back({"ボタン",
                   {preset("ガラス", {fx("gradientOverlay", J{{"from", "#2b6cb0"}, {"to", "#90cdf4"}, {"angle", 90}}),
                                     fx("innerGlow", J{{"color", "#ffffff"}, {"size", 6}, {"opacity", 45}, {"blend", "screen"}}),
                                     fx("bevelEmboss", J{{"style", "inner"}, {"size", 8}, {"depth", 100}, {"soften", 4}}),
                                     fx("dropShadow", J{{"distance", 3}, {"size", 8}, {"opacity", 40}})}),
                    preset("ジェル（緑）", {fx("gradientOverlay", J{{"from", "#1f7a3a"}, {"to", "#7be495"}, {"angle", 90}}),
                                           fx("innerShadow", J{{"distance", 2}, {"size", 5}, {"opacity", 35}}),
                                           fx("bevelEmboss", J{{"style", "inner"}, {"size", 10}, {"depth", 150}, {"soften", 6}})}),
                    preset("押し込み", {fx("colorOverlay", J{{"color", "#d7dbe0"}}),
                                       fx("innerShadow", J{{"distance", 3}, {"size", 6}, {"opacity", 60}}),
                                       fx("stroke", J{{"size", 1}, {"color", "#9aa1a9"}, {"position", "inside"}})}),
                    preset("ピル（赤）", {fx("gradientOverlay", J{{"from", "#9b1c1c"}, {"to", "#f87171"}, {"angle", 90}}),
                                         fx("bevelEmboss", J{{"style", "pillow"}, {"size", 6}, {"depth", 120}}),
                                         fx("dropShadow", J{{"distance", 2}, {"size", 4}, {"opacity", 45}})}),
                    preset("フラットなシャドウ", {fx("colorOverlay", J{{"color", "#3b82f6"}}),
                                                 fx("dropShadow", J{{"distance", 5}, {"size", 0}, {"opacity", 35}, {"angle", 90}})}),
                    preset("サテン（プラム）", {fx("colorOverlay", J{{"color", "#7e3a8c"}}),
                                               fx("satin", J{{"color", "#1b0420"}, {"opacity", 55}, {"size", 18}, {"distance", 14}}),
                                               fx("bevelEmboss", J{{"style", "inner"}, {"size", 4}, {"depth", 80}})})}});
    return out;
}

void check(const Json& item) { item_of(item, item.value("name", std::string("スタイル"))); }

// --- the library's patterns ----------------------------------------------------------------------------------------------

const Json* library_item(const std::vector<preset_store::Group>& groups, const Json& ref) {
    if (const auto asset = ref.find("asset"); asset != ref.end() && asset->is_string()) {
        for (const preset_store::Group& g : groups)
            for (const Json& item : g.items)
                if (item.value("asset", std::string()) == asset->get<std::string>()) return &item;
    }
    return preset_store::find(groups, ref.value("library", std::string()));
}

// A library pattern's picture as add_pattern takes it, with the fill's placement (set_layer_style puts it into the book).
std::pair<core::Pattern, core::Bytes> library_pattern(const Json& ref, const std::filesystem::path& dir, const std::vector<preset_store::Group>& groups) {
    const Json* item = library_item(groups, ref);
    const std::string name = ref.value("library", std::string());
    if (item == nullptr) bad("the pattern library has no pattern " + name);
    core::Pattern p = pattern_library::pattern_of(*item);
    p.name = item->value("name", name);
    core::Bytes bytes = pattern_library::picture(dir, p);
    if (!bytes) bad("the picture of the pattern " + p.name + " is not in the library");
    return {std::move(p), std::move(bytes)};
}

// The library's own pattern of this picture (its asset, mode, depth and dpi), else this one added to it → its name.
std::string library_name_of(const core::Pattern& p, const core::Bytes& bytes, const std::filesystem::path& dir,
                            std::vector<preset_store::Group>& groups, const std::optional<std::string>& group, bool& added) {
    for (const preset_store::Group& g : groups)
        for (const Json& item : g.items)
            if (item.value("asset", std::string()) == p.asset && item.value("mode", std::string()) == p.mode && item.value("depth", 0) == p.depth &&
                item.value("dpi", 0.0) == p.dpi)
                return item.value("name", std::string());
    core::Pattern own = p;
    own.id.clear();
    const Json made = pattern_library::add(dir, groups, own, bytes, group ? std::optional<std::string_view>(*group) : std::nullopt);
    added = true;
    return made.value("name", std::string());
}

Json style_part(const Json& item) {
    Json style = Json::object();
    for (const char* key : {"effects", "enabled", "fill_opacity", "blend_if"})
        if (const auto it = item.find(key); it != item.end()) style[key] = *it;
    if (!style.contains("effects")) style["effects"] = Json::array();
    return style;
}

}  // namespace

const preset_store::Kind& kind() {
    static const preset_store::Kind k = [] {
        preset_store::Kind made;
        made.file = "styles.json";
        made.default_group = "基本";
        made.builtin = builtin;
        made.check = check;
        return made;
    }();
    return k;
}

Json item_of(const Json& given, std::string_view name) {
    if (!given.is_object()) bad("a style preset is {\"effects\", \"fill_opacity\"?, \"blend_if\"?, \"blend\"?}");
    static const char* const kKeys[] = {"name", "effects", "enabled", "fill_opacity", "blend_if", "blend"};
    for (const auto& [key, value] : given.items()) {
        if (std::none_of(std::begin(kKeys), std::end(kKeys), [&](const char* k) { return key == k; })) bad("a style preset has an unknown key: " + key);
    }
    Json style = style_part(given);
    // (each library pattern named for the check as a book's "lib-<k>", then named again)
    std::vector<Json> refs;
    core::change_style_patterns(style, [&](Json& fill) {
        if (fill.contains("id")) bad("a style preset's pattern is the library's ({\"library\": its name, …}), not a book's");
        const auto lib = fill.find("library");
        if (lib == fill.end() || !lib->is_string() || lib->get<std::string>().empty() || lib->get<std::string>().size() > 200) {
            bad("a style preset's pattern names the library's pattern: {\"library\": its name, \"asset\"?, …}");
        }
        Json ref{{"library", *lib}};
        if (const auto asset = fill.find("asset"); asset != fill.end() && !asset->is_null()) {
            if (!asset->is_string() || !asset->get<std::string>().starts_with("sha256:") || asset->get<std::string>().size() > 80) {
                bad("a style preset's pattern's asset is its picture's ref (\"sha256:…\")");
            }
            ref["asset"] = *asset;
        }
        fill.erase("library");
        fill.erase("asset");
        fill["id"] = std::string(kLibraryId) + std::to_string(refs.size());
        refs.push_back(std::move(ref));
    });
    const Json normal = core::layer_style_spec(style);
    Json out{{"name", std::string(name)}, {"effects", Json::array()}};
    if (!normal.is_null())
        for (const auto& [key, value] : normal.items()) out[key] = value;
    core::change_style_patterns(out, [&](Json& fill) {
        const std::string id = fill.value("id", std::string());
        const std::size_t k = static_cast<std::size_t>(std::stoul(id.substr(kLibraryId.size())));
        Json put = refs.at(k);
        for (const auto& [key, value] : fill.items())
            if (key != "id") put[key] = value;
        fill = std::move(put);
    });
    if (const auto blend = given.find("blend"); blend != given.end() && !blend->is_null()) {
        try {
            out["blend"] = core::blend_mode(*blend);
        } catch (const std::exception& error) {
            bad(std::string("a style preset's blend: ") + error.what());
        }
    }
    return out;
}

Json item_of_layer(const core::Document& book, const core::Layer& layer, bool effects, bool blending, std::string_view name,
                   const std::filesystem::path& config_dir, std::vector<preset_store::Group>& pattern_groups, bool& pattern_added) {
    if (!effects && !blending) bad("a new style takes the layer's effects, its blending options or both");
    Json given = Json::object();
    const Json style = layer.style ? *layer.style : Json{{"effects", Json::array()}};
    given["effects"] = effects && style.contains("effects") ? style["effects"] : Json::array();
    if (blending) {
        for (const char* key : {"fill_opacity", "blend_if"})
            if (style.contains(key)) given[key] = style[key];
        given["blend"] = layer.blend.empty() ? std::string("normal") : layer.blend;
    }
    core::change_style_patterns(given, [&](Json& fill) {
        const std::string id = fill.value("id", std::string());
        const core::Pattern* p = core::find_pattern(book, id);
        if (p == nullptr) bad("the book keeps no pattern " + id);
        const auto file = book.pattern_files.find(p->asset);
        if (file == book.pattern_files.end() || !file->second) bad("the book holds no picture of the pattern " + id);
        const std::string kept = library_name_of(*p, file->second, config_dir, pattern_groups, std::nullopt, pattern_added);
        Json put{{"library", kept}, {"asset", p->asset}};
        for (const auto& [key, value] : fill.items())
            if (key != "id") put[key] = value;
        fill = std::move(put);
    });
    return item_of(given, name);
}

std::vector<Json> patterns_of(const Json& item) {
    std::vector<Json> out;
    Json style = style_part(item);
    core::change_style_patterns(style, [&](Json& fill) {
        Json ref{{"library", fill.value("library", std::string())}};
        if (fill.contains("asset")) ref["asset"] = fill["asset"];
        if (std::find(out.begin(), out.end(), ref) == out.end()) out.push_back(std::move(ref));
    });
    return out;
}

Json style_for(const Json& item, const std::filesystem::path& config_dir, const std::vector<preset_store::Group>& pattern_groups) {
    Json style = style_part(item);
    core::change_style_patterns(style, [&](Json& fill) {
        const auto [p, bytes] = library_pattern(fill, config_dir, pattern_groups);
        Json put = pattern_library::add_op(p, bytes);
        put.erase("op");
        for (const char* key : {"scale", "angle", "phase_mm", "link"})
            if (fill.contains(key)) put[key] = fill[key];
        fill = std::move(put);
    });
    return style;
}

Json apply_op(const Json& item, const Json& page, const std::vector<std::string>& ids, bool add, const std::filesystem::path& config_dir,
              const std::vector<preset_store::Group>& pattern_groups) {
    Json op{{"op", "set_layer_style"}, {"page", page}, {"ids", ids}, {"style", style_for(item, config_dir, pattern_groups)}};
    if (add) op["add"] = true;
    // (review P3-9, as Photoshop: a preset with its blending options — the layer's mode, fill opacity, Blend If — gives
    // them; one without keeps the layer's, and Shift (add) adds its effects only)
    const bool blending = !add && (item.contains("blend") || item.contains("fill_opacity") || item.contains("blend_if"));
    if (!blending) op["keep_blending"] = true;
    if (const auto blend = item.find("blend"); blending && blend != item.end() && blend->is_string()) op["blend"] = *blend;
    return op;
}

Json list(const std::vector<preset_store::Group>& groups) {
    Json out = Json::array();
    for (const preset_store::Group& g : groups) {
        Json styles = Json::array();
        for (const Json& item : g.items) {
            Json kinds = Json::array();
            for (const Json& e : item.value("effects", Json::array())) kinds.push_back(e.value("kind", std::string()));
            Json one{{"name", item.value("name", std::string())}, {"effects", std::move(kinds)}};
            if (item.contains("blend")) one["blend"] = item["blend"];
            if (item.contains("fill_opacity")) one["fill_opacity"] = item["fill_opacity"];
            styles.push_back(std::move(one));
        }
        out.push_back(Json{{"name", g.name}, {"styles", std::move(styles)}});
    }
    return Json{{"groups", std::move(out)}};
}

Json import_asl(std::string_view data, double dpi, const std::filesystem::path& config_dir, std::vector<preset_store::Group>& pattern_groups,
                const std::string& pattern_group, bool& pattern_added) {
    asl::File file = asl::read(data, dpi);
    std::vector<std::string> notes = std::move(file.notes);
    // (each of the file's patterns taken into the library once, when a style names it; none when it cannot be)
    std::vector<std::optional<Json>> refs(file.patterns.size());
    std::vector<bool> tried(file.patterns.size(), false);
    Json added_patterns = Json::array();
    const auto ref_of = [&](const Json& fill) -> std::optional<Json> {
        const std::string id = fill.value("id", std::string());
        if (!id.starts_with(asl::kPatternPrefix)) return std::nullopt;
        const std::size_t k = static_cast<std::size_t>(std::stoul(id.substr(asl::kPatternPrefix.size())));
        if (k >= refs.size()) return std::nullopt;
        if (!tried[k]) {
            tried[k] = true;
            std::string why;
            if (auto taken = pat::from_psd(file.patterns[k], dpi, &why)) {
                bool added = false;
                const std::string name = library_name_of(taken->pattern, taken->bytes, config_dir, pattern_groups, pattern_group, added);
                if (added) {
                    added_patterns.push_back(name);
                    pattern_added = true;
                }
                refs[k] = Json{{"library", name}, {"asset", taken->pattern.asset}};
            } else {
                notes.push_back("パターン「" + pat::shown_name(file.patterns[k].name) + "」を読めませんでした（" + why + "）");
            }
        }
        return refs[k];
    };
    Json items = Json::array();
    for (asl::Style& s : file.styles) {
        Json style = s.style.is_null() ? Json{{"effects", Json::array()}} : s.style;
        Json kept = Json::array();
        for (Json e : style["effects"]) {
            bool keep = true;
            if (e.contains("pattern")) {
                if (auto ref = ref_of(e["pattern"])) {
                    Json put = *ref;
                    for (const auto& [key, value] : e["pattern"].items())
                        if (key != "id") put[key] = value;
                    e["pattern"] = std::move(put);
                } else {
                    keep = false;
                    notes.push_back("「" + s.name + "」: パターンのない効果（" + e.value("kind", std::string()) + "）を除きました");
                }
            }
            if (keep && e.contains("texture") && e["texture"].is_object() && e["texture"].contains("pattern")) {
                if (auto ref = ref_of(e["texture"]["pattern"])) {
                    Json put = *ref;
                    for (const auto& [key, value] : e["texture"]["pattern"].items())
                        if (key != "id") put[key] = value;
                    e["texture"]["pattern"] = std::move(put);
                } else {
                    e.erase("texture");
                    notes.push_back("「" + s.name + "」: ベベルのテクスチャのパターンがないので、テクスチャを除きました");
                }
            }
            if (keep) kept.push_back(std::move(e));
        }
        style["effects"] = std::move(kept);
        if (s.blend) style["blend"] = *s.blend;
        items.push_back(item_of(style, s.name));
    }
    return Json{{"items", std::move(items)}, {"patterns", std::move(added_patterns)}, {"notes", notes}};
}

Image swatch(const Json& style, const core::Document* book, int size, const std::string& blend, const std::array<double, 2>& light) {
    size = std::clamp(size, 16, 256);
    const std::size_t pixels = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    // (upstream's swatch: a rounded square of light grey, 1 px of anti-aliasing)
    std::vector<float> content(pixels * 4, 0.0f);
    const float m = std::round(static_cast<float>(size) * 0.2f);
    const float x0 = m, x1 = static_cast<float>(size) - m, r = static_cast<float>(size) * 0.12f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float cx = static_cast<float>(x) + 0.5f, cy = static_cast<float>(y) + 0.5f;
            const float qx = std::fabs(cx - (x0 + x1) / 2.0f) - ((x1 - x0) / 2.0f - r);
            const float qy = std::fabs(cy - (x0 + x1) / 2.0f) - ((x1 - x0) / 2.0f - r);
            const float dist = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
            float* px = &content[(static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 4];
            px[0] = px[1] = px[2] = 0.86f;
            px[3] = std::clamp(0.5f - dist, 0.0f, 1.0f);
        }
    }
    std::vector<float> backdrop(pixels * 4, 0.0f);
    Json normal;
    try {
        normal = style.is_null() ? Json(nullptr) : core::layer_style_spec(style);
    } catch (const core::Error&) {
        normal = nullptr;
    }
    if (normal.is_null()) {
        backdrop = content;
    } else {
        layer_styles::Inputs in;
        in.style = &normal;
        in.light = light;
        in.dpi = 150.0 * size / 64.0;
        in.book = book;
        in.wide = in.area = Box{0, 0, size, size};
        in.layer_box = Box{static_cast<int>(x0), static_cast<int>(x0), static_cast<int>(x1), static_cast<int>(x1)};
        in.page = Size{size, size};
        in.blend = blend.empty() ? std::string("normal") : blend;
        layer_styles::composite(in, layer_styles::srgb_mixing(), content, backdrop);
    }
    std::string bytes(pixels * 4, '\0');
    for (std::size_t i = 0; i < pixels * 4; ++i) {
        bytes[i] = static_cast<char>(static_cast<std::uint8_t>(std::lround(std::clamp(backdrop[i], 0.0f, 1.0f) * 255.0f)));
    }
    return Image::frombytes("RGBA", Size{size, size}, bytes);
}

Image thumbnail(const Json& item, int size, const std::filesystem::path& config_dir, const std::vector<preset_store::Group>& pattern_groups) {
    // (its patterns into a book of its own: one the library has not is left out with its effect)
    core::Document book;
    Json style = style_part(item);
    Json kept = Json::array();
    for (Json e : style["effects"]) {
        bool keep = true;
        const auto bring = [&](Json& fill) {
            try {
                auto [p, bytes] = library_pattern(fill, config_dir, pattern_groups);
                p.id = core::next_pattern_id(book);
                const std::string id = core::carry_pattern(book, p, bytes).first;
                Json put{{"id", id}};
                for (const char* key : {"scale", "angle", "phase_mm", "link"})
                    if (fill.contains(key)) put[key] = fill[key];
                fill = std::move(put);
                return true;
            } catch (const std::exception&) {
                return false;
            }
        };
        if (e.contains("pattern") && e["pattern"].is_object()) keep = bring(e["pattern"]);
        if (keep && e.contains("texture") && e["texture"].is_object() && e["texture"].contains("pattern") && !bring(e["texture"]["pattern"])) e.erase("texture");
        if (keep) kept.push_back(std::move(e));
    }
    style["effects"] = std::move(kept);
    return swatch(style, &book, size, item.value("blend", std::string("normal")), {core::kDefaultLightAngle, core::kDefaultLightAltitude});
}


}  // namespace genko::render::style_presets
