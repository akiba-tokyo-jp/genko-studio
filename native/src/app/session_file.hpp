#pragma once

#include "core/json.hpp"

// session.json (PC5 H-e, session.inspect; core/session_file): the app writes what it has open — its windows, their tabs,
// the book in front, the colours — in this computer's config folder for `genko session` and MCP's inspect target=session:
// looked at once a second, written only when it changed, put in place whole (QSaveFile), taken away when the app quits.

namespace genko::app::session_file {

// Begin (once: the first window starts it).
void start();
// What would be written now (without its time).
core::Json now();
// Written now when it changed since (tests, and the timer); true when written.
bool write_now();

}  // namespace genko::app::session_file
