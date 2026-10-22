#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// patterns@1 (PC4 E-0, schema-v4.md「パターン」, SPEC.md COLOR-09): the patterns a book keeps (Document::patterns, the
// struct core::Pattern in core/model.hpp) — each the copy of a pattern its fills, shape parts and overlays use (E3, E6;
// the library is the machine's). A book that keeps none is written as before (no "patterns", no feature). The ops that
// add, rename and delete them are E3's (render/ops_patterns.cpp).

namespace genko::core {

inline constexpr std::string_view kPatternsFeature = "patterns@1";
inline constexpr std::size_t kMaxPatterns = 512;
inline constexpr std::int64_t kMaxPatternSide = 8192;
inline constexpr std::int64_t kMaxPatternPixels = 64LL * 1000 * 1000;

// A pattern as the book writes it: {"id", "name", "asset", "mode", "depth", "width", "height", "dpi"} (all of them, in
// this order).
Json pattern_to_json(const Pattern& pattern);
// One from that form: core::Error("value", "pattern …") for a key it does not know, a missing or wrong value (an id of
// other than 1..64 of A-Z a-z 0-9 - _, a name of other than 1..255 characters, not an asset ref, a mode or a depth not
// named above, a size past 8192 a side or 64 M pixels, a dpi past 50..2400).
Pattern pattern_from_json(const Json& data);

// The suffix its picture is kept with: ".png" (8-bit RGB: RGBA; 8-bit grey: LA), core::kColorRasterSuffix (16- and
// 32-bit RGB: GKCR), core::kInkRasterSuffix (grey of more bits, CMYK and Lab: GKIR version 2).
std::string_view pattern_suffix(const Pattern& pattern);
// Whether the picture's bytes are the pattern's (their kind, size and depth, read from their header): none when they
// are, else why not.
std::optional<std::string> check_pattern_file(const Pattern& pattern, std::string_view bytes);

bool uses_patterns(const Document& doc);
// The book's patterns checked (at most 512; the ids each once; each one's picture held and as check_pattern_file
// says). core::Error("value") says what is wrong.
void validate_patterns(const Document& doc);
// The pattern of that id, or none.
const Pattern* find_pattern(const Document& doc, std::string_view id);

}  // namespace genko::core
