#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// CMYK, Lab and grey pages (color-mode@1 and color-model@1, docs/cpp-migration/schema-v4.md「色モード」「色モデル」,
// SPEC.md COLOR-02, COLOR-03): a page's colour mode (its colour space, its depth, the profile it is printed with, the
// intent and black point compensation RGB colours come in with, its table of exact inks) and the pixels of its paint
// layers in that space (GKIR: zlib with PNG's row filters).
//
// The colour work itself (Little CMS) is the drawing build's (render/ink.hpp): here are the model, its JSON and the
// pixels' codec only.

namespace genko::core {

using Bytes = std::shared_ptr<const std::string>;

inline constexpr std::string_view kColorModeFeature = "color-mode@1";
inline constexpr std::string_view kInkRasterSuffix = ".gkir";
inline constexpr std::string_view kProfileSuffix = ".icc";
inline constexpr std::size_t kProfileMaxBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kInksMax = 1024;
// The most pixels and the longest side of an ink raster (as the 8-bit ops' limits), and the most bytes its samples may
// take decoded (as the PNG reader's budget).
inline constexpr std::uint64_t kInkRasterMaxPixels = 120'000'000;
inline constexpr std::uint32_t kInkRasterMaxSide = 100'000;
inline constexpr std::uint64_t kInkRasterMaxBytes = 1024ULL * 1024 * 1024;

// The spaces of ink pixels: a page's (CMYK, Lab, Gray; color-model@1's Multichannel) and a channel's (Mask: one plane
// of density, no alpha).
enum class InkSpace { Cmyk, Lab, Gray, Multi, Mask };

// What a space's pixels are (color-model@1: what each branch on the space asks of it).
struct SpaceTraits {
    std::string_view name;   // (its mode in the JSON)
    unsigned code = 1;       // GKIR's colour space byte
    unsigned planes = 4;     // its colour planes (Multichannel: 0, the raster says how many)
    bool lightness = false;  // its samples are lightness (Gray: 0 black, 1 white), not ink
    bool hdr = false;        // a 32-bit sample may be any finite value (Gray), not 0..1
    bool alpha = true;       // a pixel has its alpha after its planes (Mask: none)
};
const SpaceTraits& traits(InkSpace space);

std::string_view to_string(InkSpace space);
// A page's ink space by its mode ("cmyk", "lab", "gray"), or none.
std::optional<InkSpace> ink_space_from(std::string_view text);
// The colour channels (CMYK 4, Lab 3, Gray 1, Mask 1; Multichannel 0: its raster's); a pixel has these and its alpha
// (a Mask none).
unsigned colour_channels(InkSpace space);

// The rendering intents (Little CMS's order: their index is the intent's number).
inline constexpr std::array<std::string_view, 4> kIntents{"perceptual", "relative", "saturation", "absolute"};
bool is_intent(std::string_view intent);

// One entry of a page's table of inks: an RGB colour (0..255 each) and the ink it is printed with (CMYK: c, m, y, k in
// 0..1; Lab: L in 0..100, a and b in -128..127; Gray: k in 0..1, its black ink as a CMYK page's K).
struct InkEntry {
    std::array<int, 3> rgb{};
    std::vector<double> ink;
    bool operator==(const InkEntry&) const = default;
};

// A page's colour mode (and the book's, for the pages it makes).
struct PageColor {
    InkSpace space = InkSpace::Cmyk;
    int depth = 8;                   // 8 | 16 | 32 (32: color-model@1)
    std::string profile;             // CMYK, Gray: the ICC profile's asset ref ("sha256:…"); Lab: none
    Bytes icc;                       // CMYK, Gray: the profile's bytes (null until they are read or given)
    std::string intent = "relative";
    bool bpc = true;                 // black point compensation when colours come in (false: color-model@1)
    std::vector<InkEntry> inks;

