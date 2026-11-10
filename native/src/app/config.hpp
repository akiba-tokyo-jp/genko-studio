#pragma once

#include <QSettings>
#include <QString>

#include <filesystem>
#include <memory>
#include <string>

// Where the app keeps its own things: the config folder (core::config_dir: $GENKO_CONFIG_DIR, else
// %APPDATA%\GenkoStudio on Windows, else $XDG_CONFIG_HOME/genko-studio or ~/.config/genko-studio) with settings.ini,
// recent.json, panel_templates.json, recovery/ and cache/ — apart from the Python app's (M6, COMP-01a: its folder
// ~/.config/genko and its settings QSettings("Genko", "Genko Studio") are only read, to offer them once: migration).

namespace genko::app {

std::filesystem::path config_dir();

// The settings store (a new object each time: QSettings is cheap and not shared between threads).
std::unique_ptr<QSettings> settings();

// The Python app's settings (QSettings("Genko", "Genko Studio"): ~/.config/Genko/Genko Studio.conf, or the registry on
// Windows), to be read only; none when they are not there or $GENKO_CONFIG_DIR is set (the folder chosen holds this
// app's own).
std::unique_ptr<QSettings> legacy_settings();

// human:<name> from $GENKO_USER, else the login name (Python's session.default_actor).
std::string default_actor();

// Folders inside the config folder (made on first use).
std::filesystem::path recovery_root();
std::filesystem::path cache_root();

}  // namespace genko::app
