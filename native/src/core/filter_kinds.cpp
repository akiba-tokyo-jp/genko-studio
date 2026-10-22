// PC4 節 D (SPEC.md COLOR-08): the table of フィルター's kinds (core/filter_kinds.hpp). Settings, defaults and ranges:
// PhotoCraft v0.2.0 engine/src/filters.rs (41-134: params_for), filters_ext.rs (70-371), algo/src/artistic.rs (39-91),
// lens_cmds.rs (376-422), render_cmds.rs (185-219); attribution and the upstream MIT license:
// native/third_party/photocraft/NOTICE.md.

#include "core/filter_kinds.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include "core/base64.hpp"
#include "core/pyconv.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/gallery_kinds.hpp"
#include "core/ink.hpp"
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

AdjustParam whole(std::string_view key, double fallback, double lo, double hi) {
    AdjustParam p = number(key, fallback, lo, hi);
    p.type = Type::Whole;
    return p;
}

AdjustParam boolean(std::string_view key, bool fallback) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Bool;
    p.fallback = fallback ? 1 : 0;
    return p;
}

AdjustParam choice(std::string_view key, std::vector<std::string_view> choices) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Choice;
    p.choices = std::move(choices);
    p.text = p.choices.front();  // (the default: the first, PhotoCraft's)
    return p;
}

AdjustParam numbers(std::string_view key, std::vector<double> fallback, double lo, double hi) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Numbers;
    p.numbers = std::move(fallback);
    p.lo = lo;
    p.hi = hi;
    return p;
}

// (a seed of PhotoCraft's: a u32)
AdjustParam seed() { return whole("seed", 0, 0, 4294967295.0); }

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
        case FilterInks::LightnessAndColour: return "lightness_and_colour";
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
    // D1 (filters.rs 41-134, filters_ext.rs 234-295, extra_cmds.rs 187-205): 平均 (the selection's — else the layer's —
    // mean colour), ぼかし（移動） (its angle in degrees, the page's way up; its distance), ぼかし（放射状） (回転 or ズーム about
    // a centre of shares of the base box: a place that moves with a smart object), ぼかし（表面）, ぼかし（詳細）,
    // ぼかし（シェイプ） (PhotoCraft's built-in shapes), ぼかし（レンズ） (its iris, its depth, its highlights and noise)
    out.push_back(kind(info("blur_average", "filter.blur.average", "平均", "ぼかし"), {}));
    out.push_back(kind(info("blur_motion", "filter.blur.motionBlur", "ぼかし（移動）", "ぼかし"), {number("angle", 0, -360, 360), number("distance", 10, 1, 2000)}));
    {
        FilterInfo radial = info("blur_radial", "filter.blur.radialBlur", "ぼかし（放射状）", "ぼかし");
        radial.places = {{FilterPlace::Kind::Centre, "center_x", "center_y", {}}};
        out.push_back(kind(std::move(radial), {number("amount", 10, 1, 100), choice("method", {"spin", "zoom"}), number("center_x", 0.5, 0, 1),
                                               number("center_y", 0.5, 0, 1)}));
    }
    out.push_back(kind(info("blur_surface", "filter.blur.surfaceBlur", "ぼかし（表面）", "ぼかし"), {number("radius", 5, 1, 100), number("threshold", 15, 2, 255)}));
    {
        AdjustParam quality = choice("quality", {"high", "medium", "low"});
        out.push_back(kind(info("blur_smart", "filter.blur.smartBlur", "ぼかし（詳細）", "ぼかし"),
                           {number("radius", 3, 0.1, 100), number("threshold", 25, 0.1, 100), quality, choice("mode", {"normal", "edge_only", "overlay_edge"})}));
    }
    out.push_back(kind(info("blur_shape", "filter.blur.shapeBlur", "ぼかし（シェイプ）", "ぼかし"),
                       {number("radius", 10, 5, 1000), choice("shape", {"circle", "ring", "square", "diamond", "triangle", "hexagon", "star", "heart", "cross"})}));
    out.push_back(kind(info("blur_lens", "filter.blur.lensBlur", "ぼかし（レンズ）", "ぼかし"),
                       {number("radius", 15, 0, 100), choice("shape", {"hexagon", "triangle", "square", "pentagon", "heptagon", "octagon"}),
                        number("blade_curvature", 0, 0, 100), number("rotation", 0, 0, 360), choice("depth_map", {"none", "transparency", "layer_mask"}),
                        number("focal_distance", 0, 0, 255), boolean("invert", false), number("brightness", 0, 0, 100), number("threshold", 255, 0, 255),
                        number("noise", 0, 0, 100), choice("distribution", {"uniform", "gaussian"}), boolean("monochromatic", false), seed()}));
    return out;
}

