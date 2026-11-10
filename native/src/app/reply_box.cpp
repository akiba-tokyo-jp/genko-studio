#include "app/reply_box.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QMutexLocker>
#include <QKeyEvent>
#include <QTextBlock>
#include <QTextLayout>

#include <algorithm>
#include <system_error>
#include <vector>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "service/wording.hpp"
#include "storage/fsutil.hpp"
#include "storage/transaction.hpp"

namespace genko::app {

using core::Json;
namespace fs = std::filesystem;

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

// --- the drafts kept in the book ---------------------------------------------------------------------------------

namespace reply_drafts {

namespace {

// A draft of a format this build does not know ("genko-reply-draft@2", …): left as it is, neither read nor removed.
const QString kForeign = QStringLiteral("\x01foreign");

// Why a draft file cannot be read (empty: it can; kForeign: another format of drafts), and its item and words.
QString check(const fs::path& path, QString& item, QString& text) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec || !fs::is_regular_file(status)) return QStringLiteral("ふつうのファイルではありません");
    std::string bytes;
    try {
        bytes = storage::read_file_bounded(path, kMaxFile);
    } catch (const core::Error& error) {
        if (error.code() == "memory") return QStringLiteral("大きすぎます（%1 KiB まで）").arg(kMaxFile / 1024);
        if (error.code() == "format") return QStringLiteral("ふつうのファイルではありません");
        return QStringLiteral("読めません（%1）").arg(QString::fromStdString(error.what()));
    }
    Json data;
    try {
        if (core::utf8_error(bytes)) return QStringLiteral("UTF-8 の JSON として読めません");
        data = core::parse_python_json(bytes);
    } catch (const core::Error&) {
        return QStringLiteral("UTF-8 の JSON として読めません");
    }
    if (data.is_object() && data.value("format", Json()).is_string() && data["format"] != Json(kFormat) &&
        data["format"].get_ref<const std::string&>().starts_with("genko-reply-draft@")) {
        return kForeign;  // (a newer Genko's: kept for it)
    }
    if (!data.is_object() || data.value("format", Json()) != Json(kFormat) || !data.contains("item") || !data["item"].is_string() ||
        !data.contains("text") || !data["text"].is_string() || (data.contains("at") && !data["at"].is_string())) {
        return QStringLiteral("返事の下書きの形式（%1）ではありません").arg(QString::fromLatin1(kFormat));
    }
    const std::string& words = data["text"].get_ref<const std::string&>();
    if (words.size() > kMaxText) return QStringLiteral("言葉が長すぎます（%1 KiB まで）").arg(kMaxText / 1024);
    item = QString::fromStdString(data["item"].get<std::string>());
    if (item.isEmpty() || storage::reply_drafts::file_name(item.toStdString()) != core::path_to_utf8(path.filename())) {
        return QStringLiteral("ファイルの名前が中身の依頼と合いません");
    }
    text = QString::fromStdString(words);
    return QString();
}

}  // namespace

fs::path folder(const fs::path& book) { return storage::reply_drafts::folder(book); }

fs::path file(const fs::path& book, const QString& item) {
    return folder(book) / core::path_from_utf8(storage::reply_drafts::file_name(item.toStdString()));
}

std::string encode(const QString& item, const QString& text, const QDateTime& at) {
    Json data = Json::object();
    data["format"] = kFormat;
    data["item"] = item.toStdString();
    data["text"] = text.toStdString();
    data["at"] = at.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")).toStdString();
    return core::dump_python(data) + "\n";
}

Read read(const fs::path& book) {
    Read out;
    const fs::path dir = folder(book);
    std::error_code ec;
    // (a folder that is a link is not followed: what it points to is not the book's)
    if (!fs::is_directory(fs::symlink_status(dir, ec))) return out;
    int seen = 0;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path path = it->path();
        const std::string name = core::path_to_utf8(path.filename());
        if (!storage::reply_drafts::is_draft_name(name)) continue;  // (a write's temporary file, someone else's)
        if (seen >= kMaxFiles) {
            ++out.left;
            continue;
        }
        ++seen;
        QString item;
        QString text;
        const QString why = check(path, item, text);
        if (why.isEmpty()) {
            out.drafts[item] = text;
            continue;
        }
        if (why == kForeign) {
            out.foreign << QString::fromStdString(name);
            continue;
        }
        std::error_code gone;
        fs::remove(path, gone);
        out.discarded << QStringLiteral("%1（%2%3）").arg(QString::fromStdString(name), why,
                                                         gone ? QStringLiteral("。消せませんでした") : QString());
    }
    return out;
}

}  // namespace reply_drafts

