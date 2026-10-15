#include "core/config_dir.hpp"

#include <QDir>
#include <QString>

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

}  // namespace genko::core
