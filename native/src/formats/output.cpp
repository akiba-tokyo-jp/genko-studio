#include "formats/output.hpp"

#include <QRandomGenerator>

#include <cerrno>
#include <cstdio>
#include <optional>
#include <system_error>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "storage/fsutil.hpp"

namespace genko::formats::detail {

namespace fs = std::filesystem;

namespace {

[[noreturn]] void io_error(const std::error_code& ec, const fs::path& path) {
    throw core::Error("io", storage::os_error_text(ec.value() != 0 ? ec.value() : EIO, path));
}

// A name beside `dest` nothing else uses, of its own length whatever the name of `dest` (one of 255 bytes too has
// room for its file in the making): .genko-<16 hex><ext>.
fs::path beside(const fs::path& dest, std::string_view ext) {
    for (;;) {
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(QRandomGenerator::system()->generate64()));
        const fs::path path = dest.parent_path() / core::path_from_utf8(".genko-" + std::string(hex) + std::string(ext));
        std::error_code ignored;
        if (!fs::exists(fs::symlink_status(path, ignored))) return path;
    }
}

}  // namespace

void make_dirs(const fs::path& dir) {
    if (dir.empty()) return;
    std::error_code ec;
    if (fs::is_directory(dir, ec)) return;
    if (fs::exists(dir, ec)) throw core::Error("io", storage::os_error_text(EEXIST, dir));
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir)) io_error(ec, dir);
}

PartFile::PartFile(fs::path dest) : dest_(std::move(dest)), part_(beside(dest_, ".part")) {
    file_.open(part_, std::ios::binary | std::ios::trunc);
    if (!file_) io_error(std::error_code(errno, std::generic_category()), dest_);
}

PartFile::PartFile(PartFile&& other) noexcept
    : dest_(std::move(other.dest_)), part_(std::move(other.part_)), file_(std::move(other.file_)), size_(other.size_), done_(other.done_) {
    other.done_ = true;  // (the moved-from one owns nothing)
}

PartFile::~PartFile() {
    if (done_) return;
    if (file_.is_open()) file_.close();
    std::error_code ignored;
    fs::remove(part_, ignored);
}

void PartFile::write(std::string_view bytes) {
    file_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file_) io_error(std::error_code(EIO, std::generic_category()), dest_);
    size_ += bytes.size();
}

void PartFile::close() {
    if (!file_.is_open()) return;
    file_.close();
    if (!file_) io_error(std::error_code(EIO, std::generic_category()), dest_);
}

void PartFile::commit() {
    close();
    std::error_code ec;
    fs::rename(part_, dest_, ec);
    if (ec) io_error(ec, dest_);
    done_ = true;
}

void Output::put(const fs::path& path, std::string_view bytes) {
    PartFile file(path);
    file.write(bytes);
    file.close();
    files_.push_back(std::move(file));
}

void Output::put(PartFile&& file) {
    file.close();
    files_.push_back(std::move(file));
}

void Output::commit() {
    // every place first: one a file cannot go to (a folder there) stops the export before any file is moved
    for (const PartFile& file : files_) {
        std::error_code ec;
        if (fs::is_directory(fs::symlink_status(file.dest(), ec))) io_error(std::error_code(EISDIR, std::generic_category()), file.dest());
    }
    // the files there before are set aside until every new one is in place, and put back when one cannot be moved in
    struct Moved {
        fs::path dest;
        std::optional<fs::path> aside;
        bool placed = false;
    };
    std::vector<Moved> moved;
    const auto fail = [&](const std::error_code& ec, const fs::path& path) {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code ignored;
            if (it->placed) fs::remove(it->dest, ignored);
            if (it->aside) fs::rename(*it->aside, it->dest, ignored);
        }
        io_error(ec, path);
    };
    for (PartFile& file : files_) {
        moved.push_back(Moved{file.dest(), std::nullopt, false});
        std::error_code ec;
        if (fs::exists(fs::symlink_status(file.dest(), ec))) {
            moved.back().aside = beside(file.dest(), ".old");
            fs::rename(file.dest(), *moved.back().aside, ec);
            if (ec) {
                moved.back().aside.reset();
                fail(ec, file.dest());
            }
        }
        fs::rename(file.part_, file.dest(), ec);
        if (ec) fail(ec, file.dest());
        moved.back().placed = true;
        file.done_ = true;
    }
    for (const Moved& m : moved) {
        std::error_code ignored;
        if (m.aside) fs::remove(*m.aside, ignored);
    }
    files_.clear();
}

std::string text(const fs::path& path) { return core::path_to_utf8(path); }

}  // namespace genko::formats::detail
