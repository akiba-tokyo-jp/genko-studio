#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "core/json.hpp"
#include "core/model.hpp"

// What to do next, computed from the project state alone (Python's genko/studio/worklist.py: no stored "done" marks).

namespace genko::service::worklist {

using core::Json;

// worklist.page_side(page): {side, turn} (the even page is seen first after a turn).
Json page_side(const Json& page);
// worklist.next_actions(episode, project): the work items, in Python's order (the inbox and the assets' headers read
// when `project` is given).
Json next_actions(const core::Document& doc, const std::optional<std::filesystem::path>& project);
// worklist.plan_hash(plan): content_hash(plan).
std::string plan_hash(const Json& plan);
// jsonutil.sha256_hex(text)
std::string sha256_hex(const std::string& text);

}  // namespace genko::service::worklist
