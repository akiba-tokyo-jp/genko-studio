#include "formats/output.hpp"

#include <QRandomGenerator>

#include <cerrno>
#include <cstdio>
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

fs::path part_beside(const fs::path& dest) {
    for (;;) {
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(QRandomGenerator::system()->generate64()));
        const fs::path part = dest.parent_path() / core::path_from_utf8("." + core::path_to_utf8(dest.filename()) + "." + hex + ".part");
        std::error_code ignored;
        if (!fs::exists(part, ignored)) return part;
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

PartFile::PartFile(fs::path dest) : dest_(std::move(dest)), part_(part_beside(dest_)) {
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
    for (PartFile& file : files_) file.commit();
    files_.clear();
}

std::string text(const fs::path& path) { return core::path_to_utf8(path); }

}  // namespace genko::formats::detail
