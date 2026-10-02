#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/json.hpp"
#include "storage/lock.hpp"

// Undo and Redo of saved changes, across processes (Python's journal.restore, schema-v4 §4.4): the change on top of
// the Undo (or Redo) stack is undone (redone) by saving the state before (after) it as a new revision. Refused when
// project.json is not the state the change left (changed outside the journal), when the change is another actor's
// (Undo), and when it would change approvals and the actor is not a person; force overrides the first two.

namespace genko::storage {

struct RestoreResult {
    std::string kind;         // "undo" | "redo"
    core::Json rev;           // the revision of the change undone or redone (as Python reports it)
    std::int64_t revision = 0;  // the new committed generation
    std::string txn;          // the transaction that saved it
    core::Json to_json() const;  // {"ok": true, "kind", "rev", "revision"}
};

// Under `lock` (held). Throws core::Error with the codes nothing_to_undo / nothing_to_redo, external_change,
// other_actor, needs_person, no_before, missing_snapshot, needs_migration, and the Saver's. `txn`: the caller's id for
// the save (`genko apply --txn` with [{"op": "undo"}]); a new one when empty.
RestoreResult restore(const ProjectLock& lock, const std::string& actor, bool redo, bool force, const std::string& txn = {});

// The same, taking the project lock of `dir` (agent: the actor) for the time it takes.
RestoreResult restore(const std::filesystem::path& dir, const std::string& actor, bool redo, bool force);

}  // namespace genko::storage
