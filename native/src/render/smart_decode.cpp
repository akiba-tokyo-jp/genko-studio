// The readers of a smart object's file (render/smart_decode.hpp). libpng and libtiff are read through C callbacks that
// keep no C++ object alive across their error jumps: each step that can jump is a function of plain values.

#include "render/smart_decode.hpp"

#include <png.h>
#include <tiffio.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <csetjmp>
#include <cstdarg>
#include <cstring>
#include <memory>

#include <lcms2.h>

#include "core/error.hpp"
#include "core/smart.hpp"
#include "render/imaging.hpp"
#include "render/ink.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
#include "render/psd.hpp"

namespace genko::render::smart {

namespace {

unsigned u8(std::string_view b, std::size_t at) { return static_cast<unsigned char>(b[at]); }
std::uint32_t be16(std::string_view b, std::size_t at) { return (u8(b, at) << 8) | u8(b, at + 1); }
std::uint32_t le16(std::string_view b, std::size_t at) { return u8(b, at) | (u8(b, at + 1) << 8); }
std::uint32_t le32(std::string_view b, std::size_t at) { return le16(b, at) | (le16(b, at + 2) << 16); }

// The kind of file its first bytes say: the format and its suffix (a PSB is a PSD of version 2), or none.
std::optional<std::pair<std::string, std::string>> sniff(std::string_view b) {
    if (b.size() >= 8 && b.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8)) return std::make_pair("png", ".png");
    if (b.size() >= 3 && u8(b, 0) == 0xff && u8(b, 1) == 0xd8 && u8(b, 2) == 0xff) return std::make_pair("jpeg", ".jpg");
    if (b.size() >= 4 && (b.substr(0, 4) == std::string_view("II*\0", 4) || b.substr(0, 4) == std::string_view("MM\0*", 4)))
        return std::make_pair("tiff", ".tif");
    if (b.size() >= 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP") return std::make_pair("webp", ".webp");
    if (b.size() >= 2 && b.substr(0, 2) == "BM") return std::make_pair("bmp", ".bmp");
    if (b.size() >= 6 && (b.substr(0, 6) == "GIF87a" || b.substr(0, 6) == "GIF89a")) return std::make_pair("gif", ".gif");
    if (b.size() >= 6 && b.substr(0, 4) == "8BPS") return std::make_pair("psd", be16(b, 4) == 2 ? ".psb" : ".psd");
    return std::nullopt;
}

// --- PNG with every bit (libpng) --------------------------------------------------------------------------------------

struct PngSource {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
    char message[200] = {0};
};

void png_failed(png_structp png, png_const_charp message) {
    auto* source = static_cast<PngSource*>(png_get_error_ptr(png));
    std::snprintf(source->message, sizeof source->message, "%s", message != nullptr ? message : "broken");
    png_longjmp(png, 1);
}
void png_quiet(png_structp, png_const_charp) {}
void png_take(png_structp png, png_bytep out, png_size_t n) {
    auto* source = static_cast<PngSource*>(png_get_io_ptr(png));
    if (n > source->size - source->pos) png_error(png, "image file is truncated");
    std::memcpy(out, source->data + source->pos, n);
    source->pos += n;
}

struct PngShape {
    png_uint_32 width = 0, height = 0;
    int bit_depth = 0, color_type = 0, interlace = 0;
    png_uint_32 ppm_x = 0;
    int unit = 0;
    bool phys = false;
    std::size_t rowbytes = 0;
    int channels = 0, depth_after = 0;
};

// The header, and the reading set up: palettes and small greys expanded, a tRNS as an alpha, 16-bit samples in this
// machine's order.
bool png_head(png_structp png, png_infop info, PngShape* s) {
    if (setjmp(png_jmpbuf(png))) return false;
    png_read_info(png, info);
    png_get_IHDR(png, info, &s->width, &s->height, &s->bit_depth, &s->color_type, &s->interlace, nullptr, nullptr);
    png_uint_32 x = 0, y = 0;
    if (png_get_pHYs(png, info, &x, &y, &s->unit) != 0) {
        s->phys = true;
        s->ppm_x = x;
    }
    return true;
}

bool png_prepare(png_structp png, png_infop info, PngShape* s, png_uint_32 max_pixels) {
    if (setjmp(png_jmpbuf(png))) return false;
    if (static_cast<std::uint64_t>(s->width) * s->height > max_pixels) png_error(png, "too many pixels");
    png_set_expand(png);
    if (s->bit_depth == 16) png_set_swap(png);
    (void)png_set_interlace_handling(png);
    png_read_update_info(png, info);
    s->rowbytes = png_get_rowbytes(png, info);
    s->channels = png_get_channels(png, info);
    s->depth_after = png_get_bit_depth(png, info);
    return true;
}

bool png_rows(png_structp png, png_bytepp rows) {
    if (setjmp(png_jmpbuf(png))) return false;
    png_read_image(png, rows);
    return true;
}

struct PngReader {
    PngSource source;
    png_structp png = nullptr;
    png_infop info = nullptr;
    explicit PngReader(std::string_view bytes) {
        source.data = reinterpret_cast<const unsigned char*>(bytes.data());
        source.size = bytes.size();
        png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &source, png_failed, png_quiet);
        if (png == nullptr) throw core::Error("memory", "the PNG reader cannot be made");
        info = png_create_info_struct(png);
        if (info == nullptr) {
            png_destroy_read_struct(&png, nullptr, nullptr);
            throw core::Error("memory", "the PNG reader cannot be made");
        }
        png_set_read_fn(png, &source, png_take);
    }
    ~PngReader() { png_destroy_read_struct(&png, &info, nullptr); }
    PngReader(const PngReader&) = delete;
    PngReader& operator=(const PngReader&) = delete;
    [[noreturn]] void fail() const { throw core::Error("format", std::string("the PNG cannot be read: ") + source.message); }
};

PngShape png_shape(std::string_view bytes) {
    PngReader r(bytes);
    PngShape s;
    if (!png_head(r.png, r.info, &s)) r.fail();
    return s;
}

DeepPicture png_deep(std::string_view bytes) {
    PngReader r(bytes);
    PngShape s;
    if (!png_head(r.png, r.info, &s)) r.fail();
    if (!png_prepare(r.png, r.info, &s, static_cast<png_uint_32>(std::min<std::int64_t>(2 * kMaxImagePixels, 0xffffffffLL)))) {
        if (std::strcmp(r.source.message, "too many pixels") == 0) throw core::Error("image_too_large", "the picture has too many pixels");
        r.fail();
    }
    std::vector<unsigned char> data(s.rowbytes * s.height);
    std::vector<png_bytep> rows(s.height);
    for (png_uint_32 y = 0; y < s.height; ++y) rows[y] = data.data() + std::size_t(y) * s.rowbytes;
    if (!png_rows(r.png, rows.data())) r.fail();
    DeepPicture out;
    out.width = s.width;
    out.height = s.height;
    out.channels = static_cast<unsigned>(s.channels);
    out.alpha = s.channels == 2 || s.channels == 4;
    out.mode = std::string(s.channels <= 2 ? "gray" : "rgb") + (out.alpha ? "a" : "");
    out.depth = s.depth_after;
    const std::size_t count = std::size_t(s.width) * s.height * out.channels;
    out.samples.resize(count);
    if (s.depth_after == 16) {
        for (std::size_t i = 0; i < count; ++i) {
            std::uint16_t v = 0;
            std::memcpy(&v, data.data() + i * 2, 2);
            out.samples[i] = static_cast<float>(v / 65535.0);
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) out.samples[i] = static_cast<float>(data[i] / 255.0);
    }
    return out;
}

// --- TIFF with every bit (libtiff) ------------------------------------------------------------------------------------

struct TiffMemory {
    std::string_view bytes;
    toff_t at = 0;
};
tsize_t tiff_read(thandle_t handle, tdata_t buffer, tsize_t size) {
    auto* m = static_cast<TiffMemory*>(handle);
    if (m->at >= m->bytes.size()) return 0;
    const auto n = static_cast<tsize_t>(std::min<toff_t>(static_cast<toff_t>(size), m->bytes.size() - m->at));
    std::memcpy(buffer, m->bytes.data() + m->at, static_cast<std::size_t>(n));
    m->at += static_cast<toff_t>(n);
    return n;
}
tsize_t tiff_write(thandle_t, tdata_t, tsize_t) { return 0; }
toff_t tiff_seek(thandle_t handle, toff_t offset, int whence) {
    auto* m = static_cast<TiffMemory*>(handle);
    const toff_t base = whence == SEEK_CUR ? m->at : whence == SEEK_END ? m->bytes.size() : 0;
    m->at = base + offset;
    return m->at;
}
int tiff_close(thandle_t) { return 0; }
toff_t tiff_size(thandle_t handle) { return static_cast<TiffMemory*>(handle)->bytes.size(); }
int tiff_map(thandle_t, tdata_t*, toff_t*) { return 0; }
void tiff_unmap(thandle_t, tdata_t, toff_t) {}
void tiff_quiet(const char*, const char*, va_list) {}

struct Tiff {
    TiffMemory memory;
    TIFF* tif = nullptr;
    explicit Tiff(std::string_view bytes) : memory{bytes, 0} {
        TIFFSetErrorHandler(tiff_quiet);
        TIFFSetWarningHandler(tiff_quiet);
        tif = TIFFClientOpen("picture", "rm", &memory, tiff_read, tiff_write, tiff_seek, tiff_close, tiff_size, tiff_map, tiff_unmap);
        if (tif == nullptr) throw core::Error("format", "the TIFF cannot be read");
    }
    ~Tiff() {
        if (tif != nullptr) TIFFClose(tif);
    }
    Tiff(const Tiff&) = delete;
    Tiff& operator=(const Tiff&) = delete;
};

struct TiffShape {
    std::uint32_t width = 0, height = 0;
    std::uint16_t bits = 8, samples = 1, format = SAMPLEFORMAT_UINT, photometric = PHOTOMETRIC_MINISBLACK, planar = PLANARCONFIG_CONTIG;
    std::uint16_t extra = 0, extra_kind = EXTRASAMPLE_UNSPECIFIED, unit = RESUNIT_INCH;
    float xres = 0;
};

TiffShape tiff_shape(TIFF* tif) {
    TiffShape s;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &s.width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &s.height);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &s.bits);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &s.samples);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &s.format);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &s.planar);
    if (TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &s.photometric) == 0) s.photometric = PHOTOMETRIC_MINISBLACK;
    std::uint16_t* kinds = nullptr;
    if (TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &s.extra, &kinds) != 0 && s.extra > 0 && kinds != nullptr) s.extra_kind = kinds[0];
    if (TIFFGetField(tif, TIFFTAG_XRESOLUTION, &s.xres) == 0) s.xres = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &s.unit);
    return s;
}

