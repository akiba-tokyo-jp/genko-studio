#pragma once

#include <exception>
#include <filesystem>
#include <functional>
#include <string>

#include "core/json.hpp"

// Long work in the background (Python's genko/studio/jobs.py): an export that takes minutes is started, waited on for
// a little, and if it is not done by then the agent gets a job id and asks export_status later.
//
// studio/jobs/<id>.json: {id, kind, actor, status: running | done | failed, started, beat, pid, finished?, result?}
// The work runs in this process on a thread of its own. While it runs, `beat` is written every kBeatS seconds; a
// "running" job whose beat is older than kStaleS died with its process and is reported "lost".
//
// C++: a process does not end while a job still runs. The front ends call wait_all() before they return (Python's
// daemon threads die with the process and the job is reported lost; here the job finishes and is recorded).

namespace genko::service::jobs {

using core::Json;

inline constexpr double kWaitS = 40.0;     // (under the agent's usual 60 s time limit)
inline constexpr double kLostS = 6 * 3600;  // (a job still "running" after this long died with its process)
inline constexpr double kBeatS = 15.0;
inline constexpr double kStaleS = 90.0;

// jobs.start(project, kind, work, actor, wait): `work` (it returns the reply as a dict) run on a thread; its reply with
// "job" when it ends within `wait` seconds, else {"ok": true, "job", "status": "running", "kind", "hint"}. An
// exception out of `work` makes the reply {"ok": false, "error": "<Python's type name>: <message>"}. core::Error("io")
// when the first record cannot be written (nothing started).
Json start(const std::filesystem::path& project, const std::string& kind, std::function<Json()> work, const std::string& actor,
           double wait = kWaitS);

// jobs.status(project, job_id): the job's state; a finished job carries its reply in "result".
Json status(const std::filesystem::path& project, const Json& job_id);

// Blocks until every job this process started has ended (each job's thread joined).
void wait_all();

// f"{type(exc).__name__}: {exc}" of what a tool threw, as Python names it (ApplyError, ValueError, TypeError,
// FileNotFoundError, JSONDecodeError, a PyUncaught's own type; anything else "RuntimeError").
std::string exception_text(const std::exception_ptr& caught);

}  // namespace genko::service::jobs
