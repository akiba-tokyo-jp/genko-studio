#pragma once

#include <filesystem>
#include <string>

#include "core/error.hpp"
#include "core/json.hpp"

// Converting a v1, v2 or v3 book into a new v4 book (schema-v4 §6, SPEC DATA-01). The old book is never written.
//
//   1. When the old book has project.lock, its OS lock is taken (without writing the file; a book without one gets
//      none), so a Python writer or GC waits or fails meanwhile. Every file but project.lock is read and hashed, then
//      copied into a staging folder next to the new book and hashed again, then read and hashed once more: any
//      difference stops the conversion ("source_changed"); a book in use stops it before ("source_locked").
//   2. From then on only the copy is read.
//   3. project.json is read with the v1–v3 rules and written as v4. Missing or broken assets, values that had to be
//      changed and keys that would be lost are reported, and nothing is written without accept_repairs
//      ("needs_repairs").
//   4. legacy/ keeps the old journal, audit and project.v2.bak.json as they were; every asset of the old book comes
//      over byte for byte (the old snapshots, what they refer to, the op records), and v2's pages/ pictures become
//      assets with the same bytes.
//   5. legacy/map.json: each line of the old journal with its v3 snapshots and the v4 states made from them (empty,
//      and said, when the old journal's last state is not the old project.json).
//   6. studio/audit.jsonl: the old audit lines as they are, then {"v":4,"kind":"migrated",…}.
//   7. The new book's revision is 1; its journal starts with an "action": "migrate" transaction.
//   8. legacy/report.json: what was found and done (the same report is returned).
// The new book is put together in the staging folder and appears at `dst` in one rename, complete or not at all.

namespace genko::storage {

// A conversion that stopped, with the report so far (needs_repairs: what needs the user's consent).
class ConvertError : public core::Error {
public:
    ConvertError(std::string code, const std::string& message, core::Json report);
    const core::Json& report() const { return report_; }

private:
    core::Json report_;
};

// Convert `src` (a v1–v3 book) into the new folder `dst` (which must not exist, or be empty). Returns the report.
// Throws ConvertError (source_locked, source_changed, needs_repairs), core::Error (not_found, exists, not_legacy,
// value, io) and UnsupportedProjectVersion.
core::Json convert(const std::filesystem::path& src, const std::filesystem::path& dst, const std::string& actor = "genko",
                   bool accept_repairs = false);

}  // namespace genko::storage
