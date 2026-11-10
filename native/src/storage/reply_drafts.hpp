#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// The app's reply drafts kept in a v4 book (schema-v4 「返事の下書き」, studio/reply_drafts/): one file for each request
// (or panel) with words not sent yet, `<sha256 of the item's key, 64 lowercase hex>.json` = {"format":
// "genko-reply-draft@1", "item": key, "text": words, "at": "YYYY-MM-DDTHH:MM:SS"}. The app reads and writes them
// (app/reply_box); `genko gc` removes those whose request is closed or gone. Not part of project.json or the journal.

namespace genko::storage::reply_drafts {

inline constexpr const char* kFormat = "genko-reply-draft@1";
inline constexpr std::size_t kMaxText = 64 * 1024;   // the words, in bytes of UTF-8
inline constexpr std::size_t kMaxFile = 256 * 1024;  // a file read (a larger one is broken)
inline constexpr int kMaxFiles = 4096;               // files the app reads from one book (the rest are left as they are)

// <book>/studio/reply_drafts
std::filesystem::path folder(const std::filesystem::path& book);
// Whether a file name is a draft's: <64 lowercase hex>.json.
bool is_draft_name(std::string_view name);
// The name of an item's draft file.
std::string file_name(std::string_view item);

// The item a draft file is for, when it is a whole draft: a regular file (not a link) of at most kMaxFile bytes, UTF-8
// JSON of this format with an item and a text (at most kMaxText bytes), named after its item. Nothing otherwise.
std::optional<std::string> item_of(const std::filesystem::path& file);

// Whether an item still waits in the book (`payload`: its project.json): an open ticket or an open proposal of that id
// (str() of the id, as the app keys it), or "panel:<id>" of a frame the book has.
bool waiting(const core::Json& payload, std::string_view item);

// The draft files of a book whose item no longer waits (closed or gone), by name. Files that are not whole drafts are
// left (the app throws them away when it reads them, and says so).
std::vector<std::filesystem::path> stale(const std::filesystem::path& book, const core::Json& payload);

// A write's temporary file left beside a draft (storage::write_atomic: "<draft's name>.<pid>.<hex>.tmp").
bool is_leftover_name(std::string_view name);
// Those of a book, by name (regular files only).
std::vector<std::filesystem::path> leftovers(const std::filesystem::path& book);

}  // namespace genko::storage::reply_drafts
