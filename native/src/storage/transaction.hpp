#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "storage/asset_store.hpp"
#include "storage/lock.hpp"

// Saving a book (schema-v4 §4.2): one transaction in five steps, each durable before the next.
//   1. new assets (strokes, pictures, the .state.json of the new state, a big op record as .ops.json)
//   2. the prepare line in studio/journal.jsonl (fsync)
//   3. project.json replaced atomically (temporary file, fsync, rename, folder fsync)
//   4. the audit line in studio/audit.jsonl when approvals change (fsync)
//   5. the commit line (fsync): only now is the revision saved
// A crash at any point leaves the old or the new book, and journal::repair() settles which (by the project.json's
// sha256). The revision is the committed generation: it only goes up, by one or more, and is never used twice.

namespace genko::storage {

// The book was saved by someone else since it was read: code "revision_conflict" ("revision conflict: expected 3,
// found 5"). Nothing is written.
class RevisionConflict : public core::Error {
public:
    RevisionConflict(std::int64_t expected, std::int64_t found);
};

// A v1–v3 book: never written in place (`genko migrate` makes a v4 copy). Code "needs_migration".
class NeedsMigration : public core::Error {
public:
    explicit NeedsMigration(const std::filesystem::path& dir, int version);
};

struct SaveRequest {
    std::string actor = "genko";
    std::int64_t base_revision = 0;      // the revision the book was read at (0: no book yet)
    core::Json ops = core::Json();       // the ops' journal record (core::journal_op); null for none
    std::string action = "edit";         // edit | undo | redo | recover
    std::string target_txn;              // undo and redo: the transaction (or "legacy:<n>") undone or redone
    std::string txn;                     // the caller's id for this save (a retry with the same id is saved once); new when empty
    std::optional<std::string> expect_state;  // undo and redo: the state the book must come out as
};

struct SaveResult {
    std::int64_t revision = 0;   // the committed generation
    std::string txn;
    std::string state_ref;       // the book's state after the save
    std::string project_sha256;  // of the project.json written
    bool already_committed = false;  // the txn had been saved before: nothing was written now
    bool repaired = false;           // a step failed after project.json was replaced; the repair that followed committed it
    core::Json audit = core::Json::array();  // the approval changes recorded
};

// The book's state, revision and approvals as they are on disk (nothing when there is no project.json).
struct DiskState {
    bool exists = false;
    int version = 0;
    std::int64_t revision = 0;
    core::Json payload;   // project.json, parsed
    std::string state_ref;
};
DiskState read_disk_state(const std::filesystem::path& dir);

class Saver {
public:
    // The caller holds `lock` (it was taken before the book was read): the book cannot change underneath.
    explicit Saver(const ProjectLock& lock);

    // Save `doc` as the next revision. The journal is repaired first (a failed repair throws and nothing is written).
    // Throws RevisionConflict when the book on disk is not at request.base_revision, NeedsMigration for a v1–v3
    // book, core::Error("read_only") for a document that must not be saved, core::Error("io") when a write fails
    // (then repair() has been tried: a save whose project.json was replaced is committed and returned with
    // repaired = true instead).
    SaveResult save(const core::Document& doc, const SaveRequest& request);

    // Save a project.json payload as it is (Undo, Redo, adopting a recovery point: a state saved before, its keys
    // in project.json's order); only "revision" and "writer" are set. Its assets must be in the book already.
    SaveResult save_payload(const core::Json& payload, const SaveRequest& request);

private:
    SaveResult run(const SaveRequest& request, const std::function<core::Json(AssetStore&)>& make_payload);

    const ProjectLock& lock_;
};

// `value` with its objects' keys in the order of `model` (a payload of the same book from the writer), keys only
// `value` has after them; at the top, model's "revision" and "writer" are kept too. Values are `value`'s.
core::Json ordered_like(const core::Json& value, const core::Json& model);

// The journal record of ops (Python's _journal_ops): inline when its JSON (json.dumps, ensure_ascii=False) is at
// most 16000 characters, else a brief of each op inline and the whole list as an .ops.json asset (canonical JSON).
inline constexpr std::size_t kJournalOpsInline = 16000;

}  // namespace genko::storage
