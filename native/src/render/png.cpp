// PNG reading and writing with libpng. libpng reports errors with longjmp: every setjmp below is in a function that
// owns no C++ object created after it, and the callbacks keep no C++ objects alive when they call png_error.

#include "render/png.hpp"
#include "render/bmp.hpp"
#include "render/gif.hpp"

#include <png.h>

// MSVC warns (C4611) on every setjmp in a C++ file. Each setjmp here is in a function that owns no C++ object made
// after it (see above), so the longjmp from libpng skips no destructor.
#if defined(_MSC_VER)
#pragma warning(disable : 4611)
#endif

#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#include <jpeglib.h>
#include <jerror.h>

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

// --- JPEG reading -------------------------------------------------------------------------------------------------

// The error manager is the first member, as required by libjpeg's callback extension convention.
struct JpegError {
    jpeg_error_mgr base{};
    // Keep the platform-aligned jump buffer outside this codec callback structure.
    // Windows jmp_buf requires 16-byte alignment; embedding it adds implicit padding (C4324).
    std::jmp_buf* jump = nullptr;
    char message[JMSG_LENGTH_MAX]{};
};

void jpeg_error_exit(j_common_ptr codec) {
    auto* error = reinterpret_cast<JpegError*>(codec->err);
    codec->err->format_message(codec, error->message);
    std::longjmp(*error->jump, 1);
}

void jpeg_emit_message(j_common_ptr codec, int level) {
    // jpeg_mem_src otherwise substitutes a synthetic EOI on truncated input. Pillow's non-truncated load rejects it.
    if (level < 0 && codec->err->msg_code == JWRN_JPEG_EOF) jpeg_error_exit(codec);
}

struct JpegGuard {
    jpeg_decompress_struct codec{};
    JpegError error{};
    // Only budget bookkeeping fields are used; the token owns no pixels.
    ImagingMemoryInstance reservation{};
    ~JpegGuard() {
        // jpeg_create_decompress may fail partway through initialization, after assigning its memory manager.
        if (codec.mem != nullptr) jpeg_destroy_decompress(&codec);
        genko_imaging_budget_release(&reservation);
    }
};

// Each jump target lives in a helper without C++ owners. Mutable codec/error state belongs to its caller;
// no automatic local modified after setjmp is read after the jump.
bool jpeg_header(JpegGuard* state, const unsigned char* bytes, unsigned long size) {
    if (setjmp(*state->error.jump)) return false;
    jpeg_create_decompress(&state->codec);
    jpeg_mem_src(&state->codec, bytes, size);
    return jpeg_read_header(&state->codec, TRUE) == JPEG_HEADER_OK;
}

bool jpeg_start(JpegGuard* state) {
    if (setjmp(*state->error.jump)) return false;
    return jpeg_start_decompress(&state->codec) != FALSE;
}

bool jpeg_rows(JpegGuard* state, ImagingMemoryInstance* image, ImagingShuffler unpack,
               unsigned char* row) {
    if (setjmp(*state->error.jump)) return false;
    while (state->codec.output_scanline < state->codec.output_height) {
        const JDIMENSION y = state->codec.output_scanline;
        JSAMPROW pointer = row;
        if (jpeg_read_scanlines(&state->codec, &pointer, 1) != 1) return false;
        unpack(reinterpret_cast<UINT8*>(image->image[y]), row, static_cast<int>(state->codec.output_width));
    }
    return jpeg_finish_decompress(&state->codec) != FALSE;
}

[[noreturn]] void jpeg_failed(const JpegGuard& state, bool in_header = false) {
    if (state.error.base.msg_code == JERR_BAD_PRECISION)
        throw core::Error("unidentified_image", "unsupported JPEG precision");
    if (in_header && state.error.base.msg_code == JWRN_JPEG_EOF && state.codec.image_width != 0)
        throw core::Error("format", "Truncated File Read");
    const char* code = state.error.base.msg_code == JERR_OUT_OF_MEMORY ? "memory" :
                       state.codec.image_width != 0 || state.codec.unread_marker != 0 ? "format" : "unidentified_image";
    throw core::Error(code, std::string("cannot read JPEG: ") + state.error.message);
}

