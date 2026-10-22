// OpenEXR (render/codecs/formats.hpp; PhotoCraft v0.2.0 crates/codecs/src/codecs/exr.rs, MIT — its choice of channels;
// OpenEXR's library, BSD-3-Clause): the first part read at full resolution from its data window — its channels matched by
// the end of their names ("diffuse.R" is an R): R, G, B with A, or Y (or a single channel) with A; halves when they all
// are, floats otherwise —, after its header's size, kind and channels are checked here (no pixel allocated past the
// limits — the library's own work too: a block of rows or a tile of every channel, the tiles' table —, no deep or
// subsampled data read; a value that is not finite made one: NaN 0, ±infinity ±65504, the largest half); written as scan lines of halves or floats (Y, YA, RGB, RGBA) with a lossless compression (none, RLE, ZIPS, ZIP,
// PIZ). The light is linear (codecs.hpp: the linear-srgb profile).

#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfIO.h>
#include <ImfInputFile.h>
#include <ImfOutputFile.h>
#include <OpenEXRConfig.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "core/error.hpp"
#include "render/codecs/codecs.hpp"
#include "render/codecs/formats.hpp"

namespace genko::render::codecs {

std::string openexr_version() { return OPENEXR_VERSION_STRING; }

namespace detail {

namespace {

// (a file of more channels is not read: each block the library inflates holds all of them)
constexpr std::size_t kMaxChannels = 256;
// (a file of more tiles, all its levels counted, is not read: the library holds a table of them)
constexpr std::uint64_t kMaxTiles = std::uint64_t(1) << 24;

// What the first header says, read here before the library reads it.
struct ExrHead {
    std::int64_t width = 0, height = 0;
    std::size_t channels = 0;
    std::uint64_t channel_bytes = 0;  // a pixel of every channel (HALF 2, UINT and FLOAT 4)
    int compression = 3;              // (ZIP's 16 rows, should the header not say)
    bool tiled = false;
    std::uint32_t tile_x = 0, tile_y = 0;
    unsigned levels = 0, rounding = 0;  // (0 one level, 1 mipmap, 2 ripmap; 0 down, 1 up)
};

// The rows a block of the compression holds (OpenEXR's numLinesInBuffer; one it does not know, the most).
std::uint64_t rows_of(int compression) {
    switch (compression) {
    case 0: case 1: case 2: return 1;      // none, RLE, ZIPS
    case 3: case 5: return 16;              // ZIP, PXR24
    case 4: case 6: case 7: case 8: case 11: return 32;  // PIZ, B44, B44A, DWAA, HTJ2K32
    default: return 256;                    // DWAB, HTJ2K256, later ones
    }
}

// The tiles of all the levels (OpenEXR's numXLevels…, numXTiles…), counted up to past kMaxTiles.
std::uint64_t tiles_of(const ExrHead& h) {
    const auto sides = [&](std::int64_t side) {  // (a level's side: halved, rounded down or up)
        std::vector<std::uint64_t> out{static_cast<std::uint64_t>(side)};
        while (out.back() > 1) out.push_back(h.rounding == 1 ? (out.back() + 1) / 2 : out.back() / 2);
        return out;
    };
    const auto across = [](std::uint64_t side, std::uint32_t tile) { return (side + tile - 1) / tile; };
    const std::vector<std::uint64_t> xs = sides(h.width), ys = sides(h.height);
    std::uint64_t count = 0;
    if (h.levels == 0) return across(xs[0], h.tile_x) * across(ys[0], h.tile_y);
    if (h.levels == 1) {
        for (std::size_t l = 0; l < std::max(xs.size(), ys.size()) && count <= kMaxTiles; ++l)
            count += across(xs[std::min(l, xs.size() - 1)], h.tile_x) * across(ys[std::min(l, ys.size() - 1)], h.tile_y);
        return count;
    }
    for (const std::uint64_t x : xs) {
        for (const std::uint64_t y : ys) {
            count += across(x, h.tile_x) * across(y, h.tile_y);
            if (count > kMaxTiles) return count;
        }
    }
    return count;
}

// The picture and the library's work within the limits, before it reads the file: the picture of floats made, and a
// block of rows (a row of tiles for a tiled file) of every channel held twice — as the file has it and inflated —, with
// the table of its blocks; no more channels than kMaxChannels nor tiles than kMaxTiles, no tile larger than the picture
// (or 256 × 256).
void check_work(const ExrHead& h, const Limits& limits) {
    if (h.channels > kMaxChannels) {
        throw core::Error("image_too_large", "the OpenEXR file has too many channels (" + std::to_string(h.channels) + "; at most " +
                                                 std::to_string(kMaxChannels) + ")");
    }
    const std::uint64_t out_bytes = std::uint64_t(std::min<std::size_t>(h.channels, 4)) * 4;
    limits.check_bytes(h.width, h.height, out_bytes);
    const auto w = static_cast<std::uint64_t>(h.width), ht = static_cast<std::uint64_t>(h.height);
    std::uint64_t block_w = w, block_h = rows_of(h.compression), table = (ht + block_h - 1) / block_h;
    if (h.tiled) {
        if (h.tile_x == 0 || h.tile_y == 0 || h.levels > 2) broken("OpenEXR", "its tiles");
        if (std::uint64_t(h.tile_x) * h.tile_y > std::max<std::uint64_t>(w * ht, 256 * 256)) {
            throw core::Error("image_too_large", "the OpenEXR file's tiles are too large (" + std::to_string(h.tile_x) + "×" + std::to_string(h.tile_y) +
                                                     " pixels, for a picture of " + std::to_string(w) + "×" + std::to_string(ht) + ")");
        }
        table = tiles_of(h);
        if (table > kMaxTiles) throw core::Error("image_too_large", "the OpenEXR file has too many tiles (at most " + std::to_string(kMaxTiles) + ")");
        block_w = std::max<std::uint64_t>(w, h.tile_x);
        block_h = h.tile_y;
    }
    // (each factor at most 2^32 — sides of a picture within the limits, rows, tiles —, the channels' bytes at most 1024)
    const long double total = static_cast<long double>(w) * ht * out_bytes + 2.0L * block_w * block_h * h.channel_bytes + 8.0L * table;
    if (total > static_cast<long double>(limits.max_alloc)) {
        throw core::Error("image_too_large", "the picture is too large to read at once (" + std::to_string(static_cast<std::uint64_t>(std::min<long double>(
                                                 total, static_cast<long double>(std::numeric_limits<std::uint64_t>::max())))) +
                                                 " bytes; at most " + std::to_string(limits.max_alloc) + ")");
    }
}

// OpenEXR's own bounds on the sides of a picture and of a tile, set once (the library's checks when it reads a header).
void bound_the_library() {
    static std::once_flag once;
    std::call_once(once, [] {
        Imf::Header::setMaxImageSize(static_cast<int>(limits::kSide), static_cast<int>(limits::kSide));
        Imf::Header::setMaxTileSize(static_cast<int>(limits::kSide), static_cast<int>(limits::kSide));
    });
}

// A value that is not finite as one that is (NaN 0, ±infinity the largest half): what a page holds.
template <class T>
void made_finite(CodecImage& out, T (*read)(const char*), void (*write)(char*, T)) {
    const std::size_t each = sample_bytes(out.sample());
    char* data = out.data().data();
    for (std::size_t i = 0; i < out.sample_count(); ++i) {
        char* at = data + i * each;
        const float v = static_cast<float>(read(at));
        if (std::isfinite(v)) continue;
        write(at, static_cast<T>(std::isnan(v) ? 0.0f : std::copysign(65504.0f, v)));
    }
}

ExrHead exr_head(std::string_view b) {
    if (b.size() < 8 || be32(b, 0) != 0x762f3101) broken("OpenEXR", "its magic number");
    const std::uint32_t flags = le32(b, 4);
    if ((flags & 0xff) != 2) broken("OpenEXR", "a version it does not read");
    if ((flags & 0x800) != 0) broken("OpenEXR", "deep data, which is not read");
    const std::size_t name_max = (flags & 0x400) != 0 ? 255 : 31;
    std::size_t at = 8;
    ExrHead out;
    bool window = false, listed = false;
    const auto cstring = [&](std::size_t limit) {
        const std::size_t end = b.find('\0', at);
        if (end == std::string_view::npos || end - at > limit) broken("OpenEXR", "its header");
        const std::string_view s = b.substr(at, end - at);
        at = end + 1;
        return s;
    };
    for (;;) {
        const std::string_view name = cstring(name_max);
        if (name.empty()) break;
        const std::string_view type = cstring(name_max);
        if (at + 4 > b.size()) broken("OpenEXR", "its header is cut short");
        const std::uint32_t size = le32(b, at);
        at += 4;
        if (size > b.size() - at) broken("OpenEXR", "its header is cut short");
        const std::string_view value = b.substr(at, size);
        if (name == "dataWindow" && type == "box2i" && size == 16) {
            const auto i32 = [&](std::size_t k) { return static_cast<std::int64_t>(static_cast<std::int32_t>(le32(value, k))); };
            out.width = i32(8) - i32(0) + 1;
            out.height = i32(12) - i32(4) + 1;
            window = true;
        } else if (name == "compression" && type == "compression" && size == 1) {
            out.compression = static_cast<int>(u8(value, 0));
        } else if (name == "tiles" && type == "tiledesc" && size == 9) {
            out.tiled = true;
            out.tile_x = le32(value, 0);
            out.tile_y = le32(value, 4);
            out.levels = u8(value, 8) & 0x0f;
            out.rounding = u8(value, 8) >> 4;
        } else if (name == "channels" && type == "chlist") {
            std::size_t c = 0;
            while (c < value.size() && value[c] != '\0') {
                const std::size_t end = value.find('\0', c);
                if (end == std::string_view::npos || end + 1 + 16 > value.size()) broken("OpenEXR", "its channels");
                const std::size_t fields = end + 1;
                const std::uint32_t pixel = le32(value, fields);
                const auto xs = static_cast<std::int32_t>(le32(value, fields + 8)), ys = static_cast<std::int32_t>(le32(value, fields + 12));
                if (pixel > 2) broken("OpenEXR", "a channel of a kind it does not read");
                if (xs != 1 || ys != 1) broken("OpenEXR", "subsampled channels, which are not read");
                ++out.channels;
                out.channel_bytes += pixel == 1 ? 2 : 4;  // (UINT 0, HALF 1, FLOAT 2)
                c = fields + 16;
            }
            listed = true;
        } else if (name == "type" && (value.starts_with("deepscanline") || value.starts_with("deeptile"))) {
            broken("OpenEXR", "deep data, which is not read");
        }
        at += size;
    }
    if (!window || !listed || out.channels == 0) broken("OpenEXR", "its header has no data window or channels");
    if (out.width <= 0 || out.height <= 0) broken("OpenEXR", "its data window");
    if ((flags & 0x200) != 0 && !out.tiled) broken("OpenEXR", "its tiles");
    return out;
}

class Input : public Imf::IStream {
public:
    explicit Input(std::string_view bytes) : Imf::IStream("picture.exr"), b_(bytes) {}
    bool read(char c[], int n) override {
        if (n < 0 || pos_ > b_.size() || static_cast<std::size_t>(n) > b_.size() - pos_) throw std::runtime_error("the file is cut short");
        std::memcpy(c, b_.data() + pos_, static_cast<std::size_t>(n));
        pos_ += static_cast<std::size_t>(n);
        return pos_ < b_.size();
    }
    uint64_t tellg() override { return pos_; }
    void seekg(uint64_t pos) override { pos_ = static_cast<std::size_t>(std::min<uint64_t>(pos, b_.size() + 1)); }
    void clear() override {}

private:
    std::string_view b_;
    std::size_t pos_ = 0;
};

class Output : public Imf::OStream {
public:
    Output() : Imf::OStream("picture.exr") {}
    void write(const char c[], int n) override {
        if (pos_ + static_cast<std::size_t>(n) > bytes.size()) bytes.resize(pos_ + static_cast<std::size_t>(n));
        std::memcpy(bytes.data() + pos_, c, static_cast<std::size_t>(n));
        pos_ += static_cast<std::size_t>(n);
    }
    uint64_t tellp() override { return pos_; }
    void seekp(uint64_t pos) override { pos_ = static_cast<std::size_t>(pos); }
    std::string bytes;

private:
    std::size_t pos_ = 0;
};

// The channel's name without its layer ("diffuse.R" → "R").
std::string_view base_name(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1);
}

}  // namespace

std::pair<std::uint32_t, std::uint32_t> exr_size(std::string_view bytes) {
    const ExrHead h = exr_head(bytes);
    if (h.width > 0xffffffffLL || h.height > 0xffffffffLL) broken("OpenEXR", "its data window");
    return {static_cast<std::uint32_t>(h.width), static_cast<std::uint32_t>(h.height)};
}

CodecImage read_exr(std::string_view bytes, const Limits& limits) {
    const ExrHead head = exr_head(bytes);
    check_work(head, limits);  // (before the library reads a pixel)
    bound_the_library();
    try {
        Input in(bytes);
        Imf::InputFile file(in, 0);
        const Imf::Header& header = file.header();
        const Imath::Box2i window = header.dataWindow();
        const std::int64_t w = std::int64_t(window.max.x) - window.min.x + 1, h = std::int64_t(window.max.y) - window.min.y + 1;
        if (w != head.width || h != head.height) broken("OpenEXR", "its data window");
        const Imf::ChannelList& list = header.channels();
        const auto find = [&](std::string_view want) -> const Imf::ChannelList::ConstIterator {
            for (auto it = list.begin(); it != list.end(); ++it) {
                if (base_name(it.name()) == want) return it;
            }
            return list.end();
        };
        std::vector<Imf::ChannelList::ConstIterator> chosen;
        Layout layout = Layout::Rgb;
        const auto r = find("R"), g = find("G"), bl = find("B"), a = find("A");
        if (r != list.end() && g != list.end() && bl != list.end()) {
            chosen = {r, g, bl};
            layout = Layout::Rgb;
        } else if (auto y = find("Y"); y != list.end() || head.channels == 1) {
            chosen = {y != list.end() ? y : list.begin()};
            layout = Layout::Gray;
        } else {
            throw core::Error("format", "the OpenEXR file has no R, G, B or Y channels");
        }
        if (a != list.end() && std::find(chosen.begin(), chosen.end(), a) == chosen.end()) {
            chosen.push_back(a);
            layout = with_alpha(layout);
        }
        bool halves = true;
        for (const auto& c : chosen) {
            if (c.channel().xSampling != 1 || c.channel().ySampling != 1) throw core::Error("format", "the OpenEXR file's channels are subsampled");
            if (c.channel().type != Imf::HALF) halves = false;
        }
        const Sample sample = halves ? Sample::F16 : Sample::F32;
        limits.check(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), layout, sample);
        CodecImage out = CodecImage::blank(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), layout, sample);
        const std::size_t bytes_per = sample_bytes(sample), nc = chosen.size();
        Imf::FrameBuffer buffer;
        for (std::size_t c = 0; c < nc; ++c) {
            buffer.insert(chosen[c].name(), Imf::Slice::Make(halves ? Imf::HALF : Imf::FLOAT, out.data().data() + c * bytes_per, window,
                                                            nc * bytes_per, std::size_t(w) * nc * bytes_per));
        }
        file.setFrameBuffer(buffer);
        file.readPixels(window.min.y, window.max.y);
        if (halves) {
            made_finite<float>(
                out,
                [](const char* at) {
                    std::uint16_t bits = 0;
                    std::memcpy(&bits, at, 2);
                    return float_from_half(bits);
                },
                [](char* at, float v) {
                    const std::uint16_t bits = half_from_float(v);
                    std::memcpy(at, &bits, 2);
                });
        } else {
            made_finite<float>(
                out,
                [](const char* at) {
                    float v = 0;
                    std::memcpy(&v, at, 4);
                    return v;
                },
                [](char* at, float v) { std::memcpy(at, &v, 4); });
        }
        return out;
    } catch (const core::Error&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw core::Error("image_too_large", "the OpenEXR file is too large to read");
    } catch (const std::exception& error) {
        broken("OpenEXR", error.what());
    }
}

