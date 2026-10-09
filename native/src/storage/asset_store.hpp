#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace genko::storage {

// Content-addressed files under <project>/assets/<ab>/<sha256><suffix>, never rewritten (Python's AssetStore;
// docs/cpp-migration/schema-v4.md §1). Refs are "sha256:" + 64 lowercase hex digits; a suffix starts with "." and
// holds no "/", "\" or "..". Anything else throws core::Error("format"): a ref read from a file can never point
// outside assets/.
class AssetStore {
public:
    explicit AssetStore(std::filesystem::path project);

    static std::string ref(std::string_view bytes);
    static bool is_ref(std::string_view ref);
    static bool is_suffix(std::string_view suffix);
    // "assets/ab/<64 hex><suffix>"
    static std::string relpath(std::string_view ref, std::string_view suffix);

    std::filesystem::path path(std::string_view ref, std::string_view suffix) const;
    bool has(std::string_view ref, std::string_view suffix) const;

    // Store bytes (unless a file with that name is there already: it holds the same bytes) and return their ref.
    // The file is written atomically (temporary file, fsync, rename).
    std::string put_bytes(std::string_view bytes, std::string_view suffix);
    // The same for bytes whose ref is already known (not hashed again).
    void put_known(std::string_view ref, std::string_view bytes, std::string_view suffix);

    // The bytes, or nothing when there is no such file.
    std::optional<std::string> get_bytes(std::string_view ref, std::string_view suffix, std::optional<std::size_t> maximum = {}) const;

    // Every stored file (not the temporary files of unfinished writes), sorted.
    std::vector<std::filesystem::path> all_files() const;

    const std::filesystem::path& project() const { return project_; }
    std::filesystem::path root() const { return project_ / "assets"; }

private:
    std::filesystem::path project_;
};

// merge.copy_assets(src, dest) (作品の結合: the other book's pictures before its pages are taken in by import_pages): every
// asset file of the book at `src` copied into the book at `dest`, those it already has left; how many were copied. Only
// what is laid out as an asset is (assets/<ab>/<64 hex><suffix>, a regular file, not a temporary one) and holds the bytes
// its name says (Python copies any file there, as it is): a link is not followed (neither to a file nor to a folder, nor
// assets/ itself), so nothing outside the other book is read, and nothing that would stand in for an asset's bytes
// comes in. Each is written as AssetStore writes (atomically). Throws core::Error("io") when a file cannot be read or
// written.
std::size_t copy_assets(const std::filesystem::path& src, const std::filesystem::path& dest);

}  // namespace genko::storage