std::vector<FilterKind> sharpen_kinds() {
    // D1 (filters.rs): アンシャープマスク (amount %, radius, threshold in levels) and スマートシャープ (its noise reduction %);
    // シャープ・シャープ（強）・シャープ（輪郭のみ） are the unsharp mask at fixed settings (filter_presets)
    std::vector<FilterKind> out;
    out.push_back(kind(info("sharpen_unsharp_mask", "filter.sharpen.unsharpMask", "アンシャープマスク", "シャープ"),
                       {number("amount", 50, 1, 500), number("radius", 1, 0.1, 1000), number("threshold", 0, 0, 255)}));
    out.push_back(kind(info("sharpen_smart", "filter.sharpen.smartSharpen", "スマートシャープ", "シャープ"),
                       {number("amount", 100, 1, 500), number("radius", 1, 0.1, 64), number("reduce_noise", 10, 0, 100)}));
    return out;
}

std::vector<FilterKind> noise_kinds() {
    // D1 (filters.rs, filters_ext.rs 234-240): ノイズを加える (amount 0.1..400 %, its distribution, グレースケールノイズ, its
    // seed), ダスト＆スクラッチ, 明るさの中間値, ノイズを軽減; 輪郭以外をぼかす is ぼかし（表面） at 2 and 12 (filter_presets)
    std::vector<FilterKind> out;
    out.push_back(kind(info("noise_add", "filter.noise.addNoise", "ノイズを加える", "ノイズ"),
                       {number("amount", 12.5, 0.1, 400), choice("distribution", {"uniform", "gaussian"}), boolean("monochromatic", false), seed()}));
    out.push_back(kind(info("noise_dust_and_scratches", "filter.noise.dustAndScratches", "ダスト＆スクラッチ", "ノイズ"),
                       {number("radius", 1, 1, 500), number("threshold", 0, 0, 255)}));
    out.push_back(kind(info("noise_median", "filter.noise.median", "明るさの中間値", "ノイズ"), {number("radius", 1, 1, 500)}));
    out.push_back(kind(info("noise_reduce", "filter.noise.reduceNoise", "ノイズを軽減", "ノイズ"),
                       {number("strength", 6, 0, 10), number("preserve_details", 60, 0, 100), number("reduce_color_noise", 45, 0, 100),
                        number("sharpen_details", 25, 0, 100), boolean("remove_jpeg_artifact", false)}));
    return out;
}