std::string write_exr(const CodecImage& stored, const EncodeOptions& options) {
    const Layout layout = stored.layout();
    if (is_cmyk(layout) || !is_float(stored.sample())) throw core::Error("value", "OpenEXR holds halves or floats of grey and RGB");
    const std::vector<std::string_view> names = layout == Layout::Gray    ? std::vector<std::string_view>{"Y"}
                                                : layout == Layout::GrayA ? std::vector<std::string_view>{"Y", "A"}
                                                : layout == Layout::Rgb   ? std::vector<std::string_view>{"R", "G", "B"}
                                                                          : std::vector<std::string_view>{"R", "G", "B", "A"};
    const bool halves = stored.sample() == Sample::F16;
    Imf::Compression compression = Imf::ZIP_COMPRESSION;
    switch (options.exr_compression) {
    case ExrCompression::None: compression = Imf::NO_COMPRESSION; break;
    case ExrCompression::Rle: compression = Imf::RLE_COMPRESSION; break;
    case ExrCompression::Zip1: compression = Imf::ZIPS_COMPRESSION; break;
    case ExrCompression::Zip16: compression = Imf::ZIP_COMPRESSION; break;
    case ExrCompression::Piz: compression = Imf::PIZ_COMPRESSION; break;
    }
    try {
        Imf::Header header(static_cast<int>(stored.width()), static_cast<int>(stored.height()), 1.0f, Imath::V2f(0, 0), 1.0f, Imf::INCREASING_Y,
                           compression);
        for (const std::string_view name : names) header.channels().insert(std::string(name), Imf::Channel(halves ? Imf::HALF : Imf::FLOAT));
        Output out;
        {
            Imf::OutputFile file(out, header, 0);
            const std::size_t bytes_per = sample_bytes(stored.sample()), nc = names.size();
            Imf::FrameBuffer buffer;
            auto* base = const_cast<char*>(stored.data().data());
            for (std::size_t c = 0; c < nc; ++c) {
                buffer.insert(std::string(names[c]), Imf::Slice(halves ? Imf::HALF : Imf::FLOAT, base + c * bytes_per, nc * bytes_per,
                                                                std::size_t(stored.width()) * nc * bytes_per));
            }
            file.setFrameBuffer(buffer);
            file.writePixels(static_cast<int>(stored.height()));
        }
        return std::move(out.bytes);
    } catch (const std::exception& error) {
        throw core::Error("io", std::string("cannot write OpenEXR: ") + error.what());
    }
}

}  // namespace detail

}  // namespace genko::render::codecs
