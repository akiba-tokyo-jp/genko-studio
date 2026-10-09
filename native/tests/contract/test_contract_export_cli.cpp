// `genko export` against `python -m genko export` (genko/__main__.py's export command): every format through its
// options, with the same arguments, the same exit code, the same output (the files' paths one a line, or --json's
// {"ok", "count", "files"}), and the same files (export_files.hpp: the times Python's files carry left out; a moving
// picture as Pillow plays it: its frames, timing and loop). Python's refusals give the same JSON error and exit code;
// usage errors exit 2 on both. Python's command line runs with Pillow held to its BASIC text layout, as the reference
// was measured (render_harness.py does the same). Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <string>

#include "core/json.hpp"
#include "export_files.hpp"
#include "render/colour.hpp"
#include "rendertest.hpp"

using genko::core::Json;

class TestContractExportCli : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString books_;

    genko::test::Run python(const QStringList& args) {
        static const QString basic = QStringLiteral(
            "import runpy\nfrom PIL import ImageFont\nImageFont.core.HAVE_RAQM = False\n"
            "runpy.run_module('genko', run_name='__main__', alter_sys=True)\n");
        return genko::test::run(genko::test::python_ref(), QStringList{"-c", basic} + args, genko::test::python_env(scratch_.path() + "/pyenv"));
    }
    genko::test::Run cpp(const QStringList& args) {
        return genko::test::run(genko::test::genko_cli(), args, QProcessEnvironment::systemEnvironment());
    }
    QString book(const QString& name) const { return books_ + "/" + name + ".genko"; }

    // `export <book> <case>/<out> <options>` by both into the same place (Python's moved aside first): the exit code,
    // stdout and the files the same. Moving pictures are compared as Pillow plays them.
    void same(const QString& name, const QString& book_dir, const QString& out, const QStringList& options) {
        const QString dir = scratch_.path() + "/cli/" + name;
        const QString py_dir = scratch_.path() + "/cli_py/" + name;
        const QStringList args = QStringList{"export", book_dir, dir + "/" + out} + options;
        const auto py = python(args);
        QVERIFY2(py.finished, qPrintable(name));
        QDir().mkpath(scratch_.path() + "/cli_py");
        if (QDir(dir).exists()) QVERIFY(QDir().rename(dir, py_dir));
        const auto got = cpp(args);
        QVERIFY2(got.finished, qPrintable(name));
        QVERIFY2(got.exit_code == py.exit_code, qPrintable(QStringLiteral("%1: exit %2, Python's %3\n%4%5\n/ Python: %6%7")
                                                               .arg(name).arg(got.exit_code).arg(py.exit_code)
                                                               .arg(QString::fromUtf8(got.out), QString::fromUtf8(got.err.right(3000)),
                                                                    QString::fromUtf8(py.out), QString::fromUtf8(py.err.right(3000)))));
        if (py.exit_code == 0) {
            QVERIFY2(got.out == py.out, qPrintable(name + ": " + QString::fromUtf8(got.out) + " / Python: " + QString::fromUtf8(py.out)));
        } else if (!py.out.isEmpty()) {  // (a refusal in JSON: the same error; this build adds its code)
            const Json p = genko::core::parse_python_json(py.out.toStdString());
            const Json c = genko::core::parse_python_json(got.out.toStdString());
            QVERIFY2(c.value("ok", true) == false && c.value("error", Json()) == p.value("error", Json()),
                     qPrintable(name + ": " + QString::fromUtf8(got.out) + " / Python: " + QString::fromUtf8(py.out)));
        } else {
            QVERIFY2(got.out.isEmpty(), qPrintable(name + ": " + QString::fromUtf8(got.out)));
        }
        const bool moving = out.endsWith(".gif") || out.endsWith(".webp") || out.endsWith(".apng") || options.contains("animation") ||
                            options.contains("timelapse");
        if (!moving) {
            const QString difference = genko::test::exports::tree_difference(dir, py_dir);
            QVERIFY2(difference.isEmpty(), qPrintable(name + ": " + difference));
            return;
        }
        // a moving picture: as Pillow plays it
        QStringList files;
        const auto want = genko::test::exports::tree(py_dir);
        const auto mine = genko::test::exports::tree(dir);
        QCOMPARE(mine.size(), want.size());
        for (const auto& [rel, bytes] : want) {
            QVERIFY2(mine.contains(rel), qPrintable(name + ": missing " + rel));
            files << py_dir + "/" + rel << dir + "/" + rel;
        }
        if (files.isEmpty()) return;
        const QString info_file = scratch_.path() + "/movie-" + name + ".json";
        const auto read = genko::test::harness(QStringList{"movie-info", info_file} + files, scratch_.path() + "/pyenv");
        QVERIFY2(read.finished && read.exit_code == 0, read.err.right(3000).constData());
        const Json info = genko::test::read_json(info_file);
        for (qsizetype i = 0; i < files.size(); i += 2) {
            const Json& p = info[files[i].toStdString()];
            const Json& c = info[files[i + 1].toStdString()];
            const bool webp = files[i].endsWith(".webp");
            for (const char* key : {"format", "size", "n_frames", "loop", "durations", "sizes", "frames", "strict"}) {
                if (webp && (std::string(key) == "frames" || std::string(key) == "strict")) continue;  // (lossy: each libwebp's own)
                QVERIFY2(p.value(key, Json()) == c.value(key, Json()),
                         (name.toStdString() + " " + key + ": Python " + p.value(key, Json()).dump() + ", this build " + c.value(key, Json()).dump()).c_str());
            }
        }
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        books_ = scratch_.path() + "/books";
        const auto made = genko::test::render_harness({"make-export-books", books_}, scratch_.path());
        QVERIFY2(made.finished && made.exit_code == 0, made.err.constData());
        genko::test::write_bytes(scratch_.path() + "/srgb.icc", genko::render::colour::srgb_icc());
    }

    void formats_data() {
        QTest::addColumn<QString>("book");
        QTest::addColumn<QString>("out");
        QTest::addColumn<QStringList>("options");
        const QString icc = QString::fromUtf8(GENKO_TEST_DATA) + "/colour/photocraft-coated-cmyk.icc";
        QTest::newRow("png") << "mono" << "o" << QStringList{"--dpi", "40"};
        QTest::newRow("png-gray-trim-json") << "mono" << "o" << QStringList{"--format", "png", "--dpi", "40", "--color", "gray", "--area", "trim", "--json"};
        QTest::newRow("png-colour-bleed") << "colour" << "o" << QStringList{"--dpi=30", "--area=bleed"};
        QTest::newRow("pdf") << "colour" << "o" << QStringList{"--format", "pdf", "--dpi", "40", "--area", "bleed"};
        QTest::newRow("pdf-gray") << "mono" << "o" << QStringList{"--format", "pdf", "--dpi", "40", "--color", "gray"};
        QTest::newRow("pdf-cmyk") << "jacket" << "o" << QStringList{"--format", "pdf", "--dpi", "30", "--color", "cmyk", "--icc", icc};
        QTest::newRow("tiff") << "mono" << "o" << QStringList{"--format", "tiff", "--dpi", "40"};
        QTest::newRow("tiff-gray") << "colour" << "o" << QStringList{"--format", "tiff", "--dpi", "40", "--color", "gray", "--area", "trim"};
        QTest::newRow("tiff-screen") << "obi" << "o" << QStringList{"--format", "tiff", "--dpi", "50", "--screen-lpi", "30", "--screen-shape", "ellipse"};
        QTest::newRow("cmyk-icc") << "colour" << "o" << QStringList{"--format", "cmyk", "--dpi", "40", "--icc", icc, "--area", "trim"};
        QTest::newRow("cmyk") << "jacket" << "o" << QStringList{"--format", "cmyk", "--dpi", "30"};
        QTest::newRow("strip-folder") << "mono" << "o" << QStringList{"--format", "strip", "--dpi", "30"};
        QTest::newRow("strip-file") << "colour" << "s.png" << QStringList{"--format", "strip"};
        QTest::newRow("psd-folder") << "mono" << "o" << QStringList{"--format", "psd", "--dpi", "40"};
        QTest::newRow("psd-file") << "jacket" << "one.psd" << QStringList{"--format", "psd", "--dpi", "30"};
        QTest::newRow("epub-folder") << "obi" << "o" << QStringList{"--format", "epub", "--dpi", "30"};
        QTest::newRow("epub-file") << "colour" << "b.epub" << QStringList{"--format", "epub", "--dpi", "30"};
        QTest::newRow("kindle") << "jacket" << "o" << QStringList{"--format", "kindle", "--long-edge", "300"};
        QTest::newRow("kindle-file") << "colour" << "k.epub" << QStringList{"--format", "kindle", "--long-edge", "260"};
        QTest::newRow("webtoon") << "mono" << "o" << QStringList{"--format", "webtoon", "--width", "200", "--max-height", "300"};
        QTest::newRow("webtoon-jpeg-json") << "colour" << "o" << QStringList{"--format", "webtoon", "--width", "150", "--jpeg", "--json"};
        QTest::newRow("sns") << "colour" << "o" << QStringList{"--format", "sns", "--long-edge", "240", "--spreads"};
        QTest::newRow("sns-jpeg") << "mono" << "o" << QStringList{"--format", "sns", "--long-edge", "200", "--jpeg", "--spreads"};
        QTest::newRow("pack") << "mono" << "o" << QStringList{"--format", "pack", "--dpi", "40"};
        QTest::newRow("layers") << "jacket" << "o" << QStringList{"--format", "layers", "--dpi", "40", "--area", "bleed"};
        QTest::newRow("layers-paper") << "mono" << "o" << QStringList{"--format", "layers", "--dpi", "30"};
        QTest::newRow("animation") << "anim" << "o" << QStringList{"--format", "animation", "--page", "2", "--dpi", "30"};
        QTest::newRow("animation-apng") << "anim" << "a.png" << QStringList{"--format", "animation", "--page", "2", "--dpi", "30", "--json"};
        QTest::newRow("animation-frames") << "anim" << "f.frames" << QStringList{"--format", "animation", "--page", "2", "--dpi", "25"};
        QTest::newRow("animation-jpeg") << "anim" << "a.jpeg" << QStringList{"--format", "animation", "--page", "2"};
        // Python's refusals
        QTest::newRow("not-animation") << "mono" << "o" << QStringList{"--format", "animation"};
        QTest::newRow("no-page") << "anim" << "o" << QStringList{"--format", "animation", "--page", "9"};
        QTest::newRow("no-recording") << "mono" << "o" << QStringList{"--format", "timelapse"};
        QTest::newRow("cmyk-png") << "mono" << "o" << QStringList{"--dpi", "40", "--color", "cmyk"};
        QTest::newRow("screen-lpi") << "mono" << "o" << QStringList{"--format", "tiff", "--dpi", "40", "--screen-lpi", "5"};
        QTest::newRow("rgb-profile") << "colour" << "o" << QStringList{"--format", "pdf", "--color", "cmyk", "--icc", scratch_.path() + "/srgb.icc", "--dpi", "40"};
        QTest::newRow("no-book") << "nowhere" << "o" << QStringList{"--dpi", "40"};
    }

    void formats() {
        QFETCH(QString, book);
        QFETCH(QString, out);
        QFETCH(QStringList, options);
        same(QString::fromUtf8(QTest::currentDataTag()), this->book(book), out, options);
    }

    // The timelapse: a recording made by Python's saves (set_timelapse, lines drawn), written out by both.
    void timelapse() {
        const QString lapse = scratch_.path() + "/lapse.genko";
        genko::test::copy_tree(book("mono"), lapse);
        for (const char* ops : {R"([{"op": "set_timelapse", "on": true}])",
                                R"([{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[10, 10], [50, 60]]}])",
                                R"([{"op": "add_stroke", "page": 2, "layer": "ink", "points": [[20, 70], [40, 20]]}])"}) {
            const QString file = scratch_.path() + "/lapse-ops.json";
            genko::test::write_bytes(file, ops);
            const auto applied = python({"apply", lapse, file});
            QVERIFY2(applied.finished && applied.exit_code == 0, (applied.out + applied.err.right(2000)).constData());
        }
        same("timelapse-gif", lapse, "t.gif", {"--format", "timelapse", "--fps", "4"});
        same("timelapse-page", lapse, "o", {"--format", "timelapse", "--page", "2", "--seconds", "1"});
    }

    // Usage errors: exit 2 on both, nothing written.
    void usage_data() {
        QTest::addColumn<QStringList>("args");
        QTest::newRow("format") << QStringList{"--format", "gif"};
        QTest::newRow("png1") << QStringList{"--format", "png1"};
        QTest::newRow("dpi") << QStringList{"--dpi", "x"};
        QTest::newRow("color") << QStringList{"--color", "auto"};
        QTest::newRow("area") << QStringList{"--area", "margin"};
        QTest::newRow("shape") << QStringList{"--screen-shape", "star"};
        QTest::newRow("fps") << QStringList{"--fps", "x"};
        QTest::newRow("lpi") << QStringList{"--screen-lpi", "lots"};
        QTest::newRow("unknown") << QStringList{"--bogus"};
        QTest::newRow("no-out") << QStringList{};
    }

    void usage() {
        QFETCH(QStringList, args);
        const QString out = scratch_.path() + "/usage-out";
        QStringList full{"export", book("mono")};
        if (!args.isEmpty()) full << out;
        full += args;
        const auto py = python(full);
        const auto got = cpp(full);
        QCOMPARE(py.exit_code, 2);
        QCOMPARE(got.exit_code, 2);
        QVERIFY(got.out.isEmpty());
        QVERIFY(!QFileInfo::exists(out));
    }
};

QTEST_GUILESS_MAIN(TestContractExportCli)
#include "test_contract_export_cli.moc"
