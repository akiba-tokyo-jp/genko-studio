// PC4 節 D (SPEC.md COLOR-08): the table of フィルター's kinds (core/filter_kinds.hpp). Settings, defaults and ranges:
// PhotoCraft v0.2.0 engine/src/filters.rs (41-134: params_for), filters_ext.rs (70-371), algo/src/artistic.rs (39-91),
// lens_cmds.rs (376-422), render_cmds.rs (185-219); attribution and the upstream MIT license:
// native/third_party/photocraft/NOTICE.md.

#include "core/filter_kinds.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/command_bus.hpp"
#include "core/gallery_kinds.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace {

using Type = AdjustParam::Type;

AdjustParam number(std::string_view key, double fallback, double lo, double hi) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Number;
    p.fallback = fallback;
    p.lo = lo;
    p.hi = hi;
    return p;
}

// A kind of PC4 (a filter_raster kind of its own that looks beyond each pixel, a smart filter: design 2.3, 3.3).
FilterKind kind(FilterInfo info, std::vector<AdjustParam> params) {
    FilterKind out;
    out.kind.name = info.kind;
    out.kind.group = "PC4";
    out.kind.layer = false;
    out.kind.filter = true;
    out.kind.local = true;
    out.kind.smart = true;
    out.kind.params = std::move(params);
    // (its native — a blend, as every kind's: refused on a filter by normalize_filter; fade_layer lays a result in a blend —
    // is the table's: core/adjust_kinds.cpp)
    out.info = std::move(info);
    return out;
}

FilterInfo info(std::string_view kind, std::string_view upstream, std::string_view name, std::string_view submenu,
                FilterInks inks = FilterInks::Plates) {
    FilterInfo out;
    out.kind = kind;
    out.upstream = upstream;
    out.name = name;
    out.submenu = submenu;
    out.inks = inks;
    return out;
}

std::vector<FilterKind> all_kinds() {
    std::vector<FilterKind> all;
    for (auto group : {blur_kinds, sharpen_kinds, noise_kinds, other_kinds, video_kinds, distort_kinds, pixelate_kinds, stylize_kinds,
                       gallery_kinds, render_kinds, blur_gallery_kinds, camera_raw_kinds, lens_kinds}) {
        for (FilterKind& each : group()) all.push_back(std::move(each));
    }
    return all;
}

const std::vector<FilterKind>& table() {
    static const std::vector<FilterKind> all = all_kinds();
    return all;
}

std::string_view type_name(Type type) {
    switch (type) {
        case Type::Number: return "number";
        case Type::Whole: return "whole";
        case Type::Bool: return "bool";
        case Type::Choice: return "choice";
        case Type::Rgb: return "rgb";
        case Type::Numbers: return "numbers";
        case Type::Group: return "group";
        case Type::Text: return "text";
        case Type::Custom: return "custom";
    }
    return "custom";
}

Json number_json(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) return Json(static_cast<std::int64_t>(v));
    return Json(v);
}

Json param_json(const AdjustParam& p) {
    Json out{{"key", std::string(p.key)}, {"type", std::string(type_name(p.type))}};
    switch (p.type) {
        case Type::Number:
        case Type::Whole:
            out["default"] = number_json(p.fallback);
            out["min"] = number_json(p.lo);
            out["max"] = number_json(p.hi);
            break;
        case Type::Bool: out["default"] = p.fallback != 0; break;
        case Type::Choice: {
            Json choices = Json::array();
            for (const std::string_view c : p.choices) choices.push_back(std::string(c));
            out["choices"] = std::move(choices);
            out["default"] = std::string(p.text);
            break;
        }
        case Type::Rgb:
        case Type::Numbers: {
            Json fallback = Json::array();
            for (const double v : p.numbers) fallback.push_back(number_json(v));
            out["default"] = std::move(fallback);
            out["min"] = number_json(p.lo);
            out["max"] = number_json(p.hi);
            break;
        }
        case Type::Group: {
            Json group = Json::array();
            for (const AdjustParam& g : p.group) group.push_back(param_json(g));
            out["settings"] = std::move(group);
            break;
        }
        case Type::Text:
        case Type::Custom: break;
    }
    if (p.optional) out["optional"] = true;
    return out;
}