unsigned tiff_colours(const TiffShape& s) {
    if (s.photometric == PHOTOMETRIC_RGB) return 3;
    if (s.photometric == PHOTOMETRIC_SEPARATED) return 4;
    return 1;
}

DeepPicture tiff_deep(std::string_view bytes) {
    Tiff t(bytes);
    const TiffShape s = tiff_shape(t.tif);
    const unsigned colours = tiff_colours(s);
    if (s.planar != PLANARCONFIG_CONTIG) throw NotYetPorted("image_format");
    if (s.samples < colours || s.samples > colours + 1 || (s.bits != 16 && s.bits != 32))
        throw core::Error("format", "the TIFF's samples cannot be read with all their bits");
    if (static_cast<std::uint64_t>(s.width) * s.height > static_cast<std::uint64_t>(2 * kMaxImagePixels))
        throw core::Error("image_too_large", "the picture has too many pixels");
    DeepPicture out;
    out.width = s.width;
    out.height = s.height;
    out.channels = s.samples;
    out.alpha = s.samples > colours;
    out.depth = s.bits;
    out.mode = std::string(colours == 1 ? "gray" : colours == 3 ? "rgb" : "cmyk") + (out.alpha ? "a" : "");
    out.samples.assign(std::size_t(s.width) * s.height * s.samples, 0.0f);
    const std::size_t pixel_bytes = std::size_t(s.samples) * (s.bits / 8);
    const auto take = [&](const unsigned char* from, std::size_t x, std::size_t y, std::size_t n) {
        for (std::size_t i = 0; i < n * s.samples; ++i) {
            float v = 0;
            if (s.bits == 16) {
                std::uint16_t raw = 0;
                std::memcpy(&raw, from + i * 2, 2);
                v = static_cast<float>(raw / 65535.0);
            } else if (s.format == SAMPLEFORMAT_IEEEFP) {
                std::memcpy(&v, from + i * 4, 4);
            } else {
                std::uint32_t raw = 0;
                std::memcpy(&raw, from + i * 4, 4);
                v = static_cast<float>(raw / 4294967295.0);
            }
            out.samples[(y * s.width + x) * s.samples + i] = v;
        }
    };
    if (TIFFIsTiled(t.tif)) {
        std::uint32_t tw = 0, th = 0;
        TIFFGetField(t.tif, TIFFTAG_TILEWIDTH, &tw);
        TIFFGetField(t.tif, TIFFTAG_TILELENGTH, &th);
        if (tw == 0 || th == 0) throw core::Error("format", "the TIFF's tiles cannot be read");
        std::vector<unsigned char> tile(static_cast<std::size_t>(TIFFTileSize(t.tif)));
        for (std::uint32_t ty = 0; ty < s.height; ty += th) {
            for (std::uint32_t tx = 0; tx < s.width; tx += tw) {
                if (TIFFReadTile(t.tif, tile.data(), tx, ty, 0, 0) < 0) throw core::Error("format", "the TIFF is cut short or broken");
                for (std::uint32_t y = ty; y < std::min(s.height, ty + th); ++y) {
                    const std::size_t n = std::min(s.width, tx + tw) - tx;
                    take(tile.data() + std::size_t(y - ty) * tw * pixel_bytes, tx, y, n);
                }
            }
        }
    } else {
        std::vector<unsigned char> row(static_cast<std::size_t>(TIFFScanlineSize(t.tif)));
        for (std::uint32_t y = 0; y < s.height; ++y) {
            if (TIFFReadScanline(t.tif, row.data(), y, 0) < 0) throw core::Error("format", "the TIFF is cut short or broken");
            take(row.data(), 0, y, s.width);
        }
    }
    if (s.photometric == PHOTOMETRIC_MINISWHITE) {
        for (std::size_t i = 0; i < out.samples.size(); i += out.channels) out.samples[i] = 1.0f - out.samples[i];
    }
    if (out.alpha && s.extra_kind == EXTRASAMPLE_ASSOCALPHA) {  // (premultiplied: straight, as the page holds a picture)
        for (std::size_t i = 0; i < out.samples.size(); i += out.channels) {
            const float a = out.samples[i + colours];
            for (unsigned c = 0; c < colours; ++c) out.samples[i + c] = a > 0 ? std::min(1.0f, out.samples[i + c] / a) : 0.0f;
        }
    }
    return out;
}

