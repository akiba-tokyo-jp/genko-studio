#pragma once

#include <QStringList>

// PC5 H-e (環境設定と API; SPEC VIEW-01): this computer's preferences, the screen's commands and what the running app has
// open, from the command line — PhotoCraft v0.2.0's prefs.get / prefs.set / prefs.reset, command.list and session.inspect
// (MIT). stdout is one JSON object; exit 0 when it says ok, 1 when refused, 2 for bad usage (on stderr).
//   genko prefs get [section|section.key]        {"ok": true, "values": {path: value}} (every preference without one;
//                                                the app's memories — section state — only by their names)
//   genko prefs set <section.key> <JSON value>   checked, written (all of them or none): {"ok": true, "values"}, or
//   genko prefs set --values '<JSON object>'     {"ok": false, "error": unknown_pref|bad_value|config_unwritable, "path",
//                                                "message"}. A running app applies what shows at once (its watch of
//                                                settings.ini); QSettings' lock keeps the two writers apart.
//   genko prefs reset [section|section.key]      back to the values the app has with none chosen (the app's memories kept)
//   genko commands [--menu M] [--search W]       {"ok": true, "key_set", "commands": [{"id", "name", "menu", "keys",
//                                                "checkable", "tool"?}]} with this computer's keys
//   genko session                                {"running": true, …session.json} or {"running": false}

namespace genko::api {

int prefs_command(const QStringList& args, bool ascii);
int commands_command(const QStringList& args, bool ascii);
int session_command(const QStringList& args, bool ascii);

}  // namespace genko::api
