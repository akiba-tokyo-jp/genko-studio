#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/ruby.hpp"

// PC5 G-0 (type@1, SPEC.md TEXT-02, schema-v4.md「文字」): Photoshop's type on Genko's lines, and the type layer.
//
// A line's record (core::StoryLine) may hold `native_type`: Photoshop's settings past its style —
//   {"shape": "point" | "box",                       none: set as before it (in its box, fitted to its balloon)
//    "anchor_mm": [x, y],                            a point text's anchor
//    "antialias": none | sharp | crisp | strong | smooth | windows | windowsLcd   (smooth, the default, not written)
//    "shaping": "opentype",                          set by the OpenType typesetting (render/text/shaping)
//    "transform": [a, b, c, d, e, f],                its matrix after typesetting (mm)
//    "char_style": "cs-1", "para_style": "ps-1",     the styles the whole text takes (none: none, Basic Paragraph)
//    "char": {…}, "para": {…},                       the whole text's own character and paragraph attributes
//    "runs": [{"start", "end", "style"?, "char"?}],  ranges of graphemes [start, end), in order, none overlapping
//    "paragraphs": [{"index", "style"?, "para"?}]}   paragraphs (the text split at its line breaks) by number
// in its normal form: the keys in that order, a value given only when it is not the default, every attribute known and
// in its bounds (the tables char_attrs and para_attrs), the ranges inside the text. A type layer (core::LayerKind::Type)
// holds such a record (Layer::type: a line's record without its id, page, speaker, panel and reading order) and is drawn
// by the lines' typesetting as the layer's picture. The book's styles are Document::type_styles:
//   {"character": [{"id": "cs-1", "name", "char"}], "paragraph": [{"id": "ps-1", "name", "para", "char"?}],
//    "basic": {"para"?, "char"?}}
// (ids "cs-" / "ps-" and a number from 1, names 1..64 characters one per kind, 512 of each at most).
//
// The attributes of a range are resolved in this order (each over the one before, an attribute given only where it differs):
// the defaults → the line's style (its keys that are attributes: core::type_text::from_style) → Basic Paragraph → the
// paragraph's style → the paragraph's own attributes → (characters) the whole text's character style → its own `char` →
// the range's style → the range's own `char`. A style changed changes every text that takes it where nothing of its own
// says otherwise.
//
// Ranges follow an edit of the text (edit_line, edit_type, replace_text) as rubies do (core/ruby.hpp) in graphemes, but
// never need review: an edit before a range (an insertion just before it included) moves it; one after it (an insertion
// at its end included) leaves it; an insertion inside it widens it; what an edit takes away leaves it (a range left empty
// goes); a paragraph whose first letter an edit takes away loses its own settings.
//
// A book names type@1 while one of its lines holds native_type, one of its pages (a smart object's contents included)
// holds a type layer, or it keeps type styles; a book without any of these is written and drawn as before.