std::vector<FilterKind> other_kinds() {
    // D1 (filters.rs, filters_ext.rs 345-362): カスタム (a 5×5 kernel, row by row — its centre the 13th —, divided by its
    // scale, its offset in levels added: on the lightness of a page of inks), ハイパス (the lightness's: its middle grey),
    // HSB/HSL (the colours the inks mean), 明るさの最大値・最小値 (a square or a disc: on the lightness), スクロール (the base
    // box moved by whole working pixels, what it leaves wrapped, repeated or clear)
    std::vector<FilterKind> out;
    std::vector<double> identity(25, 0.0);
    identity[12] = 1;
    out.push_back(kind(info("other_custom", "filter.other.custom", "カスタム", "その他", FilterInks::LightnessAndColour),
                       {numbers("kernel", identity, -999, 999), number("scale", 1, 1, 9999), number("offset", 0, -9999, 9999)}));
    out.push_back(kind(info("other_high_pass", "filter.other.highPass", "ハイパス", "その他", FilterInks::LightnessAndColour),
                       {number("radius", 10, 0.1, 1000)}));
    out.push_back(kind(info("other_hsb_hsl", "filter.other.hsbHsl", "HSB/HSL", "その他", FilterInks::Meaning),
                       {choice("input_mode", {"rgb", "hsb", "hsl"}), choice("row_order", {"hsb", "hsl", "rgb"})}));
    out.push_back(kind(info("other_maximum", "filter.other.maximum", "明るさの最大値", "その他", FilterInks::Lightness),
                       {number("radius", 1, 0.2, 500), choice("preserve", {"squareness", "roundness"})}));
    out.push_back(kind(info("other_minimum", "filter.other.minimum", "明るさの最小値", "その他", FilterInks::Lightness),
                       {number("radius", 1, 0.2, 500), choice("preserve", {"squareness", "roundness"})}));
    out.push_back(kind(info("other_offset", "filter.other.offset", "スクロール", "その他"),
                       {number("horizontal", 0, -30000, 30000), number("vertical", 0, -30000, 30000), choice("undefined_areas", {"wrap", "repeat", "transparent"})}));
    return out;
}

std::vector<FilterKind> video_kinds() {
    // D1 (filters_ext.rs 363-367): インターレース解除 (the odd or even lines — of the working 200 dpi — made again by
    // interpolation or duplication) and NTSC カラー (the colours the inks mean)
    std::vector<FilterKind> out;
    out.push_back(kind(info("video_de_interlace", "filter.video.deInterlace", "インターレース解除", "ビデオ"),
                       {choice("eliminate", {"odd_fields", "even_fields"}), choice("create_by", {"interpolation", "duplication"})}));
    out.push_back(kind(info("video_ntsc_colors", "filter.video.ntscColors", "NTSC カラー", "ビデオ", FilterInks::Meaning), {}));
    return out;
}
// --- D2: 変形・ピクセレート・表現手法 ----------------------------------------------------------------------------------

namespace {

// (D2) A choice whose default is not its first value.
AdjustParam choice_of(std::string_view key, std::vector<std::string_view> choices, std::string_view fallback) {
    AdjustParam p = choice(key, std::move(choices));
    p.text = fallback;
    return p;
}

// (D2) A setting read by the kind's own normalize (absent: what the normalize says).
AdjustParam custom(std::string_view key, bool optional) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Custom;
    p.optional = optional;
    return p;
}

// (D2) A colour [r, g, b] 0..255 with its default.
AdjustParam rgb(std::string_view key, std::array<int, 3> fallback) {
    AdjustParam p;
    p.key = key;
    p.type = Type::Rgb;
    p.numbers = {static_cast<double>(fallback[0]), static_cast<double>(fallback[1]), static_cast<double>(fallback[2])};
    p.lo = 0;
    p.hi = 255;
    return p;
}

bool finite_number(const Json& v) { return v.is_number() && !v.is_boolean() && std::isfinite(v.get<double>()); }

