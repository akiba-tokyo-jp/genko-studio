// The codecs (render/codecs/codecs.hpp; PhotoCraft's crates/codecs/src/lib.rs): PC4 F-0's readers and writers are this
// build's own, behind the codecs' picture.

#include "render/codecs/codecs.hpp"

#include <algorithm>
#include <cstring>

#include "core/error.hpp"
#include "render/image.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
#include "render/smart_decode.hpp"

namespace genko::render::codecs {

namespace {

// The picture open_image read (in Pillow's mode) as the codecs' picture: its colours and its alpha — a transparent colour
// or palette entry (tRNS, a GIF's) as an alpha —, 8 bits; "I;16" 16 bits; "I" and "F" floats.
CodecImage from_pillow(const Image& picture) {
    const std::string_view mode = picture.mode();
    const bool transparent = picture.transparency().present();
    const auto w = static_cast<std::uint32_t>(picture.width()), h = static_cast<std::uint32_t>(picture.height());
    const auto take = [&](const Image& p, Layout layout) { return CodecImage::from_bytes(w, h, layout, Sample::U8, p.tobytes()); };
    if (mode == "1" || mode == "L") return transparent ? take(picture.convert("LA"), Layout::GrayA) : take(picture.convert("L"), Layout::Gray);
    if (mode == "LA") return take(picture, Layout::GrayA);
    if (mode == "La") return take(picture.convert("LA"), Layout::GrayA);
    if (mode == "RGB") return transparent ? take(picture.convert("RGBA"), Layout::Rgba) : take(picture, Layout::Rgb);
    if (mode == "RGBA") return take(picture, Layout::Rgba);
    if (mode == "CMYK") return take(picture, Layout::Cmyk);
    if (mode == "P" || mode == "PA") {
        const bool alpha = transparent || mode == "PA" || picture.palette_mode() == "RGBA";
        return alpha ? take(picture.convert("RGBA"), Layout::Rgba) : take(picture.convert("RGB"), Layout::Rgb);
    }
    if (mode.starts_with("I;16")) {  // (Pillow's I;16 bytes are little-endian: this machine's order here)
        const std::string raw = picture.tobytes();
        const bool big = mode == "I;16B";
        std::string data(raw.size(), '\0');
        for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
            const auto lo = static_cast<unsigned char>(raw[i + (big ? 1 : 0)]), hi = static_cast<unsigned char>(raw[i + (big ? 0 : 1)]);
            const auto v = static_cast<std::uint16_t>(lo | (hi << 8));
            std::memcpy(data.data() + i, &v, 2);
        }
        return CodecImage::from_bytes(w, h, Layout::Gray, Sample::U16, std::move(data));
    }
    if (mode == "I" || mode == "F") {  // (32-bit integers in Pillow's 16-bit range, floats as they are)
        const std::string raw = picture.tobytes();
        std::vector<float> values(std::size_t(w) * h);
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (mode == "F") {
                std::memcpy(&values[i], raw.data() + i * 4, 4);
            } else {
                std::int32_t v = 0;
                std::memcpy(&v, raw.data() + i * 4, 4);
                values[i] = static_cast<float>(v / 65535.0);
            }
        }
        return CodecImage::from_normalized(w, h, Layout::Gray, Sample::F32, values);
    }
    const bool alpha = transparent || mode.find('A') != std::string_view::npos || mode.find('a') != std::string_view::npos;
    return alpha ? take(picture.convert("RGBA"), Layout::Rgba) : take(picture.convert("RGB"), Layout::Rgb);
}

