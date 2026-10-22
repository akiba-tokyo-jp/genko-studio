#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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

// Files on disk packed one at a time into a new archive at `path` (deflated, as ZipFile.write packs them for a
// backup): each file is read, deflated and written before the next, so the archive is never held in memory; past
// 4 GiB or 65535 files the zip64 records are written, as ZipFile writes them. Each entry carries its file's time
// (local, two-second steps). Written to `path` itself (the caller writes it under a temporary name and renames it);
// core::Error("io") when a file cannot be read or the archive written. `zip64_from` (tests): the size from which the
// zip64 records are used.
void pack(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::filesystem::path>>& files,
          std::uint64_t zip64_from = 0xffffffffull);

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

// PC4 F-f (.pcraft, PhotoCraft's crates/format/src/zip.rs): an archive whose files are read one at a time by name through
// its central directory — stored or deflated (a .pcraft unpacked and zipped again by another tool still reads), each
// checked against its CRC-32 —, nothing read before it is asked for. No zip64, encryption or other compression (refused,
// as PhotoCraft refuses them). The bytes stay the caller's (the reader holds a view of them).
class NamedReader {
public:
    // core::Error("format", "the .pcraft is broken (zip: …)") for bytes that are not such an archive: no end record, a
    // central directory or a file outside the bytes, an encrypted file.
    explicit NamedReader(std::string_view bytes);
    bool contains(std::string_view name) const;
    // The size a file says it unpacks to (none: no such file).
    std::optional<std::uint64_t> unpacked_size(std::string_view name) const;
    // The file's bytes: core::Error("format") when there is no such file, or it is broken (its CRC, its size, its deflate
    // stream), or of another compression; core::Error("image_too_large", "… too large to read (at most `max` bytes)") when
    // it says it unpacks past `max` — refused before anything is unpacked.
    std::string read(std::string_view name, std::uint64_t max) const;
    std::vector<std::string> names() const;

private:
    struct Item {
        std::uint16_t method = 0;
        std::uint32_t crc = 0;
        std::uint64_t packed = 0;
        std::uint64_t size = 0;
        std::uint64_t at = 0;  // (its data, past its local header)
    };
    std::string_view bytes_;
    std::map<std::string, Item, std::less<>> items_;  // (a later file of a name already met left out)
    std::vector<std::string> order_;                  // (the names in the archive's order)
    const Item* find(std::string_view name) const;
};

// PC4 F-f: a new archive of stored files (PhotoCraft's ZipWriter: STORE, UTF-8 names, the time 1980-01-01 00:00), its
// bytes handed to `sink` as they are made — each file's local header and bytes when it is added, the central directory
// when it is finished —, so a large .pcraft is never held whole. core::Error("image_too_large", "the .pcraft would be
// larger than 4 GiB …") past what a zip without zip64 holds (4 GiB, 65535 files) — before the file is written.
class StoreWriter {
public:
    explicit StoreWriter(std::function<void(std::string_view)> sink);
    void add(std::string_view name, std::string_view data);
    void finish();
    std::uint64_t written() const { return offset_; }

private:
    std::function<void(std::string_view)> sink_;
    std::string central_;
    std::uint64_t offset_ = 0;
    std::uint32_t count_ = 0;
    bool finished_ = false;
};

}  // namespace genko::render::zip
