// PC5 G-0 (type@1, render/type_layer.hpp): a type layer's picture.

#include "render/type_layer.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/error.hpp"
#include "core/pyops.hpp"
#include "core/type_text.hpp"
#include "render/imaging.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_smart_parts.hpp"
#include "render/page_internal.hpp"
#include "render/text/opentype.hpp"
#include "render/text/balloons.hpp"
#include "render/text/lettering.hpp"

namespace genko::render::type_layer {

namespace {

constexpr int kFindDpi = 50;           // (where the letters are: the page drawn this coarsely first)
constexpr double kFindMarginMm = 3.0;  // (round what the coarse drawing found: what it may miss of thin marks and halos)
constexpr std::int64_t kMostPixels = 20'000'000;  // (an upright piece to turn: op_limits' most pixels over the six pictures drawing it takes)
constexpr double kReachMm = 60.0;  // (G-a: how far round its box its letters are looked for, off the page too)
constexpr double kFarMm = 1000.0;  // (… at most this far past the paper)

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
    const text::TypeStyles type_styles(ctx.episode != nullptr && ctx.episode->type_styles ? &*ctx.episode->type_styles : text::TypeStyles::current());
    return smart_ops::drawn_alone(size, [&](Image& on) {
        Image rgba = on.mode() == "RGBA" ? std::move(on) : on.convert("RGBA");
        text::draw_lines(text::PagePart{&rgba, origin, page}, lines, dpi, font_path, false, &panels, ctx.stop, &unported);
        on = std::move(rgba);
    });
}

// Where the record's letters and balloon lie on the page (mm: x0, y0, x1, y1; none: nothing drawn), found by drawing the
// page coarsely, and remembered for the record (the same record object: an op makes a new one) and the page's paper.
struct Kept {
    std::weak_ptr<const core::StoryLine> record;
    double w = 0, h = 0;
    std::string font;
    std::string page;
    std::string styles;  // (PC5 G-c: the book's type styles' digest — a style changed sets the texts that take it anew)
    std::optional<std::array<double, 4>> extent;
};
std::mutex mutex;
std::map<const core::StoryLine*, Kept> kept;

std::optional<std::array<double, 4>> extent_mm(const detail::Ctx& ctx, const core::Layer& layer, const core::StoryLine& line) {
    const double w = ctx.page->spec.width_mm.value(), h = ctx.page->spec.height_mm.value();
    const std::string font = ctx.episode != nullptr ? ctx.episode->font_path : std::string();
    const std::string page_id = ctx.page->id;
    // (PC5 G-c: a record the OpenType typesetting sets reads the book's styles: their digest, as the lettering's memo key)
    std::string styles;
    const core::Json* book_styles = ctx.episode != nullptr && ctx.episode->type_styles ? &*ctx.episode->type_styles : text::TypeStyles::current();
    if (book_styles != nullptr && !book_styles->is_null() && text::sets_opentype(line)) {
        if (book_styles == text::TypeStyles::current()) {
            styles = text::TypeStyles::digest();
        } else {
            const text::TypeStyles scope(book_styles);
            styles = text::TypeStyles::digest();
        }
    }
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = kept.find(layer.type.get()); it != kept.end()) {
            if (it->second.record.lock() == layer.type && it->second.w == w && it->second.h == h && it->second.font == font && it->second.page == page_id &&
                it->second.styles == styles)
                return it->second.extent;
        }
    }
    // (the page, and round the record's box: a type layer its matrix turns may be set off the page it is shown on — G-a;
    // never further than a metre past the paper — review P2-4)
    const double x0 = std::max(-kFarMm, std::min(0.0, line.x_mm.value() - kReachMm)), y0 = std::max(-kFarMm, std::min(0.0, line.y_mm.value() - kReachMm));
    const double x1 = std::min(w + kFarMm, std::max(w, line.x_mm.value() + line.w_mm.value() + kReachMm));
    const double y1 = std::min(h + kFarMm, std::max(h, line.y_mm.value() + line.h_mm.value() + kReachMm));
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
    kept[layer.type.get()] = Kept{layer.type, w, h, font, page_id, styles, extent};
    return extent;
}

std::atomic<std::int64_t> largest_upright{0};  // (tests: the most pixels one upright piece took)

// アンチエイリアス なし after the map: each pixel whole or none (the bicubic map made the edges soft again — review P3-4)
void binarized(Image& image) {
    if (image.mode() != "RGBA") return;
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<unsigned char*>(image.raw()->image[y]);
        for (int x = 0; x < image.width(); ++x) row[x * 4 + 3] = row[x * 4 + 3] >= 128 ? 255 : 0;
    }
}

