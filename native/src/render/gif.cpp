#include "render/gif.hpp"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <limits>

#include "core/error.hpp"
#include "render/imaging.hpp"
extern "C" {
#include "Gif.h"
}

namespace genko::render {
namespace {
[[noreturn]] void bad_gif() { throw core::Error("unidentified_image", "cannot identify image file"); }
std::uint32_t byte_at(std::string_view bytes, std::size_t at) {
    if (at >= bytes.size()) bad_gif();
    return static_cast<unsigned char>(bytes[at]);
}
std::uint32_t u16(std::string_view bytes, std::size_t at) {
    return byte_at(bytes, at) | (byte_at(bytes, at + 1) << 8);
}
struct Reservation {
    ImagingMemoryInstance owner{};
    ~Reservation() { genko_imaging_budget_release(&owner); }
};
}  // namespace

Image read_gif(std::string_view bytes, PngLimits limits) {
    constexpr std::uint64_t kBudget = 1ull << 30;
    if (bytes.size() > kBudget) throw core::Error("image_too_large", "image is too large");
    if (bytes.size() < 13 || (bytes.substr(0, 6) != "GIF87a" && bytes.substr(0, 6) != "GIF89a")) bad_gif();
    auto width = u16(bytes, 6), height = u16(bytes, 8);
    auto pixels = static_cast<std::uint64_t>(width) * height;
    if (limits.max_pixels < 0 || pixels > static_cast<std::uint64_t>(limits.max_pixels))
        throw core::Error("image_too_large", "image is too large");
    const auto flags = byte_at(bytes, 10);
    std::size_t palette_size = (flags & 0x80U) ? (1U << ((flags & 7U) + 1U)) : 0;
    std::size_t palette_bytes = palette_size * 3;
    if (palette_bytes > bytes.size() - 13) bad_gif();
    auto palette = bytes.substr(13, palette_bytes);
    const auto global_palette = palette;
    std::size_t pos = 13 + palette_bytes;
    int transparent_index = -1;
    for (;;) {
        const auto marker = byte_at(bytes, pos);
        if (marker == 0x2cU) break;
        if (marker == 0x3bU) bad_gif();
        if (marker != 0x21U) { ++pos; continue; }  // Pillow skips padding before a frame.
        const auto label = byte_at(bytes, pos + 1);
        pos += 2;
        const auto block = [&]() -> std::string_view {
            const auto count = byte_at(bytes, pos++);
            if (count > bytes.size() - pos) bad_gif();
            const auto result = bytes.substr(pos, count);
            pos += count;
            return result;
        };
        const auto first = block();
        if (label == 0xf9U && !first.empty()) {
            const bool transparent = (byte_at(first, 0) & 1U) != 0;
            if (first.size() < 3 || (transparent && first.size() < 4)) bad_gif();
            if (transparent) transparent_index = static_cast<int>(byte_at(first, 3));
        }
        if (label == 0xfeU) {
            // Comments consume their initial block and stop at its zero terminator.
            auto next = first;
            while (!next.empty()) next = block();
        } else {
            // Pillow drains following blocks even when the initial extension block is empty.
            if (label == 0xffU && first.starts_with("NETSCAPE2.0")) (void)block();
            while (!block().empty()) {}
        }
    }
    if (byte_at(bytes, pos) != 0x2cU) bad_gif();
    if (pos > bytes.size() || bytes.size() - pos < 10) bad_gif();
    const auto xoff = u16(bytes, pos + 1), yoff = u16(bytes, pos + 3);
    const auto frame_width = u16(bytes, pos + 5), frame_height = u16(bytes, pos + 7);
    if (frame_width == 0 || frame_height == 0) bad_gif();
    width = std::max(width, xoff + frame_width);
    height = std::max(height, yoff + frame_height);
    pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels > static_cast<std::uint64_t>(limits.max_pixels))
        throw core::Error("image_too_large", "image is too large");
    const auto frame_flags = byte_at(bytes, pos + 9);
    const bool interlace = (frame_flags & 0x40U) != 0;
    pos += 10;
    if ((frame_flags & 0x80U) != 0) {
        palette_size = 1U << ((frame_flags & 7U) + 1U);
        palette_bytes = palette_size * 3;
        if (palette_bytes > bytes.size() - pos) bad_gif();
        palette = bytes.substr(pos, palette_bytes);
        pos += palette_bytes;
    }
    bool identity = true;
    for (std::size_t i = 0; i < palette_size; ++i)
        for (std::size_t band = 0; band < 3; ++band)
            if (byte_at(palette, i * 3 + band) != i) identity = false;
    bool global_identity = true;
    for (std::size_t i = 0; i < global_palette.size() / 3; ++i)
        for (std::size_t band = 0; band < 3; ++band)
            if (byte_at(global_palette, i * 3 + band) != i) global_identity = false;
    const bool logical_l_with_palette = (frame_flags & 0x80U) != 0 && identity && !global_identity;
    const auto display_palette = logical_l_with_palette ? global_palette : palette;
    const auto bits = byte_at(bytes, pos++);
    if (bits > 12) throw core::Error("format", "codec configuration error when reading image file");
    const std::uint64_t rows_size = static_cast<std::uint64_t>(height) * sizeof(void*);
    const std::uint64_t work_size = bytes.size() + sizeof(GIFDECODERSTATE) + sizeof(ImagingCodecStateInstance);
    if (work_size > kBudget || rows_size > kBudget - work_size || pixels > kBudget - work_size - rows_size ||
        bytes.size() - pos > static_cast<std::size_t>(std::numeric_limits<Py_ssize_t>::max()))
        throw core::Error("memory", "GIF decoding exceeds memory budget");
    ImageAllocationBudget budget(kBudget);
    Reservation work, rows;
    if (!genko_imaging_budget_reserve(&work.owner, work_size) ||
        !genko_imaging_budget_reserve(&rows.owner, rows_size)) detail::throw_imaging_error();
    auto image = Image::create_blank(identity && !logical_l_with_palette ? "L" : "P",
                                     Size{static_cast<int>(width), static_cast<int>(height)});
    if (transparent_index >= 0)
        for (std::uint32_t y = 0; y < height; ++y)
            std::memset(image.raw()->image8[y], transparent_index, width);
    if (!identity || logical_l_with_palette) {
        if (image.raw()->palette == nullptr) detail::throw_imaging_error("out of memory");
        int palette_bits = 0;
        const auto unpack = ImagingFindUnpacker(IMAGING_MODE_RGB, detail::rawmode_id("RGB"), &palette_bits);
        if (unpack == nullptr) bad_gif();
        const auto display_palette_size = static_cast<int>(display_palette.size() / 3);
        image.raw()->palette->size = display_palette_size;
        unpack(image.raw()->palette->palette, reinterpret_cast<const UINT8*>(display_palette.data()), display_palette_size);
    }
    image.gif_logical_l_ = logical_l_with_palette;
    GIFDECODERSTATE context{};
    context.bits = static_cast<int>(bits);
    context.interlace = interlace ? 1 : 0;
    context.transparency = -1;
    ImagingCodecStateInstance state{};
    state.xsize = static_cast<int>(frame_width);
    state.ysize = static_cast<int>(frame_height);
    state.xoff = static_cast<int>(xoff);
    state.yoff = static_cast<int>(yoff);
    state.context = &context;
    const int consumed = ImagingGifDecode(image.raw(), &state,
        const_cast<UINT8*>(reinterpret_cast<const UINT8*>(bytes.data() + pos)), static_cast<Py_ssize_t>(bytes.size() - pos));
    // The vendor decoder returns -1 with errcode=0 only after every destination row has been populated.
    if (state.errcode != 0) {
        const char* message = state.errcode == IMAGING_CODEC_CONFIG ? "codec configuration error" :
                              state.errcode == IMAGING_CODEC_OVERRUN ? "buffer overrun" : "broken data stream";
        throw core::Error("format", std::string(message) + " when reading image file");
    }
    if (consumed != -1) {
        const auto remaining = bytes.size() - pos - static_cast<std::size_t>(std::max(consumed, 0));
        throw core::Error("format", "image file is truncated (" + std::to_string(remaining) + " bytes not processed)");
    }
    if (transparent_index >= 0) {
        Transparency transparency;
        transparency.kind = Transparency::Kind::Index;
        transparency.value = transparent_index;
        image.set_transparency(std::move(transparency));
    }
    if (!genko_imaging_budget_transfer(&rows.owner, image.raw())) detail::throw_imaging_error();
    return image;
}
}  // namespace genko::render
