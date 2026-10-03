#pragma once

#include <QSettings>
#include <QString>

#include <filesystem>
#include <memory>
#include <string>

// Where the app keeps its own things: the config folder (Python's genko.tokens.config_dir: $GENKO_CONFIG_DIR, else
// %APPDATA%/genko on Windows, else $XDG_CONFIG_HOME/genko or ~/.config/genko) with recent.json, panel_templates.json,
// recovery/ and cache/, and the settings (Python's QSettings("Genko", "Genko Studio"); an ini file in
// $GENKO_CONFIG_DIR when that is set, so tests never touch the person's own settings).

namespace genko::app {

std::filesystem::path config_dir();

// The settings store (a new object each time: QSettings is cheap and not shared between threads).
std::unique_ptr<QSettings> settings();

// human:<name> from $GENKO_USER, else the login name (Python's session.default_actor).
std::string default_actor();

// Folders inside the config folder (made on first use).
std::filesystem::path recovery_root();
std::filesystem::path cache_root();

}  // namespace genko::app
