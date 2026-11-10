#pragma once

#include <cstdint>
#include <filesystem>

#include "core/json.hpp"

// Measuring real agent runs (Python's genko/studio/evaluate.py, §10.8) and moving M0's sidecar drafts into the book
// (genko/studio/adopt.py).

namespace genko::service::evaluate {

using core::Json;

// evaluate.stats(project): name resubmissions per page, images and fix rounds per panel (median), tool calls that
// failed and why (studio/logs/tools.jsonl), tickets.
Json stats(const std::filesystem::path& project);
// evaluate.audit(project): did anyone but a person change an approval (studio/audit.jsonl and the approval records).
Json audit(const std::filesystem::path& project);

// evaluate.eval_sample(project, out, per_character, seed): the blind character test (D5) written into `out`: the
// approved sheets under letters (sheets/A.png …), up to `per_character` single-character panels each cropped from the
// adopted art (samples/s01.png …, shuffled with random.Random(seed)), key.json, answers.csv and form.html.
// {"ok": false, "error"} with fewer than two approved characters.
Json eval_sample(const std::filesystem::path& project, const std::filesystem::path& out, std::int64_t per_character = 10, std::int64_t seed = 0);
// evaluate.eval_score(folder, answers): how often the evaluators named the character right (target 90%).
Json eval_score(const std::filesystem::path& folder, const std::filesystem::path& answers);
// evaluate.eval_atari(truth_path, tolerance_mm): the panels found on scanned names against the true ones (D8: a panel
// counts when a found one matches it within tolerance_mm on every edge; target 90%).
Json eval_atari(const std::filesystem::path& truth_path, double tolerance_mm = 5.0);

}  // namespace genko::service::evaluate

namespace genko::service::adopt {

using core::Json;

inline constexpr const char* kActor = "system:adopt-drafts";

// adopt.adopt_drafts(project): the bible, script, name plans, self-checks, approval requests and open comments of
// studio/drafts/ as ops in one save; the folder then renamed to studio/drafts.adopted. (C++: a drafts.adopted that the
// rename could not replace is found before the save, so the book is not saved with the drafts left to adopt again.)
Json adopt_drafts(const std::filesystem::path& project);

}  // namespace genko::service::adopt
