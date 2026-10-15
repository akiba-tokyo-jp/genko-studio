#include "app/migration.hpp"

#include <QApplication>
#include <QDir>
#include <QLockFile>
#include <QProgressDialog>

#include <algorithm>
#include <memory>
#include <system_error>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "core/config_dir.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "storage/fsutil.hpp"

namespace genko::app::migration {

namespace fs = std::filesystem;

namespace {

// A path for the person (one that cannot be written as UTF-8 is said so, never thrown from here).
QString shown(const fs::path& path) {
    try {
        return QDir::toNativeSeparators(QString::fromStdString(core::path_to_utf8(path)));
    } catch (const std::exception&) {
        return QStringLiteral("（読めない名前）");
    }
}

// Why something went wrong, in words that keep the path's letters (our errors are UTF-8; the system's in its own).
QString why(const std::exception& error) {
    if (dynamic_cast<const core::Error*>(&error) != nullptr) return QString::fromUtf8(error.what());
    if (const auto* system = dynamic_cast<const fs::filesystem_error*>(&error)) return QString::fromStdString(system->code().message());
    return QString::fromLocal8Bit(error.what());
}

bool same_folder(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::equivalent(a, b, ec)) return true;
    return fs::weakly_canonical(a, ec).lexically_normal() == fs::weakly_canonical(b, ec).lexically_normal();
}

}  // namespace

bool kept_back(const fs::path& relative) {
    const auto first = relative.begin();
    if (first == relative.end()) return true;
    const std::string top = core::path_to_utf8(*first);
    for (const char* back : {"recovery", "cache", "thumbs", "fonts.json", "plugin-runs", "settings.ini", "migration.lock"}) {
        if (top == back) return true;
    }
    const std::string name = core::path_to_utf8(relative.filename());
    for (const char* end : {".lock", ".tmp", ".part", "~"}) {
        if (name.ends_with(end)) return true;
    }
    return name.starts_with(".#");
}

Found find(const fs::path& legacy_dir, const QSettings* legacy) {
    Found found;
    if (legacy != nullptr) {
        found.keys = legacy->allKeys();
        found.keys.removeIf([](const QString& key) { return key == QLatin1String(kDoneKey); });
    }
    std::error_code ec;
    if (!fs::is_directory(legacy_dir, ec)) return found;
    fs::recursive_directory_iterator walk(legacy_dir, fs::directory_options::none, ec);
    if (ec) found.unreadable << QStringLiteral("%1（%2）").arg(shown(legacy_dir), QString::fromStdString(ec.message()));
    for (; !ec && walk != fs::recursive_directory_iterator(); walk.increment(ec)) {
        const fs::path entry = walk->path();
        try {
            const fs::path relative = entry.lexically_relative(legacy_dir);
            std::error_code own;  // (the entry's own questions: never the walk's)
            const fs::file_status itself = walk->symlink_status(own);
            if (own) {
                found.unreadable << QStringLiteral("%1（%2）").arg(shown(relative), QString::fromStdString(own.message()));
                continue;
            }
            const bool link = fs::is_symlink(itself);
            if (kept_back(relative)) {
                if (!link && fs::is_directory(itself)) walk.disable_recursion_pending();
                continue;
            }
            if (link) {
                // (followed only to see what it is: a file's bytes are copied, a folder is left — never a link that
                // would let this app write into the Python app's things)
                const fs::file_status target = fs::status(entry, own);
                if (!own && fs::is_regular_file(target)) {
                    found.files.push_back(relative);
                } else if (!own && fs::is_directory(target)) {
                    found.folder_links.push_back(relative);
                } else {
                    found.unreadable << QStringLiteral("%1（リンクの先がありません）").arg(shown(relative));
                }
            } else if (fs::is_regular_file(itself)) {
                found.files.push_back(relative);
            }
        } catch (const std::exception& error) {  // (a name that cannot be read)
            found.unreadable << QStringLiteral("%1（%2）").arg(shown(entry), why(error));
        }
    }
    if (ec) found.unreadable << QStringLiteral("%1 の中を読めませんでした（%2）").arg(shown(legacy_dir), QString::fromStdString(ec.message()));
    std::sort(found.files.begin(), found.files.end());
    return found;
}

Copied copy(const Found& found, const fs::path& legacy_dir, const QSettings* legacy, const fs::path& dir, QSettings& settings,
            const std::function<void(int, int)>& progress) {
    Copied out;
    out.failed = found.unreadable;
    if (legacy != nullptr) {
        for (const QString& key : found.keys) {
            if (!settings.contains(key)) settings.setValue(key, legacy->value(key));
        }
    }
    std::error_code ec;
    const bool made = !fs::exists(dir, ec);
    try {
        storage::make_dirs_durable(dir);
        if (made) fs::permissions(dir, fs::status(legacy_dir).permissions(), fs::perm_options::replace, ec);  // (an owner-only folder stays so)
    } catch (const std::exception& error) {
        out.failed << QStringLiteral("%1（%2）").arg(shown(dir), why(error));
        return out;
    }
    int done = 0;
    const int all = static_cast<int>(found.files.size());
    for (const fs::path& relative : found.files) {
        if (progress) progress(done, all);
        ++done;
        const fs::path from = legacy_dir / relative;
        const fs::path to = dir / relative;
        fs::path part = to;
        part += ".part";
        if (fs::exists(fs::symlink_status(to, ec))) {  // this app's own already (said, unless it is the same file copied before)
            bool same = false;
            try {
                same = fs::is_regular_file(fs::symlink_status(to)) && fs::file_size(to) == fs::file_size(from) &&
                       storage::sha256_file(to) == storage::sha256_file(from);
            } catch (const std::exception&) {
                same = false;
            }
            if (!same) out.own_kept << shown(relative);
            continue;
        }
        try {
            storage::make_dirs_durable(to.parent_path());
            fs::remove(part, ec);
            // (the file's bytes — a link's file's — made with the file's own permissions: tokens.json stays readable by
            // its owner only)
            fs::copy_file(from, part, fs::copy_options::none);
            fs::permissions(part, fs::status(from).permissions(), fs::perm_options::replace);
            storage::rename_new(part, to);
        } catch (const std::exception& error) {
            out.failed << QStringLiteral("%1（%2）").arg(shown(relative), why(error));
            fs::remove(part, ec);
        }
    }
    if (progress) progress(all, all);
    settings.sync();
    if (settings.status() != QSettings::NoError) out.failed << QStringLiteral("環境設定（%1）").arg(QDir::toNativeSeparators(settings.fileName()));
    return out;
}

