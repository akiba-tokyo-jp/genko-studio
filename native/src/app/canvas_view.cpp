// The canvas's view of the page's pixels (PC5 H-a, SPEC VIEW-01): ピクセルグリッド — the grid of the layer drawn on, over the
// page once its cells are large on the screen — and ピクセルアートのプレビュー (the tiles laid nearest).

#include <QPainter>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "core/color_raster.hpp"
#include "core/ink.hpp"
#include "render/layer_masks.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"

namespace genko::app {

namespace {

const core::Layer* layer_of(const core::Page& page, const std::optional<std::string>& id) {
    if (!id) return nullptr;
    for (const core::Layer& layer : page.layers) {
        if (layer.id == *id) return &layer;
    }
    return nullptr;
}

}  // namespace

QSizeF PageCanvas::pixel_grid_cell_mm() const {
    const core::Page* p = page();
    if (p == nullptr) return {};
    const double w = p->spec.width_mm.value(), h = p->spec.height_mm.value();
    if (!(w > 0) || !(h > 0)) return {};
    // a layer of pixels: its own pixels, stretched over the page (a paint layer's, a precise or ink layer's)
    const auto across = [w, h](double cols, double rows) { return cols >= 1 && rows >= 1 ? QSizeF(w / cols, h / rows) : QSizeF(); };
    if (const core::Layer* layer = layer_of(*p, grid_layer)) {
        try {
            if (layer->color_raster) {
                const core::ColorRasterView view(*layer->color_raster);
                return across(view.width(), view.height());
            }
            if (layer->ink_raster) {
                const core::InkRasterHeader head = core::ink_raster_header(*layer->ink_raster);
                return across(head.width, head.height);
            }
            if (layer->kind == core::LayerKind::Raster) {
                if (layer->raster_png && !layer->raster_png->empty()) {
                    const render::Size size = render::picture_size(*layer->raster_png);
                    return across(size.width, size.height);
                }
                // (none yet: where its pixels will be made — the page's grid)
                const bool precise = p->color || (p->rgb && p->rgb->depth > 8);
                const int dpi = precise ? render::layer_masks::grid_dpi(*p) : render::raster::working_dpi(*p);
                return across(render::mm_to_px(w, dpi), render::mm_to_px(h, dpi));
            }
        } catch (const std::exception&) {  // (a raster this build cannot read: the page's grid)
        }
    }
    // a pen, vector or text layer (or none): the page's own pixels, as its tiles lay them (25.4 / dpi mm a pixel)
    const double dpi = p->spec.dpi.value();
    if (!std::isfinite(dpi) || dpi < 1) return {};
    const double cell = 25.4 / std::lround(dpi);
    return QSizeF(cell, cell);
}

int PageCanvas::pixel_grid_dpi() const {
    const QSizeF cell = pixel_grid_cell_mm();
    return cell.width() > 0 ? static_cast<int>(std::lround(25.4 / cell.width())) : 0;
}

bool PageCanvas::pixel_grid_shown() const {
    if (!show_pixel_grid || page() == nullptr) return false;
    const QSizeF cell = pixel_grid_cell_mm();
    const double ratio = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    return cell.width() > 0 && std::min(cell.width(), cell.height()) * view_.scale * ratio >= kPixelGridPx;
}

void PageCanvas::draw_pixel_grid(QPainter& painter) const {
    if (!pixel_grid_shown()) return;
    const core::Page* p = page();
    const QSizeF cell = pixel_grid_cell_mm();
    const QRectF seen = seen_mm() & QRectF(0, 0, p->spec.width_mm.value(), p->spec.height_mm.value());
    if (seen.isEmpty()) return;
    painter.save();
    painter.setTransform(mm_transform());  // (in page mm, through the view's turn and mirror: the grid turns with it)
    painter.setRenderHint(QPainter::Antialiasing, view_.rotation != 0.0);
    QPen pen(QColor(128, 128, 128, 120), 1.0);
    pen.setCosmetic(true);  // (one screen pixel wide at any zoom)
    painter.setPen(pen);
    const auto lines = [](double from, double to, double step) {
        return std::make_pair(static_cast<long>(std::ceil(from / step - 1e-9)), static_cast<long>(std::floor(to / step + 1e-9)));
    };
    const auto [x0, x1] = lines(seen.left(), seen.right(), cell.width());
    const auto [y0, y1] = lines(seen.top(), seen.bottom(), cell.height());
    if (x1 - x0 < 20000 && y1 - y0 < 20000) {  // (shown only at kPixelGridPx and up: a screen's worth at most)
        for (long i = x0; i <= x1; ++i) painter.drawLine(QPointF(i * cell.width(), seen.top()), QPointF(i * cell.width(), seen.bottom()));
        for (long j = y0; j <= y1; ++j) painter.drawLine(QPointF(seen.left(), j * cell.height()), QPointF(seen.right(), j * cell.height()));
    }
    painter.restore();
}

void PageCanvas::set_pixel_art(bool on) {
    renderer_->set_pixel_art(on);
    update();
}

bool PageCanvas::pixel_art() const { return renderer_->pixel_art(); }

}  // namespace genko::app
