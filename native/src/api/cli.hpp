#pragma once

namespace genko::api {

// The `genko` command line. Writes JSON to stdout (UTF-8 on every OS; --ascii escapes the rest), usage errors to
// stderr. Exit codes as Python's genko/__main__.py: 0 ok; 1 the command failed ({"ok": false, "error": …,
// "code": …} on stdout); 2 a book written by a newer Genko (the same JSON), or bad usage; 3 a v1–v3 book that must
// be converted (`genko migrate`) before it can be written.
//   genko new <dir> [--title T] [--episode N] [--pages N] [--webtoon] [--b4] [--preset P] [--paper P] [--json] [--plain]
//   genko inspect <dir> [--full] [--stroke ID]   the snapshot of a book (genko.headless.snapshot)
//   genko apply <dir> <ops.json|-> [--dry-run] [--expect-revision N] [--agent A] [--txn T]
//   genko undo|redo <dir> [--as A] [--force]
//   genko gc <dir> [--dry-run] [--legacy]
//   genko doctor <dir>
//   genko migrate <old book> <new folder> [--as A] [--accept-repairs]
//   genko schema                                 {"ok": true, "ops": [...]} (the public op list)
//   genko --version                              the versions of Genko and its libraries
int run_cli(int argc, char** argv);

}  // namespace genko::api
