#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// オートアクション, kept apart from the windows (PC5 I-0, SPEC.md AUTO-01; moved here from app/actions.cpp, which keeps
// the dialog and the menu): what was done (the ops) recorded under a name, and done again on another page. The recording
// keeps the ops as they were applied. Played back, they go to the page on screen and the layer being drawn on: "page"
// becomes the current page, "layer_id" the current layer, a panel the chosen panel (or none), and things the recording
// made (a layer, a tone, a line…) get new ids each time. Actions are kept in the settings folder (actions.json: its form
// and bytes as before), so every book — and the batch, the droplets and the script events (service/automate.hpp,
// service/script_events.hpp), and the actions panel (節 J) — can use them.

namespace genko::service::actions {

std::filesystem::path path();
// The actions kept ({name: {"ops": […], "made": "YYYY-MM-DD HH:MM"}}, in their order); none readable: empty.
core::Json load();
// Kept in actions.json; why it could not be written (a folder that cannot be written, a full disk), in Japanese, when it
// was not — said by the caller, never ending the app.
std::optional<std::string> save(const core::Json& all);
// `made`: the time it is kept with ("YYYY-MM-DD HH:MM", the local time now when empty).
std::optional<std::string> store(const std::string& name, const core::Json& ops, const std::string& made = {});
std::optional<std::string> remove(const std::string& name);

// The ops that approve or lock (approve, revoke, allow_chat_approval, lock_page, unlock_page, name_ok, advance): never a
// step of a recording, a script, a batch, a droplet or a script event (SPEC.md AUTO-01: no way round the approvals).
bool gate_op(std::string_view name);
// The ops worth recording (not undo, the book's pages, the gates …), copied.
core::Json recordable(const core::Json& ops);
// For each step: does it change one particular thing that was on the recorded page (a line, a stroke, an effect…)?
// Such steps cannot be done on another page and are left out when played.
std::vector<bool> page_bound(const core::Json& ops);
core::Json playable(const core::Json& ops);
// The recorded ops aimed at this page, layer and panel, with fresh ids for what they make (`new_id`: tests).
core::Json replay(const core::Json& ops, std::optional<core::Json> page, const std::optional<std::string>& layer_id,
                  const std::optional<std::string>& frame_id, const std::function<std::string()>& new_id = {});

}  // namespace genko::service::actions
