// TIFF and WebP pictures as Pillow reads them (PIL.TiffImagePlugin and PIL.WebPImagePlugin, Pillow 12).

#include "render/tiff_webp.hpp"

#include <tiffio.h>
#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "render/not_yet_ported.hpp"

namespace genko::render {

namespace {

// Pillow's OPEN_INFO: (big-endian, PhotometricInterpretation, SampleFormat, FillOrder, BitsPerSample, ExtraSamples)
// → (mode, raw mode)
struct OpenInfo {
    int big;
    int photo;
    std::vector<int> sample_format;
    int fill_order;
    std::vector<int> bits;
    std::vector<int> extra;
    const char* mode;
    const char* rawmode;
};

const std::vector<OpenInfo>& open_info() {
    static const std::vector<OpenInfo> table{
    {0, 0, {1}, 1, {1}, {}, "1", "1;I"},
    {1, 0, {1}, 1, {1}, {}, "1", "1;I"},
    {0, 0, {1}, 2, {1}, {}, "1", "1;IR"},
    {1, 0, {1}, 2, {1}, {}, "1", "1;IR"},
    {0, 1, {1}, 1, {1}, {}, "1", "1"},
    {1, 1, {1}, 1, {1}, {}, "1", "1"},
    {0, 1, {1}, 2, {1}, {}, "1", "1;R"},
    {1, 1, {1}, 2, {1}, {}, "1", "1;R"},
    {0, 0, {1}, 1, {2}, {}, "L", "L;2I"},
    {1, 0, {1}, 1, {2}, {}, "L", "L;2I"},
    {0, 0, {1}, 2, {2}, {}, "L", "L;2IR"},
    {1, 0, {1}, 2, {2}, {}, "L", "L;2IR"},
    {0, 1, {1}, 1, {2}, {}, "L", "L;2"},
    {1, 1, {1}, 1, {2}, {}, "L", "L;2"},
    {0, 1, {1}, 2, {2}, {}, "L", "L;2R"},
    {1, 1, {1}, 2, {2}, {}, "L", "L;2R"},
    {0, 0, {1}, 1, {4}, {}, "L", "L;4I"},
    {1, 0, {1}, 1, {4}, {}, "L", "L;4I"},
    {0, 0, {1}, 2, {4}, {}, "L", "L;4IR"},
    {1, 0, {1}, 2, {4}, {}, "L", "L;4IR"},
    {0, 1, {1}, 1, {4}, {}, "L", "L;4"},
    {1, 1, {1}, 1, {4}, {}, "L", "L;4"},
    {0, 1, {1}, 2, {4}, {}, "L", "L;4R"},
    {1, 1, {1}, 2, {4}, {}, "L", "L;4R"},
    {0, 0, {1}, 1, {8}, {}, "L", "L;I"},
    {1, 0, {1}, 1, {8}, {}, "L", "L;I"},
    {0, 0, {1}, 2, {8}, {}, "L", "L;IR"},
    {1, 0, {1}, 2, {8}, {}, "L", "L;IR"},
    {0, 1, {1}, 1, {8}, {}, "L", "L"},
    {1, 1, {1}, 1, {8}, {}, "L", "L"},
    {0, 1, {2}, 1, {8}, {}, "L", "L"},
    {1, 1, {2}, 1, {8}, {}, "L", "L"},
    {0, 1, {1}, 2, {8}, {}, "L", "L;R"},
    {1, 1, {1}, 2, {8}, {}, "L", "L;R"},
    {0, 1, {1}, 1, {12}, {}, "I;16", "I;12"},
    {0, 0, {1}, 1, {16}, {}, "I;16", "I;16"},
    {0, 1, {1}, 1, {16}, {}, "I;16", "I;16"},
    {1, 1, {1}, 1, {16}, {}, "I;16B", "I;16B"},
    {0, 1, {1}, 2, {16}, {}, "I;16", "I;16R"},
    {0, 1, {2}, 1, {16}, {}, "I", "I;16S"},
    {1, 1, {2}, 1, {16}, {}, "I", "I;16BS"},
    {0, 0, {3}, 1, {32}, {}, "F", "F;32F"},
    {1, 0, {3}, 1, {32}, {}, "F", "F;32BF"},
    {0, 1, {1}, 1, {32}, {}, "I", "I;32N"},
    {0, 1, {2}, 1, {32}, {}, "I", "I;32S"},
    {1, 1, {2}, 1, {32}, {}, "I", "I;32BS"},
    {0, 1, {3}, 1, {32}, {}, "F", "F;32F"},
    {1, 1, {3}, 1, {32}, {}, "F", "F;32BF"},
    {0, 1, {1}, 1, {8,8}, {2}, "LA", "LA"},
    {1, 1, {1}, 1, {8,8}, {2}, "LA", "LA"},
    {0, 2, {1}, 1, {8,8,8}, {}, "RGB", "RGB"},
    {1, 2, {1}, 1, {8,8,8}, {}, "RGB", "RGB"},
    {0, 2, {1}, 2, {8,8,8}, {}, "RGB", "RGB;R"},
    {1, 2, {1}, 2, {8,8,8}, {}, "RGB", "RGB;R"},
    {0, 2, {1}, 1, {8,8,8,8}, {}, "RGBA", "RGBA"},
    {1, 2, {1}, 1, {8,8,8,8}, {}, "RGBA", "RGBA"},
    {0, 2, {1}, 1, {8,8,8,8}, {0}, "RGB", "RGBX"},
    {1, 2, {1}, 1, {8,8,8,8}, {0}, "RGB", "RGBX"},
    {0, 2, {1}, 1, {8,8,8,8,8}, {0,0}, "RGB", "RGBXX"},
    {1, 2, {1}, 1, {8,8,8,8,8}, {0,0}, "RGB", "RGBXX"},
    {0, 2, {1}, 1, {8,8,8,8,8,8}, {0,0,0}, "RGB", "RGBXXX"},
    {1, 2, {1}, 1, {8,8,8,8,8,8}, {0,0,0}, "RGB", "RGBXXX"},
    {0, 2, {1}, 1, {8,8,8,8}, {1}, "RGBA", "RGBa"},
    {1, 2, {1}, 1, {8,8,8,8}, {1}, "RGBA", "RGBa"},
    {0, 2, {1}, 1, {8,8,8,8,8}, {1,0}, "RGBA", "RGBaX"},
    {1, 2, {1}, 1, {8,8,8,8,8}, {1,0}, "RGBA", "RGBaX"},
    {0, 2, {1}, 1, {8,8,8,8,8,8}, {1,0,0}, "RGBA", "RGBaXX"},
    {1, 2, {1}, 1, {8,8,8,8,8,8}, {1,0,0}, "RGBA", "RGBaXX"},
    {0, 2, {1}, 1, {8,8,8,8}, {2}, "RGBA", "RGBA"},
    {1, 2, {1}, 1, {8,8,8,8}, {2}, "RGBA", "RGBA"},
    {0, 2, {1}, 1, {8,8,8,8,8}, {2,0}, "RGBA", "RGBAX"},
    {1, 2, {1}, 1, {8,8,8,8,8}, {2,0}, "RGBA", "RGBAX"},
    {0, 2, {1}, 1, {8,8,8,8,8,8}, {2,0,0}, "RGBA", "RGBAXX"},
    {1, 2, {1}, 1, {8,8,8,8,8,8}, {2,0,0}, "RGBA", "RGBAXX"},
    {0, 2, {1}, 1, {8,8,8,8}, {999}, "RGBA", "RGBA"},
    {1, 2, {1}, 1, {8,8,8,8}, {999}, "RGBA", "RGBA"},
    {0, 2, {1}, 1, {16,16,16}, {}, "RGB", "RGB;16L"},
    {1, 2, {1}, 1, {16,16,16}, {}, "RGB", "RGB;16B"},
    {0, 2, {1}, 1, {16,16,16,16}, {}, "RGBA", "RGBA;16L"},
    {1, 2, {1}, 1, {16,16,16,16}, {}, "RGBA", "RGBA;16B"},
    {0, 2, {1}, 1, {16,16,16,16}, {0}, "RGB", "RGBX;16L"},
    {1, 2, {1}, 1, {16,16,16,16}, {0}, "RGB", "RGBX;16B"},
    {0, 2, {1}, 1, {16,16,16,16}, {1}, "RGBA", "RGBa;16L"},
    {1, 2, {1}, 1, {16,16,16,16}, {1}, "RGBA", "RGBa;16B"},
    {0, 2, {1}, 1, {16,16,16,16}, {2}, "RGBA", "RGBA;16L"},
    {1, 2, {1}, 1, {16,16,16,16}, {2}, "RGBA", "RGBA;16B"},
    {0, 3, {1}, 1, {1}, {}, "P", "P;1"},
    {1, 3, {1}, 1, {1}, {}, "P", "P;1"},
    {0, 3, {1}, 2, {1}, {}, "P", "P;1R"},
    {1, 3, {1}, 2, {1}, {}, "P", "P;1R"},
    {0, 3, {1}, 1, {2}, {}, "P", "P;2"},
    {1, 3, {1}, 1, {2}, {}, "P", "P;2"},
    {0, 3, {1}, 2, {2}, {}, "P", "P;2R"},
    {1, 3, {1}, 2, {2}, {}, "P", "P;2R"},
    {0, 3, {1}, 1, {4}, {}, "P", "P;4"},
    {1, 3, {1}, 1, {4}, {}, "P", "P;4"},
    {0, 3, {1}, 2, {4}, {}, "P", "P;4R"},
    {1, 3, {1}, 2, {4}, {}, "P", "P;4R"},
    {0, 3, {1}, 1, {8}, {}, "P", "P"},
    {1, 3, {1}, 1, {8}, {}, "P", "P"},
    {0, 3, {1}, 1, {8,8}, {0}, "P", "PX"},
    {1, 3, {1}, 1, {8,8}, {0}, "P", "PX"},
    {0, 3, {1}, 1, {8,8}, {2}, "PA", "PA"},
    {1, 3, {1}, 1, {8,8}, {2}, "PA", "PA"},
    {0, 3, {1}, 2, {8}, {}, "P", "P;R"},
    {1, 3, {1}, 2, {8}, {}, "P", "P;R"},
    {0, 5, {1}, 1, {8,8,8,8}, {}, "CMYK", "CMYK"},
    {1, 5, {1}, 1, {8,8,8,8}, {}, "CMYK", "CMYK"},
    {0, 5, {1}, 1, {8,8,8,8,8}, {0}, "CMYK", "CMYKX"},
    {1, 5, {1}, 1, {8,8,8,8,8}, {0}, "CMYK", "CMYKX"},
    {0, 5, {1}, 1, {8,8,8,8,8,8}, {0,0}, "CMYK", "CMYKXX"},
    {1, 5, {1}, 1, {8,8,8,8,8,8}, {0,0}, "CMYK", "CMYKXX"},
    {0, 5, {1}, 1, {16,16,16,16}, {}, "CMYK", "CMYK;16L"},
    {1, 5, {1}, 1, {16,16,16,16}, {}, "CMYK", "CMYK;16B"},
    {0, 6, {1}, 1, {8}, {}, "L", "L"},
    {1, 6, {1}, 1, {8}, {}, "L", "L"},
    {0, 6, {1}, 1, {8,8,8}, {}, "RGB", "RGBX"},
    {1, 6, {1}, 1, {8,8,8}, {}, "RGB", "RGBX"},
    {0, 8, {1}, 1, {8,8,8}, {}, "LAB", "LAB"},
    {1, 8, {1}, 1, {8,8,8}, {}, "LAB", "LAB"},
    };
    return table;
}

constexpr std::size_t kMaxSamplesPerPixel = 6;

[[noreturn]] void unidentified(const std::string& why) { throw core::Error("unidentified_image", "cannot identify image file (" + why + ")"); }
[[noreturn]] void undecodable(const std::string& why) { throw core::Error("format", why); }

void check_size(std::int64_t width, std::int64_t height, const PngLimits& limits) {
    const std::int64_t pixels = width * height;
    if (pixels > limits.max_pixels) {
        throw core::Error("image_too_large", "Image size (" + std::to_string(pixels) + " pixels) exceeds limit of " + std::to_string(limits.max_pixels) +
                                                 " pixels, could be decompression bomb DOS attack.");
    }
    if (beyond_side(width, height, limits)) throw core::Error("image_too_large", "image is too wide or too tall");
}

// --- TIFF through libtiff, from memory ---------------------------------------------------------------------------------

struct Memory {
    std::string_view bytes;
    toff_t at = 0;
};

tsize_t on_read(thandle_t handle, tdata_t buffer, tsize_t size) {
    auto* m = static_cast<Memory*>(handle);
    if (m->at >= m->bytes.size()) return 0;
    const auto n = static_cast<tsize_t>(std::min<toff_t>(static_cast<toff_t>(size), m->bytes.size() - m->at));
    std::memcpy(buffer, m->bytes.data() + m->at, static_cast<std::size_t>(n));
    m->at += static_cast<toff_t>(n);
    return n;
}
tsize_t on_write(thandle_t, tdata_t, tsize_t) { return 0; }
toff_t on_seek(thandle_t handle, toff_t offset, int whence) {
    auto* m = static_cast<Memory*>(handle);
    toff_t base = whence == SEEK_CUR ? m->at : whence == SEEK_END ? m->bytes.size() : 0;
    m->at = base + offset;
    return m->at;
}
int on_close(thandle_t) { return 0; }
toff_t on_size(thandle_t handle) { return static_cast<Memory*>(handle)->bytes.size(); }
int on_map(thandle_t, tdata_t*, toff_t*) { return 0; }
void on_unmap(thandle_t, tdata_t, toff_t) {}

void quiet(const char*, const char*, va_list) {}

struct Tiff {
    Memory memory;
    TIFF* tif = nullptr;
    explicit Tiff(std::string_view bytes) : memory{bytes, 0} {
        static const bool silenced = [] {
            TIFFSetErrorHandler(quiet);  // (libtiff's own words are not printed: its failures become the errors below)
            TIFFSetWarningHandler(quiet);
            return true;
        }();
        (void)silenced;
        tif = TIFFClientOpen("picture", "rm", &memory, on_read, on_write, on_seek, on_close, on_size, on_map, on_unmap);
    }
    ~Tiff() {
        if (tif != nullptr) TIFFClose(tif);
    }
    Tiff(const Tiff&) = delete;
    Tiff& operator=(const Tiff&) = delete;
};

// What Pillow's _setup works out of the first directory.
struct Setup {
    int width = 0;
    int height = 0;
    int orientation = 1;
    int photo = 0;
    int compression = 1;
    int planar = 1;
    std::string mode;
    std::string rawmode;
};

std::vector<int> u16_list(TIFF* tif, ttag_t tag, int count) {
    std::vector<int> out;
    uint16_t* values = nullptr;
    if (TIFFGetFieldDefaulted(tif, tag, &values) == 1 && values != nullptr) {
        for (int i = 0; i < count; ++i) out.push_back(values[i]);
    }
    return out;
}

Setup setup(TIFF* tif) {
    Setup s;
    uint16_t compression = 1;
    TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression);
    s.compression = compression;
    uint16_t planar = 1;
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    s.planar = planar;
    uint16_t photo = 0;
    if (TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photo) != 1) photo = 0;  // (Pillow's default 0, not libtiff's guess)
    s.photo = compression == 6 ? 6 : photo;  // (old-style JPEG: YCbCr)
    uint16_t fill = 1;
    TIFFGetFieldDefaulted(tif, TIFFTAG_FILLORDER, &fill);
    uint32_t width = 0;
    uint32_t height = 0;
    if (TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width) != 1 || TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height) != 1) unidentified("Missing dimensions");
    s.width = static_cast<int>(width);
    s.height = static_cast<int>(height);
    uint16_t orientation = 1;
    if (TIFFGetField(tif, TIFFTAG_ORIENTATION, &orientation) == 1) s.orientation = orientation;
    uint16_t spp = 0;
    const bool has_spp = TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp) == 1;
    int samples = has_spp ? spp : (compression == 6 && (s.photo == 2 || s.photo == 6) ? 3 : 1);
    uint16_t bps_one = 1;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps_one);
    // (libtiff keeps one BitsPerSample and one SampleFormat for all the samples, as Pillow's tuple of equal values)
    uint16_t format_one = 1;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &format_one);
    std::vector<int> sample_format{format_one};
    std::vector<int> extra;
    uint16_t extra_count = 0;
    uint16_t* extra_values = nullptr;
    if (TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &extra_count, &extra_values) == 1) {
        for (int i = 0; i < extra_count; ++i) extra.push_back(extra_values[i]);
    }
    std::vector<int> bits(static_cast<std::size_t>(std::max(1, samples)), bps_one);
    if (s.planar == 2 && !extra.empty() && *std::max_element(extra.begin(), extra.end()) == 0) {
        bits.resize(bits.size() - std::min(bits.size(), extra.size()));
        samples -= static_cast<int>(extra.size());
        extra.clear();
    }
    if (static_cast<std::size_t>(samples) > kMaxSamplesPerPixel) unidentified("Invalid value for samples per pixel");
    if (bits.size() != static_cast<std::size_t>(samples)) bits.assign(static_cast<std::size_t>(std::max(0, samples)), bps_one);
    const int big = TIFFIsBigEndian(tif) ? 1 : 0;
    const auto find = [&](int fill_order) -> const OpenInfo* {
        for (const OpenInfo& info : open_info()) {
            if (info.big == big && info.photo == s.photo && info.sample_format == sample_format && info.fill_order == fill_order &&
                info.bits == bits && info.extra == extra) {
                return &info;
            }
        }
        return nullptr;
    };
    // (the key as the file has it, fill order 2 included: Pillow refuses at _open what its table does not know; then
    // libtiff turns the bits of FillOrder 2 round itself, and the key with FillOrder 1 gives the raw mode)
    if (find(fill) == nullptr) unidentified("unknown pixel mode");
    const OpenInfo* info = find(1);
    if (info == nullptr) unidentified("unknown pixel mode");
    s.mode = info->mode;
    s.rawmode = info->rawmode;
    if (s.photo == 6 && s.compression == 7 && s.planar == 1) s.rawmode = "RGB";  // (JPEG's YCbCr given as RGB)
    if (s.rawmode == "I;16") {
        s.rawmode = "I;16N";
    } else if (s.rawmode.size() > 4 && (s.rawmode.ends_with(";16B") || s.rawmode.ends_with(";16L"))) {
        s.rawmode = s.rawmode.substr(0, s.rawmode.size() - 1) + "N";
    }
    return s;
}

