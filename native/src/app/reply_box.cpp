#include "app/reply_box.hpp"

#include <QKeyEvent>
#include <QTextBlock>
#include <QTextLayout>

namespace genko::app {

ReplyEdit::ReplyEdit(QWidget* parent) : QPlainTextEdit(parent) {
    setTabChangesFocus(true);  // (Tab goes on to the buttons; the reply is words, not a table)
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setMinimumHeight(fontMetrics().lineSpacing() * 3 + 12);
    setMaximumHeight(fontMetrics().lineSpacing() * 8 + 12);
    setToolTip(QStringLiteral("Enter で改行、Ctrl+Enter で送ります"));
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        if (binding_ || book_.isEmpty() || item_.isEmpty()) return;
        Drafts::instance().set(book_, item_, toPlainText());
    });
}

void ReplyEdit::bind(const QString& book, const QString& item) {
    if (book == book_ && item == item_) return;
    binding_ = true;
    if (!book_.isEmpty() && !item_.isEmpty()) Drafts::instance().set(book_, item_, toPlainText());
    book_ = book;
    item_ = item;
    const QString draft = book.isEmpty() || item.isEmpty() ? QString() : Drafts::instance().get(book, item);
    setPlainText(draft);
    moveCursor(QTextCursor::End);
    binding_ = false;
}

void ReplyEdit::sent() {
    binding_ = true;
    if (!book_.isEmpty() && !item_.isEmpty()) Drafts::instance().forget(book_, item_);
    clear();
    binding_ = false;
}

void ReplyEdit::keyPressEvent(QKeyEvent* event) {
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    // (while the input method is still converting, its words are not sent half-made)
    const QTextLayout* layout = textCursor().block().layout();
    const bool converting = layout != nullptr && !layout->preeditAreaText().isEmpty();
    if (enter && converting) {  // (the input method's own Enter: it settles the words, it does not send or break the line)
        event->accept();
        return;
    }
    if (enter && (event->modifiers() & Qt::ControlModifier)) {
        event->accept();
        emit sendRequested();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);  // (Enter: a new line)
}

Drafts& Drafts::instance() {
    static Drafts drafts;
    return drafts;
}

QString Drafts::get(const QString& book, const QString& item) const {
    const auto it = drafts_.find({book, item});
    return it == drafts_.end() ? QString() : it->second;
}

void Drafts::set(const QString& book, const QString& item, const QString& text) {
    if (text.trimmed().isEmpty()) {
        drafts_.erase({book, item});
    } else {
        drafts_[{book, item}] = text;
    }
}

void Drafts::forget(const QString& book, const QString& item) { drafts_.erase({book, item}); }

int Drafts::count(const QString& book) const {
    int n = 0;
    for (const auto& [key, text] : drafts_) {
        if (key.first == book && !text.trimmed().isEmpty()) ++n;
    }
    return n;
}

}  // namespace genko::app
