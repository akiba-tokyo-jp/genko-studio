#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// Reading a .genko folder of any version (1–4) into a Document: Python's genko/migrate.py (migrate_payload) and
// genko/io.py (load_episode, _attach_legacy_rasters), with the same interpretation of every field.

namespace genko::storage {

// The newest project.json version this build reads (and the only one it writes).
inline constexpr int kReadableVersion = 4;

// The file was written by a newer Genko; opening it here could lose data. (Python's UnsupportedProjectVersion; the
// CLI exits with 2.)
class UnsupportedProjectVersion : public core::Error {
public:
    explicit UnsupportedProjectVersion(const std::string& message);
};

// Something in a book that cannot be kept as it is. Python reads such a book silently (a missing asset is
// read as nothing and is gone at the next save); this build reads it the same way, reports it, and opens the book
// read-only.
struct LoadIssue {
    // missing_asset: an asset the book refers to is not there.
    // broken_ref: a ref or path that is not a valid asset ref or a path inside the book.
    // hash_mismatch: an asset whose bytes do not hash to its name (the bytes are used, as Python uses them).
    // broken_asset: an asset that cannot be read (a strokes blob that is not JSON, not base64, …).
    // repair: a value that had to be changed to be held (NaN/Infinity → null, an integer beyond 64 bits, a value
    //         of the wrong type).
    // unknown_key: a key that this build (like Python) does not read, so it would not be written back.
    // unknown_feature: a v4 feature this build does not know.
    std::string kind;
    std::string pointer;  // where in project.json (JSON pointer; "" for the whole file)
    std::string ref;      // the asset ref or path concerned, when there is one
    std::string message;  // what happened, in English

    core::Json to_json() const;
};

struct LoadReport {
    int source_version = 0;
    std::vector<LoadIssue> issues;

    bool clean() const { return issues.empty(); }
    core::Json to_json() const;
};

struct LoadOptions {
    // Hash every asset read and compare it with its name.
    bool verify_hashes = true;
};

struct LoadResult {
    core::Document document;
    LoadReport report;
};

// Read <dir>/project.json and the assets it refers to. Throws UnsupportedProjectVersion for a version above 4 (or
// not an integer) or a min_reader above 4, core::Error("not_found") when there is no project.json,
// core::Error("json") for text that is not JSON (Python's message) and core::Error("format") for JSON that is not
// a book. Problems with assets and values are not errors: they are in the report (and make the book read-only).
LoadResult load_document(const std::filesystem::path& dir, const LoadOptions& options = {});

// The keys of project.json and of a page that this build reads (Python's KNOWN_TOP_KEYS and KNOWN_PAGE_KEYS, and
// v4's min_reader, writer, book_id, features). Other keys are kept in Document::extra and Page::extra.
bool is_known_top_key(std::string_view key);
bool is_known_page_key(std::string_view key);

// The v4 features this build can read and write (none yet: schema-v4.md §3 adds them in M3/M4).
bool is_known_feature(std::string_view feature);

}  // namespace genko::storage
