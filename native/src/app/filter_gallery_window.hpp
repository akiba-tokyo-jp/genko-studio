#pragma once

#include <QWidget>

#include <array>
#include <functional>
#include <optional>

#include "app/filter_menu.hpp"
#include "core/json.hpp"
#include "render/image.hpp"

// PC4 節 D3 (SPEC.md COLOR-08「フィルター」, design 5.2): フィルター → フィルターギャラリー… — Photoshop's window of the
// gallery: on the left the result of the effects over the layer (a part of it at 100 %, 50 % or 200 % of its pixels, or
// all of it; dragged to look elsewhere), worked out on a thread of its own a moment after a change; in the middle the six
// folders of the 47 effects, each with a small picture of it made from a small copy of the layer (one after another on a
// thread, every time the window opens: the window answers meanwhile); on the right the chosen effect's settings and the
// stack of effects (each with its eye, 新しいエフェクトレイヤー, エフェクトレイヤーを削除, dragged into another order: the
// lowest laid on first). かける sends the stack as filter_raster's filter_gallery (on a smart object a smart filter) with
// the main and sub colours as its colours (描画色・背景色); やめる changes nothing.

namespace genko::app::filter_gallery {

// What the window lays the effects on: the layer as the page shows it — its picture over a part of the page's pixels at
// a resolution (made on the window's thread) —, the page's size, the part of it the layer shows on, and the resolution of
// the layer's own pixels (an 8-bit layer's the working 200 dpi; a precise or ink layer's its grid's).
struct Source {
    std::function<render::Image(int dpi, const render::Box& area)> picture;
    double width_mm = 0, height_mm = 0;
    double x0_mm = 0, y0_mm = 0, x1_mm = 0, y1_mm = 0;  // (none shown: the page)
    double dpi = 200;
};

// The window: the stack it starts from (`now`: filter_gallery's settings; none: one effect, 色鉛筆 — Photoshop's first),
// the colours its sketches use. The settings chosen (filter_gallery's, their normal form), or none when the person stopped.
std::optional<core::Json> dialog(QWidget* parent, const Source& source, const core::Json& now, std::array<int, 3> foreground,
                                 std::array<int, 3> background);

// The window from フィルター's command or the layer panel's smart filter (app/filter_menu.cpp's filter_windows).
void open(const FilterWindowCall& call);

}  // namespace genko::app::filter_gallery
