#include "core/config_dir.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QString>

#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/paths.hpp"

namespace genko::core {

namespace {

std::filesystem::path path_of(const QString& text) { return path_from_utf8(text.toStdString()); }

std::filesystem::path folder(const char* windows, const char* elsewhere) {
#ifdef _WIN32
    if (const QString appdata = qEnvironmentVariable("APPDATA"); !appdata.isEmpty()) return path_of(appdata) / windows;
#else
    (void)windows;
#endif
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.config");
    return path_of(base) / elsewhere;
}

}  // namespace

std::filesystem::path config_dir() {
    if (const QString own = qEnvironmentVariable("GENKO_CONFIG_DIR"); !own.isEmpty()) return path_of(own);
    return folder("GenkoStudio", "genko-studio");
}

std::filesystem::path legacy_config_dir() { return folder("genko", "genko"); }

bool legacy_settings_exist() {
#ifdef _WIN32
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Genko\\Genko Studio", 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
#else
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.config");
    return QFileInfo(base + QStringLiteral("/Genko/Genko Studio.conf")).isFile();
#endif
}

bool import_pending() {
    if (!qEnvironmentVariableIsEmpty("GENKO_CONFIG_DIR")) return false;
    const std::filesystem::path dir = config_dir();
    const std::filesystem::path legacy = legacy_config_dir();
    std::error_code ec;
    if (std::filesystem::equivalent(dir, legacy, ec) || dir == legacy) return false;
    const QSettings own(QString::fromStdString(path_to_utf8(dir / "settings.ini")), QSettings::IniFormat);
    if (!own.value(QStringLiteral("migration/done")).toString().isEmpty()) return false;
    if (legacy_settings_exist()) return true;
    return std::filesystem::is_directory(legacy, ec) && !std::filesystem::is_empty(legacy, ec) && !ec;
}

}  // namespace genko::core
