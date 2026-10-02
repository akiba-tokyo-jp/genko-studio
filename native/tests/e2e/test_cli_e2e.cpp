// The genko command line end to end: what `genko inspect` prints and returns for books it cannot read, as Python's
// genko/__main__.py does ({"ok": false, "error": …} on stdout; exit 2 for a book from a newer Genko, 1 for the rest),
// usage errors, --ascii, --stroke and `genko schema`.

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <string>

#include "testsupport.hpp"

using genko::core::Json;

namespace {

genko::test::Run genko_run(const QStringList& args) {
    return genko::test::run(genko::test::genko_cli(), args, QProcessEnvironment::systemEnvironment(), 120000);
}

void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

Json one_line(const QByteArray& out) {
    const std::string text = out.toStdString();
    if (text.empty() || text.back() != '\n' || text.find('\n') != text.size() - 1) return Json("not one line: " + text);
    return genko::core::parse_python_json(text);
}

QByteArray book_json(int version) {
    return QByteArray(R"({"version": )") + QByteArray::number(version) +
           R"(, "title": "t", "episode": 1, "spec": {"width_mm": 210, "height_mm": 297, "dpi": 600, "bleed_mm": 3,)"
           R"( "inner_margin_mm": 10}, "pages": []})";
}

}  // namespace

class TestCliE2e : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

