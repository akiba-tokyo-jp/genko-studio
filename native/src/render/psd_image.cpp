// A PSD opened as one picture: Pillow 12.3's PsdImagePlugin (PsdImageFile._open, _maketile) with ImageFile.load and its
// "raw" and "packbits" decoders (libImaging's RawDecode.c and PackDecode.c, with the vendored unpackers), as io.BytesIO
// reads (a read past the end gives fewer bytes, a seek goes anywhere).

#include "render/psd_image.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "render/imaging.hpp"

namespace genko::render {

namespace {

// What Image.open turns into UnidentifiedImageError: a SyntaxError of the plugin (struct.error, IndexError, KeyError
// and "not identified by this driver" are made SyntaxError by ImageFile.__init__)
[[noreturn]] void unidentified() { throw core::Error("unidentified_image", "cannot identify image file"); }

// io.BytesIO over the file's bytes
class Stream {
public:
    explicit Stream(std::string_view data) : data_(data) {}
    std::string_view read(std::uint64_t n) {
        if (pos_ >= data_.size()) return {};
        const std::uint64_t left = data_.size() - pos_;
        const std::string_view out = data_.substr(static_cast<std::size_t>(pos_), static_cast<std::size_t>(std::min(n, left)));
        pos_ += out.size();
        return out;
    }
    std::uint64_t tell() const { return pos_; }
    void seek(std::uint64_t pos) { pos_ = pos; }
    std::string_view data() const { return data_; }

private:
    std::string_view data_;
    std::uint64_t pos_ = 0;
};

// i16 / i32 / i8 (unsigned, big-endian): struct.error or IndexError when the bytes are too few
std::uint32_t be(std::string_view b, std::size_t at, std::size_t n) {
    if (at > b.size() || n > b.size() - at) unidentified();
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v = (v << 8) | static_cast<unsigned char>(b[at + i]);
    return v;
}
std::uint32_t i16(std::string_view b, std::size_t at = 0) { return be(b, at, 2); }
std::uint32_t i32(std::string_view b, std::size_t at = 0) { return be(b, at, 4); }
std::uint32_t i8(std::string_view b) { return be(b, 0, 1); }

struct Tile {
    bool packbits = false;
    std::uint64_t offset = 0;
    std::string rawmode;
};

struct Opened {
    std::string mode;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<std::string> palette;  // (the colour mode data of a "P" file, 768 bytes asked for)
    std::vector<Tile> tiles;
};

// MODES: (photoshop mode, bits) → (pil mode, required channels)
bool mode_of(std::uint32_t psd_mode, std::uint32_t bits, std::string* mode, std::uint32_t* channels) {
    struct Entry {
        std::uint32_t psd_mode;
        std::uint32_t bits;
        const char* mode;
        std::uint32_t channels;
    };
    static constexpr Entry kModes[] = {{0, 1, "1", 1}, {0, 8, "L", 1},   {1, 8, "L", 1}, {2, 8, "P", 1}, {3, 8, "RGB", 3},
                                       {4, 8, "CMYK", 4}, {7, 8, "L", 1}, {8, 8, "L", 1}, {9, 8, "LAB", 3}};
    for (const Entry& e : kModes) {
        if (e.psd_mode == psd_mode && e.bits == bits) {
            *mode = e.mode;
            *channels = e.channels;
            return true;
        }
    }
    return false;
}

// _maketile(file, mode, (0, 0) + size, channels)
std::vector<Tile> maketile(Stream& file, const std::string& mode, std::uint32_t xsize, std::uint32_t ysize, std::uint32_t channels) {
    std::vector<Tile> tiles;
    const std::uint32_t compression = i16(file.read(2));
    std::uint64_t offset = file.tell();
    const auto layer_of = [&](std::uint32_t channel) {
        // mode[channel] (a mode of one channel is its own letter, "1", "L" or "P"), ";I" after CMYK's
        std::string layer(1, mode[channel]);
        if (mode == "CMYK") layer += ";I";
        return layer;
    };
    if (compression == 0) {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            tiles.push_back(Tile{false, offset, layer_of(channel)});
            offset += static_cast<std::uint64_t>(xsize) * ysize;
        }
    } else if (compression == 1) {
        const std::string_view bytecount = file.read(static_cast<std::uint64_t>(channels) * ysize * 2);
        offset = file.tell();
        std::size_t i = 0;
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            tiles.push_back(Tile{true, offset, layer_of(channel)});
            for (std::uint32_t y = 0; y < ysize; ++y) {
                offset += i16(bytecount, i);
                i += 2;
            }
        }
    }
    file.seek(offset);
    if ((offset & 1) != 0) file.read(1);  // padding
    return tiles;
}

