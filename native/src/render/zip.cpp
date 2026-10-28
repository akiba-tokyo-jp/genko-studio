// Zip archives for material packs (Python's zipfile as materials.import_pack and export_pack use it).

#include "render/zip.hpp"

#include <QDateTime>
#include <QFileInfo>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string_view>

#include <zlib.h>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
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

// The upper half of code page 437, as Python's zipfile reads a name without the UTF-8 flag (0x00-0x7f are ASCII).
const char16_t kCp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

// A name as Python's zipfile has it: UTF-8 when the archive says so (flag 0x800), else code page 437; cut at a NUL;
// on Windows "\" is "/".
std::string decoded_name(std::string_view raw, bool utf8) {
    std::string name;
    if (utf8) {
        if (core::utf8_error(raw)) bad("a name inside is not UTF-8");
        name.assign(raw);
    } else {
        for (const char c : raw) {
            const auto byte = static_cast<unsigned char>(c);
            const char32_t u = byte < 0x80 ? byte : kCp437[byte - 0x80];
            if (u < 0x80) {
                name.push_back(static_cast<char>(u));
            } else if (u < 0x800) {
                name.push_back(static_cast<char>(0xC0 | (u >> 6)));
                name.push_back(static_cast<char>(0x80 | (u & 0x3F)));
            } else {
                name.push_back(static_cast<char>(0xE0 | (u >> 12)));
                name.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
                name.push_back(static_cast<char>(0x80 | (u & 0x3F)));
            }
        }
    }
    if (const std::size_t nul = name.find('\0'); nul != std::string::npos) name.resize(nul);
#ifdef _WIN32
    std::replace(name.begin(), name.end(), '\\', '/');
#endif
    return name;
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

std::string member_path(const std::string& name) {
    std::string out;
    std::size_t start = 0;
#ifdef _WIN32
    if (name.size() >= 2 && name[1] == ':') start = 2;  // (os.path.splitdrive)
    const char* separators = "/\\";
#else
    const char* separators = "/";
#endif
    while (start <= name.size()) {
        const std::size_t end = name.find_first_of(separators, start);
        std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
#ifdef _WIN32
        for (char& c : part) {
            if (std::string_view(":<>|\"?*").find(c) != std::string_view::npos) c = '_';
        }
        while (!part.empty() && part.back() == '.') part.pop_back();
#endif
        if (!part.empty() && part != "." && part != "..") out += (out.empty() ? "" : "/") + part;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::optional<std::string> inside_path(const std::string& path) {
    if (!inside(path)) return std::nullopt;
    std::string out;
    std::size_t start = 0;
#ifdef _WIN32
    const char* separators = "/\\";
#else
    const char* separators = "/";  // (a "\\" is part of a name here, as on this system's disks)
#endif
    while (start <= path.size()) {
        const std::size_t end = path.find_first_of(separators, start);
        const std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + part;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (out.empty()) return std::nullopt;
    return out;
}

std::map<std::string, std::string> read(const std::filesystem::path& path, const Limits& limits) {
    std::error_code ec;
    const auto length = std::filesystem::file_size(path, ec);
    if (ec) throw core::PyUncaught("FileNotFoundError", "[Errno 2] No such file or directory: " + core::py_repr_str(core::path_to_utf8(path)));
    if (static_cast<std::int64_t>(length) > limits.max_archive_bytes) bad("it is too large");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw core::PyUncaught("PermissionError", "[Errno 13] Permission denied: " + core::py_repr_str(core::path_to_utf8(path)));
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
        const std::string name = decoded_name(b.substr(at + 46, name_len), (flags & 0x800) != 0);
        at += 46 + name_len + extra_len + comment_len;
        if (!inside(name)) throw core::PyValueError("the pack has a file outside itself");
        if (name.back() == '/') continue;  // (a folder)
        const std::string unpacked = member_path(name);  // (where ZipFile.extractall puts it)
        if (unpacked.empty()) continue;
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
        files[unpacked] = std::move(content);
    }
    return files;
}

std::string archive(const std::vector<Entry>& entries) {
    // (the time the files are written, as ZipFile.writestr stamps them: local time, two-second steps)
    const QDateTime now = QDateTime::currentDateTime();
    const std::uint32_t dos_time = std::uint32_t(now.time().hour()) << 11 | std::uint32_t(now.time().minute()) << 5 | std::uint32_t(now.time().second() / 2);
    const std::uint32_t dos_date = std::uint32_t(std::max(0, now.date().year() - 1980)) << 9 | std::uint32_t(now.date().month()) << 5 | std::uint32_t(now.date().day());
    std::string out, directory;
    for (const Entry& entry : entries) {
        const std::string& name = entry.name;
        const std::string& content = entry.data;
        if (content.size() > 0xfffffffeu || name.size() > 0xffff) throw core::PyValueError("a file is too large for the pack");
        const std::uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(content.data()), static_cast<uInt>(content.size()));
        std::string packed;
        if (entry.deflated) {
            packed.assign(compressBound(static_cast<uLong>(content.size())) + 16, '\0');
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
        } else {
            packed = content;  // (ZIP_STORED)
        }
        if (out.size() + packed.size() > 0xfffffffeu) throw core::PyValueError("the pack is too large");
        const auto offset = static_cast<std::uint32_t>(out.size());
        const bool ascii = std::all_of(name.begin(), name.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x80; });
        const auto header = [&](std::string& to, bool central) {
            put32(to, central ? 0x02014b50 : 0x04034b50);
            if (central) put16(to, 3 << 8 | 20);  // (made on Unix, zip 2.0)
            put16(to, 20);
            put16(to, ascii ? 0 : 0x800);  // (a name past ASCII is UTF-8, said so as ZipFile says it)
            put16(to, entry.deflated ? 8 : 0);
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
    if (entries.size() > 0xfffe) throw core::PyValueError("the pack holds too many files");
    const auto dir_at = static_cast<std::uint32_t>(out.size());
    out += directory;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put32(out, static_cast<std::uint32_t>(directory.size()));
    put32(out, dir_at);
    put16(out, 0);
    return out;
}

namespace {

void put64(std::string& out, std::uint64_t v) {
    put32(out, static_cast<std::uint32_t>(v & 0xffffffffu));
    put32(out, static_cast<std::uint32_t>(v >> 32));
}

std::string read_whole(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw core::Error("io", "cannot read " + core::path_to_utf8(path));
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) throw core::Error("io", "cannot read " + core::path_to_utf8(path));
    return data;
}

// raw deflate (zlib's default level), in steps a uInt can count
std::string deflated(const std::string& content) {
    z_stream z{};
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) throw core::Error("io", "zlib");
    std::string out;
    std::string chunk(1 << 20, '\0');
    std::size_t at = 0;
    int result = Z_OK;
    do {
        const std::size_t take = std::min<std::size_t>(content.size() - at, 1u << 30);
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(content.data() + at));
        z.avail_in = static_cast<uInt>(take);
        at += take;
        const int flush = at == content.size() ? Z_FINISH : Z_NO_FLUSH;
        do {
            z.next_out = reinterpret_cast<Bytef*>(chunk.data());
            z.avail_out = static_cast<uInt>(chunk.size());
            result = deflate(&z, flush);
            if (result == Z_STREAM_ERROR) {
                deflateEnd(&z);
                throw core::Error("io", "zlib");
            }
            out.append(chunk.data(), chunk.size() - z.avail_out);
        } while (z.avail_out == 0);
    } while (at < content.size());
    deflateEnd(&z);
    if (result != Z_STREAM_END) throw core::Error("io", "zlib");
    return out;
}

std::uint32_t crc_of(const std::string& content) {
    uLong crc = crc32(0, nullptr, 0);
    for (std::size_t at = 0; at < content.size();) {
        const std::size_t take = std::min<std::size_t>(content.size() - at, 1u << 30);
        crc = crc32(crc, reinterpret_cast<const Bytef*>(content.data() + at), static_cast<uInt>(take));
        at += take;
    }
    return static_cast<std::uint32_t>(crc);
}

}  // namespace

