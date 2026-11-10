#pragma once

#include <QDateTime>
#include <QObject>
#include <QPlainTextEdit>
#include <QString>
#include <QStringList>
#include <QMutex>
#include <QThreadPool>
#include <QTimer>

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "storage/reply_drafts.hpp"

// The person's words to the AI (C++ only: SPEC UX-02, improvement I13 — Python's one-line QLineEdit): a reply to a
// question, the reason a request is sent back, a panel's instruction. Several lines and paragraphs, typed with the
// input method, scrolled when long. Enter (and the input method's own Enter while it converts) makes a new line;
// Ctrl+Enter (⌘+Enter) sends — never a plain Enter. What is not sent yet is kept as a draft for each book and each
// request (Drafts), so choosing another request, the book changing under it, another tab, a window closed and opened
// again or the app started again bring it back; it goes when it is sent.

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

// The draft files in a book (schema-v4 §1 "studio/reply_drafts/"): one file for each request (or panel) with words
// not sent yet, `<sha256 of the item's key, 64 lowercase hex>.json` = {"format": "genko-reply-draft@1", "item": key,
// "text": words, "at": "YYYY-MM-DDTHH:MM:SS"}. Only in v4 books; not part of project.json, the journal or Undo.
namespace reply_drafts {

// (the format, its limits and the files gc removes: storage/reply_drafts.hpp)
using storage::reply_drafts::kFormat;
using storage::reply_drafts::kMaxFile;
using storage::reply_drafts::kMaxFiles;
using storage::reply_drafts::kMaxText;

std::filesystem::path folder(const std::filesystem::path& book);
std::filesystem::path file(const std::filesystem::path& book, const QString& item);
std::string encode(const QString& item, const QString& text, const QDateTime& at);

struct Read {
    std::map<QString, QString> drafts;  // item → words
    QStringList discarded;              // a line for each broken file thrown away: "<file>（<why>）"
    QStringList foreign;                // drafts of a format this build does not know (another Genko's): left as they are
    int left = 0;                       // files past kMaxFiles, not read
};
// The drafts of a book. A broken file (not a regular file, larger than kMaxFile, not UTF-8 JSON, another format, words
// longer than kMaxText, a name that is not the item's) is not read: it is removed and named in `discarded`. Other
// names (a write's temporary file) are left alone.
Read read(const std::filesystem::path& book);

}  // namespace reply_drafts

// The drafts not sent yet: by book, then by request. Kept in memory, and in the book's folder (reply_drafts) when the
// book is a v4 book that can be changed: written a moment after the typing stops (and at once by flush(): a window
// closed, the app quitting), each file replaced whole; removed when the words are sent or emptied.
class Drafts : public QObject {
    Q_OBJECT

public:
    static Drafts& instance();

    // Where a book's drafts are kept: `folder`, the book's folder when it has one; `keep`, whether drafts may be written
    // there (the book can be changed) — then only when it is a v4 book. The first time the book's drafts are wanted,
    // its version is looked at and the drafts kept there read (the words in memory win), broken files thrown away with
    // a problem().
    void place(const QString& book, const std::optional<std::filesystem::path>& folder, bool keep);
    // The book's drafts under another key too (Save As: the words go on in the new book's folder).
    void carry(const QString& from, const QString& to);

    QString get(const QString& book, const QString& item);
    void set(const QString& book, const QString& item, const QString& text);
    void forget(const QString& book, const QString& item);
    // How many requests of a book have words not sent yet.
    int count(const QString& book);
    // Write every draft waiting to be written, now, and wait until the files are written (a window closed, the app
    // quitting, tests). The pause after typing only starts the writes.
    void flush();
    // Whether a book's drafts are written in its folder (place()).
    bool kept_on_disk(const QString& book);
    // How long after the last change a draft is written (tests).
    void set_delay_ms(int ms) { timer_.setInterval(ms); }

signals:
    // A broken draft file thrown away, words too long to keep, a file that could not be written or removed.
    void problem(const QString& book, const QString& message);

private:
    Drafts();
    using Key = std::pair<QString, QString>;
    struct Place {
        std::filesystem::path folder;
        bool asked = false;   // (the window asked to keep them there)
        bool loaded = false;  // (looked at: keep is known, the drafts there are read)
        bool keep = false;    // (and it is a v4 book: they are read from and written there)
    };
    const Place* place_of(const QString& book) const;
    const Place* load(const QString& book);
    void start_writes();
    void write(const Key& key);
    void remove_file(const Key& key);
    // A file written (bytes) or removed (none) on the files' worker, in order; what went wrong comes back as a problem().
    void queue(const QString& book, const std::filesystem::path& folder, const std::filesystem::path& file, std::optional<std::string> bytes);
    static QString file_job(const std::filesystem::path& folder, const std::filesystem::path& file, const std::optional<std::string>& bytes);
    void report();

    std::map<QString, Place> places_;
    std::map<Key, QString> drafts_;
    std::set<Key> dirty_;
    std::set<Key> too_long_;
    QTimer timer_;
    QThreadPool files_;
    QMutex said_lock_;
    std::vector<std::pair<QString, QString>> said_;  // (book, what went wrong), from the worker
};

}  // namespace genko::app
