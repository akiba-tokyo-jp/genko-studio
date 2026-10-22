// A file opened as a book of one page (formats/open_file.hpp).

#include "formats/open_file.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/smart.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"

namespace genko::formats {

namespace {

using render::codecs::CodecImage;
using render::codecs::Layout;
using render::codecs::Sample;

// The name without its folder; its stem (the name less its last extension).
std::string base_name(std::string_view name) {
    const std::size_t cut = name.find_last_of("/\\");
    return std::string(cut == std::string_view::npos ? name : name.substr(cut + 1));
}
std::string stem_of(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}
std::string suffix_of(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return {};
    std::string suffix = name.substr(dot);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return suffix;
}

// A profile the file carries, of the picture's colours (none: it carries none, or one of other colours — said in the notes).
std::optional<std::string> own_profile(const CodecImage& picture, const OpenOptions& options, std::vector<std::string>& notes) {
    if (!picture.icc || picture.icc->empty() || options.profile != "keep") return std::nullopt;
    const auto space = render::ink::profile_space(*picture.icc);
    const bool fits = space && ((*space == core::ProfileSpace::Gray && render::codecs::is_gray(picture.layout())) ||
                                (*space == core::ProfileSpace::Rgb && render::codecs::is_rgb(picture.layout())) ||
                                (*space == core::ProfileSpace::Cmyk && render::codecs::is_cmyk(picture.layout())));
    if (!fits) {
        notes.push_back("埋め込みプロファイルが画像の色と合わないため、作業用のプロファイルにしました");
        return std::nullopt;
    }
    return picture.icc;
}

std::string builtin_bytes(std::string_view id) {
    const core::BuiltinProfile* builtin = core::builtin_profile(id);
    if (builtin == nullptr || !builtin->bytes) throw core::Error("value", "the built-in profile " + std::string(id) + " is not in this build");
    return *builtin->bytes;
}

core::PageColor ink_mode(core::InkSpace space, int depth, const std::string& icc) {
    core::PageColor color;
    color.space = space;
    color.depth = depth;
    color.icc = std::make_shared<const std::string>(icc);
    color.profile = core::bytes_ref(icc);
    return color;
}

}  // namespace

Opened picture_book(CodecImage picture, std::string_view title, const OpenOptions& options) {
    if (picture.empty()) throw core::Error("value", "a picture of no pixels cannot be opened");
    options.limits.check(picture.width(), picture.height(), picture.layout(), picture.sample());
    Opened out;
    const std::uint32_t w = picture.width(), h = picture.height();
    const bool alpha = render::codecs::has_alpha(picture.layout());
    const bool gray = render::codecs::is_gray(picture.layout()), cmyk = render::codecs::is_cmyk(picture.layout());
    const bool eight = picture.sample() == Sample::U8;
    // (its resolution: the file's, 72 ppi when it says none or less, as Photoshop opens it)
    const double said = picture.meta.dpi ? picture.meta.dpi->first : 0.0;
    const auto dpi = static_cast<std::int64_t>(std::clamp<double>(std::llround(said), 72, 100000));

    core::PageSpec spec;
    spec.width_mm = core::Num(static_cast<double>(w) * 25.4 / static_cast<double>(dpi));
    spec.height_mm = core::Num(static_cast<double>(h) * 25.4 / static_cast<double>(dpi));
    spec.dpi = core::Num(dpi);
    spec.bleed_mm = core::Num(std::int64_t{0});
    spec.inner_margin_mm = core::Num(std::int64_t{0});
    spec.trim_w_mm = spec.width_mm;
    spec.trim_h_mm = spec.height_mm;
    // (the margins as the reader reads them back: floats)
    spec.margins_mm = core::NumList{core::Num(0.0), core::Num(0.0), core::Num(0.0), core::Num(0.0)};
    spec.expression = gray ? "mono" : "color";
    if (eight && !gray && !cmyk) spec.grid = "finest";  // (image-grid@1: design 3, 8-2 (b))

    core::Document doc = core::new_episode(title.empty() ? std::string("無題") : std::string(title), core::Num(std::int64_t{1}), 1, spec,
                                           core::Binding::Left);
    core::Page& page = doc.edit_page(0);
    page.frames.clear();
    page.numero = false;
    core::Layer layer;
    layer.id = core::new_id();
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.title = alpha ? "レイヤー 0" : "背景";
    layer.exportable = true;
    layer.panel_clip = false;
    layer.panel_each = false;

    const std::optional<std::string> own = own_profile(picture, options, out.notes);
    if (gray || cmyk) {
        // (a page of inks: grey 8, 16 or 32 bits — a half float's values as floats —, CMYK 8, 16 or 32)
        const int depth = picture.sample() == Sample::U8 ? 8 : picture.sample() == Sample::U16 ? 16 : 32;
        const std::string icc = own ? *own
                                    : gray ? (options.working_gray.empty() ? builtin_bytes("sgray") : options.working_gray)
                                           : (options.working_cmyk.empty() ? builtin_bytes("coated-cmyk") : options.working_cmyk);
        const core::PageColor color = ink_mode(gray ? core::InkSpace::Gray : core::InkSpace::Cmyk, depth, icc);
        core::InkPixels inks = core::blank_ink_pixels(color.space, depth, w, h);
        const unsigned colours = render::codecs::colour_channels(picture.layout()), all = render::codecs::channels(picture.layout());
        for (std::size_t i = 0; i < inks.pixel_count(); ++i) {
            for (unsigned c = 0; c < colours; ++c) inks.set(i, c, picture.value(i * all + c));
            inks.set(i, colours, alpha ? picture.value(i * all + colours) : 1.0);
        }
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(inks));
        page.color = color;
        doc.color = color;
    } else {
        std::optional<core::RgbMode> mode;
        const int depth = picture.sample() == Sample::U8 ? 8 : picture.sample() == Sample::U16 ? 16 : 32;
        const bool srgb = own && core::icc_description(*own).starts_with("sRGB");  // (an sRGB profile: the 8-bit page's own)
        const std::string icc = own && !srgb ? *own : options.working_rgb;
        if (depth != 8 || !icc.empty()) {
            mode = core::RgbMode{};
            mode->depth = depth;
            if (!icc.empty()) {
                mode->icc = std::make_shared<const std::string>(icc);
                mode->profile = core::bytes_ref(icc);
            }
        }
        if (depth == 8) {
            const CodecImage rgba = picture.convert(Layout::Rgba, Sample::U8);
            const render::Image image = render::Image::frombytes("RGBA", render::Size{static_cast<int>(w), static_cast<int>(h)}, rgba.data());
            layer.raster_png = std::make_shared<const std::string>(render::write_png(image));
        } else {
            const unsigned all = render::codecs::channels(picture.layout());
            layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(w, h, depth == 16 ? "u16" : "f32", [&](std::size_t i) {
                return std::array<double, 4>{picture.value(i * all), picture.value(i * all + 1), picture.value(i * all + 2),
                                             alpha ? std::clamp(static_cast<double>(picture.value(i * all + 3)), 0.0, 1.0) : 1.0};
            }));
        }
        page.rgb = mode;
        doc.rgb = mode;
    }
    page.layers = {std::move(layer)};
    out.doc = std::move(doc);
    return out;
}

