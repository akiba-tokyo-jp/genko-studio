// `genko render` against `python -m genko render`: the same arguments give the same JSON and a PNG with the same
// pixels, for pages of random books (tools/migration/render_harness.py make-books) in every mode, a jacket's and a
// band's folds too (drawn since M4②: nothing in these books is left undrawn now; a page with something not drawn yet
// would say so, {"code": "not_yet_ported"}, and write nothing); the usage and save errors match
// (a page that is not there: the same exit code, an error in JSON where Python stops with a traceback). Python's
// command line runs with Pillow held to its BASIC text layout, as the reference was measured (render_harness.py does
// the same): a reference Python whose Pillow has raqm would otherwise lay out the letters of the balloons by it.
// Skipped without the Python reference.

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <set>
#include <string>

#include "core/json.hpp"
#include "render/png.hpp"
#include "rendertest.hpp"

using genko::core::Json;

class TestContractRenderCli : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString books_;
    Json manifest_;

    // `python -m genko <args>`, Pillow held to BASIC (ImageFont.core.HAVE_RAQM = False before genko is imported)
    genko::test::Run python(const QStringList& args) {
        static const QString basic = QStringLiteral(
            "import runpy\nfrom PIL import ImageFont\nImageFont.core.HAVE_RAQM = False\n"
            "runpy.run_module('genko', run_name='__main__', alter_sys=True)\n");
        return genko::test::run(genko::test::python_ref(), QStringList{"-c", basic} + args, genko::test::python_env(scratch_.path()));
    }
    genko::test::Run cpp(const QStringList& args) {
        return genko::test::run(genko::test::genko_cli(), args, QProcessEnvironment::systemEnvironment());
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        books_ = scratch_.path() + QStringLiteral("/books");
        const auto py = genko::test::render_harness({"make-books", books_, "--seed", "77", "--count", "8"}, scratch_.path());
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        manifest_ = genko::test::read_json(books_ + QStringLiteral("/MANIFEST.json"));
    }

    void pages() {
        int compared = 0;
        int not_yet = 0;
        int folded = 0;  // (a jacket or a band drawn in name or proof: its folds and their names)
        for (const auto& [book, pages] : manifest_.items()) {
            std::set<std::string> covers;
            const Json payload = genko::test::read_json(books_ + QLatin1Char('/') + QString::fromStdString(book) + "/project.json");
            for (const Json& page : payload["pages"]) {
                if (page.contains("cover")) covers.insert(genko::core::dump_python(page["index"]));
            }
            for (const auto& [index, unported] : pages.items()) {
                for (const char* mode : {"print", "proof", "name"}) {
                    const QString dir = books_ + QLatin1Char('/') + QString::fromStdString(book);
                    const QString out = scratch_.path() + QStringLiteral("/out/page.png");
                    const QStringList args{"render", dir, "--page", QString::fromStdString(index), "--dpi", "100", "--mode", mode, "--out", out};
                    QFile::remove(out);
                    const auto py = python(args);
                    QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
                    const genko::render::Image want = genko::render::read_png(genko::test::read_bytes(out));
                    QFile::remove(out);
                    const auto got = cpp(args);
                    QVERIFY(got.finished);
                    const Json got_json = genko::core::parse_python_json(got.out.toStdString());
                    if (got.exit_code == 1 && got_json.value("code", "") == "not_yet_ported") {
                        // only a page with something not drawn yet may say so (or one whose onion page has)
                        QVERIFY2(!unported.empty(), (book + " p" + index + " " + mode + ": " + got.out.toStdString()).c_str());
                        QVERIFY(!QFile::exists(out));
                        ++not_yet;
                        continue;
                    }
                    QVERIFY2(got.exit_code == 0, (got.out + got.err).constData());
                    QCOMPARE(got.out, py.out);  // the same JSON, byte for byte
                    const genko::render::Image image = genko::render::read_png(genko::test::read_bytes(out));
                    QCOMPARE(std::string(image.mode()), std::string("RGB"));
                    QVERIFY2(image.tobytes() == want.tobytes(), (book + " p" + index + " " + mode).c_str());
                    ++compared;
                    if (std::string(mode) != "print" && covers.contains(index)) ++folded;
                }
            }
        }
        qInfo("render: %d pages the same (%d with folds), %d not yet ported", compared, folded, not_yet);
        QVERIFY(compared >= 20);
        QCOMPARE(not_yet, 0);
        QVERIFY(folded >= 4);
    }

    void errors() {
        const QString book = books_ + QStringLiteral("/book-00.genko");
        // an extension Pillow does not know
        const QStringList bad{"render", book, "--page", "1", "--mode", "name", "--out", scratch_.path() + "/x.unknown"};
        const auto py = python(bad);
        const auto got = cpp(bad);
        QCOMPARE(got.exit_code, py.exit_code);
        const Json p = genko::core::parse_python_json(py.out.toStdString());
        const Json c = genko::core::parse_python_json(got.out.toStdString());
        QCOMPARE(c["ok"], p["ok"]);
        QCOMPARE(c["error"], p["error"]);
        // a page that is not there: Python stops with a traceback (StopIteration, exit 1); here an error in JSON
        const QString nowhere = scratch_.path() + QStringLiteral("/missing.png");
        const QStringList missing{"render", book, "--page", "99", "--out", nowhere};
        QCOMPARE(python(missing).exit_code, 1);
        const auto none = cpp(missing);
        QCOMPARE(none.exit_code, 1);
        const Json n = genko::core::parse_python_json(none.out.toStdString());
        QVERIFY2(n.value("ok", true) == false && n.value("code", "") == "not_found", none.out.constData());
        QVERIFY(!QFile::exists(nowhere));
        // usage errors exit with 2
        QCOMPARE(cpp({"render", book}).exit_code, 2);
        QCOMPARE(python({"render", book}).exit_code, 2);
        QCOMPARE(cpp({"render", book, "--out", "x.png", "--page", "one"}).exit_code, 2);
        QCOMPARE(python({"render", book, "--out", "x.png", "--page", "one"}).exit_code, 2);
        QCOMPARE(cpp({"render", book, "--out", "x.png", "--mode", "draft"}).exit_code, 2);
        QCOMPARE(python({"render", book, "--out", "x.png", "--mode", "draft"}).exit_code, 2);
    }
};

QTEST_GUILESS_MAIN(TestContractRenderCli)
#include "test_contract_render_cli.moc"