void pack(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::filesystem::path>>& files, std::uint64_t zip64_from) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw core::Error("io", "cannot write " + core::path_to_utf8(path));
    std::string directory;
    std::uint64_t offset = 0;
    bool any64 = false;
    const auto big = [zip64_from](std::uint64_t v) { return v >= zip64_from; };
    for (const auto& [name, source] : files) {
        if (name.size() > 0xffff) throw core::Error("io", "a file name is too long for the archive");
        const std::string content = read_whole(source);
        const std::string packed = deflated(content);
        const std::uint32_t crc = crc_of(content);
        // (the file's own time, as ZipFile.write takes it: local time, two-second steps)
        const QDateTime when = QFileInfo(QString::fromStdString(core::path_to_utf8(source))).lastModified();
        const std::uint32_t dos_time = std::uint32_t(when.time().hour()) << 11 | std::uint32_t(when.time().minute()) << 5 | std::uint32_t(when.time().second() / 2);
        const std::uint32_t dos_date = std::uint32_t(std::max(0, when.date().year() - 1980)) << 9 | std::uint32_t(when.date().month()) << 5 | std::uint32_t(when.date().day());
        const std::uint64_t size = content.size();
        const std::uint64_t csize = packed.size();
        const bool sizes64 = big(size) || big(csize);
        const bool offset64 = big(offset);
        any64 = any64 || sizes64 || offset64;
        const bool ascii = std::all_of(name.begin(), name.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x80; });
        // the local header (zip64: both sizes in its extra field)
        std::string local;
        put32(local, 0x04034b50);
        put16(local, sizes64 ? 45 : 20);
        put16(local, ascii ? 0 : 0x800);
        put16(local, 8);
        put16(local, dos_time);
        put16(local, dos_date);
        put32(local, crc);
        put32(local, sizes64 ? 0xffffffffu : static_cast<std::uint32_t>(csize));
        put32(local, sizes64 ? 0xffffffffu : static_cast<std::uint32_t>(size));
        put16(local, static_cast<std::uint32_t>(name.size()));
        put16(local, sizes64 ? 20 : 0);
        local += name;
        if (sizes64) {
            put16(local, 0x0001);
            put16(local, 16);
            put64(local, size);
            put64(local, csize);
        }
        out.write(local.data(), static_cast<std::streamsize>(local.size()));
        out.write(packed.data(), static_cast<std::streamsize>(packed.size()));
        if (!out) throw core::Error("io", "cannot write " + core::path_to_utf8(path));
        // its entry in the central directory (zip64: the fields that overflowed, in order)
        std::string extra;
        if (big(size)) put64(extra, size);
        if (big(csize)) put64(extra, csize);
        if (offset64) put64(extra, offset);
        put32(directory, 0x02014b50);
        put16(directory, 3 << 8 | (extra.empty() ? 20 : 45));  // (made on Unix)
        put16(directory, extra.empty() ? 20 : 45);
        put16(directory, ascii ? 0 : 0x800);
        put16(directory, 8);
        put16(directory, dos_time);
        put16(directory, dos_date);
        put32(directory, crc);
        put32(directory, big(csize) ? 0xffffffffu : static_cast<std::uint32_t>(csize));
        put32(directory, big(size) ? 0xffffffffu : static_cast<std::uint32_t>(size));
        put16(directory, static_cast<std::uint32_t>(name.size()));
        put16(directory, extra.empty() ? 0 : static_cast<std::uint32_t>(extra.size() + 4));
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, 0100644u << 16);  // (a plain file, -rw-r--r--)
        put32(directory, offset64 ? 0xffffffffu : static_cast<std::uint32_t>(offset));
        directory += name;
        if (!extra.empty()) {
            put16(directory, 0x0001);
            put16(directory, static_cast<std::uint32_t>(extra.size()));
            directory += extra;
        }
        offset += local.size() + packed.size();
    }
    const std::uint64_t count = files.size();
    const std::uint64_t dir_at = offset;
    const std::uint64_t dir_size = directory.size();
    std::string end;
    if (any64 || count >= 0xffff || big(dir_at) || big(dir_size)) {
        // the zip64 end of the central directory and its locator
        put32(end, 0x06064b50);
        put64(end, 44);
        put16(end, 3 << 8 | 45);
        put16(end, 45);
        put32(end, 0);
        put32(end, 0);
        put64(end, count);
        put64(end, count);
        put64(end, dir_size);
        put64(end, dir_at);
        put32(end, 0x07064b50);
        put32(end, 0);
        put64(end, dir_at + dir_size);
        put32(end, 1);
    }
    put32(end, 0x06054b50);
    put16(end, 0);
    put16(end, 0);
    put16(end, count >= 0xffff ? 0xffffu : static_cast<std::uint32_t>(count));
    put16(end, count >= 0xffff ? 0xffffu : static_cast<std::uint32_t>(count));
    put32(end, big(dir_size) ? 0xffffffffu : static_cast<std::uint32_t>(dir_size));
    put32(end, big(dir_at) ? 0xffffffffu : static_cast<std::uint32_t>(dir_at));
    put16(end, 0);
    out.write(directory.data(), static_cast<std::streamsize>(directory.size()));
    out.write(end.data(), static_cast<std::streamsize>(end.size()));
    out.flush();
    if (!out) throw core::Error("io", "cannot write " + core::path_to_utf8(path));
    out.close();
    if (!out) throw core::Error("io", "cannot write " + core::path_to_utf8(path));
}

