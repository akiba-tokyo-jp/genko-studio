#pragma once

#include <QString>

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/actor.hpp"
#include "core/json.hpp"

// 制御チャンネル (PC5 J-g, SPEC.md PROD-01「制御チャンネル」; the shape of PhotoCraft v0.2.0's control protocol, MIT —
// docs/control-protocol.md, crates/automation/src/security.rs — on Genko's rules): a program of this computer's person
// looks at the running app and changes the book in front through the same gate as MCP's apply_ops. These are the
// channel's rules without a window (app/control_channel serves them; `genko control` is its client):
//   1. the same key: a token of tokens.json; the first line is {"method": "auth", "params": {"token"}} or nothing is
//      answered (the connection closed); the token's actor is every change's actor;
//   2. the same gate: the book changes through engine.execute and the pen (ui.pointer) only — the ops MCP's apply_ops
//      takes (service::agent_op), applied as the token's actor with an agent's rights (core::Actor::with_agent_rights:
//      a human: token too), so the page locks, strict_gates, the person's own ops and fields refuse them as they refuse
//      MCP, and the journal names the token's actor; nothing undoes (a person's change is never taken back from here);
//   3. no method reaches an approval (HumanService), the plugins' consent, the AI connection, the preferences or the
//      tokens — whatever the token;
//   4. no input of the person's (a key, a menu of any kind, a dialog): the pen reaches the canvas alone, as the token's
//      ops, and a dialog it would bring is not shown (refused);
//   5. looking: ui.inspect, ui.screenshot (the canvas; the whole window for a human: token only), engine.commands,
//      ui.set (the view only) and ui.menu.invoke of the view's commands in view_commands();
//   6. a local socket of the person's own (QLocalServer, never a TCP port), off unless the person turns it on
//      (環境設定 → 連携: integrations.controlChannel), a line of at most 1 MiB, 4 connections, 30 s to read or write;
//   7. no file: nothing opens or saves (app.open, app.save are not made).

namespace genko::service::control {

using core::Json;

inline constexpr std::size_t kMaxLine = std::size_t(1) << 20;  // a request's bytes, its newline among them
inline constexpr int kMaxConnections = 4;
inline constexpr int kTimeoutMs = 30000;  // a line read, an answer written; the pen held without a word
inline constexpr std::size_t kMaxOps = 500;  // as MCP's apply_ops

// The channel's local socket: genko-studio-control-<the user's name, letters and digits only> (app/links' form) — a named
// pipe on Windows, a socket in the person's runtime folder elsewhere (a folder of the person's own: no one else can put a
// socket of that name before Genko's).
QString server_name();

// Every method the channel answers (the table the tests go round): who may call it, whether it changes the book.
struct Method {
    std::string_view name;
    std::string_view who;  // in Japanese: 「human・ai」 or what is the person's alone
    bool changes_book = false;
    std::string_view about;  // in Japanese
};
std::span<const Method> methods();
const Method* method(std::string_view name);
// The table as methods answers it: [{name, who, changes_book, about}].
Json methods_json();

// The first line of a connection: its token's actor when it is {"method": "auth", "params": {"token": …}} with a token
// of `tokens` (compared without stopping early), else none.
std::optional<std::string> authenticate(std::string_view line, const std::map<std::string, std::string>& tokens);

// The actor the channel's changes are made as: the token's name, an agent's rights (rule 2).
core::Actor actor_of(const std::string& token_actor);
bool human_token(const std::string& token_actor);

// What engine.execute and the pen may not apply (none: they may): ops not an array of objects or more than kMaxOps; an op
// MCP's apply_ops does not take (those for_pages repeats too: the approvals, the page locks, undo, set_meta…); the ops that
// read a file (import_psd, import_pages: the channel has no folder of its own — MCP's and the command line's do).
struct Refusal {
    std::string code;     // ops_invalid | op_not_allowed
    std::string message;  // in Japanese
    std::string where;    // a JSON pointer into the params ("/ops/2/op")
};
std::optional<Refusal> refused_ops(const Json& ops);

// ui.menu.invoke's commands (the view's only: the zoom and the fit, the timeline's frames, the extras' switches, the
// panels' switches — never one that changes the book, opens a dialog or reaches the approvals), by attribute.
const std::vector<std::string>& view_commands();
bool view_command(std::string_view attribute);
// The panels ui.set shows or hides and ui.inspect reports (by the dock's object name; never the studio's — 承認箱,
// コマの詳細, 資料 — nor a dialog's).
const std::vector<std::string>& view_panels();
bool view_panel(std::string_view name);
// The tools the pen draws with (ui.pointer): the pen, the pencil and the eraser — a line made into ops (add_stroke, erase,
// paint_mask) and nothing else.
bool pen_tool(std::string_view tool);

// The answers, one JSON line each: {"id", "ok": true, "result"} and {"id", "ok": false, "error": {"code", "message"}}.
Json answer(const Json& id, Json result);
Json refusal(const Json& id, const std::string& code, const std::string& message);

}  // namespace genko::service::control
