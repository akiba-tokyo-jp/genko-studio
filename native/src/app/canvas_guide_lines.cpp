// The guides placed on the page, drawn and grabbed; the layers' edges; the equal gaps of a moved box; the lengths in the
// ruler's unit (PC5 H-b, 節 H 4.4 and 5.1, SPEC.md VIEW-01「ガイド・グリッド・エクストラ」「単位・定規」; PhotoCraft v0.2.0's
// ui-egui rulers.rs finish_drag — a guide moved, or taken away when let go off the canvas —, MIT). The guides are the page's
// rulers of kind "guide" (axis "h" across at y = at, "v" upright at x = at, mm): what changes them is the window's op from
// the canvas's signals (edit_ruler, delete_ruler).

#include <QPainter>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"

namespace genko::app {

namespace {

const QColor kLayerEdge(64, 160, 255);

struct Guide {
    std::string id;
    bool across = false;  // (axis "h")
    double at = 0;
};

// A ruler as a guide (none: not one, hidden, or not as a guide is made).
std::optional<Guide> guide_of(const core::Json& r, bool grabbing) {
    if (!r.is_object() || !core::py_equals(core::py_get(r, "kind"), core::Json("guide"))) return std::nullopt;
    if (!core::py_truthy(core::py_get(r, "visible", core::Json(true)))) return std::nullopt;
    if (grabbing && core::py_truthy(core::py_get(r, "fixed", core::Json(false)))) return std::nullopt;  // (固定: as a ruler's points)
    const core::Json at = core::py_get(r, "at");
    const core::Json axis = core::py_get(r, "axis");
    const core::Json id = core::py_get(r, "id");
    if (!at.is_number() || !axis.is_string() || !std::isfinite(at.get<double>())) return std::nullopt;
    const std::string way = axis.get<std::string>();
    if (way != "h" && way != "v") return std::nullopt;
    return Guide{id.is_string() ? id.get<std::string>() : std::string(), way == "h", at.get<double>()};
}

}  // namespace

// --- the unit ---------------------------------------------------------------------------------------------------------

core::units::Basis PageCanvas::unit_basis(bool across) const {
    core::units::Basis basis;
    basis.points_per_inch = points_per_inch;
    basis.column_pt = column_pt;
    basis.gutter_pt = gutter_pt;
    if (const core::Page* p = page()) {
        basis.dpi = p->spec.dpi.value();
        basis.extent_mm = across ? p->spec.width_mm.value() : p->spec.height_mm.value();
    }
    return basis;
}

QString PageCanvas::length_words(double mm, bool across) const {
    const double v = core::units::from_mm(mm, ruler_unit, unit_basis(across));
    return QStringLiteral("%1 %2").arg(v, 0, 'f', core::units::decimals_of(ruler_unit)).arg(QString::fromStdString(core::units::suffix_of(ruler_unit)));
}

// --- the guides -------------------------------------------------------------------------------------------------------

std::optional<std::string> PageCanvas::guide_at(const QPointF& widget) const {
    if (page() == nullptr || lock_guides || !shows(show_guide_lines) || !show_canvas_guides) return std::nullopt;
    const QPointF mm = to_mm(widget);
    const double reach = 4.0 / std::max(0.01, view_.scale);  // (4 screen pixels)
    std::optional<std::string> best;
    double nearest = reach;
    for (const core::Json& r : page_rulers()) {
        const auto g = guide_of(r, true);
        if (!g || g->id.empty()) continue;
        const double d = std::abs((g->across ? mm.y() : mm.x()) - g->at);
        if (d <= nearest) {
            nearest = d;
            best = g->id;
        }
    }
    return best;
}

std::optional<std::string> PageCanvas::dragged_guide() const { return guide_drag_ ? guide_drag_->id : std::nullopt; }

bool PageCanvas::guide_grab(const QPointF& widget, Qt::KeyboardModifiers modifiers) {
    // (移動ツール, or another tool with Ctrl held, as Photoshop's; the ruler tool's own points, the hand and the zoom never)
    if (tool_ == QLatin1String("ruler") || tool_ == QLatin1String("hand") || tool_ == QLatin1String("zoom")) return false;
    if (tool_ != QLatin1String("move") && !(modifiers & Qt::ControlModifier)) return false;
    const auto id = guide_at(widget);
    if (!id) return false;
    for (const core::Json& r : page_rulers()) {
        const auto g = guide_of(r, true);
        if (!g || g->id != *id) continue;
        guide_drag_ = GuideDrag{g->across ? QStringLiteral("h") : QStringLiteral("v"), g->at, g->id, (modifiers & Qt::AltModifier) != 0};
        update();
        return true;
    }
    return false;
}

void PageCanvas::draw_guide_lines(QPainter& painter) const {
    if (page() == nullptr) return;
    // (across the whole view, past the paper: the view's corners in the unturned view)
    const QRectF view = view_transform().inverted().mapRect(QRectF(rect()));
    if (guide_preview) {  // (新規ガイドレイアウトを作成…: shown whatever is hidden — the window asks for it)
        painter.save();
        QColor faint = guide_colour;
        faint.setAlpha(150);
        QPen pen(faint, 1, Qt::DashLine);
        pen.setCosmetic(true);
        painter.setPen(pen);
        for (const double x : guide_preview->down) painter.drawLine(QPointF(pt(x, 0).x(), view.top()), QPointF(pt(x, 0).x(), view.bottom()));
        for (const double y : guide_preview->across) painter.drawLine(QPointF(view.left(), pt(0, y).y()), QPointF(view.right(), pt(0, y).y()));
        painter.restore();
    }
    if (!shows(show_guide_lines) || !show_canvas_guides) return;
    painter.save();
    QPen pen(guide_colour, 1, guide_style);
    pen.setCosmetic(true);
    painter.setPen(pen);
    const std::optional<std::string> dragged = dragged_guide();
    for (const core::Json& r : page_rulers()) {
        const auto g = guide_of(r, false);
        if (!g || (dragged && g->id == *dragged)) continue;  // (the one dragged: where it is going, draw_guide_drag)
        const QPointF at = pt(g->at, g->at);
        if (g->across) {
            painter.drawLine(QPointF(view.left(), at.y()), QPointF(view.right(), at.y()));
        } else {
            painter.drawLine(QPointF(at.x(), view.top()), QPointF(at.x(), view.bottom()));
        }
    }
    painter.restore();
}

// --- the layers' edges ------------------------------------------------------------------------------------------------

void PageCanvas::draw_layer_edges(QPainter& painter) const {
    if (page() == nullptr || !shows(show_layer_edges) || !layer_edges) return;
    painter.save();
    QPen pen(kLayerEdge, 1);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    for (const auto& b : layer_edges()) painter.drawRect(QRectF(pt(b[0], b[1]), pt(b[2], b[3])).normalized());
    painter.restore();
}

// --- the equal gaps (スマートガイド) ------------------------------------------------------------------------------------

void PageCanvas::forget_snap() {
    snap_lines_.clear();
    spacing_.clear();
}

void PageCanvas::find_spacing(const std::array<double, 4>& box) {
    spacing_.clear();
    if (!shows(show_smart_guides) || !snap_on || !snap_layers || !snap_boxes) return;
    const double tolerance = 1.0 / std::max(0.01, view_.scale);  // (one screen pixel)
    std::vector<std::array<double, 4>> others;
    for (const auto& [id, b] : snap_boxes()) {
        if (std::find(snap_moving.begin(), snap_moving.end(), id) == snap_moving.end()) others.push_back(b);
    }
    for (const bool along_x : {true, false}) {
        const int lo = along_x ? 0 : 1, hi = along_x ? 2 : 3;          // (the axis the gaps are along)
        const int o_lo = along_x ? 1 : 0, o_hi = along_x ? 3 : 2;      // (the other: the boxes must meet the moved one there)
        const std::array<double, 4>* before = nullptr;
        const std::array<double, 4>* after = nullptr;
        for (const auto& o : others) {
            if (o[o_hi] < box[o_lo] || o[o_lo] > box[o_hi]) continue;
            if (o[hi] <= box[lo] + tolerance && (before == nullptr || o[hi] > (*before)[hi])) before = &o;
            if (o[lo] >= box[hi] - tolerance && (after == nullptr || o[lo] < (*after)[lo])) after = &o;
        }
        if (before == nullptr || after == nullptr) continue;
        const double gap_before = box[lo] - (*before)[hi], gap_after = (*after)[lo] - box[hi];
        if (gap_before <= tolerance || gap_after <= tolerance || std::abs(gap_before - gap_after) > tolerance) continue;
        // (each drawn in the middle of where the two boxes meet across it)
        const auto middle = [&](const std::array<double, 4>& o) { return (std::max(o[o_lo], box[o_lo]) + std::min(o[o_hi], box[o_hi])) / 2.0; };
        spacing_.push_back(Spacing{!along_x, (*before)[hi], box[lo], middle(*before)});
        spacing_.push_back(Spacing{!along_x, box[hi], (*after)[lo], middle(*after)});
    }
}

void PageCanvas::draw_spacing(QPainter& painter) const {
    if (spacing_.empty() || !shows(show_smart_guides)) return;
    painter.save();
    QPen pen(smart_colour, 1);
    pen.setCosmetic(true);
    painter.setBrush(Qt::NoBrush);
    for (const Spacing& s : spacing_) {
        // (a gap along x is drawn across at `across` mm down; one along y upright at `across` mm along)
        const QPointF a = s.vertical ? pt(s.across, s.from) : pt(s.from, s.across);
        const QPointF b = s.vertical ? pt(s.across, s.to) : pt(s.to, s.across);
        painter.setPen(pen);
        painter.drawLine(a, b);
        const QPointF tick = s.vertical ? QPointF(4, 0) : QPointF(0, 4);
        painter.drawLine(a - tick, a + tick);
        painter.drawLine(b - tick, b + tick);
        const QString words = length_words(s.to - s.from, !s.vertical);
        const QPointF mid = (a + b) / 2.0;
        const QRectF label = painter.fontMetrics().boundingRect(words).adjusted(-3, -1, 3, 1);
        const QRectF placed(mid.x() - label.width() / 2.0, mid.y() - label.height() - 4, label.width(), label.height());
        painter.fillRect(placed, smart_colour);
        painter.setPen(Qt::white);
        painter.drawText(placed, Qt::AlignCenter, words);
    }
    painter.restore();
}

}  // namespace genko::app
