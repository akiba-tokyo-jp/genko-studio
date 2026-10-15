// The distribution (M6, AC-PACKAGE; cmake/GenkoPackage.cmake, package/): this build installed with `cmake --install`
// into a folder of the test's own, then:
// - the folder runs on its own: with a bare environment (no LD_LIBRARY_PATH, no Qt, no Python, a home of the test's
//   own) `genko --version` answers, `genko new` makes a book and the app opens it and shows its first page;
// - (Linux) every library the program and Qt's plugins load is in the folder or the computer's own — none missing,
//   none from the Qt or the build it was made with — and lib/ holds Qt's libraries, its ICU and the six xcb utility
//   libraries (all six), nothing else;
// - the notes and the licences are there, and the SBOM lists every library file in the folder with its SHA-256 and
//   every package with a row of THIRD_PARTY.md that names its licence (the computer's own on Linux are said so);
// - (Linux) install.sh puts it in ~/.local/opt for the person alone (refusing a second time), the menu item and the
//   link run it, and uninstall.sh takes away only those: books, this app's settings and the old app's settings stay
//   byte for byte.

#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <map>
#include <set>

#include "core/json.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace gt = genko::test;

namespace {

#ifdef _WIN32
const QString kProgram = QStringLiteral("bin/genko.exe");
#else
const QString kProgram = QStringLiteral("bin/genko");
#endif

QString sha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return QString::fromLatin1(hash.result().toHex());
}

// A bare environment: the system's programs, a home of the test's own, no display (and the sanitizers' options when
// this build has them).
QProcessEnvironment bare(const QString& home) {
    QProcessEnvironment env;
    const QProcessEnvironment mine = QProcessEnvironment::systemEnvironment();
#ifdef _WIN32
    for (const char* keep : {"SystemRoot", "SystemDrive", "windir", "TEMP", "TMP", "PATHEXT", "COMSPEC"}) {
        if (mine.contains(QString::fromLatin1(keep))) env.insert(QString::fromLatin1(keep), mine.value(QString::fromLatin1(keep)));
    }
    env.insert(QStringLiteral("PATH"), mine.value(QStringLiteral("SystemRoot")) + QStringLiteral("\\System32"));
    env.insert(QStringLiteral("USERPROFILE"), home);
    env.insert(QStringLiteral("APPDATA"), home + QStringLiteral("/AppData/Roaming"));
    env.insert(QStringLiteral("LOCALAPPDATA"), home + QStringLiteral("/AppData/Local"));
#else
    env.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
#endif
    env.insert(QStringLiteral("HOME"), home);
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    for (const char* keep : {"ASAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS", "LSAN_OPTIONS"}) {
        if (mine.contains(QString::fromLatin1(keep))) env.insert(QString::fromLatin1(keep), mine.value(QString::fromLatin1(keep)));
    }
    return env;
}

// Every file under a folder (paths inside it, "/" between folders), links included, folders not.
QStringList files_in(const QString& root) {
    QStringList out;
    QDirIterator walk(root, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (walk.hasNext()) out << QDir(root).relativeFilePath(walk.next());
    out.sort();
    return out;
}

bool is_library(const QString& name) {
    static const QRegularExpression library(QStringLiteral("\\.(so(\\.[0-9]+)*|dll)$"));
    return library.match(name).hasMatch();
}

// The row of THIRD_PARTY.md's tables that begins with `start`.
QString row(const QString& table, const QString& start) {
    for (const QString& line : table.split(QLatin1Char('\n'))) {
        if (line.startsWith(start)) return line;
    }
    return {};
}

}  // namespace

class TestPackage : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString prefix_;
    QString home_;

