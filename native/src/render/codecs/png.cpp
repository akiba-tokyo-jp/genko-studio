// PNG (render/codecs/formats.hpp; PhotoCraft's crates/codecs/src/codecs/png.rs): read through this build's PNG readers
// (8 bits as the drawing reads them — palettes, small greys and a tRNS as an alpha, Adam7 put together —, 16 bits with all
// their bits), its iCCP, eXIf, iTXt "XML:com.adobe.xmp", pHYs and text chunks (tEXt, zTXt, iTXt) from the file; written with
// libpng (grey, grey and alpha, RGB, RGBA of 8 or 16 bits, Adam7 when asked) and those chunks after its header.

#include <png.h>
#include <zlib.h>

// MSVC warns (C4611) on every setjmp in a C++ file: each one here is in a function that owns no C++ object made after it.
#if defined(_MSC_VER)
#pragma warning(disable : 4611)
#endif

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/error.hpp"
#include "render/codecs/formats.hpp"
#include "render/codecs/metadata.hpp"
#include "render/proof.hpp"
#include "render/smart_decode.hpp"

namespace genko::render::codecs::detail {

namespace {

constexpr std::size_t kTextMax = std::size_t(64) << 20;  // (a text past 64 MB, inflated, is not taken)

std::string latin1_to_utf8(std::string_view latin1) {
    std::string out;
    out.reserve(latin1.size());
    for (const char ch : latin1) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// UTF-8 as Latin-1 bytes, or none when a character is past U+00FF (or the bytes are not UTF-8).
std::optional<std::string> utf8_to_latin1(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size() && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80) {
            const unsigned code = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            if (code < 0x80 || code > 0xFF) return std::nullopt;
            out.push_back(static_cast<char>(code));
            i += 2;
        } else {
            return std::nullopt;
        }
    }
    return out;
}

std::optional<std::string> inflated(std::string_view data) {
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
        if (out.size() > kTextMax) {
            status = Z_DATA_ERROR;
            break;
        }
    }
    inflateEnd(&z);
    if (status != Z_STREAM_END) return std::nullopt;
    return out;
}

std::string deflated(std::string_view data) {
    uLongf size = compressBound(static_cast<uLong>(data.size()));
    std::string out(size, '\0');
    if (compress2(reinterpret_cast<Bytef*>(out.data()), &size, reinterpret_cast<const Bytef*>(data.data()), static_cast<uLong>(data.size()), 9) != Z_OK) {
        throw core::Error("memory", "the PNG's profile cannot be compressed");
    }
    out.resize(size);
    return out;
}

std::string chunk(std::string_view type, std::string_view data) {
    std::string out;
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    out.append(type);
    out.append(data);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(type.data()), static_cast<uInt>(type.size()));
    if (!data.empty()) crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
    put_be32(out, static_cast<std::uint32_t>(crc));
    return out;
}

