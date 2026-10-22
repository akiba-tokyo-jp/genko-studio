#pragma once

#include <QWidget>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

#include "app/filter_menu.hpp"
#include "core/json.hpp"
#include "render/image.hpp"

// PC4 節 D3 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → フィルターギャラリー… — Photoshop's window of the
// gallery: on the left the result of the effects over the layer (a part of it at 100 %, 50 % or 200 % of its pixels, or
// all of it; dragged to look elsewhere), worked out on a thread of its own a moment after a change — what the op lays on
// that part: its marks where the op puts them, the selection and the base box the op's, on the pixels it works (the layer
// alone: no layer style, mask or opacity; the channel drawn on); in the middle the six folders of the 47 effects, each with
// a small picture of it made from a small copy of the layer (one after another on a thread, every time the window opens:
// the window answers meanwhile); on the right the chosen effect's settings and the stack of effects (each with its eye,
// 新しいエフェクトレイヤー, エフェクトレイヤーを削除, dragged into another order: the lowest laid on first). かける sends the
// stack as filter_raster's filter_gallery (on a smart object a smart filter) with the main and sub colours as its colours
// (描画色・背景色); やめる changes nothing.

namespace genko::app {
class MainWindow;
}

namespace genko::app::filter_gallery {

// What the window lays the effects on: the pixels the op works — the layer alone (no layer style, mask, vector mask,
// opacity, blend or tint; a smart object's picture with the filters below the one set again), or the channel drawn on —
// over a part of the page's pixels at a resolution (made on a thread), the page's size, the selection (none: everywhere),
// and the layer's grid: its resolution (an 8-bit layer's the working 200 dpi; a precise or ink layer's its grid's; a
// channel's 200 dpi), its size, and — for whether the stack can be worked at once — the scale it is drawn at for print
// (a smart object's the page's dpi) and whether the page is of inks.
struct Source {
    std::function<render::Image(int dpi, const render::Box& area)> picture;
    double width_mm = 0, height_mm = 0;
    std::optional<core::Json> selection;
    double dpi = 200;
    std::uint32_t grid_width = 0, grid_height = 0;
    double print_s = 1;
    bool inks = false;
};

// The source of the layer of `page` (as the book has it now; `filter`: a smart object's smart filter set again — the
// filters below it alone —, -1 a filter added) with the window's selection and channel drawn on.
Source source_of(const MainWindow& window, const core::Page& page, const core::Layer& layer, int filter);

// The box (mm: x0, y0, x1, y1) of what the source shows (the page when nothing does), from a small picture of it.
std::array<double, 4> shown_box(const Source& source);

// The effects of `settings` (filter_gallery's) over the source's pixels at `area` of the page's pixels at `dpi` ("RGBA",
// the alpha as the pixels'): what the op lays on that part — its marks where the op on the whole grid puts them, the
// selection mixing it, the base box the op's (the selection's box, else the page).
render::Image laid_on(const Source& source, int dpi, const render::Box& area, const core::Json& settings);

// The window: the stack it starts from (`now`: filter_gallery's settings; none: the one used last — at first 色鉛筆 alone),
// the colours its sketches use. The settings chosen (filter_gallery's, their normal form), or none when the person stopped.
std::optional<core::Json> dialog(QWidget* parent, const Source& source, const core::Json& now, std::array<int, 3> foreground,
                                 std::array<int, 3> background);

// The window from フィルター's command or the layer panel's smart filter (app/filter_menu.cpp's filter_windows).
void open(const FilterWindowCall& call);

}  // namespace genko::app::filter_gallery