// PsdImageFile._open, ImageFile.__init__'s checks and Image.open's decompression bomb check
Opened open_psd(std::string_view bytes, const PngLimits& limits) {
    Stream fp(bytes);
    Opened out;
    // header
    const std::string_view s = fp.read(26);
    if (!s.starts_with("8BPS") || i16(s, 4) != 1) unidentified();  // ("not a PSD file")
    const std::uint32_t psd_bits = i16(s, 22);
    const std::uint32_t psd_channels = i16(s, 12);
    const std::uint32_t psd_mode = i16(s, 24);
    std::uint32_t channels = 0;
    if (!mode_of(psd_mode, psd_bits, &out.mode, &channels)) unidentified();  // (KeyError)
    if (channels > psd_channels) throw core::Error("format", "not enough channels");
    if (out.mode == "RGB" && psd_channels == 4) {
        out.mode = "RGBA";
        channels = 4;
    }
    out.width = i32(s, 18);
    out.height = i32(s, 14);
    // color mode data
    std::uint32_t size = i32(fp.read(4));
    if (size != 0) {
        const std::string_view data = fp.read(size);
        if (out.mode == "P" && size == 768) out.palette = std::string(data);
    }
    // image resources
    size = i32(fp.read(4));
    if (size != 0) {
        const std::uint64_t end = fp.tell() + size;
        while (fp.tell() < end) {
            fp.read(4);  // signature
            (void)i16(fp.read(2));  // id
            const std::string_view name = fp.read(i8(fp.read(1)));
            if ((name.size() & 1) == 0) fp.read(1);  // padding
            const std::string_view data = fp.read(i32(fp.read(4)));
            if ((data.size() & 1) != 0) fp.read(1);  // padding
        }
    }
    // layer and mask information (the layers are not read: only the merged picture is opened)
    size = i32(fp.read(4));
    if (size != 0) {
        const std::uint64_t end = fp.tell() + size;
        (void)i32(fp.read(4));
        fp.seek(end);
    }
    // image descriptor
    out.tiles = maketile(fp, out.mode, out.width, out.height, channels);
    if (out.width == 0 || out.height == 0) unidentified();  // ("not identified by this driver")
    const std::uint64_t pixels = static_cast<std::uint64_t>(out.width) * out.height;
    if (limits.max_pixels >= 0 && pixels > static_cast<std::uint64_t>(limits.max_pixels)) {
        throw core::Error("image_too_large", "Image size (" + std::to_string(pixels) + " pixels) exceeds limit of " +
                                                 std::to_string(limits.max_pixels) + " pixels, could be decompression bomb DOS attack.");
    }
    if (beyond_side(out.width, out.height, limits)) {
        throw core::Error("image_too_large", "image is wider or taller than " + std::to_string(limits.max_side) + " pixels");
    }
    return out;
}

[[noreturn]] void truncated(std::size_t left) {
    throw core::Error("format", "image file is truncated (" + std::to_string(left) + " bytes not processed)");
}

}  // namespace

Size psd_image_size(std::string_view bytes, const PngLimits& limits) {
    const Opened opened = open_psd(bytes, limits);
    return Size{static_cast<int>(opened.width), static_cast<int>(opened.height)};
}

bool psd_lab(std::string_view bytes) {
    return bytes.size() >= 26 && bytes.starts_with("8BPS") && i16(bytes, 4) == 1 && i16(bytes, 24) == 9 && i16(bytes, 22) == 8;
}

