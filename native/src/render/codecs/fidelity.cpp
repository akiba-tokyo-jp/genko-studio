// What a write keeps and loses (render/codecs/fidelity.hpp; PhotoCraft's crates/codecs/src/fidelity.rs).

#include "render/codecs/fidelity.hpp"

#include <algorithm>
#include <set>

#include "core/color_model.hpp"
#include "render/codecs/codecs.hpp"
#include "render/codecs/formats.hpp"

namespace genko::render::codecs {

namespace {

bool holds(std::span<const Sample> list, Sample s) { return std::find(list.begin(), list.end(), s) != list.end(); }
bool holds(std::span<const Layout> list, Layout l) { return std::find(list.begin(), list.end(), l) != list.end(); }

Sample pick_sample(Sample source, std::span<const Sample> depths) {
    if (holds(depths, source)) return source;
    // (integers widen into floats without a loss: a float holds every 8- and 16-bit value)
    std::array<Sample, 3> order{};
    switch (source) {
    case Sample::U8: order = {Sample::U16, Sample::F32, Sample::F16}; break;
    case Sample::U16: order = {Sample::F32, Sample::U8, Sample::F16}; break;
    case Sample::F16: order = {Sample::F32, Sample::U16, Sample::U8}; break;
    case Sample::F32: order = {Sample::F16, Sample::U16, Sample::U8}; break;
    }
    for (const Sample s : order) {
        if (holds(depths, s)) return s;
    }
    return depths.empty() ? Sample::U8 : depths.front();
}

Layout pick_layout(Layout source, std::span<const Layout> layouts, bool alpha) {
    Layout candidate = alpha ? source : without_alpha(source);
    if (holds(layouts, candidate)) return candidate;
    if (is_cmyk(candidate) || is_gray(candidate)) candidate = has_alpha(candidate) ? Layout::Rgba : Layout::Rgb;
    if (holds(layouts, candidate)) return candidate;
    if (holds(layouts, with_alpha(candidate))) return with_alpha(candidate);
    if (holds(layouts, without_alpha(candidate))) return without_alpha(candidate);
    return layouts.empty() ? Layout::Rgb : layouts.front();
}

std::string_view depth_words(Sample s) {
    switch (s) {
    case Sample::U8: return "8 bit";
    case Sample::U16: return "16 bit";
    case Sample::F16: return "16 bit（浮動小数点）";
    case Sample::F32: return "32 bit（浮動小数点）";
    }
    return "";
}

// The picture's profile is the one the format implies (OpenEXR's and Radiance HDR's linear light: the built-in linear-srgb).
bool implied_profile(const CodecImage& image, Format format) {
    if (!image.icc || !linear_light(format)) return false;
    const core::BuiltinProfile* p = core::builtin_profile("linear-srgb");
    return p != nullptr && p->bytes && *p->bytes == *image.icc;
}

// (GIF) The opaque colours of the picture as it is written — more than half transparent is transparent —, and whether one
// is partly transparent.
struct GifColours {
    std::size_t colours = 0;
    bool transparent = false;
    bool partial = false;
};
GifColours gif_colours(const CodecImage& stored) {
    GifColours out;
    std::set<std::uint32_t> seen;
    const auto* d = reinterpret_cast<const unsigned char*>(stored.data().data());
    for (std::size_t i = 0; i < stored.pixel_count(); ++i) {
        const unsigned char* p = d + i * 4;
        if (p[3] != 0 && p[3] != 255) out.partial = true;
        if (p[3] < 128) {
            out.transparent = true;
            continue;
        }
        if (seen.size() <= 256) seen.insert((std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2]);
    }
    out.colours = seen.size();
    return out;
}

std::string_view layout_words(Layout l) {
    if (is_gray(l)) return "グレースケール";
    if (is_cmyk(l)) return "CMYK";
    return "RGB";
}

}  // namespace

std::string Loss::text() const {
    const std::string fmt(name(format));
    switch (kind) {
    case Kind::WriteUnsupported: return "この版の Genko では、" + fmt + " を書けません";
    case Kind::DepthReduced: return std::string(depth_words(from)) + " を " + std::string(depth_words(to)) + " にします";
    case Kind::RangeClipped: return "0〜1 の外の値（HDR の明るさ）を切ります";
    case Kind::AlphaDiscarded: return fmt + " は透明を持てないため、透明を捨てます";
    case Kind::AlphaBinarized: return "半透明を、透明か不透明のどちらかにします";
    case Kind::CmykConverted: return "CMYK を " + std::string(layout_words(layout)) + " に変換します";
    case Kind::ColorToGray: return "色をグレースケールにします";
    case Kind::PaletteQuantized: return "256 色に減色します";
    case Kind::LossyCompression: return fmt + " は非可逆の圧縮です（細部が少し変わります）";
    case Kind::IccDropped: return "ICC プロファイルを書けないため、捨てます";
    case Kind::ExifDropped: return "EXIF を書けないため、捨てます";
    case Kind::XmpDropped: return "XMP（ファイル情報）を書けないため、捨てます";
    case Kind::DpiDropped: return "解像度（dpi）を書けないため、捨てます";
    case Kind::TextDropped: return "文字の情報を書けないため、捨てます";
    case Kind::DimensionsExceeded:
        return "画像が " + fmt + " の大きさの上限（" + std::to_string(max_width) + "×" + std::to_string(max_height) + " px）を超えるため、書けません";
    }
    return {};
}

Plan plan(const CodecImage& image, Format format, const EncodeOptions&) {
    const FormatCaps c = caps(format);
    if (format == Format::Pnm && is_float(image.sample())) {
        // (Netpbm: integers in any layout (PAM); floats as PFM, which holds grey and RGB only)
        static constexpr std::array<Layout, 2> kPfm{Layout::Gray, Layout::Rgb};
        return Plan{pick_layout(image.layout(), kPfm, false), Sample::F32};
    }
    return Plan{pick_layout(image.layout(), c.layouts, c.alpha), pick_sample(image.sample(), c.depths)};
}

std::vector<Loss> losses(const CodecImage& image, Format format, const EncodeOptions& options) {
    std::vector<Loss> out;
    const FormatCaps c = caps(format);
    const auto add = [&](Loss::Kind kind) -> Loss& {
        Loss loss;
        loss.kind = kind;
        loss.format = format;
        out.push_back(loss);
        return out.back();
    };
    if (!c.write) {
        add(Loss::Kind::WriteUnsupported);
        return out;
    }
    if (const auto most = max_dimensions(format); most && (image.width() > most->first || image.height() > most->second)) {
        Loss& loss = add(Loss::Kind::DimensionsExceeded);
        loss.max_width = most->first;
        loss.max_height = most->second;
    }
    const Plan p = plan(image, format, options);
    const Layout from_layout = image.layout();
    const Sample from_sample = image.sample();

    // the depth
    bool lower = from_sample != p.sample;
    if (from_sample == Sample::U8 || (from_sample == Sample::U16 && p.sample == Sample::F32) || (from_sample == Sample::F16 && p.sample == Sample::F32))
        lower = false;
    if (lower) {
        Loss& loss = add(Loss::Kind::DepthReduced);
        loss.from = from_sample;
        loss.to = p.sample;
    }
    if (is_float(from_sample) && !is_float(p.sample) && image.has_out_of_range()) add(Loss::Kind::RangeClipped);

    // the layout (GIF: an alpha of all or nothing — a pixel more transparent than half one transparent colour —, its colours
    // as they are when they are 256 or fewer)
    if (has_alpha(from_layout) && !has_alpha(p.layout) && image.has_translucency()) add(Loss::Kind::AlphaDiscarded);
    std::optional<GifColours> gif;
    if (format == Format::Gif) gif = gif_colours(image.convert(p.layout, p.sample));
    if (gif && gif->partial) add(Loss::Kind::AlphaBinarized);
    if (is_cmyk(from_layout) && !is_cmyk(p.layout)) add(Loss::Kind::CmykConverted).layout = p.layout;
    if (!is_gray(from_layout) && is_gray(p.layout)) add(Loss::Kind::ColorToGray);

    // the compression
    if (gif) {
        if (gif->colours > (gif->transparent ? 255u : 256u)) add(Loss::Kind::PaletteQuantized);
    } else if (c.lossy || (format == Format::WebP && !options.webp_lossless)) {
        add(Loss::Kind::LossyCompression);
    }

    // the metadata (JPEG: each in one segment — an EXIF block or an XMP packet past it, a profile past 255 of them, not
    // written —; PNG: text under a keyword of 1 to 79 Latin-1 characters; TIFF: text of its seven tags)
    const bool icc_kept = c.icc && same_model(from_layout, p.layout) && !(format == Format::Jpeg && image.icc && image.icc->size() > detail::kJpegIccMax);
    if (image.icc && options.embed_icc && !icc_kept && !implied_profile(image, format)) add(Loss::Kind::IccDropped);
    if (options.embed_metadata) {
        if (image.meta.exif && (!c.exif || (format == Format::Jpeg && image.meta.exif->size() > detail::kJpegExifMax))) add(Loss::Kind::ExifDropped);
        if (image.meta.xmp && (!c.xmp || (format == Format::Jpeg && image.meta.xmp->size() > detail::kJpegXmpMax))) add(Loss::Kind::XmpDropped);
        if (image.meta.dpi && !c.dpi) add(Loss::Kind::DpiDropped);
        const bool text_kept = c.text && std::all_of(image.meta.text.begin(), image.meta.text.end(), [format](const auto& kv) {
            return format == Format::Png ? detail::png_keyword(kv.first).has_value() : detail::tiff_text_tag(kv.first).has_value();
        });
        if (!image.meta.text.empty() && !text_kept) add(Loss::Kind::TextDropped);
    }
    return out;
}

}  // namespace genko::render::codecs
