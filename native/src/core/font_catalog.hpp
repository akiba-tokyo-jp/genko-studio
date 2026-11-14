#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// fonts@1 (書体の追加 F2, docs/cpp-migration/schema-v4.md「書体」): the catalogue of the lettering faces Genko comes with —
// native/resources/fonts/FONTS.json, embedded here byte for byte, so the program knows every face whether or not its
// file is on this computer.
//
//   - The six fonts of before (gothic, mincho, maru, hand, sfx, sfx_pop) and their composite antique — the face of a
//     line that names none — are compiled into the program (storage "embedded") and mean what they always meant.
//   - The forty added faces (storage "file") are files installed beside the program (render/font_files finds them).
//     A line, a ruby, a type layer's record, a book's type style or the book's usual face (font_path) names one by
//     its key, the same plain string a face of before is named by.
//   - The feature: the book names fonts@1 while it names one of the forty. A build of before would draw such a key
//     in the default face without a word; the feature makes it open the book read-only instead. Whether the feature is
//     named depends on the catalogue alone, never on the files of the computer that saves.
//   - A face that has no kanji, or only some (kanji "none", "partial"), names a partner (fallback): the face that
//     draws what it lacks, inside the same key. A face without the full-width ！？ names its own look-alikes (the
//     ASCII marks it does have).

namespace genko::core::fonts {

inline constexpr std::string_view kFontsFeature = "fonts@1";

struct Category {
    std::string id;     // dialogue, narration, shout, maru, cute, hand, brush, horror, machine, sfx, other
    std::string label;  // 台詞, ナレーション・モノローグ, …
};

struct Entry {
    std::string key;         // what a book names the face by: lowercase ASCII letters, digits and _
    std::string file;        // its file's name (in native/resources/fonts, and in share/genko-studio/fonts when added)
    bool added = false;      // one of the forty (storage "file", feature fonts@1); false: compiled into the program
    std::string label;       // name_ja: the name a person reads
    std::string category;    // a Category's id
    std::string use;         // use_ja: what it is for, in a few words
    std::string family;      // the font's own names
    std::string style;
    std::string postscript;
    std::int64_t weight = 400;
    std::int64_t bytes = 0;  // the file's size and SHA-256 (lowercase hex)
    std::string sha256;
    std::string kanji;       // full | partial | none
    bool kana_complete = true;
    std::string fallback;    // the partner's key (empty: none)
    std::vector<std::pair<char32_t, char32_t>> lookalike;  // this face's own swaps: a mark it lacks → one it has
    bool vertical = true;            // a GSUB vert feature and vhea/vmtx tables
    bool presentation_forms = true;  // every vertical bracket form is in its cmap (false: brackets are turned)
};

// The catalogue's own bytes (FONTS.json as it is in the source tree and in the package) and what they parse to.
std::string_view catalog_text();
const Json& catalog();

// The 46 fonts in the catalogue's order (the six of before first) and the 11 categories in theirs.
const std::vector<Entry>& entries();
const std::vector<Category>& categories();
// The font of a key (null: not one of the 46 — "antique", a composite, is none of them).
const Entry* find(std::string_view key);
// Whether the key is one of the forty added faces.
bool added(std::string_view key);
// The added face a PostScript name is the name of (null: none of the forty).
const Entry* by_postscript(std::string_view name);

// The added faces the book names, sorted, each once: a line's style.font, a ruby's font, a "font" anywhere in a
// line's native_type, the same of a type layer's record on a page the book writes, a "font" anywhere in the book's
// type styles, the book's usual face (font_path). The nombre's font is never one (it stays the faces of before).
std::vector<std::string> named(const Document& doc);
// Whether the book names one: it then says fonts@1.
bool uses_feature(const Document& doc);
// The added faces a book read without fonts@1 names (a book of before whose words happened to be a key of today):
// they are drawn in those faces from now on, which the person is told once when the book is opened. Empty for a book
// that said the feature when it was read.
std::vector<std::string> named_unannounced(const Document& doc);

// 新しい本のいつもの書体（利用者の決定 2026-11-14）: a book made new from now on starts with 源暎アンチック as its usual
// face (font_path), the face of its lines that name none. Only a book being made: a book of before — one with no
// font_path among them — keeps what it has and is drawn and saved as it was (no font_path: アンチック, as always). On a
// computer without the face's file the book is drawn in アンチック and the person is told, as for any added face.
inline constexpr std::string_view kNewBookFace = "antique_genei";
// Makes a book being made new one of today: kNewBookFace its usual face, and fonts@1 among its features (as its first
// save will say; a book not yet saved is no book of before whose words happen to name a face). Every way a person
// makes a book calls it — the 新規 dialog (漫画 and 画像), the window's untitled book, `genko new` (and --image),
// HTTP /v1/new, MCP create_project and `genko studio init` —; a book opened from a file, a copy, a smart object's
// contents, an action's or a merge's result do not.
void make_new_book(Document& doc);

}  // namespace genko::core::fonts
