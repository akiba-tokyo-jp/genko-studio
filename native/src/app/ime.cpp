#include "app/ime.hpp"

#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>

#include <algorithm>

#include "app/lettering.hpp"
#include "app/text_style.hpp"
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

// --- the text tool's editor -----------------------------------------------------------------------------------------

InlineEditor::InlineEditor(QWidget* parent, const QString& text, std::function<void(std::optional<QString>)> on_done)
    : QPlainTextEdit(parent), on_done_(std::move(on_done)) {
    setObjectName(QStringLiteral("lineEditor"));
    setPlainText(text);
    const theme::Tokens t = theme::tokens();
    setStyleSheet(QStringLiteral("QPlainTextEdit{background:#fffdf5;color:#1f2124;border:2px solid %1;font-size:15px}").arg(t.accent));
    setPlaceholderText(QStringLiteral("台詞を入力（改行で次の列、ルビは ｜約束《やくそく》、傍点は 《《強調》》）"));
    hint_ = new QLabel(QStringLiteral("Ctrl+Enter で決定・Esc でやめる・文字を選んで右クリックで大きく・太く・色"), parent);
    hint_->setObjectName(QStringLiteral("lineEditorHint"));
    hint_->setStyleSheet(QStringLiteral("background:%1;color:%2;padding:1px 6px;border-radius:3px").arg(t.accent, t.accent_text));
    hint_->adjustSize();
    hint_->hide();
    text_style::install(this);  // (the chosen characters made larger, bolder or coloured without typing the notation)
}

InlineEditor::~InlineEditor() {
    if (!hint_.isNull()) hint_->deleteLater();
}

void InlineEditor::place(double x, double y) {
    setGeometry(static_cast<int>(x), static_cast<int>(y), 260, 110);
    QWidget* parent = parentWidget();
    const int hx = std::max(0, std::min(static_cast<int>(x), (parent != nullptr ? parent->width() : 10000) - hint_->width()));
    hint_->move(hx, std::max(0, static_cast<int>(y) - hint_->height()));
    show();
    hint_->show();
    hint_->raise();
    raise();
    setFocus();
    moveCursor(QTextCursor::End);
}

void InlineEditor::finish(bool keep) {
    if (finished_) return;
    finished_ = true;
    const QString text = lettering::strip(toPlainText());
    // (the keys go back to the page it was typed on: hidden with the focus, Qt would hand it to whatever widget comes
    // next in the window's chain)
    if (hasFocus() && parentWidget() != nullptr) parentWidget()->setFocus(Qt::OtherFocusReason);
    hide();
    // (it goes after this event: taken off the canvas now, so the canvas never finds it again)
    if (!hint_.isNull()) {
        hint_->hide();
        hint_->setParent(nullptr);
        hint_->deleteLater();
    }
    setParent(nullptr);
    deleteLater();
    const auto done = on_done_;
    if (done) done(keep ? std::optional<QString>(text) : std::nullopt);
}

void InlineEditor::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        finish(false);
        return;
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && (event->modifiers() & Qt::ControlModifier) && preedit_.isEmpty()) {
        finish(true);
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

void InlineEditor::focusOutEvent(QFocusEvent* event) {
    QPlainTextEdit::focusOutEvent(event);
    if (finished_ || property("menuOpen").toBool() || event->reason() == Qt::PopupFocusReason) return;  // (its own menu: still typing)
    finish(true);
}

void InlineEditor::inputMethodEvent(QInputMethodEvent* event) {
    preedit_ = event->preeditString();  // (the words being converted: shown in the text, not yet the line's)
    QPlainTextEdit::inputMethodEvent(event);
}

}  // namespace genko::app
