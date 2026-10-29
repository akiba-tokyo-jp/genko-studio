#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/draw.hpp"
#include "render/text/bidi.hpp"
#include "render/text/fonts.hpp"
#include "render/text/shaping.hpp"
#include "render/text/tategaki.hpp"

// PC5 G-b (SPEC.md TEXT-02「OpenType の組み方」, design section G 4.1): the OpenType typesetting's parts the lettering
// (render/text/lettering, tategaki) sets a record of type@1 with — which records it sets so, each letter's look resolved
// from the record (core::type_text's attributes: its style, Basic Paragraph, the paragraph's, the whole text's and its
// ranges'), a paragraph shaped by HarfBuzz run by run (font, size, features, axes, language, script and bidirectional level
// alike) in SheenBidi's order, and a row's glyphs drawn by their ids. The rows themselves (breaking, kinsoku, alignment,
// indents, rubies, 傍点) are the lettering's.

namespace genko::render::text {

// Is a record set by the OpenType typesetting? A record of type@1 that asks for it (native_type.shaping "opentype": a type
// layer from add_type_layer; or attributes, ranges, paragraphs or styles of its own — set_type marks it so), and — the
// coordinator's decision for G (design 3) — a line set across whose style asks for OpenType features (Pillow's BASIC
// layout drew none of its letters). A line of before is set as before.
bool sets_opentype(const core::StoryLine& line);

// One letter's look: the font file it is drawn in (Fonts::path_for of its face, or of the face its attributes name), its
// size (px), its OpenType features ({tag, value}: the record's, its ligatures, kerning and caps as features) and axes, its
// language, what it adds after its letter (tracking, px), how its glyph is drawn (its width and height against the size,
// a faux italic's lean), its baseline's shift (px, up), its colour and faux bold (the stroke's px), its lines.
struct Look {
    std::string font;
    std::int64_t size = 0;
    std::vector<std::pair<std::string, std::uint32_t>> features;
    std::vector<std::pair<std::string, float>> variations;
    std::string language;
    double tracking = 0.0;
    GlyphShape shape;
    double shift = 0.0;
    Rgb rgb;
    int bold = 0;
    bool underline = false;
    bool strikethrough = false;
    bool small_caps = false;  // (drawn as capitals at 0.7 of the size where the font has no smcp)
    bool all_caps = false;
    bool operator==(const Look&) const = default;
};

// The attributes of the record's letters (core::type_text::resolve_chars), letter by letter of `text` (code points of the
// text as set: `from` maps each to its code point in the record's text, npos: none — a line break put in), and of its
// paragraphs (resolve_para) by the text's paragraphs. `styles`: the book's type styles (none: none).
struct Resolved {
    std::vector<core::Json> chars;   // (each letter's attributes; a letter from nowhere: its neighbour's)
    std::vector<core::Json> paras;   // (each paragraph of the record's text)
    std::vector<std::size_t> para_of;  // (each letter's paragraph)
};
Resolved resolve(const core::StoryLine& line, std::u32string_view text, const std::vector<std::size_t>& from, const core::Json* styles);

// The book's type styles the lettering on this thread resolves records with (render_page sets them from the book, as it
// sets TextGamma): none outside one.
class TypeStyles {
public:
    explicit TypeStyles(const core::Json* styles);
    ~TypeStyles();
    TypeStyles(const TypeStyles&) = delete;
    TypeStyles& operator=(const TypeStyles&) = delete;
    static const core::Json* current();

private:
    const core::Json* was_;
};

// A paragraph shaped: its runs (logical order) and each letter's advance in px — a cluster's on its first letter (with its
// look's tracking and width), nothing on the others —, whether a letter begins a cluster, and its bidirectional levels.
struct ShapedRun {
    std::size_t start = 0;  // (letters of the paragraph [start, end))
    std::size_t end = 0;
    std::size_t look = 0;
    std::uint8_t level = 0;
    double scale_x = 1.0;  // (its look's width: its glyphs' advances and offsets scaled so)
    std::vector<ShapedGlyph> glyphs;  // (in the order they are drawn; clusters: letters of the paragraph)
};
struct ShapedParagraph {
    std::vector<ShapedRun> runs;
    std::vector<double> advance;
    std::vector<bool> starts;
    std::vector<double> tracking;  // (each letter's look's)
    std::vector<std::size_t> look_of;
    BidiParagraph bidi;
};
// `looks`: the looks; `look_of`: each letter's. `direction`: the paragraph's (auto, ltr, rtl).
ShapedParagraph shape_paragraph(std::u32string_view text, const std::vector<Look>& looks, const std::vector<std::size_t>& look_of,
                                std::string_view direction);

// The glyphs of the letters [start, end) of a shaped paragraph in the order they are drawn, left to right (SheenBidi's
// visual runs of the row, a right-to-left run's glyphs as HarfBuzz gave them), each with its look and its pen's x from the
// row's left (px) — the clusters' advances as the paragraph's — and, for each letter, the left and right of its cluster.
struct RowGlyph {
    std::size_t look = 0;
    std::size_t cluster = 0;  // (the letter its cluster begins at)
    GlyphPlace place;         // (x from the row's left, y the glyph's own offset: y down)
};
struct RowGlyphs {
    std::vector<RowGlyph> glyphs;
    std::vector<std::pair<double, double>> span;  // (each letter of [start, end): its cluster's left and right; a letter
                                                  // at the row's end that is not drawn: where it would be)
    double width = 0.0;
};
// `extra`: px added after each cluster but the row's last (justified rows).
RowGlyphs row_glyphs(const ShapedParagraph& paragraph, std::size_t start, std::size_t end, double extra = 0.0);

}  // namespace genko::render::text
