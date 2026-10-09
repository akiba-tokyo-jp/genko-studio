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
// tategaki.STYLE_TAGS: the marks typed in {…|words} and the style each gives, in Python's order.
const std::vector<std::pair<QString, core::Json>>& style_tags();

// columns(text, per_column): explicit line breaks first, then between phrases, a spoken line without its closing 。.
std::vector<std::u32string> columns(std::u32string_view text, std::int64_t per_column);
// studio.letter.measure(breaks, balloon, em): the balloon's box as the renderer draws it.
std::pair<double, double> measure(const std::vector<std::u32string>& breaks, std::string_view balloon, double em = kEmMm);
// box_size(text, balloon, vertical, max_h_mm, max_w_mm): (w, h), each rounded to 0.01 mm.
std::pair<double, double> box_size(const std::string& text, std::string_view balloon, bool vertical, double max_h_mm, double max_w_mm);
// place_new(episode, page, frame, text, balloon, vertical): {x_mm, y_mm, w_mm, h_mm, wrap}.
core::Json place_new(const core::Document& doc, const core::Page& page, const core::Frame& frame, const std::string& text,
                     const std::string& balloon, bool vertical);
// refit(line, frame, text, balloon, vertical): {x_mm, y_mm, w_mm, h_mm}, the balloon's top-right corner kept.
core::Json refit(const core::StoryLine& line, const core::Frame* frame, const std::string& text, const std::string& balloon,
                 bool vertical);
// place_at(x_mm, y_mm, text, balloon, vertical, frame): {x_mm, y_mm, w_mm, h_mm, wrap}, centred where clicked.
core::Json place_at(double x_mm, double y_mm, const std::string& text, const std::string& balloon, bool vertical,
                    const core::Frame* frame);

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

// The rubies a line holds once typed marks are applied (TEXT-01, core/ruby.hpp): for a new line, the rubies typed. For
// an edit of `line`, its rubies are first followed through the change of its text by the edit rules; then a ruby typed
// where one of them went is that one (its reading as typed); a ruby typed with one's reading over a place that is not
// where that one went (typed into its base, its base cut or moved by hand) is that one edited in its base: it needs
// review, its base and reading kept; one that needs review and was not typed over stays so; a ruby typed anywhere else
// is new, and one not typed again is gone (its marks were taken away).
std::vector<core::RubySpan> ruby_after(const Marks& marks, const core::StoryLine* line);
// The rubies of an add_line or edit_line op made from typed marks: Python's ruby_runs as Python's window sends them
// (always: even when empty, as edit_line) when they put every ruby where it was typed and the line holds nothing by
// place; else ruby_spans (ruby_runs taken out).
void put_ruby(core::Json& op, const Marks& marks, const core::StoryLine* line, bool always);
// The rubies by place as an op gives them: {start, end, ruby}, and base and review for one that needs review.
core::Json spans_op(const std::vector<core::RubySpan>& spans);
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
