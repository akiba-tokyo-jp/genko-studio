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

// An indexed page's colour table (color-model@1, PC1d; PhotoCraft's doc/src/mode.rs ColorTable): 1..256 sRGB colours
// and the one shown as transparent, if any. Every pixel of the page's paint layers that shows (alpha > 0) is one of its
// colours, and the page is shown in them.
inline constexpr std::size_t kColorTableMax = 256;
struct ColorTable {
    std::vector<std::array<int, 3>> colors;
    std::optional<int> transparent;
    bool operator==(const ColorTable&) const = default;
    // The nearest colour's index (squared distance in 0..255; a tie: the lower index).
    std::size_t nearest(int r, int g, int b) const;
};
void check_color_table(const ColorTable& table);

// An RGB page's colour model past Python's (8 bits, sRGB): its depth (8: raster_png; 16 and 32: precise rasters,
// GKCR u16 and f32), the RGB profile its pixels are in (none: sRGB, as Python's; else a matrix-shaper RGB ICC profile,
// an asset "sha256:…"), and the intent and black point compensation colours come in with (sRGB elements, conversions).
// An indexed page is one of 8 bits in sRGB with its colour table. A page or book with the default (8 bits, sRGB,
// relative, bpc, no table) has none: its `color` is not written.
struct RgbMode {
    int depth = 8;
    std::string profile;  // ("": sRGB)
    Bytes icc;            // (the profile's bytes; null for sRGB, or until they are read or given)
    std::string intent = "relative";
    bool bpc = true;
    std::optional<ColorTable> table;  // (an indexed page's)

    bool operator==(const RgbMode& other) const {
        return depth == other.depth && profile == other.profile && intent == other.intent && bpc == other.bpc && table == other.table;
    }
    bool is_default() const { return depth == 8 && profile.empty() && intent == "relative" && bpc && !table; }
};

// {"mode": "rgb", "depth", "profile"?, "intent", "bpc"?} in that order ("profile" only for one, "bpc" only when false);
// an indexed page's {"mode": "indexed", "depth": 8, "intent", "bpc"?, "table": {"colors", "transparent"}}.
Json to_json(const RgbMode& mode);
Json to_json(const ColorTable& table);

// A page's or book's `color`, read: a mode of inks (core/ink.hpp), or an RGB mode, or neither (null). core::Error("value")
// says what is wrong — an RGB mode of every default too, which the writer never writes (no `color`).
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

// The description an ICC profile gives itself (its 'desc' tag: ICC v2 text or v4 multi-localized, English first), or
// "" when it has none this reads. Always UTF-8: a v2 text's bytes past ASCII are '?', a v4 text's lone surrogate halves
// U+FFFD.
std::string icc_description(std::string_view icc);
// The description of a mode's profile: a built-in's, the profile's own, else sRGB's or Lab's (none: Little CMS's own).
std::string profile_description(const std::string& ref, const Bytes& icc, bool lab = false);

// The book's text_gamma (core::Document): more than 1 (1 is none: Python's mixing) and at most 2.2, as Photoshop's.
inline constexpr double kTextGammaMax = 2.2;
bool is_text_gamma(double gamma);

struct Document;
struct Page;
struct Layer;
// Is the page an RGB page of a profile (color-model@1: its paint pixels are that profile's values, not sRGB)?
bool profiled_rgb(const Page& page);
// Does a layer's 8-bit picture (raster_png) hold its page's values — on an RGB page of a profile —, not sRGB? Every
// paint pixel of such a page does, whatever the layer's kind (a paint layer, the raster of a layer of lines a v2 book
// made, a tone layer's erased raster): converting, drawing, merging, lending and exporting them all ask this.
bool holds_page_values(const Page& page, const Layer& layer);

// Does the book need color-model@1 (a colour model color-mode@1 cannot hold: an RGB mode, a grey page, 32 bits, no
// black point compensation; GKIR version 2 pixels; precise rasters in a profile's space; the book's text_gamma)?
bool uses_color_model(const Document& doc);
// The book's RGB modes as they must be (core::Error("value") says what is not): a depth of 8, 16 or 32, a profile by its
// ref and the bytes held for it, a known intent; an indexed page's table, and its pixels 8 bits only (no precise
// raster). Pages not read yet are not looked at.
void validate_color_model(const Document& doc);

}  // namespace genko::core