Image oriented(Image image, int orientation) {
    switch (orientation) {  // (ImageOps.exif_transpose)
        case 2: return image.transpose(Transpose::FlipLeftRight);
        case 3: return image.transpose(Transpose::Rotate180);
        case 4: return image.transpose(Transpose::FlipTopBottom);
        case 5: return image.transpose(Transpose::Transpose);
        case 6: return image.transpose(Transpose::Rotate270);
        case 7: return image.transpose(Transpose::Transverse);
        case 8: return image.transpose(Transpose::Rotate90);
        default: return image;
    }
}

}  // namespace

bool tiff_signature(std::string_view b) {
    for (const std::string_view prefix : {std::string_view("MM\x00\x2a", 4), std::string_view("II\x2a\x00", 4), std::string_view("MM\x2a\x00", 4),
                                          std::string_view("II\x00\x2a", 4), std::string_view("MM\x00\x2b", 4), std::string_view("II\x2b\x00", 4)}) {
        if (b.substr(0, 4) == prefix) return true;
    }
    return false;
}

Size tiff_size(std::string_view bytes, const PngLimits& limits) {
    Tiff t(bytes);
    if (t.tif == nullptr) unidentified("not a TIFF file");
    const Setup s = setup(t.tif);
    check_size(s.width, s.height, limits);
    return s.orientation >= 5 && s.orientation <= 8 ? Size{s.height, s.width} : Size{s.width, s.height};
}