// --- PSD and PSB (render/psd, PC3 A3) ---------------------------------------------------------------------------------

// The file read as render/psd reads it (PSD and PSB, every colour mode — Lab too —, 1 to 32 bits), its errors as a picture's.
psd::File psd_file(std::string_view bytes) {
    try {
        psd::File f = psd::File::read(std::string(bytes), psd::default_limits(), true);
        if (f.width() <= 0 || f.height() <= 0) throw core::Error("format", "the PSD has no picture");
        if (static_cast<std::uint64_t>(f.width()) * static_cast<std::uint64_t>(f.height()) > static_cast<std::uint64_t>(kPillowOpenLimits.max_pixels))
            throw core::Error("image_too_large", "the picture has too many pixels");
        return f;
    } catch (const psd::ReadError& error) {
        if (error.too_large()) throw core::Error("image_too_large", error.what());
        throw core::Error("format", std::string("the PSD cannot be read (") + error.what() + ")");
    }
}

// Lab bytes (L 0..255 for 0..100, a and b with 128 for 0: a PSD's, Pillow's "LAB") as sRGB, through Little CMS's Lab (D50).
Image lab_to_rgb(const Image& lab) {
    std::string in = lab.tobytes();
    const std::size_t count = std::size_t(lab.width()) * lab.height();
    for (std::size_t i = 0; i < count; ++i) {  // (Pillow's LAB bytes give a and b signed: back to 128 for 0)
        in[i * 3 + 1] = static_cast<char>(in[i * 3 + 1] ^ 0x80);
        in[i * 3 + 2] = static_cast<char>(in[i * 3 + 2] ^ 0x80);
    }
    std::string out(count * 3, '\0');
    cmsHPROFILE from = cmsCreateLab4Profile(nullptr);
    cmsHPROFILE to = cmsCreate_sRGBProfile();
    cmsHTRANSFORM transform = from != nullptr && to != nullptr
                                  ? cmsCreateTransform(from, TYPE_Lab_8, to, TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE)
                                  : nullptr;
    if (from != nullptr) cmsCloseProfile(from);
    if (to != nullptr) cmsCloseProfile(to);
    if (transform == nullptr) throw core::Error("value", "the Lab picture cannot be taken to sRGB");
    cmsDoTransform(transform, in.data(), out.data(), static_cast<cmsUInt32Number>(count));
    cmsDeleteTransform(transform);
    return Image::frombytes("RGB", lab.size(), out);
}

