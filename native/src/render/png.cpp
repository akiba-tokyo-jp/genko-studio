// PNG reading and writing with libpng. libpng reports errors with longjmp: every setjmp below is in a function that
// owns no C++ object created after it, and the callbacks keep no C++ objects alive when they call png_error.

#include "render/png.hpp"

#include <png.h>

// MSVC warns (C4611) on every setjmp in a C++ file. Each setjmp here is in a function that owns no C++ object made
// after it (see above), so the longjmp from libpng skips no destructor.
#if defined(_MSC_VER)
#pragma warning(disable : 4611)
#endif

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "render/imaging.hpp"
#include "render/not_yet_ported.hpp"

namespace genko::render {

namespace {

// --- reading ------------------------------------------------------------------------------------------------------

struct ReadState {
    const unsigned char* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
    char message[200] = {};
};

void on_error(png_structp png, png_const_charp message) {
    auto* state = static_cast<ReadState*>(png_get_error_ptr(png));
    if (state != nullptr) std::snprintf(state->message, sizeof(state->message), "%s", message != nullptr ? message : "");
    png_longjmp(png, 1);
}

void on_warning(png_structp, png_const_charp) {}

void on_read(png_structp png, png_bytep out, png_size_t n) {
    auto* state = static_cast<ReadState*>(png_get_io_ptr(png));
    if (n > state->size - state->pos) png_error(png, "image file is truncated");
    std::memcpy(out, state->data + state->pos, n);
    state->pos += n;
}

struct Header {
    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    int interlace = 0;
    std::size_t rowbytes = 0;
    int palette_size = 0;
    unsigned char palette[256 * 3] = {};
    bool has_trns = false;
    int trns_count = 0;
    unsigned char trns_alpha[256] = {};
    png_color_16 trns_color{};
};

struct ReadGuard {
    png_structp png = nullptr;
    png_infop info = nullptr;
    ~ReadGuard() {
        if (png != nullptr) png_destroy_read_struct(&png, info != nullptr ? &info : nullptr, nullptr);
    }
};

bool read_header(png_structp png, png_infop info, Header* h) {
    if (setjmp(png_jmpbuf(png))) return false;
    png_read_info(png, info);
    png_get_IHDR(png, info, &h->width, &h->height, &h->bit_depth, &h->color_type, &h->interlace, nullptr, nullptr);
    png_colorp palette = nullptr;
    int count = 0;
    if (png_get_PLTE(png, info, &palette, &count) != 0 && palette != nullptr) {
        h->palette_size = count > 256 ? 256 : count;
        for (int i = 0; i < h->palette_size; ++i) {
            h->palette[i * 3] = palette[i].red;
            h->palette[i * 3 + 1] = palette[i].green;
            h->palette[i * 3 + 2] = palette[i].blue;
        }
    }
    png_bytep alpha = nullptr;
    int trns = 0;
    png_color_16p colour = nullptr;
    if (png_get_tRNS(png, info, &alpha, &trns, &colour) != 0) {
        h->has_trns = true;
        h->trns_count = trns > 256 ? 256 : trns;
        if (alpha != nullptr) std::memcpy(h->trns_alpha, alpha, static_cast<std::size_t>(h->trns_count));
        if (colour != nullptr) h->trns_color = *colour;
    }
    return true;
}

bool prepare_rows(png_structp png, png_infop info, Header* h) {
    if (setjmp(png_jmpbuf(png))) return false;
    (void)png_set_interlace_handling(png);
    png_read_update_info(png, info);
    h->rowbytes = png_get_rowbytes(png, info);
    return true;
}

bool read_rows(png_structp png, png_bytepp rows) {
    if (setjmp(png_jmpbuf(png))) return false;
    png_read_image(png, rows);
    return true;
}

// Pillow reads the chunks up to the image data when it opens a file: a failure there means the file is not
// identified ("cannot identify image file"); a failure while decoding the pixels is a broken file.
[[noreturn]] void unidentified(const ReadState& state) {
    throw core::Error("unidentified_image", std::string("cannot identify image file (") + state.message + ")");
}

[[noreturn]] void broken(const ReadState& state) {
    throw core::Error("format", std::string("broken PNG file: ") + state.message);
}

// The first bytes of the other formats Pillow opens.
bool other_image_format(std::string_view b) {
    const auto starts = [&](std::string_view p) { return b.substr(0, p.size()) == p; };
    return starts("\xff\xd8\xff") || starts("GIF87a") || starts("GIF89a") || starts("BM") || starts("II*\0") ||
           starts(std::string_view("MM\0*", 4)) || (starts("RIFF") && b.size() >= 12 && b.substr(8, 4) == "WEBP") ||
           starts("8BPS");
}

// PngImagePlugin._MODES
bool mode_of(int bit_depth, int color_type, const char** mode, const char** rawmode) {
    struct Entry {
        int bits, type;
        const char* mode;
        const char* raw;
    };
    static constexpr Entry kModes[] = {
        {1, 0, "1", "1"},        {2, 0, "L", "L;2"},           {4, 0, "L", "L;4"},     {8, 0, "L", "L"},
        {16, 0, "I;16", "I;16B"}, {8, 2, "RGB", "RGB"},         {16, 2, "RGB", "RGB;16B"}, {1, 3, "P", "P;1"},
        {2, 3, "P", "P;2"},      {4, 3, "P", "P;4"},           {8, 3, "P", "P"},       {8, 4, "LA", "LA"},
        {16, 4, "RGBA", "LA;16B"}, {8, 6, "RGBA", "RGBA"},     {16, 6, "RGBA", "RGBA;16B"},
    };
    for (const Entry& e : kModes) {
        if (e.bits == bit_depth && e.type == color_type) {
            *mode = e.mode;
            *rawmode = e.raw;
            return true;
        }
    }
    return false;
}

// --- writing ------------------------------------------------------------------------------------------------------

struct WriteState {
    unsigned char* data = nullptr;
    std::size_t size = 0;
    std::size_t capacity = 0;
    char message[200] = {};
};

void on_write_error(png_structp png, png_const_charp message) {
    auto* state = static_cast<WriteState*>(png_get_error_ptr(png));
    if (state != nullptr) std::snprintf(state->message, sizeof(state->message), "%s", message != nullptr ? message : "");
    png_longjmp(png, 1);
}

void on_write(png_structp png, png_bytep data, png_size_t n) {
    auto* state = static_cast<WriteState*>(png_get_io_ptr(png));
    if (state->size + n > state->capacity) {
        std::size_t capacity = state->capacity == 0 ? 65536 : state->capacity;
        while (capacity < state->size + n) capacity *= 2;
        auto* grown = static_cast<unsigned char*>(std::realloc(state->data, capacity));
        if (grown == nullptr) png_error(png, "out of memory");
        state->data = grown;
        state->capacity = capacity;
    }
    std::memcpy(state->data + state->size, data, n);
    state->size += n;
}

void on_flush(png_structp) {}

struct WriteGuard {
    png_structp png = nullptr;
    png_infop info = nullptr;
    WriteState state;
    ~WriteGuard() {
        if (png != nullptr) png_destroy_write_struct(&png, info != nullptr ? &info : nullptr);
        std::free(state.data);
    }
};

struct WriteSpec {
    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bit_depth = 8;
    int color_type = PNG_COLOR_TYPE_GRAY;
    int level = 6;
    const png_color* palette = nullptr;
    int palette_size = 0;
};

bool write_all(png_structp png, png_infop info, const WriteSpec* spec, png_bytepp rows) {
    if (setjmp(png_jmpbuf(png))) return false;
    png_set_compression_level(png, spec->level);
    png_set_IHDR(png, info, spec->width, spec->height, spec->bit_depth, spec->color_type, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    if (spec->palette != nullptr) png_set_PLTE(png, info, spec->palette, spec->palette_size);
    png_write_info(png, info);
    png_write_image(png, rows);
    png_write_end(png, nullptr);
    return true;
}

}  // namespace

Image read_png(std::string_view bytes, const PngLimits& limits) {
    if (bytes.size() < 8 || png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes.data()), 0, 8) != 0) {
        throw core::Error("unidentified_image", "cannot identify image file (not a PNG file)");
    }
    ReadState state;
    state.data = reinterpret_cast<const unsigned char*>(bytes.data());
    state.size = bytes.size();
    ReadGuard guard;
    guard.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &state, on_error, on_warning);
    if (guard.png == nullptr) throw core::Error("memory", "cannot read PNG");
    guard.info = png_create_info_struct(guard.png);
    if (guard.info == nullptr) throw core::Error("memory", "cannot read PNG");
    png_set_read_fn(guard.png, &state, on_read);
    png_set_user_limits(guard.png, 0x7fffffffU, 0x7fffffffU);
    png_set_benign_errors(guard.png, 1);
