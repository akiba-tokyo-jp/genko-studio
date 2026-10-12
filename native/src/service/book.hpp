#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "storage/lock.hpp"
#include "storage/transaction.hpp"

// A book as the studio tools read and change it (Python's load_episode, ProjectLock, apply_ops and save_episode in
// genko/studio/service.py): read without the lock to look, or under the lock (taken at once, or Python's
// "project locked: …"), changed by batches of ops through the CommandBus with every op of this build, and saved once
// as the next revision (this build's v4 transaction: a book of v1–v3 is never written in place, needs_migration).

namespace genko::service {

using core::Json;

// The bus the studio tools apply ops through (render::ops_registry: every op this build has).
const core::CommandBus& bus();

// load_episode(path): the book as it is on disk (any version; core::Error "not_found" without project.json).
core::Document load_episode(const std::filesystem::path& dir);

// apply_ops(episode, ops, agent=actor[, dry_run]) on a book in memory: the book after the batch (all of it or none). As
// Python's, a batch applied takes one more id (its reply's "job_id"), so the ids after it are Python's.
// core::ApplyError as Python's ApplyError; an exception Python lets through apply_ops (the bus's "python_error":
// TypeError, ValueError, AttributeError, …) is thrown again as Python raises it (core::PyTypeError, PyValueError,
// PyUncaught), so the callers that catch only ApplyError let it through as Python's do.
core::ApplyResult apply_ops(const core::Document& doc, const Json& ops, const std::string& actor, bool dry_run = false);

// A book changed in memory by batches of ops (Python's Episode, which keeps the batches for its next save's journal).
struct Work {
    core::Document doc;
    Json journal = Json::array();  // the ops of every batch applied, as the journal records them

    // apply_ops(episode, ops, agent=actor): the book changed, or left as it was on an error.
    core::ApplyResult apply(const Json& ops, const std::string& actor);
};

// The page whose index == `index` (Python's `next((p for p in episode.pages if p.index == index), None)`).
std::optional<std::size_t> page_at(const core::Document& doc, const Json& index);

// C++: storage::NeedsMigration for a book of v1–v3, which this build does not write (`genko migrate` makes a v4 copy);
// nothing for a v4 book or a folder without one (a project.json that cannot be read is left to the reading).
void refuse_old_book(const std::filesystem::path& dir);

// with ProjectLock(path, agent=…): episode = load_episode(path) … save_episode(episode, path, actor=…)
class Session {
public:
    // Refuses a book of v1–v3 (refuse_old_book) before anything, takes the lock (core::ApplyError "project locked: …",
    // code "locked", when someone holds it: Python's ProjectLock raises ApplyError), settles the journal at its last
    // consistent point and reads the book.
    Session(const std::filesystem::path& dir, const std::string& agent);
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    core::Document& doc() { return work_.doc; }
    Work& work() { return work_; }
    const std::filesystem::path& dir() const { return dir_; }

    // apply_ops(episode, ops, agent=actor): the book changed (and the ops kept for the save's journal record) or, on
    // an error, left as it was.
    core::ApplyResult apply(const Json& ops, const std::string& actor) { return work_.apply(ops, actor); }
    // save_episode(episode, path, actor=actor): the next revision (the timelapse recorded as a save does).
    storage::SaveResult save(const std::string& actor);

private:
    std::filesystem::path dir_;
    storage::ProjectLock lock_;
    Work work_;
    std::int64_t base_revision_ = 0;
};

// The Python exception an ApplyError of the bus stands for, thrown again (see apply_ops); nothing for a plain one.
void rethrow_python(const core::ApplyError& error);

}  // namespace genko::service
