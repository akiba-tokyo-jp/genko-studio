#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// PC5 G-d (edit.checkSpelling, SPEC.md TEXT-02「スペルチェック」, design section G 3.3 and 8-4 (b)): 編集 → スペルチェック….
//
// Two checks of a book's words — its lines and its type layers (a smart object's contents are not looked in):
//   - spelling: the Latin words against the English dictionary the program carries (SCOWL size 50, American, as PhotoCraft
//     v0.2.0's text/src/spell.rs, MIT; the list's licence in native/resources/dict/LICENSE-SCOWL.txt) and the person's own
//     words (the user dictionary). Only words of Latin letters are read: kana, kanji and other scripts end a word and are
//     never said to be misspelt; words with digits are skipped; a lowercase word may be written capitalised or in capitals,
//     a capitalised one (a name) keeps its capital; a trailing 's and typographic apostrophes are read as upstream reads them.
//   - notation (表記の点検): a table the person makes of words written one way that should be written another (誤 → 正,
//     or one of two spellings to settle on: 「出来る → できる」「ヶ月 → か月」). Nothing of it is carried by the program.
// Neither is an op: the dictionary and the table are this computer's (core/prefs_table type.userDictionary,
// type.notations), so the window and `genko spell` read them and hand them here. A finding is changed by the ops that
// change text (edit_type's replace, replace_text).

namespace genko::service::spell {

// The English word list with each word's SCOWL level (10 most common … 50), which ranks suggestions.
class Dictionary {
public:
    // The program's dictionary (read once, on first use).
    static const Dictionary& english();
    // One word a line; a line "#NN" sets the level of the words after it.
    static Dictionary from_list(std::string_view list);

    std::size_t size() const { return words_.size(); }
    // Is `word` spelt right (the person's words in `user`, lowercase, accepted whatever their case)?
    bool check(std::string_view word, const std::set<std::string>& user) const;
    // Up to `max` suggestions, nearest first (then most common), in the word's case.
    std::vector<std::string> suggest(std::string_view word, std::size_t max) const;

private:
    std::unordered_map<std::u32string, std::uint8_t> words_;                // (as listed → level)
    std::unordered_map<std::u32string, std::vector<std::u32string>> lower_;  // (lowercase → as listed)
    struct Entry {
        std::u32string lower;
        std::uint8_t level = 50;
    };
    std::vector<std::vector<Entry>> by_length_;  // (lowercase words by their length, for suggestions)
};

// The words of a text as code point ranges [start, end): Latin letters and digits, an apostrophe inside a word, a mark on
// a letter of the word; a hyphen, a space and a letter of another script end a word.
std::vector<std::pair<std::size_t, std::size_t>> words(std::u32string_view text);

// One entry of the notation table: what is written, what it should be.
struct Notation {
    std::string from;
    std::string to;
};
// The table read from the settings' JSON ([{"from", "to"}] or [[from, to]]): entries with nothing to find or the same on
// both sides left out, 2000 at most.
std::vector<Notation> notations_of(const core::Json& raw);
core::Json notations_json(const std::vector<Notation>& table);
// The user dictionary from the settings' words (one a line or a tab; blank ones left out), lowercase.
std::set<std::string> user_words(std::string_view joined);

// What the check is told.
struct Options {
    std::set<std::string> user;      // (the user dictionary, lowercase)
    std::set<std::string> ignore;    // (すべて無視 for this check, lowercase)
    std::vector<Notation> notations;
    bool spelling = true;
    bool notation = true;
    bool sfx = false;                // (a line in a sound-effect balloon — 描き文字 — read too)
    std::size_t suggestions = 5;
    std::optional<core::Num> page;   // (one page; none: the whole book)
    std::size_t most = 5000;         // (findings at most)
};

// One finding, in the order of the book (its pages, each page's lines in reading order, then its type layers from the top).
struct Finding {
    std::string kind;                // "spelling" | "notation"
    std::string line;                // (a line's id, or "")
    std::string layer_id;            // (a type layer's id, or "")
    core::Num page;
    std::int64_t start = 0;          // (graphemes of the text, as edit_type's replace takes them)
    std::int64_t end = 0;
    std::string word;
    std::vector<std::string> suggestions;
};
std::vector<Finding> check(const core::Document& doc, const Options& options);
// {"findings": [{kind, line|page+layer_id, page, start, end, word, suggestions}], "count"}
core::Json to_json(const std::vector<Finding>& findings);

// Every place `word` stands as a whole word in `text` (graphemes), for すべて置換 of a misspelt word.
std::vector<std::pair<std::int64_t, std::int64_t>> places_of(std::string_view text, std::string_view word);

}  // namespace genko::service::spell
