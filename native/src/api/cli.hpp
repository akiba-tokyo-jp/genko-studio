#pragma once

namespace genko::api {

// The `genko` command line. Writes JSON to stdout (UTF-8 on every OS), diagnostics to stderr.
// Exit codes: 0 ok, 1 the command failed, 2 bad usage.
int run_cli(int argc, char** argv);

}  // namespace genko::api
