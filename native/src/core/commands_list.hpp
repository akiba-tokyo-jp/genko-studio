#pragma once

#include <QSettings>

#include <string>
#include <string_view>

#include "core/json.hpp"

// The screen's commands (PC5 H-e, command.list; SPEC VIEW-01): native/src/core/data/commands.json, embedded at build time —
// every command of the menus once, by the name the app knows it by (its words, with its place when another has the same
// words), its attribute ("act_zoom_in") when it has one, its old names (preferences::renamed), its place in the menus, the
// keys each built-in key set gives it, whether it is a switch and whether it is a tool. The GUI test test_gui_prefs makes it
// from the menus of a window and checks it is the same (as ops_schema.json is checked), and writes it with
// GENKO_WRITE_COMMANDS=1. `genko commands` lays this computer's keys over it — the key set chosen, the person's own set,
// the keys the person gave a command — as the app does (key_sets::apply, preferences::apply_shortcuts). Reading only: the
// commands are not run from here (a running app's state — what can be used now — is the control channel's, 節 J).

namespace genko::core::commands {

std::string_view catalogue_text();
// {"v": 1, "commands": [{"id", "name", "old_names", "menu", "keys": {"photoshop", "genko"}, "checkable", "tool"}]}
const Json& catalogue();

// {"ok": true, "key_set", "commands": [{"id", "name", "menu", "keys", "checkable", "tool"?}]}: the commands with the keys
// this computer gives them (settings: settings.ini), those of a menu alone (`menu`: its words, or a path "編集 → 変形") and
// those whose name, menu or old name has every one of `words` (spaces between, any case).
Json list(QSettings& settings, std::string_view menu = {}, std::string_view words = {});

}  // namespace genko::core::commands