// smart_decode's picture with all its bits (a PNG of 16 bits, a TIFF of 16 or 32) as the codecs' picture.
CodecImage from_deep(const smart::DeepPicture& deep) {
    Layout layout = Layout::Rgb;
    const std::string_view mode = deep.mode;
    if (mode.starts_with("gray")) layout = deep.alpha ? Layout::GrayA : Layout::Gray;
    else if (mode.starts_with("cmyk")) layout = deep.alpha ? Layout::CmykA : Layout::Cmyk;
    else layout = deep.alpha ? Layout::Rgba : Layout::Rgb;
    if (channels(layout) != deep.channels) throw core::Error("format", "the picture's channels are not ones this build reads");
    const Sample sample = deep.depth == 16 ? Sample::U16 : deep.depth == 32 ? Sample::F32 : Sample::U8;
    return CodecImage::from_normalized(static_cast<std::uint32_t>(deep.width), static_cast<std::uint32_t>(deep.height), layout, sample, deep.samples);
}

std::string_view pillow_mode(Layout layout) {
    switch (layout) {
    case Layout::Gray: return "L";
    case Layout::GrayA: return "LA";
    case Layout::Rgb: return "RGB";
    case Layout::Rgba: return "RGBA";
    case Layout::Cmyk: return "CMYK";
    case Layout::CmykA: return "CMYK";
    }
    return "RGB";
}

int png_level(PngCompression c) {
    switch (c) {
    case PngCompression::None: return 0;
    case PngCompression::Fast: return 1;
    case PngCompression::Default: return 6;
    case PngCompression::Best: return 9;
    }
    return 6;
}

}  // namespace

CodecImage decode(std::string_view bytes, const Limits& limits) {
    const auto format = detect(bytes);
    if (!format) throw core::Error("unidentified_image", "the bytes are not a picture of a format this build knows");
    return decode_as(*format, bytes, limits);
}

CodecImage decode_as(Format format, std::string_view bytes, const Limits& limits) {
    if (!caps(format).read) throw NotYetPorted("image_format");
    const auto said = detect(bytes);
    if (!said || *said != format) throw core::Error("format", "the file is not a " + std::string(name(format)) + " file");
    // (its size from its header first: no pixel is allocated for a picture past the limits)
    const Size size = picture_size(bytes);
    const int depth = smart::stored_depth(bytes);
    limits.check_bytes(size.width, size.height, depth > 8 ? 16 : 4);  // (the readers' work: 4 bytes a pixel, floats for the deep ones)
    CodecImage out;
    if (depth > 8 && (format == Format::Png || format == Format::Tiff)) {
        out = from_deep(smart::deep_picture(bytes));
    } else {
        PngLimits reading;
        reading.max_pixels = std::min<std::int64_t>(limits.max_pixels, reading.max_pixels);
        reading.max_side = std::max(limits.max_width, limits.max_height);
        out = from_pillow(open_image(bytes, reading));
    }
    limits.check(out.width(), out.height(), out.layout(), out.sample());  // (what was made, once more)
    out.icc = proof::embedded_profile(bytes);
    if (const double dpi = smart::stored_dpi(bytes); dpi > 0) out.meta.dpi = std::make_pair(dpi, dpi);
    return out;
}

std::string encode(const CodecImage& image, Format format, const EncodeOptions& options) {
    if (!caps(format).write) throw NotYetPorted("image_write");
    if (image.empty()) throw core::Error("value", "a picture of no pixels cannot be written");
    if (const auto most = max_dimensions(format); most && (image.width() > most->first || image.height() > most->second)) {
        throw core::Error("image_too_large", std::string(name(format)) + " holds at most " + std::to_string(most->first) + "×" +
                                                 std::to_string(most->second) + " pixels");
    }
    const Plan p = plan(image, format, options);
    const CodecImage stored = image.convert(p.layout, p.sample);
    const Size size{static_cast<int>(stored.width()), static_cast<int>(stored.height())};
    switch (format) {
    case Format::Png: return write_png(Image::frombytes(pillow_mode(p.layout), size, stored.data()), png_level(options.png_compression));
    case Format::Jpeg: return write_jpeg(Image::frombytes("RGB", size, stored.data()), std::clamp(options.jpeg_quality, 1, 100));
    default: throw NotYetPorted("image_write");
    }
}

}  // namespace genko::render::codecs
