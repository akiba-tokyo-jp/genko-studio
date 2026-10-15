#include "app/migration.hpp"

#include <QDir>

#include <algorithm>
#include <system_error>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "core/config_dir.hpp"
#include "core/paths.hpp"
#include "storage/fsutil.hpp"

namespace genko::app::migration {

namespace fs = std::filesystem;

namespace {

QString shown(const fs::path& path) { return QDir::toNativeSeparators(QString::fromStdString(core::path_to_utf8(path))); }

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
    if (top == "recovery" || top == "cache" || top == "plugin-runs" || top == "settings.ini") return true;
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
    fs::recursive_directory_iterator walk(legacy_dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && walk != fs::recursive_directory_iterator(); walk.increment(ec)) {
        const fs::path relative = walk->path().lexically_relative(legacy_dir);
        const bool link = walk->is_symlink(ec);
        if (kept_back(relative)) {
            if (walk->is_directory(ec) && !link) walk.disable_recursion_pending();
            continue;
        }
        if (link || walk->is_regular_file(ec)) found.files.push_back(relative);  // (a link stays a link: never followed)
    }
    std::sort(found.files.begin(), found.files.end());
    return found;
}

QStringList copy(const Found& found, const fs::path& legacy_dir, const QSettings* legacy, const fs::path& dir, QSettings& settings) {
    QStringList failed;
    if (legacy != nullptr) {
        for (const QString& key : found.keys) {
            if (!settings.contains(key)) settings.setValue(key, legacy->value(key));
        }
    }
    for (const fs::path& relative : found.files) {
        const fs::path from = legacy_dir / relative;
        const fs::path to = dir / relative;
        std::error_code ec;
        if (fs::exists(fs::symlink_status(to, ec))) continue;  // this app's own already
        try {
            storage::make_dirs_durable(to.parent_path());
            fs::path part = to;
            part += ".part";
            fs::remove(part, ec);
            if (fs::is_symlink(fs::symlink_status(from))) {
                fs::copy_symlink(from, part);
            } else {
                // (copy_file makes the copy with the original's permissions: tokens.json stays readable by its owner only)
                fs::copy_file(from, part, fs::copy_options::none);
                fs::permissions(part, fs::status(from).permissions(), fs::perm_options::replace);
            }
            storage::rename_new(part, to);
        } catch (const std::exception& error) {
            failed << QStringLiteral("%1（%2）").arg(shown(relative), QString::fromLocal8Bit(error.what()));
            fs::path part = to;
            part += ".part";
            fs::remove(part, ec);
        }
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) failed << QStringLiteral("環境設定（%1）").arg(settings.fileName());
    return failed;
}

QString offer(QWidget* parent, const Places& places) {
    if (places.settings == nullptr) return {};
    QSettings& settings = *places.settings;
    if (const QString done = settings.value(QLatin1String(kDoneKey)).toString(); !done.isEmpty()) return done;
    const Found found = find(places.legacy_dir, places.legacy);
    QString answer;
    if (found.empty()) {
        answer = QStringLiteral("none");
    } else if (!ask::question(parent, QStringLiteral("以前の Genko Studio の設定"),
                              QStringLiteral("以前の Genko Studio（Python 版）の設定が見つかりました。\n"
                                             "環境設定・最近の原稿・自作のブラシ・色・ポーズ・素材・オートアクション・AI 接続の鍵・プラグインを、"
                                             "この Genko Studio に引き継ぎますか？\n\n"
                                             "以前の版の設定はそのまま残り、変更しません。引き継がなくても、以前の版は今までどおり使えます。"))) {
        answer = QStringLiteral("declined");
    } else {
        const QStringList failed = copy(found, places.legacy_dir, places.legacy, places.dir, settings);
        if (!failed.isEmpty()) {
            ask::warning(parent, QStringLiteral("以前の Genko Studio の設定"),
                         QStringLiteral("次のものを引き継げませんでした（引き継げたものはそのまま使えます。次に起動したときにもう一度聞きます）:\n%1")
                             .arg(failed.mid(0, 12).join(QLatin1Char('\n')) + (failed.size() > 12 ? QStringLiteral("\nほか %1 件").arg(failed.size() - 12) : QString())));
            return {};
        }
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