// The PSD as the 8-bit page reads it: its merged picture in its own colours (grey, RGB, CMYK; Lab as sRGB; an indexed
// file's colours as Pillow reads them), or — a file without one, or with a blank one (saved without maximised
// compatibility) — its visible layers laid over each other.
Image psd_eight(const psd::File& f, std::string_view bytes) {
    if (f.mode() == 2 && !f.psb() && f.depth() == 8) return open_image(bytes, kPillowOpenLimits);  // (its palette)
    std::optional<Image> picture = f.real_merged() ? f.merged_picture() : std::nullopt;
    if (!picture) {
        if (std::none_of(f.layers().begin(), f.layers().end(), [](const psd::Layer& l) { return !l.folder && l.kind != "adjust"; }))
            throw core::Error("format", "the PSD has no picture");
        return f.composited();
    }
    if (picture->mode() == "LAB") return lab_to_rgb(*picture);
    return std::move(*picture);
}

// --- a picture's resolution, as its file says it --------------------------------------------------------------------

double jpeg_dpi(std::string_view b) {
    std::size_t at = 2;
    while (at + 4 <= b.size() && u8(b, at) == 0xff) {
        const unsigned marker = u8(b, at + 1);
        if (marker == 0xda || marker == 0xd9) break;
        const std::size_t length = be16(b, at + 2);
        if (length < 2 || at + 2 + length > b.size()) break;
        if (marker == 0xe0 && length >= 14 && b.substr(at + 4, 5) == std::string_view("JFIF\0", 5)) {
            const unsigned unit = u8(b, at + 11);
            const double x = be16(b, at + 12);
            if (unit == 1 && x > 0) return x;
            if (unit == 2 && x > 0) return x * 2.54;
            return 0;
        }
        at += 2 + length;
    }
    return 0;
}

