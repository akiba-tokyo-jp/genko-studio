// Native BMP input. Header fields and raw modes follow Pillow's BMP plugin;
// pixel unpacking uses the existing, Python-free libImaging backend.
#include "render/bmp.hpp"
#include <array>
#include <algorithm>
#include <limits>
#include "core/error.hpp"
#include "render/imaging.hpp"

namespace genko::render {
namespace {
[[noreturn]] void bad_bmp() { throw core::Error("unidentified_image", "cannot read BMP"); }
std::uint32_t le(std::string_view b, std::size_t at, std::size_t n = 4) {
    if (at > b.size() || n > b.size() - at) bad_bmp();
    std::uint32_t result = 0;
    for (std::size_t i = 0; i < n; ++i)
        result |= static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + i])) << (i * 8);
    return result;
}
struct Work {
    ImagingMemoryInstance reservation{};
    ~Work() { genko_imaging_budget_release(&reservation); }
};
struct MaskMode {
    std::array<std::uint32_t, 4> masks;
    std::string_view raw;
};
// Pillow's pull decoder writes a flat index stream: EOL aligns it to width, delta
// inserts zero indices, and RLE4 absolute packets read count/2 packed bytes.
// Decode directly into the budgeted output; never grow a second full-image buffer.
void decode_rle(Image& image, std::string_view bytes, std::size_t pos, bool four, bool top_down) {
    const auto width = static_cast<std::uint64_t>(image.width());
    const auto pixels = width * static_cast<std::uint64_t>(image.height());
    std::uint64_t out = 0, x = 0;
    const auto emit = [&](unsigned char value) {
        if (out >= pixels) return;
        const auto row = static_cast<int>(out / width);
        const int dest = top_down ? row : image.height() - row - 1;
        reinterpret_cast<UINT8*>(image.raw()->image[dest])[out % width] = value;
        ++out;
    };
    while (out < pixels) {
        if (pos > bytes.size() || bytes.size() - pos < 2) throw core::Error("value", "not enough image data");
        const auto count = static_cast<unsigned char>(bytes[pos++]);
        const auto value = static_cast<unsigned char>(bytes[pos++]);
        if (count != 0) {
            const auto n = std::min<std::uint64_t>(count, width - std::min(width, x));
            for (std::uint64_t i = 0; i < n; ++i)
                emit(four ? static_cast<unsigned char>(i % 2 == 0 ? value >> 4 : value & 15) : value);
            x += n;
        } else if (value == 0) {
            const auto remainder = out % width;
            if (remainder != 0) out += std::min(width - remainder, pixels - out);
            x = 0;
        } else if (value == 1) {
            break;
        } else if (value == 2) {
            if (pos > bytes.size() || bytes.size() - pos < 2) throw core::Error("value", "not enough image data");
            const auto right = static_cast<unsigned char>(bytes[pos++]);
            const auto up = static_cast<unsigned char>(bytes[pos++]);
            out += std::min<std::uint64_t>(right + static_cast<std::uint64_t>(up) * width, pixels - out);
            x = out % width;
        } else {
            const std::size_t n = four ? value / 2U : value;
            if (pos > bytes.size()) throw core::Error("value", "not enough image data");
            const auto available = std::min(n, bytes.size() - pos);
            for (std::size_t i = 0; i < available; ++i) {
                const auto packed = static_cast<unsigned char>(bytes[pos++]);
                if (four) { emit(static_cast<unsigned char>(packed >> 4)); emit(static_cast<unsigned char>(packed & 15)); }
                else emit(packed);
            }
            // Pillow appends actual bytes before its short-read break. A complete
            // image must succeed even when the packet promises unused extra bytes.
            if (available < n) break;
            x += value;
            if (pos % 2 != 0) ++pos;  // Missing final alignment padding is not missing pixels.
        }
    }
    if (out != pixels) throw core::Error("value", "not enough image data");
}
constexpr std::array<MaskMode, 8> k32{{
    {{{0xff0000, 0xff00, 0xff, 0}}, "BGRX"},
    {{{0xff000000, 0xff0000, 0xff00, 0}}, "XBGR"},
    {{{0xff000000, 0xff00, 0xff, 0}}, "BGXR"},
    {{{0xff000000, 0xff0000, 0xff00, 0xff}}, "ABGR"},
    {{{0xff, 0xff00, 0xff0000, 0xff000000}}, "RGBA"},
    {{{0xff0000, 0xff00, 0xff, 0xff000000}}, "BGRA"},
    {{{0xff000000, 0xff00, 0xff, 0xff0000}}, "BGAR"},
    {{{0, 0, 0, 0}}, "BGRA"},
}};
}  // namespace

