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

// CMYK and Lab pages (color-mode@1, docs/cpp-migration/schema-v4.md「色モード」, SPEC.md COLOR-02): a page's colour
// mode (its colour space, its depth, the CMYK profile it is printed with, the intent RGB colours come in with, its
// table of exact inks) and the pixels of its paint layers in that space (GKIR: zlib with PNG's row filters).
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

enum class InkSpace { Cmyk, Lab };

std::string_view to_string(InkSpace space);
std::optional<InkSpace> ink_space_from(std::string_view text);
// The colour channels (CMYK 4, Lab 3); a pixel has these and its alpha.
unsigned colour_channels(InkSpace space);

// The rendering intents (Little CMS's order: their index is the intent's number).
inline constexpr std::array<std::string_view, 4> kIntents{"perceptual", "relative", "saturation", "absolute"};
bool is_intent(std::string_view intent);

// One entry of a page's table of inks: an RGB colour (0..255 each) and the ink it is printed with (CMYK: c, m, y, k in
// 0..1; Lab: L in 0..100, a and b in -128..127).
struct InkEntry {
    std::array<int, 3> rgb{};
    std::vector<double> ink;
    bool operator==(const InkEntry&) const = default;
};

// A page's colour mode (and the book's, for the pages it makes).
struct PageColor {
    InkSpace space = InkSpace::Cmyk;
    int depth = 8;                   // 8 | 16
    std::string profile;             // CMYK: the ICC profile's asset ref ("sha256:…"); Lab: none
    Bytes icc;                       // CMYK: the profile's bytes (null until they are read or given)
    std::string intent = "relative";
    std::vector<InkEntry> inks;

    // (the profile by its ref: the same bytes)
    bool operator==(const PageColor& other) const {
        return space == other.space && depth == other.depth && profile == other.profile && intent == other.intent &&
               inks == other.inks;
    }
};

// {"mode", "depth", "profile"?, "intent", "inks"} in that order.
Json to_json(const PageColor& color);
// The JSON as the writer writes it, checked (core::Error("value") says what is wrong). The profile's bytes are not
// looked at (icc stays null).
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
// only in the blends Lab has (lab_blend). Pages not read yet are not looked at.
void validate_ink_document(const Document& doc);
// The blend modes a Lab page composites (SPEC.md COLOR-02).
bool lab_blend(std::string_view mode);

// --- GKIR: a paint layer's pixels on a CMYK or Lab page ------------------------------------------------------------

// The pixels decoded: the samples of each pixel in a row (the colour channels, then the straight alpha), the rows from
// the top, each sample as stored (0..255 at depth 8, 0..65535 at 16; CMYK: 0 is no ink; Lab: the ICC encoding).
struct InkPixels {
    InkSpace space = InkSpace::Cmyk;
    int depth = 8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint16_t> samples;

    unsigned channels() const { return colour_channels(space) + 1; }
    std::uint32_t max() const { return depth == 8 ? 255u : 65535u; }
    std::size_t pixel_count() const { return std::size_t(width) * height; }
};

// A blank raster (every pixel transparent, no ink: Lab white).
InkPixels blank_ink_pixels(InkSpace space, int depth, std::uint32_t width, std::uint32_t height);

struct InkRasterHeader {
    InkSpace space = InkSpace::Cmyk;
    int depth = 8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// The header of GKIR bytes, checked (core::Error("format") when they are not an ink raster this build reads).
InkRasterHeader ink_raster_header(std::string_view bytes);
// The pixels of GKIR bytes (core::Error("format") for bytes that are not exactly a whole raster).
InkPixels decode_ink_raster(std::string_view bytes);
// GKIR bytes of the pixels (the same pixels always give the same bytes).
std::string encode_ink_raster(const InkPixels& pixels);

}  // namespace genko::core
