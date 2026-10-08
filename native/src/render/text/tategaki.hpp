#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "render/image.hpp"
#include "render/text/fonts.hpp"

// 縦書き (Python's genko/tategaki.py): the text as cells (縦中横, Latin words laid on their side, variation selectors
// kept with their letter), the cells in columns with kinsoku, each cell's glyph (vertical forms, small kana and
// punctuation set in the em's upper right, long marks turned), ruby beside its words, 傍点, part of a line in another
// size, weight or colour; and the phrases a letterer breaks a balloon's columns at (文節で改行).
//
// Texts are Python's str: code points (std::u32string). A colour (`fill`) is Python's tuple of ints.

namespace genko::render::text {

using Cell = std::u32string;
using Column = std::vector<Cell>;
// StoryLine::style_runs: [[words, {scale, bold, rgb, tcy, …}], …]
using StyleRuns = std::vector<std::pair<std::string, core::Json>>;
using Rgb = std::vector<std::int64_t>;

inline constexpr char32_t kRot = U'\x1b';       // ROT: a cell of half-width letters laid on their side
inline constexpr char32_t kTcyOpen = U'\x01';   // TCY_OPEN, TCY_CLOSE: around the words a style run sets 縦中横
inline constexpr char32_t kTcyClose = U'\x02';

// A variation selector (異体字セレクタ): U+FE00–FE0F, U+E0100–E01EF.
bool is_vs(char32_t c);

// tuple(colour): a colour's ints as Pillow's getink takes them (PyTypeError for anything but ints).
Rgb rgb_tuple(const core::Json& value);

// cells(text, tcy, latin)
std::vector<Cell> cells(std::u32string_view text, bool tcy = true, bool latin = false);
// has_vs(cell): a letter and its variation selector
bool has_vs(std::u32string_view cell);
// cell_text(cell): the letters of a cell (without ROT)
std::u32string_view cell_text(std::u32string_view cell);
// columns_of(text, per_col, tcy, latin, units): "\n" starts a column, a column holds per_col units (units(cell),
// 1 each without it), kinsoku moves closing marks and small kana back and opening brackets on.
std::vector<Column> columns_of(std::u32string_view text, std::int64_t per_col, bool tcy = true, bool latin = false,
                               const std::function<std::int64_t(const Cell&)>& units = {});

// phrases(text), phrase_columns(text, per, size) (size: len without it), without_periods(text)
std::vector<std::u32string> phrases(std::u32string_view text);
std::vector<std::u32string> phrase_columns(std::u32string_view text, std::int64_t per,
                                           const std::function<std::int64_t(std::u32string_view)>& size = {});
std::u32string without_periods(std::u32string_view text);

// mark_tcy(text, style_runs), mono_runs(ruby_runs), char_styles(text, style_runs, base) (base: a dict or null)
std::u32string mark_tcy(std::u32string_view text, const StyleRuns& style_runs);
std::vector<core::Json> mono_runs(const std::vector<core::Json>& ruby_runs);
std::vector<core::Json> char_styles(std::u32string_view text, const StyleRuns& style_runs, const core::Json& base);

// weight_level(value): 0 normal, 1 bold, 2 heavy; bold_px(em, level): how much a letter is thickened.
int weight_level(const core::Json& value);
int bold_px(std::int64_t em, const core::Json& level);
int bold_px(std::int64_t em, int level);

// _ruby_spans(cols, ruby_runs): (column, first row, last row, ruby) for each run found in reading order.
struct RubySpan {
    std::size_t column = 0;
    std::size_t first = 0;
    std::size_t last = 0;
    std::u32string ruby;
};
std::vector<RubySpan> ruby_spans(const std::vector<Column>& cols, const std::vector<core::Json>& ruby_runs);
// emphasis_cells(cols, runs): (column, row) of every cell that carries a 傍点.
std::set<std::pair<std::size_t, std::size_t>> emphasis_cells(const std::vector<Column>& cols,
                                                             const std::vector<std::u32string>& runs);

// glyph(char, font, em, fill, bold), tcy_glyph(cell, …), latin_glyph(word, …): RGBA pictures of one cell.
Image glyph(std::u32string_view ch, const TrueTypeFont& font, std::int64_t em, const Rgb& fill, int bold = 0);
Image tcy_glyph(std::u32string_view cell, const TrueTypeFont& font, std::int64_t em, const Rgb& fill, int bold = 0);
Image latin_glyph(std::u32string_view word, const TrueTypeFont& font, std::int64_t em, const Rgb& fill, int bold = 0);
// draw_mark(image, centre, size, kind, fill, vertical): a sesame (﹅) or, for "dot", a dot (・).
void draw_mark(Image& image, double cx, double cy, double size, std::string_view kind, const Rgb& fill, bool vertical = true);

// Image.alpha_composite(im, dest): im over the image with its corner at dest (anywhere: what falls outside is lost).
void composite_at(Image& base, const Image& im, std::int64_t x, std::int64_t y);

// compose(text, font, em, max_height, fill, ruby_runs, face=…, …): the keyword arguments as balloons._vertical
// passes them (empty lists for None).
struct ComposeArgs {
    std::u32string text;
    std::int64_t em = 0;
    std::int64_t max_height = 0;
    Rgb fill{10, 10, 10};
    std::vector<core::Json> ruby_runs;
    double tracking = 0.0;
    double leading = 0.0;
    bool tcy = false;
    std::string align = "top";
    bool latin = false;
    std::vector<std::u32string> emphasis_runs;
    std::string emphasis_mark = "sesame";
    StyleRuns style_runs;
    int bold = 0;
    double ruby_scale = 0.5;
    bool mono_ruby = false;
};
Image compose(Fonts& fonts, const Face& face, const ComposeArgs& args);

}  // namespace genko::render::text
