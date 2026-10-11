#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"

// The studio's own files in a book's folder and in the person's config folder, written and read as Python's genko.studio
// writes and reads them (the same bytes, so the two versions can share a book):
//   studio/presence.json       who is writing the book from an agent (presence.py)
//   studio/logs/tools.jsonl     one line per agent tool call (toollog.py; tools.1.jsonl past 20 MiB)
//   studio/claims/<item>.json   short leases on work items (claims.py)
//   studio/requests/<id>/       a generation request: request.json and its files; studio/inbox/<id>/ (genreq.write, read)
//   <config>/tools.json         the image tools the agent uses, as the person describes them (tools_registry.py)
// and the JSON helpers they use (jsonutil.py). None of these is the book's content: project.json is written only by
// storage::Saver.

namespace genko::studio {

// --- jsonutil ------------------------------------------------------------------------------------------------------------

// jsonutil.canonical_json: keys sorted, "," and ":" without spaces, UTF-8.
std::string canonical_json(const core::Json& value);
// jsonutil.content_hash: "sha256:" and the sha256 of canonical_json.
std::string content_hash(const core::Json& value);
// jsonutil.write_json: json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) and "\n", replacing the file
// atomically (its folders made). Throws core::Error("io").
void write_json(const std::filesystem::path& path, const core::Json& value);
// jsonutil.read_json: the file's JSON, `fallback` when there is no such file. core::Error("json") for a file that is
// not JSON (Python's message), core::Error("io") when it cannot be read.
core::Json read_json(const std::filesystem::path& path, const core::Json& fallback = core::Json());

// --- presence (studio/presence.json: {"actors": {actor: {"at": epoch}}, "acks": {actor: {"others": [...], "at"}}}) -------

inline constexpr double kActiveSeconds = 15 * 60;  // another conversation that wrote this recently is "using" the book
inline constexpr double kAckSeconds = 30 * 60;     // a warning once seen holds for this long

// presence.actor_for: the recorded name of a conversation, "<base>/<session>" (base alone without a session).
// core::Error("value") with Python's words for a session that is not 1 to 40 of A-Z a-z 0-9 _ . : @ -.
std::string actor_for(const std::string& base, const std::optional<std::string>& session);

struct Other {
    std::string actor;
    double minutes_ago = 0;  // round((now - at) / 60, 1)
};
// presence.others: the other actors that wrote the book in the last kActiveSeconds, by name.
std::vector<Other> others(const std::filesystem::path& project, const std::string& actor, double now);
// presence.check: before a write, the others this actor has not been told about (told once: noted in "acks").
std::vector<Other> check(const std::filesystem::path& project, const std::string& actor, double now);
// presence.touch: after a write, this actor is using the book (actors older than a day are dropped).
void touch(const std::filesystem::path& project, const std::string& actor, double now);

// --- toollog (studio/logs/tools.jsonl) ------------------------------------------------------------------------------

inline constexpr std::uintmax_t kToolLogBytes = 20ULL * 1024 * 1024;

// toollog.record: one line for an agent tool call made at `at` (epoch seconds): its tool, its arguments summarised
// (numbers, bools and texts of at most 64 characters only), how long it took, whether it worked and its error codes or
// error. A log that cannot be written is left (logging never breaks a tool).
void record_tool(const std::filesystem::path& project, const std::string& actor, const std::string& tool, const core::Json& kwargs,
                 const std::optional<core::Json>& result, const std::optional<std::string>& error, double ms, double at);
// toollog.read: the lines of tools.1.jsonl then tools.jsonl (lines that are not JSON left out).
std::vector<core::Json> read_tools(const std::filesystem::path& project);

// --- claims (studio/claims/<item>.json: {"actor", "expires"}) --------------------------------------------------------------

inline constexpr double kLeaseSeconds = 600;

// claims.holder: who holds the item now (none when nobody, or the lease has run out, or the file cannot be read).
std::optional<std::string> holder(const std::filesystem::path& project, const std::string& item_id, double now);
// claims.claim: whether `actor` holds the item afterwards (a new claim, a renewal, or an expired one taken over), the file
// made with O_EXCL as Python makes it, so a Python writer and this build exclude each other.
bool claim(const std::filesystem::path& project, const std::string& item_id, const std::string& actor, double now);
// claims.release: the claim given back when `actor` holds it.
void release(const std::filesystem::path& project, const std::string& item_id, const std::string& actor, double now);

// --- generation requests (studio/requests/<id>/) -------------------------------------------------------------------------

// genreq.read: studio/requests/<id>/request.json (nothing for an id that is not "rq_…" or holds "/", "\" or "..", or
// when there is no such file). core::Error("json") for a file that is not JSON.
std::optional<core::Json> read_request(const std::filesystem::path& project, const std::string& request_id);
// genreq.write: the request's files (each written only when it differs, by name under its folder) and request.json
// (indent 2, "\n"), and the inbox folder studio/inbox/<id>/. Returns the request's folder. core::Error("value") for a
// request without an id or a file name that leaves the folder; core::Error("io").
std::filesystem::path write_request(const std::filesystem::path& project, const core::Json& request,
                                    const std::map<std::string, std::string>& files);

// --- tools_registry (<config>/tools.json) ------------------------------------------------------------------------------

// tools_registry.GENERIC: what an unknown tool is taken to be.
core::Json generic_tool();
// tools_registry.load: {tool id: description} (empty when there is no file, or it is not a JSON object).
core::Json load_tools(const std::filesystem::path& config);
// tools_registry.get: (the id, its description over GENERIC), or (none, GENERIC) for an unknown or missing tool.
std::pair<std::optional<std::string>, core::Json> get_tool(const std::filesystem::path& config, const std::optional<std::string>& tool_id);
// tools_registry.set_tool: the tool's description kept (sizes_px as [[w, h], …] of ints); core::Error("value") with
// Python's words otherwise.
core::Json set_tool(const std::filesystem::path& config, const std::string& tool_id, const core::Json& spec);
// tools_registry.remove: whether it was there.
bool remove_tool(const std::filesystem::path& config, const std::string& tool_id);

}  // namespace genko::studio