private slots:
    void initTestCase() {
        QVERIFY(scratch_.isValid());
        // (named as the .tar.gz names its folder: install.sh takes the version from it)
        prefix_ = scratch_.filePath(QStringLiteral("genko-studio-%1-linux-x86_64").arg(QStringLiteral(GENKO_VERSION)));
        home_ = scratch_.filePath(QStringLiteral("home"));
        QVERIFY(QDir().mkpath(home_));
        const gt::Run installed = gt::run(QStringLiteral(GENKO_CMAKE_COMMAND),
                                          {QStringLiteral("--install"), QStringLiteral(GENKO_BUILD_DIR), QStringLiteral("--prefix"), prefix_},
                                          QProcessEnvironment::systemEnvironment(), 900000);
        QVERIFY2(installed.finished && installed.exit_code == 0, qPrintable(installed.described()));
    }

    void theFolderRunsOnItsOwn() {
        const QProcessEnvironment env = bare(home_);
        const gt::Run version = gt::run(prefix_ + QLatin1Char('/') + kProgram, {QStringLiteral("--version")}, env, 120000);
        QVERIFY2(version.finished && version.exit_code == 0, qPrintable(version.described()));
        const Json said = gt::one_line(version.out);
        QCOMPARE(QString::fromStdString(said["build"]["genko"].get<std::string>()), QStringLiteral(GENKO_VERSION));
        QVERIFY(QString::fromStdString(said["build"]["qt"].get<std::string>()).startsWith(QStringLiteral("6.11")));
        // a book, and the app on it: its first page shown
        const QString book = home_ + QStringLiteral("/books/first.genko");
        const gt::Run made = gt::run(prefix_ + QLatin1Char('/') + kProgram, {QStringLiteral("new"), book, QStringLiteral("--pages"), QStringLiteral("2")}, env, 120000);
        QVERIFY2(made.finished && made.exit_code == 0, qPrintable(made.described()));
        QProcessEnvironment app_env = env;
        const QString log = scratch_.filePath(QStringLiteral("perf.jsonl"));
        app_env.insert(QStringLiteral("GENKO_PERF_LOG"), log);
        QProcess app;
        app.setProcessEnvironment(app_env);
        app.setProcessChannelMode(QProcess::MergedChannels);
        app.start(prefix_ + QLatin1Char('/') + kProgram, {QStringLiteral("app"), book});
        QVERIFY(app.waitForStarted(60000));
        bool shown = false;
        for (int waited = 0; waited < 180000 && !shown && app.state() == QProcess::Running; waited += 250) {
            app.waitForFinished(250);
            shown = QString::fromUtf8(gt::read_bytes(log).c_str()).contains(QStringLiteral("\"page_first_shown\""));
        }
        const QByteArray said_by_app = app.readAll();
        app.kill();
        app.waitForFinished(30000);
        QVERIFY2(shown, said_by_app.constData());
        // (the first start found no old app's settings: nothing asked, this app's own folder under the home)
#ifndef _WIN32
        QVERIFY(QFileInfo(home_ + QStringLiteral("/.config/genko-studio")).isDir());
        QVERIFY(!QFileInfo::exists(home_ + QStringLiteral("/.config/genko")));
#endif
    }