std::string_view place_kind(FilterPlace::Kind kind) {
    switch (kind) {
        case FilterPlace::Kind::Centre: return "centre";
        case FilterPlace::Kind::Point: return "point_mm";
        case FilterPlace::Kind::Points: return "points_mm";
        case FilterPlace::Kind::Length: return "length_mm";
    }
    return "point_mm";
}

// A smart filter's kind and settings, for each smart filter of the pages the book writes (its pages, its contents').
template <typename Each>
void each_smart_filter(const Document& doc, Each&& each) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        for (const Layer& layer : written.page->layers) {
            if (layer.kind != LayerKind::Smart || !layer.smart || !layer.smart->is_object()) continue;
            const Json* filters = get(*layer.smart, "filters");
            if (filters == nullptr || !filters->is_array()) continue;
            for (const Json& f : *filters) {
                if (!f.is_object()) continue;
                const Json* kind = get(f, "kind");
                if (kind == nullptr || !kind->is_string()) continue;
                const Json* params = get(f, "params");
                each(kind->get<std::string>(), params != nullptr && params->is_object() ? *params : Json::object(), layer);
            }
        }
    }
}

bool is_ref(const std::string& ref) {
    constexpr std::string_view prefix = "sha256:";
    if (ref.size() != prefix.size() + 64 || ref.compare(0, prefix.size(), prefix) != 0) return false;
    return std::all_of(ref.begin() + static_cast<std::ptrdiff_t>(prefix.size()), ref.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// The suffixes a filter's file may have (the displacement map: a PNG or a PSD, design 2.6).
bool known_suffix(const std::string& suffix) { return suffix == ".png" || suffix == ".psd"; }

// Photoshop's units (the document's pixels at `dpi`) and the working 200 dpi's.
double to_working(double px, double dpi) { return dpi > 0 ? px * 200.0 / dpi : px; }
double from_working(double px, double dpi) { return dpi > 0 ? px * dpi / 200.0 : px; }

double number_at(const Json& params, const char* key, double fallback) {
    const Json* v = params.is_object() ? get(params, key) : nullptr;
    return v != nullptr && v->is_number() && !v->is_boolean() ? v->get<double>() : fallback;
}

}  // namespace

std::string_view to_string(FilterInks inks) {
    switch (inks) {
        case FilterInks::Plates: return "plates";
        case FilterInks::Lightness: return "lightness";
        case FilterInks::Meaning: return "meaning";
        case FilterInks::Halftone: return "halftone";
    }
    return "plates";
}

// --- each group's kinds ---------------------------------------------------------------------------------------------

std::vector<FilterKind> blur_kinds() {
    std::vector<FilterKind> out;
    // D0 (design 2.1): Gaussian Blur (filters.rs: radius 0.1..1000, default 1) and Box Blur (1..2000, default 1); the
    // radius in pixels of the working 200 dpi
    out.push_back(kind(info("blur_gaussian", "filter.blur.gaussianBlur", "ぼかし（ガウス）", "ぼかし"), {number("radius", 1, 0.1, 1000)}));
    out.push_back(kind(info("blur_box", "filter.blur.boxBlur", "ぼかし（ボックス）", "ぼかし"), {number("radius", 1, 1, 2000)}));
    return out;
}
std::vector<FilterKind> sharpen_kinds() { return {}; }       // (D1)
std::vector<FilterKind> noise_kinds() { return {}; }         // (D1)
std::vector<FilterKind> other_kinds() { return {}; }         // (D1)
std::vector<FilterKind> video_kinds() { return {}; }         // (D1)
std::vector<FilterKind> distort_kinds() { return {}; }       // (D2)
std::vector<FilterKind> pixelate_kinds() { return {}; }      // (D2)
std::vector<FilterKind> stylize_kinds() { return {}; }       // (D2)
std::vector<FilterKind> render_kinds() { return {}; }        // (D4)
std::vector<FilterKind> blur_gallery_kinds() { return {}; }  // (D4)
std::vector<FilterKind> camera_raw_kinds() { return {}; }    // (D5)
std::vector<FilterKind> lens_kinds() { return {}; }          // (D6)

// --- the table ------------------------------------------------------------------------------------------------------

std::vector<AdjustKind> filter_kinds() {
    std::vector<AdjustKind> out;
    for (const FilterKind& each : table()) out.push_back(each.kind);
    return out;
}

const std::vector<FilterInfo>& filter_infos() {
    static const std::vector<FilterInfo> all = [] {
        std::vector<FilterInfo> out;
        for (const FilterKind& each : table()) out.push_back(each.info);
        return out;
    }();
    return all;
}

const FilterInfo* filter_info(std::string_view kind) {
    for (const FilterInfo& each : filter_infos()) {
        if (each.kind == kind) return &each;
    }
    return nullptr;
}

bool pc4_filter(std::string_view kind) { return filter_info(kind) != nullptr; }

const std::vector<FilterPreset>& filter_presets() {
    // (PhotoCraft's one-step presets, filters.rs:45-50: their settings in the working 200 dpi's pixels — design 9 章)
    static const std::vector<FilterPreset> all{
        {"filter.blur.blur", "ぼかし", "ぼかし", "blur_gaussian", Json{{"radius", 0.6}}},
        {"filter.blur.blurMore", "ぼかし（強）", "ぼかし", "blur_gaussian", Json{{"radius", 1.4}}},
        {"filter.sharpen.sharpen", "シャープ", "シャープ", "sharpen_unsharp_mask", Json{{"amount", 60}, {"radius", 0.5}, {"threshold", 0}}},
        {"filter.sharpen.sharpenMore", "シャープ（強）", "シャープ", "sharpen_unsharp_mask", Json{{"amount", 150}, {"radius", 0.6}, {"threshold", 0}}},
        {"filter.sharpen.sharpenEdges", "シャープ（輪郭のみ）", "シャープ", "sharpen_unsharp_mask", Json{{"amount", 100}, {"radius", 0.8}, {"threshold", 6}}},
        {"filter.noise.despeckle", "輪郭以外をぼかす", "ノイズ", "blur_surface", Json{{"radius", 2}, {"threshold", 12}}},
    };
    return all;
}

std::optional<std::pair<std::string, Json>> from_upstream(std::string_view command, const Json& params, double dpi) {
    for (const FilterPreset& preset : filter_presets()) {
        if (preset.upstream != command || !pc4_filter(preset.kind)) continue;
        if (preset.kind == "blur_gaussian") {  // (its radius is the document's pixels upstream)
            return std::make_pair(std::string(preset.kind), Json{{"radius", to_working(preset.params["radius"].get<double>(), dpi)}});
        }
    }
    if (command == "filter.blur.gaussianBlur") return std::make_pair(std::string("blur_gaussian"), Json{{"radius", to_working(number_at(params, "radius", 1), dpi)}});
    if (command == "filter.blur.boxBlur") return std::make_pair(std::string("blur_box"), Json{{"radius", to_working(number_at(params, "radius", 1), dpi)}});
    return std::nullopt;  // (D7: the rest of the table)
}

std::optional<std::pair<std::string, Json>> to_upstream(std::string_view kind, const Json& params, double dpi) {
    const FilterInfo* found = filter_info(kind);
    if (found == nullptr) return std::nullopt;
    if (kind == "blur_gaussian" || kind == "blur_box")
        return std::make_pair(std::string(found->upstream), Json{{"radius", from_working(number_at(params, "radius", 1), dpi)}});
    return std::nullopt;  // (D7)
}

// --- filters@1 -------------------------------------------------------------------------------------------------------

bool uses_filters(const Document& doc) {
    bool used = false;
    each_smart_filter(doc, [&used](const std::string& kind, const Json&, const Layer&) { used = used || pc4_filter(kind); });
    return used;
}

std::vector<std::pair<std::string, std::string>> filter_file_refs(const Document& doc) {
    std::vector<std::pair<std::string, std::string>> out;
    std::set<std::pair<std::string, std::string>> seen;
    each_smart_filter(doc, [&](const std::string& kind, const Json& params, const Layer&) {
        const FilterInfo* found = filter_info(kind);
        if (found == nullptr) return;
        for (const std::string_view key : found->files) {
            const Json* ref = get(params, key);
            const Json* suffix = get(params, std::string(key) + "_suffix");
            if (ref == nullptr || !ref->is_string() || suffix == nullptr || !suffix->is_string()) continue;
            auto item = std::make_pair(ref->get<std::string>(), suffix->get<std::string>());
            if (seen.insert(item).second) out.push_back(std::move(item));
        }
    });
    return out;
}

void validate_filters(const Document& doc, const std::function<bool(const std::string&, const std::string&)>& held) {
    each_smart_filter(doc, [&](const std::string& kind, const Json& params, const Layer& layer) {
        const FilterInfo* found = filter_info(kind);
        if (found == nullptr) return;
        for (const std::string_view key : found->files) {
            const Json* ref = get(params, key);
            if (ref == nullptr || ref->is_null()) continue;
            const std::string where = "the smart object " + layer.id + "'s filter " + kind + "." + std::string(key);
            const Json* suffix = get(params, std::string(key) + "_suffix");
            if (!ref->is_string() || !is_ref(ref->get<std::string>()))
                throw Error("value", where + " is not one of the book's files (sha256:…)");
            if (suffix == nullptr || !suffix->is_string() || !known_suffix(suffix->get<std::string>()))
                throw Error("value", where + "_suffix is not .png or .psd");
            const std::string& name = ref->get_ref<const std::string&>();
            if (doc.filter_files.contains(name)) continue;
            if (held && !held(name, suffix->get<std::string>())) throw Error("value", where + ": the book holds no file " + name);
        }
    });
}

// --- the catalogue ---------------------------------------------------------------------------------------------------

Json filter_catalogue() {
    Json kinds = Json::array();
    for (const FilterKind& each : table()) {
        Json settings = Json::array();
        for (const AdjustParam& p : each.kind.params) settings.push_back(param_json(p));
        Json places = Json::array();
        for (const FilterPlace& place : each.info.places) {
            Json item{{"kind", std::string(place_kind(place.kind))}, {"key", std::string(place.key)}};
            if (!place.key2.empty()) item["key2"] = std::string(place.key2);
            if (!place.within.empty()) item["within"] = std::string(place.within);
            places.push_back(std::move(item));
        }
        Json colours = Json::array();
        for (const std::string_view key : each.info.colours) colours.push_back(std::string(key));
        Json files = Json::array();
        for (const std::string_view key : each.info.files) files.push_back(std::string(key));
        kinds.push_back(Json{{"kind", std::string(each.info.kind)},
                             {"upstream", std::string(each.info.upstream)},
                             {"name", std::string(each.info.name)},
                             {"submenu", std::string(each.info.submenu)},
                             {"settings", std::move(settings)},
                             {"smart", each.kind.smart},
                             {"inks", std::string(to_string(each.info.inks))},
                             {"places", std::move(places)},
                             {"colours", std::move(colours)},
                             {"files", std::move(files)}});
    }
    Json presets = Json::array();
    for (const FilterPreset& preset : filter_presets()) {
        presets.push_back(Json{{"upstream", std::string(preset.upstream)},
                               {"name", std::string(preset.name)},
                               {"submenu", std::string(preset.submenu)},
                               {"kind", std::string(preset.kind)},
                               {"params", preset.params},
                               {"usable", pc4_filter(preset.kind)}});
    }
    // (the lengths: the working 200 dpi's pixels; places: the page's mm or shares of the base box; colours 0..255)
    return Json{{"units", Json{{"length", "px200"}, {"place", "mm"}, {"centre", "share"}, {"colour", "rgb255"}}},
                {"kinds", std::move(kinds)},
                {"presets", std::move(presets)},
                {"gallery", gallery_catalogue()}};  // (D3: the 47 effects of フィルターギャラリー, core/gallery_kinds.cpp)
}

}  // namespace genko::core
