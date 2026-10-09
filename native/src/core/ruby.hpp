#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// TEXT-01 (改善 I04; schema-v4 ruby-layout@1): a line's ruby held by its place in the text.
//
// Units. A range [start, end) counts the text's extended grapheme clusters (Unicode UAX #29, as Qt's
// QTextBoundaryFinder finds them with the Unicode data Qt carries, kUnicodeVersion) — never UTF-16 units or code
// points: a letter with its combining marks, an emoji ZWJ sequence, a flag, a letter with its variation selector are
// one each. The ranges are the text's, never the layout's: columns broken by kinsoku or the box never change them.
//
// The rules an edit of the text follows (edit_line's text, replace_text), for each ruby:
//  - an edit before it (an insertion just before it included) moves both ends; one after it (an insertion at its end
//    included) changes nothing;
//  - an edit that takes the whole base away (a deletion of it, or a replacement of all of it) takes the ruby away: it
//    is not drawn, and the history (Undo) gives it back with the text;
//  - any other edit that reaches the base (an insertion inside it, a part of it deleted or replaced, a line break
//    inside it, a replacement across its edge, a combining mark joined to its last letter) makes it need review
//    ("edited"): its base and reading are kept and it is not drawn until a person reassigns or clears it; nothing is
//    guessed;
//  - edit_line's whole new text is compared with the old one by the longest common subsequence of their graphemes: a
//    ruby whose base every such alignment keeps whole in one same place goes there; one the alignments put in more
//    than one place, or keep in one and delete in another, needs review ("ambiguous"); one every alignment deletes
//    is taken away; any other needs review ("edited"). (Texts past kLcsCells are compared by their common start and
//    end only.)
//  - replace_text knows where each match was: those are its edits, each match and its replacement compared first (so
//    京 → 京都 adds 都 after the 京).
// A ruby that needs review keeps a range only as where it was last seen (where the edit's place maps it).
//
// Python's ruby_runs ([[base, reading], …], found by their words when drawn) become spans by the explicit rule
// from_runs. A line of Python's keeps them as they are while what an edit does to them is what Python does
// (core/ops_lines.cpp).

namespace genko::core {

inline constexpr std::string_view kRubyLayoutFeature = "ruby-layout@1";

namespace ruby {

// The Unicode version of the grapheme rules: Qt 6.11.2's own data (fixed with the Qt version, ARCHITECTURE §1).
inline constexpr std::string_view kUnicodeVersion = "17.0";
// Whether the Qt this build runs with has that version's data.
bool unicode_is_fixed();

// The most cells (graphemes before + 1) × (after + 1) compared by the longest common subsequence.
inline constexpr std::int64_t kLcsCells = 4'000'000;

// Code points of UTF-8 text, and back (core::Error "value" for text that is not UTF-8).
std::u32string code_points(std::string_view utf8);
std::string to_utf8(std::u32string_view text);

// The text's grapheme clusters as code-point offsets: where each begins, then the text's length (one item for an empty
// text).
std::vector<std::size_t> bounds(std::u32string_view text);
std::int64_t count(std::string_view utf8);
// The graphemes [start, end) of a text (UTF-8 in and out; the range clamped to the text).
std::string slice(std::string_view utf8, std::int64_t start, std::int64_t end);

// The keys this feature adds to a line's style (C++ only; not Python's STYLE_KEYS): ruby_font (a face, as style.font),
// ruby_align (head | center | tail; default center), ruby_gap_mm (the distance from the text, -1..10 mm; default 0),
// ruby_tracking (between the ruby's letters, -0.5..2 of the ruby's size; default 0). The ruby's size against the text
// is Python's ruby_scale (0.25..0.8 of the text, unitless).
bool is_layout_key(std::string_view key);
const std::vector<std::string>& layout_keys();
// The value a layout key keeps (OpError "<key> must be …" otherwise).
Json layout_value(std::string_view key, const Json& value);
bool uses_layout(const Json& style);

// The explicit rule for Python's ruby_runs: in order, a run that is a list whose first two items are truthy (Python's
// `run and run[0] and run[1]`) has str(run[0]) for its base and str(run[1]) for its reading; the base is looked for,
// by code points, in the text from where the previous run's base was found (the first place there, as Python's
// renderer finds it), and becomes the graphemes it covers when both its ends fall between graphemes. A run not found
// is not drawn (as Python); with `keep_unplaced` it is kept as needing review ("unplaced", at [0, 0)).
std::vector<RubySpan> from_runs(std::string_view text, const std::vector<Json>& runs, bool keep_unplaced);
// The spans as Python's ruby_runs: [base, reading] of each drawn span, in order.
std::vector<Json> runs_of(const std::vector<RubySpan>& spans);

// Spans given to an op ([{start, end, ruby, base?, review?}, …]) against the text: a drawn span's base is the text there
// (a base given must be it); a span needing review keeps the base given (default: the text there) and may lie
// anywhere in the text. Sorted by place. OpError for anything else.
std::vector<RubySpan> parse(const Json& raw, std::string_view text);
// As project.json holds them: [{start, end, base, ruby, review?}, …].
Json to_json(const std::vector<RubySpan>& spans);
// What is wrong with spans read for a text (empty: nothing): ranges outside it or overlapping drawn ones, a drawn
// span whose base is not the text there, a review that is not one of the reasons, a span without its reading.
std::string problem(const std::vector<RubySpan>& spans, std::string_view text);

// What an edit did to a line's spans. `removed`: rubies whose whole base went (not drawn; Undo has them).
struct Followed {
    std::vector<RubySpan> spans;
    std::int64_t removed = 0;
};
// edit_line's text: `before` became `after` (the rules above).
Followed follow_text(std::string_view before, std::string_view after, const std::vector<RubySpan>& spans);
// One match of replace_text: the code points [start, end) of the text before, replaced by `with`.
struct Replacement {
    std::size_t start = 0;
    std::size_t end = 0;
    std::u32string with;
};
// replace_text: `before` became `after` by these replacements (left to right, none overlapping).
Followed follow_replacements(std::string_view before, std::string_view after, const std::vector<Replacement>& edits,
                             const std::vector<RubySpan>& spans);

// The drawn spans of two lists are the same rubies (place, base, reading) and neither has one needing review but for
// "unplaced" ones.
bool same_drawn(const std::vector<RubySpan>& a, const std::vector<RubySpan>& b);

std::int64_t review_count(const StoryLine& line);
std::int64_t review_count(const Document& doc);
// The line uses the feature: it holds spans or a layout key.
bool uses_feature(const StoryLine& line);

// After an op changed a line: the spans in order and ruby_runs their mirror; a line of Python's given a layout key
// holds its ruby by place (from_runs, keeping the runs not found); a line with neither spans nor a layout key back in
// Python's form; the book's features given ruby-layout@1 when the line uses it.
void settle(Document& doc, StoryLine& line);
void add_feature(Document& doc);

}  // namespace ruby
}  // namespace genko::core
