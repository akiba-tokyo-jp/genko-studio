#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Zip files as Python's zipfile reads and writes them for material packs (materials.import_pack, export_pack) and
// e-books (export.export_epub): the files of an archive, stored or deflated, and a new archive written deflated
// (ZIP_DEFLATED) or with some files stored (ZIP_STORED: an EPUB's mimetype and pictures). No encryption, no
// zip64, no other compression: those are refused (core::PyValueError) where Python would read them, and so are
// archives past the caps below, so a pack can never fill the memory or the disk.

namespace genko::render::zip {

struct Limits {
    std::int64_t max_archive_bytes = 1ll << 30;     // the .zip itself
    std::int64_t max_file_bytes = 64ll << 20;       // one file, unpacked
    std::int64_t max_total_bytes = 512ll << 20;     // every file, unpacked
    std::size_t max_files = 10000;
};

// The files of an archive by name ("a/b.png"; directories left out, a later file of the same name in place of an
// earlier one). A name that would land outside the folder it is unpacked into ("/x", "../x", "a/../../x", "C:x",
// "a\\..\\x") is refused with "the pack has a file outside itself", as Python's import_pack refuses it.
std::map<std::string, std::string> read(const std::filesystem::path& path, const Limits& limits = {});

// Where ZipFile.extractall puts a member (a name already found inside): its parts that are empty, "." or ".." left
// out (and on Windows, a drive left out, :<>|"?* made _ and trailing dots taken off); "" for none.
std::string member_path(const std::string& name);

// A path inside a pack as `Path(pack) / path` finds it (pack.json's "file"): its empty and "." parts left out; nothing
// for one that leaves the pack (absolute, a drive, a ".." part), which is never read.
std::optional<std::string> inside_path(const std::string& path);

// The files (name, bytes) as a new archive at `path`, each deflated (zlib's default level), written whole or not at
// all. core::Error("io") when it cannot be written.
void write(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::string>>& files);

// One file of an archive as ZipFile.writestr writes it: deflated (ZIP_DEFLATED, zlib's default level) or stored
// (ZIP_STORED).
struct Entry {
    std::string name;
    std::string data;
    bool deflated = true;
};

// The entries as the bytes of a new archive, in order, as ZipFile writes them (an EPUB's zip: export_epub), each
// stamped with the local time it is written.
std::string archive(const std::vector<Entry>& entries);

}  // namespace genko::render::zip
