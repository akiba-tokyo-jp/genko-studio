#pragma once

#include <filesystem>

// Where this app keeps its own things (the settings, recent books, the person's brushes, colours, poses, materials,
// actions, AI keys and plugins): $GENKO_CONFIG_DIR when that is set (tests, and anyone who wants it elsewhere); else
// %APPDATA%\GenkoStudio on Windows, $XDG_CONFIG_HOME/genko-studio or ~/.config/genko-studio elsewhere. The C++ app
// keeps them apart from the Python app's (M6, COMP-01a): the two can be used side by side, and the C++ app only ever
// reads the Python app's folder, to offer its things once on the first start (app/migration).

namespace genko::core {

std::filesystem::path config_dir();

// The Python app's folder (Python's genko.tokens.config_dir without $GENKO_CONFIG_DIR): %APPDATA%\genko on Windows,
// $XDG_CONFIG_HOME/genko or ~/.config/genko elsewhere.
std::filesystem::path legacy_config_dir();

}  // namespace genko::core
