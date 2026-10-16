#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ink.hpp"
#include "core/json.hpp"

// color-model@1 (docs/cpp-migration/schema-v4.md「色モデル」, SPEC.md COLOR-03): the colour model of a page past
// color-mode@1 — RGB pages of another profile or depth, grey pages, 32-bit pixels — and the ICC profiles the C++ build
// carries itself.

namespace genko::core {

inline constexpr std::string_view kColorModelFeature = "color-model@1";

// An RGB page's colour model past Python's (8 bits, sRGB): its depth (8: raster_png; 16 and 32: precise rasters,
// GKCR u16 and f32), the RGB profile its pixels are in (none: sRGB, as Python's; else a matrix-shaper RGB ICC profile,
// an asset "sha256:…"), and the intent and black point compensation colours come in with (sRGB elements, conversions).
// A page or book with the default (8 bits, sRGB, relative, bpc) has none: its `color` is not written.
struct RgbMode {
    int depth = 8;
    std::string profile;  // ("": sRGB)
    Bytes icc;            // (the profile's bytes; null for sRGB, or until they are read or given)
    std::string intent = "relative";
    bool bpc = true;

    bool operator==(const RgbMode& other) const {
        return depth == other.depth && profile == other.profile && intent == other.intent && bpc == other.bpc;
    }
    bool is_default() const { return depth == 8 && profile.empty() && intent == "relative" && bpc; }
};

// {"mode": "rgb", "depth", "profile"?, "intent", "bpc"?} in that order ("profile" only for one, "bpc" only when false).
Json to_json(const RgbMode& mode);

// A page's or book's `color`, read: a mode of inks (core/ink.hpp), or an RGB mode, or neither (null; an RGB mode with
// every default). core::Error("value") says what is wrong.
struct ColorModel {
    std::optional<PageColor> inks;
    std::optional<RgbMode> rgb;
};
ColorModel color_model_from_json(const Json& value);

// The precise rasters' "space" (GKCR metadata) on a page of this RGB mode: "srgb", or its profile's ref.
std::string color_raster_space(const std::optional<RgbMode>& rgb);

// The colour space an ICC profile is for.
enum class ProfileSpace { Rgb, Gray, Cmyk, Lab };

std::string_view to_string(ProfileSpace space);

// A built-in profile (PhotoCraft v0.2.0's ids, aliases and descriptions: crates/cms/src/builtin.rs:18-102). srgb is
// Little CMS's own sRGB (a page with no profile) and lab-d50 Little CMS's D50 Lab v4: they carry no bytes. The others'
// bytes are built into the program (data/profiles/<id>.icc) and named by their sha256.
struct BuiltinProfile {
    std::string_view id;
    std::string_view description;
    ProfileSpace space = ProfileSpace::Rgb;
    std::string_view sha256;  // (of the bytes; "" for srgb and lab-d50)
    Bytes bytes;              // (null for srgb and lab-d50)

    // The bytes' asset ref ("sha256:…"), or "" for one with no bytes.
    std::string ref() const { return sha256.empty() ? std::string() : "sha256:" + std::string(sha256); }
};

// Every built-in profile, in PhotoCraft's order (srgb, display-p3, adobe-rgb-compat, prophoto-compat, linear-srgb,
// rec2020, gray-gamma-2.2, sgray, lab-d50, coated-cmyk).
const std::vector<BuiltinProfile>& builtin_profiles();
// A built-in profile by its id (any case; PhotoCraft's aliases, its description), or null.
const BuiltinProfile* builtin_profile(std::string_view name);
// The built-in profile whose bytes these are (by their asset ref), or null.
const BuiltinProfile* builtin_profile_by_ref(std::string_view ref);

struct Document;
// Does the book need color-model@1 (a colour model color-mode@1 cannot hold: an RGB mode, a grey page, 32 bits, no
// black point compensation; GKIR version 2 pixels; precise rasters in a profile's space)?
bool uses_color_model(const Document& doc);
// The book's RGB modes as they must be (core::Error("value") says what is not): a depth of 8, 16 or 32, a profile by its
// ref and the bytes held for it, a known intent. Pages not read yet are not looked at.
void validate_color_model(const Document& doc);

}  // namespace genko::core