void write(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<Entry> entries;
    entries.reserve(files.size());
    for (const auto& [name, content] : files) entries.push_back(Entry{name, content, true});
    storage::write_atomic(path, archive(entries));
}

// --- PC4 F-f: by name, and stored as it streams (.pcraft) -----------------------------------------------------------

namespace {

[[noreturn]] void pcraft_broken(const std::string& why) { throw core::Error("format", "the .pcraft is broken (zip: " + why + ")"); }

std::uint64_t le(std::string_view b, std::uint64_t at, int n) {
    if (at > b.size() || static_cast<std::uint64_t>(n) > b.size() - at) pcraft_broken("it ends too soon");
    std::uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = v << 8 | static_cast<std::uint8_t>(b[static_cast<std::size_t>(at) + static_cast<std::size_t>(i)]);
    return v;
}

std::uint32_t crc_of_view(std::string_view data) {
    uLong crc = crc32(0, nullptr, 0);
    for (std::size_t at = 0; at < data.size();) {
        const std::size_t take = std::min<std::size_t>(data.size() - at, 1u << 30);
        crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data() + at), static_cast<uInt>(take));
        at += take;
    }
    return static_cast<std::uint32_t>(crc);
}

}  // namespace

NamedReader::NamedReader(std::string_view bytes) : bytes_(bytes) {
    constexpr std::uint32_t kEnd = 0x06054b50, kCentral = 0x02014b50, kLocal = 0x04034b50;
    if (bytes.size() < 22) pcraft_broken("not a zip archive");
    // (the end record: from the last 22 bytes back over a comment of at most 65535)
    std::uint64_t end = bytes.size() - 22;
    const std::uint64_t lowest = bytes.size() >= 22 + 65535 ? bytes.size() - 22 - 65535 : 0;
    for (;; --end) {
        if (le(bytes, end, 4) == kEnd) break;
        if (end == lowest) pcraft_broken("no end of central directory");
    }
    const std::uint64_t count = le(bytes, end + 10, 2), cd_size = le(bytes, end + 12, 4), cd_at = le(bytes, end + 16, 4);
    if (cd_at + cd_size > end) pcraft_broken("the central directory is outside the archive");
    std::uint64_t p = cd_at;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (p + 46 > end || le(bytes, p, 4) != kCentral) pcraft_broken("a bad central directory entry");
        const std::uint64_t flags = le(bytes, p + 8, 2);
        Item item;
        item.method = static_cast<std::uint16_t>(le(bytes, p + 10, 2));
        item.crc = static_cast<std::uint32_t>(le(bytes, p + 16, 4));
        item.packed = le(bytes, p + 20, 4);
        item.size = le(bytes, p + 24, 4);
        const std::uint64_t name_len = le(bytes, p + 28, 2), extra_len = le(bytes, p + 30, 2), comment_len = le(bytes, p + 32, 2);
        const std::uint64_t local = le(bytes, p + 42, 4);
        if (p + 46 + name_len > end) pcraft_broken("a name outside the central directory");
        std::string name(bytes.substr(static_cast<std::size_t>(p + 46), static_cast<std::size_t>(name_len)));
        if ((flags & 1) != 0) throw core::Error("format", "the .pcraft is broken (zip: an encrypted file, " + name + ")");
        if (local + 30 > bytes.size() || le(bytes, local, 4) != kLocal) pcraft_broken("a bad local header");
        item.at = local + 30 + le(bytes, local + 26, 2) + le(bytes, local + 28, 2);
        if (item.at > bytes.size() || item.packed > bytes.size() - item.at) pcraft_broken("a file outside the archive (" + name + ")");
        if (find(name) == nullptr) {
            order_.push_back(name);
            items_.emplace(std::move(name), item);
        }
        p += 46 + name_len + extra_len + comment_len;
    }
}