double bmp_dpi(std::string_view b) {
    if (b.size() < 46 || le32(b, 14) < 40) return 0;
    const double ppm = le32(b, 38);
    return ppm > 0 ? ppm * 0.0254 : 0;
}

std::string mode_of(std::string_view pillow, bool transparent) {
    if (pillow == "1" || pillow == "L" || pillow.starts_with("I") || pillow == "F") return transparent ? "graya" : "gray";
    if (pillow == "LA" || pillow == "La") return "graya";
    if (pillow == "RGBA" || pillow == "RGBa" || pillow == "PA") return "rgba";
    if (pillow == "CMYK") return "cmyk";
    if (pillow == "LAB") return "lab";
    return transparent ? "rgba" : "rgb";
}

int depth_of(std::string_view pillow) {
    if (pillow == "1") return 1;
    if (pillow == "I;16" || pillow == "I;16B" || pillow == "I;16L") return 16;
    if (pillow == "I" || pillow == "F") return 32;
    return 8;
}

// Little CMS's way for an intent's name.
cmsUInt32Number intent_of(std::string_view intent) {
    if (intent == "perceptual") return INTENT_PERCEPTUAL;
    if (intent == "saturation") return INTENT_SATURATION;
    if (intent == "absolute") return INTENT_ABSOLUTE_COLORIMETRIC;
    return INTENT_RELATIVE_COLORIMETRIC;
}

struct Profile {
    cmsHPROFILE handle = nullptr;
    explicit Profile(cmsHPROFILE h) : handle(h) {}
    ~Profile() {
        if (handle != nullptr) cmsCloseProfile(handle);
    }
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
};

}  // namespace

