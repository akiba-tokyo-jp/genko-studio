#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/type_text.hpp"

// PC5 G-c (type@1, SPEC.md TEXT-02「文字スタイル・段落スタイル」, schema-v4.md「文字」): the book's character styles,
// paragraph styles and Basic Paragraph (Document::type_styles, core/type_text.hpp's form) — Photoshop's 文字スタイル and
// 段落スタイル panels as what render/ops_type_styles.cpp's type_style does to the book and its texts.
//
// A text (a line's or a type layer's record) takes a style by its native_type: the whole text's char_style and para_style,
// a range's style, a paragraph's style (core/type_text.hpp). Its own attributes (the whole text's char and para, a range's
// char, a paragraph's para) are its overrides: what it says past its styles (Photoshop's "+"). Genko keeps them as given
// (a style changed changes every text that takes it where nothing of its own says otherwise); whether a text overrides its
// style is told by its look — what its own attributes change of what its styles give (upstream's char_overrides).
//
// The ops change the structure so that what Photoshop's panels do is what the text looks like:
//  - a style applied to part of a text whose whole takes another: the whole's style is laid on its ranges first (each range
//    then takes one character style, as Photoshop's runs do), the text's look elsewhere kept (keep_look);
//  - a style deleted: the texts that took it keep their look, its attributes their own (keep_look: upstream's delete);
//  - override cleared: the own attributes of the range taken away, the look elsewhere kept.
// keep_look lays the attributes a change altered back on the text as its own (a range's char, a paragraph's para: the
// last in the order of resolution), so the look of the text it is asked to keep is the same to the value.