Drafts& Drafts::instance() {
    static Drafts* drafts = new Drafts;  // (never destroyed: it may be used until the app's last window goes)
    return *drafts;
}

Drafts::Drafts() {
    timer_.setSingleShot(true);
    timer_.setInterval(500);
    connect(&timer_, &QTimer::timeout, this, &Drafts::start_writes);
    if (QCoreApplication* app = QCoreApplication::instance()) connect(app, &QCoreApplication::aboutToQuit, this, &Drafts::flush);
    // (the files are written on a worker of their own, one at a time and in order: an fsync while the book itself is
    // being saved takes 100 ms and more, which the window must not wait for)
    files_.setMaxThreadCount(1);
}

const Drafts::Place* Drafts::place_of(const QString& book) const {
    const auto it = places_.find(book);
    return it == places_.end() ? nullptr : &it->second;
}

bool Drafts::kept_on_disk(const QString& book) {
    const Place* place = load(book);
    return place != nullptr && place->keep;
}

void Drafts::place(const QString& book, const std::optional<fs::path>& folder, bool keep) {
    const fs::path dir = folder.value_or(fs::path());
    const bool asked = keep && folder.has_value();
    if (const auto it = places_.find(book); it != places_.end() && it->second.folder == dir && it->second.asked == asked) return;
    Place next;  // (read the first time its drafts are wanted: load())
    next.folder = dir;
    next.asked = asked;
    places_[book] = next;
}

const Drafts::Place* Drafts::load(const QString& book) {
    const auto it = places_.find(book);
    if (it == places_.end()) return nullptr;
    Place& place = it->second;
    if (place.loaded || !place.asked) {
        place.loaded = true;
        return &place;
    }
    place.loaded = true;
    try {
        place.keep = storage::read_disk_state(place.folder).version >= 4;  // (v1–v3 books are not written: NeedsMigration)
    } catch (const core::Error&) {
        place.keep = false;
    }
    if (!place.keep) return &place;
    const reply_drafts::Read found = reply_drafts::read(place.folder);
    for (const auto& [item, text] : found.drafts) drafts_.emplace(Key{book, item}, text);  // (the words in memory win)
    for (const auto& [key, text] : drafts_) {
        if (key.first != book) continue;
        const auto on_disk = found.drafts.find(key.second);
        if (on_disk == found.drafts.end() || on_disk->second != text) dirty_.insert(key);
    }
    if (!dirty_.empty()) timer_.start();
    if (!found.discarded.isEmpty()) {
        emit problem(book, QStringLiteral("返事の下書きのファイルが壊れていたため、読まずに捨てました: %1")
                               .arg(found.discarded.join(QStringLiteral("、"))));
    }
    if (!found.foreign.isEmpty()) {
        emit problem(book, QStringLiteral("この版の Genko が知らない形式の返事の下書きがあります。読まずに、そのまま残しました: %1")
                               .arg(found.foreign.join(QStringLiteral("、"))));
    }
    return place_of(book);
}

void Drafts::carry(const QString& from, const QString& to) {
    if (from == to) return;
    load(from);
    std::vector<std::pair<QString, QString>> moved;
    for (const auto& [key, text] : drafts_) {
        if (key.first == from) moved.emplace_back(key.second, text);
    }
    for (const auto& [item, text] : moved) set(to, item, text);
}

QString Drafts::get(const QString& book, const QString& item) {
    load(book);
    const auto it = drafts_.find({book, item});
    return it == drafts_.end() ? QString() : it->second;
}

void Drafts::set(const QString& book, const QString& item, const QString& text) {
    load(book);
    const Key key{book, item};
    const auto it = drafts_.find(key);
    const bool blank = text.trimmed().isEmpty();
    if (blank ? it == drafts_.end() : (it != drafts_.end() && it->second == text)) return;
    if (blank) {
        drafts_.erase(key);
    } else {
        drafts_[key] = text;
    }
    if (kept_on_disk(book)) {
        dirty_.insert(key);
        timer_.start();
    }
}

