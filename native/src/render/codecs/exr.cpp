// OpenEXR (render/codecs/formats.hpp; PhotoCraft v0.2.0 crates/codecs/src/codecs/exr.rs, MIT — its choice of channels;
// OpenEXR's library, BSD-3-Clause): the first part read at full resolution from its data window — its channels matched by
// the end of their names ("diffuse.R" is an R): R, G, B with A, or Y (or a single channel) with A; halves when they all
// are, floats otherwise —, after its header's size, kind and channels are checked here (no pixel allocated past the
// limits, no deep or subsampled data read); written as scan lines of halves or floats (Y, YA, RGB, RGBA) with a lossless
// compression (none, RLE, ZIPS, ZIP, PIZ). The light is linear (codecs.hpp: the linear-srgb profile).

#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfIO.h>
#include <ImfInputFile.h>
#include <ImfOutputFile.h>
#include <OpenEXRConfig.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <vector>

#include "core/error.hpp"
#include "render/codecs/codecs.hpp"
#include "render/codecs/formats.hpp"

namespace genko::render::codecs {

std::string openexr_version() { return OPENEXR_VERSION_STRING; }

namespace detail {

namespace {

// What the first header says, read here before the library reads it.
struct ExrHead {
    std::int64_t width = 0, height = 0;
    std::size_t channels = 0;
};

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
    // (each channel's samples as floats, and the picture of them: checked before the library reads a pixel)
    limits.check_bytes(head.width, head.height, std::uint64_t(std::min<std::size_t>(head.channels, 4)) * 4);
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