// What the file says besides its pixels that the eXIf, iTXt XMP and iCCP readers do not: its resolution (pHYs in pixels a
// metre) and its text (tEXt and zTXt in Latin-1, iTXt in UTF-8 — the XMP's iTXt left out).
void png_extras(std::string_view b, CodecImage& out) {
    std::size_t at = 8;
    while (at + 12 <= b.size()) {
        const std::uint32_t length = be32(b, at);
        if (length > b.size() - at - 12) break;
        const std::string_view type = b.substr(at + 4, 4);
        const std::string_view data = b.substr(at + 8, length);
        if (type == "pHYs" && data.size() >= 9 && data[8] == 1) {
            const std::uint32_t x = be32(data, 0), y = be32(data, 4);
            if (x > 0 && y > 0) out.meta.dpi = std::make_pair(x * 0.0254, y * 0.0254);
        } else if (type == "tEXt" || type == "zTXt" || type == "iTXt") {
            const std::size_t key_end = data.find('\0');
            if (key_end != std::string_view::npos && key_end > 0) {
                const std::string key = latin1_to_utf8(data.substr(0, key_end));
                std::optional<std::string> text;
                if (type == "tEXt") {
                    text = latin1_to_utf8(data.substr(key_end + 1));
                } else if (type == "zTXt") {
                    if (key_end + 2 <= data.size() && data[key_end + 1] == 0) {
                        if (const auto raw = inflated(data.substr(key_end + 2))) text = latin1_to_utf8(*raw);
                    }
                } else if (key != "XML:com.adobe.xmp" && key_end + 3 <= data.size()) {
                    const bool compressed = data[key_end + 1] != 0;
                    const std::size_t language_end = data.find('\0', key_end + 3);
                    const std::size_t translated_end = language_end == std::string_view::npos ? language_end : data.find('\0', language_end + 1);
                    if (translated_end != std::string_view::npos) {
                        const std::string_view value = data.substr(translated_end + 1);
                        text = compressed ? inflated(value) : std::optional<std::string>(std::string(value));
                    }
                }
                if (text) out.meta.text.emplace_back(key, std::move(*text));
            }
        } else if (type == "IEND") {
            break;
        }
        at += 12 + std::size_t(length);
    }
}

// --- writing with libpng ------------------------------------------------------------------------------------------------

struct Sink {
    std::string bytes;
    char message[200] = {0};
};

void on_error(png_structp png, png_const_charp message) {
    auto* sink = static_cast<Sink*>(png_get_error_ptr(png));
    std::snprintf(sink->message, sizeof sink->message, "%s", message != nullptr ? message : "broken");
    png_longjmp(png, 1);
}
void on_warning(png_structp, png_const_charp) {}
void on_write(png_structp png, png_bytep data, png_size_t n) {
    static_cast<Sink*>(png_get_io_ptr(png))->bytes.append(reinterpret_cast<const char*>(data), n);
}
void on_flush(png_structp) {}

struct Writing {
    png_uint_32 width = 0, height = 0;
    int bit_depth = 8, colour = PNG_COLOR_TYPE_RGB, interlace = PNG_INTERLACE_NONE, level = 6;
    bool swap = false;
    std::optional<std::pair<png_uint_32, png_uint_32>> ppm;
    png_bytepp rows = nullptr;
};

// (the jump target in a function of no C++ owners)
bool write_rows(png_structp png, png_infop info, const Writing* w) {
    if (setjmp(png_jmpbuf(png))) return false;
    png_set_compression_level(png, w->level);
    png_set_IHDR(png, info, w->width, w->height, w->bit_depth, w->colour, w->interlace, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    if (w->ppm) png_set_pHYs(png, info, w->ppm->first, w->ppm->second, PNG_RESOLUTION_METER);
    png_write_info(png, info);
    if (w->swap) png_set_swap(png);
    png_write_image(png, w->rows);
    png_write_end(png, info);
    return true;
}

struct WriteGuard {
    png_structp png = nullptr;
    png_infop info = nullptr;
    ~WriteGuard() { png_destroy_write_struct(&png, &info); }
};

png_uint_32 pixels_a_metre(double dpi) {
    const double ppm = std::round(dpi / 0.0254);
    return static_cast<png_uint_32>(std::clamp(ppm, 1.0, 2147483647.0));
}

}  // namespace

std::optional<std::string> png_keyword(std::string_view key) {
    auto latin = utf8_to_latin1(key);
    if (!latin || latin->empty() || latin->size() > 79 || latin->find('\0') != std::string::npos) return std::nullopt;
    return latin;
}

CodecImage read_png(std::string_view bytes, const Limits& limits) {
    CodecImage out = this_builds_picture(bytes, limits, smart::stored_depth(bytes) > 8);
    out.icc = proof::embedded_profile(bytes);
    const FileMetadata meta = read_metadata(Format::Png, bytes);
    out.meta.exif = meta.exif;
    out.meta.xmp = meta.xmp;
    png_extras(bytes, out);
    return out;
}

