#include "app/config.hpp"

#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>

#include <cstdlib>
#include <system_error>

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

fs::path config_dir() {
    if (const QString chosen = env("GENKO_CONFIG_DIR"); !chosen.isEmpty()) return core::path_from_utf8(chosen.toStdString());
#ifdef _WIN32
    if (const QString appdata = env("APPDATA"); !appdata.isEmpty()) return core::path_from_utf8(appdata.toStdString()) / "genko";
#endif
    QString base = env("XDG_CONFIG_HOME");
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.config");
    return core::path_from_utf8(base.toStdString()) / "genko";
}

std::unique_ptr<QSettings> settings() {
    if (const QString chosen = env("GENKO_CONFIG_DIR"); !chosen.isEmpty()) {
        return std::make_unique<QSettings>(chosen + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    }
    return std::make_unique<QSettings>(QStringLiteral("Genko"), QStringLiteral("Genko Studio"));
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
