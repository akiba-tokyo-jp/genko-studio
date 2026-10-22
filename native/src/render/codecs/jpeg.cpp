// JPEG (render/codecs/formats.hpp; PhotoCraft's crates/codecs/src/codecs/jpeg.rs): read through this build's JPEG reader
// (8-bit grey, RGB and CMYK as the drawing reads them, Adobe's inverted CMYK straightened), its profile (APP2, in as many
// segments as it takes), EXIF and XMP (APP1) and resolution (JFIF's density) from the file; written with libjpeg — grey, RGB
// (4:2:0 or 4:4:4) and CMYK (4:4:4, Adobe's inverted inks with its APP14 marker, as Photoshop writes them) — with those
// segments.

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <jpeglib.h>

// MSVC warns (C4611) on every setjmp in a C++ file: each one here is in a function that owns no C++ object made after it.
#if defined(_MSC_VER)
#pragma warning(disable : 4611)
#endif

#include "core/error.hpp"
#include "render/codecs/formats.hpp"
#include "render/codecs/metadata.hpp"
#include "render/proof.hpp"

namespace genko::render::codecs::detail {

namespace {

// The resolution of a JFIF segment (APP0 "JFIF\0": its unit 1 dots an inch, 2 dots a centimetre; 0 an aspect only).
std::optional<std::pair<double, double>> jfif_dpi(std::string_view b) {
    std::size_t at = 2;
    while (at + 4 <= b.size() && u8(b, at) == 0xff) {
        const unsigned marker = u8(b, at + 1);
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01 || marker == 0xff) {
            at += marker == 0xff ? 1 : 2;
            continue;
        }
        if (marker == 0xda || marker == 0xd9) break;
        const std::size_t length = be16(b, at + 2);
        if (length < 2 || at + 2 + length > b.size()) break;
        if (marker == 0xe0 && length >= 14 && b.substr(at + 4, 5) == std::string_view("JFIF\0", 5)) {
            const unsigned unit = u8(b, at + 11);
            const double x = be16(b, at + 12), y = be16(b, at + 14);
            if (x <= 0 || y <= 0) return std::nullopt;
            if (unit == 1) return std::make_pair(x, y);
            if (unit == 2) return std::make_pair(x * 2.54, y * 2.54);
            return std::nullopt;
        }
        at += 2 + length;
    }
    return std::nullopt;
}

struct JpegError {
    jpeg_error_mgr base{};
    std::jmp_buf* jump = nullptr;
    char message[JMSG_LENGTH_MAX]{};
};

void error_exit(j_common_ptr codec) {
    auto* error = reinterpret_cast<JpegError*>(codec->err);
    codec->err->format_message(codec, error->message);
    std::longjmp(*error->jump, 1);
}

struct Writing {
    const unsigned char* pixels = nullptr;  // (rows of `components` bytes a pixel)
    int width = 0, height = 0, components = 3;
    J_COLOR_SPACE space = JCS_RGB;
    int quality = 90;
    bool subsample = true;
    std::optional<std::pair<int, int>> density;
    std::vector<std::pair<int, std::string>> markers;  // (APPn and their data, after the start of the frame)
};

struct WriteState {
    jpeg_compress_struct codec{};
    JpegError error{};
    unsigned char* buffer = nullptr;
    unsigned long size = 0;
    ~WriteState() {
        if (codec.mem != nullptr) jpeg_destroy_compress(&codec);
        std::free(buffer);
    }
};

// (the jump target in a helper without C++ owners)
bool compress(WriteState* s, const Writing* w) {
    std::jmp_buf jump;
    s->error.jump = &jump;
    s->codec.err = jpeg_std_error(&s->error.base);
    s->error.base.error_exit = error_exit;
    if (setjmp(jump)) return false;
    jpeg_create_compress(&s->codec);
    jpeg_mem_dest(&s->codec, &s->buffer, &s->size);
    s->codec.image_width = static_cast<JDIMENSION>(w->width);
    s->codec.image_height = static_cast<JDIMENSION>(w->height);
    s->codec.input_components = w->components;
    s->codec.in_color_space = w->space;
    jpeg_set_defaults(&s->codec);
    if (w->space == JCS_CMYK) jpeg_set_colorspace(&s->codec, JCS_CMYK);  // (Adobe's APP14 marker, every component 1 × 1)
    jpeg_set_quality(&s->codec, w->quality, TRUE);
    if (w->space == JCS_RGB && !w->subsample) {
        for (int c = 0; c < s->codec.num_components; ++c) {
            s->codec.comp_info[c].h_samp_factor = 1;
            s->codec.comp_info[c].v_samp_factor = 1;
        }
    }
    if (w->density) {
        s->codec.write_JFIF_header = TRUE;  // (a CMYK file's resolution too: JFIF's density is where readers look)
        s->codec.density_unit = 1;
        s->codec.X_density = static_cast<UINT16>(w->density->first);
        s->codec.Y_density = static_cast<UINT16>(w->density->second);
    }
    jpeg_start_compress(&s->codec, TRUE);
    for (const auto& [marker, data] : w->markers) {
        jpeg_write_marker(&s->codec, marker, reinterpret_cast<const JOCTET*>(data.data()), static_cast<unsigned int>(data.size()));
    }
    const std::size_t stride = std::size_t(w->width) * static_cast<std::size_t>(w->components);
    while (s->codec.next_scanline < s->codec.image_height) {
        JSAMPROW row = const_cast<unsigned char*>(w->pixels + std::size_t(s->codec.next_scanline) * stride);
        jpeg_write_scanlines(&s->codec, &row, 1);
    }
    jpeg_finish_compress(&s->codec);
    return true;
}

int density_of(double dpi) { return static_cast<int>(std::clamp(std::lround(dpi), 1L, 65535L)); }

}  // namespace

