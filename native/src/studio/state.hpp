#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/pynum.hpp"

// The agent's state read from a book (Python's genko/studio/state.py: no sidecar files):
//   bible  → the book's bible (plot, characters, constraints) + studio.bible_doc (the rest)
//   script → studio.script
//   name   → page.plan: {"name": plan, "input_hash", "rev", "slot_to_frame", "reading_order", "by", "reviews"}
//   tickets→ the book's tickets (kind "gate" = approval request, "fix" = instruction, "help" = a question)

namespace genko::studio {

// state.bible: {**studio.bible_doc, "plot", "characters", "constraints"}, or nothing when the book has no bible_doc.
std::optional<core::Json> bible(const core::Document& doc);
// state.script: studio.script (nothing when it is not there).
std::optional<core::Json> script(const core::Document& doc);
// state.name: the plan of page `index` when it has a name, else nothing.
std::optional<core::Json> name(const core::Document& doc, const core::Num& index);
// state.names: {page index: plan} of the pages with a name (in page order; a repeated index keeps its first place).
std::vector<std::pair<core::Num, core::Json>> names(const core::Document& doc);
// state.name_review: the name's review (plan.reviews.name), or nothing.
std::optional<core::Json> name_review(const core::Document& doc, const core::Num& index);
// state.open_tickets: the open tickets (of one kind).
std::vector<core::Json> open_tickets(const core::Document& doc, const std::optional<std::string>& kind = std::nullopt);
// state.page_fixes: a person's open instructions for the agent about page `index` as a whole (not one panel's art).
std::vector<core::Json> page_fixes(const core::Document& doc, const core::Num& index);
// state.written_look: what the bible says a character looks like, in one line (髪: …。目: …。…).
std::string written_look(const core::Json& character);

}  // namespace genko::studio
