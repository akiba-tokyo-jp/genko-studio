#pragma once

#include "core/json.hpp"

// commands.json (PC5 H-e, command.list; core/commands_list): the screen's commands of a window as `genko commands` reads
// them — made here from its menus (test_gui_prefs makes it and checks native/src/core/data/commands.json is the same; with
// GENKO_WRITE_COMMANDS=1 it writes it).

namespace genko::app {

class MainWindow;

// {"v": 1, "commands": [{"id", "name", "old_names", "menu", "keys": {"photoshop", "genko"}, "checkable", "tool"}]}, by name.
core::Json commands_catalogue(MainWindow* window);

}  // namespace genko::app
