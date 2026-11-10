// The codecs' formats (render/codecs/format.hpp; PhotoCraft's crates/codecs/src/format.rs).

#include "render/codecs/format.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace genko::render::codecs {

namespace {

using L = Layout;
using S = Sample;

constexpr std::array<Sample, 1> kU8{S::U8};
constexpr std::array<Sample, 2> kU8U16{S::U8, S::U16};
constexpr std::array<Sample, 3> kU8U16F32{S::U8, S::U16, S::F32};
constexpr std::array<Sample, 2> kF16F32{S::F16, S::F32};
constexpr std::array<Sample, 1> kF32{S::F32};
constexpr std::array<Layout, 1> kRgbOnly{L::Rgb};
constexpr std::array<Layout, 3> kJpegLayouts{L::Gray, L::Rgb, L::Cmyk};
constexpr std::array<Layout, 2> kRgbRgba{L::Rgb, L::Rgba};
constexpr std::array<Layout, 1> kRgbaOnly{L::Rgba};
constexpr std::array<Layout, 4> kRgbGray{L::Gray, L::GrayA, L::Rgb, L::Rgba};
constexpr std::array<Layout, 6> kAllLayouts{L::Gray, L::GrayA, L::Rgb, L::Rgba, L::Cmyk, L::CmykA};

constexpr std::array<std::string_view, 2> kPngExt{"png", "apng"};
constexpr std::array<std::string_view, 4> kJpegExt{"jpg", "jpeg", "jpe", "jfif"};
constexpr std::array<std::string_view, 2> kTiffExt{"tif", "tiff"};
constexpr std::array<std::string_view, 1> kWebpExt{"webp"};
constexpr std::array<std::string_view, 1> kGifExt{"gif"};
constexpr std::array<std::string_view, 2> kBmpExt{"bmp", "dib"};
constexpr std::array<std::string_view, 4> kTgaExt{"tga", "icb", "vda", "vst"};
constexpr std::array<std::string_view, 1> kIcoExt{"ico"};
constexpr std::array<std::string_view, 6> kPnmExt{"pnm", "pbm", "pgm", "ppm", "pam", "pfm"};
constexpr std::array<std::string_view, 1> kQoiExt{"qoi"};
constexpr std::array<std::string_view, 1> kExrExt{"exr"};
constexpr std::array<std::string_view, 1> kHdrExt{"hdr"};
constexpr std::array<std::string_view, 1> kAvifExt{"avif"};

// (PC4 F-a: every format is read and written; none left asymmetric)
constexpr std::array<Asymmetric, 0> kAsymmetric{};

FormatCaps base() {
    FormatCaps c;
    c.read = true;
    c.write = true;
    c.depths = kU8;
    c.layouts = kRgbRgba;
    c.alpha = true;
    return c;
}

bool starts(std::string_view bytes, std::string_view prefix) { return bytes.substr(0, prefix.size()) == prefix; }
unsigned u8(std::string_view b, std::size_t at) { return static_cast<unsigned char>(b[at]); }
unsigned le16(std::string_view b, std::size_t at) { return u8(b, at) | (u8(b, at + 1) << 8); }

bool is_avif_ftyp(std::string_view b) {
    const std::size_t size = (std::size_t(u8(b, 0)) << 24) | (u8(b, 1) << 16) | (u8(b, 2) << 8) | u8(b, 3);
    const std::size_t end = std::clamp<std::size_t>(size, 12, std::min<std::size_t>(b.size(), 64));
    for (std::size_t at = 8, i = 0; at + 4 <= end; at += 4, ++i) {
        if (i == 1) continue;  // (the minor version)
        const std::string_view brand = b.substr(at, 4);
        if (brand == "avif" || brand == "avis") return true;
    }
    return false;
}

bool looks_like_tga(std::string_view b) {
    if (b.size() < 18) return false;
    if (b.size() >= 26 && b.substr(b.size() - 18) == std::string_view("TRUEVISION-XFILE.\0", 18)) return true;
    const unsigned cmap_type = u8(b, 1), img_type = u8(b, 2), cmap_len = le16(b, 5), cmap_depth = u8(b, 7);
    const unsigned w = le16(b, 12), h = le16(b, 14), depth = u8(b, 16), desc = u8(b, 17);
    const bool type_ok = img_type == 1 || img_type == 2 || img_type == 3 || img_type == 9 || img_type == 10 || img_type == 11;
    const bool mapped = img_type == 1 || img_type == 9;
    bool cmap_ok = false;
    if (cmap_type == 0) cmap_ok = cmap_len == 0 && cmap_depth == 0 && !mapped;
    if (cmap_type == 1) cmap_ok = mapped && cmap_len > 0 && (cmap_depth == 15 || cmap_depth == 16 || cmap_depth == 24 || cmap_depth == 32);
    bool depth_ok = false;
    if (mapped) {
        depth_ok = depth == 8;
    } else if (img_type == 3 || img_type == 11) {
        depth_ok = depth == 8 || depth == 16;
    } else {
        depth_ok = depth == 15 || depth == 16 || depth == 24 || depth == 32;
    }
    return type_ok && cmap_ok && depth_ok && w > 0 && h > 0 && (desc & 0xC0) == 0;
}

}  // namespace

