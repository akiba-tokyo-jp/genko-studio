#include "app/settings_watch.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>

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

class Watch : public QObject {
public:
    explicit Watch(QObject* parent) : QObject(parent) {
        file_ = QString::fromStdString(core::path_to_utf8(config_dir() / "settings.ini"));
        last_ = live_values();
        quiet_.setSingleShot(true);
        quiet_.setInterval(300);  // (the file written in several steps: looked at once it stops changing)
        connect(&quiet_, &QTimer::timeout, this, [this] { (void)look(); });
        // (QSettings writes the file anew and renames it into place: the folder is watched, and the file again each time)
        watcher_.addPath(QString::fromStdString(core::path_to_utf8(config_dir())));
        if (QFileInfo::exists(file_)) watcher_.addPath(file_);
        connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { touched(); });
        connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { touched(); });
    }

    std::function<void()> changed;

    bool look() {
        if (QFileInfo::exists(file_) && !watcher_.files().contains(file_)) watcher_.addPath(file_);
        core::Json now = live_values();
        if (now == last_) return false;
        last_ = std::move(now);
        if (changed) changed();
        return true;
    }

private:
    void touched() { quiet_.start(); }

    QString file_;
    QFileSystemWatcher watcher_;
    QTimer quiet_;
    core::Json last_;
};

Watch* the_watch = nullptr;

}  // namespace

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
