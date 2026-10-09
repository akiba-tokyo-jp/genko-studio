#include "formats/pillow_save.hpp"

#include <zlib.h>

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include <jpeglib.h>
#include <tiffio.h>

#include "core/error.hpp"
#include "render/colour.hpp"

namespace genko::formats {

namespace {

using render::Image;

void put32(std::string& out, std::uint32_t v) {
    out += static_cast<char>((v >> 24) & 0xff);
    out += static_cast<char>((v >> 16) & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
    out += static_cast<char>(v & 0xff);
}

// PngImagePlugin.putchunk: length, type, data, the CRC of type and data.
void chunk(std::string& out, const char* cid, std::string_view data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    out.append(cid, 4);
    out.append(data);
    uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(cid), 4);
    if (!data.empty()) crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
    put32(out, static_cast<std::uint32_t>(crc));
}

// zlib.compress(data) (CPython's: level -1, the default window and memory, the default strategy).
std::string zlib_compress(std::string_view data, int level = Z_DEFAULT_COMPRESSION) {
    uLongf size = compressBound(static_cast<uLong>(data.size()));
    std::string out(size, '\0');
    if (compress2(reinterpret_cast<Bytef*>(out.data()), &size, reinterpret_cast<const Bytef*>(data.data()),
                  static_cast<uLong>(data.size()), level) != Z_OK) {
        throw core::Error("memory", "zlib cannot compress");
    }
    out.resize(size);
    return out;
}

// _OUTMODES: (bit depth, colour type) of the modes the exports write, and the bits a pixel takes in its raw mode.
struct PngMode {
    int bit_depth;
    int colour_type;
    int bits;
};

PngMode png_mode(std::string_view mode) {
    if (mode == "1") return {1, 0, 1};
    if (mode == "L") return {8, 0, 8};
    if (mode == "LA") return {8, 4, 16};
    if (mode == "RGB") return {8, 2, 24};
    if (mode == "RGBA") return {8, 6, 32};
    throw core::Error("io", "cannot write mode " + std::string(mode) + " as PNG");
}

}  // namespace

// --- PNG: PngImagePlugin._save, ImageFile._save and ZipEncode.c --------------------------------------------------------

struct PngWriter::Impl {
    render::Size size;
    std::string mode;
    PngMode layout{};
    std::string out;          // the file so far
    z_stream z{};
    bool z_open = false;
    int bytes = 0;            // a row's bytes (state->bytes)
    int bpp = 1;              // (state->bits + 7) / 8
    bool optimize = false;
    std::size_t bufsize = 0;  // ImageFile._save: max(MAXBLOCK, im.size[0] * 4)
    std::string buf;          // one encode(bufsize) call's output
    int y = 0;
    std::vector<unsigned char> buffer, previous, prior, up, average, paeth;

    ~Impl() {
        if (z_open) deflateEnd(&z);
    }

    void reset_out() {
        z.next_out = reinterpret_cast<Bytef*>(buf.data());
        z.avail_out = static_cast<uInt>(bufsize);
    }

    // One encode(bufsize) call's data written as an IDAT chunk (_idat.write), even when it is empty.
    void emit() {
        chunk(out, "IDAT", std::string_view(buf.data(), bufsize - z.avail_out));
        reset_out();
    }

    void deflate_or_throw(int flush, int& result) {
        result = deflate(&z, flush);
        if (result < 0 && result != Z_BUF_ERROR) throw core::Error("io", "encoder error " + std::to_string(result) + " when writing image file");
    }

