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

#include "core/error.hpp"
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
constexpr std::int64_t kMostPixels = 120'000'000;  // (an upright picture to turn: op_limits' most pixels)
constexpr double kReachMm = 60.0;  // (G-a: how far round its box its letters are looked for, off the page too)

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
    // (PC5 G-a: the book's text gamma, as its lines are drawn — the picture drawn on black and on white in it, so its
    // cover is what that gamma makes of the letters' edges there: a dark text over a light page mixed as a line's)
    const text::TextGamma gamma(ctx.episode != nullptr ? ctx.episode->text_gamma : text::TextGamma::current());
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
    // (the page, and round the record's box: a type layer its matrix turns may be set off the page it is shown on — G-a)
    const double x0 = std::min(0.0, line.x_mm.value() - kReachMm), y0 = std::min(0.0, line.y_mm.value() - kReachMm);
    const double x1 = std::max(w, line.x_mm.value() + line.w_mm.value() + kReachMm), y1 = std::max(h, line.y_mm.value() + line.h_mm.value() + kReachMm);
    const auto coarse_px = [](double mm) { return static_cast<int>(std::floor(mm / 25.4 * kFindDpi)); };
    const Point origin{coarse_px(x0), coarse_px(y0)};
    const Size page{std::max(1, coarse_px(x1) + 1), std::max(1, coarse_px(y1) + 1)};
    const Size size{std::max(1, page.width - origin.x), std::max(1, page.height - origin.y)};
    const Image coarse = drawn(ctx, line, origin, size, page, kFindDpi);
    std::optional<std::array<double, 4>> extent;
    if (std::optional<Box> b = coarse.getbbox()) {
        *b = Box{b->x0 + origin.x, b->y0 + origin.y, b->x1 + origin.x, b->y1 + origin.y};
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

// PC5 G-a: a type layer turned, scaled or bent by its matrix (native_type.transform, the page's mm to the page's mm):
// its letters drawn upright over their extent, then mapped through it (bicubic, its colours premultiplied) to where
// the matrix puts them; the part of `area` they reach.
std::optional<Image> turned(const detail::Ctx& ctx, const core::StoryLine& line, const std::array<double, 6>& t, const std::array<double, 4>& extent,
                            const Box& area) {
    const double k = ctx.dpi / 25.4;
    const double det = t[0] * t[3] - t[1] * t[2];
    if (!(std::fabs(det) > 1e-12)) return std::nullopt;
    const Box from{static_cast<int>(std::floor(extent[0] * k)), static_cast<int>(std::floor(extent[1] * k)), static_cast<int>(std::ceil(extent[2] * k)),
                   static_cast<int>(std::ceil(extent[3] * k))};
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto& [x, y] : {std::pair{extent[0], extent[1]}, {extent[2], extent[1]}, {extent[0], extent[3]}, {extent[2], extent[3]}}) {
        const double u = (t[0] * x + t[2] * y + t[4]) * k, v = (t[1] * x + t[3] * y + t[5]) * k;
        x0 = std::min(x0, u);
        y0 = std::min(y0, v);
        x1 = std::max(x1, u);
        y1 = std::max(y1, v);
    }
    const Box reach{std::max(area.x0, static_cast<int>(std::floor(std::max(x0, -1e9)))), std::max(area.y0, static_cast<int>(std::floor(std::max(y0, -1e9)))),
                    std::min(area.x1, static_cast<int>(std::ceil(std::min(x1, 1e9)))), std::min(area.y1, static_cast<int>(std::ceil(std::min(y1, 1e9))))};
    if (reach.width() <= 0 || reach.height() <= 0 || from.width() <= 0 || from.height() <= 0) return std::nullopt;
    if (static_cast<std::int64_t>(from.width()) * from.height() > kMostPixels)
        throw core::Error("image_too_large", "the type layer is too large to turn at this resolution");
    const Image upright = drawn(ctx, line, Point{from.x0, from.y0}, Size{from.width(), from.height()}, ctx.size, ctx.dpi);
    if (!upright.getbbox()) return std::nullopt;
    // (a pixel of `reach` (u, v) → the page's px → mm → back through the matrix → the upright picture's px)
    const double ex = reach.x0 - t[4] * k, ey = reach.y0 - t[5] * k;
    const double data[6] = {t[3] / det, -t[2] / det, (t[3] * ex - t[2] * ey) / det - from.x0,
                            -t[1] / det, t[0] / det, (-t[1] * ex + t[0] * ey) / det - from.y0};
    const Image part = upright.transform(Size{reach.width(), reach.height()}, TransformMethod::Affine, data, Resample::Bicubic);
    if (!part.getbbox()) return std::nullopt;
    if (reach == area) return part;
    Image picture = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
    picture.paste(part, Point{reach.x0 - area.x0, reach.y0 - area.y0});
    return picture;
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
    if (const std::optional<std::array<double, 6>> t = core::type_text::transform_of(line)) return turned(ctx, line, *t, *extent, area);
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
