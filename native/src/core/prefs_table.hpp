#pragma once

#include <QSettings>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// 環境設定の表 (PC5 H-0, SPEC VIEW-01; PhotoCraft v0.2.0's prefs.rs sections and paths, MIT): every key the app keeps in
// its settings.ini, named by a "section.key" path as upstream's prefs.get/set/reset name them, with its type, its range
// or choices, its value when the file has none and when a change shows. The keys stay as they are (a person's settings
// are read as before); the paths lie over them, so that the screen, the command line and the AI read and write the same
// values through one table. The table is about this computer, never the book: no op reads it (an op's result never
// depends on it).
//
// Sections: upstream's (general … integrations, in Photoshop's order of its preferences' panes) and Genko's own —
// tablet (ペンタブレット), comfort (長い時間の作業), book (原稿と保存), colorSettings (編集 → カラー設定…), shortcuts (a
// command's keys, by its name), keyboardShortcuts (the key set), workspace (the panels and the bars chosen), view (the
// view's switches kept) and state (what the tools and the windows remember: not a preference, kept as written).
// A "*" in a path or a key stands for one name (shortcuts.<a command's name> ↔ shortcuts/<a command's name>).

namespace genko::core::prefs {

enum class Kind {
    Boolean,  // true/false, written with the entry's own spellings (on/off)
    Integer,  // a whole number in [min, max]
    Number,   // a number in [min, max]
    Choice,   // one of `choices`
    Colour,   // "#rrggbb"
    Text,     // any words (a name, a list joined by tabs)
    Path,     // a file or a folder ("" for none)
    Keys,     // keys as Qt writes them ("Ctrl+K", several joined by "|"; "" for none)
    Data,     // kept as written (the windows' layouts, the tools' memories): read only as text, never checked
};

// When a change shows: at once (the open windows apply it), in a window opened after, or on the next start; Never: a
// memory of the app, not a preference.
enum class Apply { Live, NewWindow, Restart, Never };

struct Pref {
    std::string path;       // "interface.theme"
    std::string key;        // "ui/theme"
    Kind kind = Kind::Text;
    std::string fallback;   // as written in the file, when it has none ("" with `none_ok`: no value)
    double min = 0;         // Integer, Number (min == max: no bound)
    double max = 0;
    std::vector<std::string> choices;  // Choice
    std::string on = "true";           // Boolean's spellings
    std::string off = "false";
    Apply apply = Apply::Live;
    std::string label;      // the words of 環境設定 (Japanese); "" for the app's memories
    bool none_ok = false;   // "" is a value too (none chosen: the computer's own, no folder…)
};

// The sections in order: upstream's eighteen, then Genko's.
const std::vector<std::string>& sections();
// Every preference, in the sections' order.
const std::vector<Pref>& table();

// The entry of a path (an exact one before a pattern's), or none. A pattern's name is given back in `name`.
const Pref* by_path(std::string_view path, std::string* name = nullptr);
// The entry of a key of settings.ini (likewise).
const Pref* by_key(std::string_view key, std::string* name = nullptr);
// A path's key in settings.ini (the pattern's "*" filled), and back.
std::optional<std::string> key_of(std::string_view path);
std::optional<std::string> path_of(std::string_view key);

// What is wrong with a value for this entry (nothing: it fits). JSON: booleans, numbers, strings.
std::optional<std::string> check(const Pref& pref, const Json& value);
// A value as the file holds it, read as the entry's type: its fallback when the file has none or holds what the type
// cannot read; a number out of its range brought into it.
Json decode(const Pref& pref, const QVariant& stored);
// A checked value as the file holds it (Boolean's own spellings, a number as a number).
QVariant encode(const Pref& pref, const Json& value);

// settings.ini of this computer's config folder (core::config_dir).
std::unique_ptr<QSettings> store();

// What prefs.get / prefs.set / prefs.reset answer: {"ok": true, "values": {path: value}} or the refusal.
struct Outcome {
    bool ok = true;
    std::string code;     // unknown_pref | bad_value | config_unwritable
    std::string path;     // the path refused
    std::string message;  // in Japanese, for the person
    Json values = Json::object();
    Json json() const;
};

// The values of a path, a section, or every entry ("" — the app's memories, section state, only when asked by name):
// a pattern's entries as the file holds them.
Outcome get(QSettings& settings, std::string_view path = {});
// Every path to its value, checked first: all of them written, or none (a value refused, a path unknown — a section's
// object sets its keys).
Outcome set(QSettings& settings, const Json& values);
// A path, a section or everything back to its value when the file has none — the app's memories (section state) left
// alone: "" never touches them, naming them is refused (bad_value).
Outcome reset(QSettings& settings, std::string_view path = {});

// Old names of commands a later command took for its own ("グラデーション": the tool's before PC4 E-0, the panel's since E4;
// "書き出し…": 原稿を書き出す…'s before PC4 F-0, 編集 → 環境設定 → 書き出し…'s since PC5 H-0). A list of commands or a key
// kept under one is the new command's only when marked as written by a build that knows them: names_current/<key> holding
// the same value (app/preferences' names_current). set writes the mark with such a key — ui/quick, ui/commandbar,
// shortcuts/<one of these>.
const std::vector<std::string>& reused_names();
// The mark set() writes with a key ("names_current/ui/quick"), or none.
std::optional<std::string> mark_of(std::string_view key);

// One value read through the table (the app's getters): `path` must be in it.
Json value(QSettings& settings, std::string_view path);
Json value(std::string_view path);

}  // namespace genko::core::prefs