Image read_jpeg(std::string_view bytes, const PngLimits& limits) {
    if (bytes.size() > std::numeric_limits<unsigned long>::max())
        throw core::Error("image_too_large", "JPEG input too large");
    std::jmp_buf jump{};
    JpegGuard state;
    state.error.jump = &jump;
    state.codec.err = jpeg_std_error(&state.error.base);
    state.error.base.error_exit = jpeg_error_exit;
    state.error.base.emit_message = jpeg_emit_message;
    if (!jpeg_header(&state, reinterpret_cast<const unsigned char*>(bytes.data()),
                     static_cast<unsigned long>(bytes.size()))) jpeg_failed(state, true);
    const std::int64_t pixels = static_cast<std::int64_t>(state.codec.image_width) * state.codec.image_height;
    if (pixels > limits.max_pixels || state.codec.image_width > 0x7fffffffU / 4U ||
        state.codec.image_height > 0x7fffffffU || beyond_side(state.codec.image_width, state.codec.image_height, limits))
        throw core::Error("image_too_large", "JPEG image exceeds pixel/dimension limit");
    const char* mode = nullptr;
    const char* rawmode = nullptr;
    if (state.codec.data_precision != 8) throw core::Error("unidentified_image", "unsupported JPEG precision");
    if (state.codec.num_components == 1) {
        mode = rawmode = "L";
        state.codec.out_color_space = JCS_GRAYSCALE;
    } else if (state.codec.num_components == 3) {
        mode = rawmode = "RGB";
        state.codec.out_color_space = JCS_RGB;
    } else if (state.codec.num_components == 4) {
        mode = "CMYK";
        rawmode = "CMYK;I";  // Pillow's JpegImagePlugin inverts all four decoded components.
        state.codec.out_color_space = JCS_CMYK;
    } else {
        throw core::Error("unidentified_image", "unsupported JPEG component count");
    }
    // libjpeg allocates padded full-image DCT arrays before reading progressive/multiscan entropy.
    // Bound and charge that memory, the input and working space before start; its memory setting is advisory.
    constexpr std::uint64_t kDecodeBudget = 1ull << 30;
    std::uint64_t work = static_cast<std::uint64_t>(bytes.size()) + (64ull << 20);
    if (jpeg_has_multiple_scans(&state.codec)) {
        for (int i = 0; i < state.codec.num_components; ++i) {
            const jpeg_component_info& component = state.codec.comp_info[i];
            const auto h = static_cast<std::uint64_t>(component.h_samp_factor);
            const auto v = static_cast<std::uint64_t>(component.v_samp_factor);
            const std::uint64_t columns = (component.width_in_blocks + h - 1) / h * h;
            const std::uint64_t rows = (component.height_in_blocks + v - 1) / v * v;
            work += columns * rows * sizeof(JBLOCK);
        }
    }
    const std::uint64_t output = static_cast<std::uint64_t>(pixels) * (state.codec.num_components == 1 ? 1 : 4) +
                                 static_cast<std::uint64_t>(state.codec.image_height) * sizeof(void*);
    if (work > kDecodeBudget || output > kDecodeBudget - work)
        throw core::Error("image_too_large", "JPEG decoding exceeds memory budget");
    ImageAllocationBudget budget(kDecodeBudget);  // Nested callers retain their existing, possibly stricter budget.
    if (!genko_imaging_budget_reserve(&state.reservation, work)) detail::throw_imaging_error();
    Image image = Image::create_blank(mode, Size{static_cast<int>(state.codec.image_width),
                                                static_cast<int>(state.codec.image_height)});
    if (!jpeg_start(&state)) jpeg_failed(state);
    if (state.codec.output_width != state.codec.image_width || state.codec.output_height != state.codec.image_height ||
        state.codec.output_components != state.codec.num_components)
        throw core::Error("format", "unexpected JPEG output dimensions/components");
    const std::size_t stride = static_cast<std::size_t>(state.codec.output_width) *
                               static_cast<std::size_t>(state.codec.output_components);
    std::vector<unsigned char> row(stride);
    int bits = 0;
    const ImagingShuffler unpack = ImagingFindUnpacker(image.raw()->mode, detail::rawmode_id(rawmode), &bits);
    if (unpack == nullptr) throw core::Error("format", "no JPEG pixel unpacker");
    if (!jpeg_rows(&state, image.raw(), unpack, row.data())) jpeg_failed(state);
    return image;
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
    if (limits.max_side > 0 && bytes.size() >= 24 && bytes.substr(12, 4) == "IHDR") {
        // (a side beyond max_side is refused from the IHDR chunk itself, before libpng reads any further)
        const auto be32 = [&](std::size_t at) {
            std::uint32_t v = 0;
            for (std::size_t i = 0; i < 4; ++i) v = (v << 8) | static_cast<unsigned char>(bytes[at + i]);
            return v;
        };
        if (beyond_side(be32(16), be32(20), limits)) {
            throw core::Error("image_too_large", "image is wider or taller than " + std::to_string(limits.max_side) + " pixels");
        }
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
    if (beyond_side(h.width, h.height, limits)) {
        throw core::Error("image_too_large", "image is wider or taller than " + std::to_string(limits.max_side) + " pixels");
    }
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

namespace {

bool png_signature(std::string_view bytes) {
    return bytes.size() >= 8 && png_sig_cmp(reinterpret_cast<png_const_bytep>(bytes.data()), 0, 8) == 0;
}

}  // namespace

bool unported_image_format(std::string_view bytes) {
    if (png_signature(bytes) || bytes.substr(0, 3) == "\xff\xd8\xff" || bytes.substr(0, 2) == "BM" ||
        bytes.substr(0, 6) == "GIF87a" || bytes.substr(0, 6) == "GIF89a") {
        return false;
    }
    return other_image_format(bytes);  // (Pillow would open these; the remaining formats are not ported yet)
}

Image open_image(std::string_view bytes, const PngLimits& limits) {
    if (png_signature(bytes)) return read_png(bytes, limits);
    if (bytes.substr(0, 3) == "\xff\xd8\xff") return read_jpeg(bytes, limits);
    if (bytes.substr(0, 2) == "BM") return read_bmp(bytes, limits);
    if (bytes.substr(0, 6) == "GIF87a" || bytes.substr(0, 6) == "GIF89a") return read_gif(bytes, limits);
    if (unported_image_format(bytes)) throw NotYetPorted("image_format");
    throw core::Error("unidentified_image", "cannot identify image file");
}

// --- JPEG writing ------------------------------------------------------------------------------------------------

namespace {

struct JpegWriteGuard {
    jpeg_compress_struct codec{};
    JpegError error{};
    unsigned char* buffer = nullptr;
    unsigned long size = 0;
    ~JpegWriteGuard() {
        if (codec.mem != nullptr) jpeg_destroy_compress(&codec);
        std::free(buffer);
    }
};

// (the jump target in a helper without C++ owners, as jpeg_header's)
bool jpeg_encode(JpegWriteGuard* state, const unsigned char* pixels, int width, int height, int quality) {
    std::jmp_buf jump;
    state->error.jump = &jump;
    state->codec.err = jpeg_std_error(&state->error.base);
    state->error.base.error_exit = jpeg_error_exit;
    if (setjmp(jump)) return false;
    jpeg_create_compress(&state->codec);
    jpeg_mem_dest(&state->codec, &state->buffer, &state->size);
    state->codec.image_width = static_cast<JDIMENSION>(width);
    state->codec.image_height = static_cast<JDIMENSION>(height);
    state->codec.input_components = 3;
    state->codec.in_color_space = JCS_RGB;
    jpeg_set_defaults(&state->codec);
    jpeg_set_quality(&state->codec, quality, TRUE);
    jpeg_start_compress(&state->codec, TRUE);
    while (state->codec.next_scanline < state->codec.image_height) {
        JSAMPROW row = const_cast<unsigned char*>(pixels + static_cast<std::size_t>(state->codec.next_scanline) * static_cast<std::size_t>(width) * 3);
        jpeg_write_scanlines(&state->codec, &row, 1);
    }
    jpeg_finish_compress(&state->codec);
    return true;
}

}  // namespace

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

std::string write_jpeg(const Image& image, int quality) {
    if (image.empty()) throw core::Error("value", "no image");
    const Image rgb = image.mode() == "RGB" ? image : image.convert("RGB");
    const std::string pixels = rgb.tobytes();
    JpegWriteGuard state;
    if (!jpeg_encode(&state, reinterpret_cast<const unsigned char*>(pixels.data()), rgb.width(), rgb.height(), quality)) {
        throw core::Error("format", std::string("cannot write JPEG: ") + state.error.message);
    }
    return std::string(reinterpret_cast<const char*>(state.buffer), state.size);
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