const NamedReader::Item* NamedReader::find(std::string_view name) const {
    const auto it = items_.find(name);
    return it == items_.end() ? nullptr : &it->second;
}

bool NamedReader::contains(std::string_view name) const { return find(name) != nullptr; }

std::optional<std::uint64_t> NamedReader::unpacked_size(std::string_view name) const {
    const Item* item = find(name);
    if (item == nullptr) return std::nullopt;
    return item->size;
}

std::vector<std::string> NamedReader::names() const { return order_; }

std::string NamedReader::read(std::string_view name, std::uint64_t max) const {
    const Item* item = find(name);
    if (item == nullptr) pcraft_broken("no " + std::string(name));
    if (item->size > max) {
        throw core::Error("image_too_large", "the .pcraft holds a file too large to read (" + std::string(name) + ": " + std::to_string(item->size) +
                                                 " bytes; at most " + std::to_string(max) + ")");
    }
    const std::string_view packed = bytes_.substr(static_cast<std::size_t>(item->at), static_cast<std::size_t>(item->packed));
    std::string data;
    if (item->method == 0) {
        if (item->packed != item->size) pcraft_broken("a stored file of the wrong size (" + std::string(name) + ")");
        data.assign(packed);
    } else if (item->method == 8) {
        data.assign(static_cast<std::size_t>(item->size), '\0');
        z_stream z{};
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) pcraft_broken("zlib");
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(packed.data()));
        z.avail_in = static_cast<uInt>(packed.size());
        z.next_out = reinterpret_cast<Bytef*>(data.data());
        z.avail_out = static_cast<uInt>(data.size());
        const int result = inflate(&z, Z_FINISH);
        const bool whole = result == Z_STREAM_END && z.total_out == static_cast<uLong>(item->size);
        inflateEnd(&z);
        if (!whole) pcraft_broken("a deflated file is broken (" + std::string(name) + ")");
    } else {
        throw core::Error("format", "the .pcraft is broken (zip: compression method " + std::to_string(item->method) + " of " + std::string(name) + ")");
    }
    if (crc_of_view(data) != item->crc) pcraft_broken("a file fails its CRC check (" + std::string(name) + ")");
    return data;
}

