// What a file says besides its pixels (render/codecs/metadata.hpp).

#include "render/codecs/metadata.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "core/exif.hpp"

namespace genko::render::codecs {

namespace {

constexpr std::size_t kMetaMax = std::size_t(64) << 20;  // (an EXIF block or an XMP packet past 64 MB is not taken)

std::uint32_t be32(std::string_view b, std::size_t at) {
    return (std::uint32_t(static_cast<unsigned char>(b[at])) << 24) | (std::uint32_t(static_cast<unsigned char>(b[at + 1])) << 16) |
           (std::uint32_t(static_cast<unsigned char>(b[at + 2])) << 8) | std::uint32_t(static_cast<unsigned char>(b[at + 3]));
}
std::uint32_t le32(std::string_view b, std::size_t at) {
    return std::uint32_t(static_cast<unsigned char>(b[at])) | (std::uint32_t(static_cast<unsigned char>(b[at + 1])) << 8) |
           (std::uint32_t(static_cast<unsigned char>(b[at + 2])) << 16) | (std::uint32_t(static_cast<unsigned char>(b[at + 3])) << 24);
}

// A TIFF structure (a file's, its EXIF's): core/exif's reader, the one this build has (PC4 D6's review P3-4).
using Tiff = core::exif::Tiff;

// A SHORT or LONG entry's first value in the IFD at `ifd`.
std::optional<std::uint32_t> whole_of(const Tiff& t, std::size_t ifd, std::uint16_t tag) {
    const auto e = t.entry(ifd, tag);
    return e ? t.whole(*e) : std::nullopt;
}

std::optional<std::string> inflate_text(std::string_view data) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return std::nullopt;
    std::string out;
    char buffer[16384];
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    z.avail_in = static_cast<uInt>(data.size());
    int status = Z_OK;
    while (status == Z_OK) {
        z.next_out = reinterpret_cast<Bytef*>(buffer);
        z.avail_out = sizeof buffer;
        status = inflate(&z, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) break;
        out.append(buffer, sizeof buffer - z.avail_out);
        if (out.size() > kMetaMax) break;
    }
    inflateEnd(&z);
    if (status != Z_STREAM_END) return std::nullopt;
    return out;
}

void png(std::string_view b, FileMetadata& m) {
    std::size_t at = 8;
    while (at + 12 <= b.size()) {
        const std::uint32_t length = be32(b, at);
        if (length > b.size() - at - 12) break;
        const std::string_view type = b.substr(at + 4, 4);
        const std::string_view data = b.substr(at + 8, length);
        if (type == "IHDR" && data.size() >= 13) {
            const auto depth = static_cast<unsigned char>(data[8]), colour = static_cast<unsigned char>(data[9]);
            m.palette = colour == 3;
            m.one_bit = colour == 0 && depth == 1;
        } else if (type == "eXIf" && !m.exif && length <= kMetaMax) {
            m.exif = std::string(data);
        } else if (type == "acTL" && data.size() >= 4) {
            m.frames = static_cast<int>(std::min<std::uint32_t>(be32(data, 0), 1u << 20));
        } else if (type == "iTXt" && !m.xmp) {
            const std::size_t key_end = data.find('\0');
            if (key_end != std::string_view::npos && data.substr(0, key_end) == "XML:com.adobe.xmp" && key_end + 2 < data.size()) {
                const bool compressed = data[key_end + 1] != 0;
                const std::size_t language_end = data.find('\0', key_end + 3);
                const std::size_t translated_end = language_end == std::string_view::npos ? language_end : data.find('\0', language_end + 1);
                if (translated_end != std::string_view::npos) {
                    const std::string_view text = data.substr(translated_end + 1);
                    if (compressed) {
                        m.xmp = inflate_text(text);
                    } else {
                        m.xmp = std::string(text);
                    }
                }
            }
        } else if (type == "IEND") {
            break;
        }
        at += 12 + std::size_t(length);
    }
}

void jpeg(std::string_view b, FileMetadata& m) {
    static constexpr std::string_view kExif("Exif\0\0", 6);
    static constexpr std::string_view kXmp("http://ns.adobe.com/xap/1.0/\0", 29);
    std::size_t at = 2;
    while (at + 4 <= b.size()) {
        if (static_cast<unsigned char>(b[at]) != 0xFF) break;
        const auto marker = static_cast<unsigned char>(b[at + 1]);
        if (marker == 0xFF) {  // (fill bytes)
            ++at;
            continue;
        }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            at += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) break;  // (the scan: no more markers of this kind before it)
        const std::size_t length = (std::size_t(static_cast<unsigned char>(b[at + 2])) << 8) | static_cast<unsigned char>(b[at + 3]);
        if (length < 2 || at + 2 + length > b.size()) break;
        const std::string_view data = b.substr(at + 4, length - 2);
        if (marker == 0xE1) {
            if (!m.exif && data.starts_with(kExif)) m.exif = std::string(data.substr(kExif.size()));
            else if (!m.xmp && data.starts_with(kXmp)) m.xmp = std::string(data.substr(kXmp.size()));
        }
        at += 2 + length;
    }
}

