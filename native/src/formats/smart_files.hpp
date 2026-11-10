#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "core/smart.hpp"

// smart-objects@1 (PC3 A1, docs/cpp-migration/SPEC.md COLOR-05「中身とリンク」): what the window and the command line do
// with a smart object's files outside the book — the ops never read or write a path. A linked file looked for (its path,
// else its place from the book's folder) and told ok, modified or missing; the contents written out (コンテンツを書き出し…:
// a file's own bytes, a page's as a PSD or a PNG); a book packed with its linked files (パッケージ…: PhotoCraft v0.2.0's
// engine/src/print_cmds.rs:401-449, MIT, third_party/photocraft/NOTICE.md — here the book's copy of each linked file is
// what is written, so none is missing).

namespace genko::formats::smart_files {

namespace fs = std::filesystem;

// Where a linked file is on this machine: its path when a file is there, else its place from the book's folder (the
// link's "relative") when one is there; none when neither is.
std::optional<fs::path> linked_file(const core::Json& link, const std::optional<fs::path>& book_dir);

// A linked contents' state: "ok" (the file holds the bytes the book has), "modified" (the file holds others: 変更された
// コンテンツを更新 takes them), "missing" (no file at its path or its place: the book draws its copy).
struct LinkState {
    std::string contents;  // its id
    std::string name;
    std::string path;
    std::string relative;
    std::string status;
    std::optional<fs::path> found;
    std::optional<core::smart::Showing> shown;  // a smart object showing them (what an op names)
};
std::vector<LinkState> link_states(const core::Document& doc);
core::Json to_json(const LinkState& state);

// What a smart object's contents are written as: a file's own bytes and suffix (".png", …), a page's "psd" or "png".
std::string export_suffix(const core::SmartContents& contents, const std::string& format);
// The contents written to `dest` (a file's own bytes; a page as a PSD — its layers — or a PNG of its layers over nothing,
// at its dpi): core::Error("value") for a format a page is not written in, ("not_found") for a file the book does not hold.
// {"path", "kind", "format", "bytes"}.
core::Json export_contents(const core::Document& doc, const std::string& contents_id, const fs::path& dest, std::string format = {});

// The book at `book` packed into `out` (パッケージ…): `out`/<the book's folder name> — a new book (its own book_id, history
// from here) with every file of the book's (storage::copy_assets) and its linked files' copies in its Links/, each linked
// to that copy ("relative": "Links/<name>"). Throws core::Error("exists") when a book is there, ("read_only") for a book
// that is not whole. {"ok", "book", "links": [paths], "relinked": n}.
core::Json package(const fs::path& book, const fs::path& out, const std::string& actor);

}  // namespace genko::formats::smart_files
