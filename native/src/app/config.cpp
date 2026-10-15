#include "app/config.hpp"

#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>

#include <cstdlib>
#include <system_error>

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

std::unique_ptr<QSettings> settings() {
    return std::make_unique<QSettings>(QString::fromStdString(core::path_to_utf8(config_dir() / "settings.ini")), QSettings::IniFormat);
}

std::unique_ptr<QSettings> legacy_settings() {
    if (!qEnvironmentVariableIsEmpty("GENKO_CONFIG_DIR")) return nullptr;
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
