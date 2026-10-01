#pragma once

#include <filesystem>
#include <set>
#include <string>

#include "core/json.hpp"

// `genko gc` (Python's maintenance.gc, schema-v4 §4.4, SPEC SAVE-03): delete the assets nothing refers to, under the
// project lock. Kept: what project.json refers to; the states and op records of the last 100 committed
// transactions, of every unfinished one, and of every change on the Undo and Redo stacks, with the assets those
// states refer to; the whole converted history (legacy/: the old snapshots, their states and their assets); the
// recovery points under studio/autosave/; and any asset written in the last 24 hours (an import in progress outside
// the lock). Files under assets/ that are not named like an asset are never deleted. Never run by the converter.

namespace genko::storage {

inline constexpr double kKeepNewAssetsSeconds = 24 * 3600;
inline constexpr std::size_t kKeepCommits = 100;

// {"ok": true, "dry_run", "removed": [paths relative to the book], "legacy_pages_removed"} (Python's reply). With
// legacy_pages, the v2 pages/ folder is deleted too (Python's --legacy). Throws LockedError, NeedsMigration (v1–v3).
core::Json gc(const std::filesystem::path& dir, bool dry_run, const std::string& agent = "genko", bool legacy_pages = false);

// Every asset ref the book keeps (see above), without the 24-hour rule.
std::set<std::string> referenced_assets(const std::filesystem::path& dir);

// Every "sha256:<64 hex>" string anywhere in a JSON value (Python's journal.refs_in).
void refs_in(const core::Json& value, std::set<std::string>& out);

}  // namespace genko::storage