#ifdef PNG_SKIP_sRGB_CHECK_PROFILE
    png_set_option(guard.png, PNG_SKIP_sRGB_CHECK_PROFILE, PNG_OPTION_ON);
#endif

    Header h;
    if (!read_header(guard.png, guard.info, &h)) unidentified(state);
    const char* mode = nullptr;
    const char* rawmode = nullptr;
    if (!mode_of(h.bit_depth, h.color_type, &mode, &rawmode)) {
        throw core::Error("unidentified_image", "cannot identify image file (PNG bit depth " + std::to_string(h.bit_depth) +
                                                    " for colour type " + std::to_string(h.color_type) + ")");
    }
    const std::int64_t pixels = static_cast<std::int64_t>(h.width) * static_cast<std::int64_t>(h.height);
    if (pixels > limits.max_pixels) {
        throw core::Error("image_too_large", "Image size (" + std::to_string(pixels) + " pixels) exceeds limit of " +
                                                 std::to_string(limits.max_pixels) +
                                                 " pixels, could be decompression bomb DOS attack.");
    }
    if (h.width > 0x7fffffffU / 4U || h.height > 0x7fffffffU) throw core::Error("image_too_large", "image too large");
    if (!prepare_rows(guard.png, guard.info, &h)) broken(state);

    std::vector<unsigned char> raw(h.rowbytes * static_cast<std::size_t>(h.height));
    std::vector<png_bytep> rows(static_cast<std::size_t>(h.height));
    for (png_uint_32 y = 0; y < h.height; ++y) rows[y] = raw.data() + h.rowbytes * y;
    if (!read_rows(guard.png, rows.data())) broken(state);

    const auto w = static_cast<int>(h.width);
    const auto ht = static_cast<int>(h.height);
    Image out = Image::create_blank(mode, Size{w, ht});
    ImagingMemoryInstance* im = out.raw();
    int bits = 0;
    const ImagingShuffler unpack = ImagingFindUnpacker(im->mode, detail::rawmode_id(rawmode), &bits);
    if (unpack == nullptr) throw core::Error("format", std::string("no unpacker for ") + rawmode);
    for (int y = 0; y < ht; ++y) {
        unpack(reinterpret_cast<UINT8*>(im->image[y]), raw.data() + h.rowbytes * static_cast<std::size_t>(y), w);
    }

    const std::string_view m = mode;
    if (m == "P") {
        // ImagePalette.raw("RGB", PLTE) realized at load: putpalette("RGB", "RGB", data)
        if (h.palette_size > 0) {
            ImagingPaletteDelete(im->palette);
            im->palette = ImagingPaletteNew(IMAGING_MODE_RGB);
            if (im->palette == nullptr) detail::throw_imaging_error();
            int pbits = 0;
            const ImagingShuffler punpack = ImagingFindUnpacker(IMAGING_MODE_RGB, IMAGING_RAWMODE_RGB, &pbits);
            im->palette->size = h.palette_size;
            punpack(im->palette->palette, h.palette, h.palette_size);
        }
    }
    if (h.has_trns) {
        Transparency t;
        if (m == "P") {
            // one fully transparent entry and the rest opaque: its index; otherwise one alpha per entry
            int zeros = 0;
            int zero_at = -1;
            bool simple = true;
            for (int i = 0; i < h.trns_count; ++i) {
                if (h.trns_alpha[i] == 0) {
                    ++zeros;
                    if (zero_at < 0) zero_at = i;
                } else if (h.trns_alpha[i] != 255) {
                    simple = false;
                }
            }
            if (simple && zeros == 1) {
                t.kind = Transparency::Kind::Index;
                t.value = zero_at;
            } else if (!(simple && zeros == 0)) {
                t.kind = Transparency::Kind::Bytes;
                t.bytes.assign(reinterpret_cast<const char*>(h.trns_alpha), static_cast<std::size_t>(h.trns_count));
            } else {
                // all opaque: _simple_palette matches only with one zero; otherwise the bytes are kept
                t.kind = Transparency::Kind::Bytes;
                t.bytes.assign(reinterpret_cast<const char*>(h.trns_alpha), static_cast<std::size_t>(h.trns_count));
            }
        } else if (m == "1") {
            t.kind = Transparency::Kind::Index;
            t.value = h.trns_color.gray != 0 ? 255 : 0;
        } else if (m == "L" || m == "I;16") {
            t.kind = Transparency::Kind::Index;
            t.value = h.trns_color.gray;
        } else if (m == "RGB") {
            t.kind = Transparency::Kind::Rgb;
            t.rgb = {h.trns_color.red, h.trns_color.green, h.trns_color.blue};
        }
        out.set_transparency(std::move(t));
    }
    return out;
}

