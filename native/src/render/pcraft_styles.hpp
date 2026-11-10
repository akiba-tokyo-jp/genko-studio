#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>

#include "core/json.hpp"
#include "render/pcraft_manifest.hpp"

// PC6 (layer-styles@1 in a .pcraft, SPEC.md FILE-01 and COLOR-09「レイヤースタイル」): a layer's style — its effects, fill
// opacity and Blend If (core/layer_styles.hpp) — as PhotoCraft v0.2.0's layer has them in a .pcraft's manifest (crates/
// format/src/manifest.rs LayerM: "effects" {"enabled", "items", "psd_raw", "reference"}, "fill_opacity", "blend_if"
// {"ranges"}; the effects crates/doc/src/effects.rs's enum Effect in serde's form, Blend If blend_if.rs's), both ways. The
// ten kinds are the same ten; upstream's lengths are the document's pixels, Genko's mm.
//
// What a style of Genko's cannot hold of upstream's is left out and said (`say`, in Japanese): a glow painted with a
// pattern, an effect of a pattern the document does not hold, a blend mode Genko has not (its effect's: 通常), a bevel's
// highlight or shadow switched off (its opacity 0 then), the effects' reference point and the PSD's original effects
// block. Values past Genko's bounds are brought into them (a length to 100 mm). What upstream cannot hold of Genko's — a
// stroke's pattern's angle and place — is said when written.

namespace genko::render::pcraft {

struct StyleRead {
    double mm = 25.4 / 72;  // mm a document pixel
    Mode mode = Mode::Rgb;  // the document's (Blend If's channels)
    // an upstream colour as Genko's [r, g, b] (0..255)
    std::function<core::Json(const core::Json& color)> rgb;
    // a pattern of the document (upstream's id and name, its scale as a fraction, angle, link and phase in pixels) as a
    // pattern fill of the book's (core::pattern_fill_spec's raw form); none when the document holds no such pattern
    std::function<std::optional<core::Json>(const std::string& id, const std::string& name, double scale, double angle, bool link,
                                            const std::array<double, 2>& phase)>
        pattern;
    std::function<void(const std::string&)> say;
    // (the caller keeps the effects' psd_raw — a layer's, as its PSD block —: not said)
    bool psd_raw_kept = false;
};

// The style an upstream layer (LayerM) holds — its effects, fill opacity and Blend If — as Genko's style in its normal
// form (core::layer_style_spec), null when it holds none. Pre: `layer` is an object.
core::Json style_from_upstream(const core::Json& layer, const StyleRead& in);
// Does the layer hold effects (switched on or not), Blend If or a fill opacity below 1?
bool has_upstream_effects(const core::Json& layer);
bool has_upstream_blend_if(const core::Json& layer);

struct StyleWrite {
    double px = 72 / 25.4;  // document pixels a mm
    Mode mode = Mode::Rgb;
    // a pattern fill of the book's as upstream's {"name", "id", "scale" (a fraction), "angle", "link", "phase" [x, y]
    // pixels}; none when the book holds no such pattern
    std::function<std::optional<core::Json>(const core::Json& fill)> pattern;
    std::function<void(const std::string&)> say;
};

// A style's effects as upstream's EffectsM ({"enabled", "items", "psd_raw": null, "reference": null}); its Blend If as
// upstream's BlendIf ({"ranges": …}; null when it hides nothing).
core::Json effects_to_upstream(const core::Json& style, const StyleWrite& out);
core::Json blend_if_to_upstream(const core::Json& style, const StyleWrite& out);

}  // namespace genko::render::pcraft