private slots:
    void missingFolder() {
        const QString dir = tmp_.path() + "/nowhere.genko";
        const auto r = genko_run({"inspect", dir});
        QCOMPARE(r.exit_code, 1);
        const Json out = one_line(r.out);
        QCOMPARE(out["ok"], Json(false));
        QCOMPARE(out["error"].get<std::string>(),
                 "not found: [Errno 2] No such file or directory: '" + dir.toStdString() + "/project.json'");
        QCOMPARE(out["code"].get<std::string>(), std::string("not_found"));
        // the path as Python's pathlib writes it
        const auto relative = genko_run({"inspect", "./nowhere//x/"});
        QCOMPARE(one_line(relative.out)["error"].get<std::string>(),
                 std::string("not found: [Errno 2] No such file or directory: 'nowhere/x/project.json'"));
    }

    void brokenJson() {
        const QString dir = tmp_.path() + "/broken.genko";
        QVERIFY(QDir().mkpath(dir));
        write(dir + "/project.json", "{\"title\": \"t\",\n  oops}");
        const auto r = genko_run({"inspect", dir});
        QCOMPARE(r.exit_code, 1);
        const Json out = one_line(r.out);
        QCOMPARE(out["ok"], Json(false));
        QCOMPARE(out["error"].get<std::string>(),
                 std::string("Expecting property name enclosed in double quotes: line 2 column 3 (char 17)"));
        QCOMPARE(out["code"].get<std::string>(), std::string("json"));
        write(dir + "/project.json", "\xff\xfe{}");
        QCOMPARE(one_line(genko_run({"inspect", dir}).out)["error"].get<std::string>(),
                 std::string("'utf-8' codec can't decode byte 0xff in position 0: invalid start byte"));
    }

    void newerVersions() {
        const QString dir = tmp_.path() + "/v99.genko";
        QVERIFY(QDir().mkpath(dir));
        write(dir + "/project.json", book_json(99));
        const auto r = genko_run({"inspect", dir});
        QCOMPARE(r.exit_code, 2);
        const Json out = one_line(r.out);
        QCOMPARE(out["ok"], Json(false));
        QCOMPARE(out["error"].get<std::string>(),
                 std::string("project.json version 99 is newer than this build supports (4); update Genko"));
        QCOMPARE(out["code"].get<std::string>(), std::string("unsupported_version"));
        QByteArray needs = book_json(4);
        needs.replace("\"version\": 4", "\"version\": 4, \"min_reader\": 7");
        write(dir + "/project.json", needs);
        QCOMPARE(genko_run({"inspect", dir}).exit_code, 2);
        write(dir + "/project.json", book_json(4));
        QCOMPARE(genko_run({"inspect", dir}).exit_code, 0);  // (read-only for want of a book_id, but readable)
    }

    void notABook() {
        const QString dir = tmp_.path() + "/list.genko";
        QVERIFY(QDir().mkpath(dir));
        write(dir + "/project.json", "[]");
        auto r = genko_run({"inspect", dir});
        QCOMPARE(r.exit_code, 1);
        QCOMPARE(one_line(r.out)["code"].get<std::string>(), std::string("format"));
        write(dir + "/project.json", R"({"title": "t", "episode": 1, "pages": []})");
        r = genko_run({"inspect", dir});
        QCOMPARE(r.exit_code, 1);
        QCOMPARE(one_line(r.out)["error"].get<std::string>(), std::string("missing key 'spec'"));
        const auto file = genko_run({"inspect", dir + "/project.json"});  // a file, not a folder
        QCOMPARE(file.exit_code, 1);
#ifdef Q_OS_WIN
        // (Windows reports a file in the middle of a path as a missing path, as Python's open() does there)
        QVERIFY(one_line(file.out)["error"].get<std::string>().find("[Errno 2] No such file or directory") !=
                std::string::npos);
#else
        QVERIFY(one_line(file.out)["error"].get<std::string>().starts_with("[Errno 20] Not a directory"));
#endif
    }

    void usageErrors() {
        QCOMPARE(genko_run({}).exit_code, 2);
        QCOMPARE(genko_run({"frobnicate"}).exit_code, 2);
        QCOMPARE(genko_run({"inspect"}).exit_code, 2);
        QCOMPARE(genko_run({"inspect", genko::test::test_data("legacy/book-v3.genko"), "--fast"}).exit_code, 2);
        QCOMPARE(genko_run({"inspect", "a", "b"}).exit_code, 2);
        QCOMPARE(genko_run({"schema", "extra"}).exit_code, 2);
        const auto r = genko_run({"inspect"});
        QVERIFY(r.out.isEmpty());  // (usage goes to stderr)
        QVERIFY(!r.err.isEmpty());
    }

    void inspectOutput() {
        const QString book = genko::test::test_data("legacy/book-v3.genko");
        const auto plain = genko_run({"inspect", book});
        QCOMPARE(plain.exit_code, 0);
        const Json snapshot = one_line(plain.out);
        QCOMPARE(snapshot["title"].get<std::string>(), std::string("旧原稿 v3"));
        QVERIFY(plain.out.contains("旧原稿"));  // UTF-8, not escaped
        QVERIFY(!snapshot["pages"][0].contains("ink_strokes"));
        const auto ascii = genko_run({"--ascii", "inspect", book});
        QCOMPARE(ascii.exit_code, 0);
        for (const char c : ascii.out) QVERIFY(static_cast<unsigned char>(c) < 0x80);
        std::string where;
        QVERIFY2(genko::test::strict_equal(one_line(ascii.out), snapshot, &where), where.c_str());
        const auto full = genko_run({"inspect", "--full", book});
        QCOMPARE(one_line(full.out)["pages"][0]["ink_strokes"].size(), std::size_t{2});
    }

    void inspectStroke() {
        const QString book = genko::test::test_data("legacy/book-v3.genko");
        const auto r = genko_run({"inspect", book, "--stroke", "be2170a0e2fb"});
        QCOMPARE(r.exit_code, 0);
        QCOMPARE(r.out, QByteArray("{\"id\": \"be2170a0e2fb\", \"page\": 1, \"layer\": \"ink\", \"points\": [[100.0, 120.0], "
                                   "[140.0, 160.0]], \"kind\": \"gpen\"}\n"));
        const auto missing = genko_run({"inspect", book, "--stroke=nope"});
        QCOMPARE(missing.exit_code, 1);
        QCOMPARE(one_line(missing.out)["error"].get<std::string>(), std::string("no stroke nope"));
    }

    void schemaIsTheEmbeddedFile() {
        const auto r = genko_run({"schema"});
        QCOMPARE(r.exit_code, 0);
        const Json out = one_line(r.out);
        QCOMPARE(out["ok"], Json(true));
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/src/core/data/ops_schema.json");
        std::string where;
        QVERIFY2(genko::test::strict_equal(out["ops"], file, &where), where.c_str());
        QCOMPARE(out["ops"].size(), std::size_t{168});
        QVERIFY(r.out.startsWith("{\"ok\": true, \"ops\": [{\"op\": \"split_frame\""));
    }
};

QTEST_GUILESS_MAIN(TestCliE2e)
#include "test_cli_e2e.moc"
