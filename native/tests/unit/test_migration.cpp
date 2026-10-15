// The app's own settings folder and the Python app's, taken over once (M6, COMP-01a; app/migration, core/config_dir):
// - where things are: $GENKO_CONFIG_DIR, else ~/.config/genko-studio (%APPDATA%\GenkoStudio), the settings in its
//   settings.ini; the Python app's folder ~/.config/genko (%APPDATA%\genko) and settings QSettings("Genko", "Genko
//   Studio") only read;
// - on yes, its keys and files are copied (never the recovery, caches, plugin runs, locks or unfinished files; never
//   over this app's own; the AI keys stay owner-only; a link stays a link) and the Python app's things are left byte
//   for byte as they were; on no, nothing is copied; either way it is asked once;
// - with nothing to take nothing is asked; when something cannot be copied that is said, what was copied stays, and it
//   is asked again on the next start (and the rest copied then).

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include <filesystem>
#include <map>
#include <memory>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/migration.hpp"
#include "core/config_dir.hpp"
#include "core/paths.hpp"

namespace fs = std::filesystem;
namespace migration = genko::app::migration;
namespace ask = genko::app::ask;

namespace {

fs::path path_of(const QString& text) { return genko::core::path_from_utf8(text.toStdString()); }
QString text_of(const fs::path& path) { return QString::fromStdString(genko::core::path_to_utf8(path)); }

void write(const QString& name, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(name).path());
    QFile file(name);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}

