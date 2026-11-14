#pragma once

#include <QString>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// Where a line a person types goes, and how big its balloon is (Python's genko/app/lettering.py, with the measure of
// genko/studio/letter.py it uses): a new line goes into the chosen panel, vertical, at the panel's top right (manga
// reads right to left), to the left of the lines already there; a line typed where the text tool clicked is centred
// there and kept inside its panel. Long text is broken into columns that fit. And the notation typed for ruby
// (｜約束《やくそく》), 傍点 (《《強調》》) and styled parts ({大|…}), read into a line's runs and written back.
//
// Numbers keep Python's types where Python keeps them (a panel's int corner stays an int through max() and round()).

namespace genko::app::lettering {

inline constexpr double kMarginMm = 3.0;  // MARGIN_MM
inline constexpr double kEmMm = 5.0;      // studio.letter.EM_MM

// KINDS: each balloon kind's key and its name; KIND_LABEL.get(key, key).
const std::vector<std::pair<QString, QString>>& kinds();
QString kind_label(const std::string& key);
// fonts.BUNDLED: each bundled face's key and its name.
const std::vector<std::pair<QString, QString>>& bundled_fonts();
// fonts@1 (書体の追加 F2): every face Genko comes with as the 書体 palette lists it — the seven of before (アンチック first,
// the others in the category the catalogue gives their fonts) and the forty added (core/font_catalog.hpp), by the
// catalogue's categories in their order —, and a category's name. `mark`: what a person must know before choosing it
// (「かなだけ（漢字は◯◯）」, 「漢字は一部（残りは◯◯）」; empty: nothing).
//
// A face has one name everywhere a person reads it (`label`): an added face's is the catalogue's; the six of before
// keep the names the text tool has always shown (ゴシック, 明朝, 丸ゴシック, 手書き風, 極太（効果音）, 勢い（効果音）), and
// アンチック is アンチック. The palette's row (`row`) is that name with, for the six, their font's own name in brackets —
// ゴシック（ZEN角ゴシック B）, 極太（効果音・デラゴシック）.
struct FaceRow {
    QString key;       // what a line's style.font takes
    QString label;     // its name
    QString row;       // the palette's row: the name, and the font's own name for the six of before
    QString category;  // the category's id
    QString use;       // what it is for
    QString mark;
    QString search;    // (its names, key, family, use and category, lower case: what the palette's search looks through)
    bool added = false;
};
const std::vector<FaceRow>& catalog_faces();
const std::vector<std::pair<QString, QString>>& face_categories();  // {id, name}
// A face's name for a person: a bundled face's (bundled_fonts), an added face's (the catalogue's), a font file's name
// without its folder and extension.
QString face_label(const QString& font);
// The same in a sentence: アンチック for the composite of before (its list's name explains it in brackets).
QString face_name(const QString& font);
// tategaki.STYLE_TAGS: the marks typed in {…|words} and the style each gives, in Python's order.
const std::vector<std::pair<QString, core::Json>>& style_tags();

// columns(text, per_column): explicit line breaks first, then between phrases, a spoken line without its closing 。.
std::vector<std::u32string> columns(std::u32string_view text, std::int64_t per_column);
// studio.letter.measure(breaks, balloon, em): the balloon's box as the renderer draws it. A capsule and a polygon
// (fukidashi@1) are sized by service::letter::measure itself (`sides`: a polygon's style.sides, 0 when none is said).
// fukidashi@2: a symbol (heart, star, flower, diamond) is sized by service::letter::measure_styled, its points and depth
// from `style`.
std::pair<double, double> measure(const std::vector<std::u32string>& breaks, std::string_view balloon, double em = kEmMm, int sides = 0,
                                  const core::Json& style = core::Json());
// box_size(text, balloon, vertical, max_h_mm, max_w_mm): (w, h), each rounded to 0.01 mm. `style` (fukidashi@2): a
// symbol's points and depth, and the drips below the body — the box that much taller (drip_growth).
std::pair<double, double> box_size(const std::string& text, std::string_view balloon, bool vertical, double max_h_mm, double max_w_mm,
                                   int sides = 0, const core::Json& style = core::Json());
// fukidashi@2: how far below its body the drips of a balloon of this kind and style reach in a box `box_h` mm high, as
// the renderer draws them (core::drip_reach): style.drip_mm, 30 at most and 0.6 of the box; none for the kinds that
// draw none. The body is the box less that.
double drip_height(std::string_view balloon, const core::Json& style, double box_h);
// How much taller than a body `body_h` mm high its box is made for its drips: style.drip_mm, 30 at most and 1.5 × the
// body — the drips drawn in that box (drip_height: 0.6 of it) reach exactly as far, and the body keeps its size.
double drip_growth(std::string_view balloon, const core::Json& style, double body_h);
// A style's sides as the renderer reads them (5..12; 0: none said).
int sides_of(const core::Json& style);
// place_new(episode, page, frame, text, balloon, vertical): {x_mm, y_mm, w_mm, h_mm, wrap}.
core::Json place_new(const core::Document& doc, const core::Page& page, const core::Frame& frame, const std::string& text,
                     const std::string& balloon, bool vertical);
// refit(line, frame, text, balloon, vertical): {x_mm, y_mm, w_mm, h_mm}, the balloon's top-right corner kept.
core::Json refit(const core::StoryLine& line, const core::Frame* frame, const std::string& text, const std::string& balloon,
                 bool vertical);
// fukidashi@1: the box a line is given when a person changes its balloon to or from a kind that holds its letters in
// its own shape (a capsule, a polygon; fukidashi@2: a symbol): {x_mm, y_mm, w_mm, h_mm}, fitted to its words as a new line's is, about the
// balloon's middle — a round oval made a capsule becomes a slim one, not a blob. fukidashi@2: to any other kind (or
// the same), the box as much taller or shorter as the drips drawn change (a kind that draws none — a flash, a dotted
// box —, or a style without them), its top kept: the body stays the size it was. Null when neither changes the box,
// the balloon is drawn by hand or the line is not set in a balloon. `style_after`: the style the line has then (a
// polygon's sides, the drips).
core::Json refit_for_kind(const core::StoryLine& line, const core::Frame* frame, const std::string& kind, const core::Json& style_after);
// place_at(x_mm, y_mm, text, balloon, vertical, frame): {x_mm, y_mm, w_mm, h_mm, wrap}, centred where clicked (`style`:
// the style the line is given — a polygon's sides size its box).
core::Json place_at(double x_mm, double y_mm, const std::string& text, const std::string& balloon, bool vertical,
                    const core::Frame* frame, const core::Json& style = core::Json());

// parse_marks(typed): the text without its marks, and its ruby runs [[base, ruby]], 傍点 [words] and styled parts
// [[words, {scale, bold, rgb, tcy}]] (JSON lists, as Python's lists); and (TEXT-01) each ruby where it was typed: the
// graphemes of the text its base covers (a ｜ typed inside a grapheme takes the whole of it).
struct Marks {
    std::string text;
    core::Json ruby_runs = core::Json::array();
    core::Json emphasis_runs = core::Json::array();
    core::Json style_runs = core::Json::array();
    std::vector<core::RubySpan> ruby_spans;
};
Marks parse_marks(const QString& typed);

// The rubies a line holds once typed marks are applied (TEXT-01, core/ruby.hpp): the notation typed is what the line's
// rubies are. For a new line, the rubies typed. For an edit of `line`: a ruby typed with one's reading over the same
// base is that one (its own settings kept), drawn, whatever the edit rules made of it (one needing review included) —
// the one the edit rules surely put there, else those of the same words in the order they appear;
// one typed where a drawn one went with another reading is that one; one typed with a drawn one's reading over a place
// that is not where that one went but reaches it (typed into its base, its base cut or moved by hand) is that one
// edited in its base: it needs review, its base, reading and settings kept; a ruby typed anywhere else is new (no
// settings of its own). A drawn one not typed again is gone (its notation was taken away); one that needed review
// before (not in the notation) and was not typed over stays so, where the edit rules take it.
std::vector<core::RubySpan> ruby_after(const Marks& marks, const core::StoryLine* line);
// The rubies of an add_line or edit_line op made from typed marks: Python's ruby_runs as Python's window sends them
// (always: even when empty, as edit_line) when they put every ruby where it was typed and the line holds nothing by
// place; else ruby_spans (ruby_runs taken out).
void put_ruby(core::Json& op, const Marks& marks, const core::StoryLine* line, bool always);
// The rubies by place as an op gives them: {start, end, ruby}, the settings each has (font, scale, align, gap_mm,
// tracking), and base and review for one that needs review.
core::Json spans_op(const std::vector<core::RubySpan>& spans);
// The edit_line that gives `line` these rubies (振り直す…, 外す, a ruby's settings): for a line of Python's, Python's
// ruby_runs when they say them (each drawn where its run is found, none needing review, no settings), the line staying
// Python's; else ruby_spans.
core::Json rubies_op(const core::StoryLine& line, const std::vector<core::RubySpan>& spans);
// A line's rubies as listed (by place; for a line of Python's, its runs where the explicit rule finds them).
std::vector<core::RubySpan> rubies_of(const core::StoryLine& line);
// parse_ruby(text): ('約束の日', [['約束', 'やくそく']]) from '｜約束《やくそく》の日'.
std::pair<QString, core::Json> parse_ruby(const QString& text);
// with_marks(line): the line as typed back, its ruby, 傍点 and styled parts in the notation (a line holding its ruby by
// place: each drawn ruby where it is; one that needs review is not in the text, it is listed with the line).
QString with_marks(const core::StoryLine& line);
// with_ruby(text, runs): the text with its ruby in the notation (each run once, in order).
QString with_ruby(const QString& text, const std::vector<core::Json>& runs);
// The parsed marks equal to a line's own (Python compares the lists, the styles' dicts in any order; a line holding its
// ruby by place: the rubies it would hold the same).
bool same_marks(const Marks& marks, const core::StoryLine& line);

// str.strip() of what a person typed (Python's whitespace at either end).
QString strip(const QString& text);

}  // namespace genko::app::lettering
