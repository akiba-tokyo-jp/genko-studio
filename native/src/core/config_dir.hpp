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

// The Python app's settings are there (QSettings("Genko", "Genko Studio"): $XDG_CONFIG_HOME/Genko/Genko Studio.conf, or
// the registry key HKCU\Software\Genko\Genko Studio, looked at without making it).
bool legacy_settings_exist();

// The Python app's things are there and the person has not been asked about them yet (app/migration asks on the app's
// first start): $GENKO_CONFIG_DIR unset, the two folders apart, no answer in this app's settings.ini, and the Python
// app's settings or a file of its folder there. The command line says so, so that a key the AI uses is not missed.
bool import_pending();

}  // namespace genko::core