Image read_bmp(std::string_view bytes, const PngLimits& limits) {
    if (bytes.substr(0, 2) != "BM" || bytes.size() < 18) bad_bmp();
    const auto header = le(bytes, 14);
    if (header != 12 && header != 40 && header != 52 && header != 56 && header != 64 && header != 108 && header != 124)
        throw core::Error("format", "Unsupported BMP header type (" + std::to_string(header) + ")");
    if (static_cast<std::size_t>(header) > bytes.size() - 14)
        throw core::Error("format", "Truncated File Read");
    std::size_t metadata_end = 14 + static_cast<std::size_t>(header);
    const auto width = le(bytes, 18, header == 12 ? 2 : 4);
    const auto encoded_height = le(bytes, header == 12 ? 20 : 22, header == 12 ? 2 : 4);
    const bool top_down = header != 12 && (encoded_height & 0x80000000U) != 0;
    const std::uint64_t height = top_down ? (1ull << 32) - encoded_height : encoded_height;
    if (width == 0 || width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || height == 0 ||
        height > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) bad_bmp();
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (static_cast<std::int64_t>(pixels) > limits.max_pixels) throw core::Error("image_too_large", "image has too many pixels");
    if (beyond_side(width, static_cast<std::int64_t>(height), limits)) throw core::Error("image_too_large", "image is too wide or too tall");
    if (le(bytes, header == 12 ? 22 : 26, 2) != 1) bad_bmp();
    const auto bits = le(bytes, header == 12 ? 24 : 28, 2);
    const auto compression = header == 12 ? 0 : le(bytes, 30);
    const bool rle = (compression == 1 && bits == 8) || (compression == 2 && bits == 4);
    std::string_view mode, raw;
    switch (bits) {
    case 1: mode = "P"; raw = "P;1"; break;
    case 4: mode = "P"; raw = "P;4"; break;
    case 8: mode = raw = "P"; break;
    case 16: mode = "RGB"; raw = "BGR;15"; break;
    case 24: mode = "RGB"; raw = "BGR"; break;
    case 32: mode = "RGB"; raw = "BGRX"; break;
    default: throw core::Error("format", "Unsupported BMP pixel depth (" + std::to_string(bits) + ")");
    }
    if (compression == 3) {
        if (bits < 16) throw core::Error("format", "Unsupported BMP bitfields layout");
        std::array<std::uint32_t, 4> masks{};
        if (header >= 52) {
            for (std::size_t i = 0; i < (header >= 56 ? 4U : 3U); ++i) masks[i] = le(bytes, 54 + i * 4);
        } else {
            for (std::size_t i = 0; i < 3; ++i) masks[i] = le(bytes, metadata_end + i * 4);
            metadata_end += 12;
        }
        if (bits == 32) {
            bool found = false;
            for (const auto& candidate : k32) if (candidate.masks == masks) { raw = candidate.raw; found = true; break; }
            if (!found) throw core::Error("format", "Unsupported BMP bitfields layout");
            if (raw.find('A') != std::string_view::npos) mode = "RGBA";
        } else if (bits == 16) {
            if (masks[0] == 0xf800 && masks[1] == 0x7e0 && masks[2] == 0x1f) raw = "BGR;16";
            else if (masks[0] != 0x7c00 || masks[1] != 0x3e0 || masks[2] != 0x1f) throw core::Error("format", "Unsupported BMP bitfields layout");
        } else if (masks[0] != 0xff0000 || masks[1] != 0xff00 || masks[2] != 0xff) throw core::Error("format", "Unsupported BMP bitfields layout");
    } else if (compression != 0 && !rle) {
        throw core::Error("format", "Unsupported BMP compression (" + std::to_string(compression) + ")");
    }
    std::string_view palette;
    const std::size_t palette_stride = header == 12 ? 3 : 4;
    if (bits <= 8) {
        auto colors = header == 12 ? 0 : le(bytes, 46);
        if (colors == 0) colors = 1U << bits;
        if (colors > 256) bad_bmp();
        const auto length = static_cast<std::size_t>(colors) * palette_stride;
        if (metadata_end > bytes.size() || length > bytes.size() - metadata_end) bad_bmp();
        palette = bytes.substr(metadata_end, length);
        metadata_end += length;
        bool gray = true;
        for (std::uint32_t i = 0; i < colors; ++i) {
            const auto value = colors == 2 ? i * 255 : i;
            for (std::size_t band = 0; band < 3; ++band)
                if (static_cast<unsigned char>(palette[static_cast<std::size_t>(i) * palette_stride + band]) != value)
                    gray = false;
        }
        if (gray) { mode = colors == 2 ? "1" : "L"; raw = mode; }
    }
    std::size_t offset = le(bytes, 10);
    if (offset == 0) offset = metadata_end;  // Pillow: offset or the post-header/palette cursor.
    // Pillow accepts an offset pointing to the start of the palette by advancing past it.
    if (bits <= 8 && offset == 14 + static_cast<std::size_t>(header)) offset = metadata_end;
    if (offset < metadata_end || offset > bytes.size()) bad_bmp();
    const std::uint64_t stride = ((static_cast<std::uint64_t>(width) * bits + 31) / 32) * 4;
    int unpack_bits = 0;
    const auto unpack = ImagingFindUnpacker(detail::mode_id(mode), detail::rawmode_id(raw), &unpack_bits);
    if (unpack == nullptr || unpack_bits <= 0) bad_bmp();
    const std::uint64_t row_bytes = (static_cast<std::uint64_t>(width) * static_cast<unsigned int>(unpack_bits) + 7) / 8;
    if ((!rle && row_bytes > stride) || (rle && mode != "P" && mode != "L")) bad_bmp();
    // The final row's alignment padding is optional; its actual pixels are not.
    const std::uint64_t required = (height - 1) * stride + row_bytes;
    if (!rle && required > bytes.size() - offset)
        throw core::Error("format", "image file is truncated (" + std::to_string((bytes.size() - offset) % stride) + " bytes not processed)");
    constexpr std::uint64_t kBudget = 1ull << 30;
    const std::uint64_t row_pointers = height * sizeof(void*);
    const std::uint64_t work_bytes = bytes.size() + row_pointers;
    const std::uint64_t pixel_bytes = pixels * (mode == "RGB" || mode == "RGBA" ? 4 : 1);
    if (work_bytes > kBudget || pixel_bytes > kBudget - work_bytes ||
        stride > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        throw core::Error("memory", "BMP decoding exceeds memory budget");
    ImageAllocationBudget budget(kBudget);
    Work work, rows;
    if (!genko_imaging_budget_reserve(&work.reservation, bytes.size()) ||
        !genko_imaging_budget_reserve(&rows.reservation, row_pointers)) detail::throw_imaging_error();
    auto image = Image::create_blank(mode, Size{static_cast<int>(width), static_cast<int>(height)});
    if (rle) {
        decode_rle(image, bytes, offset, compression == 2, top_down);
    } else for (int y = 0; y < static_cast<int>(height); ++y) {
        const int dest = top_down ? y : static_cast<int>(height) - y - 1;
        const auto at = offset + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        unpack(reinterpret_cast<UINT8*>(image.raw()->image[dest]),
               reinterpret_cast<const UINT8*>(bytes.data() + at), static_cast<int>(width));
    }
    if (mode == "P") {
        auto* im = image.raw();
        ImagingPaletteDelete(im->palette);
        im->palette = ImagingPaletteNew(IMAGING_MODE_RGB);
        if (im->palette == nullptr) detail::throw_imaging_error();
        int palette_bits = 0;
        const auto unpack_palette = ImagingFindUnpacker(IMAGING_MODE_RGB,
            detail::rawmode_id(palette_stride == 3 ? "BGR" : "BGRX"), &palette_bits);
        if (unpack_palette == nullptr) bad_bmp();
        im->palette->size = static_cast<int>(palette.size() / palette_stride);
        unpack_palette(im->palette->palette, reinterpret_cast<const UINT8*>(palette.data()), im->palette->size);
    }
    if (!genko_imaging_budget_transfer(&rows.reservation, image.raw())) detail::throw_imaging_error();
    return image;
}
}  // namespace genko::render