FileInfo look_at(std::string_view bytes, std::string_view suffix) {
    if (bytes.size() > kFileMaxBytes) throw core::Error("too_large", "the file is larger than 256 MiB");
    const auto kind = sniff(bytes);
    if (!kind) throw core::Error("unidentified_image", "the file is not a picture this build reads");
    FileInfo info;
    info.format = kind->first;
    info.suffix = kind->second;
    if (!suffix.empty()) {
        const auto said = core::smart::format_of_suffix(suffix);
        if (!said || *said != info.format) throw core::Error("unidentified_image", "the file is not the " + std::string(suffix) + " its name says");
        info.suffix = std::string(suffix);
    }
    if (info.format == "psd") {  // (PC3 A3: PSD and PSB, every colour mode and depth, read by render/psd)
        const psd::File f = psd_file(bytes);
        const Image picture = psd_eight(f, bytes);  // (decoded whole, as the drawing decodes it)
        info.width = f.width();
        info.height = f.height();
        info.depth = f.depth();
        const bool alpha = picture.mode() == "RGBA" || picture.mode() == "LA";
        const int mode = f.mode();
        info.mode = mode == 4 ? "cmyk" : mode == 9 ? "lab" : (mode == 0 || mode == 1 || mode == 8) ? "gray" : "rgb";
        if (mode == 2 && picture.mode() != "L" && picture.mode() != "LA") info.mode = "rgb";  // (indexed: its palette's colours)
        if (alpha && (info.mode == "gray" || info.mode == "rgb")) info.mode += "a";
        if (picture.mode() == "RGBA" && info.mode == "graya") info.mode = "rgba";  // (its layers laid: RGB)
        info.dpi = f.dpi();
        if (!std::isfinite(info.dpi) || info.dpi < 1 || info.dpi > 100000) info.dpi = 0;
        info.dpi = std::round(info.dpi * 1000) / 1000;
        info.icc = proof::embedded_profile(bytes);
        return info;
    }
    // (the whole picture decoded as the drawing decodes it: a picture cut short, broken or too large is refused here)
    const Image picture = eight_bit_picture(bytes);
    info.width = picture.width();
    info.height = picture.height();
    const bool transparent = picture.transparency().present();
    info.mode = mode_of(picture.mode(), transparent);
    info.depth = depth_of(picture.mode());
    if (info.format == "png") {
        const PngShape s = png_shape(bytes);
        if (s.bit_depth == 16) info.depth = 16;
        if (s.phys && s.unit == PNG_RESOLUTION_METER && s.ppm_x > 0) info.dpi = s.ppm_x * 0.0254;
    } else if (info.format == "tiff") {
        Tiff t(bytes);
        const TiffShape s = tiff_shape(t.tif);
        if (s.bits == 16 || s.bits == 32) info.depth = s.bits;
        if (s.photometric == PHOTOMETRIC_SEPARATED) info.mode = s.samples > 4 ? "cmyka" : "cmyk";
        if (s.xres > 0) info.dpi = s.unit == RESUNIT_CENTIMETER ? s.xres * 2.54 : s.unit == RESUNIT_INCH ? s.xres : 0;
    } else if (info.format == "jpeg") {
        info.dpi = jpeg_dpi(bytes);
    } else if (info.format == "bmp") {
        info.dpi = bmp_dpi(bytes);
    }
    if (!std::isfinite(info.dpi) || info.dpi < 1 || info.dpi > 100000) info.dpi = 0;
    info.dpi = std::round(info.dpi * 1000) / 1000;
    info.icc = proof::embedded_profile(bytes);
    return info;
}

Image eight_bit_picture(std::string_view bytes) {
    if (const auto kind = sniff(bytes); kind && kind->first == "psd") return psd_eight(psd_file(bytes), bytes);  // (PC3 A3)
    try {
        return open_image(bytes, kPillowOpenLimits);
    } catch (const NotYetPorted&) {
        throw;
    } catch (const core::Error& error) {
        // (a PNG or TIFF of 16 or 32 bits open_image does not read — an RGB TIFF of 16 bits —: its samples, rounded)
        const auto kind = sniff(bytes);
        if (error.code() == "image_too_large" || !kind || (kind->first != "png" && kind->first != "tiff")) throw;
        bool deep_file = false;
        try {
            if (kind->first == "png") {
                deep_file = png_shape(bytes).bit_depth == 16;
            } else {
                Tiff t(bytes);
                const TiffShape s = tiff_shape(t.tif);
                deep_file = s.bits == 16 || s.bits == 32;
            }
        } catch (const core::Error&) {
        }
        if (!deep_file) throw;
        const DeepPicture deep = deep_picture(bytes);
        const unsigned colours = deep.alpha ? deep.channels - 1 : deep.channels;
        const std::string_view mode = colours == 4 ? "CMYK" : colours == 3 ? (deep.alpha ? "RGBA" : "RGB") : (deep.alpha ? "LA" : "L");
        const unsigned stride = mode == "L" ? 1 : mode == "RGB" ? 3 : 4;
        std::string packed(std::size_t(deep.width) * deep.height * stride, '\0');
        for (std::size_t i = 0; i < std::size_t(deep.width) * deep.height; ++i) {
            const float* s = deep.samples.data() + i * deep.channels;
            const auto byte = [](float v) { return static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f))); };
            if (mode == "LA") {
                packed[i * 4] = packed[i * 4 + 1] = packed[i * 4 + 2] = byte(s[0]);
                packed[i * 4 + 3] = byte(s[1]);
                continue;
            }
            for (unsigned c = 0; c < stride; ++c) packed[i * stride + c] = byte(s[c]);
        }
        const Image out = Image::frombytes(mode == "LA" ? "RGBA" : mode, Size{static_cast<int>(deep.width), static_cast<int>(deep.height)}, packed);
        return mode == "LA" ? out.convert("LA") : out;
    }
}

