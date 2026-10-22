#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/adjust_kinds.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// PC4 節 D (SPEC.md COLOR-08「フィルター」): filter_raster's kinds of Photoshop's フィルター menu — PhotoCraft v0.2.0's
// filters (algo/src/lib.rs FilterParams, engine/src/filters.rs params_for), named "<submenu>_<filter>" in snake_case
// (blur_gaussian, distort_twirl…: never one of Python's 22) —, in the correction kinds' table (core/adjust_kinds.hpp:
// group "PC4", filter, local, smart) with a side table of what the rest of the build needs of them (FilterInfo).
//
// Each group of PC4 (D0…D6) adds its kinds to its own function below (one submenu each); the table, the drawing
// (render/fx: one formula, the three ways), the windows (app/filter_forms) and the フィルター menu (app/filter_menu) take
// them from there. A kind not in the table yet is not a kind: filter_raster refuses it as an unknown filter, and its
// menu item stays grey.
//
// Lengths in a kind's settings are pixels of the working 200 dpi (scaled to a layer's resolution when drawn), places in
// the page's mm or shares of the base box, colours [r, g, b] 0..255 (design 2.2).

namespace genko::core {

// A book whose smart filters use a kind of PC4 (or hold a filter's file) names filters@1 (schema-v4.md §3).
inline constexpr std::string_view kFiltersFeature = "filters@1";

// How a kind is laid on a page of inks (CMYK, Lab, grey, multichannel; design 4.1).
enum class FilterInks {
    Plates,     // on each plate's canvas value as it is, alpha-weighted (blurs, sharpening, noise, distortions, cells…)
    Lightness,  // on the lightness: CMYK and multichannel plates as 1 − ink, back after; a Lab page's L alone (its a and b as
                // they were); grey as it is (maximum, emboss, edges…)
    LightnessAndColour,  // (D1) CMYK and multichannel plates as 1 − ink, back after; a Lab page's L, a and b each (the kernel
                         // told it is Lab: a and b about their 0, as RGB's colours about their grey); grey as it is — a kernel
                         // whose result is about a middle (ハイパス, カスタム), or (D2) that keeps or greys a Lab page's a and b
                         // itself while it moves, shades or traces the lightness (表現手法: エンボス, 輪郭検出, 押し出し, 風…)
    Meaning,    // on the colours the inks mean, back by the page's rules (the gallery, HSB/HSL, Camera Raw…)
    Halftone,   // its own screen on each plate (カラーハーフトーン, D2's メゾティント): each plate's canvas value as it is, a Lab
                // page's — no plates — the colours it means
};
std::string_view to_string(FilterInks inks);

// (D2) 置き換え's map given as a file's data (map_data, base64): at most 32 MiB of a PNG or PSD file.
inline constexpr std::size_t kDisplacementMapMaxBytes = 32u * 1024 * 1024;
// (D2) シアー's curve: at most this many points.
inline constexpr std::size_t kShearMostPoints = 64;

// A setting that names a place on the picture (design 2.2, 3.3): moved and scaled with a smart object (its "space":
// "contents"), as PC3's bending filters' points.
struct FilterPlace {
    enum class Kind {
        Centre,  // `key` and `key2`: the x and y of a place as shares 0..1 of the base box (a selection's box, else the grid)
        Point,   // `key`: [x, y] in the page's mm
        Points,  // `key`: [[x, y], …] in the page's mm
        Length,  // `key`: a length in the page's mm (scaled as the smart object scales there)
    };
    Kind kind = Kind::Point;
    std::string_view key;
    std::string_view key2;
    // A list of objects each holding the place (pins, lights, constraints); empty: the settings themselves.
    std::string_view within;
};

// What the build needs of a kind besides its settings.
struct FilterInfo {
    std::string_view kind;
    std::string_view upstream;  // PhotoCraft's command id ("filter.blur.gaussianBlur": .pcraft, the catalogue)
    std::string_view name;      // Photoshop's name of its command (Japanese; its window's title)
    std::string_view submenu;   // the フィルター submenu it is in (Photoshop's name; empty: the menu itself)
    std::vector<FilterPlace> places;
    std::vector<std::string_view> colours;  // keys the window fills with the main and sub colours (描画色・背景色)
    std::vector<std::string_view> files;    // keys holding a file of the book's (an asset ref; its suffix at "<key>_suffix")
    FilterInks inks = FilterInks::Plates;
};

// A kind of PC4: its settings (an AdjustKind of group "PC4") and its side.
struct FilterKind {
    AdjustKind kind;
    FilterInfo info;
};

// Each group's kinds (core/filter_kinds.cpp, one function a submenu; a group fills its own).
std::vector<FilterKind> blur_kinds();          // D0: blur_gaussian, blur_box; D1: the rest of ぼかし
std::vector<FilterKind> sharpen_kinds();       // D1
std::vector<FilterKind> noise_kinds();         // D1
std::vector<FilterKind> other_kinds();         // D1
std::vector<FilterKind> video_kinds();         // D1
std::vector<FilterKind> distort_kinds();       // D2
std::vector<FilterKind> pixelate_kinds();      // D2
std::vector<FilterKind> stylize_kinds();       // D2
std::vector<FilterKind> gallery_kinds();       // D3: filter_gallery
std::vector<FilterKind> render_kinds();        // D4: 描画
std::vector<FilterKind> blur_gallery_kinds();  // D4: ぼかしギャラリー
std::vector<FilterKind> camera_raw_kinds();    // D5: camera_raw
std::vector<FilterKind> lens_kinds();          // D6: lens_correction, adaptive_wide_angle

// Every PC4 kind as the correction kinds' table holds them (core/adjust_kinds.cpp's make_table adds them after PC2's).
std::vector<AdjustKind> filter_kinds();
// The side of every PC4 kind, in the table's order; one kind's (none: not a PC4 kind).
const std::vector<FilterInfo>& filter_infos();
const FilterInfo* filter_info(std::string_view kind);
bool pc4_filter(std::string_view kind);

// フィルター's commands without a window (design 2.1, PhotoCraft's filters.rs:45-50): a kind with fixed settings —
// ぼかし (blur_gaussian 0.6), ぼかし（強） (1.4), シャープ・シャープ（強）・シャープ（輪郭のみ）, 輪郭以外をぼかす. Each its
// upstream command id, Photoshop's name, the submenu, the kind and its settings (a kind not in the table yet: listed, not
// usable).
struct FilterPreset {
    std::string_view upstream;
    std::string_view name;
    std::string_view submenu;
    std::string_view kind;
    Json params;
};
const std::vector<FilterPreset>& filter_presets();

// D7 (.pcraft's smart filters, design 2.9): an upstream command and its params (Photoshop's units: the document's
// pixels at `dpi`, camelCase keys, colours 0..1) as Genko's kind and settings (lengths in working pixels, snake_case),
// and back. None for a command or kind not in the table. (D0: the blurs of its own; D7 the rest.)
std::optional<std::pair<std::string, Json>> from_upstream(std::string_view command, const Json& params, double dpi);
std::optional<std::pair<std::string, Json>> to_upstream(std::string_view kind, const Json& params, double dpi);

// filters@1 (design 2.8): does a smart filter of the book (on its pages or in its contents) use a PC4 kind, or hold a
// filter's file?
bool uses_filters(const Document& doc);
// The files its smart filters hold (each its asset ref and suffix), in the order met, each once.
std::vector<std::pair<std::string, std::string>> filter_file_refs(const Document& doc);
// Its smart filters' files well formed (an asset ref, a known suffix — never a layer named nor the file's data: D2) and
// held: by the book (Document::filter_files) or its folder (`held`: does the folder hold this ref with this suffix; none:
// not asked). core::Error("value") says what is not.
void validate_filters(const Document& doc, const std::function<bool(const std::string&, const std::string&)>& held = {});

// The catalogue of genko inspect --filters (design 3.2): each PC4 kind — its upstream id, Photoshop's name, submenu,
// settings (type, default, range, choices), whether it can be a smart filter, how it is laid on a page of inks — and the
// presets.
Json filter_catalogue();

}  // namespace genko::core
