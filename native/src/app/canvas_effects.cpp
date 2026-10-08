// The canvas's effect-line tool and material stamp (M3④-5, Python's PageCanvas and GuidesMixin): 効果線 — a click in
// a panel puts the chosen effect line there, the centre (＋) of a focus line or flash drags to a new place —, and 素材を置く
// — a click asks for the chosen material there.

#include <QPainter>

#include "app/canvas.hpp"
#include "app/theme.hpp"
#include "core/pynum.hpp"
#include "render/effects.hpp"

namespace genko::app {

std::vector<std::pair<std::string, QPointF>> PageCanvas::effect_handles() const {
    // the centres of the page's focus lines and flashes (the middle of their panel when they have none)
    std::vector<std::pair<std::string, QPointF>> out;
    const core::Page* p = page();
    if (p == nullptr) return out;
    for (const core::Json& effect : p->effects) {
        if (!effect.is_object()) continue;
        const core::Json kind = effect.value("kind", core::Json());
        if (kind != "focus" && kind != "uni_flash" && kind != "beta_flash") continue;
        try {
            const auto [outline, box] = render::effects::panel_area(effect, *p);
            const core::Json params = effect.contains("params") && effect["params"].is_object() ? effect["params"] : core::Json::object();
            const render::effects::XY c = render::effects::centre(params, box);
            out.emplace_back(effect.value("id", std::string()), QPointF(c.x, c.y));
        } catch (const std::exception&) {  // (an effect that cannot be drawn has no handle)
        }
    }
    return out;
}

void PageCanvas::draw_effect_handles(QPainter& painter) const {
    if (page() == nullptr || tool_ != QLatin1String("effect")) return;
    painter.save();
    for (auto [effect_id, point] : effect_handles()) {
        if (effect_drag_ && effect_drag_->id == effect_id) point = effect_drag_->to;
        const QPointF q = pt(point.x(), point.y());
        painter.setPen(QPen(theme::accent(), 2));
        painter.setBrush(selected_effect_id && *selected_effect_id == effect_id ? theme::accent() : QColor(255, 255, 255, 220));
        painter.drawEllipse(q, 7, 7);
        painter.drawLine(QPointF(q.x() - 11, q.y()), QPointF(q.x() + 11, q.y()));
        painter.drawLine(QPointF(q.x(), q.y() - 11), QPointF(q.x(), q.y() + 11));
    }
    painter.restore();
}

void PageCanvas::effect_press(const QPointF& pos) {
    for (const auto& [effect_id, point] : effect_handles()) {
        if (near_point(pos, point, 10)) {
            selected_effect_id = effect_id;
            effect_drag_ = EffectDrag{effect_id, point, false};
            emit effectSelected(QString::fromStdString(effect_id));
            update();
            return;
        }
    }
    const QPointF mm = mm_of(pos);
    emit effectRequested(mm.x(), mm.y());
}

bool PageCanvas::effect_move(const QPointF& mm) {
    if (!effect_drag_) return false;
    effect_drag_->to = QPointF(core::py_round(mm.x(), 2), core::py_round(mm.y(), 2));
    effect_drag_->moved = true;
    update();
    return true;
}

bool PageCanvas::effect_release() {
    if (!effect_drag_) return false;
    const EffectDrag drag = *effect_drag_;
    effect_drag_.reset();
    if (drag.moved) emit effectMoved(QString::fromStdString(drag.id), drag.to);
    update();
    return true;
}

}  // namespace genko::app