DeepPicture deep_picture(std::string_view bytes) {
    const auto kind = sniff(bytes);
    if (kind && kind->first == "psd") {  // (PC3 A3: a grey or RGB PSD of 16 or 32 bits, its merged picture's samples)
        const psd::File f = psd_file(bytes);
        const int mode = f.mode();
        const bool colours_kept = mode == 0 || mode == 1 || mode == 3 || mode == 8;
        if ((f.depth() == 16 || f.depth() == 32) && colours_kept && f.real_merged()) {
            if (auto merged = f.merged_deep()) {
                DeepPicture out;
                out.width = merged->width;
                out.height = merged->height;
                out.channels = merged->colours + (merged->alpha ? 1 : 0);
                out.alpha = merged->alpha;
                out.depth = f.depth();
                out.mode = std::string(merged->colours == 1 ? "gray" : "rgb") + (out.alpha ? "a" : "");
                out.samples = std::move(merged->samples);
                if (f.depth() == 32) {
                    // (a 32-bit file's colours are linear light: encoded as the 8-bit picture encodes them, 1/2.2, its
                    // values past 1 kept)
                    for (std::size_t i = 0; i < out.samples.size(); ++i) {
                        if (out.alpha && i % out.channels == out.channels - 1) continue;
                        out.samples[i] = static_cast<float>(std::pow(std::max(0.0f, out.samples[i]), 1.0 / 2.2));
                    }
                }
                return out;
            }
        }
    }
    if (kind && kind->first == "png" && png_shape(bytes).bit_depth == 16) return png_deep(bytes);
    if (kind && kind->first == "tiff") {
        Tiff t(bytes);
        const TiffShape s = tiff_shape(t.tif);
        if (s.bits == 16 || s.bits == 32) return tiff_deep(bytes);
    }
    const Image picture = eight_bit_picture(bytes);
    const std::string_view mode = picture.mode();
    DeepPicture out;
    out.width = picture.width();
    out.height = picture.height();
    out.depth = depth_of(mode);
    Image in;
    if (mode == "CMYK") {
        in = picture;
        out.channels = 4;
        out.mode = "cmyk";
    } else if (mode == "1" || mode == "L") {
        in = picture.convert("L");
        out.channels = 1;
        out.mode = "gray";
    } else if (mode == "LA") {
        in = picture;
        out.channels = 2;
        out.alpha = true;
        out.mode = "graya";
    } else {
        in = picture.convert("RGBA");
        out.channels = 4;
        out.alpha = true;
        out.mode = "rgba";
    }
    out.samples.resize(std::size_t(out.width) * out.height * out.channels);
    const unsigned stride = in.mode() == "L" ? 1 : 4;  // (libImaging keeps LA, RGBA and CMYK in four bytes)
    for (int y = 0; y < in.height(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(in.raw()->image[y]);
        for (int x = 0; x < in.width(); ++x) {
            float* to = out.samples.data() + (std::size_t(y) * out.width + x) * out.channels;
            if (out.mode == "graya") {
                to[0] = row[x * 4] / 255.0f;
                to[1] = row[x * 4 + 3] / 255.0f;
                continue;
            }
            for (unsigned c = 0; c < out.channels; ++c) to[c] = row[x * stride + c] / 255.0f;
        }
    }
    return out;
}

std::vector<float> srgb_floats(const DeepPicture& p, const std::optional<std::string>& icc, std::string_view intent, bool bpc) {
    const std::size_t count = std::size_t(p.width) * p.height;
    const unsigned colours = p.alpha ? p.channels - 1 : p.channels;
    std::vector<float> out(count * 4, 1.0f);
    for (std::size_t i = 0; i < count; ++i) out[i * 4 + 3] = p.alpha ? p.samples[i * p.channels + colours] : 1.0f;
    if (icc && !icc->empty()) {
        const Profile from(cmsOpenProfileFromMem(icc->data(), static_cast<cmsUInt32Number>(icc->size())));
        const Profile srgb(cmsCreate_sRGBProfile());
        if (from.handle == nullptr) throw core::Error("value", "the picture's ICC profile cannot be read");
        const cmsColorSpaceSignature space = cmsGetColorSpace(from.handle);
        const bool fits = (space == cmsSigGrayData && colours == 1) || (space == cmsSigRgbData && colours == 3) ||
                          (space == cmsSigCmykData && colours == 4);
        if (!fits) throw core::Error("value", "the ICC profile is not of the picture's colours");
        const cmsUInt32Number in_format = colours == 1 ? TYPE_GRAY_FLT : colours == 3 ? TYPE_RGB_FLT : TYPE_CMYK_FLT;
        cmsHTRANSFORM transform = cmsCreateTransform(from.handle, in_format, srgb.handle, TYPE_RGB_FLT, intent_of(intent),
                                                     (bpc ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0) | cmsFLAGS_NOCACHE);
        if (transform == nullptr) throw core::Error("value", "the picture's colours cannot be taken from its profile");
        std::vector<float> in(count * colours), rgb(count * 3);
        for (std::size_t i = 0; i < count; ++i) {
            for (unsigned c = 0; c < colours; ++c) {
                const float v = p.samples[i * p.channels + c];
                in[i * colours + c] = colours == 4 ? v * 100.0f : v;  // (Little CMS's CMYK floats are 0..100)
            }
        }
        cmsDoTransform(transform, in.data(), rgb.data(), static_cast<cmsUInt32Number>(count));
        cmsDeleteTransform(transform);
        for (std::size_t i = 0; i < count; ++i)
            for (unsigned c = 0; c < 3; ++c) out[i * 4 + c] = rgb[i * 3 + c];
        return out;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const float* s = p.samples.data() + i * p.channels;
        if (colours == 1) {
            out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = s[0];
        } else if (colours == 4) {  // (Pillow's plain CMYK → RGB: what is left of each colour after its ink and black)
            for (unsigned c = 0; c < 3; ++c) out[i * 4 + c] = std::max(0.0f, 1.0f - std::min(1.0f, s[c] + s[3]));
        } else {
            for (unsigned c = 0; c < 3; ++c) out[i * 4 + c] = s[c];
        }
    }
    return out;
}

core::InkPixels file_inks(std::string_view bytes, const core::PageColor& color, std::string_view use) {
    const auto kind = sniff(bytes);
    const bool keep = use == "keep";
    if (kind && kind->first == "psd") {  // (PC3 A3: a CMYK or Lab PSD's merged picture as the page's inks, render/psd's)
        const psd::File f = psd_file(bytes);
        if ((f.mode() == 4 || f.mode() == 9) && f.real_merged()) {
            if (auto inks = f.merged_inks(color, !keep)) return std::move(*inks);
        }
        const Image picture = psd_eight(f, bytes);
        if (picture.mode() == "CMYK") return ink::from_cmyk_image(picture, nullptr, keep ? std::string() : f.icc(), color);
        return ink::from_rgb(picture.convert("RGBA"), color);
    }
    if (kind && kind->first == "tiff") {
        Tiff t(bytes);
        const TiffShape s = tiff_shape(t.tif);
        if ((s.bits == 16 || s.bits == 32) && (s.photometric == PHOTOMETRIC_SEPARATED || s.photometric <= PHOTOMETRIC_MINISBLACK)) {
            const DeepPicture deep = tiff_deep(bytes);
            const bool cmyk = s.photometric == PHOTOMETRIC_SEPARATED;
            core::InkPixels source;
            source.space = cmyk ? core::InkSpace::Cmyk : core::InkSpace::Gray;
            source.depth = 16;
            source.width = static_cast<std::uint32_t>(deep.width);
            source.height = static_cast<std::uint32_t>(deep.height);
            const unsigned colours = cmyk ? 4 : 1, all = colours + 1;
            source.samples.assign(source.pixel_count() * all, 0);
            for (std::size_t i = 0; i < source.pixel_count(); ++i) {
                const float* from = deep.samples.data() + i * deep.channels;
                for (unsigned c = 0; c < colours; ++c)
                    source.samples[i * all + c] = static_cast<std::uint16_t>(std::lround(std::clamp(from[c], 0.0f, 1.0f) * 65535.0f));
                source.samples[i * all + colours] =
                    static_cast<std::uint16_t>(std::lround((deep.alpha ? std::clamp(from[colours], 0.0f, 1.0f) : 1.0f) * 65535.0f));
            }
            core::PageColor own;
            own.space = source.space;
            own.depth = 16;
            own.intent = color.intent;
            const std::string icc = ink::picture_icc(bytes).empty() ? proof::embedded_profile(bytes).value_or("") : ink::picture_icc(bytes);
            if (!keep && !icc.empty()) {
                own.icc = std::make_shared<const std::string>(icc);
                own.profile = core::bytes_ref(icc);
            } else if (color.space == source.space) {
                own.icc = color.icc;  // (its numbers as the page's own)
                own.profile = color.profile;
            }
            return ink::convert(source, own, color);
        }
    }
    const Image picture = eight_bit_picture(bytes);
    if (picture.mode() == "CMYK") return ink::from_cmyk_image(picture, nullptr, keep ? std::string() : ink::picture_icc(bytes), color);
    if (picture.mode() == "LAB") return ink::from_lab_image(picture, nullptr, color);
    return ink::from_rgb(picture.convert("RGBA"), color);
}

}  // namespace genko::render::smart