FormatCaps caps(Format format) {
    // (PC4 F-a: what each writer keeps — the plan and the losses come from these —, as PhotoCraft's table has it but for
    // what this build's own writers add: a BMP's resolution, a WebP written lossy too, AVIF read and written with its
    // profile, EXIF and XMP)
    FormatCaps c = base();
    switch (format) {
    case Format::Png:
        c.depths = kU8U16;
        c.layouts = kRgbGray;
        c.icc = c.exif = c.xmp = c.dpi = c.text = true;
        c.animation = true;
        break;
    case Format::Jpeg:
        c.layouts = kJpegLayouts;
        c.alpha = false;
        c.icc = c.exif = c.xmp = c.dpi = true;
        c.lossy = true;
        break;
    case Format::Tiff:
        c.depths = kU8U16F32;
        c.layouts = kAllLayouts;
        c.icc = c.xmp = c.dpi = c.text = true;
        break;
    case Format::WebP:
        c.icc = c.exif = c.xmp = true;
        c.animation = true;
        break;
    case Format::Gif:
        c.layouts = kRgbaOnly;
        c.animation = true;
        c.lossy = true;
        break;
    case Format::Bmp:
        c.dpi = true;
        break;
    case Format::Tga:
        c.layouts = kRgbGray;
        break;
    case Format::Ico:
        c.layouts = kRgbaOnly;
        break;
    case Format::Pnm:
        c.depths = kU8U16F32;
        c.layouts = kAllLayouts;
        break;
    case Format::Qoi:
        break;
    case Format::OpenExr:
        c.depths = kF16F32;
        c.layouts = kRgbGray;
        break;
    case Format::Hdr:
        c.depths = kF32;
        c.layouts = kRgbOnly;
        c.alpha = false;
        c.lossy = true;
        break;
    case Format::Avif:
        c.icc = c.exif = c.xmp = true;
        c.animation = true;
        c.lossy = true;
        break;
    }
    return c;
}

bool enabled(Format format) {
    const FormatCaps c = caps(format);
    return c.read || c.write;
}

std::string_view name(Format format) {
    switch (format) {
    case Format::Png: return "PNG";
    case Format::Jpeg: return "JPEG";
    case Format::Tiff: return "TIFF";
    case Format::WebP: return "WebP";
    case Format::Gif: return "GIF";
    case Format::Bmp: return "BMP";
    case Format::Tga: return "TGA";
    case Format::Ico: return "ICO";
    case Format::Pnm: return "Netpbm";
    case Format::Qoi: return "QOI";
    case Format::OpenExr: return "OpenEXR";
    case Format::Hdr: return "Radiance HDR";
    case Format::Avif: return "AVIF";
    }
    return "";
}

std::string_view id_of(Format format) {
    switch (format) {
    case Format::Png: return "png";
    case Format::Jpeg: return "jpeg";
    case Format::Tiff: return "tiff";
    case Format::WebP: return "webp";
    case Format::Gif: return "gif";
    case Format::Bmp: return "bmp";
    case Format::Tga: return "tga";
    case Format::Ico: return "ico";
    case Format::Pnm: return "pnm";
    case Format::Qoi: return "qoi";
    case Format::OpenExr: return "exr";
    case Format::Hdr: return "hdr";
    case Format::Avif: return "avif";
    }
    return "";
}

