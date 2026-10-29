// PC5 J-c (annotations@1, SPEC.md PROD-01「注釈」): the notes over the page (the extra "notes": each note's mark in its colour,
// its box of text while it is open) and the 注釈ツール — a click on a mark chooses it, a drag moves it, a double click opens
// or closes its box, a click on the paper elsewhere asks for a new note there. The window makes the ops (app/
// main_window_notes.cpp).

#include <QDateTime>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "app/canvas.hpp"
#include "render/ops_annotations.hpp"

namespace genko::app {

namespace {

constexpr double kMarkW = 16, kMarkH = 18;  // (a mark on the screen, its pixels: the same at any zoom)

QColor colour_of(const core::Annotation& note) { return QColor(note.color[0], note.color[1], note.color[2]); }

}  // namespace

void PageCanvas::paint_notes(QPainter& painter, const core::Page& page, const ExtraView& view) const {
    if (page.annotations.empty()) return;
    const QRectF seen = view.seen_mm.adjusted(-200, -200, 200, 200);  // (a box beside a mark out of sight may show)
    QFont font = painter.font();
    font.setPixelSize(11);
    for (const core::Annotation& note : page.annotations) {
        QPointF at(note.at_mm[0], note.at_mm[1]);
        if (note_drag_ && note_drag_->id == QString::fromStdString(note.id) && note_drag_->moved) at += note_drag_->to - note_drag_->grabbed;
        if (!seen.contains(at)) continue;
        // (on the screen, upright: the view's turn and flip undone for the words)
        const QPointF mark = painter.transform().map(view.at.map(at));
        std::array<double, 4> box = render::annotations::popup_of(note);
        box[0] += at.x() - note.at_mm[0];
        box[1] += at.y() - note.at_mm[1];
        const QPointF box_at = painter.transform().map(view.at.map(QPointF(box[0], box[1])));
        painter.save();
        painter.resetTransform();
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor colour = colour_of(note);
        const bool chosen = chosen_note == QString::fromStdString(note.id);
        // the mark: a sheet with its corner folded
        QPainterPath sheet;
        const QRectF r(mark, QSizeF(kMarkW, kMarkH));
        sheet.moveTo(r.topLeft());
        sheet.lineTo(r.right() - 5, r.top());
        sheet.lineTo(r.right(), r.top() + 5);
        sheet.lineTo(r.bottomRight());
        sheet.lineTo(r.bottomLeft());
        sheet.closeSubpath();
        painter.setPen(QPen(QColor(60, 60, 60), 1));
        painter.setBrush(colour);
        painter.drawPath(sheet);
        painter.drawLine(QPointF(r.right() - 5, r.top()), QPointF(r.right() - 5, r.top() + 5));
        painter.drawLine(QPointF(r.right() - 5, r.top() + 5), QPointF(r.right(), r.top() + 5));
        for (int k = 0; k < 3; ++k) painter.drawLine(QPointF(r.left() + 3, r.top() + 8 + 3 * k), QPointF(r.right() - 3, r.top() + 8 + 3 * k));
        if (chosen) {
            painter.setPen(QPen(QColor(30, 120, 230), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(r.adjusted(-2, -2, 2, 2));
        }
        // its box, while it is open
        if (note.open) {
            const double w = std::clamp(box[2] * view.per_mm, 140.0, 320.0), h = std::clamp(box[3] * view.per_mm, 70.0, 240.0);
            const QRectF b(box_at, QSizeF(w, h));
            painter.setPen(QPen(chosen ? QColor(30, 120, 230) : QColor(90, 90, 90), 1));
            painter.setBrush(colour.lighter(108));
            painter.drawRect(b);
            painter.setFont(font);
            const QFontMetricsF metrics(font);
            // (an agent's note says so — its author is any name it wrote: the review's P3-2)
            QString head = (core::annotation_by_person(note.actor) ? QString() : QStringLiteral("★ ")) + QString::fromStdString(note.author);
            if (!note.modified.empty()) {
                const QString when = QDateTime::fromString(QString::fromStdString(note.modified), Qt::ISODate).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                head = head.isEmpty() ? when : head + QStringLiteral("  ") + when;
            }
            const QRectF head_box = b.adjusted(4, 2, -4, 0);
            painter.setPen(QColor(40, 40, 40));
            painter.drawText(QRectF(head_box.topLeft(), QSizeF(head_box.width(), metrics.height())), Qt::AlignLeft | Qt::AlignVCenter,
                             metrics.elidedText(head, Qt::ElideRight, head_box.width()));
            painter.drawLine(QPointF(b.left(), b.top() + metrics.height() + 3), QPointF(b.right(), b.top() + metrics.height() + 3));
            painter.drawText(b.adjusted(4, metrics.height() + 5, -4, -3), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, QString::fromStdString(note.text));
        }
        painter.restore();
    }
}

std::optional<QString> PageCanvas::note_at(const QPointF& widget) const {
    const core::Page* p = page();
    if (p == nullptr || !extras_shown || !extra_shown(QStringLiteral("notes"))) return std::nullopt;
    for (auto it = p->annotations.rbegin(); it != p->annotations.rend(); ++it) {  // (the last drawn, on top, first)
        const QPointF mark = to_widget(QPointF(it->at_mm[0], it->at_mm[1]));
        if (QRectF(mark, QSizeF(kMarkW, kMarkH)).adjusted(-2, -2, 2, 2).contains(widget)) return QString::fromStdString(it->id);
    }
    return std::nullopt;
}

bool PageCanvas::note_press(const QPointF& pos, const QPointF& mm) {
    note_asked_ = false;
    const core::Page* p = page();
    if (p == nullptr) return true;
    if (const std::optional<QString> hit = note_at(view_transform().map(pos))) {
        note_drag_ = NoteDrag{*hit, mm, mm, false};
        emit noteChosen(*hit);
        update();
        return true;
    }
    // (on the paper only, as Photoshop's on the canvas)
    if (mm.x() < 0 || mm.y() < 0 || mm.x() > p->spec.width_mm.value() || mm.y() > p->spec.height_mm.value()) return true;
    note_asked_ = true;
    emit noteRequested(mm.x(), mm.y());
    return true;
}

bool PageCanvas::note_move(const QPointF& mm) {
    if (!note_drag_) return false;
    note_drag_->to = mm;
    if (!note_drag_->moved && std::hypot(mm.x() - note_drag_->grabbed.x(), mm.y() - note_drag_->grabbed.y()) * view_.scale >= 3.0) note_drag_->moved = true;
    update();
    return true;
}

bool PageCanvas::note_release() {
    if (!note_drag_) return false;
    const NoteDrag drag = *note_drag_;
    note_drag_.reset();
    const core::Page* p = page();
    if (drag.moved && p != nullptr) {
        for (const core::Annotation& note : p->annotations) {
            if (QString::fromStdString(note.id) != drag.id) continue;
            const QPointF to = QPointF(note.at_mm[0], note.at_mm[1]) + drag.to - drag.grabbed;
            emit noteMoved(drag.id, to.x(), to.y());
        }
    }
    update();
    return true;
}

bool PageCanvas::note_double(const QPointF& widget) {
    if (note_asked_) {  // (the first press asked for a note there: the double click is that note's)
        note_asked_ = false;
        return true;
    }
    const std::optional<QString> hit = note_at(widget);
    if (!hit) return false;
    note_drag_.reset();
    emit noteOpened(*hit);
    return true;
}

}  // namespace genko::app