// 置き換え's map (design 2.6) in its normal form: a file's data (map_data: a PNG or PSD, base64 — the map is then that
// file, its ref and suffix added), a layer of the book ({"page"?, "layer": id | "merged"}) or a file the book holds
// ("map": "sha256:…" and its "map_suffix"). One is required; the data first, then a layer, else a file.
Json read_displacement_map(Json read) {
    const std::string where = "adjust distort_displace";
    if (const Json* data = get(read, "map_data"); data != nullptr && !data->is_null()) {
        const std::string wrong = where + ".map_data must be the base64 text of a PNG or PSD file";
        if (!data->is_string()) throw OpError(wrong);
        std::string bytes;
        try {
            bytes = a2b_base64(data->get_ref<const std::string&>());
        } catch (const Error&) {
            throw OpError(wrong);
        }
        if (bytes.size() > kDisplacementMapMaxBytes) throw OpError(where + ".map_data is larger than 32 MiB");
        std::string suffix;
        if (bytes.starts_with(std::string_view("\x89PNG\r\n\x1a\n", 8))) suffix = ".png";
        else if (bytes.starts_with("8BPS")) suffix = ".psd";
        else throw OpError(wrong);
        read["map"] = bytes_ref(bytes);
        read["map_suffix"] = suffix;
        return read;
    }
    read.erase("map_data");
    const Json* map = get(read, "map");
    if (map == nullptr || map->is_null())
        throw OpError(where + ".map is required: a layer of the book ({\"layer\": id or \"merged\"}) or a file's data (map_data: a PNG or PSD)");
    if (map->is_object()) {
        for (const auto& [key, value] : map->items()) {
            if (key != "page" && key != "layer") throw OpError(where + ".map has no setting " + py_repr_str(key) + " (page, layer)");
        }
        Json layer = Json::object();
        if (const Json* page = get(*map, "page"); page != nullptr && !page->is_null()) {
            if (!page->is_number_integer() && !page->is_string()) throw OpError(where + ".map.page must be a page of the book");
            layer["page"] = *page;
        }
        const Json* id = get(*map, "layer");
        if (id == nullptr || !id->is_string() || id->get_ref<const std::string&>().empty())
            throw OpError(where + ".map.layer must be a layer's id or \"merged\"");
        layer["layer"] = *id;
        read["map"] = std::move(layer);
        read.erase("map_suffix");  // (a layer's: none)
        return read;
    }
    if (!map->is_string() || !is_ref(map->get<std::string>()))
        throw OpError(where + ".map must be a layer of the book ({\"layer\": id or \"merged\"}) or one of its files (sha256:…)");
    const Json* suffix = get(read, "map_suffix");
    if (suffix == nullptr || !suffix->is_string() || !known_suffix(suffix->get<std::string>()))
        throw OpError(where + ".map_suffix must be .png or .psd (the kind of the book's file)");
    return read;
}

// シアー's curve (filters_ext.rs Shear, distort2.rs shear): 2 to 64 points [t, offset] — t 0..1 from the base box's top
// to its bottom, offset −1..1 of half its width —, clamped, sorted by t, the second of two within 1e-6 left out; none
// given: the straight line [[0, 0], [1, 0]].
Json read_shear_points(Json read) {
    const Json* points = get(read, "points");
    if (points == nullptr || points->is_null()) {
        read["points"] = Json::array({Json::array({0, 0}), Json::array({1, 0})});
        return read;
    }
    const std::string message = "adjust distort_shear.points must be a list of 2 to " + std::to_string(kShearMostPoints) +
                                " points [t, offset] (t 0..1 from the top, offset −1..1 of half the width)";
    if (!points->is_array() || points->size() < 2 || points->size() > kShearMostPoints) throw OpError(message);
    std::vector<std::array<double, 2>> kept;
    for (const Json& point : *points) {
        if (!point.is_array() || point.size() != 2 || !finite_number(point[0]) || !finite_number(point[1])) throw OpError(message);
        kept.push_back({std::clamp(point[0].get<double>(), 0.0, 1.0), std::clamp(point[1].get<double>(), -1.0, 1.0)});
    }
    std::stable_sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    Json out = Json::array();
    for (const auto& point : kept) {
        if (!out.empty() && std::fabs(point[0] - out.back()[0].get<double>()) < 1e-6) continue;
        out.push_back(Json::array({number_json(point[0]), number_json(point[1])}));
    }
    read["points"] = std::move(out);
    return read;
}

}  // namespace

