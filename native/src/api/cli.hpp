#pragma once

namespace genko::api {

// The `genko` command line. Writes JSON to stdout (UTF-8 on every OS; --ascii escapes the rest), usage errors to
// stderr. Exit codes as Python's genko/__main__.py: 0 ok; 1 the command failed ({"ok": false, "error": …,
// "code": …} on stdout); 2 a book written by a newer Genko (the same JSON), or bad usage.
//   genko inspect <dir> [--full] [--stroke ID]   the snapshot of a book (genko.headless.snapshot)
//   genko schema                                 {"ok": true, "ops": [...]} (the public op list)
//   genko render <dir> --page N [--dpi D] [--mode print|proof|name] --out file.png
//                                                one page as PNG; {"ok": true, "path", "mode"}, or
//                                                {"ok": false, "code": "not_yet_ported", "element", …} for a page with
//                                                something this build does not draw yet
//   genko --version                              the versions of Genko and its libraries
int run_cli(int argc, char** argv);

}  // namespace genko::api