Opened open_file(std::string_view bytes, std::string_view name, const OpenOptions& options) {
    const std::string file = base_name(name);
    const std::string suffix = suffix_of(file);
    // (F-d: a PSD or PSB as a book of its layers; F-e: a camera RAW file through Camera Raw — a TIFF-shaped RAW is not read
    // as its TIFF —; F-f: a .pcraft)
    if (bytes.substr(0, 4) == "8BPS") throw render::NotYetPorted("image_format");
    if (const auto later = core::smart::later_format_of_suffix(suffix); later && *later == "raw") throw render::NotYetPorted("image_format");
    if (suffix == ".pcraft") throw render::NotYetPorted("image_format");
    const auto format = options.as ? options.as : render::codecs::detect(bytes);
    if (!format) throw core::Error("unidentified_image", "the file is not a picture of a format this build knows");
    CodecImage picture = render::codecs::decode_as(*format, bytes, options.limits);
    const std::string mode(render::codecs::to_string(picture.layout()));
    const int depth = render::codecs::depth_of(picture.sample());
    core::NativeSource source;
    source.format = std::string(render::codecs::id_of(*format));
    source.name = file.empty() ? std::string("image") : file;
    source.width_px = picture.width();
    source.height_px = picture.height();
    source.dpi = picture.meta.dpi;
    source.depth = depth;
    source.mode = mode;
    Opened out = picture_book(std::move(picture), stem_of(file), options);
    out.doc.edit_page(0).native_source = std::move(source);
    return out;
}

}  // namespace genko::formats
