#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Zip files as Python's zipfile reads and writes them for material packs (materials.import_pack, export_pack): the
// files of an archive, stored or deflated, and a new archive written deflated (ZIP_DEFLATED). No encryption, no
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

// The files (name, bytes) as a new archive at `path`, each deflated (zlib's default level), written whole or not at
// all. core::Error("io") when it cannot be written.
void write(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::string>>& files);

}  // namespace genko::render::zip
