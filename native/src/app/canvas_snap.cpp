// 表示 → スナップ・スナップ先 on the canvas (PC3 C-d, SPEC.md COLOR-07「移動・整列」; PhotoCraft v0.2.0's snap_ui.rs and
// core/snap.hpp): the targets made from the page now — the guide rulers, the grid, the paper's edges and middle, the other
// layers' boxes the window gives —, a point or a moving box snapped to them within 8 screen pixels (Ctrl held: not), and
// the lines they lined up with drawn while the drag lasts (the layer-to-layer ones and the paper's middle magenta, as
// Photoshop's smart guides). And the move tool's バウンディングボックスを表示: the box of what it moves, with its □.

#include <QPainter>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"

namespace genko::app {

namespace {

const QColor kSnapped(QStringLiteral("#15aabf"));  // (a guide, the grid, the paper's edge)
const QColor kMoveBox(QStringLiteral("#1c7ed6"));

}  // namespace

double PageCanvas::snap_reach() const { return 8.0 / std::max(0.01, view_.scale); }

core::snap::Targets PageCanvas::snap_targets() const {
    using core::snap::Kind;
    core::snap::Targets t;
    const core::Page* p = page();
    if (p == nullptr || !snap_on) return t;
    const double w = p->spec.width_mm.value(), h = p->spec.height_mm.value();
    t.extent = {0.0, 0.0, w, h};
    if (snap_bounds) t.add_box({0.0, 0.0, w, h}, Kind::PaperEdge, Kind::PaperMiddle);
    if (snap_guides) {
        for (const core::Json& r : page_rulers()) {  // (the guides: rulers of kind "guide", across ("h") or down ("v"))
            if (!r.is_object()) continue;
            const auto kind = r.find("kind"), at = r.find("at"), axis = r.find("axis");
            if (kind == r.end() || *kind != core::Json("guide") || at == r.end() || !at->is_number()) continue;
            const double v = at->get<double>();
            if (axis != r.end() && *axis == core::Json("h")) {
                t.y.push_back(core::snap::Target{v, Kind::Guide, {0.0, w}});
            } else {
                t.x.push_back(core::snap::Target{v, Kind::Guide, {0.0, h}});
            }
        }
    }
    if (grid_snap && grid_mm > 0) t.grid = grid_mm;
    if (snap_layers && snap_boxes) {
        for (const auto& [id, box] : snap_boxes()) {
            if (std::find(snap_moving.begin(), snap_moving.end(), id) != snap_moving.end()) continue;
            t.add_box(box, Kind::LayerEdge, Kind::LayerMiddle);
        }
    }
    return t;
}

QPointF PageCanvas::snap_point(const QPointF& mm, Qt::KeyboardModifiers modifiers) {
    forget_snap();
    if (modifiers & Qt::ControlModifier) return mm;  // (Ctrl held: as it is)
    const core::snap::Targets t = snap_targets();
    if (t.empty()) return mm;
    const auto [p, lines] = t.point({mm.x(), mm.y()}, snap_reach());
    snap_lines_ = lines;
    return QPointF(p[0], p[1]);
}

QPointF PageCanvas::snap_box(const std::array<double, 4>& box, Qt::KeyboardModifiers modifiers) {
    forget_snap();
    if (modifiers & Qt::ControlModifier) return QPointF(0, 0);
    const core::snap::Targets t = snap_targets();
    if (t.empty()) return QPointF(0, 0);
    const auto [d, lines] = t.box(box, snap_reach());
    snap_lines_ = lines;
    find_spacing({box[0] + d[0], box[1] + d[1], box[2] + d[0], box[3] + d[1]});  // (PC5 H-b: the equal gaps, スマートガイド)
    return QPointF(d[0], d[1]);
}

void PageCanvas::draw_snap(QPainter& painter) const {
    if (snap_lines_.empty()) return;
    painter.save();
    painter.setBrush(Qt::NoBrush);
    for (const core::snap::Line& l : snap_lines_) {
        // (PC5 H-b: スマートガイド hidden — or エクストラ —, the layer-to-layer lines are not drawn; the snap is as it was)
        const bool smart = core::snap::is_smart(l.kind);
        if (smart && !shows(show_smart_guides)) continue;
        painter.setPen(QPen(smart ? smart_colour : kSnapped, 1));
        if (l.vertical) {
            painter.drawLine(pt(l.pos, l.from), pt(l.pos, l.to));
        } else {
            painter.drawLine(pt(l.from, l.pos), pt(l.to, l.pos));
        }
    }
    painter.restore();
}

// --- the move tool's bounding box -------------------------------------------------------------------------------------

std::optional<std::array<double, 4>> PageCanvas::move_box_now() const {
    if (tool_ != QLatin1String("move") || !show_move_box || !moving_box || xform_) return std::nullopt;
    return moving_box();
}

std::optional<int> PageCanvas::move_box_handle(const QPointF& pos) const {
    const auto b = move_box_now();
    if (!b) return std::nullopt;
    const std::array<QPointF, 8> at{QPointF((*b)[0], (*b)[1]), QPointF((*b)[2], (*b)[1]), QPointF((*b)[2], (*b)[3]), QPointF((*b)[0], (*b)[3]),
                                    QPointF(((*b)[0] + (*b)[2]) / 2, (*b)[1]), QPointF((*b)[2], ((*b)[1] + (*b)[3]) / 2),
                                    QPointF(((*b)[0] + (*b)[2]) / 2, (*b)[3]), QPointF((*b)[0], ((*b)[1] + (*b)[3]) / 2)};
    for (std::size_t i = 0; i < at.size(); ++i) {
        const QPointF p = pt(at[i].x(), at[i].y());
        if (std::abs(p.x() - pos.x()) <= 8 && std::abs(p.y() - pos.y()) <= 8) return static_cast<int>(i);
    }
    return std::nullopt;
}

void PageCanvas::draw_move_box(QPainter& painter) const {
    if (tool_drag_) return;  // (while the layers are dragged, their picture shows)
    const auto b = move_box_now();
    if (!b) return;
    painter.save();
    const QRectF box(pt((*b)[0], (*b)[1]), pt((*b)[2], (*b)[3]));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kMoveBox, 1));
    painter.drawRect(box);
    painter.setBrush(Qt::white);
    for (const QPointF& c : {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft(), QPointF(box.center().x(), box.top()),
                             QPointF(box.right(), box.center().y()), QPointF(box.center().x(), box.bottom()), QPointF(box.left(), box.center().y())})
        painter.drawRect(QRectF(c.x() - 3.5, c.y() - 3.5, 7, 7));
    painter.restore();
}

}  // namespace genko::app
