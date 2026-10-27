#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "render/psd_shapes.hpp"

// PC4 E6 (layer-styles@1, SPEC.md COLOR-09「レイヤースタイル」): a layer's style in a PSD — PhotoCraft v0.2.0's
// crates/io/src/effects_map.rs (the object-based effects descriptor lfx2: each effect's keys, the "…Multi" lists of
// several of a kind, the contours, the blend modes' ids), MIT: native/third_party/photocraft/NOTICE.md; the same descriptor
// is .asl's (render/asl). Lengths are pixels of the canvas (its resolution), the book's mm; colours RGB (a file's CMYK,
// Lab, grey or HSB colour read as the sRGB it means); patterns the book's (render/psd_shapes' pattern items). What the
// file holds that the style does not keep (an effect of a kind none has, a blend mode Genko has not) is said in `notes`.

namespace genko::render::psd_effects {

// The payload of an lfx2 block (object effects version 0, descriptor version 16) of the style's effects and its master
// switch ("enabled"); "" for a style of no effects.
std::string lfx2(const core::Json& style, const psd_shapes::Canvas& canvas);
// The effects descriptor of the style (an .asl style's "Lefx" too).
psd::descriptor::Writer effects_descriptor(const core::Json& style, const psd_shapes::Canvas& canvas);

// An lfx2's or lmfx's payload read back: {"effects": [...], "enabled"?: false} in its normal form (core::layer_style_spec;
// none when it holds no effect). Broken, the payload ends early or is too large to read: none, said.
std::optional<core::Json> style_of(std::string_view payload, const psd_shapes::Placement& placement, std::vector<std::string>& notes);
// The same from an effects descriptor (an .asl style's "Lefx").
core::Json effects_of(const psd::descriptor::Object& effects, const psd_shapes::Placement& placement, std::vector<std::string>& notes);

// iOpa's payload of a fill opacity (0..1: one byte and three of padding), and back (none: not one).
std::string iopa(double fill);
std::optional<double> fill_of_iopa(std::string_view payload);

// Blend If as a layer record's blending ranges for a file whose colour channels are `names` (the composite grey's pair,
// then each channel's: this layer's range then the underlying's, 4 bytes each — black low, black high, white low, white
// high); and the ranges read back as a style's "blend_if" (an empty list for the defaults).
std::string ranges_of(const core::Json& blend_if, const std::vector<std::string>& names);
core::Json blend_if_of(std::string_view ranges, const std::vector<std::string>& names);

// The global light's image resources: 1037 (its angle) and 1049 (its altitude), 4-byte integers, and back.
std::string light_resource(double degrees);
std::optional<double> light_of_resource(std::string_view payload);

// The blend mode's id in a descriptor (BlnM), and back (none: one Genko has not).
std::string blend_id(std::string_view mode);
std::optional<std::string> blend_of_id(std::string_view id);

}  // namespace genko::render::psd_effects