    // The filter ZipEncode.c chooses for this row (the least total distance from zero), as the bytes deflated.
    const unsigned char* filtered() {
        const unsigned char* output = buffer.data();
        int sum = 0;
        for (int i = 1; i <= bytes; ++i) {
            const unsigned char v = buffer[i];
            sum += v < 128 ? v : 256 - v;
        }
        if (sum > 0) {  // 2. Up
            int s = 0;
            for (int i = 1; i <= bytes; ++i) {
                const unsigned char v = static_cast<unsigned char>(buffer[i] - previous[i]);
                up[i] = v;
                s += v < 128 ? v : 256 - v;
            }
            if (s < sum) {
                output = up.data();
                sum = s;
            }
        }
        if (sum > 0) {  // 1. Prior (Sub)
            int s = 0;
            int i = 1;
            for (; i <= bpp && i <= bytes; ++i) {
                const unsigned char v = buffer[i];
                prior[i] = v;
                s += v < 128 ? v : 256 - v;
            }
            for (; i <= bytes; ++i) {
                const unsigned char v = static_cast<unsigned char>(buffer[i] - buffer[i - bpp]);
                prior[i] = v;
                s += v < 128 ? v : 256 - v;
            }
            if (s < sum) {
                output = prior.data();
                sum = s;
            }
        }
        if (optimize && sum > 0) {  // 3. Average (with optimize only)
            int s = 0;
            int i = 1;
            for (; i <= bpp && i <= bytes; ++i) {
                const unsigned char v = static_cast<unsigned char>(buffer[i] - previous[i] / 2);
                average[i] = v;
                s += v < 128 ? v : 256 - v;
            }
            for (; i <= bytes; ++i) {
                const unsigned char v = static_cast<unsigned char>(buffer[i] - (buffer[i - bpp] + previous[i]) / 2);
                average[i] = v;
                s += v < 128 ? v : 256 - v;
            }
            if (s < sum) {
                output = average.data();
                sum = s;
            }
        }
        if (sum > 0) {  // 4. Paeth
            int s = 0;
            int i = 1;
            for (; i <= bpp && i <= bytes; ++i) {
                const unsigned char v = static_cast<unsigned char>(buffer[i] - previous[i]);
                paeth[i] = v;
                s += v < 128 ? v : 256 - v;
            }
            for (; i <= bytes; ++i) {
                const int a = buffer[i - bpp];
                const int b = previous[i];
                const int c = previous[i - bpp];
                const int pa = std::abs(b - c);
                const int pb = std::abs(a - c);
                const int pc = std::abs(a + b - 2 * c);
                const unsigned char v = static_cast<unsigned char>(buffer[i] - ((pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c));
                paeth[i] = v;
                s += v < 128 ? v : 256 - v;
            }
            if (s < sum) output = paeth.data();
        }
        return output;
    }

    // One row (its raw bytes, packed as the mode's raw mode) into the stream, as ZipEncode's loop takes it; a full
    // output buffer is an IDAT chunk, and the next call first deflates what is left of the row.
    void row(const unsigned char* raw) {
        std::memcpy(buffer.data() + 1, raw, static_cast<std::size_t>(bytes));
        const unsigned char* output = filtered();
        z.next_in = const_cast<Bytef*>(output);
        z.avail_in = static_cast<uInt>(bytes + 1);
        int result = Z_OK;
        deflate_or_throw(Z_NO_FLUSH, result);
        std::swap(buffer, previous);  // (the deflated row stays where it is until it is all taken)
        while (z.avail_out == 0) {
            emit();
            if (z.avail_in == 0) break;
            deflate_or_throw(Z_NO_FLUSH, result);
        }
        ++y;
    }

    void finish() {
        for (;;) {
            int result = Z_OK;
            deflate_or_throw(Z_FINISH, result);
            if (result == Z_STREAM_END) {
                emit();
                break;
            }
            if (z.avail_out == 0) emit();
        }
        deflateEnd(&z);
        z_open = false;
    }
};

PngWriter::PngWriter(render::Size size, std::string_view mode, const PngSave& params) : impl_(std::make_unique<Impl>()) {
    Impl& s = *impl_;
    s.size = size;
    s.mode = std::string(mode);
    s.layout = png_mode(mode);
    if (size.width <= 0 || size.height <= 0) throw core::Error("value", "cannot write an empty image as PNG");
    // (a row's bytes are counted in an int: a wider picture is refused before anything is made for it)
    if ((static_cast<std::int64_t>(s.layout.bits) * size.width + 7) / 8 >= std::numeric_limits<int>::max()) {
        throw core::Error("image_too_large", "image too large to write as PNG");
    }
    s.out = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    put32(ihdr, static_cast<std::uint32_t>(size.width));
    put32(ihdr, static_cast<std::uint32_t>(size.height));
    ihdr += static_cast<char>(s.layout.bit_depth);
    ihdr += static_cast<char>(s.layout.colour_type);
    ihdr.append(3, '\0');
    chunk(s.out, "IHDR", ihdr);
    if (!params.icc.empty()) chunk(s.out, "iCCP", std::string("ICC Profile", 11) + std::string(2, '\0') + zlib_compress(params.icc));
    if (params.dpi) {
        std::string phys;
        put32(phys, static_cast<std::uint32_t>(static_cast<std::int64_t>((*params.dpi)[0] / 0.0254 + 0.5)));
        put32(phys, static_cast<std::uint32_t>(static_cast<std::int64_t>((*params.dpi)[1] / 0.0254 + 0.5)));
        phys += '\x01';
        chunk(s.out, "pHYs", phys);
    }
    s.optimize = params.optimize;
    s.bytes = static_cast<int>((static_cast<std::int64_t>(s.layout.bits) * size.width + 7) / 8);
    s.bpp = (s.layout.bits + 7) / 8;
    s.bufsize = static_cast<std::size_t>(std::max<std::int64_t>(65536, static_cast<std::int64_t>(size.width) * 4));
    s.buf.assign(s.bufsize, '\0');
    const std::size_t n = static_cast<std::size_t>(s.bytes) + 1;
    s.buffer.assign(n, 0);
    s.previous.assign(n, 0);  // (the row before the first is black)
    s.prior.assign(n, 1);
    s.up.assign(n, 2);
    s.average.assign(n, 3);
    s.paeth.assign(n, 4);
    s.buffer[0] = 0;
    s.previous[0] = 0;
    // optimize: Z_BEST_COMPRESSION; the strategy Z_FILTERED (the data are filtered); window 15, memory level 9
    if (deflateInit2(&s.z, params.optimize ? Z_BEST_COMPRESSION : Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 9, Z_FILTERED) != Z_OK) {
        throw core::Error("io", "encoder error when writing image file");
    }
    s.z_open = true;
    s.reset_out();
}

PngWriter::~PngWriter() = default;

void PngWriter::add(const render::Image& rows) {
    Impl& s = *impl_;
    if (rows.mode() != s.mode || rows.width() != s.size.width) throw core::Error("value", "the rows do not match the picture");
    if (s.y + rows.height() > s.size.height) throw core::Error("value", "more rows than the picture has");
    const int band = std::max(1, (1 << 22) / std::max(1, s.bytes));  // (rows packed a few MB at a time)
    for (int top = 0; top < rows.height(); top += band) {
        const int bottom = std::min(rows.height(), top + band);
        const Image part = top == 0 && bottom == rows.height() ? rows : rows.crop(render::Box{0, top, rows.width(), bottom});
        const std::string raw = part.tobytes();
        for (int r = 0; r < bottom - top; ++r) {
            s.row(reinterpret_cast<const unsigned char*>(raw.data()) + static_cast<std::size_t>(r) * static_cast<std::size_t>(s.bytes));
        }
    }
}

std::string PngWriter::finish() {
    Impl& s = *impl_;
    if (s.y != s.size.height) throw core::Error("value", "the picture is missing rows");
    s.finish();
    chunk(s.out, "IEND", {});
    return std::move(s.out);
}

std::string png_bytes(const render::Image& image, const PngSave& params) {
    if (image.empty()) throw core::Error("value", "no image");
    PngWriter writer(image.size(), image.mode(), params);
    writer.add(image);
    return writer.finish();
}

// --- JPEG: JpegImagePlugin._save and JpegEncode.c ----------------------------------------------------------------------

namespace {

struct JpegFail {
    jpeg_error_mgr base{};
    std::jmp_buf* jump = nullptr;
    char message[JMSG_LENGTH_MAX] = {};
};

void jpeg_fail(j_common_ptr codec) {
    auto* fail = reinterpret_cast<JpegFail*>(codec->err);
    (*codec->err->format_message)(codec, fail->message);
    std::longjmp(*fail->jump, 1);
}

struct JpegState {
    jpeg_compress_struct codec{};
    JpegFail fail{};
    unsigned char* data = nullptr;
    unsigned long size = 0;
    bool created = false;
    ~JpegState() {
        if (created) jpeg_destroy_compress(&codec);
        std::free(data);
    }
};

// (the jump target in a function without C++ owners)
bool jpeg_run(JpegState* s, const unsigned char* pixels, int width, int height, int components, const JpegSave* params,
              const std::vector<std::string>* markers) {
    std::jmp_buf jump;
    s->fail.jump = &jump;
    s->codec.err = jpeg_std_error(&s->fail.base);
    s->fail.base.error_exit = jpeg_fail;
    if (setjmp(jump)) return false;
    jpeg_create_compress(&s->codec);
    s->created = true;
    jpeg_mem_dest(&s->codec, &s->data, &s->size);
    s->codec.image_width = static_cast<JDIMENSION>(width);
    s->codec.image_height = static_cast<JDIMENSION>(height);
    s->codec.input_components = components;
    s->codec.in_color_space = components == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&s->codec);
    if (params->quality != -1) jpeg_set_quality(&s->codec, params->quality, TRUE);
    s->codec.smoothing_factor = 0;
    s->codec.optimize_coding = params->optimize ? TRUE : FALSE;
    s->codec.restart_interval = 0;
    s->codec.restart_in_rows = 0;
    jpeg_start_compress(&s->codec, TRUE);
    for (const std::string& marker : *markers) {  // (the profile: Pillow's "extra", right after the JFIF header)
        jpeg_write_marker(&s->codec, JPEG_APP0 + 2, reinterpret_cast<const JOCTET*>(marker.data()), static_cast<unsigned>(marker.size()));
    }
    const std::size_t stride = static_cast<std::size_t>(width) * static_cast<std::size_t>(components);
    while (s->codec.next_scanline < s->codec.image_height) {
        JSAMPROW row = const_cast<unsigned char*>(pixels + static_cast<std::size_t>(s->codec.next_scanline) * stride);
        jpeg_write_scanlines(&s->codec, &row, 1);
    }
    jpeg_finish_compress(&s->codec);
    return true;
}

}  // namespace

std::string jpeg_bytes(const render::Image& image, const JpegSave& params) {
    if (image.empty()) throw core::Error("value", "no image");
    const std::string_view mode = image.mode();
    if (mode != "L" && mode != "RGB") throw core::Error("io", "cannot write mode " + std::string(mode) + " as JPEG");
    if (params.icc.size() > 255 * (65533 - 14)) throw core::PyValueError("ICC profile is too long");
    // the profile in APP2 markers of at most 65519 bytes of it each, numbered from 1 (made here: nothing with a destructor
    // lives where libjpeg may jump out)
    std::vector<std::string> markers;
    if (!params.icc.empty()) {
        constexpr std::size_t kMost = 65533 - 14;
        const std::size_t count = (params.icc.size() + kMost - 1) / kMost;
        for (std::size_t i = 0; i < count; ++i) {
            std::string marker = std::string("ICC_PROFILE", 11) + '\0';
            marker += static_cast<char>(i + 1);
            marker += static_cast<char>(count);
            marker += params.icc.substr(i * kMost, kMost);
            markers.push_back(std::move(marker));
        }
    }
    const std::string pixels = image.tobytes();
    JpegState state;
    if (!jpeg_run(&state, reinterpret_cast<const unsigned char*>(pixels.data()), image.width(), image.height(), mode == "L" ? 1 : 3, &params,
                  &markers)) {
        throw core::Error("io", std::string("encoder error -2 when writing image file (") + state.fail.message + ")");
    }
    return std::string(reinterpret_cast<const char*>(state.data), state.size);
}

// --- TIFF: TiffImagePlugin._save with libtiff (Pillow's libtiff encoder) -------------------------------------------

namespace {

struct TiffMemory {
    std::string data;
    toff_t pos = 0;
};

tsize_t tiff_read(thandle_t h, tdata_t buf, tsize_t size) {
    auto* m = static_cast<TiffMemory*>(h);
    if (m->pos >= m->data.size()) return 0;
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(size), m->data.size() - static_cast<std::size_t>(m->pos));
    std::memcpy(buf, m->data.data() + m->pos, n);
    m->pos += n;
    return static_cast<tsize_t>(n);
}

tsize_t tiff_write(thandle_t h, tdata_t buf, tsize_t size) {
    auto* m = static_cast<TiffMemory*>(h);
    const std::size_t end = static_cast<std::size_t>(m->pos) + static_cast<std::size_t>(size);
    if (end > m->data.size()) m->data.resize(end, '\0');
    std::memcpy(m->data.data() + m->pos, buf, static_cast<std::size_t>(size));
    m->pos = end;
    return size;
}

toff_t tiff_seek(thandle_t h, toff_t off, int whence) {
    auto* m = static_cast<TiffMemory*>(h);
    switch (whence) {
        case SEEK_SET: m->pos = off; break;
        case SEEK_CUR: m->pos += off; break;
        case SEEK_END: m->pos = m->data.size() + off; break;
        default: return static_cast<toff_t>(-1);
    }
    return m->pos;
}

int tiff_close(thandle_t) { return 0; }
toff_t tiff_size(thandle_t h) { return static_cast<TiffMemory*>(h)->data.size(); }
int tiff_map(thandle_t, tdata_t*, toff_t*) { return 0; }
void tiff_unmap(thandle_t, tdata_t, toff_t) {}

}  // namespace

