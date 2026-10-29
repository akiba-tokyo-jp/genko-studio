#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// PC5 G-0 (type@1, SPEC.md TEXT-02「OpenType の組み方」): the OpenType typesetting's way in — HarfBuzz choosing a font's
// glyphs and their advances for a run of text (its features, variable axes, language and direction). The lettering
// (render/text/lettering, tategaki) sets each letter alone as Pillow's BASIC layout does and keeps doing so for every line
// of before; G-b sets a record of type@1 (core/type_text.hpp: a shape, new attributes or styles) through this: across,
// kerning, ligatures and the other features, the axes and the language; down, its own grid of cells (columns, kinsoku,
// tate-chu-yoko, rubies) with the glyphs HarfBuzz chooses for each cell (vert, vrt2, vpal, jp78 …). Its glyphs are drawn by
// their ids (render/draw), never by their characters.
//
// The font is one TrueTypeFonts opens (a bundled font's name, or a font file's path: render::font_bytes), read once and its
// HarfBuzz face kept for the process. HarfBuzz's version is the system's on Linux, vcpkg's on Windows: their glyphs may
// differ a little (the tests compare ids and advances within a tolerance, never pixels across systems).

namespace genko::render::text {

// One glyph HarfBuzz chose: its id in the font, the code point (of the run's text) its cluster begins at, and its advance
// and offset in pixels at the run's size (y up, as HarfBuzz gives them; a vertical run advances down: y_advance < 0).
struct ShapedGlyph {
    std::uint32_t glyph = 0;
    std::uint32_t cluster = 0;
    double x_advance = 0;
    double y_advance = 0;
    double x_offset = 0;
    double y_offset = 0;
};

// What to set: the text (UTF-8), the font (render::font_bytes' name) and its face in a collection, its size in pixels (the
// em), the direction ("ltr", "rtl", "ttb" down; "" from the text's script), the language (BCP 47; "" none), the OpenType
// features ({tag, value}: value 0 turns one off, 1 on, more picks an alternate) and the variable axes ({tag, value}).
struct ShapeRequest {
    std::string text;
    std::string font;
    unsigned face_index = 0;
    double size_px = 0;
    std::string direction;
    std::string language;
    std::vector<std::pair<std::string, std::uint32_t>> features;
    std::vector<std::pair<std::string, float>> variations;
    std::string script;  // (PC5 G-b: an ISO 15924 tag, "Arab", …; "" from the text — render/text/bidi's scripts_of)
};

// The glyphs of the run, in the order they are drawn. core::Error("value") for a size that is not above 0 or a tag that is
// not 4 characters; render::NotYetPorted ("default_font", "large_font") for a font file that cannot be read.
std::vector<ShapedGlyph> shape(const ShapeRequest& request);

// HarfBuzz's version ("8.3.0").
std::string harfbuzz_version();

// PC5 G-b (type.fonts, the 書式 → OpenType ▸ menu): what a font is and has — its family and style (the typographic ones
// where it names them, else its own), its PostScript name, its variable axes (tag, name, least, default and most) and the
// OpenType features its GSUB and GPOS tables hold (their tags, sorted, each once), and how many faces its file holds (a
// collection: more than 1). core::Error / render::NotYetPorted as shape() for a font that cannot be read.
struct FontAxis {
    std::string tag;
    std::string name;
    double min = 0;
    double value = 0;
    double max = 0;
};
struct FontInfo {
    std::string family;
    std::string style;
    std::string postscript;
    std::vector<FontAxis> axes;
    std::vector<std::string> features;
    unsigned faces = 1;
};
FontInfo font_info(const std::string& font, unsigned face_index = 0);

}  // namespace genko::render::text