    // (the profile by its ref: the same bytes)
    bool operator==(const PageColor& other) const {
        return space == other.space && depth == other.depth && profile == other.profile && intent == other.intent &&
               bpc == other.bpc && inks == other.inks;
    }
    // Can color-mode@1 hold it (CMYK or Lab, 8 or 16 bits, black point compensation)? Else it is color-model@1's.
    bool color_mode_only() const { return (space == InkSpace::Cmyk || space == InkSpace::Lab) && depth != 32 && bpc; }
};

// {"mode", "depth", "profile"?, "intent", "bpc"?, "inks"} in that order ("bpc" only when false).
Json to_json(const PageColor& color);
// The JSON as the writer writes it, checked (core::Error("value") says what is wrong): a mode of inks (cmyk, lab,
// gray). The profile's bytes are not looked at (icc stays null).
PageColor page_color_from_json(const Json& value);
// Is an ink right for the space (the count of its values, their ranges, finite)?
void check_ink(InkSpace space, const std::vector<double>& ink);
// The table's entry for an RGB colour, or none.
const InkEntry* ink_for(const PageColor& color, const std::array<int, 3>& rgb);

// The asset ref of some bytes ("sha256:" and their sha256), as the asset store names them.
std::string bytes_ref(std::string_view bytes);

struct Document;
// The book's colour modes and ink rasters as they must be (core::Error("value") says what is not): an ink raster only
// on a page of a colour mode, in its space and depth, and a layer with one kind of pixels at most; a Lab page's layers
// only in the blends Lab has (lab_blend); a page with one colour model at most (inks or RGB). Pages not read yet are
// not looked at.
void validate_ink_document(const Document& doc);
// The blend modes a Lab page composites (SPEC.md COLOR-02).
bool lab_blend(std::string_view mode);

// --- GKIR: a paint layer's pixels on a CMYK, Lab or grey page, a channel's ------------------------------------------

// The pixels decoded: the samples of each pixel in a row (the colour planes, then the straight alpha — a Mask has
// none), the rows from the top. At depth 8 and 16 each sample as stored (0..255, 0..65535; CMYK: 0 is no ink; Lab: the
// ICC encoding; Gray: lightness, 0 black), at 32 each a float of the same meaning normalized (0..1; a Gray 32-bit
// sample may be any finite value: HDR). value() and set() read and write any depth as that normalized value.
struct InkPixels {
    InkSpace space = InkSpace::Cmyk;
    int depth = 8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint16_t> samples;  // (depth 8, 16)
    unsigned planes = 0;                 // (Multichannel: its planes; the other spaces have their own count)
    std::vector<float> floats;           // (depth 32)

    unsigned colours() const { return space == InkSpace::Multi ? planes : colour_channels(space); }
    bool has_alpha() const { return traits(space).alpha; }
    unsigned channels() const { return colours() + (has_alpha() ? 1 : 0); }
    // The most a stored sample is (8: 255; 16: 65535; 32-bit values are not stored as samples).
    std::uint32_t max() const { return depth == 8 ? 255u : 65535u; }
    std::size_t pixel_count() const { return std::size_t(width) * height; }
    bool is_float() const { return depth == 32; }
    // The normalized value of channel c of pixel i (the alpha is channel colours()).
    double value(std::size_t i, unsigned c) const {
        const std::size_t at = i * channels() + c;
        return depth == 32 ? static_cast<double>(floats[at]) : samples[at] / static_cast<double>(max());
    }
    // Sets it: at 8 and 16 bits rounded to the depth (clamped to 0..1), at 32 as a float (clamped to 0..1, but for a
    // Gray colour plane, which keeps any finite value).
    void set(std::size_t i, unsigned c, double v);
    // Pixel j of `from` (the same space, depth and planes) as pixel i, as it is.
    void copy_pixel(std::size_t i, const InkPixels& from, std::size_t j);
};

// A blank raster (every pixel transparent, no ink: Lab and Gray white).
InkPixels blank_ink_pixels(InkSpace space, int depth, std::uint32_t width, std::uint32_t height, unsigned planes = 0);

struct InkRasterHeader {
    InkSpace space = InkSpace::Cmyk;
    int depth = 8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    unsigned version = 1;
    unsigned planes = 4;  // (its colour planes)
};

// The header of GKIR bytes, checked (core::Error("format") when they are not an ink raster this build reads).
InkRasterHeader ink_raster_header(std::string_view bytes);
// The pixels of GKIR bytes (core::Error("format") for bytes that are not exactly a whole raster, or 32-bit samples that
// are not finite or not in their range).
InkPixels decode_ink_raster(std::string_view bytes);
// GKIR bytes of the pixels (the same pixels always give the same bytes): version 1 for CMYK and Lab at 8 and 16 bits
// (as color-mode@1 writes them), version 2 for the rest (color-model@1).
std::string encode_ink_raster(const InkPixels& pixels);

}  // namespace genko::core
