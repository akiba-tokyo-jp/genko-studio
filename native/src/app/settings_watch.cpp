#include "app/settings_watch.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>

#include <optional>

#include "app/config.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/prefs_table.hpp"

namespace genko::app {

namespace {

// The preferences that show at once, as the file holds them now.
core::Json live_values() {
    const auto store = settings();
    const core::prefs::Outcome all = core::prefs::get(*store);
    core::Json out = core::Json::object();
    for (const auto& [path, value] : all.values.items()) {
        const core::prefs::Pref* pref = core::prefs::by_path(path);
        if (pref != nullptr && pref->apply == core::prefs::Apply::Live) out[path] = value;
    }
    return out;
}

QString settings_file() { return QString::fromStdString(core::path_to_utf8(config_dir() / "settings.ini")); }

// The file as the app itself last left it (its own writes: settings() — app/config).
std::optional<SettingsStamp> own;

class Watch : public QObject {
public:
    explicit Watch(QObject* parent) : QObject(parent) {
        quiet_.setSingleShot(true);
        quiet_.setInterval(300);  // (the file written in several steps: looked at once it stops changing)
        connect(&quiet_, &QTimer::timeout, this, [this] { (void)look(); });
        connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { touched(); });
        connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { touched(); });
        follow();
        last_ = live_values();
        seen_ = settings_stamp(file_);
    }

    std::function<void()> changed;

    bool look() {
        follow();
        seen_ = settings_stamp(file_);
        core::Json now = live_values();
        // (the app's own write: what it has applied itself — the base taken again, nothing applied twice)
        if (own && *own == seen_) {
            last_ = std::move(now);
            return false;
        }
        if (now == last_) return false;
        last_ = std::move(now);
        if (changed) changed();
        return true;
    }

private:
    // (the folder of this computer's settings, followed if it changes — a test's own; QSettings writes the file anew and
    // renames it into place: the folder is watched, and the file again each time)
    void follow() {
        const QString file = settings_file();
        if (file != file_) {
            if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
            if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
            file_ = file;
            watcher_.addPath(QFileInfo(file_).absolutePath());
        }
        if (QFileInfo::exists(file_) && !watcher_.files().contains(file_)) watcher_.addPath(file_);
    }

    // (only settings.ini itself: the folder's other files — recent.json, the lock file — leave it as it was)
    void touched() {
        if (settings_stamp(file_) == seen_) return;
        quiet_.start();
    }

    QString file_;
    QFileSystemWatcher watcher_;
    QTimer quiet_;
    core::Json last_;
    SettingsStamp seen_;
};

Watch* the_watch = nullptr;

}  // namespace

SettingsStamp settings_stamp(const QString& file) {
    const QFileInfo info(file);  // (a fresh look: QFileInfo does not keep an old one)
    if (!info.exists()) return {};
    return {info.lastModified().toMSecsSinceEpoch(), info.size()};
}

void settings_written(const SettingsStamp& stamp) { own = stamp; }

void watch_settings(std::function<void()> changed) {
    if (the_watch == nullptr) {
        auto* app = QCoreApplication::instance();
        if (app == nullptr) return;
        the_watch = new Watch(app);
        QObject::connect(app, &QObject::destroyed, [] { the_watch = nullptr; });
    }
    the_watch->changed = std::move(changed);
}

bool settings_changed_now() { return the_watch != nullptr && the_watch->look(); }

}  // namespace genko::app
