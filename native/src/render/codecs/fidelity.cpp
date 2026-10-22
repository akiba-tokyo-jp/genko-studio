// What a write keeps and loses (render/codecs/fidelity.hpp; PhotoCraft's crates/codecs/src/fidelity.rs).

#include "render/codecs/fidelity.hpp"

#include <algorithm>

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

    // the layout
    if (has_alpha(from_layout) && !has_alpha(p.layout) && image.has_translucency()) add(Loss::Kind::AlphaDiscarded);
    if (format == Format::Gif && has_alpha(p.layout) && image.has_translucency()) add(Loss::Kind::AlphaBinarized);
    if (is_cmyk(from_layout) && !is_cmyk(p.layout)) add(Loss::Kind::CmykConverted).layout = p.layout;
    if (!is_gray(from_layout) && is_gray(p.layout)) add(Loss::Kind::ColorToGray);

    // the compression
    if (format == Format::Gif) {
        add(Loss::Kind::PaletteQuantized);
    } else if (c.lossy) {
        add(Loss::Kind::LossyCompression);
    }

    // the metadata
    if (image.icc && options.embed_icc && (!c.icc || !same_model(from_layout, p.layout))) add(Loss::Kind::IccDropped);
    if (options.embed_metadata) {
        if (image.meta.exif && !c.exif) add(Loss::Kind::ExifDropped);
        if (image.meta.xmp && !c.xmp) add(Loss::Kind::XmpDropped);
        if (image.meta.dpi && !c.dpi) add(Loss::Kind::DpiDropped);
        if (!image.meta.text.empty() && !c.text) add(Loss::Kind::TextDropped);
    }
    return out;
}

}  // namespace genko::render::codecs