void tiff(std::string_view b, FileMetadata& m) {
    const Tiff t(b);
    if (!t.ok()) return;
    const std::size_t first = t.first_ifd();
    if (const auto e = t.entry(first, 700); e && e->count <= kMetaMax) {
        if (const auto xmp = t.bytes_of(*e)) m.xmp = std::string(*xmp);
    }
    const auto photometric = whole_of(t, first, 262);
    const auto bits = whole_of(t, first, 258);
    m.palette = photometric && *photometric == 3;
    m.one_bit = bits && *bits == 1 && photometric && (*photometric == 0 || *photometric == 1);
    // (its pages: the IFDs after the first; a chain that loops is cut)
    int pages = 1;
    std::size_t ifd = t.next_ifd(first);
    while (ifd != 0 && ifd != first && pages < 1024 && t.has(ifd, 2)) {
        ++pages;
        const std::size_t next = t.next_ifd(ifd);
        if (next == ifd) break;
        ifd = next;
    }
    m.frames = pages;
}

void webp(std::string_view b, FileMetadata& m) {
    if (b.size() < 12 || b.substr(0, 4) != "RIFF" || b.substr(8, 4) != "WEBP") return;
    std::size_t at = 12;
    int frames = 0;
    while (at + 8 <= b.size()) {
        const std::string_view type = b.substr(at, 4);
        const std::uint32_t length = le32(b, at + 4);
        if (length > b.size() - at - 8) break;
        const std::string_view data = b.substr(at + 8, length);
        if (type == "EXIF" && !m.exif && length <= kMetaMax) {
            // (some writers keep JPEG's "Exif\0\0" before the TIFF structure)
            m.exif = std::string(data.starts_with(std::string_view("Exif\0\0", 6)) ? data.substr(6) : data);
        } else if (type == "XMP " && !m.xmp && length <= kMetaMax) {
            m.xmp = std::string(data);
        } else if (type == "ANMF") {
            ++frames;
        }
        at += 8 + std::size_t(length) + (length & 1);
    }
    if (frames > 1) m.frames = frames;
}

void gif(std::string_view b, FileMetadata& m) {
    m.palette = true;
    if (b.size() < 13) return;
    std::size_t at = 13;
    const auto packed = static_cast<unsigned char>(b[10]);
    if (packed & 0x80) at += 3 * (std::size_t(1) << ((packed & 7) + 1));
    const auto skip_blocks = [&](std::size_t from) -> std::size_t {
        while (from < b.size()) {
            const auto n = static_cast<unsigned char>(b[from]);
            if (n == 0) return from + 1;
            from += 1 + std::size_t(n);
        }
        return b.size();
    };
    int frames = 0;
    while (at < b.size()) {
        const auto kind = static_cast<unsigned char>(b[at]);
        if (kind == 0x3B) break;
        if (kind == 0x21) {
            if (at + 2 > b.size()) break;
            at = skip_blocks(at + 2);
        } else if (kind == 0x2C) {
            if (at + 10 > b.size()) break;
            ++frames;
            if (frames > 1) break;  // (more than one: enough to say so)
            const auto local = static_cast<unsigned char>(b[at + 9]);
            at += 10;
            if (local & 0x80) at += 3 * (std::size_t(1) << ((local & 7) + 1));
            at = skip_blocks(at + 1);  // (the LZW code size, then its blocks)
        } else {
            break;
        }
    }
    if (frames > 1) m.frames = frames;
}

void bmp(std::string_view b, FileMetadata& m) {
    if (b.size() < 30) return;
    const auto bits = static_cast<unsigned>(static_cast<unsigned char>(b[28]) | (static_cast<unsigned char>(b[29]) << 8));
    m.palette = bits >= 1 && bits <= 8;
}

}  // namespace

