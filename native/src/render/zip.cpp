// Zip archives for material packs (Python's zipfile as materials.import_pack and export_pack use it).

#include "render/zip.hpp"

#include <QDateTime>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string_view>

#include <zlib.h>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "storage/fsutil.hpp"

namespace genko::render::zip {

namespace {

[[noreturn]] void bad(const std::string& why) { throw core::PyValueError("the pack cannot be read as a zip file (" + why + ")"); }

std::uint32_t u16(std::string_view b, std::size_t at) {
    if (at + 2 > b.size()) bad("it ends too soon");
    return std::uint32_t(std::uint8_t(b[at])) | std::uint32_t(std::uint8_t(b[at + 1])) << 8;
}

std::uint32_t u32(std::string_view b, std::size_t at) {
    if (at + 4 > b.size()) bad("it ends too soon");
    return u16(b, at) | u16(b, at + 2) << 16;
}

// Whether the name stays inside the folder it is unpacked into (Python checks (tmp / member).resolve() against tmp;
// here a part that climbs out or starts elsewhere is refused before anything is read).
bool inside(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\') return false;
    if (name.size() >= 2 && name[1] == ':') return false;  // (a Windows drive)
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = name.find_first_of("/\\", start);
        const std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

std::string inflate_raw(std::string_view data, std::int64_t size) {
    std::string out(static_cast<std::size_t>(size), '\0');
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) bad("zlib");
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    const int result = inflate(&z, Z_FINISH);
    const bool whole = result == Z_STREAM_END && z.total_out == static_cast<uLong>(size);
    inflateEnd(&z);
    if (!whole) bad("a file inside is broken");
    return out;
}

void put16(std::string& out, std::uint32_t v) {
    out.push_back(char(v & 0xff));
    out.push_back(char((v >> 8) & 0xff));
}

void put32(std::string& out, std::uint32_t v) {
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

}  // namespace

std::map<std::string, std::string> read(const std::filesystem::path& path, const Limits& limits) {
    std::error_code ec;
    const auto length = std::filesystem::file_size(path, ec);
    if (ec) throw core::PyUncaught("FileNotFoundError", "[Errno 2] No such file or directory: '" + core::path_to_utf8(path) + "'");
    if (static_cast<std::int64_t>(length) > limits.max_archive_bytes) bad("it is too large");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw core::PyUncaught("PermissionError", "[Errno 13] Permission denied: '" + core::path_to_utf8(path) + "'");
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad() || bytes.size() != length) bad("it could not be read whole");
    const std::string_view b(bytes);
    // the end of the central directory: the last record, after which only its comment (at most 64 KB) can follow
    if (b.size() < 22) bad("it is not a zip file");
    std::size_t end = std::string_view::npos;
    const std::size_t lowest = b.size() > 22 + 65535 ? b.size() - 22 - 65535 : 0;
    for (std::size_t at = b.size() - 22 + 1; at-- > lowest;) {
        if (u32(b, at) == 0x06054b50 && at + 22 + u16(b, at + 20) == b.size()) {
            end = at;
            break;
        }
    }
    if (end == std::string_view::npos) bad("it is not a zip file");
    if (u16(b, end + 4) != 0 || u16(b, end + 6) != 0) bad("it is split over several files");
    const std::uint32_t count = u16(b, end + 10);
    const std::uint32_t dir_size = u32(b, end + 12);
    const std::uint32_t dir_at = u32(b, end + 16);
    if (count == 0xffff || dir_size == 0xffffffff || dir_at == 0xffffffff) bad("zip64 is not read");
    if (count > limits.max_files) bad("it holds too many files");
    if (std::uint64_t(dir_at) + dir_size > end) bad("its directory is broken");
    std::map<std::string, std::string> files;
    std::int64_t total = 0;
    std::size_t at = dir_at;
    for (std::uint32_t n = 0; n < count; ++n) {
        if (u32(b, at) != 0x02014b50) bad("its directory is broken");
        const std::uint32_t flags = u16(b, at + 8);
        const std::uint32_t method = u16(b, at + 10);
        const std::uint32_t crc = u32(b, at + 16);
        const std::uint32_t packed = u32(b, at + 20);
        const std::uint32_t size = u32(b, at + 24);
        const std::uint32_t name_len = u16(b, at + 28);
        const std::uint32_t extra_len = u16(b, at + 30);
        const std::uint32_t comment_len = u16(b, at + 32);
        const std::uint32_t local = u32(b, at + 42);
        if (at + 46 + name_len > b.size()) bad("its directory is broken");
        const std::string name(b.substr(at + 46, name_len));
        at += 46 + name_len + extra_len + comment_len;
        if (!inside(name)) throw core::PyValueError("the pack has a file outside itself");
        if (name.back() == '/') continue;  // (a folder)
        if (flags & 0x1) bad("a file inside is encrypted");
        if (packed == 0xffffffff || size == 0xffffffff || local == 0xffffffff) bad("zip64 is not read");
        if (method != 0 && method != 8) bad("a file inside is packed in a way that is not read");
        if (static_cast<std::int64_t>(size) > limits.max_file_bytes) bad("a file inside is too large");
        total += size;
        if (total > limits.max_total_bytes) bad("its files are too large");
        if (u32(b, local) != 0x04034b50) bad("a file inside is broken");
        const std::size_t data_at = local + 30 + u16(b, local + 26) + u16(b, local + 28);
        if (data_at + std::uint64_t(packed) > b.size()) bad("a file inside is broken");
        const std::string_view data = b.substr(data_at, packed);
        std::string content;
        if (method == 0) {
            if (packed != size) bad("a file inside is broken");
            content.assign(data);
        } else {
            content = inflate_raw(data, size);
        }
        if (crc32(0, reinterpret_cast<const Bytef*>(content.data()), static_cast<uInt>(content.size())) != crc) bad("a file inside is broken");
        files[name] = std::move(content);
    }
    return files;
}

void write(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::string>>& files) {
    // (the time the files are written, as ZipFile.writestr stamps them: local time, two-second steps)
    const QDateTime now = QDateTime::currentDateTime();
    const std::uint32_t dos_time = std::uint32_t(now.time().hour()) << 11 | std::uint32_t(now.time().minute()) << 5 | std::uint32_t(now.time().second() / 2);
    const std::uint32_t dos_date = std::uint32_t(std::max(0, now.date().year() - 1980)) << 9 | std::uint32_t(now.date().month()) << 5 | std::uint32_t(now.date().day());
    std::string out, directory;
    for (const auto& [name, content] : files) {
        if (content.size() > 0xfffffffeu || name.size() > 0xffff) throw core::PyValueError("a file is too large for the pack");
        const std::uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(content.data()), static_cast<uInt>(content.size()));
        std::string packed(compressBound(static_cast<uLong>(content.size())) + 16, '\0');
        z_stream z{};
        if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) throw core::Error("io", "zlib");
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(content.data()));
        z.avail_in = static_cast<uInt>(content.size());
        z.next_out = reinterpret_cast<Bytef*>(packed.data());
        z.avail_out = static_cast<uInt>(packed.size());
        const int result = deflate(&z, Z_FINISH);
        packed.resize(z.total_out);
        deflateEnd(&z);
        if (result != Z_STREAM_END) throw core::Error("io", "zlib");
        if (out.size() + packed.size() > 0xfffffffeu) throw core::PyValueError("the pack is too large");
        const auto offset = static_cast<std::uint32_t>(out.size());
        const auto header = [&](std::string& to, bool central) {
            put32(to, central ? 0x02014b50 : 0x04034b50);
            if (central) put16(to, 3 << 8 | 20);  // (made on Unix, zip 2.0)
            put16(to, 20);
            put16(to, 0x800);  // (the name is UTF-8)
            put16(to, 8);
            put16(to, dos_time);
            put16(to, dos_date);
            put32(to, crc);
            put32(to, static_cast<std::uint32_t>(packed.size()));
            put32(to, static_cast<std::uint32_t>(content.size()));
            put16(to, static_cast<std::uint32_t>(name.size()));
            put16(to, 0);
            if (central) {
                put16(to, 0);
                put16(to, 0);
                put16(to, 0);
                put32(to, 0600u << 16);  // (-rw-------, as writestr gives)
                put32(to, offset);
            }
            to += name;
        };
        header(out, false);
        out += packed;
        header(directory, true);
    }
    if (files.size() > 0xfffe) throw core::PyValueError("the pack holds too many files");
    const auto dir_at = static_cast<std::uint32_t>(out.size());
    out += directory;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint32_t>(files.size()));
    put16(out, static_cast<std::uint32_t>(files.size()));
    put32(out, static_cast<std::uint32_t>(directory.size()));
    put32(out, dir_at);
    put16(out, 0);
    storage::write_atomic(path, out);
}

}  // namespace genko::render::zip