QString offer(QWidget* parent, const Places& places) {
    if (places.settings == nullptr) return {};
    QSettings& settings = *places.settings;
    if (const QString done = settings.value(QLatin1String(kDoneKey)).toString(); !done.isEmpty()) return done;
    // (one start of the app at a time: two started together never ask twice nor copy over each other)
    std::error_code ec;
    const bool made = !fs::exists(places.dir, ec);
    try {
        storage::make_dirs_durable(places.dir);
    } catch (const std::exception&) {
        return {};
    }
    if (made && fs::is_directory(places.legacy_dir, ec)) {  // (an owner-only folder stays so)
        fs::permissions(places.dir, fs::status(places.legacy_dir, ec).permissions(), fs::perm_options::replace, ec);
    }
    QLockFile lock(QString::fromStdString(core::path_to_utf8(places.dir / "migration.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return {};
    settings.sync();  // (the other start may have answered meanwhile)
    if (const QString done = settings.value(QLatin1String(kDoneKey)).toString(); !done.isEmpty()) return done;
    const Found found = find(places.legacy_dir, places.legacy);
    QString answer;
    const QString title = QStringLiteral("以前の Genko Studio の設定");
    if (found.empty()) {
        answer = QStringLiteral("none");
    } else if (!ask::question(parent, title,
                              QStringLiteral("以前の Genko Studio（Python 版）の設定が見つかりました。\n"
                                             "環境設定・最近の原稿・自作のブラシ・色・ポーズ・素材・オートアクション・AI 接続の鍵・プラグインを、"
                                             "この Genko Studio に引き継ぎますか？\n\n"
                                             "以前の版の設定はそのまま残り、変更しません。引き継がなくても、以前の版は今までどおり使えます。"))) {
        answer = QStringLiteral("declined");
    } else {
        std::unique_ptr<QProgressDialog> waiting;
        if (ask::responder() == nullptr && QApplication::instance() != nullptr && found.files.size() > 50) {
            waiting = std::make_unique<QProgressDialog>(QStringLiteral("以前の設定を引き継いでいます…"), QString(), 0,
                                                        static_cast<int>(found.files.size()), parent);
            waiting->setWindowTitle(title);
            waiting->setCancelButton(nullptr);
            waiting->setWindowModality(Qt::ApplicationModal);
            waiting->setMinimumDuration(500);
        }
        const Copied copied = copy(found, places.legacy_dir, places.legacy, places.dir, settings, [&waiting](int done, int) {
            if (waiting) waiting->setValue(done);
        });
        waiting.reset();
        const auto listed = [](const QStringList& items) {
            return items.mid(0, 12).join(QLatin1Char('\n')) + (items.size() > 12 ? QStringLiteral("\nほか %1 件").arg(items.size() - 12) : QString());
        };
        QStringList left;
        for (const fs::path& link : found.folder_links) left << shown(link);
        QString note;
        if (!left.isEmpty()) {
            note += QStringLiteral("\n\nフォルダーへのリンクは写していません（この版の変更が以前の版のフォルダーに及ばないように）。"
                                   "要るときは、中身をこの版の設定のフォルダー（%1）へ写してください:\n%2")
                        .arg(shown(places.dir), listed(left));
        }
        if (!copied.own_kept.isEmpty()) {
            note += QStringLiteral("\n\nこの版にすでにあるため、写さずにこの版のものを残しました:\n%1").arg(listed(copied.own_kept));
        }
        if (!copied.failed.isEmpty()) {
            ask::warning(parent, title,
                         QStringLiteral("次のものを引き継げませんでした（引き継げたものはそのまま使えます。次に起動したときにもう一度聞きます）:\n%1%2")
                             .arg(listed(copied.failed), note));
            return {};
        }
        if (!note.isEmpty()) ask::warning(parent, title, QStringLiteral("引き継ぎました。") + note);
        answer = QStringLiteral("copied");
    }
    settings.setValue(QLatin1String(kDoneKey), answer);
    settings.sync();
    return answer;
}

QString offer_on_first_start(QWidget* parent) {
    if (!qEnvironmentVariableIsEmpty("GENKO_CONFIG_DIR")) return {};
    const fs::path dir = core::config_dir();
    const fs::path legacy_dir = core::legacy_config_dir();
    if (same_folder(dir, legacy_dir)) return {};
    const std::unique_ptr<QSettings> legacy = legacy_settings();
    const std::unique_ptr<QSettings> own = settings();
    return offer(parent, Places{legacy_dir, legacy.get(), dir, own.get()});
}

}  // namespace genko::app::migration
