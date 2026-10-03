#include "app/ime.hpp"

#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>

#include "app/theme.hpp"

namespace genko::app {

ImeEntry::ImeEntry(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setFocusPolicy(Qt::StrongFocus);
    setObjectName(QStringLiteral("imeEntry"));
}

void ImeEntry::clear() {
    preedit_.clear();
    text_.clear();
    update();
}

QVariant ImeEntry::inputMethodQuery(Qt::InputMethodQuery query) const {
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle:
        return QRect(fontMetrics().horizontalAdvance(text_ + preedit_) + 4, 4, 1, height() - 8);
    case Qt::ImSurroundingText:
        return text_;
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition:
        return static_cast<int>(text_.size());
    default:
        return QWidget::inputMethodQuery(query);
    }
}

void ImeEntry::inputMethodEvent(QInputMethodEvent* event) {
    if (!event->commitString().isEmpty()) {
        text_ += event->commitString();
        emit committed(event->commitString());  // (the words settled: handed on whole)
    }
    if (preedit_ != event->preeditString()) {
        preedit_ = event->preeditString();
        emit preeditChanged(preedit_);
    }
    event->accept();
    update();
}

void ImeEntry::keyPressEvent(QKeyEvent* event) {
    const QString typed = event->text();
    if (!typed.isEmpty() && typed.at(0).isPrint() && preedit_.isEmpty()) {
        text_ += typed;
        emit committed(typed);
        update();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ImeEntry::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(QStringLiteral("#fffdf5")));
    painter.setPen(QPen(theme::accent(), 2));
    painter.drawRect(rect().adjusted(1, 1, -1, -1));
    painter.setPen(QColor(QStringLiteral("#1f2124")));
    const int x = 4;
    const int base = (height() + fontMetrics().ascent() - fontMetrics().descent()) / 2;
    painter.drawText(x, base, text_);
    if (!preedit_.isEmpty()) {
        const int at = x + fontMetrics().horizontalAdvance(text_);
        painter.drawText(at, base, preedit_);
        painter.drawLine(at, base + 2, at + fontMetrics().horizontalAdvance(preedit_), base + 2);  // (still being converted)
    }
}

}  // namespace genko::app