Image open_image(std::string_view bytes, const PngLimits& limits) {
    if (bytes.size() >= 8 && png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes.data()), 0, 8) == 0) {
        return read_png(bytes, limits);
    }
    // Pillow would open these; this build reads PNG only so far
    if (other_image_format(bytes)) throw NotYetPorted("image_format");
    throw core::Error("unidentified_image", "cannot identify image file");
}

std::string write_png(const Image& image, int compress_level) {
    if (image.empty()) throw core::Error("value", "no image");
    const std::string_view mode = image.mode();
    WriteSpec spec;
    spec.width = static_cast<png_uint_32>(image.width());
    spec.height = static_cast<png_uint_32>(image.height());
    spec.level = compress_level < 0 ? 0 : compress_level > 9 ? 9 : compress_level;
    std::string_view rawmode;
    if (mode == "1") {
        spec.bit_depth = 1;
        spec.color_type = PNG_COLOR_TYPE_GRAY;
        rawmode = "1";
    } else if (mode == "L") {
        spec.color_type = PNG_COLOR_TYPE_GRAY;
        rawmode = "L";
    } else if (mode == "LA") {
        spec.color_type = PNG_COLOR_TYPE_GRAY_ALPHA;
        rawmode = "LA";
    } else if (mode == "I;16") {
        spec.bit_depth = 16;
        spec.color_type = PNG_COLOR_TYPE_GRAY;
        rawmode = "I;16B";
    } else if (mode == "RGB") {
        spec.color_type = PNG_COLOR_TYPE_RGB;
        rawmode = "RGB";
    } else if (mode == "RGBA") {
        spec.color_type = PNG_COLOR_TYPE_RGB_ALPHA;
        rawmode = "RGBA";
    } else if (mode == "P") {
        spec.color_type = PNG_COLOR_TYPE_PALETTE;
        rawmode = "P";
    } else {
        throw core::Error("value", "cannot write mode " + std::string(mode) + " as PNG");
    }
    if (spec.width == 0 || spec.height == 0) throw core::Error("value", "cannot write an empty image as PNG");

    ImagingMemoryInstance* im = image.raw();
    int bits = 0;
    const ImagingShuffler pack = ImagingFindPacker(im->mode, detail::rawmode_id(rawmode), &bits);
    if (pack == nullptr) throw core::Error("value", "no packer for " + std::string(mode));
    const std::size_t stride = (static_cast<std::size_t>(bits) * spec.width + 7) / 8;
    std::vector<unsigned char> raw(stride * spec.height);
    std::vector<png_bytep> rows(spec.height);
    for (png_uint_32 y = 0; y < spec.height; ++y) {
        rows[y] = raw.data() + stride * y;
        pack(rows[y], reinterpret_cast<const UINT8*>(im->image[y]), static_cast<int>(spec.width));
    }
    std::array<png_color, 256> palette{};
    if (mode == "P") {
        const int n = im->palette != nullptr ? (im->palette->size > 0 ? im->palette->size : 256) : 0;
        if (n == 0) throw core::Error("value", "image has no palette");
        for (int i = 0; i < n && i < 256; ++i) {
            palette[static_cast<std::size_t>(i)] = png_color{im->palette->palette[i * 4], im->palette->palette[i * 4 + 1],
                                                             im->palette->palette[i * 4 + 2]};
        }
        spec.palette = palette.data();
        spec.palette_size = n > 256 ? 256 : n;
    }

    WriteGuard guard;
    guard.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &guard.state, on_write_error, on_warning);
    if (guard.png == nullptr) throw core::Error("memory", "cannot write PNG");
    guard.info = png_create_info_struct(guard.png);
    if (guard.info == nullptr) throw core::Error("memory", "cannot write PNG");
    png_set_write_fn(guard.png, &guard.state, on_write, on_flush);
    if (!write_all(guard.png, guard.info, &spec, rows.data())) {
        throw core::Error("io", std::string("cannot write PNG: ") + guard.state.message);
    }
    return std::string(reinterpret_cast<const char*>(guard.state.data), guard.state.size);
}

void save_png(const Image& image, const std::filesystem::path& path, int compress_level) {
    const std::string bytes = write_png(image, compress_level);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw core::Error("io", "cannot write " + core::path_to_utf8(path));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) throw core::Error("io", "cannot write " + core::path_to_utf8(path));
}

}  // namespace genko::render