void Drafts::forget(const QString& book, const QString& item) {
    load(book);
    const Key key{book, item};
    drafts_.erase(key);
    dirty_.erase(key);
    too_long_.erase(key);
    if (kept_on_disk(book)) remove_file(key);
}

int Drafts::count(const QString& book) {
    load(book);
    int n = 0;
    for (const auto& [key, text] : drafts_) {
        if (key.first == book && !text.trimmed().isEmpty()) ++n;
    }
    return n;
}

void Drafts::start_writes() {
    timer_.stop();
    const std::set<Key> waiting = std::exchange(dirty_, {});
    for (const Key& key : waiting) write(key);
}

void Drafts::flush() {
    start_writes();
    files_.waitForDone(30000);
    report();
}

void Drafts::write(const Key& key) {
    const Place* place = place_of(key.first);
    if (place == nullptr || !place->keep) return;
    const auto it = drafts_.find(key);
    if (it == drafts_.end() || it->second.trimmed().isEmpty()) {
        too_long_.erase(key);
        remove_file(key);
        return;
    }
    if (static_cast<std::size_t>(it->second.toUtf8().size()) > reply_drafts::kMaxText) {
        remove_file(key);  // (an older, shorter copy would bring back words that are not the ones typed)
        if (too_long_.insert(key).second) {
            emit problem(key.first, QStringLiteral("返事の下書きが長すぎるため（%1 KiB まで）、原稿には残しません。送るか、短くしてください")
                                        .arg(reply_drafts::kMaxText / 1024));
        }
        return;
    }
    too_long_.erase(key);
    queue(key.first, place->folder, reply_drafts::file(place->folder, key.second),
          reply_drafts::encode(key.second, it->second, QDateTime::currentDateTime()));
}

void Drafts::remove_file(const Key& key) {
    const Place* place = place_of(key.first);
    if (place == nullptr || !place->keep) return;
    queue(key.first, place->folder, reply_drafts::file(place->folder, key.second), std::nullopt);
}

void Drafts::queue(const QString& book, const fs::path& folder, const fs::path& file, std::optional<std::string> bytes) {
    files_.start([this, book, folder, file, bytes = std::move(bytes)] {
        const QString said = file_job(folder, file, bytes);
        if (said.isEmpty()) return;
        {
            const QMutexLocker locked(&said_lock_);
            said_.emplace_back(book, said);
        }
        QMetaObject::invokeMethod(this, &Drafts::report, Qt::QueuedConnection);
    });
}

QString Drafts::file_job(const fs::path& folder, const fs::path& file, const std::optional<std::string>& bytes) {
    std::error_code ec;
    // (studio/reply_drafts must be the book's own folder: a link, or a file, there is neither written through nor
    // emptied)
    const fs::file_status place = fs::symlink_status(file.parent_path(), ec);
    if (fs::exists(place) && !fs::is_directory(place)) {
        return QStringLiteral("返事の下書きの置き場所（studio/reply_drafts）が、原稿の中のフォルダーではありません（リンクなど）。下書きを書き込みも消しもしません");
    }
    if (!bytes) {
        if (!fs::remove(file, ec) && ec && ec != std::errc::no_such_file_or_directory) {
            return QStringLiteral("返事の下書きのファイルを消せませんでした: %1（%2）")
                .arg(QString::fromStdString(core::path_to_utf8(file)), QString::fromStdString(ec.message()));
        }
        return {};
    }
    if (!fs::is_regular_file(folder / "project.json", ec)) return {};  // (the book is gone: its folder is not made again)
    try {
        storage::write_atomic(file, *bytes);
    } catch (const core::Error& error) {
        return QStringLiteral("返事の下書きを原稿に書けませんでした: %1").arg(wording::error(std::string(error.what())));
    }
    return {};
}

void Drafts::report() {
    std::vector<std::pair<QString, QString>> said;
    {
        const QMutexLocker locked(&said_lock_);
        said.swap(said_);
    }
    for (const auto& [book, message] : said) emit problem(book, message);
}

}  // namespace genko::app
