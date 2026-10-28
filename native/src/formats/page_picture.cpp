// A page or a layer as one picture at its depth (formats/page_picture.hpp).

#include "formats/page_picture.hpp"

#include <algorithm>
#include <cstring>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "formats/export.hpp"
#include "render/page.hpp"

namespace genko::formats {

namespace {

using render::codecs::CodecImage;
using render::codecs::Layout;
using render::codecs::Sample;

// The part of a page's picture (w × h) an export's area keeps, as the exports cut their values and inks.
render::Box area_box(const core::Page& page, std::string_view area, int dpi, int w, int h) {
    if (area == "paper") return {0, 0, w, h};
    if (area != "bleed" && area != "trim") throw core::Error("value", "the area is paper, bleed or trim");
    const core::Rect r = area == "bleed" ? page.bleed_rect_mm() : page.trim_rect_mm();
    const int x0 = std::clamp(render::mm_to_px(r.x.value(), dpi), 0, std::max(0, w - 1));
    const int y0 = std::clamp(render::mm_to_px(r.y.value(), dpi), 0, std::max(0, h - 1));
    const int x1 = std::clamp(x0 + render::mm_to_px(r.width.value(), dpi), x0 + 1, std::max(x0 + 1, w));
    const int y1 = std::clamp(y0 + render::mm_to_px(r.height.value(), dpi), y0 + 1, std::max(y0 + 1, h));
    return {x0, y0, x1, y1};
}

CodecImage cut(const CodecImage& image, const render::Box& box) {
    if (box.x0 == 0 && box.y0 == 0 && box.x1 == static_cast<int>(image.width()) && box.y1 == static_cast<int>(image.height())) return image;
    return image.crop(static_cast<std::uint32_t>(box.x0), static_cast<std::uint32_t>(box.y0), static_cast<std::uint32_t>(box.width()),
                      static_cast<std::uint32_t>(box.height()));
}

// An 8-bit picture of Pillow's ("RGB" or "RGBA") as the codecs'.
CodecImage from_image(const render::Image& image, bool alpha) {
    const render::Image rgb = image.mode() == (alpha ? "RGBA" : "RGB") ? image : image.convert(alpha ? "RGBA" : "RGB");
    return CodecImage::from_bytes(static_cast<std::uint32_t>(rgb.width()), static_cast<std::uint32_t>(rgb.height()), alpha ? Layout::Rgba : Layout::Rgb,
                                  Sample::U8, rgb.tobytes());
}

Sample sample_of(int depth) { return depth == 32 ? Sample::F32 : depth == 16 ? Sample::U16 : Sample::U8; }

// A grey or CMYK page's inks (with their alpha or without it) as the codecs' picture at their depth.
CodecImage from_inks(const core::InkPixels& inks, bool alpha) {
    const unsigned colours = inks.colours(), all = inks.channels();
    const bool cmyk = inks.space == core::InkSpace::Cmyk;
    const Layout layout = cmyk ? (alpha ? Layout::CmykA : Layout::Cmyk) : (alpha ? Layout::GrayA : Layout::Gray);
    const Sample sample = sample_of(inks.depth);
    const unsigned keep = colours + (alpha ? 1 : 0);
    CodecImage out = CodecImage::blank(inks.width, inks.height, layout, sample);
    char* to = out.data().data();
    for (std::size_t i = 0; i < inks.pixel_count(); ++i) {
        for (unsigned c = 0; c < keep; ++c) {
            const std::size_t at = i * keep + c, from = i * all + c;
            if (sample == Sample::U8) {
                to[at] = static_cast<char>(static_cast<unsigned char>(inks.samples[from]));
            } else if (sample == Sample::U16) {
                const std::uint16_t v = inks.samples[from];
                std::memcpy(to + at * 2, &v, 2);
            } else {
                const float v = inks.floats[from];
                std::memcpy(to + at * 4, &v, 4);
            }
        }
    }
    return out;
}

// GKCR values (straight RGBA, the page's profile's) as the codecs' picture at the page's depth.
CodecImage from_values(std::string_view values, int depth, bool alpha) {
    const core::ColorRasterView view(values);
    const Layout layout = alpha ? Layout::Rgba : Layout::Rgb;
    const unsigned keep = alpha ? 4 : 3;
    std::vector<float> samples(std::size_t(view.width()) * view.height() * keep);
    for (std::size_t i = 0; i < std::size_t(view.width()) * view.height(); ++i) {
        const std::array<double, 4> p = view.pixel(i);
        for (unsigned c = 0; c < keep; ++c) samples[i * keep + c] = static_cast<float>(p[c]);
    }
    return CodecImage::from_normalized(view.width(), view.height(), layout, sample_of(depth), samples);
}

// The ICC profile a page's picture carries: an RGB page of a colour model its own (or the built-in sRGB's), a grey or
// CMYK page its inks' profile.
std::optional<std::string> page_icc(const core::Page& page) {
    if (page.color) {
        if (page.color->space != core::InkSpace::Lab && page.color->icc && !page.color->icc->empty()) return *page.color->icc;
        return std::nullopt;
    }
    if (page.rgb && !page.rgb->is_default()) return std::string(detail::rgb_icc(*page.rgb));
    return std::nullopt;
}

// What a page's picture cannot keep of its colour mode (the screen's colours are written instead).
std::optional<std::string> shown_only(const core::Page& page) {
    if (page.color) {
        const std::string_view mode = core::mode_name(*page.color);
        if (mode == "lab") return "Lab のページは、画面の色（sRGB）にしました";
        if (mode == "bitmap") return "モノクロ 2 階調のページは、画面の色（sRGB）にしました";
        if (mode == "duotone") return "ダブルトーンのページは、画面の色（sRGB）にしました";
        if (mode == "multichannel") return "マルチチャンネルのページは、画面の色（sRGB）にしました";
    }
    if (page.rgb && page.rgb->table) return "インデックスカラーのページは、画面の色（sRGB）にしました";
    return std::nullopt;
}

}  // namespace

PagePicture page_picture(const core::Page& page, const core::Document& doc, int dpi, std::string_view area) {
    check_dpi(dpi);
    render::RenderOptions options;
    options.mode = "print";
    PagePicture out;
    if (const auto note = shown_only(page)) {
        const render::RenderResult made = render::render_page(page, dpi, options, &doc);
        out.image = from_image(made.image, false);
        out.notes.push_back(*note);
    } else if (page.color) {  // (a grey or CMYK page: its inks, as its TIFF is written)
        options.inks = true;
        render::RenderResult made = render::render_page(page, dpi, options, &doc);
        if (!made.inks) throw core::Error("value", "the page's inks were not drawn");
        out.image = from_inks(*made.inks, false);
    } else if (page.rgb && !page.rgb->is_default()) {  // (an RGB page of a colour model: its values, as its TIFF is written)
        const detail::PageValues made = detail::page_values(page, doc, dpi, options);
        out.image = from_values(made.values, page.rgb->depth, false);
    } else {
        out.image = from_image(render::render_page(page, dpi, options, &doc).image, false);
    }
    out.image.icc = shown_only(page) ? std::nullopt : page_icc(page);
    out.image.meta.dpi = std::make_pair(double(dpi), double(dpi));
    out.image = cut(out.image, area_box(page, area, dpi, static_cast<int>(out.image.width()), static_cast<int>(out.image.height())));
    return out;
}

std::pair<std::uint32_t, std::uint32_t> page_picture_size(const core::Page& page, int dpi, std::string_view area) {
    check_dpi(dpi);
    const int w = render::mm_to_px(page.spec.width_mm.value(), dpi), h = render::mm_to_px(page.spec.height_mm.value(), dpi);
    const render::Box box = area_box(page, area, dpi, std::max(1, w), std::max(1, h));
    return {static_cast<std::uint32_t>(std::max(1, box.width())), static_cast<std::uint32_t>(std::max(1, box.height()))};
}

PagePicture layer_picture(const core::Page& page, const core::Layer& layer, const core::Document& doc, int dpi) {
    check_dpi(dpi);
    const int w = render::mm_to_px(page.spec.width_mm.value(), dpi), h = render::mm_to_px(page.spec.height_mm.value(), dpi);
    const bool plain = layer.kind == core::LayerKind::Raster && layer.stroke_count() == 0 && (!layer.shapes || layer.shapes->items.empty()) &&
                       !(layer.mask && layer.mask->enabled) && !layer.vector_mask && !(layer.effect && !layer.effect->is_null()) &&
                       !(layer.screen && !layer.screen->is_null()) && !(layer.color && !layer.color->empty()) && !shown_only(page);
    PagePicture out;
    if (plain && layer.color_raster && page.rgb && !page.rgb->is_default() && !page.color) {
        const core::ColorRasterView view(*layer.color_raster);
        if (static_cast<int>(view.width()) == w && static_cast<int>(view.height()) == h) {
            out.image = from_values(*layer.color_raster, page.rgb->depth, true);
            out.image.icc = page_icc(page);
            out.image.meta.dpi = std::make_pair(double(dpi), double(dpi));
            return out;
        }
    }
    if (plain && layer.ink_raster && page.color && (page.color->space == core::InkSpace::Gray || page.color->space == core::InkSpace::Cmyk)) {
        const core::InkPixels inks = core::decode_ink_raster(*layer.ink_raster);
        if (static_cast<int>(inks.width) == w && static_cast<int>(inks.height) == h) {
            out.image = from_inks(inks, true);
            out.image.icc = page_icc(page);
            out.image.meta.dpi = std::make_pair(double(dpi), double(dpi));
            return out;
        }
    }
    out.image = from_image(render::layer_image(page, layer, dpi, &doc), true);
    out.image.meta.dpi = std::make_pair(double(dpi), double(dpi));
    if (page.color || (page.rgb && !page.rgb->is_default())) out.notes.push_back("このレイヤーは、画面の色（sRGB）の 8 bit にしました");
    return out;
}

}  // namespace genko::formats