bool tiff_unported(std::string_view bytes) {
    if (!tiff_signature(bytes)) return false;
    try {
        Tiff t(bytes);
        return t.tif != nullptr && setup(t.tif).planar == 2;
    } catch (const core::Error&) {
        return false;  // (not a picture Pillow opens: no picture, as Python's)
    }
}

Image read_tiff(std::string_view bytes, const PngLimits& limits) {
    Tiff t(bytes);
    if (t.tif == nullptr) unidentified("not a TIFF file");
    TIFF* tif = t.tif;
    const Setup s = setup(tif);
    check_size(s.width, s.height, limits);
    if (s.planar == 2) throw NotYetPorted("image_format");  // (the samples in separate planes)
    const Size size{s.width, s.height};
    Image image;
    if (s.photo == 6 && s.compression != 7) {
        // old-style YCbCr: through libtiff's RGBA reader, as Pillow's decoder reads it (raw mode RGBX)
        std::vector<uint32_t> raster(static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height));
        if (TIFFReadRGBAImageOriented(tif, static_cast<uint32_t>(s.width), static_cast<uint32_t>(s.height), raster.data(), ORIENTATION_TOPLEFT, 0) != 1) {
            undecodable("decoder error -2");
        }
        std::string data(raster.size() * 4, '\0');
        for (std::size_t i = 0; i < raster.size(); ++i) {
            data[i * 4] = static_cast<char>(TIFFGetR(raster[i]));
            data[i * 4 + 1] = static_cast<char>(TIFFGetG(raster[i]));
            data[i * 4 + 2] = static_cast<char>(TIFFGetB(raster[i]));
            data[i * 4 + 3] = static_cast<char>(TIFFGetA(raster[i]));
        }
        image = Image::frombytes(s.mode, size, data, "RGBX");
    } else {
        if (s.photo == 6 && s.compression == 7) TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
        const tmsize_t row = TIFFScanlineSize(tif);
        if (row <= 0) undecodable("decoder error -2");
        std::string data(static_cast<std::size_t>(row) * static_cast<std::size_t>(s.height), '\0');
        if (TIFFIsTiled(tif)) {
            uint32_t tile_w = 0;
            uint32_t tile_h = 0;
            TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tile_w);
            TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_h);
            const tmsize_t tile_row = TIFFTileRowSize(tif);
            const tmsize_t tile_bytes = TIFFTileSize(tif);
            if (tile_w == 0 || tile_h == 0 || tile_row <= 0 || tile_bytes <= 0) undecodable("decoder error -2");
            std::vector<unsigned char> tile(static_cast<std::size_t>(tile_bytes));
            for (uint32_t y = 0; y < static_cast<uint32_t>(s.height); y += tile_h) {
                for (uint32_t x = 0; x < static_cast<uint32_t>(s.width); x += tile_w) {
                    if (TIFFReadTile(tif, tile.data(), x, y, 0, 0) < 0) undecodable("decoder error -2");
                    const std::size_t at = static_cast<std::size_t>(x / tile_w) * static_cast<std::size_t>(tile_row);
                    for (uint32_t r = 0; r < tile_h && y + r < static_cast<uint32_t>(s.height); ++r) {
                        if (at >= static_cast<std::size_t>(row)) break;
                        const std::size_t take = std::min(static_cast<std::size_t>(tile_row), static_cast<std::size_t>(row) - at);
                        std::memcpy(data.data() + static_cast<std::size_t>(y + r) * static_cast<std::size_t>(row) + at, tile.data() + r * static_cast<std::size_t>(tile_row), take);
                    }
                }
            }
        } else {
            uint32_t rows_per_strip = static_cast<uint32_t>(s.height);
            TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
            rows_per_strip = std::max<uint32_t>(1, std::min<uint32_t>(rows_per_strip, static_cast<uint32_t>(s.height)));
            const tstrip_t strips = TIFFNumberOfStrips(tif);
            for (tstrip_t strip = 0; strip < strips; ++strip) {
                const std::size_t first = static_cast<std::size_t>(strip) * rows_per_strip;
                if (first >= static_cast<std::size_t>(s.height)) break;
                const std::size_t rows = std::min<std::size_t>(rows_per_strip, static_cast<std::size_t>(s.height) - first);
                if (TIFFReadEncodedStrip(tif, strip, data.data() + first * static_cast<std::size_t>(row), static_cast<tmsize_t>(rows) * row) < 0) {
                    undecodable("decoder error -2");
                }
            }
        }
        image = Image::frombytes(s.mode, size, data, s.rawmode);
    }
    if (s.mode == "P" || s.mode == "PA") {
        uint16_t* red = nullptr;
        uint16_t* green = nullptr;
        uint16_t* blue = nullptr;
        uint16_t bps = 8;
        TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
        if (TIFFGetField(tif, TIFFTAG_COLORMAP, &red, &green, &blue) == 1) {
            const std::size_t n = std::size_t(1) << std::min<int>(bps, 8);
            std::string palette(n * 3, '\0');
            for (std::size_t i = 0; i < n; ++i) {  // (o8(b // 256), each of the three lines in turn)
                palette[i * 3] = static_cast<char>(red[i] >> 8);
                palette[i * 3 + 1] = static_cast<char>(green[i] >> 8);
                palette[i * 3 + 2] = static_cast<char>(blue[i] >> 8);
            }
            image.putpalette("RGB", palette);
        }
    }
    return oriented(std::move(image), s.orientation);
}