// PC5 G-a: a type layer turned, scaled or bent by its matrix (native_type.transform, the page's mm to the page's mm):
// its letters drawn upright, then mapped through it (bicubic, its colours premultiplied) to where the matrix puts them;
// the part of `area` they reach. Only the upright part a piece of `area` takes is drawn (the piece's corners back
// through the matrix, met with the extent, 3 px round for the bicubic), in rows of the area small enough that one
// upright piece stays within kMostPixels: a tile of the screen draws its own share, a whole page at a high resolution
// draws in bands (review P1-1). The record is set where none of it lies above or left of the paper — moved by whole
// inches (whole pixels at any resolution), the matrix moved back — so a balloon there is not cut by the page's edge
// (review P2-3).
std::optional<Image> turned(const detail::Ctx& ctx, const core::StoryLine& line, const std::array<double, 6>& t, const std::array<double, 4>& extent,
                            const Box& area) {
    const double k = ctx.dpi / 25.4;
    const double det = t[0] * t[3] - t[1] * t[2];
    if (!(std::fabs(det) > 1e-12)) return std::nullopt;
    core::StoryLine upright = line;
    if (upright.native_type) upright.native_type->erase("transform");  // (set upright; the map is this function's)
    const double sx = extent[0] < 1 ? 25.4 * std::ceil((1 - extent[0]) / 25.4) : 0.0;
    const double sy = extent[1] < 1 ? 25.4 * std::ceil((1 - extent[1]) / 25.4) : 0.0;
    std::array<double, 6> m = t;
    std::array<double, 4> e = extent;
    if (sx != 0 || sy != 0) {
        upright = core::type_text::moved(upright, sx, sy);
        m[4] -= t[0] * sx + t[2] * sy;
        m[5] -= t[1] * sx + t[3] * sy;
        e = {extent[0] + sx, extent[1] + sy, extent[2] + sx, extent[3] + sy};
    }
    const Box set_in{static_cast<int>(std::floor(e[0] * k)), static_cast<int>(std::floor(e[1] * k)), static_cast<int>(std::ceil(e[2] * k)),
                     static_cast<int>(std::ceil(e[3] * k))};
    const Size paper{std::max(ctx.size.width, set_in.x1 + 4), std::max(ctx.size.height, set_in.y1 + 4)};
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto& [x, y] : {std::pair{e[0], e[1]}, {e[2], e[1]}, {e[0], e[3]}, {e[2], e[3]}}) {
        const double u = (m[0] * x + m[2] * y + m[4]) * k, v = (m[1] * x + m[3] * y + m[5]) * k;
        x0 = std::min(x0, u);
        y0 = std::min(y0, v);
        x1 = std::max(x1, u);
        y1 = std::max(y1, v);
    }
    const Box reach{std::max(area.x0, static_cast<int>(std::floor(std::max(x0, -1e9)))), std::max(area.y0, static_cast<int>(std::floor(std::max(y0, -1e9)))),
                    std::min(area.x1, static_cast<int>(std::ceil(std::min(x1, 1e9)))), std::min(area.y1, static_cast<int>(std::ceil(std::min(y1, 1e9))))};
    if (reach.width() <= 0 || reach.height() <= 0 || set_in.width() <= 0 || set_in.height() <= 0) return std::nullopt;
    // (a point of the page (px) back through the matrix: the upright px)
    const auto back = [&](double u, double v) {
        const double X = u / k - m[4], Y = v / k - m[5];
        return std::pair{(m[3] * X - m[2] * Y) / det * k, (-m[1] * X + m[0] * Y) / det * k};
    };
    const auto upright_of = [&](const Box& piece) {
        double a0 = 1e300, b0 = 1e300, a1 = -1e300, b1 = -1e300;
        for (const auto& [u, v] : {std::pair<double, double>{piece.x0, piece.y0}, {piece.x1, piece.y0}, {piece.x0, piece.y1}, {piece.x1, piece.y1}}) {
            const auto [p, q] = back(u, v);
            a0 = std::min(a0, p);
            b0 = std::min(b0, q);
            a1 = std::max(a1, p);
            b1 = std::max(b1, q);
        }
        const auto clamp = [](double v) { return static_cast<int>(std::clamp(v, -1e9, 1e9)); };
        return Box{std::max(set_in.x0, clamp(std::floor(a0)) - 3), std::max(set_in.y0, clamp(std::floor(b0)) - 3),
                   std::min(set_in.x1, clamp(std::ceil(a1)) + 3), std::min(set_in.y1, clamp(std::ceil(b1)) + 3)};
    };
    const core::Json* aa = upright.native_type ? core::get(*upright.native_type, "antialias") : nullptr;
    const bool hard = aa != nullptr && *aa == core::Json("none");
    std::optional<Image> picture;
    int rows = reach.height();
    for (int y = reach.y0; y < reach.y1;) {
        const Box piece{reach.x0, y, reach.x1, std::min(reach.y1, y + rows)};
        const Box from = upright_of(piece);
        if (from.width() <= 0 || from.height() <= 0) {
            y = piece.y1;
            continue;
        }
        const std::int64_t pixels = static_cast<std::int64_t>(from.width()) * from.height();
        if (pixels > kMostPixels && piece.height() > 1) {  // (too much at once: fewer rows)
            rows = std::max(1, piece.height() / 2);
            continue;
        }
        if (pixels > kMostPixels) throw core::Error("image_too_large", "the type layer is too large to turn at this resolution");
        for (std::int64_t was = largest_upright.load(); pixels > was && !largest_upright.compare_exchange_weak(was, pixels);) {}
        const Image drawn_piece = drawn(ctx, upright, Point{from.x0, from.y0}, Size{from.width(), from.height()}, paper, ctx.dpi);
        if (drawn_piece.getbbox()) {
            // (a pixel of the piece (u, v) → the page's px → mm → back through the matrix → the upright piece's px)
            const double ex = piece.x0 - m[4] * k, ey = piece.y0 - m[5] * k;
            const double data[6] = {m[3] / det, -m[2] / det, (m[3] * ex - m[2] * ey) / det - from.x0,
                                    -m[1] / det, m[0] / det, (-m[1] * ex + m[0] * ey) / det - from.y0};
            Image part = drawn_piece.transform(Size{piece.width(), piece.height()}, TransformMethod::Affine, data, Resample::Bicubic);
            if (hard) binarized(part);
            if (part.getbbox()) {
                if (!picture) picture = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
                picture->paste(part, Point{piece.x0 - area.x0, piece.y0 - area.y0});
            }
        }
        y = piece.y1;
    }
    return picture;
}

}  // namespace

void forget_extents() {
    const std::lock_guard<std::mutex> lock(mutex);
    kept.clear();
}

std::int64_t largest_upright_pixels(bool reset) {
    const std::int64_t out = largest_upright.load();
    if (reset) largest_upright.store(0);
    return out;
}

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
