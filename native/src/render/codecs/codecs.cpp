// The codecs (render/codecs/codecs.hpp; PhotoCraft's crates/codecs/src/lib.rs): each format's reader and writer
// (render/codecs/formats.hpp) behind the codecs' picture.

#include "render/codecs/codecs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/color_model.hpp"
#include "core/error.hpp"
#include "render/codecs/formats.hpp"
#include "render/image.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"
#include "render/smart_decode.hpp"

namespace genko::render::codecs {

namespace detail {

[[noreturn]] void broken(std::string_view what, std::string_view why) {
    throw core::Error("format", "the " + std::string(what) + " file cannot be read (" + std::string(why) + ")");
}

CodecImage from_eight_bit(std::uint32_t width, std::uint32_t height, Layout layout, std::string data) {
    return CodecImage::from_bytes(width, height, layout, Sample::U8, std::move(data));
}

// The picture open_image read (in Pillow's mode) as the codecs' picture: its colours and its alpha — a transparent colour
// or palette entry (tRNS, a GIF's) as an alpha —, 8 bits; "I;16" 16 bits; "I" and "F" floats.
CodecImage from_pillow(const Image& picture) {
    const std::string_view mode = picture.mode();
    const bool transparent = picture.transparency().present();
    const auto w = static_cast<std::uint32_t>(picture.width()), h = static_cast<std::uint32_t>(picture.height());
    const auto take = [&](const Image& p, Layout layout) { return from_eight_bit(w, h, layout, p.tobytes()); };
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

// smart_decode's picture with all its bits (a PNG of 16 bits, a TIFF of 8, 16 or 32) as the codecs' picture.
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

CodecImage this_builds_picture(std::string_view bytes, const Limits& limits, bool deep) {
    // (its size from its header first: no pixel is allocated for a picture past the limits)
    const Size size = picture_size(bytes);
    const int depth = smart::stored_depth(bytes);
    // (the readers' work at its height: 4 bytes a pixel for an 8-bit picture; a deep one's floats — 4 bytes a sample —
    // held with the picture made from them, 2 bytes a 16-bit sample and 4 a float (a 16-bit PNG's rows as libpng gives
    // them, 2 bytes a sample, are held with the floats the same way); an 8-bit one read with all its bits, 1 byte a sample)
    const auto samples = static_cast<std::uint64_t>(std::clamp(smart::stored_samples(bytes), 1, 5));
    limits.check_bytes(size.width, size.height, deep ? samples * (4 + (depth > 16 ? 4 : depth > 8 ? 2 : 1)) : 4);
    if (deep) return from_deep(smart::deep_picture(bytes));
    PngLimits reading;
    reading.max_pixels = std::min<std::int64_t>(limits.max_pixels, reading.max_pixels);
    reading.max_side = std::max(limits.max_width, limits.max_height);
    return from_pillow(open_image(bytes, reading));
}

}  // namespace detail

namespace {

CodecImage read_as(Format format, std::string_view bytes, const Limits& limits) {
    switch (format) {
    case Format::Png: return detail::read_png(bytes, limits);
    case Format::Jpeg: return detail::read_jpeg(bytes, limits);
    case Format::Tiff: return detail::read_tiff(bytes, limits);
    case Format::WebP: return detail::read_webp(bytes, limits);
    case Format::Gif: return detail::read_gif(bytes, limits);
    case Format::Bmp: return detail::read_bmp(bytes, limits);
    case Format::Tga: return detail::read_tga(bytes, limits);
    case Format::Ico: return detail::read_ico(bytes, limits);
    case Format::Pnm: return detail::read_pnm(bytes, limits);
    case Format::Qoi: return detail::read_qoi(bytes, limits);
    case Format::OpenExr: return detail::read_exr(bytes, limits);
    case Format::Hdr: return detail::read_hdr(bytes, limits);
    case Format::Avif: return detail::read_avif(bytes, limits);
    }
    throw NotYetPorted("image_format");
}

const std::string& linear_srgb_bytes() {
    static const std::string bytes = [] {
        const core::BuiltinProfile* p = core::builtin_profile("linear-srgb");
        return p != nullptr && p->bytes ? *p->bytes : std::string();
    }();
    return bytes;
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
    // (a TGA has no magic number: one taken for another format by its first bytes is still read as a TGA when asked)
    if (format != Format::Tga && (!said || *said != format)) throw core::Error("format", "the file is not a " + std::string(name(format)) + " file");
    CodecImage out = read_as(format, bytes, limits);
    limits.check(out.width(), out.height(), out.layout(), out.sample());  // (what was made, once more)
    // (linear light of sRGB's primaries: an RGB picture's profile the built-in linear-srgb; a grey one has none — there is
    // no built-in linear grey: its readers give it sRGB's curve, srgb_encoded)
    if (linear_light(format) && is_rgb(out.layout()) && !out.icc && !linear_srgb_bytes().empty()) out.icc = linear_srgb_bytes();
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
    CodecImage stored = image.convert(p.layout, p.sample);
    // (the profile kept for the same colours only — convert's rule —, and none where the format's implied profile is it)
    if (!options.embed_icc || !caps(format).icc) stored.icc.reset();
    if (!options.embed_metadata) stored.meta = Metadata{};
    switch (format) {
    case Format::Png: return detail::write_png(stored, options);
    case Format::Jpeg: return detail::write_jpeg(stored, options);
    case Format::Tiff: return detail::write_tiff(stored, options);
    case Format::WebP: return detail::write_webp(stored, options);
    case Format::Gif: return detail::write_gif(stored, options);
    case Format::Bmp: return detail::write_bmp(stored, options);
    case Format::Tga: return detail::write_tga(stored, options);
    case Format::Ico: return detail::write_ico(std::span<const CodecImage>(&stored, 1), options);
    case Format::Pnm: return detail::write_pnm(stored, options);
    case Format::Qoi: return detail::write_qoi(stored, options);
    case Format::OpenExr: return detail::write_exr(stored, options);
    case Format::Hdr: return detail::write_hdr(stored, options);
    case Format::Avif: return detail::write_avif(stored, options);
    }
    throw NotYetPorted("image_write");
}

std::string encode_icon(std::span<const CodecImage> sizes, const EncodeOptions& options) {
    if (sizes.empty()) throw core::Error("value", "an icon of no pictures cannot be written");
    std::vector<CodecImage> stored;
    stored.reserve(sizes.size());
    for (const CodecImage& image : sizes) {
        if (image.empty()) throw core::Error("value", "a picture of no pixels cannot be written");
        if (image.width() > 256 || image.height() > 256) throw core::Error("image_too_large", "ICO holds at most 256×256 pixels");
        const Plan p = plan(image, Format::Ico, options);
        stored.push_back(image.convert(p.layout, p.sample));
    }
    return detail::write_ico(stored, options);
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> header_size(std::string_view bytes) {
    const auto format = detect(bytes);
    if (!format) return std::nullopt;
    try {
        switch (*format) {
        case Format::Tga: return detail::tga_size(bytes);
        case Format::Ico: return detail::ico_size(bytes);
        case Format::Pnm: return detail::pnm_size(bytes);
        case Format::Qoi: return detail::qoi_size(bytes);
        case Format::OpenExr: return detail::exr_size(bytes);
        case Format::Hdr: return detail::hdr_size(bytes);
        case Format::Avif: return detail::avif_size(bytes);
        default: {
            const Size size = picture_size(bytes);
            return std::make_pair(static_cast<std::uint32_t>(size.width), static_cast<std::uint32_t>(size.height));
        }
        }
    } catch (const core::Error&) {
        return std::nullopt;
    }
}

bool linear_light(Format format) { return format == Format::OpenExr || format == Format::Hdr; }

bool read_here(Format format) {
    switch (format) {
    case Format::Tga:
    case Format::Ico:
    case Format::Pnm:
    case Format::Qoi:
    case Format::OpenExr:
    case Format::Hdr:
    case Format::Avif: return true;
    default: return false;
    }
}

namespace {

// sRGB's curve on a linear value (IEC 61966-2-1), its power part going on past 1; NaN as 0.
float srgb_curve(float v) {
    if (std::isnan(v)) return 0.0f;
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

CodecImage srgb_encoded(const CodecImage& linear) {
    std::vector<float> values = linear.normalized();
    const unsigned all = channels(linear.layout()), colours = colour_channels(linear.layout());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i % all < colours) values[i] = srgb_curve(values[i]);
    }
    CodecImage out = CodecImage::from_normalized(linear.width(), linear.height(), linear.layout(), Sample::F32, values);
    out.meta = linear.meta;
    return out;
}

Image eight_bit(const CodecImage& picture, bool linear) {
    const Size size{static_cast<int>(picture.width()), static_cast<int>(picture.height())};
    const Layout layout = picture.layout();
    CodecImage bytes;
    if (linear) {
        bytes = srgb_encoded(picture).convert(layout, Sample::U8);
    } else if (picture.sample() == Sample::U8) {
        bytes = picture;
    } else {
        bytes = picture.convert(layout, Sample::U8);
    }
    switch (layout) {
    case Layout::Gray: return Image::frombytes("L", size, bytes.data());
    case Layout::GrayA: return Image::frombytes("LA", size, bytes.data());
    case Layout::Rgb: return Image::frombytes("RGB", size, bytes.data());
    case Layout::Rgba: return Image::frombytes("RGBA", size, bytes.data());
    case Layout::Cmyk: return Image::frombytes("CMYK", size, bytes.data());
    case Layout::CmykA: {
        // (Pillow's plain CMYK → RGB — what is left of each colour after its ink and black — with the alpha kept)
        const std::string& d = bytes.data();
        std::string rgba(picture.pixel_count() * 4, '\0');
        for (std::size_t i = 0; i < picture.pixel_count(); ++i) {
            const auto k = static_cast<unsigned char>(d[i * 5 + 3]);
            for (unsigned c = 0; c < 3; ++c) rgba[i * 4 + c] = static_cast<char>(255 - std::min(255, static_cast<unsigned char>(d[i * 5 + c]) + k));
            rgba[i * 4 + 3] = d[i * 5 + 4];
        }
        return Image::frombytes("RGBA", size, rgba);
    }
    }
    return Image::frombytes("RGB", size, bytes.convert(Layout::Rgb, Sample::U8).data());
}

}  // namespace genko::render::codecs