#ifndef _WIN32
    void everyLibraryIsInTheFolderOrTheComputers() {
        const QString qt_prefix = QDir::cleanPath(QLibraryInfo::path(QLibraryInfo::PrefixPath));
        const QString build = QDir::cleanPath(QStringLiteral(GENKO_BUILD_DIR));
        QStringList checked{kProgram};
        for (const QString& name : files_in(prefix_)) {
            if ((name.startsWith(QStringLiteral("lib/")) || name.startsWith(QStringLiteral("plugins/"))) && is_library(name) &&
                !QFileInfo(prefix_ + QLatin1Char('/') + name).isSymLink()) {
                checked << name;
            }
        }
        QVERIFY(checked.contains(QStringLiteral("plugins/platforms/libqxcb.so")));
        QVERIFY(checked.contains(QStringLiteral("plugins/platforms/libqoffscreen.so")));
        QVERIFY(QFileInfo::exists(prefix_ + QStringLiteral("/bin/qt.conf")));
        const QStringList system{QStringLiteral("/lib/"), QStringLiteral("/lib64/"), QStringLiteral("/usr/lib/"), QStringLiteral("/usr/lib64/")};
        static const QRegularExpression resolved(QStringLiteral("^\\s*(\\S+)\\s+=>\\s+(.*?)\\s+\\(0x[0-9a-f]+\\)\\s*$"));
        int links = 0;
        for (const QString& name : checked) {
            const gt::Run ldd = gt::run(QStringLiteral("/usr/bin/ldd"), {prefix_ + QLatin1Char('/') + name}, bare(home_), 60000);
            QVERIFY2(ldd.finished && ldd.exit_code == 0, qPrintable(name + QLatin1Char(' ') + ldd.described()));
            for (const QString& line : QString::fromUtf8(ldd.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                QVERIFY2(!line.contains(QStringLiteral("not found")), qPrintable(name + QStringLiteral(": ") + line));
                const QRegularExpressionMatch m = resolved.match(line);
                if (!m.hasMatch()) continue;
                const QString path = QDir::cleanPath(m.captured(2));
                const bool inside = path.startsWith(QDir::cleanPath(prefix_) + QLatin1Char('/'));
                bool computers = false;
                for (const QString& place : system) computers = computers || path.startsWith(place);
                QVERIFY2(inside || computers, qPrintable(name + QStringLiteral(": ") + line));
                QVERIFY2(!path.startsWith(qt_prefix + QLatin1Char('/')) && !path.startsWith(build + QLatin1Char('/')),
                         qPrintable(name + QStringLiteral(" loads the Qt or the build it was made with: ") + line));
                if (m.captured(1).startsWith(QStringLiteral("libQt6"))) QVERIFY2(inside, qPrintable(name + QStringLiteral(": ") + line));
                ++links;
            }
        }
        QVERIFY(links > 0);
        // in lib/: Qt's libraries, the ICU they come with and the six xcb utility libraries — all six there (the computer's
        // own would be found by ldd above even without them), and nothing else: never the computer's image, text or
        // runtime libraries
        static const QRegularExpression allowed(QStringLiteral(
            "^lib/(libQt6[A-Za-z]+\\.so(\\.[0-9]+)*|libicu(data|i18n|uc|io|tu|test)\\.so\\.[0-9.]+|libxcb-(cursor|icccm|image|keysyms|render-util|util)\\.so\\.[0-9.]+)$"));
        static const QRegularExpression util(QStringLiteral("^lib/(libxcb-[a-z-]+)\\.so"));
        std::set<QString> xcb;
        for (const QString& name : files_in(prefix_)) {
            if (!name.startsWith(QStringLiteral("lib/"))) continue;
            QVERIFY2(allowed.match(name).hasMatch(), qPrintable(name + QStringLiteral(" is in lib/")));
            if (const auto m = util.match(name); m.hasMatch()) xcb.insert(m.captured(1));
        }
        QCOMPARE(xcb, (std::set<QString>{QStringLiteral("libxcb-cursor"), QStringLiteral("libxcb-icccm"), QStringLiteral("libxcb-image"),
                                         QStringLiteral("libxcb-keysyms"), QStringLiteral("libxcb-render-util"), QStringLiteral("libxcb-util")}));
    }
#endif

    void theNotesTheLicencesAndTheSbom() {
        const QString doc = prefix_ + QStringLiteral("/share/doc/genko-studio");
        for (const char* there : {"README-ja.md", "QT-LGPL-ja.md", "THIRD_PARTY.md", "sbom.spdx.json", "licenses/qt/LGPL-3.0.txt",
                                  "licenses/qt/GPL-3.0.txt", "licenses/pillow/LICENSE", "licenses/freetype/FTL.TXT", "licenses/freetype/LICENSE.TXT",
                                  "licenses/cpython/LICENSE", "licenses/cpython/LICENSE-SRE", "licenses/cpython/LICENSE-Unicode",
                                  "licenses/optimized-routines/LICENSE-MIT", "licenses/lucide/LICENSE", "licenses/photocraft/LICENSE-MIT"}) {
            QVERIFY2(QFileInfo(doc + QLatin1Char('/') + QString::fromLatin1(there)).size() > 0, there);
        }
        QCOMPARE(QDir(doc + QStringLiteral("/licenses/fonts")).entryList({QStringLiteral("*-OFL.txt")}, QDir::Files).size(), 7);
#ifndef _WIN32
        QVERIFY(QFileInfo(doc + QStringLiteral("/licenses/icu/LICENSE")).size() > 0);
        QCOMPARE(QDir(doc + QStringLiteral("/licenses/xcb-util")).entryList({QStringLiteral("*.copyright")}, QDir::Files).size(), 6);
        // (the menu item's template for install.sh, not itself a menu item of the folder)
        QVERIFY(QFileInfo(prefix_ + QStringLiteral("/share/genko-studio/genko-studio.desktop.in")).isFile());
        QVERIFY(!QFileInfo::exists(prefix_ + QStringLiteral("/share/applications")));
#endif
        QCOMPARE(gt::read_bytes(doc + QStringLiteral("/THIRD_PARTY.md")), gt::read_bytes(gt::repo_root() + QStringLiteral("/docs/cpp-migration/THIRD_PARTY.md")));
        const QString readme = QString::fromUtf8(gt::read_bytes(doc + QStringLiteral("/README-ja.md")).c_str());
        for (const char* said : {"Ubuntu 24.04", "Windows 10 / 11", "電子署名がありません", "SHA-256", "install.sh", "uninstall.sh", "~/.config/genko-studio",
                                 "引き継ぎますか", "旧版に戻す", "知られている制限"}) {
            QVERIFY2(readme.contains(QString::fromUtf8(said)), said);
        }
        // the SBOM: every file it names is there with its SHA-256; every library file of the folder is named
        const Json sbom = gt::read_json(doc + QStringLiteral("/sbom.spdx.json"));
        QCOMPARE(QString::fromStdString(sbom["spdxVersion"].get<std::string>()), QStringLiteral("SPDX-2.3"));
        std::set<QString> named;
        for (const Json& file : sbom["files"]) {
            const QString name = QString::fromStdString(file["fileName"].get<std::string>()).mid(2);  // ("./…")
            QCOMPARE(QString::fromStdString(file["checksums"][0]["checksumValue"].get<std::string>()), sha256(prefix_ + QLatin1Char('/') + name));
            named.insert(name);
        }
        QVERIFY(named.contains(kProgram));
        for (const QString& name : files_in(prefix_)) {
            if (is_library(name) && !QFileInfo(prefix_ + QLatin1Char('/') + name).isSymLink()) QVERIFY2(named.contains(name), qPrintable(name));
        }
        // every package has its row in THIRD_PARTY.md, with its licence (and the computer's own are said so)
        const QString table = QString::fromUtf8(gt::read_bytes(gt::repo_root() + QStringLiteral("/docs/cpp-migration/THIRD_PARTY.md")).c_str());
        const std::map<std::string, QString> rows{
            {"qt", QStringLiteral("| Qt 6")}, {"icu", QStringLiteral("| ICU")}, {"xcb-extras", QStringLiteral("| libxcb の補助ライブラリ")},
            {"nlohmann-json", QStringLiteral("| nlohmann/json")}, {"freetype", QStringLiteral("| FreeType")},
            {"pillow-libimaging", QStringLiteral("| Pillow の `libImaging`")}, {"optimized-routines", QStringLiteral("| Arm optimized-routines")},
            {"photocraft", QStringLiteral("| PhotoCraft（")},
            {"cpython-re", QStringLiteral("| CPython の `re`")}, {"fonts", QStringLiteral("| Dela Gothic One")}, {"lucide", QStringLiteral("| Lucide")},
            {"zlib", QStringLiteral("| zlib")}, {"libpng", QStringLiteral("| libpng")}, {"libjpeg-turbo", QStringLiteral("| libjpeg-turbo")},
            {"libtiff", QStringLiteral("| libtiff")}, {"harfbuzz", QStringLiteral("| HarfBuzz")}, {"libwebp", QStringLiteral("| libwebp")},
            {"lcms2", QStringLiteral("| Little CMS 2")}, {"msvc-runtime", QStringLiteral("| Microsoft Visual C++")}};
        std::set<std::string> listed;
        std::map<std::string, std::string> relation;
        for (const Json& r : sbom["relationships"]) {
            if (r["spdxElementId"] == Json("SPDXRef-genko-studio")) relation[r["relatedSpdxElement"].get<std::string>()] = r["relationshipType"].get<std::string>();
        }
        for (const Json& package : sbom["packages"]) {
            const std::string spdx = package["SPDXID"].get<std::string>();
            if (spdx == "SPDXRef-genko-studio") continue;
            const std::string id = spdx.substr(std::string("SPDXRef-Package-").size());
            QVERIFY2(rows.contains(id), id.c_str());
            listed.insert(id);
            const QString line = row(table, rows.at(id));
            QVERIFY2(!line.isEmpty(), id.c_str());
            const QString licence = QString::fromStdString(package["licenseDeclared"].get<std::string>());
            for (const QString& part : licence.split(QRegularExpression(QStringLiteral(" (AND|OR|WITH) ")))) {
                QVERIFY2(line.contains(part), qPrintable(QString::fromStdString(id) + QStringLiteral(": ") + part));
            }
            const QString version = QString::fromStdString(package["versionInfo"].get<std::string>());
            if (version != QStringLiteral("NOASSERTION")) QVERIFY2(line.contains(version), qPrintable(QString::fromStdString(id) + QStringLiteral(": ") + version));
            const bool computers = QString::fromStdString(package["comment"].get<std::string>()).contains(QStringLiteral("the computer's own"));
            QCOMPARE(relation[spdx], std::string(computers ? "DEPENDS_ON" : "CONTAINS"));
#ifndef _WIN32
            if (computers) QVERIFY2(line.contains(QStringLiteral("Linux: コンピューターのもの")), id.c_str());
#endif
        }
        for (const auto& [id, start] : rows) {
#ifdef _WIN32
            const bool here = id != "icu" && id != "xcb-extras";
#else
            const bool here = id != "msvc-runtime";
#endif
            if (here) QVERIFY2(listed.contains(id), id.c_str());
        }
    }

#ifndef _WIN32
    void installAndUninstallForThePersonAlone() {
        const QString home = scratch_.filePath(QStringLiteral("person"));
        QVERIFY(QDir().mkpath(home));
        const QProcessEnvironment env = bare(home);
        // what must stay: a book, this app's settings, the old app's settings and folder
        QVERIFY(gt::run(prefix_ + QLatin1Char('/') + kProgram, {QStringLiteral("new"), home + QStringLiteral("/books/mine.genko")}, env, 120000).exit_code == 0);
        gt::write_bytes(home + QStringLiteral("/.config/genko-studio/settings.ini"), "[ui]\nfont_pt=13\n");
        gt::write_bytes(home + QStringLiteral("/.config/genko/recent.json"), "[]\n");
        gt::write_bytes(home + QStringLiteral("/.config/Genko/Genko Studio.conf"), "[ui]\ntheme=dark\n");
        const auto kept = gt::tree_hashes(home);
        const QString target = home + QStringLiteral("/.local/opt/genko-studio-") + QStringLiteral(GENKO_VERSION);
        const QString menu = home + QStringLiteral("/.local/share/applications/genko-studio-") + QStringLiteral(GENKO_VERSION) + QStringLiteral(".desktop");
        const QString link = home + QStringLiteral("/.local/bin/genko-studio");
        const gt::Run installed = gt::run(QStringLiteral("/bin/sh"), {prefix_ + QStringLiteral("/install.sh")}, env, 300000);
        QVERIFY2(installed.finished && installed.exit_code == 0, qPrintable(installed.described()));
        QVERIFY(QFileInfo(target + QStringLiteral("/bin/genko")).isExecutable());
        QCOMPARE(QFileInfo(link).symLinkTarget(), target + QStringLiteral("/bin/genko"));
        const QString entry = QString::fromUtf8(gt::read_bytes(menu).c_str());
        QVERIFY2(entry.contains(QStringLiteral("Exec=\"%1/bin/genko\" app %f").arg(target)), qPrintable(entry));
        // (run through the link, and as the menu item says: the libraries found beside the real file)
        const gt::Run version = gt::run(link, {QStringLiteral("--version")}, env, 120000);
        QVERIFY2(version.finished && version.exit_code == 0, qPrintable(version.described()));
        const QRegularExpressionMatch exec = QRegularExpression(QStringLiteral("^Exec=\"([^\"]+)\" app %f$"), QRegularExpression::MultilineOption).match(entry);
        QVERIFY(exec.hasMatch());
        const gt::Run from_menu = gt::run(exec.captured(1), {QStringLiteral("--version")}, env, 120000);
        QVERIFY2(from_menu.finished && from_menu.exit_code == 0, qPrintable(from_menu.described()));
        // a second time: refused, nothing changed
        const auto installed_tree = gt::tree_hashes(target);
        const gt::Run again = gt::run(QStringLiteral("/bin/sh"), {prefix_ + QStringLiteral("/install.sh")}, env, 300000);
        QCOMPARE(again.exit_code, 1);
        QVERIFY(QString::fromUtf8(again.err).contains(QStringLiteral("すでに入っています")));
        QVERIFY(gt::tree_hashes(target) == installed_tree);
        // taken away: only what install.sh made
        const gt::Run removed = gt::run(QStringLiteral("/bin/sh"), {target + QStringLiteral("/uninstall.sh")}, env, 300000);
        QVERIFY2(removed.finished && removed.exit_code == 0, qPrintable(removed.described()));
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(!QFileInfo::exists(menu));
        QVERIFY(!QFileInfo(link).isSymLink() && !QFileInfo::exists(link));
        QVERIFY(gt::tree_hashes(home) == kept);
        QVERIFY(QFileInfo(prefix_ + QLatin1Char('/') + kProgram).isExecutable());  // (the folder it came from stays)
    }
#endif
};

QTEST_GUILESS_MAIN(TestPackage)
#include "test_package.moc"
