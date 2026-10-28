#pragma once

#include <array>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "core/json.hpp"
#include "core/model.hpp"

// PC4 E6 (layer-styles@1, SPEC.md COLOR-09「レイヤースタイル」, schema-v4.md「レイヤースタイル」): a layer's style —
// Photoshop's layer effects (PhotoCraft v0.2.0's crates/doc/src/effects.rs, blend_if.rs and engine/src/layer_style.rs, MIT:
// native/third_party/photocraft/NOTICE.md) — kept on the layer as `style` and the book's global light (包括光源) as
// `global_light`, in mm (Genko's pages are in mm; Photoshop's sizes are pixels of one resolution), with what a layer's
// blending options add: its fill opacity (塗り) and Blend If (ブレンド条件).
//
// A style in its normal form (what layer_style_spec gives and the book writes):
//   {"effects": [effect, …], "enabled"?: false, "fill_opacity"?: 0..1, "blend_if"?: [{"channel", "this", "under"}, …]}
// — "enabled" (the effects shown: すべての効果を隠す) written only when false, "fill_opacity" only when not 1, "blend_if"
// only when a range hides something. The effects in Photoshop's order of the list (the first listed drawn on top of the
// others of its kind); each {"kind", "enabled", "blend", "opacity", …its kind's settings (every one written)}:
//   drop_shadow / inner_shadow: rgb, angle, global_light, distance_mm, spread (inner: choke), size_mm, noise, anti_alias,
//     contour?, knocks_out (drop shadow only)
//   outer_glow / inner_glow: rgb | gradient, technique (softer | precise), spread (inner: choke), size_mm, range, jitter,
//     noise, anti_alias, contour?, source (inner glow only: edge | center)
//   bevel: style (inner | outer | emboss | pillow | stroke), technique (smooth | chisel_hard | chisel_soft), depth (0.01..10),
//     direction (up | down), size_mm, soften_mm, angle, altitude, global_light, highlight {blend, opacity, rgb}, shadow
//     {blend, opacity, rgb}, gloss_contour?, contour? {curve?, range, anti_alias}, texture? {pattern, depth (−10..10), invert}
//   satin: rgb, angle, distance_mm, size_mm, invert, anti_alias, contour?
//   color_overlay: rgb;  gradient_overlay: gradient, dither;  pattern_overlay: pattern
//   stroke: size_mm, position (outside | inside | center), rgb | gradient | pattern
// A colour is [r, g, b] (sRGB 0..255: on a page of inks its inks by the page's rules); a gradient {"stops": [[place 0..1,
// [r, g, b]], …] (2..16), "transparency"?: [[place, opacity], …] (2..16), "style" (linear | radial | angle | reflected |
// diamond), "angle", "scale" (0.1..1.5), "reverse", "align", "offset" [x, y] (−1..1 of the box)}; a pattern a pattern fill
// of the book's patterns (core::pattern_fill_spec's form: its uses go through core::each_pattern_fill_on); a contour (a curve
// of the effect's coverage) {"name", "points": [[in, out], …] (2..32, 0..1)} — absent: linear. Lengths 0..100 mm; angles
// −360..360°, altitude 0..90°; opacities, spread, choke, range, jitter, noise 0..1. Noise, jitter and anti_alias are kept
// (the PSD's), not drawn.
//
// Blend If: per channel ("gray", then the page's: red, green, blue | cyan, magenta, yellow, black | lightness, a, b) the
// layer's own value's range ("this") and the value beneath's ("under"), each [black low, black high, white low, white high]
// on 0..255 (Photoshop's slider pairs, split for a soft edge); applied on RGB and grey pages (kept on CMYK and Lab ones).
//
// A book names layer-styles@1 while one of its layers has a style or it sets its global light; a book without either is
// written as before. The kinds of layer that take a style: paint, pen, fill, placed pictures and smart objects (not a
// folder — Genko's folders are not drawn as one —, a correction or a tone layer).

namespace genko::core {

inline constexpr std::string_view kLayerStylesFeature = "layer-styles@1";
// The effects' kinds in the order Photoshop's レイヤースタイル window lists them.
inline constexpr std::array<std::string_view, 10> kEffectKinds{"bevel",         "stroke",           "inner_shadow",    "inner_glow", "satin",
                                                                "color_overlay", "gradient_overlay", "pattern_overlay", "outer_glow", "drop_shadow"};
inline constexpr double kEffectMaxMm = 100.0;  // a size or distance
inline constexpr std::size_t kMaxEffects = 64;  // in one style (Photoshop: ten of a kind)
inline constexpr double kDefaultLightAngle = 120.0, kDefaultLightAltitude = 30.0;

// A style checked and in its normal form; null when it holds nothing (no effect, fill 100 %, no Blend If). core::Error
// ("value") says what is wrong ("layer style …").
Json layer_style_spec(const Json& raw);
// One effect ({"kind", …}) in its normal form: `raw`'s settings over `base`'s (an effect of the same kind) or its kind's
// defaults (Photoshop's, sizes in mm). core::Error("value").
Json layer_effect_spec(const Json& raw, const Json* base = nullptr);
// The normal form of a kind's effect with its defaults.
Json default_effect(std::string_view kind);
// Is a style in its normal form (layer_style_spec gives it back), its keys in any order — the history's snapshots are
// written with their keys sorted?
bool layer_style_normal(const Json& style);
// The same values, objects' keys in any order.
bool same_json_unordered(const Json& a, const Json& b);

// Does the layer have a style? Does its kind take one (paint, pen, fill, placed, smart)?
bool has_layer_style(const Layer& layer);
bool takes_layer_style(const Layer& layer);
// Does the style show effects (switched on and holding one switched on)?
bool style_shows_effects(const Json& style);
double style_fill_opacity(const Json& style);

// The book's global light: {angle, altitude} (degrees).
std::array<double, 2> global_light(const Document& doc);
// {"angle", "altitude"} checked (angle −180..180 after turning into it, altitude 0..90) → [angle, altitude]. core::Error.
std::array<double, 2> global_light_of(const Json& raw);
// The global light as the book writes it: {"angle", "altitude"}.
Json global_light_json(const std::array<double, 2>& light);

bool uses_layer_styles(const Document& doc);
// Every layer's style in its normal form on a layer that takes one, and the global light in its bounds (core::Error
// "value" says which). Pages not read yet are not looked at.
void validate_layer_styles(const Document& doc);

// Each pattern a style names (a pattern overlay's, a stroke's or a glow's paint, a bevel's texture) with where it is (for
// core::each_pattern_fill_on), and the same with the fills changed (`change` gives the fill again or another).
void each_style_pattern(const Json& style, const std::function<void(const Json& fill, const std::string& where)>& each, const std::string& where);
bool repoint_style_patterns(Json& style, const std::map<std::string, std::string>& ids);
// The same walk with each pattern fill given to `change` to change in place (render/style_presets: a preset's patterns,
// named by the library's).
void change_style_patterns(Json& style, const std::function<void(Json& fill)>& change);

// The style with its lengths (sizes, distances, soften) times `factor` (効果を拡大・縮小: 0.01..10), each kept to its bounds.
Json scaled_layer_style(const Json& style, double factor);
// The effects' reach beyond the layer's shape (mm): its largest distance + size (and soften), 0 when it shows none.
double style_reach_mm(const Json& style);

}  // namespace genko::core