namespace genko::core {

inline constexpr std::string_view kTypeFeature = "type@1";

namespace type_text {

// What an attribute's value is.
enum class AttrKind {
    Number,    // a finite number in [lo, hi]
    Bool,      // true or false
    Choice,    // one of `choices`
    Name,      // a string of 1..hi characters (a face's name or file)
    Rgb,       // three integers 0..255
    Tags,      // {an OpenType feature's tag (4 of a-z A-Z 0-9 and space): an integer 0..65535}, 64 at most
    Axes,      // {a variable font's axis tag (4 of a-z A-Z 0-9): a number −32768..32767}, 64 at most
    Leading,   // "auto" or a number in [lo, hi]
    Language,  // a BCP 47 tag: 1..35 of a-z A-Z 0-9 and -
};

struct Attr {
    std::string_view name;
    AttrKind kind;
    double lo = 0;
    double hi = 0;
    std::span<const std::string_view> choices = {};
};

// The character attributes (mm for lengths; colours 8-bit sRGB as a line's): font, family, font_style, postscript,
// size_mm, rgb, tracking (of the em), leading_mm, baseline_shift_mm, scale_x, scale_y, faux_bold, faux_italic, underline,
// strikethrough, caps, kerning, ligatures, discretionary_ligatures, features, variations, language, proportional, tcy.
std::span<const Attr> char_attrs();
// The paragraph attributes: align, first_indent_mm, start_indent_mm, end_indent_mm, space_before_mm, space_after_mm,
// auto_leading, kinsoku, yakumono, hyphenate, direction, composer.
std::span<const Attr> para_attrs();
const Attr* char_attr(std::string_view name);
const Attr* para_attr(std::string_view name);
// Each attribute's default ({name: value}, null where it is the line's own — its face, size, colour, alignment — or
// none), in the table's order.
const Json& char_defaults();
const Json& para_defaults();
// The anti-alias methods (Photoshop's order: なし, シャープ, 鮮明, 強く, 滑らかに, Windows LCD, Windows).
std::span<const std::string_view> antialias_methods();

// The normal form of character (paragraph) attributes: each known and in its bounds, in the table's order; a value of
// null left out (an override taken away). OpError ("char size_mm must be …", "unknown char attribute x") otherwise.
Json check_char(const Json& raw, std::string_view where = "char");
Json check_para(const Json& raw, std::string_view where = "para");
// The normal form of a line's native_type for its text (null for one that holds nothing). OpError otherwise.
Json check_native_type(const Json& raw, std::string_view text);
// The normal form of the book's type styles (null for none). OpError otherwise.
Json check_type_styles(const Json& raw);
// Is it in its normal form (its keys in any order: a history snapshot sorts them)?
bool native_type_normal(const Json& value, std::string_view text);
bool type_styles_normal(const Json& value);

// The text's paragraphs (split at "\n"): how many, and the grapheme where paragraph `index` begins.
std::int64_t paragraph_count(std::string_view text);
std::int64_t paragraph_start(std::string_view text, std::int64_t index);

// What a line's style says of the attributes (font, size_mm, tracking, rgb, scale_x, tcy, features, faux_bold from bold or
// a weight of bold or heavy, faux_italic from italic; align and yakumono): {"char": {…}, "para": {…}}.
Json from_style(const Json& style);

// A range of graphemes [start, end) and the character attributes resolved there.
struct CharRun {
    std::int64_t start = 0;
    std::int64_t end = 0;
    Json attrs;
};
// The attributes of paragraph `index` of the record (every one of para_attrs: resolved), and of its characters in runs
// covering the text in order (adjacent runs differ). `styles`: the book's type styles (none: none).
Json resolve_para(const StoryLine& record, const Json* styles, std::int64_t index);
std::vector<CharRun> resolve_chars(const StoryLine& record, const Json* styles);

// One edit of a text in graphemes: [start, end) of the text before replaced by `inserted` graphemes.
struct Edit {
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::int64_t inserted = 0;
};
// The edit that made `after` of `before` (their common start and end, in graphemes).
Edit edit_between(std::string_view before, std::string_view after);
// A native_type's runs and paragraphs through edits of its text (`before` → `after`; the edits left to right, none
// overlapping, in the graphemes of `before`). Null when nothing is left in it.
Json follow(const Json& native_type, std::string_view before, std::string_view after, const std::vector<Edit>& edits);
// replace_text's code point replacements as edits in graphemes.
std::vector<Edit> edits_of(std::string_view before, const std::vector<ruby::Replacement>& replacements);

// A line's style_runs ([[words, {scale, bold, weight, rgb, tcy}]], found by their words) as ranges of the text: the runs
// of native_type they become (each word found from where the last one was, as the lettering finds them; one not found
// left out). For when a range of the line is first given attributes of its own.
Json runs_from_style_runs(const StoryLine& line);

// A line's record as a type layer holds it (its id, page, speaker, panel and reading order taken away; the rest kept).
StoryLine record_of(const StoryLine& line);
// A type layer's record as a line of `page` to draw or measure (its id the layer's).
StoryLine line_of(const Layer& layer, const Num& page_index);
// The record moved by (dx, dy) mm: its box, point text's anchor, tails and hand-drawn balloon.
StoryLine moved(const StoryLine& record, double dx, double dy);
// The record shared as Layer::type holds it.
std::shared_ptr<const StoryLine> shared_record(StoryLine record);

// Is the layer a type layer (its kind; its record held)?
bool is_type_layer(const Layer& layer);

// The book uses type@1 (above). After an op: its features given type@1 when it does (never taken away here: the writer does).
bool uses_type(const Document& doc);
void add_feature(Document& doc);
// What every op leaves: a native_type in its normal form for its text, a type layer with its record and nothing of its own
// drawn (no strokes, pixels or shape parts), the styles named kept by the book and in their normal form. core::Error("value").
void validate_type(const Document& doc);

}  // namespace type_text
}  // namespace genko::core
