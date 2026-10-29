// PC5 G-0 (type@1, render/type_layer.hpp): a type layer's picture.

#include "render/type_layer.hpp"

#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/type_text.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_smart_parts.hpp"
#include "render/page_internal.hpp"
#include "render/text/balloons.hpp"
#include "render/text/lettering.hpp"

namespace genko::render::type_layer {

namespace {

constexpr int kFindDpi = 50;           // (where the letters are: the page drawn this coarsely first)
constexpr double kFindMarginMm = 3.0;  // (round what the coarse drawing found: what it may miss of thin marks and halos)

text::Panels panels_of(const core::Page& page) {
    text::Panels panels;
    for (const core::Frame* f : page.leaf_frames())
        panels.emplace_back(f->id, std::array<double, 4>{f->rect.x.value(), f->rect.y.value(), f->rect.width.value(), f->rect.height.value()});
    return panels;
}

// The record drawn over a part of the page (its corner `origin`, its size `size`) as a layer's picture ("RGBA").
Image drawn(const detail::Ctx& ctx, const core::StoryLine& line, Point origin, Size size, Size page, int dpi) {
    const std::vector<const core::StoryLine*> lines{&line};
    const text::Panels panels = panels_of(*ctx.page);
    const std::optional<std::string> font_path = ctx.episode != nullptr ? std::optional<std::string>(ctx.episode->font_path) : std::nullopt;
    const text::Unported unported = [&ctx](const NotYetPorted& e) { detail::skip_unported(ctx, e.element()); };
    const text::TextGamma plain(std::nullopt);  // (the layer's own picture: no page under it to mix with)
    return smart_ops::drawn_alone(size, [&](Image& on) {
        Image rgba = on.mode() == "RGBA" ? std::move(on) : on.convert("RGBA");
        text::draw_lines(text::PagePart{&rgba, origin, page}, lines, dpi, font_path, false, &panels, ctx.stop, &unported);
        on = std::move(rgba);
    });
}

// Where the record's letters and balloon lie on the page (mm: x0, y0, x1, y1; none: nothing drawn), found by drawing the
// page coarsely, and remembered for the record (the same record object: an op makes a new one) and the page's paper.
std::optional<std::array<double, 4>> extent_mm(const detail::Ctx& ctx, const core::Layer& layer, const core::StoryLine& line) {
    struct Kept {
        std::weak_ptr<const core::StoryLine> record;
        double w = 0, h = 0;
        std::string font;
        std::string page;
        std::optional<std::array<double, 4>> extent;
    };
    static std::mutex mutex;
    static std::map<const core::StoryLine*, Kept> kept;
    const double w = ctx.page->spec.width_mm.value(), h = ctx.page->spec.height_mm.value();
    const std::string font = ctx.episode != nullptr ? ctx.episode->font_path : std::string();
    const std::string page_id = ctx.page->id;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = kept.find(layer.type.get()); it != kept.end()) {
            if (it->second.record.lock() == layer.type && it->second.w == w && it->second.h == h && it->second.font == font && it->second.page == page_id)
                return it->second.extent;
        }
    }
    const Size page{std::max(1, static_cast<int>(std::lround(w / 25.4 * kFindDpi))), std::max(1, static_cast<int>(std::lround(h / 25.4 * kFindDpi)))};
    const Image coarse = drawn(ctx, line, Point{0, 0}, page, page, kFindDpi);
    std::optional<std::array<double, 4>> extent;
    if (const std::optional<Box> b = coarse.getbbox()) {
        const double k = 25.4 / kFindDpi;
        // (with its box: what the finer drawing sets there, whatever the coarse one made of it)
        extent = std::array<double, 4>{std::min(b->x0 * k, line.x_mm.value()) - kFindMarginMm, std::min(b->y0 * k, line.y_mm.value()) - kFindMarginMm,
                                       std::max(b->x1 * k, line.x_mm.value() + line.w_mm.value()) + kFindMarginMm,
                                       std::max(b->y1 * k, line.y_mm.value() + line.h_mm.value()) + kFindMarginMm};
    }
    const std::lock_guard<std::mutex> lock(mutex);
    if (kept.size() > 512) {  // (records no longer held: let go)
        std::erase_if(kept, [](const auto& item) { return item.second.record.expired(); });
        if (kept.size() > 512) kept.clear();
    }
    kept[layer.type.get()] = Kept{layer.type, w, h, font, page_id, extent};
    return extent;
}

}  // namespace

std::optional<Image> picture_8bit(const detail::Ctx& ctx, const core::Layer& layer, const Box& area) {
    if (!layer.type || ctx.page == nullptr || area.width() <= 0 || area.height() <= 0) return std::nullopt;
    const core::StoryLine line = core::type_text::line_of(layer, ctx.page->index);
    // (only where its letters are: the part of `area` they reach drawn, the rest of the picture clear — the same pixels as
    // the whole drawn and cut there)
    const std::optional<std::array<double, 4>> extent = extent_mm(ctx, layer, line);
    if (!extent) return std::nullopt;
    const double px = ctx.dpi / 25.4;
    const Box reach{std::max(area.x0, static_cast<int>(std::floor((*extent)[0] * px))), std::max(area.y0, static_cast<int>(std::floor((*extent)[1] * px))),
                    std::min(area.x1, static_cast<int>(std::ceil((*extent)[2] * px))), std::min(area.y1, static_cast<int>(std::ceil((*extent)[3] * px)))};
    if (reach.width() <= 0 || reach.height() <= 0) return std::nullopt;
    const Image part = drawn(ctx, line, Point{reach.x0, reach.y0}, Size{reach.width(), reach.height()}, ctx.size, ctx.dpi);
    if (!part.getbbox()) return std::nullopt;
    if (reach == area) return part;
    Image picture = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
    picture.paste(part, Point{reach.x0 - area.x0, reach.y0 - area.y0});
    return picture;
}

}  // namespace genko::render::type_layer