Image read_psd_image(std::string_view bytes, const PngLimits& limits) {
    Opened opened = open_psd(bytes, limits);
    // ImageFile.load
    if (opened.tiles.empty()) throw core::Error("format", "cannot load this image");
    const int width = static_cast<int>(opened.width);
    const int height = static_cast<int>(opened.height);
    if (opened.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        opened.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw core::Error("image_too_large", "image is too large");
    }
    constexpr std::uint64_t kBudget = 1ull << 30;
    const std::uint64_t pixel_bytes = static_cast<std::uint64_t>(opened.width) * opened.height * (detail::mode_bands(opened.mode) > 1 ? 4 : 1);
    if (pixel_bytes > kBudget) throw core::Error("memory", "PSD decoding exceeds memory budget");
    ImageAllocationBudget budget(kBudget);
    // load_prepare: the picture, and a "P" file's palette realized (putpalette("RGB", "RGB;L", data))
    Image image = Image::create_blank(opened.mode, Size{width, height});
    if (opened.mode == "P" && opened.palette) {
        auto* im = image.raw();
        int bits = 0;
        const ImagingShuffler unpack = ImagingFindUnpacker(IMAGING_MODE_RGB, detail::rawmode_id("RGB;L"), &bits);
        if (unpack == nullptr || bits <= 0) unidentified();
        ImagingPaletteDelete(im->palette);
        im->palette = ImagingPaletteNew(IMAGING_MODE_RGB);
        if (im->palette == nullptr) detail::throw_imaging_error();
        im->palette->size = static_cast<int>(opened.palette->size() * 8 / static_cast<std::size_t>(bits));
        unpack(im->palette->palette, reinterpret_cast<const UINT8*>(opened.palette->data()), im->palette->size);
    }
    // the tiles in file order; of the ones after one another alike but for their offset, the last
    std::stable_sort(opened.tiles.begin(), opened.tiles.end(), [](const Tile& a, const Tile& b) { return a.offset < b.offset; });
    std::vector<Tile> tiles;
    for (const Tile& tile : opened.tiles) {
        if (!tiles.empty() && tiles.back().packbits == tile.packbits && tiles.back().rawmode == tile.rawmode) tiles.back() = tile;
        else tiles.push_back(tile);
    }
    const std::string_view data = bytes;
    for (const Tile& tile : tiles) {
        int bits = 0;
        const ImagingShuffler shuffle = ImagingFindUnpacker(detail::mode_id(opened.mode), detail::rawmode_id(tile.rawmode), &bits);
        if (shuffle == nullptr || bits <= 0) throw core::Error("value", "unknown raw mode for given image mode");
        const std::uint64_t row_bytes = (static_cast<std::uint64_t>(bits) * opened.width + 7) / 8;
        const auto row_of = [&](std::uint32_t y) {
            return reinterpret_cast<UINT8*>(image.raw()->image[y]);
        };
        // what the reads from the tile's offset give, to the end of the file
        const std::string_view avail = tile.offset < data.size() ? data.substr(static_cast<std::size_t>(tile.offset)) : std::string_view();
        if (!tile.packbits) {
            // RawDecode: whole rows, unpacked as they come
            const std::uint64_t rows = row_bytes == 0 ? 0 : avail.size() / row_bytes;
            const std::uint64_t whole = std::min<std::uint64_t>(rows, opened.height);
            for (std::uint32_t y = 0; y < whole; ++y) {
                shuffle(row_of(y), reinterpret_cast<const UINT8*>(avail.data() + static_cast<std::size_t>(y * row_bytes)), width);
            }
            if (whole < opened.height) truncated(static_cast<std::size_t>(avail.size() - whole * row_bytes));
            continue;
        }
        // PackDecode: runs and literals into a row buffer; a full row unpacked; what overruns a row dropped
        std::vector<UINT8> buffer(static_cast<std::size_t>(row_bytes), 0);
        std::size_t x = 0;
        std::uint32_t y = 0;
        std::size_t at = 0;
        bool done = false;
        const auto line_full = [&] {
            if (x >= buffer.size()) {
                shuffle(row_of(y), buffer.data(), width);
                x = 0;
                if (++y >= opened.height) done = true;
            }
        };
        while (!done) {
            const std::size_t left = avail.size() - at;
            if (left < 1) break;
            const auto head = static_cast<unsigned char>(avail[at]);
            if ((head & 0x80) != 0) {
                if (head == 0x80) {  // nop
                    ++at;
                    continue;
                }
                if (left < 2) break;  // run
                for (unsigned n = 257u - head; n > 0; --n) {
                    if (x >= buffer.size()) break;
                    buffer[x++] = static_cast<UINT8>(avail[at + 1]);
                }
                at += 2;
            } else {
                const std::size_t n = static_cast<std::size_t>(head) + 2;  // literal
                if (left < n) break;
                for (std::size_t i = 1; i < n; ++i) {
                    if (x >= buffer.size()) break;
                    buffer[x++] = static_cast<UINT8>(avail[at + i]);
                }
                at += n;
            }
            line_full();
        }
        if (!done) truncated(avail.size() - at);
    }
    return image;
}

}  // namespace genko::render
