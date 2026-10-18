#pragma once

// A locale whose decimal point is a comma (German, French …) for C's numbers (LC_NUMERIC) while it lives — as the app
// and the command line run in one: Qt sets the environment's locale on Unix (setlocale(LC_ALL, "")). One installed is
// taken; on Linux without one, one is made with localedef into a folder of its own (LOCPATH). ok(): it is set.

#include <QByteArray>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>

#include <clocale>
#include <string>

namespace genko::test {

class CommaLocale {
public:
    CommaLocale() {
        const char* now = std::setlocale(LC_NUMERIC, nullptr);
        was_ = now != nullptr ? std::string(now) : std::string("C");
        for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8", "de_DE", "German_Germany.1252"}) {
            if (take(name)) return;
        }
#ifdef __linux__
        if (dir_.isValid()) {
            QProcess made;
            made.start(QStringLiteral("localedef"), {QStringLiteral("-c"), QStringLiteral("-i"), QStringLiteral("de_DE"), QStringLiteral("-f"),
                                                    QStringLiteral("UTF-8"), dir_.path() + QStringLiteral("/de_DE.UTF-8")});
            made.waitForFinished(120000);
            was_path_ = qgetenv("LOCPATH");
            had_path_ = qEnvironmentVariableIsSet("LOCPATH");
            qputenv("LOCPATH", dir_.path().toLocal8Bit());
            set_path_ = true;
            if (take("de_DE.UTF-8")) return;
        }
#endif
    }
    ~CommaLocale() {
        std::setlocale(LC_NUMERIC, was_.c_str());
        if (set_path_) {
            if (had_path_) {
                qputenv("LOCPATH", was_path_);
            } else {
                qunsetenv("LOCPATH");
            }
        }
    }
    CommaLocale(const CommaLocale&) = delete;
    CommaLocale& operator=(const CommaLocale&) = delete;
    bool ok() const { return ok_; }

private:
    bool take(const char* name) {
        if (std::setlocale(LC_NUMERIC, name) == nullptr) return false;
        ok_ = std::localeconv()->decimal_point[0] == ',';
        if (!ok_) std::setlocale(LC_NUMERIC, was_.c_str());
        return ok_;
    }
    std::string was_;
    QTemporaryDir dir_;
    QByteArray was_path_;
    bool had_path_ = false, set_path_ = false, ok_ = false;
};

}  // namespace genko::test
