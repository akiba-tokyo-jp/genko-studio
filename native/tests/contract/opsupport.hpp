#pragma once

#include <QString>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "storage/asset_store.hpp"

// Running op batches the way the Python reference harness's "steps" job does (tools/migration/pyref_harness.py):
// batches applied one after another to one book in memory; after each, the reply, the full snapshot and the
// project.json payload as Python's v3 writer writes it. Shared by the op contract tests.

namespace genko::test {

struct StepOutcome {
    core::Json reply;    // {"ok": true, "applied", "snapshot", "job_id", "warnings"?, "results"?} or {"ok": false, "error"}
    std::string code;    // the error's code ("" for a success)
    core::Json full;     // snapshot(full=True) of the book after the step
    core::Json payload;  // project.json as Python's v3 writer writes it, without "revision"
};

// project.json v4 as Python's v3 writer writes it: version 3, without min_reader, writer, book_id, features and
// revision.
core::Json as_v3_payload(core::Json payload);

// sha256 of json.dumps(value, ensure_ascii=False) (the harness's digests).
std::string json_digest(const core::Json& value);

// Apply the steps ([{"ops", "agent"?, "dry_run"?}]) to `doc` as the harness does, with new ids counted from
// `first_id` (the harness's --ids). `store` holds the assets the payloads refer to. `digest` replaces the snapshots
// and payloads with their digests (the harness's "digest"). `last` gets the book after the steps.
std::vector<StepOutcome> run_steps(core::Document doc, const core::Json& steps, std::uint64_t first_id,
                                   storage::AssetStore& store, bool digest = false, core::Document* last = nullptr);

// The full snapshot and the project.json payload as Python's v3 writer writes it (without "revision") of `doc`.
StepOutcome state_of(const core::Document& doc, storage::AssetStore& store);

// What differs between C++'s outcome of a step and Python's record of it ({"reply", "full", "payload"}): "" when they
// match. A reply Python could not give (it stopped with an exception apply_ops lets through, "uncaught") matches a
// C++ "python_error" whose message is the ValueError's (Python's command line prints it as the error) or ends with
// "<exception>: <message>" (where Python's command line stops with a traceback).
std::string compare_step(const StepOutcome& cpp, const core::Json& python);

// What saving a book and reading it back leaves out on both sides (Python's save_episode and load_episode, this
// build's Saver and reader), counted: the frame selected, the layers of a page that has none (read back with the
// default ones), the int margins of a paper preset (read back as floats).
struct ReadBackNotes {
    int unselected = 0;
    int refilled = 0;
    int floated = 0;
};

// `doc` saved by storage::Saver as a new v4 book in `dir` and read back: "" when it reads back as it was (but for what
// `notes` counts) and as Python's book saved by save_episode and read back (`reread`: the "reread" record of a harness
// "steps" job); else what differs. `store` holds the assets the payloads refer to.
std::string read_back_difference(const core::Document& doc, const std::filesystem::path& dir, storage::AssetStore& store,
                                 const core::Json& reread, ReadBackNotes& notes);

}  // namespace genko::test
