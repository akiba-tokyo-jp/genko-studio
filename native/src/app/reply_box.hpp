#pragma once

#include <QPlainTextEdit>
#include <QString>

#include <map>
#include <string>
#include <utility>

// The person's words to the AI (C++ only: SPEC UX-02, improvement I13 — Python's one-line QLineEdit): a reply to a
// question, the reason a request is sent back, a panel's instruction. Several lines and paragraphs, typed with the
// input method, scrolled when long. Enter (and the input method's own Enter while it converts) makes a new line;
// Ctrl+Enter (⌘+Enter) sends — never a plain Enter. What is not sent yet is kept as a draft for each book and each
// request (drafts()), so choosing another request, the book changing under it, another tab or a window closed and opened
// again bring it back; it goes when it is sent.

namespace genko::app {

class ReplyEdit : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit ReplyEdit(QWidget* parent = nullptr);

    // The draft this editor shows and keeps (book: the book's folder or id; item: the request, the panel). Switching
    // keeps the text typed so far under the old key first.
    void bind(const QString& book, const QString& item);
    const QString& book() const { return book_; }
    const QString& item() const { return item_; }
    // The text as it was sent: the draft is gone, the editor empty.
    void sent();

signals:
    // Ctrl+Enter: send what is written.
    void sendRequested();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    QString book_;
    QString item_;
    bool binding_ = false;
};

// The drafts not sent yet, in this run of the app: by book, then by request. (Kept in memory only: written nowhere.)
class Drafts {
public:
    static Drafts& instance();
    QString get(const QString& book, const QString& item) const;
    void set(const QString& book, const QString& item, const QString& text);
    void forget(const QString& book, const QString& item);
    // How many requests of a book have words not sent yet.
    int count(const QString& book) const;

private:
    std::map<std::pair<QString, QString>, QString> drafts_;
};

}  // namespace genko::app