std::vector<FilterKind> distort_kinds() {
    // D2 (filters.rs 92-129, filters_ext.rs 151-171; design 2.1, 2.6): each a distortion of the base box — a selection's
    // box, else the layer's grid — centred on it, its pixels moved (each plate of a page of inks as it is). Lengths (波形's
    // wavelengths and amplitudes, 波紋's sizes, 置き換え's 100 % = 128 px) are pixels of the working 200 dpi.
    std::vector<FilterKind> out;
    {
        FilterKind displace = kind(info("distort_displace", "filter.distort.displace", "置き換え", "変形"),
                                   {number("horizontal", 10, -999, 999), number("vertical", 10, -999, 999), choice("fit", {"stretch", "tile"}),
                                    choice("undefined_areas", {"repeat", "wrap"}), custom("map", true), custom("map_suffix", true), custom("map_data", true)});
        displace.info.files = {"map"};
        displace.kind.hooks.normalize = [](Json read, const Page*, LookupTables*) { return read_displacement_map(std::move(read)); };
        out.push_back(std::move(displace));
    }
    out.push_back(kind(info("distort_pinch", "filter.distort.pinch", "つまむ", "変形"), {number("amount", 50, -100, 100)}));
    out.push_back(kind(info("distort_polar_coordinates", "filter.distort.polarCoordinates", "極座標", "変形"),
                       {choice("mode", {"rectangular_to_polar", "polar_to_rectangular"})}));
    out.push_back(kind(info("distort_ripple", "filter.distort.ripple", "波紋", "変形"),
                       {number("amount", 100, -999, 999), choice_of("size", {"small", "medium", "large"}, "medium")}));
    {
        FilterKind shear = kind(info("distort_shear", "filter.distort.shear", "シアー", "変形"), {custom("points", false), choice("undefined_areas", {"wrap", "repeat"})});
        shear.kind.hooks.normalize = [](Json read, const Page*, LookupTables*) { return read_shear_points(std::move(read)); };
        out.push_back(std::move(shear));
    }
    out.push_back(kind(info("distort_spherize", "filter.distort.spherize", "球面", "変形"),
                       {number("amount", 100, -100, 100), choice("mode", {"normal", "horizontal_only", "vertical_only"})}));
    out.push_back(kind(info("distort_twirl", "filter.distort.twirl", "渦巻き", "変形"), {number("angle", 50, -999, 999)}));
    out.push_back(kind(info("distort_wave", "filter.distort.wave", "波形", "変形"),
                       {whole("generators", 5, 1, 999), number("wavelength_min", 10, 1, 998), number("wavelength_max", 120, 2, 999),
                        number("amplitude_min", 5, 1, 998), number("amplitude_max", 35, 1, 999), choice("type", {"sine", "triangle", "square"}),
                        choice("undefined_areas", {"wrap", "repeat"}), seed()}));
    out.push_back(kind(info("distort_zigzag", "filter.distort.zigZag", "ジグザグ", "変形"),
                       {number("amount", 10, -100, 100), number("ridges", 5, 0, 20), choice("style", {"pond_ripples", "out_from_center", "around_center"})}));
    return out;
}

std::vector<FilterKind> pixelate_kinds() {
    // D2 (filters.rs 86, filters_ext.rs 74-99): cells and screens anchored at the base box's corner, a cell's size in
    // pixels of the working 200 dpi. カラーハーフトーン and メゾティント screen each plate of a page of inks (a Lab page: the
    // colours it means); 点描 lays its dots on the background colour (the colours the inks mean).
    std::vector<FilterKind> out;
    out.push_back(kind(info("pixelate_color_halftone", "filter.pixelate.colorHalftone", "カラーハーフトーン", "ピクセレート", FilterInks::Halftone),
                       {number("max_radius", 8, 4, 127), number("channel1", 108, -360, 360), number("channel2", 162, -360, 360), number("channel3", 90, -360, 360),
                        number("channel4", 45, -360, 360)}));
    out.push_back(kind(info("pixelate_crystallize", "filter.pixelate.crystallize", "水晶", "ピクセレート"), {number("cell_size", 10, 3, 300), seed()}));
    out.push_back(kind(info("pixelate_facet", "filter.pixelate.facet", "ファセット", "ピクセレート"), {}));
    out.push_back(kind(info("pixelate_fragment", "filter.pixelate.fragment", "ぶれ", "ピクセレート"), {}));
    out.push_back(kind(info("pixelate_mezzotint", "filter.pixelate.mezzotint", "メゾティント", "ピクセレート", FilterInks::Halftone),
                       {choice("type", {"fine_dots", "medium_dots", "grainy_dots", "coarse_dots", "short_lines", "medium_lines", "long_lines",
                                        "short_strokes", "medium_strokes", "long_strokes"}),
                        seed()}));
    out.push_back(kind(info("pixelate_mosaic", "filter.pixelate.mosaic", "モザイク", "ピクセレート"), {number("cell_size", 10, 2, 200)}));
    {
        FilterInfo dots = info("pixelate_pointillize", "filter.pixelate.pointillize", "点描", "ピクセレート", FilterInks::Meaning);
        dots.colours = {"background"};
        out.push_back(kind(std::move(dots), {number("cell_size", 5, 3, 300), seed(), rgb("background", {255, 255, 255})}));
    }
    return out;
}