namespace genko::core::type_styles {

enum class Kind { Character, Paragraph };

// Basic Paragraph's id (a paragraph style the book always has; its attributes the type_styles' "basic").
inline constexpr std::string_view kBasic = "basic";
// What the panels call no character style and Basic Paragraph (Photoshop's 日本語版: 「[なし]」, 「[基本段落]」); no style may
// take them as its name.
inline constexpr std::string_view kNoneName = "[なし]";
inline constexpr std::string_view kBasicName = "[基本段落]";
// The names new styles take (a number after them: 「文字スタイル 1」, Photoshop's).
inline constexpr std::string_view kCharacterBase = "文字スタイル";
inline constexpr std::string_view kParagraphBase = "段落スタイル";

std::string_view kind_name(Kind kind);  // "character" | "paragraph"

// A range of graphemes [start, end) of a text.
struct Range {
    std::int64_t start = 0;
    std::int64_t end = 0;
};

// --- the book's styles (a type_styles JSON in its normal form, or null) ---------------------------------------------------

// The style of the kind with the id (Basic Paragraph: kBasic, its "basic" — an object, maybe empty), or null.
const Json* find(const Json* styles, Kind kind, std::string_view id);
// The first id of the kind the book does not use ("cs-1", "ps-2", …).
std::string next_id(const Json* styles, Kind kind);
// A name the kind's styles do not have: `base` itself when `numbered` is false and it is free, else `base` and the first
// number that makes it free (「文字スタイル 1」; a copy: 「強調 のコピー」, then 「強調 のコピー 2」).
std::string unique_name(const Json* styles, Kind kind, const std::string& base, bool numbered);
// A name given for a style: trimmed, 1..64 characters, not 「[なし]」 or 「[基本段落]」, none of the kind's others (`except`: the
// style renamed). core::OpError otherwise.
std::string checked_name(const Json* styles, Kind kind, const Json& name, std::string_view except = {});
// Attributes given by their names (core::type_text's tables; size_pt for size_mm; null: the attribute taken away), split
// into a character and a paragraph part (each {name: value}, nulls kept). core::OpError for an unknown name or a value out
// of its bounds.
struct Attrs {
    Json chars = Json::object();
    Json paras = Json::object();
};
Attrs split_attrs(const Json& given);

// --- texts ----------------------------------------------------------------------------------------------------------------

// What a text looks like: each stretch's character attributes and each paragraph's (core::type_text::resolve_*).
struct Look {
    std::vector<type_text::CharRun> chars;
    std::vector<Json> paras;
};
Look look_of(const StoryLine& record, const Json* styles);

// The record's own attributes laid where its look under `styles` differs from `before` — the characters outside `except`
// (a range's char), the paragraphs not in `except_paragraphs` (a paragraph's para) —, so it looks as it did there.
void keep_look(StoryLine& record, const Json* styles, const Look& before, std::optional<Range> except = std::nullopt,
               const std::set<std::int64_t>& except_paragraphs = {});

// The paragraphs a range touches (an empty range: the one it is in), and where paragraph `p` begins and ends (graphemes:
// [its first, the next one's first)).
std::vector<std::int64_t> paragraphs_of(std::string_view text, std::int64_t start, std::int64_t end);
Range paragraph_span(std::string_view text, std::int64_t p);

// A style applied to the text (or part of it) and/or its overrides cleared — Photoshop's 適用 (with Alt: 上書きを消去) and
// 上書きを消去:
//  - kind Character: the characters of `range` (none: the whole text; an empty range is refused) take `set` (a style's id;
//    nullopt inside: なし) — the whole text's char_style, or its ranges' style — and, `clear`, lose their own attributes;
//  - kind Paragraph: the paragraphs `range` touches (none: all) take `set` (a style's id; nullopt inside or kBasic: 基本段落)
//    and, `clear`, lose their own paragraph and character attributes.
// `set` absent: the styles stay (上書きを消去). The rest of the text looks as it did. core::OpError when the record would
// hold more ranges than it may.
void restyle(StoryLine& record, const Json* styles, Kind kind, std::optional<Range> range, const std::optional<std::optional<std::string>>& set,
             bool clear);

// The record with no reference to the style (its whole, ranges and paragraphs): the texts that took it look as they did
// under `before` (the book's styles before it went; `after`, without it). Whether it took it.
bool take_away(StoryLine& record, Kind kind, std::string_view id, const Json* before, const Json* after);
// Does the record take the style (Basic Paragraph: every record)?
bool takes(const StoryLine& record, Kind kind, std::string_view id);

// The style of the text's range — {"character": id | null (なし, or mixed), "character_mixed", "character_override",
// "paragraph": id | "basic" | null (mixed), "paragraph_mixed", "paragraph_override"} — over `range` (none: the whole text;
// an empty one: the letter at it, and its paragraph). An override: its own attributes change its look there.
Json current(const StoryLine& record, const Json* styles, std::optional<Range> range);

// The style the text takes at grapheme `at` (the character style: its range's, else its whole's — none: nullopt; the
// paragraph style: its paragraph's, else its whole's, else kBasic).
std::optional<std::string> character_style_at(const StoryLine& record, std::int64_t at);
std::string paragraph_style_at(const StoryLine& record, std::int64_t at);

// 新規スタイル from the text (Photoshop starts a style from the text chosen): the attributes at grapheme `at` that differ
// from what the book's Basic Paragraph (and, kind Character, the paragraph's style) gives — {"char", "para"} (para:
// kind Paragraph only).
Json formatting_at(const StoryLine& record, const Json* styles, Kind kind, std::int64_t at);

// スタイルを再定義: what the style `id` takes from the text at grapheme `at` ({"char", "para"}: the attributes there that the
// style, applied with no override, would not give) — for kind Paragraph, the character attributes a character style there
// sets left out.
Json redefinition_at(const StoryLine& record, const Json* styles, Kind kind, std::string_view id, std::int64_t at);
// The stretch of the text at `at` whose letters look alike (the range 再定義 gives the style to).
Range stretch_at(const StoryLine& record, const Json* styles, std::int64_t at);
// The own attributes of `range` (character: its ranges' char; paragraph: the paragraphs' para) whose value is `values`'
// taken away (再定義's: the style says them now).
// The whole text's own of them too (review P2-3): over the whole text taken away; over part of it, laid on the rest first
// (`styles`: the book's, to keep the rest's look).
void drop_same(StoryLine& record, const Json* styles, Kind kind, Range range, const Json& values);

// 初期設定の書式を保存: the text's look at its start — {"char": {…}, "para": {…}}, every attribute that has a value — for
// the machine's settings (never the book's).
Json defaults_of(const StoryLine& record, const Json* styles);
// The saved formatting read back: in the attributes' normal form ({"char", "para"}), or null when it is not one.
Json checked_defaults(const Json& saved);

// The book's styles listed for the panels: kind Character — [なし] first ({"id": null, "name"}), then each {"id", "name",
// "char", "resolved", "texts"}; kind Paragraph — [基本段落] first ({"id": "basic", "name", "para", "char", "resolved":
// {"para", "char"}, "texts"}), then each. "texts": how many texts take it.
Json list(const Document& doc, Kind kind);

// Every text of the book (its lines, its pages' type layers, the type layers in the contents of its smart objects).
std::vector<const StoryLine*> texts(const Document& doc);

}  // namespace genko::core::type_styles