CodecImage read_jpeg(std::string_view bytes, const Limits& limits) {
    CodecImage out = this_builds_picture(bytes, limits, false);
    out.icc = proof::embedded_profile(bytes);
    const FileMetadata meta = read_metadata(Format::Jpeg, bytes);
    out.meta.exif = meta.exif;
    out.meta.xmp = meta.xmp;
    out.meta.dpi = jfif_dpi(bytes);
    return out;
}

std::string write_jpeg(const CodecImage& stored, const EncodeOptions& options) {
    if (stored.sample() != Sample::U8) throw core::Error("value", "JPEG holds 8 bits");
    Writing w;
    w.width = static_cast<int>(stored.width());
    w.height = static_cast<int>(stored.height());
    std::string inverted;
    switch (stored.layout()) {
    case Layout::Gray:
        w.components = 1;
        w.space = JCS_GRAYSCALE;
        break;
    case Layout::Rgb:
        w.components = 3;
        w.space = JCS_RGB;
        break;
    case Layout::Cmyk:
        w.components = 4;
        w.space = JCS_CMYK;
        inverted = stored.data();  // (Adobe's CMYK: 0 is full ink)
        for (char& c : inverted) c = static_cast<char>(255 - static_cast<unsigned char>(c));
        break;
    default: throw core::Error("value", "JPEG holds grey, RGB and CMYK, without an alpha");
    }
    w.pixels = reinterpret_cast<const unsigned char*>(inverted.empty() ? stored.data().data() : inverted.data());
    w.quality = std::clamp(options.jpeg_quality, 1, 100);
    w.subsample = options.jpeg_chroma_subsampling;
    if (stored.meta.dpi && stored.meta.dpi->first > 0 && stored.meta.dpi->second > 0)
        w.density = std::make_pair(density_of(stored.meta.dpi->first), density_of(stored.meta.dpi->second));
    // (the EXIF and the XMP in an APP1 segment each — what does not fit in one is not written, as the losses say —; the
    // profile in APP2 segments of 65519 bytes, numbered)
    if (stored.meta.exif && stored.meta.exif->size() <= kJpegExifMax) w.markers.emplace_back(JPEG_APP0 + 1, std::string("Exif\0\0", 6) + *stored.meta.exif);
    if (stored.meta.xmp && stored.meta.xmp->size() <= kJpegXmpMax)
        w.markers.emplace_back(JPEG_APP0 + 1, std::string("http://ns.adobe.com/xap/1.0/\0", 29) + *stored.meta.xmp);
    if (stored.icc && !stored.icc->empty() && stored.icc->size() <= kJpegIccMax) {
        constexpr std::size_t kPart = 65519;
        const std::size_t count = (stored.icc->size() + kPart - 1) / kPart;
        for (std::size_t i = 0; i < count; ++i) {
            std::string data("ICC_PROFILE\0", 12);
            data.push_back(static_cast<char>(i + 1));
            data.push_back(static_cast<char>(count));
            data += stored.icc->substr(i * kPart, kPart);
            w.markers.emplace_back(JPEG_APP0 + 2, std::move(data));
        }
    }
    WriteState state;
    if (!compress(&state, &w)) throw core::Error("format", std::string("cannot write JPEG: ") + state.error.message);
    return std::string(reinterpret_cast<const char*>(state.buffer), state.size);
}

}  // namespace genko::render::codecs::detail