std::string write_png(const CodecImage& stored, const EncodeOptions& options) {
    Writing w;
    w.width = stored.width();
    w.height = stored.height();
    w.bit_depth = stored.sample() == Sample::U16 ? 16 : 8;
    switch (stored.layout()) {
    case Layout::Gray: w.colour = PNG_COLOR_TYPE_GRAY; break;
    case Layout::GrayA: w.colour = PNG_COLOR_TYPE_GRAY_ALPHA; break;
    case Layout::Rgb: w.colour = PNG_COLOR_TYPE_RGB; break;
    case Layout::Rgba: w.colour = PNG_COLOR_TYPE_RGB_ALPHA; break;
    default: throw core::Error("value", "PNG holds grey and RGB, with or without an alpha");
    }
    if (stored.sample() != Sample::U8 && stored.sample() != Sample::U16) throw core::Error("value", "PNG holds 8 or 16 bits");
    w.interlace = options.png_interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE;
    switch (options.png_compression) {
    case PngCompression::None: w.level = 0; break;
    case PngCompression::Fast: w.level = 1; break;
    case PngCompression::Default: w.level = 6; break;
    case PngCompression::Best: w.level = 9; break;
    }
    {
        std::uint16_t probe = 1;
        unsigned char first = 0;
        std::memcpy(&first, &probe, 1);
        w.swap = w.bit_depth == 16 && first == 1;  // (this machine's order is little-endian: PNG's is big)
    }
    if (stored.meta.dpi && stored.meta.dpi->first > 0 && stored.meta.dpi->second > 0)
        w.ppm = std::make_pair(pixels_a_metre(stored.meta.dpi->first), pixels_a_metre(stored.meta.dpi->second));
    const std::size_t stride = std::size_t(w.width) * channels(stored.layout()) * sample_bytes(stored.sample());
    std::vector<png_bytep> rows(w.height);
    auto* data = reinterpret_cast<png_bytep>(const_cast<char*>(stored.data().data()));
    for (png_uint_32 y = 0; y < w.height; ++y) rows[y] = data + std::size_t(y) * stride;
    w.rows = rows.data();

    Sink sink;
    WriteGuard guard;
    guard.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &sink, on_error, on_warning);
    if (guard.png == nullptr) throw core::Error("memory", "the PNG writer cannot be made");
    guard.info = png_create_info_struct(guard.png);
    if (guard.info == nullptr) throw core::Error("memory", "the PNG writer cannot be made");
    png_set_write_fn(guard.png, &sink, on_write, on_flush);
    if (!write_rows(guard.png, guard.info, &w)) throw core::Error("io", std::string("cannot write PNG: ") + sink.message);

    // (the profile, the EXIF, the XMP and the text, put after the header: before the pixels, where readers look for them)
    std::string extra;
    if (stored.icc && !stored.icc->empty()) extra += chunk("iCCP", std::string("ICC profile\0\0", 13) + deflated(*stored.icc));
    if (stored.meta.exif) extra += chunk("eXIf", *stored.meta.exif);
    if (stored.meta.xmp) extra += chunk("iTXt", std::string("XML:com.adobe.xmp\0\0\0\0\0", 22) + *stored.meta.xmp);
    for (const auto& [key, value] : stored.meta.text) {
        const auto keyword = png_keyword(key);
        if (!keyword) continue;
        if (const auto latin = utf8_to_latin1(value)) {
            extra += chunk("tEXt", *keyword + '\0' + *latin);
        } else {
            extra += chunk("iTXt", *keyword + std::string("\0\0\0\0\0", 5) + value);
        }
    }
    constexpr std::size_t kAfterHeader = 8 + 12 + 13;
    std::string& bytes = sink.bytes;
    if (!extra.empty() && bytes.size() > kAfterHeader) bytes.insert(kAfterHeader, extra);
    return std::move(bytes);
}

}  // namespace genko::render::codecs::detail
