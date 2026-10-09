#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// How the exports write their files (internal to genko_formats). Every file of one export is first written beside
// where it goes and moved into place when the whole export has been made: an export that fails part way (a page this
// build cannot draw yet, a full disk) leaves none of its files, and never a cut-off one. (Python writes each file as it
// goes; on success the files are the same.)

namespace genko::formats::detail {

// Path(p).mkdir(parents=True, exist_ok=True): core::Error("io") with Python's text ("[Errno 17] File exists: '…'") when
// something that is not a folder is in the way.
void make_dirs(const std::filesystem::path& dir);

// A file written in parts: beside `dest`, under a name nothing else uses, until it is moved into place.
class Output;

class PartFile {
public:
    explicit PartFile(std::filesystem::path dest);
    ~PartFile();
    PartFile(const PartFile&) = delete;
    PartFile& operator=(const PartFile&) = delete;
    PartFile(PartFile&& other) noexcept;
    PartFile& operator=(PartFile&& other) noexcept = delete;

    void write(std::string_view bytes);
    std::uint64_t size() const { return size_; }
    // Written whole (flushed and closed); not moved yet.
    void close();
    // Moved over `dest`.
    void commit();
    const std::filesystem::path& dest() const { return dest_; }

private:
    friend class Output;

    std::filesystem::path dest_;
    std::filesystem::path part_;
    std::ofstream file_;
    std::uint64_t size_ = 0;
    bool done_ = false;
};

// The files of one export: each written whole beside its place as it is made (put), all moved into place together
// (commit), in the order they were put (a later file of the same name in place of an earlier one, as Python's writes
// leave it). Those not committed are removed. A place a file cannot go to (a folder there) is found before any file
// is moved; a file that still cannot be moved in puts back the ones moved before it (the files there before are kept
// aside until all are in).
class Output {
public:
    Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    void put(const std::filesystem::path& path, std::string_view bytes);
    // A file whose bytes come in parts (made by the caller, put here when it is closed).
    void put(PartFile&& file);
    void commit();

private:
    std::vector<PartFile> files_;
};

// str(Path): the path as Python prints it ("/" between parts on every OS: ARCHITECTURE.md §3).
std::string text(const std::filesystem::path& path);

}  // namespace genko::formats::detail
