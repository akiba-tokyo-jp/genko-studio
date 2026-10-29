#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// スクリプトイベントマネージャーの表 (file.scripts.scriptEventsManager; PC5 I-0 the frame, I-e the window; SPEC.md AUTO-01;
// PhotoCraft v0.2.0 engine/src/automate_cmds.rs 141-260, MIT): steps — a script file or an オートアクション — run when the
// app starts, a book is made, opened, saved, closed, printed or exported. Kept in the settings folder
// (script_events.json: {"enabled": bool, "bindings": [{"event", "name", "script"?: path, "sha256"?, "action"?: name}]}).
//
// Never round the approvals (SPEC.md AUTO-01「承認の決まり」, design 節 I 3.5):
//   - only the app's own window writes the table (save), and keeps its sha256 beside it (script_events.sha256): a table
//     changed by anything else is read turned off, with why (load's `problem`), until the person turns it on again;
//   - a script file is bound with its sha256, an action with its ops' (action_sha256): changed since, it is not run
//     (chosen again: COMP-04's rule for plugins);
//   - the seal is a sha256 with no key: it finds a table changed by accident or by another program, not one changed on
//     purpose by something that can write the settings folder (it can write the seal too) — COMP-04's level;
//   - only the window's own doings raise events — never MCP, HTTP or the command line (an agent's save or export calls no
//     person's steps) — and the steps an event runs raise none (Firing);
//   - the steps run as the window's person, through the bus (service/automate.hpp's rules: no gate op, …), the journal
//     naming them "<name>（<event>）".

namespace genko::service::script_events {

namespace fs = std::filesystem;

enum class Event { StartApplication, NewDocument, OpenDocument, SaveDocument, CloseDocument, Print, Export, Everything };

// "startApplication", "newDocument", "openDocument", "saveDocument", "closeDocument", "print", "export", "everything"
std::string_view id_of(Event event);
std::optional<Event> event_of(std::string_view id);
// Every event, in the manager's order (everything last).
const std::vector<Event>& events();
// The event's name as the window shows it (アプリケーションを起動, 新規ドキュメント, …).
std::string label_of(Event event);

struct Binding {
    std::string event;                  // an id of events()
    std::string name;                   // the binding's name (the journal's, the window's)
    std::optional<std::string> script;  // a steps file (service::automate::read_steps_file) …
    std::optional<std::string> sha256;  // … as it was bound (the file's bytes'; an action's: action_sha256 of its ops)
    std::optional<std::string> action;  // or an オートアクション by name (service::actions)
};

// The sha256 an action is bound with: of its ops as JSON (compact, in their order) — an action changed since (in Genko's
// window or outside) is not run.
std::string action_sha256(const core::Json& ops);

struct Table {
    bool enabled = false;
    std::vector<Binding> bindings;
};

// What a table may be: read whole at most this big, this many bindings.
inline constexpr std::size_t kMaxBytes = 1u << 20;
inline constexpr std::size_t kMaxBindings = 1000;

fs::path path();       // <config>/script_events.json
fs::path seal_path();  // <config>/script_events.sha256: the table's sha256 as the window last wrote it

struct Loaded {
    Table table;
    // Why the table is read turned off, in Japanese (it cannot be read; it was changed outside Genko); none: as written.
    std::optional<std::string> problem;
};
// The table (none there: empty and off).
Loaded load();
// Written with its seal — by the app's スクリプトイベントマネージャー only. Why it could not be, in Japanese.
std::optional<std::string> save(const Table& table);
// The table as JSON (save's bytes are this, dumped) and back (core::Error("value") for what is not a table).
core::Json to_json(const Table& table);
Table from_json(const core::Json& json);

// The bindings this event runs, in order (those of "everything" too); none when the table is off.
std::vector<Binding> bound_to(const Table& table, Event event);

// The steps a binding runs: its script file read (service::automate::read_steps_file) or its action's recorded steps
// (`recorded`: played as the app plays an action) — core::Error("script_changed") when either is not as bound
// (選び直し), "not_found" when it is gone. `label`: the journal's name, "<name>（<event's label>）".
struct Steps {
    core::Json steps = core::Json::array();
    bool recorded = false;
    std::string label;
};
Steps steps_of(const Binding& binding, Event event);

// While one lives, events raise nothing (the steps an event runs save, export … without calling more steps).
class Firing {
public:
    Firing();
    ~Firing();
    Firing(const Firing&) = delete;
    Firing& operator=(const Firing&) = delete;
    // An event raised now runs its steps (no Firing lives but this one).
    bool first() const { return first_; }
    static bool active();

private:
    bool first_ = false;
};

}  // namespace genko::service::script_events