StoreWriter::StoreWriter(std::function<void(std::string_view)> sink) : sink_(std::move(sink)) {}

std::uint32_t crc32_of(std::string_view data, std::uint32_t crc) {
    while (!data.empty()) {
        const auto n = static_cast<uInt>(std::min<std::size_t>(data.size(), 1u << 30));
        crc = static_cast<std::uint32_t>(::crc32(crc, reinterpret_cast<const Bytef*>(data.data()), n));
        data.remove_prefix(n);
    }
    return crc;
}

void StoreWriter::add(std::string_view name, std::string_view data) {
    add_known(name, data.size(), crc_of_view(data), [data](const std::function<void(std::string_view)>& put) { put(data); });
}

void StoreWriter::add_known(std::string_view name, std::uint64_t size, std::uint32_t crc,
                            const std::function<void(const std::function<void(std::string_view)>&)>& data) {
    if (finished_) throw core::Error("value", "the archive is finished");
    const std::uint64_t record = 30 + name.size() + size;
    // (what the archive would be with this file and the central directory and end record after it)
    if (count_ >= 0xffff || size > 0xffffffffull || offset_ + record + central_.size() + 46 + name.size() + 22 > 0xffffffffull || name.size() > 0xffff) {
        throw core::Error("image_too_large", "the .pcraft would be larger than 4 GiB or hold more than 65535 files (a zip without zip64)");
    }
    std::string head;
    put32(head, 0x04034b50);
    put16(head, 20);      // (version needed)
    put16(head, 0x0800);  // (UTF-8 names)
    put16(head, 0);       // (stored)
    put16(head, 0);       // (time)
    put16(head, 0x21);    // (1980-01-01)
    put32(head, crc);
    put32(head, static_cast<std::uint32_t>(size));
    put32(head, static_cast<std::uint32_t>(size));
    put16(head, static_cast<std::uint32_t>(name.size()));
    put16(head, 0);
    head.append(name);
    put32(central_, 0x02014b50);
    put16(central_, 20);
    put16(central_, 20);
    put16(central_, 0x0800);
    put16(central_, 0);
    put16(central_, 0);
    put16(central_, 0x21);
    put32(central_, crc);
    put32(central_, static_cast<std::uint32_t>(size));
    put32(central_, static_cast<std::uint32_t>(size));
    put16(central_, static_cast<std::uint32_t>(name.size()));
    central_.append(8, '\0');  // (extra and comment lengths, disk, internal attributes)
    put32(central_, 0);        // (external attributes)
    put32(central_, static_cast<std::uint32_t>(offset_));
    central_.append(name);
    sink_(head);
    std::uint64_t given = 0;
    data([&](std::string_view part) {
        given += part.size();
        if (given > size) throw core::Error("io", "an archive's file is longer than it said");
        sink_(part);
    });
    if (given != size) throw core::Error("io", "an archive's file is shorter than it said");
    offset_ += record;
    ++count_;
}

void StoreWriter::finish() {
    if (finished_) return;
    finished_ = true;
    std::string tail = central_;
    put32(tail, 0x06054b50);
    put32(tail, 0);  // (disk numbers)
    put16(tail, count_);
    put16(tail, count_);
    put32(tail, static_cast<std::uint32_t>(central_.size()));
    put32(tail, static_cast<std::uint32_t>(offset_));
    put16(tail, 0);
    sink_(tail);
    offset_ += tail.size();
}

}  // namespace genko::render::zip