// --- WebP through libwebp's animation decoder ---------------------------------------------------------------------------

bool webp_signature(std::string_view b) {
    if (b.size() < 16 || b.substr(0, 4) != "RIFF" || b.substr(8, 4) != "WEBP") return false;
    const std::string_view chunk = b.substr(12, 4);
    return chunk == "VP8 " || chunk == "VP8X" || chunk == "VP8L";
}

namespace {

struct AnimDecoder {
    WebPAnimDecoder* decoder = nullptr;
    ~AnimDecoder() {
        if (decoder != nullptr) WebPAnimDecoderDelete(decoder);
    }
};

}  // namespace

Size webp_size(std::string_view bytes, const PngLimits& limits) {
    if (!webp_signature(bytes)) unidentified("not a WEBP file");
    WebPData data{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
    AnimDecoder anim;
    anim.decoder = WebPAnimDecoderNew(&data, nullptr);
    if (anim.decoder == nullptr) unidentified("could not create decoder object");
    WebPAnimInfo info{};
    if (!WebPAnimDecoderGetInfo(anim.decoder, &info)) unidentified("could not get info from decoder");
    check_size(info.canvas_width, info.canvas_height, limits);
    return Size{static_cast<int>(info.canvas_width), static_cast<int>(info.canvas_height)};
}

Image read_webp(std::string_view bytes, const PngLimits& limits) {
    if (!webp_signature(bytes)) unidentified("not a WEBP file");
    WebPData data{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
    // (Pillow's _webp: RGBA when the features say alpha, else RGBX read as RGB)
    WebPBitstreamFeatures features{};
    const bool alpha = WebPGetFeatures(data.bytes, data.size, &features) == VP8_STATUS_OK && features.has_alpha;
    AnimDecoder anim;
    anim.decoder = WebPAnimDecoderNew(&data, nullptr);
    if (anim.decoder == nullptr) unidentified("could not create decoder object");
    WebPAnimInfo info{};
    if (!WebPAnimDecoderGetInfo(anim.decoder, &info)) unidentified("could not get info from decoder");
    check_size(info.canvas_width, info.canvas_height, limits);
    uint8_t* frame = nullptr;
    int timestamp = 0;
    if (!WebPAnimDecoderGetNext(anim.decoder, &frame, &timestamp) || frame == nullptr) undecodable("failed to decode next frame in WebP file");
    const Size size{static_cast<int>(info.canvas_width), static_cast<int>(info.canvas_height)};
    const std::string_view pixels(reinterpret_cast<const char*>(frame), static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height) * 4);
    return alpha ? Image::frombytes("RGBA", size, pixels, "RGBA") : Image::frombytes("RGB", size, pixels, "RGBX");
}

}  // namespace genko::render