std::string tiff_bytes(const render::Image& image, const TiffSave& params) {
    if (image.empty()) throw core::Error("value", "no image");
    const std::string_view mode = image.mode();
    int photometric = 0;
    int samples = 1;
    int bits = 8;
    if (mode == "1") {
        photometric = PHOTOMETRIC_MINISBLACK;
        bits = 1;
    } else if (mode == "L") {
        photometric = PHOTOMETRIC_MINISBLACK;
    } else if (mode == "RGB") {
        photometric = PHOTOMETRIC_RGB;
        samples = 3;
    } else if (mode == "CMYK") {
        photometric = PHOTOMETRIC_SEPARATED;
        samples = 4;
    } else {
        throw core::Error("io", "cannot write mode " + std::string(mode) + " as TIFF");
    }
    int compression = COMPRESSION_LZW;
    if (params.compression == "group4") {
        compression = COMPRESSION_CCITTFAX4;
    } else if (params.compression != "tiff_lzw") {
        throw core::Error("value", "unknown compression " + params.compression);
    }
    const std::int64_t w = image.width();
    const std::int64_t h = image.height();
    const std::int64_t stride = samples * ((w * bits + 7) / 8);
    std::int64_t rows_per_strip = stride == 0 ? 1 : std::min<std::int64_t>(65536 / stride, h);  // (STRIP_SIZE)
    if (rows_per_strip == 0) rows_per_strip = 1;

    TiffMemory memory;
    TIFF* tif = TIFFClientOpen("memory", "w", &memory, tiff_read, tiff_write, tiff_seek, tiff_close, tiff_size, tiff_map, tiff_unmap);
    if (tif == nullptr) throw core::Error("io", "encoder error -9 when writing image file");
    struct Closer {
        TIFF* t;
        ~Closer() { TIFFClose(t); }
    } closer{tif};
    // the tags in Pillow's order (sorted by number), as its encoder sets them
    bool ok = TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(w)) == 1;
    ok = ok && TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(h)) == 1;
    ok = ok && TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bits) == 1;
    ok = ok && TIFFSetField(tif, TIFFTAG_COMPRESSION, compression) == 1;
    ok = ok && TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, photometric) == 1;
    if (samples != 1) ok = ok && TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, samples) == 1;
    ok = ok && TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, static_cast<std::uint32_t>(rows_per_strip)) == 1;
    if (params.dpi) {
        ok = ok && TIFFSetField(tif, TIFFTAG_XRESOLUTION, (*params.dpi)[0]) == 1;
        ok = ok && TIFFSetField(tif, TIFFTAG_YRESOLUTION, (*params.dpi)[1]) == 1;
    }
    ok = ok && TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) == 1;
    if (params.dpi) ok = ok && TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH) == 1;
    if (!params.icc.empty()) {
        ok = ok && TIFFSetField(tif, TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(params.icc.size()), params.icc.data()) == 1;
    }
    if (!ok) throw core::Error("io", "encoder error -9 when writing image file");
    const int band = static_cast<int>(std::max<std::int64_t>(1, (1 << 22) / std::max<std::int64_t>(1, stride)));
    for (int top = 0; top < image.height(); top += band) {
        const int bottom = std::min(image.height(), top + band);
        const std::string raw = image.crop(render::Box{0, top, image.width(), bottom}).tobytes();
        for (int y = top; y < bottom; ++y) {
            void* row = const_cast<char*>(raw.data()) + static_cast<std::size_t>(y - top) * static_cast<std::size_t>(stride);
            if (TIFFWriteScanline(tif, row, static_cast<std::uint32_t>(y), 0) == -1) {
                throw core::Error("io", "encoder error -2 when writing image file");
            }
        }
    }
    if (!TIFFFlush(tif)) throw core::Error("io", "encoder error -9 when writing image file");
    return std::move(memory.data);
}

const std::string& srgb_icc_once() {
    static const std::string profile = render::colour::srgb_icc();
    return profile;
}

}  // namespace genko::formats
