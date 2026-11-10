#pragma once

#include <QStringList>

// `genko studio …` (Python's genko/studio/cli.py): the agent tools as MCP has them (the project is a path here) and the
// human approvals (approve, revoke, comment, …: --as human:<name>). stdout is one JSON object: the tool's reply, exit 0
// when it says ok and 1 otherwise. What Python's command line catches (ApplyError, ValueError, TypeError,
// FileNotFoundError, JSONDecodeError) is {"ok": false, "error": str(exc)} as there, plus "code" (ARCHITECTURE.md §8);
// what Python lets through with a traceback is {"ok": false, "error": "<Type>: <message>", "code": "python_error"}
// here, exit 1. Usage errors (argparse's) go to stderr, exit 2.

namespace genko::api {

int studio_command(const QStringList& args, bool ascii);

}  // namespace genko::api
