#include "app/config.hpp"

#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>

#include <cstdlib>
#include <system_error>

#include "app/settings_watch.hpp"
#include "core/config_dir.hpp"
#include "core/paths.hpp"

namespace genko::app {

namespace fs = std::filesystem;

namespace {

QString env(const char* name) { return qEnvironmentVariable(name); }

fs::path ensure(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

}  // namespace

fs::path config_dir() { return core::config_dir(); }

namespace {

// The app's own store: when it goes, having changed the file, the watch of settings.ini knows the app wrote it (PC5 H-0:
// app/settings_watch — what the app writes it has applied itself).
class OwnSettings : public QSettings {
public:
    explicit OwnSettings(const QString& file) : QSettings(file, QSettings::IniFormat), file_(file), before_(settings_stamp(file)) {}
    ~OwnSettings() override {
        sync();
        if (const SettingsStamp after = settings_stamp(file_); after != before_) settings_written(after);
    }

private:
    QString file_;
    SettingsStamp before_;
};

}  // namespace

std::unique_ptr<QSettings> settings() {
    return std::make_unique<OwnSettings>(QString::fromStdString(core::path_to_utf8(config_dir() / "settings.ini")));
}

std::unique_ptr<QSettings> legacy_settings() {
    if (!qEnvironmentVariableIsEmpty("GENKO_CONFIG_DIR")) return nullptr;
    // (looked at first: Qt opens the first registry key for writing and makes it when it is missing — the Python app's
    // settings are never made or changed)
    if (!core::legacy_settings_exist()) return nullptr;
    auto legacy = std::make_unique<QSettings>(QStringLiteral("Genko"), QStringLiteral("Genko Studio"));
    legacy->setFallbacksEnabled(false);  // (its own keys, not the organisation's or the computer's)
    return legacy;
}

std::string default_actor() {
    QString name = env("GENKO_USER");
    if (name.isEmpty()) {
        for (const char* key : {"LOGNAME", "USER", "LNAME", "USERNAME"}) {  // (getpass.getuser's order)
            name = env(key);
            if (!name.isEmpty()) break;
        }
    }
    if (name.isEmpty()) name = QStringLiteral("user");
    static const QRegularExpression other(QStringLiteral("[^\\w.-]"), QRegularExpression::UseUnicodePropertiesOption);
    name.replace(other, QStringLiteral("_"));
    if (name.isEmpty()) name = QStringLiteral("user");
    const std::string out = name.toStdString();
    return out.rfind("human:", 0) == 0 ? out : "human:" + out;
}

fs::path recovery_root() { return ensure(config_dir() / "recovery"); }

fs::path cache_root() { return ensure(config_dir() / "cache"); }

}  // namespace genko::app
