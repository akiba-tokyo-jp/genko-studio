#include "app/history_log.hpp"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <map>
#include <filesystem>
#include <memory>
#include <vector>

#include "app/config.hpp"
#include "app/documents.hpp"
#include "app/history_panel.hpp"
#include "app/main_window.hpp"
#include "app/session.hpp"
#include "core/config_dir.hpp"
#include "core/paths.hpp"
#include "core/prefs_table.hpp"
#include "formats/copy.hpp"
#include "formats/file_xmp.hpp"
#include "storage/journal.hpp"

namespace genko::app::history_log {

namespace fs = std::filesystem;
using core::Json;

namespace {

constexpr std::size_t kMetadataMost = 1000;  // (節 H 9: the latest changes only — a journal of tens of thousands)

struct Chosen {
    bool text = false;
    bool metadata = false;
    QString detail;
    QString file;
};

Chosen chosen() {
    const auto store = core::prefs::store();  // (not the app's own store: read on an export's thread too)
    Chosen out;
    if (!core::prefs::value(*store, "historyLog.enabled").get<bool>()) return out;
    const std::string where = core::prefs::value(*store, "historyLog.destination").get<std::string>();
    out.detail = QString::fromStdString(core::prefs::value(*store, "historyLog.detail").get<std::string>());
    out.file = QString::fromStdString(core::prefs::value(*store, "historyLog.filePath").get<std::string>()).trimmed();
    out.text = (where == "textFile" || where == "both") && !out.file.isEmpty();
    out.metadata = where == "metadata" || where == "both";
    return out;
}

QString title_of(const Session& session) {
    const auto doc = session.snapshot();
    if (doc && !doc->title.empty()) return QString::fromStdString(doc->title);
    if (session.path()) return QString::fromStdString(core::path_to_utf8(session.path()->filename()));
    return QStringLiteral("無題");
}

QString op_names(const Json& ops) {
    QStringList names;
    if (ops.is_array()) {
        for (const Json& op : ops) {
            if (op.is_object() && op.contains("op") && op["op"].is_string()) names << QString::fromStdString(op["op"].get<std::string>());
        }
    }
    return names.join(QStringLiteral(", "));
}

QSet<QString> said;  // (a file refused or not written: said once)

void say_once(const QString& file, const QString& words) {
    if (said.contains(file)) return;
    said.insert(file);
    for (MainWindow* window : documents::windows()) {
        if (window->isActiveWindow() || documents::windows().size() == 1) {
            window->flash(words, 8000, true);
            break;
        }
    }
}

// What a followed book's lines are kept from, taken while it is open (its folder, the picture it was opened from, its
// book as last changed — the files its smart objects link): its 閉じる line, written when the book is gone, is kept
// from the same (review P2-2).
struct Material {
    std::optional<fs::path> path;
    std::optional<fs::path> source;
    std::shared_ptr<const core::Document> doc;
};
std::map<const Session*, std::shared_ptr<Material>> followed;

// Whether a line may go to `dest` — never a book's folder or what lies in it, a file a book's smart object links, a picture
// a book was opened from (this book and every one open now: formats::write_guard, as the exports' files are kept from
// them), nor a file of this computer's config folder (its settings, its tokens, session.json — review P3-3).
bool may_write(const fs::path& dest, const Material& own) {
    try {
        const fs::path config = core::config_dir().lexically_normal();
        const fs::path in_config = dest.lexically_normal().lexically_relative(config);
        if (!in_config.empty() && *in_config.begin() != "..") return false;
        formats::WriteGuard guard;
        const auto take = [&guard](const core::Document* doc, const std::optional<fs::path>& path, const std::optional<fs::path>& source) {
            if (doc != nullptr) {
                formats::WriteGuard one = formats::write_guard(*doc, path);
                guard.books.insert(guard.books.end(), one.books.begin(), one.books.end());
                guard.linked.insert(guard.linked.end(), one.linked.begin(), one.linked.end());
                guard.sources.insert(guard.sources.end(), one.sources.begin(), one.sources.end());
            } else if (path) {
                guard.books.push_back(*path);
            }
            if (source) guard.sources.push_back(*source);
        };
        take(own.doc.get(), own.path, own.source);
        for (MainWindow* window : documents::windows()) {
            for (const Document& d : window->documents()) {
                if (!d.session) continue;
                const auto snapshot = d.session->snapshot();
                take(snapshot.get(), d.session->path(), d.source);
            }
        }
        formats::check_target(guard, dest);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// The line appended to the chosen file when may_write lets it.
void append(const Material& own, const QString& text) {
    const Chosen c = chosen();
    if (!c.text) return;
    const fs::path dest = core::path_from_utf8(c.file.toStdString());
    if (!may_write(dest, own)) {
        say_once(c.file, QStringLiteral("ヒストリーログを「%1」に書けません（原稿・原稿の中・原稿が使うファイルには書きません）。"
                                        "編集 → 環境設定 → ヒストリーログ でほかの場所を選びます")
                             .arg(c.file));
        return;
    }
    QFile out(c.file);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Append)) {
        say_once(c.file, QStringLiteral("ヒストリーログのファイル「%1」に書けません").arg(c.file));
        return;
    }
    const QByteArray bytes = (text + QLatin1Char('\n')).toUtf8();
    out.write(bytes);
    said.remove(c.file);
}

QString stamp() { return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")); }

}  // namespace

QString line(const QString& title, const QString& words, const Json& ops, bool detailed) {
    QString out = stamp() + QLatin1Char('\t') + title + QLatin1Char('\t') + words;
    if (detailed) {
        const QString names = op_names(ops);
        if (!names.isEmpty()) out += QLatin1Char('\t') + names;
    }
    return out;
}

void follow(Session* session, std::optional<std::filesystem::path> source) {
    if (session == nullptr) return;
    if (const auto known = followed.find(session); known != followed.end()) {
        if (source) known->second->source = std::move(source);  // (a picture's book: its file, known once its tab has it)
        return;
    }
    auto own = std::make_shared<Material>(Material{session->path(), std::move(source), session->snapshot()});
    followed[session] = own;
    auto title = std::make_shared<QString>(title_of(*session));
    if (chosen().text) append(*own, line(*title, QStringLiteral("開く"), Json(), false));
    QObject::connect(session, &Session::changed, session, [session, title, own](const BookChange& change) {
        *title = title_of(*session);
        own->path = session->path();
        own->doc = session->snapshot();
        const Chosen c = chosen();
        if (!c.text || c.detail == QLatin1String("sessionsOnly")) return;
        QString words;
        Json ops;
        switch (change.why) {
            case BookChange::Why::Edit: words = history::describe(change.ops); ops = change.ops; break;
            case BookChange::Why::Undo: words = QStringLiteral("取り消し"); break;
            case BookChange::Why::Redo: words = QStringLiteral("やり直し: ") + history::describe(change.ops); ops = change.ops; break;
            case BookChange::Why::Recover: words = QStringLiteral("復旧用コピーを採用"); break;
            case BookChange::Why::SaveAs: words = QStringLiteral("別名で保存"); break;
            default: return;  // (another's change taken in, the book read again: not this person's edit)
        }
        append(*own, line(*title, words, ops, c.detail == QLatin1String("detailed")));
    });
    QObject::connect(session, &QObject::destroyed, [session, title, own] {
        // (the book closed: kept from what it was kept from while open — and from the books still open — review P2-2)
        followed.erase(session);
        append(*own, line(*title, QStringLiteral("閉じる"), Json(), false));
    });
}

std::optional<std::string> metadata(const core::Document& doc) {
    const Chosen c = chosen();
    if (!c.metadata || !doc.asset_dir) return std::nullopt;
    const storage::journal::Lines lines = storage::journal::read_lines(storage::journal::journal_file(*doc.asset_dir));
    std::vector<const Json*> done;
    const auto transactions = storage::journal::transactions(lines);
    for (const storage::journal::Transaction& t : transactions) {
        if (t.status == storage::journal::Transaction::Status::committed) done.push_back(&t.prepare);
    }
    if (done.empty()) return std::nullopt;
    if (done.size() > kMetadataMost) done.erase(done.begin(), done.end() - static_cast<std::ptrdiff_t>(kMetadataMost));
    const auto when = [](const Json& prepare) {
        const double at = prepare.value("at", 0.0);
        return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(at * 1000.0)).toString(Qt::ISODate);
    };
    QStringList out;
    if (c.detail == QLatin1String("sessionsOnly")) {
        out << QStringLiteral("%1 〜 %2: %3 件の編集").arg(when(*done.front()), when(*done.back())).arg(done.size());
    } else {
        for (const Json* prepare : done) {
            const Json ops = prepare->value("ops", Json::array());
            const std::string action = prepare->value("action", std::string());
            QString words = action == "undo" ? QStringLiteral("取り消し") : action == "redo" ? QStringLiteral("やり直し") : history::describe(ops);
            QString row = when(*prepare) + QLatin1Char('\t') + words;
            if (c.detail == QLatin1String("detailed")) {
                if (const QString names = op_names(ops); !names.isEmpty()) row += QLatin1Char('\t') + names;
            }
            out << row;
        }
    }
    return out.join(QLatin1Char('\n')).toStdString();
}

void install() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    formats::set_history_source([](const core::Document& doc) { return metadata(doc); });
}

}  // namespace genko::app::history_log
