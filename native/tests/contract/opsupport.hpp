#pragma once

#include <QString>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>
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

// What a PNG holds: "png:" and the sha256 of "<mode>|<w>x<h>|" and its pixels as RGBA ("unreadable:" and the sha256
// of the bytes when they cannot be read; "missing" for none): the harness's _pixels_digest.
std::string pixels_digest(const std::optional<std::string>& bytes);

// The payload with each PNG it refers to (a layer's pixels, its mask and its patches, and an area kept on a page) as
// the pixels it holds (the harness's _png_pixels): this build writes other PNG bytes for the same pixels.
core::Json with_png_pixels(core::Json payload, const storage::AssetStore& store);

// sha256 of json.dumps(value, ensure_ascii=False) (the harness's digests).
std::string json_digest(const core::Json& value);

// Apply the steps ([{"ops", "agent"?, "dry_run"?}]) to `doc` as the harness does, with new ids counted from
// `first_id` (the harness's --ids), through every op of this build (render::ops_registry). `store` holds the assets
// the payloads refer to. `digest` replaces the snapshots and payloads with their digests (the harness's "digest").
// `last` gets the book after the steps.
std::vector<StepOutcome> run_steps(core::Document doc, const core::Json& steps, std::uint64_t first_id,
                                   storage::AssetStore& store, bool digest = false, core::Document* last = nullptr,
                                   const std::function<void(core::Document&, std::size_t, const StepOutcome&)>& after_step = {});

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
// `deviations` (a deviating case's cpp_differs, below): those places are not compared with Python's book read back; the
// book read back is still compared whole with the book before the save.
std::string read_back_difference(const core::Document& doc, const std::filesystem::path& dir, storage::AssetStore& store,
                                 const core::Json& reread, ReadBackNotes& notes, const std::vector<std::string>& deviations = {});

// --- cases this build answers otherwise than Python on purpose ----------------------------------------------------
//
// A case marked "cpp": "deviates" (in book_cases.json, ops_cases.json) is one where this build deliberately does not
// do what Python does: a decision of the user's, named in its "cpp_says" (SPEC §2: a known bug of Python's is not
// reproduced). Its "cpp_differs" lists the places of a step's record where it may differ from Python's — paths into
// {"reply", "full", "payload"} ("payload/pages/7/layers/*/parent_id"; "*" any key or index) — and everything else is
// compared as for any case, and at least one step must differ there (else the mark is stale). Its "cpp_keeps" says
// what this build keeps that Python does not (kept_by_deviation), checked on the book after the case, the book saved
// and read back, and — failing — on Python's book.

bool deviates(const core::Json& c);
// The case's cpp_differs.
std::vector<std::string> deviation_paths(const core::Json& c);
// The step's outcome, or Python's record of it, with what is at those places (where there is something) replaced by
// one mark.
StepOutcome without_deviations(StepOutcome outcome, const std::vector<std::string>& paths);
core::Json without_deviations(core::Json record, const std::vector<std::string>& paths);
// Where a step's outcome differs from Python's record (up to `most` places, as paths of cpp_differs' kind).
std::vector<std::string> step_differences(const StepOutcome& cpp, const core::Json& python, std::size_t most = 16);
// "" when the case's steps are Python's but at its places (compare_step) and differ there somewhere; else what is
// wrong.
std::string compare_deviating_steps(const std::vector<StepOutcome>& outcomes, const core::Json& records, const core::Json& c);

// What a deviating case's "cpp_keeps" asks of the book after it (`after`; `before`: the book before its last step):
//   "layer_refs": true — every reference to a layer on a page (a layer's parent_id, a ruler's layer_id, the old
//     single ruler's, an animation track's folder and cels and its light table, a line's style.below_layer) names a
//     layer of that page
//     (the user's decision D2: a page copied takes its references along to its own layers);
//   "moved": [[page, layer id, "paint" | "mask"], …] — the layer's pixels (alpha over 127) or its mask's hidden part
//     (under 128) have moved with the page's basic frame from `before` to `after`, as everything else on the page:
//     their middle where the frame takes it (within 0.5 mm) and their area scaled with it (within 5%), the picture as
//     big as the page's own paper (raster.WORKING_DPI; a mask ops.MASK_DPI) — none of it cut off (the user's decision
//     D1).
// "" when it holds; else where it does not.
std::string kept_by_deviation(const core::Json& keeps, const core::Document& before, const core::Document& after);
// Python's words without the addresses of its objects ("<_io.BytesIO object at 0x7f…>" → "<_io.BytesIO object>").
std::string without_addresses(std::string text);

// The layers' PNGs of `doc` (its pixels, masks and patches) written into `dir` as
// "<prefix>p<page>-l<layer>-<asset|mask|patchN>.png", as the harness's "dump" writes Python's.
void dump_pictures(const core::Document& doc, storage::AssetStore& store, const QString& dir, const std::string& prefix);

// Where the pictures `dump_pictures` wrote in `cpp_dir` differ from Python's in `py_dir` (the files whose names start
// with `prefix`): for each, the mode and size or the pixels that differ (how many, by how much at most, where). ""
// when they hold the same pixels.
std::string picture_differences(const QString& cpp_dir, const QString& py_dir, const std::string& prefix);

// Where compare_step cannot ask for the same bits (a perspective warp: Python's homography comes from numpy's SVD,
// LAPACK through OpenBLAS, whose last bits depend on its kernels and the machine — this build solves the same
// equations its own way): what C++ and Python each kept, for compare_step_near.
struct NearSides {
    storage::AssetStore* cpp_store = nullptr;  // the C++ payload's JSON assets (the lines of a layer)
    QString py_store;                          // the Python job's "store"
    QString cpp_dump;                          // the C++ pictures of the step (dump_pictures)
    QString py_dump;                           // Python's (the harness's "dump")
    std::string prefix;                        // "<step>-"
    std::vector<std::string> numeric_paths;
    std::vector<std::string> picture_paths;
};

// compare_step, with numbers equal within 1e-9 (relative), a layer's lines (its JSON asset) read on each side and
// compared so, and the pictures (compared by their pixels) within ARCHITECTURE.md §9: the same mode and size, the mean
// difference of the values at most 2/255 and 99% of the pixels within 32/255. `tolerated` (when given) counts what
// differed within the tolerance. "" when they match so.
// Require all generated steps exactly once, with their full operation count.
std::string step_coverage_error(const core::Json& sequences, const std::vector<std::size_t>& taken,
    const std::set<std::pair<std::size_t, std::size_t>>& compared, std::size_t steps, std::size_t ops);

std::string compare_picture_near(const QString& cpp_file, const QString& py_file);
std::string compare_step_near(const StepOutcome& cpp, const core::Json& python, const NearSides& sides, int* tolerated = nullptr);

// Whether a batch (its "ops") warps an area in perspective (transform_area with warp.perspective).
void affect_perspective(NearSides& sides, const core::Json& ops, const core::Json& payload);
bool warps_in_perspective(const core::Json& ops);

}  // namespace genko::test
