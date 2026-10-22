#pragma once

#include <optional>
#include <set>
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
// says; E3: every pattern fill in its normal form and naming a pattern the book keeps). core::Error("value") says what
// is wrong.
void validate_patterns(const Document& doc);
// The pattern of that id, or none.
const Pattern* find_pattern(const Document& doc, std::string_view id);

// --- pattern fills (PC4 E3) -------------------------------------------------------------------------------------------

inline constexpr double kPatternScaleMin = 1.0, kPatternScaleMax = 1000.0;  // (%)
inline constexpr double kPatternPhaseMax = 100000.0;                         // (mm, either way)
// A pattern fill — a fill layer's {"pattern": …}, a shape part's fill's or stroke's, an op's — checked, in its normal
// form {"id", "scale", "angle", "phase_mm", "link"}: the pattern's id, its scale 1..1000 % (100), its angle −360..360°
// counter-clockwise (0), where it starts on the page [x, y] mm (±100000, [0, 0]), whether it moves with its layer
// (true). core::Error("value") says what is wrong.
Json pattern_fill_spec(const Json& raw);
// Is a pattern fill in its normal form (pattern_fill_spec gives it back), its keys in any order — a shapes blob is
// written with its keys sorted? core::Error("value") as pattern_fill_spec for one not well formed.
bool pattern_fill_normal(const Json& fill);
// Is a fill ({"rgb"}, {"gradient"}, {"pattern"} …) a pattern's? Its pattern fill, or null.
const Json* pattern_of_fill(const Json& fill);
// The ids of the patterns the book's fills name — its fill layers', its shape parts' fills and strokes' — on its pages
// and its smart objects' contents (core::smart::written_pages).
std::set<std::string> patterns_used(const Document& doc);
// The same on one page (its fill layers' and shape parts').
std::set<std::string> patterns_on(const Page& page);
// The next free id of a pattern ("pat-1", "pat-2"…).
std::string next_pattern_id(const Document& doc);

}  // namespace genko::core
