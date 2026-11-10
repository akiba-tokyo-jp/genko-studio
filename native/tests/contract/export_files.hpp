#pragma once

// Comparing exported files with Python's (test_contract_export, test_contract_export_cli; header only, not part of the
// product). The files must be the same bytes, except where Python's own file holds the time it was made:
//   - the sRGB profile embedded in RGB files (Little CMS writes the date and time it made the profile into bytes 24–35
//     of its header; Python makes its profile in its process, this build in its own): those 12 bytes are left out, in
//     PNG (iCCP, compressed: compared uncompressed), TIFF (tag 34675), JPEG (APP2 ICC_PROFILE) and PDF (the ICCBased
//     stream, compressed: compared uncompressed, and the objects compared one by one, their offsets checked in each
//     file on its own);
//   - an EPUB's zip entries (each stamped with the local time it was written) and its dcterms:modified (the UTC time):
//     the times are left out, and content.opf is compared uncompressed with the modified time taken out.
// Everything else is compared byte for byte.

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QString>

#include <zlib.h>

#include <cstdint>
#include <map>
#include <string>

#include "testsupport.hpp"

namespace genko::test::exports {

inline std::uint32_t be32(const std::string& s, std::size_t at) {
    return (std::uint32_t(static_cast<unsigned char>(s[at])) << 24) | (std::uint32_t(static_cast<unsigned char>(s[at + 1])) << 16) |
           (std::uint32_t(static_cast<unsigned char>(s[at + 2])) << 8) | std::uint32_t(static_cast<unsigned char>(s[at + 3]));
}
inline std::uint32_t le32(const std::string& s, std::size_t at) {
    return std::uint32_t(static_cast<unsigned char>(s[at])) | (std::uint32_t(static_cast<unsigned char>(s[at + 1])) << 8) |
           (std::uint32_t(static_cast<unsigned char>(s[at + 2])) << 16) | (std::uint32_t(static_cast<unsigned char>(s[at + 3])) << 24);
}
inline std::uint16_t le16(const std::string& s, std::size_t at) {
    return static_cast<std::uint16_t>(std::uint32_t(static_cast<unsigned char>(s[at])) | (std::uint32_t(static_cast<unsigned char>(s[at + 1])) << 8));
}

// zlib (raw: a deflate stream without its header) undone; "<bad zlib>" when it is not one.
inline std::string inflated(std::string_view data, bool raw = false) {
    z_stream z{};
    if (inflateInit2(&z, raw ? -15 : 15) != Z_OK) return "<bad zlib>";
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    z.avail_in = static_cast<uInt>(data.size());
    std::string out;
    char buf[1 << 16];
    int result = Z_OK;
    do {
        z.next_out = reinterpret_cast<Bytef*>(buf);
        z.avail_out = sizeof buf;
        result = inflate(&z, Z_NO_FLUSH);
        out.append(buf, sizeof buf - z.avail_out);
    } while (result == Z_OK);
    inflateEnd(&z);
    return result == Z_STREAM_END ? out : "<bad zlib>";
}

// An ICC profile without the date and time it was made.
inline std::string undated(std::string profile) {
    if (profile.size() >= 36) profile.replace(24, 12, 12, '\0');
    return profile;
}

inline std::string png_normalized(const std::string& bytes) {
    if (bytes.size() < 8) return bytes;
    std::string out = bytes.substr(0, 8);
    std::size_t at = 8;
    while (at + 12 <= bytes.size()) {
        const std::uint32_t n = be32(bytes, at);
        if (at + 12 + n > bytes.size()) return bytes;
        const std::string type = bytes.substr(at + 4, 4);
        if (type == "iCCP") {
            const std::string data = bytes.substr(at + 8, n);
            const std::size_t zero = data.find('\0');
            out += "iCCP:" + data.substr(0, zero + 2) + undated(inflated(std::string_view(data).substr(zero + 2)));
        } else {
            out += bytes.substr(at, 12 + n);
        }
        at += 12 + n;
    }
    return out + bytes.substr(at);
}

inline std::string tiff_normalized(std::string bytes) {
    if (bytes.size() < 8 || bytes.substr(0, 2) != "II") return bytes;
    const std::uint32_t ifd = le32(bytes, 4);
    if (ifd + 2 > bytes.size()) return bytes;
    const std::uint16_t count = le16(bytes, ifd);
    for (std::uint16_t i = 0; i < count && ifd + 2 + 12 * (i + 1) <= bytes.size(); ++i) {
        const std::size_t e = ifd + 2 + 12 * i;
        if (le16(bytes, e) == 34675) {
            const std::uint32_t n = le32(bytes, e + 4);
            const std::uint32_t off = le32(bytes, e + 8);
            if (n >= 36 && off + n <= bytes.size()) bytes.replace(off + 24, 12, 12, '\0');
        }
    }
    return bytes;
}

inline std::string jpeg_normalized(std::string bytes) {
    const std::size_t at = bytes.find(std::string("ICC_PROFILE", 11) + '\0');
    if (at != std::string::npos && at + 14 + 36 <= bytes.size()) bytes.replace(at + 14 + 24, 12, 12, '\0');
    return bytes;
}

// The PDF's objects in order (the ICC profiles' streams uncompressed and undated), with "<bad offsets>" when the
// cross-reference table does not point at them, and its trailer.
inline std::string pdf_normalized(const std::string& bytes) {
    std::string out;
    std::size_t at = bytes.find("1 0 obj\n");
    if (at == std::string::npos) return bytes;
    out += bytes.substr(0, at);
    std::vector<std::size_t> offsets;
    for (int n = 1;; ++n) {
        const std::string head = std::to_string(n) + " 0 obj\n";
        if (bytes.compare(at, head.size(), head) != 0) break;
        offsets.push_back(at);
        const std::size_t end = bytes.find("\nendobj\n", at);
        if (end == std::string::npos) return bytes;
        std::string body = bytes.substr(at + head.size(), end - at - head.size());
        if (body.starts_with("<< /N ")) {
            const std::size_t s = body.find(">>\nstream\n");
            const std::size_t e = body.rfind("\nendstream");
            if (s != std::string::npos && e != std::string::npos) {
                body = "<ICC " + body.substr(0, body.find(" /Length")) + ">" + undated(inflated(std::string_view(body).substr(s + 10, e - s - 10)));
            }
        }
        out += head + body + "\n";
        at = end + 8;
    }
    const std::size_t xref = at;
    if (bytes.compare(xref, 5, "xref\n") != 0) return out + "<no xref>";
    // the table, read against the objects' offsets in this file
    std::size_t line = bytes.find('\n', bytes.find('\n', xref + 5) + 1) + 1;
    for (const std::size_t offset : offsets) {
        if (std::stoull(bytes.substr(line, 10)) != offset) out += "<bad offsets>";
        line += 20;
    }
    const std::size_t trailer = bytes.find("trailer\n", line);
    if (trailer != line) out += "<bad xref>";
    const std::size_t startxref = bytes.find("startxref\n", trailer);
    out += bytes.substr(trailer, startxref - trailer);
    if (std::stoull(bytes.substr(startxref + 10)) != xref) out += "<bad startxref>";
    return out;
}

// The zip's entries (local headers without their times; content.opf uncompressed without its modified time) and its
// central directory (without the times, and each entry's offset checked against where it is in this file).
inline std::string zip_normalized(const std::string& bytes) {
    std::string out;
    std::size_t at = 0;
    std::vector<std::size_t> offsets;
    while (at + 30 <= bytes.size() && le32(bytes, at) == 0x04034b50) {
        offsets.push_back(at);
        const std::uint16_t name_n = le16(bytes, at + 26), extra_n = le16(bytes, at + 28);
        const std::uint32_t csize = le32(bytes, at + 18);
        const std::string name = bytes.substr(at + 30, name_n);
        const std::string data = bytes.substr(at + 30 + name_n + extra_n, csize);
        std::string head = bytes.substr(at, 30);
        head.replace(10, 4, 4, '\0');  // (time, date)
        if (name == "OEBPS/content.opf") {
            head.replace(14, 8, 8, '\0');  // (crc, compressed size)
            std::string text = le16(bytes, at + 8) == 8 ? inflated(data, true) : data;
            const std::string tag = "<meta property=\"dcterms:modified\">";
            const std::size_t m = text.find(tag);
            if (m != std::string::npos) text.replace(m + tag.size(), 20, "YYYY-MM-DDTHH:MM:SSZ");
            out += head + name + bytes.substr(at + 30 + name_n, extra_n) + text;
        } else {
            out += head + bytes.substr(at + 30, name_n + extra_n) + data;
        }
        at += 30 + name_n + extra_n + csize;
    }
    std::size_t entry = 0;
    while (at + 46 <= bytes.size() && le32(bytes, at) == 0x02014b50) {
        const std::uint16_t name_n = le16(bytes, at + 28), extra_n = le16(bytes, at + 30), comment_n = le16(bytes, at + 32);
        std::string head = bytes.substr(at, 46);
        head.replace(12, 4, 4, '\0');
        const std::string name = bytes.substr(at + 46, name_n);
        if (name == "OEBPS/content.opf") head.replace(16, 8, 8, '\0');
        if (entry >= offsets.size() || le32(bytes, at + 42) != offsets[entry]) out += "<bad offset>";
        head.replace(42, 4, 4, '\0');
        out += head + bytes.substr(at + 46, name_n + extra_n + comment_n);
        at += 46 + name_n + extra_n + comment_n;
        ++entry;
    }
    if (at + 22 <= bytes.size() && le32(bytes, at) == 0x06054b50) {
        std::string end = bytes.substr(at);
        end.replace(12, 8, 8, '\0');  // (the directory's size and offset follow the entries)
        out += end;
    } else {
        out += "<no end record>";
    }
    return out;
}

inline std::string normalized(const QString& path, const std::string& bytes) {
    const QString lower = path.toLower();
    if (lower.endsWith(".png")) return png_normalized(bytes);
    if (lower.endsWith(".tif") || lower.endsWith(".tiff")) return tiff_normalized(bytes);
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return jpeg_normalized(bytes);
    if (lower.endsWith(".pdf")) return pdf_normalized(bytes);
    if (lower.endsWith(".epub")) return zip_normalized(bytes);
    return bytes;
}

// Every file under `dir` (relative path, "/" between folders) → its bytes, normalized.
inline std::map<QString, std::string> tree(const QString& dir) {
    std::map<QString, std::string> out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        out[QDir(dir).relativeFilePath(file)] = normalized(file, read_bytes(file));
    }
    return out;
}

// How many files of `want_dir` are in `got_dir` with the very same bytes (not only after the times are left out).
inline int same_bytes(const QString& got_dir, const QString& want_dir) {
    int n = 0;
    QDirIterator it(want_dir, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString other = got_dir + "/" + QDir(want_dir).relativeFilePath(file);
        if (QFileInfo::exists(other) && read_bytes(other) == read_bytes(file)) ++n;
    }
    return n;
}

// The first difference between two trees ("" when they are the same).
inline QString tree_difference(const QString& got_dir, const QString& want_dir) {
    const auto got = tree(got_dir);
    const auto want = tree(want_dir);
    for (const auto& [name, bytes] : want) {
        const auto it = got.find(name);
        if (it == got.end()) return "missing " + name;
        if (it->second != bytes) {
            std::size_t i = 0;
            while (i < bytes.size() && i < it->second.size() && bytes[i] == it->second[i]) ++i;
            return QStringLiteral("%1 differs at byte %2 (sizes %3, Python's %4)").arg(name).arg(i).arg(it->second.size()).arg(bytes.size());
        }
    }
    for (const auto& [name, bytes] : got) {
        if (!want.contains(name)) return "extra " + name;
    }
    return {};
}

}  // namespace genko::test::exports