QByteArray read(const QString& name) {
    QFile file(name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

// Every file under a folder: (its path inside, its bytes and its time)
std::map<QString, std::pair<QByteArray, QDateTime>> snapshot(const QString& root) {
    std::map<QString, std::pair<QByteArray, QDateTime>> out;
    QDirIterator walk(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (walk.hasNext()) {
        const QString name = walk.next();
        const QFileInfo info(name);
        out[QDir(root).relativeFilePath(name)] = {info.isFile() && !info.isSymLink() ? read(name) : QByteArray(info.isSymLink() ? "<link>" : "<dir>"),
                                                  info.lastModified()};
    }
    return out;
}

struct Answers {
    std::shared_ptr<ask::Responder> responder = std::make_shared<ask::Responder>();
    QStringList asked;
    QStringList warned;
    bool yes = true;
    Answers() {
        responder->question = [this](const QString& title, const QString& text) {
            asked << title + QLatin1Char('\n') + text;
            return yes;
        };
        responder->warning = [this](const QString&, const QString& text) { warned << text; };
        ask::set_responder(responder);
    }
    ~Answers() { ask::set_responder(nullptr); }
};

// The Python app's things: settings (an ini of the test's own) and a folder.
struct Legacy {
    QTemporaryDir root;
    QString dir;
    QString ini;
    std::unique_ptr<QSettings> settings;
    QString own_dir;
    std::unique_ptr<QSettings> own;
    Legacy() {
        dir = root.filePath(QStringLiteral("old/genko"));
        ini = root.filePath(QStringLiteral("old/Genko Studio.conf"));
        own_dir = root.filePath(QStringLiteral("new/genko-studio"));
        {
            QSettings make(ini, QSettings::IniFormat);
            make.setValue(QStringLiteral("ui/font_pt"), 12);
            make.setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
            make.setValue(QStringLiteral("ui/default_paper"), QStringLiteral("B5"));
            make.setValue(QStringLiteral("window/geometry"), QByteArray("\x01\x02\x00\xff", 4));
            make.setValue(QStringLiteral("shortcuts/保存"), QStringLiteral("Ctrl+S"));
        }
        write(dir + QStringLiteral("/recent.json"), "[\"/books/a\"]\n");
        write(dir + QStringLiteral("/brushes.json"), "{\"brushes\": {}}\n");
        write(dir + QStringLiteral("/colorsets.json"), "{}\n");
        write(dir + QStringLiteral("/actions.json"), "{\"old\": 1}\n");
        write(dir + QStringLiteral("/tokens.json"), "{\"secret\": \"k\"}\n");
        QFile::setPermissions(dir + QStringLiteral("/tokens.json"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        write(dir + QStringLiteral("/materials/folder/one.png"), "png");
        write(dir + QStringLiteral("/brush_papers/p.png"), "paper");
        write(dir + QStringLiteral("/plugins/tint/plugin.json"), "{}");
        // kept back
        write(dir + QStringLiteral("/recovery/book/journal"), "j");
        write(dir + QStringLiteral("/cache/material-previews/x.png"), "c");
        write(dir + QStringLiteral("/plugin-runs/r1/out.png"), "r");
        write(dir + QStringLiteral("/brushes.json.lock"), "l");
        write(dir + QStringLiteral("/tokens.json.tmp"), "t");
        write(dir + QStringLiteral("/recent.json.part"), "p");
        settings = std::make_unique<QSettings>(ini, QSettings::IniFormat);
        own = std::make_unique<QSettings>(own_dir + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    }
    migration::Places places() const { return {path_of(dir), settings.get(), path_of(own_dir), own.get()}; }
};

}  // namespace

class TestMigration : public QObject {
    Q_OBJECT

    // (a home of the test's own, set before anything reads a settings file: Qt finds its settings' place once)
    QTemporaryDir home;
    QByteArray chosen_;
    QByteArray xdg_;
    QByteArray appdata_;

private slots:
    void initTestCase() {
        chosen_ = qgetenv("GENKO_CONFIG_DIR");
        xdg_ = qgetenv("XDG_CONFIG_HOME");
        appdata_ = qgetenv("APPDATA");
        qunsetenv("GENKO_CONFIG_DIR");
        qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("xdg")).toUtf8());
        qputenv("APPDATA", home.filePath(QStringLiteral("AppData/Roaming")).toUtf8());
    }

    void cleanupTestCase() {
        chosen_.isNull() ? qunsetenv("GENKO_CONFIG_DIR") : (void)qputenv("GENKO_CONFIG_DIR", chosen_);
        xdg_.isNull() ? qunsetenv("XDG_CONFIG_HOME") : (void)qputenv("XDG_CONFIG_HOME", xdg_);
        appdata_.isNull() ? qunsetenv("APPDATA") : (void)qputenv("APPDATA", appdata_);
    }

    void theFoldersAreApart() {
#ifdef _WIN32
        const QString base = home.filePath(QStringLiteral("AppData/Roaming"));
        QCOMPARE(genko::core::config_dir(), path_of(base) / "GenkoStudio");
        QCOMPARE(genko::core::legacy_config_dir(), path_of(base) / "genko");
        // (the Python app's settings are in the registry: only where it is is looked at)
        QCOMPARE(genko::app::legacy_settings()->fileName(), QStringLiteral("\\HKEY_CURRENT_USER\\Software\\Genko\\Genko Studio"));
#else
        const QString base = home.filePath(QStringLiteral("xdg"));
        QCOMPARE(genko::core::config_dir(), path_of(base) / "genko-studio");
        QCOMPARE(genko::core::legacy_config_dir(), path_of(base) / "genko");
        QCOMPARE(QDir::cleanPath(genko::app::legacy_settings()->fileName()), QDir::cleanPath(base + QStringLiteral("/Genko/Genko Studio.conf")));
#endif
        QCOMPARE(QDir::cleanPath(genko::app::settings()->fileName()), QDir::cleanPath(text_of(genko::core::config_dir() / "settings.ini")));
        QCOMPARE(genko::app::config_dir(), genko::core::config_dir());
        // a folder chosen is the app's own: the settings in it, and no Python app's settings to offer
        qputenv("GENKO_CONFIG_DIR", home.filePath(QStringLiteral("chosen")).toUtf8());
        QCOMPARE(genko::core::config_dir(), path_of(home.filePath(QStringLiteral("chosen"))));
        QCOMPARE(QDir::cleanPath(genko::app::settings()->fileName()), QDir::cleanPath(home.filePath(QStringLiteral("chosen/settings.ini"))));
        QVERIFY(genko::app::legacy_settings() == nullptr);
        {
            Answers answers;
            QCOMPARE(migration::offer_on_first_start(), QString());
            QVERIFY(answers.asked.isEmpty());
        }
        QVERIFY(!QFile::exists(home.filePath(QStringLiteral("chosen/settings.ini"))));
        qunsetenv("GENKO_CONFIG_DIR");
        // (without $XDG_CONFIG_HOME: ~/.config)
#ifndef _WIN32
        qunsetenv("XDG_CONFIG_HOME");
        QCOMPARE(genko::core::config_dir(), path_of(QDir::homePath()) / ".config" / "genko-studio");
        QCOMPARE(genko::core::legacy_config_dir(), path_of(QDir::homePath()) / ".config" / "genko");
        qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("xdg")).toUtf8());
#endif
    }

    void whatIsKeptBack() {
        for (const char* back : {"recovery", "recovery/a/journal", "cache/x", "plugin-runs/r/out.png", "settings.ini", "brushes.json.lock",
                                 "tokens.json.tmp", "a/b.part", "notes~", ".#recent.json"}) {
            QVERIFY2(migration::kept_back(path_of(QString::fromUtf8(back))), back);
        }
        for (const char* taken : {"recent.json", "tokens.json", "materials/cache/x.png", "plugins/tint/plugin.json", "brush_papers/p.png"}) {
            QVERIFY2(!migration::kept_back(path_of(QString::fromUtf8(taken))), taken);
        }
    }

    void yesCopiesAndLeavesTheOldAsItWas() {
        Legacy legacy;
        // this app's own already: never written over
        write(legacy.own_dir + QStringLiteral("/actions.json"), "{\"mine\": 2}\n");
        legacy.own->setValue(QStringLiteral("ui/default_paper"), QStringLiteral("A4"));
#ifndef _WIN32
        QVERIFY(QFile::link(QStringLiteral("../../elsewhere/dev-plugin"), legacy.dir + QStringLiteral("/plugins/dev")));
#endif
        const auto before = snapshot(legacy.root.filePath(QStringLiteral("old")));
        Answers answers;
        QCOMPARE(migration::offer(nullptr, legacy.places()), QStringLiteral("copied"));
        QCOMPARE(answers.asked.size(), 1);
        QVERIFY(answers.asked.front().contains(QStringLiteral("引き継ぎますか")));
        QVERIFY(answers.asked.front().contains(QStringLiteral("以前の版の設定はそのまま残り、変更しません")));
        QVERIFY(answers.warned.isEmpty());
        // the files
        for (const char* taken : {"recent.json", "brushes.json", "colorsets.json", "tokens.json", "materials/folder/one.png", "brush_papers/p.png",
                                  "plugins/tint/plugin.json"}) {
            const QString name = QString::fromUtf8(taken);
            QCOMPARE(read(legacy.own_dir + QLatin1Char('/') + name), read(legacy.dir + QLatin1Char('/') + name));
        }
        QCOMPARE(read(legacy.own_dir + QStringLiteral("/actions.json")), QByteArray("{\"mine\": 2}\n"));
        for (const char* back : {"recovery", "cache", "plugin-runs", "brushes.json.lock", "tokens.json.tmp", "recent.json.part"}) {
            QVERIFY2(!QFileInfo::exists(legacy.own_dir + QLatin1Char('/') + QString::fromUtf8(back)), back);
        }
        QVERIFY(QDir(legacy.own_dir).entryList(QStringList{QStringLiteral("*.part")}, QDir::Files).isEmpty());
#ifndef _WIN32
        QCOMPARE(fs::status(path_of(legacy.own_dir + QStringLiteral("/tokens.json"))).permissions() & fs::perms::all,
                 fs::perms::owner_read | fs::perms::owner_write);  // (the AI keys stay the person's own)
        QCOMPARE(QFileInfo(legacy.own_dir + QStringLiteral("/plugins/dev")).symLinkTarget(), QFileInfo(legacy.dir + QStringLiteral("/plugins/dev")).symLinkTarget());
        QVERIFY(QFileInfo(legacy.own_dir + QStringLiteral("/plugins/dev")).isSymLink());
#endif
        // the settings: the Python app's keys, but not over this app's own
        QSettings own(legacy.own_dir + QStringLiteral("/settings.ini"), QSettings::IniFormat);
        QCOMPARE(own.value(QStringLiteral("ui/font_pt")).toInt(), 12);
        QCOMPARE(own.value(QStringLiteral("ui/theme")).toString(), QStringLiteral("dark"));
        QCOMPARE(own.value(QStringLiteral("window/geometry")).toByteArray(), QByteArray("\x01\x02\x00\xff", 4));
        QCOMPARE(own.value(QStringLiteral("shortcuts/保存")).toString(), QStringLiteral("Ctrl+S"));
        QCOMPARE(own.value(QStringLiteral("ui/default_paper")).toString(), QStringLiteral("A4"));
        QCOMPARE(own.value(QLatin1String(migration::kDoneKey)).toString(), QStringLiteral("copied"));
        // the Python app's things: as they were, byte for byte (and not one key more)
        legacy.settings.reset();
        QCOMPARE(snapshot(legacy.root.filePath(QStringLiteral("old"))), before);
        QSettings old(legacy.ini, QSettings::IniFormat);
        QCOMPARE(old.allKeys().size(), 5);
        // asked once
        legacy.settings = std::make_unique<QSettings>(legacy.ini, QSettings::IniFormat);
        write(legacy.dir + QStringLiteral("/poses.json"), "[]");  // (made later by the Python app: not offered again)
        QCOMPARE(migration::offer(nullptr, legacy.places()), QStringLiteral("copied"));
        QCOMPARE(answers.asked.size(), 1);
        QVERIFY(!QFile::exists(legacy.own_dir + QStringLiteral("/poses.json")));
    }

    void noCopiesNothing() {
        Legacy legacy;
        const auto before = snapshot(legacy.root.filePath(QStringLiteral("old")));
        Answers answers;
        answers.yes = false;
        QCOMPARE(migration::offer(nullptr, legacy.places()), QStringLiteral("declined"));
        QCOMPARE(answers.asked.size(), 1);
        QCOMPARE(QDir(legacy.own_dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList{QStringLiteral("settings.ini")});
        QSettings own(legacy.own_dir + QStringLiteral("/settings.ini"), QSettings::IniFormat);
        QCOMPARE(own.allKeys(), QStringList{QString::fromLatin1(migration::kDoneKey)});
        QCOMPARE(own.value(QLatin1String(migration::kDoneKey)).toString(), QStringLiteral("declined"));
        QCOMPARE(migration::offer(nullptr, legacy.places()), QStringLiteral("declined"));
        QCOMPARE(answers.asked.size(), 1);
        legacy.settings.reset();
        QCOMPARE(snapshot(legacy.root.filePath(QStringLiteral("old"))), before);
    }

    void nothingToTakeAsksNothing() {
        QTemporaryDir root;
        QSettings own(root.filePath(QStringLiteral("new/settings.ini")), QSettings::IniFormat);
        QSettings none(root.filePath(QStringLiteral("old/missing.conf")), QSettings::IniFormat);
        Answers answers;
        // no folder and no keys; then a folder with only what is kept back
        QCOMPARE(migration::offer(nullptr, {path_of(root.filePath(QStringLiteral("old/genko"))), &none, path_of(root.filePath(QStringLiteral("new"))), &own}),
                 QStringLiteral("none"));
        QVERIFY(answers.asked.isEmpty());
        QCOMPARE(own.value(QLatin1String(migration::kDoneKey)).toString(), QStringLiteral("none"));
        QSettings own2(root.filePath(QStringLiteral("new2/settings.ini")), QSettings::IniFormat);
        write(root.filePath(QStringLiteral("old/genko/recovery/a/journal")), "j");
        write(root.filePath(QStringLiteral("old/genko/cache/x")), "c");
        QCOMPARE(migration::offer(nullptr, {path_of(root.filePath(QStringLiteral("old/genko"))), &none, path_of(root.filePath(QStringLiteral("new2"))), &own2}),
                 QStringLiteral("none"));
        QVERIFY(answers.asked.isEmpty());
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("old/missing.conf"))));
    }

    void whatCannotBeCopiedIsSaidAndAskedAgain() {
        Legacy legacy;
        write(legacy.own_dir + QStringLiteral("/materials"), "a file where the folder would go");
        Answers answers;
        QCOMPARE(migration::offer(nullptr, legacy.places()), QString());
        QCOMPARE(answers.warned.size(), 1);
        QVERIFY(answers.warned.front().contains(QStringLiteral("one.png")));
        QVERIFY(answers.warned.front().contains(QStringLiteral("次に起動したときにもう一度聞きます")));
        QCOMPARE(read(legacy.own_dir + QStringLiteral("/recent.json")), read(legacy.dir + QStringLiteral("/recent.json")));  // (the rest copied)
        QVERIFY(!legacy.own->contains(QLatin1String(migration::kDoneKey)));
        QVERIFY(QDir(legacy.own_dir).entryList(QStringList{QStringLiteral("*.part")}, QDir::Files).isEmpty());
        // the next start: asked again, the rest copied, nothing twice
        QVERIFY(QFile::remove(legacy.own_dir + QStringLiteral("/materials")));
        write(legacy.own_dir + QStringLiteral("/recent.json"), "[\"/books/new\"]\n");  // (used since)
        QCOMPARE(migration::offer(nullptr, legacy.places()), QStringLiteral("copied"));
        QCOMPARE(answers.asked.size(), 2);
        QCOMPARE(read(legacy.own_dir + QStringLiteral("/materials/folder/one.png")), QByteArray("png"));
        QCOMPARE(read(legacy.own_dir + QStringLiteral("/recent.json")), QByteArray("[\"/books/new\"]\n"));
    }

