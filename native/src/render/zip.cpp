// Zip archives for material packs (Python's zipfile as materials.import_pack and export_pack use it).

#include "render/zip.hpp"

#include <QDateTime>

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

void write(const std::filesystem::path& path, const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<Entry> entries;
    entries.reserve(files.size());
    for (const auto& [name, content] : files) entries.push_back(Entry{name, content, true});
    storage::write_atomic(path, archive(entries));
}

}  // namespace genko::render::zip