FileMetadata read_metadata(Format format, std::string_view bytes) {
    FileMetadata m;
    switch (format) {
    case Format::Png: png(bytes, m); break;
    case Format::Jpeg: jpeg(bytes, m); break;
    case Format::Tiff: tiff(bytes, m); break;
    case Format::WebP: webp(bytes, m); break;
    case Format::Gif: gif(bytes, m); break;
    case Format::Bmp: bmp(bytes, m); break;
    default: break;
    }
    if (m.exif && !Tiff(*m.exif).ok()) m.exif.reset();  // (not a TIFF structure: not EXIF this build reads)
    if (m.xmp && m.xmp->empty()) m.xmp.reset();
    return m;
}

int exif_orientation(std::string_view exif) {
    const Tiff t(exif);
    if (!t.ok()) return 1;
    const auto value = whole_of(t, t.first_ifd(), 0x0112);
    return value && *value >= 1 && *value <= 8 ? static_cast<int>(*value) : 1;
}

std::string exif_upright(std::string exif) {
    const Tiff t(exif);
    if (!t.ok()) return exif;
    const auto e = t.entry(t.first_ifd(), 0x0112);
    if (!e) return exif;
    const bool little = t.little();
    if (e->type == 3) {
        // (a SHORT's value in the entry's first two bytes, in the block's own order)
        exif[e->field] = little ? '\x01' : '\0';
        exif[e->field + 1] = little ? '\0' : '\x01';
    } else if (e->type == 4) {
        // (a LONG — some writers' —: its four bytes)
        for (int k = 0; k < 4; ++k) exif[e->field + static_cast<std::size_t>(k)] = (little ? k == 0 : k == 3) ? '\x01' : '\0';
    }
    return exif;
}

int exif_colour_space(std::string_view exif) {
    const Tiff t(exif);
    if (!t.ok()) return 0;
    const auto sub = whole_of(t, t.first_ifd(), 0x8769);
    if (!sub || *sub == 0) return 0;
    const auto value = whole_of(t, *sub, 0xA001);
    return value ? static_cast<int>(*value) : 0;
}

std::string xmp_upright(std::string xmp) {
    for (std::size_t at = xmp.find("tiff:Orientation"); at != std::string::npos; at = xmp.find("tiff:Orientation", at + 1)) {
        std::size_t v = at + 16;
        if (v < xmp.size() && xmp[v] == '=' && v + 2 < xmp.size() && (xmp[v + 1] == '"' || xmp[v + 1] == '\'')) {
            const std::size_t end = xmp.find(xmp[v + 1], v + 2);
            if (end != std::string::npos) xmp.replace(v + 2, end - v - 2, "1");
        } else if (v < xmp.size() && xmp[v] == '>') {
            const std::size_t end = xmp.find('<', v + 1);
            if (end != std::string::npos) xmp.replace(v + 1, end - v - 1, "1");
        }
    }
    return xmp;
}

CodecImage oriented(const CodecImage& picture, int orientation) {
    if (orientation < 2 || orientation > 8 || picture.empty()) return picture;
    const std::uint32_t w = picture.width(), h = picture.height();
    const bool swap = orientation >= 5;
    const std::uint32_t ow = swap ? h : w, oh = swap ? w : h;
    const std::size_t px = std::size_t(channels(picture.layout())) * sample_bytes(picture.sample());
    std::string data(picture.data().size(), '\0');
    const char* from = picture.data().data();
    char* to = data.data();
    for (std::uint32_t y = 0; y < oh; ++y) {
        for (std::uint32_t x = 0; x < ow; ++x) {
            std::uint32_t sx = x, sy = y;
            switch (orientation) {
            case 2: sx = w - 1 - x; sy = y; break;
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;
            case 4: sx = x; sy = h - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = h - 1 - x; break;
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;
            case 8: sx = w - 1 - y; sy = x; break;
            default: break;
            }
            std::memcpy(to + (std::size_t(y) * ow + x) * px, from + (std::size_t(sy) * w + sx) * px, px);
        }
    }
    CodecImage out = CodecImage::from_bytes(ow, oh, picture.layout(), picture.sample(), std::move(data));
    out.icc = picture.icc;
    out.meta = picture.meta;
    if (swap && out.meta.dpi) out.meta.dpi = std::make_pair(out.meta.dpi->second, out.meta.dpi->first);
    return out;
}

}  // namespace genko::render::codecs