std::vector<FilterKind> stylize_kinds() {
    // D2 (filters.rs 87-91, filters_ext.rs 100-150): on a page of inks by the lightness (CMYK and multichannel plates as
    // 1 − ink — a relief, an edge, a shade and a streak as on RGB —; a Lab page's L, a and b each: the kernel keeps or greys
    // its a and b), タイル by the colours the inks mean (its fills are colours). Lengths in pixels of the working 200 dpi.
    std::vector<FilterKind> out;
    const FilterInks light = FilterInks::LightnessAndColour;
    out.push_back(kind(info("stylize_diffuse", "filter.stylize.diffuse", "拡散", "表現手法", light),
                       {choice("mode", {"normal", "darken_only", "lighten_only", "anisotropic"}), seed()}));
    out.push_back(kind(info("stylize_emboss", "filter.stylize.emboss", "エンボス", "表現手法", light),
                       {number("angle", 135, -180, 180), number("height", 3, 1, 100), number("amount", 100, 1, 500)}));
    out.push_back(kind(info("stylize_extrude", "filter.stylize.extrude", "押し出し", "表現手法", light),
                       {choice("type", {"blocks", "pyramids"}), number("size", 30, 2, 255), number("depth", 30, 1, 255), choice("depth_mode", {"random", "level_based"}),
                        boolean("solid_front_faces", false), boolean("mask_incomplete_blocks", false), seed()}));
    out.push_back(kind(info("stylize_find_edges", "filter.stylize.findEdges", "輪郭検出", "表現手法", light), {}));
    out.push_back(kind(info("stylize_oil_paint", "filter.stylize.oilPaint", "油彩", "表現手法", light),
                       {number("stylization", 4, 0.1, 10), number("cleanliness", 5, 0, 10), number("scale", 1, 0.1, 10), number("bristle_detail", 5, 0, 10),
                        boolean("lighting", true), number("angle", -60, -180, 180), number("shine", 1, 0, 10)}));
    out.push_back(kind(info("stylize_solarize", "filter.stylize.solarize", "ソラリゼーション", "表現手法", light), {}));
    {
        FilterInfo tiles = info("stylize_tiles", "filter.stylize.tiles", "タイル", "表現手法", FilterInks::Meaning);
        tiles.colours = {"foreground", "background"};
        out.push_back(kind(std::move(tiles), {whole("count", 10, 1, 99), number("max_offset", 10, 1, 99), choice("fill", {"background", "foreground", "inverse", "unaltered"}),
                                              rgb("foreground", {0, 0, 0}), rgb("background", {255, 255, 255}), seed()}));
    }
    out.push_back(kind(info("stylize_trace_contour", "filter.stylize.traceContour", "等高線", "表現手法", light),
                       {number("level", 128, 0, 255), choice("edge", {"lower", "upper"})}));
    out.push_back(kind(info("stylize_wind", "filter.stylize.wind", "風", "表現手法", light),
                       {choice("method", {"wind", "blast", "stagger"}), choice("direction", {"from_right", "from_left"}), seed()}));
    return out;
}

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
            const std::string where = "the smart object " + layer.id + "'s filter " + kind + "." + std::string(key);
            // (D2: a smart filter holds its file as the book's — never the file's data, which the op makes the book's)
            if (const Json* data = get(params, std::string(key) + "_data"); data != nullptr && !data->is_null())
                throw Error("value", where + "_data: a smart filter's file is one of the book's files (sha256:…), not its data");
            const Json* ref = get(params, key);
            if (ref == nullptr || ref->is_null()) continue;
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