#ifndef _WIN32
    void theFirstStartFindsThePythonAppsOwn() {
        // (the app's own places, under the test's own home: the Python app's settings file and folder as it leaves
        // them on Linux; on Windows its settings are the person's registry, which a test never reads)
        const QString conf = genko::app::legacy_settings()->fileName();
        QVERIFY(conf.startsWith(home.path()));
        write(conf, "[ui]\nfont_pt=14\n");
        write(home.filePath(QStringLiteral("xdg/genko/recent.json")), "[]\n");
        {
            Answers answers;
            QCOMPARE(migration::offer_on_first_start(), QStringLiteral("copied"));
            QCOMPARE(answers.asked.size(), 1);
            QCOMPARE(genko::app::settings()->value(QStringLiteral("ui/font_pt")).toInt(), 14);
            QCOMPARE(read(home.filePath(QStringLiteral("xdg/genko-studio/recent.json"))), QByteArray("[]\n"));
            QCOMPARE(migration::offer_on_first_start(), QStringLiteral("copied"));
            QCOMPARE(answers.asked.size(), 1);
        }
        QCOMPARE(read(conf), QByteArray("[ui]\nfont_pt=14\n"));
    }
#endif
};

QTEST_GUILESS_MAIN(TestMigration)
#include "test_migration.moc"
