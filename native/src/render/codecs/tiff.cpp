// TIFF (render/codecs/formats.hpp; PhotoCraft's crates/codecs/src/codecs/tiff.rs): read through this build's TIFF readers
// (8 bits as the drawing reads them; 16 and 32 bits — and 8-bit CMYK with an alpha, which the drawing's reader does not
// read — with all their bits), its profile (tag 34675), XMP (tag 700), text tags and resolution from the file; written with
// libtiff: grey, RGB and CMYK (InkSet CMYK) of 8, 16 bits or 32-bit floats, an unassociated alpha as an extra sample,
// strips compressed as asked (none, LZW, Deflate, PackBits).

#include <tiffio.h>

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstring>

#include "core/error.hpp"
#include "render/codecs/formats.hpp"
#include "render/codecs/metadata.hpp"
#include "render/proof.hpp"
#include "render/smart_decode.hpp"

namespace genko::render::codecs::detail {

namespace {

constexpr std::array<std::pair<std::uint16_t, std::string_view>, 7> kTextTags{{{TIFFTAG_IMAGEDESCRIPTION, "Description"},
                                                                               {TIFFTAG_MAKE, "Make"},
                                                                               {TIFFTAG_MODEL, "Model"},
                                                                               {TIFFTAG_SOFTWARE, "Software"},
                                                                               {TIFFTAG_DATETIME, "DateTime"},
                                                                               {TIFFTAG_ARTIST, "Artist"},
                                                                               {TIFFTAG_COPYRIGHT, "Copyright"}}};

struct Memory {
    std::string data;
    std::string_view source;  // (reading)
    toff_t pos = 0;
    bool writing = false;
    std::size_t size() const { return writing ? data.size() : source.size(); }
    const char* bytes() const { return writing ? data.data() : source.data(); }
};

tsize_t on_read(thandle_t h, tdata_t buffer, tsize_t size) {
    auto* m = static_cast<Memory*>(h);
    if (m->pos >= m->size()) return 0;
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(size), m->size() - static_cast<std::size_t>(m->pos));
    std::memcpy(buffer, m->bytes() + m->pos, n);
    m->pos += n;
    return static_cast<tsize_t>(n);
}
tsize_t on_write(thandle_t h, tdata_t buffer, tsize_t size) {
    auto* m = static_cast<Memory*>(h);
    if (!m->writing) return 0;
    const std::size_t end = static_cast<std::size_t>(m->pos) + static_cast<std::size_t>(size);
    if (end > m->data.size()) m->data.resize(end, '\0');
    std::memcpy(m->data.data() + m->pos, buffer, static_cast<std::size_t>(size));
    m->pos = end;
    return size;
}
toff_t on_seek(thandle_t h, toff_t offset, int whence) {
    auto* m = static_cast<Memory*>(h);
    switch (whence) {
    case SEEK_SET: m->pos = offset; break;
    case SEEK_CUR: m->pos += offset; break;
    case SEEK_END: m->pos = m->size() + offset; break;
    default: return static_cast<toff_t>(-1);
    }
    return m->pos;
}
int on_close(thandle_t) { return 0; }
toff_t on_size(thandle_t h) { return static_cast<Memory*>(h)->size(); }
int on_map(thandle_t, tdata_t*, toff_t*) { return 0; }
void on_unmap(thandle_t, tdata_t, toff_t) {}
void quiet(const char*, const char*, va_list) {}

struct File {
    Memory memory;
    TIFF* tif = nullptr;
    File(std::string_view bytes, const char* mode) {
        TIFFSetErrorHandler(quiet);
        TIFFSetWarningHandler(quiet);
        memory.source = bytes;
        memory.writing = mode[0] == 'w';
        tif = TIFFClientOpen("picture", mode, &memory, on_read, on_write, on_seek, on_close, on_size, on_map, on_unmap);
    }
    ~File() {
        if (tif != nullptr) TIFFClose(tif);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};

// The resolution its tags say (an inch or a centimetre), and its text tags.
void tiff_extras(TIFF* tif, CodecImage& out) {
    float x = 0, y = 0;
    std::uint16_t unit = RESUNIT_INCH;
    if (TIFFGetField(tif, TIFFTAG_XRESOLUTION, &x) != 0 && TIFFGetField(tif, TIFFTAG_YRESOLUTION, &y) != 0 && x > 0 && y > 0 &&
        std::isfinite(x) && std::isfinite(y)) {
        TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &unit);
        if (unit == RESUNIT_INCH) out.meta.dpi = std::make_pair(double(x), double(y));
        else if (unit == RESUNIT_CENTIMETER) out.meta.dpi = std::make_pair(x * 2.54, y * 2.54);
    }
    for (const auto& [tag, key] : kTextTags) {
        char* text = nullptr;
        if (TIFFGetField(tif, tag, &text) != 0 && text != nullptr) out.meta.text.emplace_back(std::string(key), std::string(text));
    }
}

// The rows in strips of about 8 KB (libtiff's default), then the directory (libtiff reports by its return values).
bool write_strips(TIFF* tif, const CodecImage& stored) {
    const std::size_t stride = std::size_t(stored.width()) * channels(stored.layout()) * sample_bytes(stored.sample());
    const std::uint32_t per = std::max<std::uint32_t>(1, TIFFDefaultStripSize(tif, 0));
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, per);
    for (std::uint32_t first = 0, strip = 0; first < stored.height(); first += per, ++strip) {
        const std::uint32_t rows = std::min(per, stored.height() - first);
        auto* data = const_cast<char*>(stored.data().data() + std::size_t(first) * stride);
        if (TIFFWriteEncodedStrip(tif, strip, data, static_cast<tmsize_t>(std::size_t(rows) * stride)) < 0) return false;
    }
    return TIFFWriteDirectory(tif) != 0;
}

}  // namespace