std::optional<Format> from_id(std::string_view id) {
    for (const Format f : kFormats) {
        if (id_of(f) == id) return f;
    }
    return std::nullopt;
}

std::span<const std::string_view> extensions(Format format) {
    switch (format) {
    case Format::Png: return kPngExt;
    case Format::Jpeg: return kJpegExt;
    case Format::Tiff: return kTiffExt;
    case Format::WebP: return kWebpExt;
    case Format::Gif: return kGifExt;
    case Format::Bmp: return kBmpExt;
    case Format::Tga: return kTgaExt;
    case Format::Ico: return kIcoExt;
    case Format::Pnm: return kPnmExt;
    case Format::Qoi: return kQoiExt;
    case Format::OpenExr: return kExrExt;
    case Format::Hdr: return kHdrExt;
    case Format::Avif: return kAvifExt;
    }
    return {};
}

std::string_view mime_type(Format format) {
    switch (format) {
    case Format::Png: return "image/png";
    case Format::Jpeg: return "image/jpeg";
    case Format::Tiff: return "image/tiff";
    case Format::WebP: return "image/webp";
    case Format::Gif: return "image/gif";
    case Format::Bmp: return "image/bmp";
    case Format::Tga: return "image/x-tga";
    case Format::Ico: return "image/x-icon";
    case Format::Pnm: return "image/x-portable-anymap";
    case Format::Qoi: return "image/qoi";
    case Format::OpenExr: return "image/x-exr";
    case Format::Hdr: return "image/vnd.radiance";
    case Format::Avif: return "image/avif";
    }
    return "";
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> max_dimensions(Format format) {
    switch (format) {
    case Format::Jpeg: return std::make_pair(65500u, 65500u);  // (libjpeg's JPEG_MAX_DIMENSION)
    case Format::Gif:
    case Format::Tga: return std::make_pair(65535u, 65535u);
    case Format::WebP: return std::make_pair(16383u, 16383u);
    case Format::Ico: return std::make_pair(256u, 256u);
    case Format::Avif: return std::make_pair(65536u, 65536u);
    default: return std::nullopt;
    }
}

std::optional<Format> detect(std::string_view b) {
    if (starts(b, std::string_view("\x89PNG\r\n\x1a\n", 8))) return Format::Png;
    if (starts(b, "\xFF\xD8\xFF")) return Format::Jpeg;
    if (starts(b, std::string_view("II*\0", 4)) || starts(b, std::string_view("MM\0*", 4)) || starts(b, std::string_view("II+\0", 4)) ||
        starts(b, std::string_view("MM\0+", 4)))
        return Format::Tiff;
    if (b.size() >= 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP") return Format::WebP;
    if (starts(b, "GIF87a") || starts(b, "GIF89a")) return Format::Gif;
    if (starts(b, "qoif")) return Format::Qoi;
    if (starts(b, "\x76\x2F\x31\x01")) return Format::OpenExr;
    if (starts(b, "#?RADIANCE") || starts(b, "#?RGBE")) return Format::Hdr;
    if (b.size() >= 12 && b.substr(4, 4) == "ftyp" && is_avif_ftyp(b)) return Format::Avif;
    if (b.size() >= 14 && starts(b, "BM")) return Format::Bmp;
    if (b.size() >= 6 && starts(b, std::string_view("\0\0\1\0", 4)) && (b[4] != 0 || b[5] != 0)) return Format::Ico;
    if (b.size() >= 3 && b[0] == 'P' && ((b[1] >= '1' && b[1] <= '7') || b[1] == 'F' || b[1] == 'f') &&
        (b[2] == ' ' || b[2] == '\t' || b[2] == '\n' || b[2] == '\r' || b[2] == '\f'))
        return Format::Pnm;
    if (looks_like_tga(b)) return Format::Tga;
    return std::nullopt;
}

std::optional<Format> from_extension(std::string_view extension) {
    const std::size_t cut = extension.find_last_of("./\\");
    std::string ext(cut == std::string_view::npos ? extension : extension.substr(cut + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const Format f : kFormats) {
        const auto list = extensions(f);
        if (std::find(list.begin(), list.end(), ext) != list.end()) return f;
    }
    return std::nullopt;
}

std::span<const Asymmetric> asymmetric_formats() { return kAsymmetric; }

}  // namespace genko::render::codecs
