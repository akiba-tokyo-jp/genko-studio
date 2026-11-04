#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

#include "core/json.hpp"

// session.json (PC5 H-e, session.inspect; SPEC VIEW-01): what the running app has open, written by it in this computer's
// config folder (app/session_file — at most once a second, when it changed, put in place whole) and read by
// `genko session` and MCP's inspect target=session. Never the book's: a machine's own file, as settings.ini is.
//   {"v": 1, "pid", "app_version", "windows": [{"active", "arrange", "tabs": [{"index", "title", "path"?, "saved",
//    "revision", "page", "contents"?}]}], "foreground": "#rrggbb", "background": "#rrggbb", "updated_at"}
// A book with no folder (untitled, a picture opened as a book) has no path.

namespace genko::core::session_file {

std::filesystem::path path();

// Whether the process is there (this computer's: a pid of a session.json written by an app that ended is not).
bool process_alive(std::int64_t pid);

// What the app has open: the file with "running": true while its app is there, {"running": false} when there is no file,
// or its app has gone (a file left behind when it could not take it away), or the file cannot be read.
Json read();

}  // namespace genko::core::session_file