std::optional<std::uint16_t> tiff_text_tag(std::string_view key) {
    for (const auto& [tag, name] : kTextTags) {
        if (name == key) return tag;
    }
    return std::nullopt;
}

std::span<const std::pair<std::uint16_t, std::string_view>> tiff_text_tags() { return kTextTags; }

CodecImage read_tiff(std::string_view bytes, const Limits& limits) {
    std::uint16_t bits = 8, samples = 1, photometric = PHOTOMETRIC_MINISBLACK, planar = PLANARCONFIG_CONTIG;
    std::uint32_t width = 0, height = 0;
    {
        File f(bytes, "rm");
        if (f.tif == nullptr) throw core::Error("unidentified_image", "cannot identify image file (not a TIFF file)");
        TIFFGetField(f.tif, TIFFTAG_IMAGEWIDTH, &width);
        TIFFGetField(f.tif, TIFFTAG_IMAGELENGTH, &height);
        TIFFGetFieldDefaulted(f.tif, TIFFTAG_BITSPERSAMPLE, &bits);
        TIFFGetFieldDefaulted(f.tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
        TIFFGetFieldDefaulted(f.tif, TIFFTAG_PLANARCONFIG, &planar);
        if (TIFFGetField(f.tif, TIFFTAG_PHOTOMETRIC, &photometric) == 0) photometric = PHOTOMETRIC_MINISBLACK;
    }
    // (with all their bits: 16 and 32; CMYK and an alpha of 8 — smart_decode's reader)
    const bool deep = bits == 16 || bits == 32 || smart::cmyk_alpha_tiff(bytes);
    // (a deep one's size from libtiff: Pillow's table of modes, which render/png's picture_size reads it through, does not
    // know every layout of 16 bits or of floats this build writes)
    CodecImage out = deep ? this_builds_picture(bytes, limits, true, std::make_pair(width, height)) : this_builds_picture(bytes, limits, false);
    out.icc = proof::embedded_profile(bytes);
    out.meta.xmp = read_metadata(Format::Tiff, bytes).xmp;
    File f(bytes, "rm");
    if (f.tif != nullptr) tiff_extras(f.tif, out);
    return out;
}

std::string write_tiff(const CodecImage& stored, const EncodeOptions& options) {
    const Layout layout = stored.layout();
    const Sample sample = stored.sample();
    if (sample == Sample::F16) throw core::Error("value", "TIFF is written of 8 or 16 bits or 32-bit floats");
    File f({}, "w");
    if (f.tif == nullptr) throw core::Error("io", "cannot write TIFF");
    TIFF* tif = f.tif;
    const std::uint16_t spp = static_cast<std::uint16_t>(channels(layout));
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, stored.width());
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, stored.height());
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, static_cast<std::uint16_t>(depth_of(sample)));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, spp);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, static_cast<std::uint16_t>(is_float(sample) ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT));
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    if (is_gray(layout)) {
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    } else if (is_rgb(layout)) {
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    } else {
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_SEPARATED);
        TIFFSetField(tif, TIFFTAG_INKSET, INKSET_CMYK);
    }
    if (has_alpha(layout)) {
        std::uint16_t kind = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &kind);
    }
    std::uint16_t compression = COMPRESSION_NONE;
    switch (options.tiff_compression) {
    case TiffCompression::None: compression = COMPRESSION_NONE; break;
    case TiffCompression::Lzw: compression = COMPRESSION_LZW; break;
    case TiffCompression::Deflate: compression = COMPRESSION_ADOBE_DEFLATE; break;
    case TiffCompression::PackBits: compression = COMPRESSION_PACKBITS; break;
    }
    TIFFSetField(tif, TIFFTAG_COMPRESSION, compression);
    if ((compression == COMPRESSION_LZW || compression == COMPRESSION_ADOBE_DEFLATE) && !is_float(sample))
        TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
    if (stored.meta.dpi && stored.meta.dpi->first > 0 && stored.meta.dpi->second > 0) {
        TIFFSetField(tif, TIFFTAG_XRESOLUTION, static_cast<float>(stored.meta.dpi->first));
        TIFFSetField(tif, TIFFTAG_YRESOLUTION, static_cast<float>(stored.meta.dpi->second));
        TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    }
    if (stored.icc && !stored.icc->empty()) TIFFSetField(tif, TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(stored.icc->size()), stored.icc->data());
    if (stored.meta.xmp) TIFFSetField(tif, TIFFTAG_XMLPACKET, static_cast<std::uint32_t>(stored.meta.xmp->size()), stored.meta.xmp->data());
    for (const auto& [key, value] : stored.meta.text) {
        if (const auto tag = tiff_text_tag(key)) TIFFSetField(tif, *tag, value.c_str());
    }
    if (!write_strips(tif, stored)) throw core::Error("io", "cannot write TIFF");
    TIFFClose(tif);
    f.tif = nullptr;
    return std::move(f.memory.data);
}

}  // namespace genko::render::codecs::detail
