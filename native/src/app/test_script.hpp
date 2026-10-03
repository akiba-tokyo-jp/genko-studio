#pragma once

// GENKO_TEST_SCRIPT=<file.json>: steps a test runs inside the app it started (`genko app <book>` in another process),
// such as drawing a line with the pen, choosing a command or answering the questions the app asks, with a log of
// what the app shows after each of them. Only in the builds that have the save path's fault injection (Debug and
// ASan); a release build ignores the variable.
//
//   {"log": "<file.jsonl>", "steps": [{"do": "wait_book"}, {"do": "stroke", "points": [[x, y], …]}, …]}
//
// Steps: wait_book {path?, ms?} · tool {name} · stroke {points: [[x_mm, y_mm, pressure?], …], device: mouse|tablet,
// tilt?: [x, y], rotation?} · action {name: act_…} · ops {ops: […]} (applied as the window's commands apply theirs) ·
// page {row} · wait_saved {ms?} · wait_status {kind: saved|dirty|saving|failed|recovery_only} · wait_rendered {ms?} ·
// sleep {ms} · answers {close: save|save_as|discard|cancel, question: bool, questions: {title: bool}, save_path} ·
// fault {set: "commit:fail" | ""} · start_dialog {choose: path | close: true} · report {tag} · quit · exit_now {code}
// (the process ends at once, as when it is killed).
// Each step writes a line to the log ({"step": i, "do": …, "ok": true, …}); the last writes {"done": true}. A step
// that fails writes {"step": i, "ok": false, "error": …} and the app ends with exit code 70.

namespace genko::app::test_script {

// This build runs test scripts.
bool available();
// Start the script GENKO_TEST_SCRIPT names, if any (its steps run from the event loop). Returns whether one runs.
bool start();

}  // namespace genko::app::test_script
