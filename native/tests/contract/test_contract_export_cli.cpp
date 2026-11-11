// `genko export`: each format written (exit 0); its refusals — a resolution past what an export takes, for every format
// that takes one, and what cannot be written (no animation, no page, nothing recorded, CMYK as PNG, a screen too coarse,
// an RGB profile for CMYK, no book) — as a JSON error with nothing written; usage errors exit 2 with nothing written;
// the time-lapse; a long title's file names.

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>

#include <string>

#include "core/json.hpp"
#include "render/colour.hpp"
#include "testsupport.hpp"

using genko::core::Json;

class TestContractExportCli : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString books_;

    genko::test::Run cpp(const QStringList& args) {
        return genko::test::run(genko::test::genko_cli(), args, QProcessEnvironment::systemEnvironment());
    }
    QString book(const QString& name) const { return books_ + "/" + name + ".genko"; }

private slots:
    void initTestCase() {
        QVERIFY(scratch_.isValid());
        books_ = scratch_.path() + "/books";
        QVERIFY(QDir().mkpath(books_));
        // (a book of two pages, and one whose second page is an animation)
        for (const char* name : {"mono", "anim"}) {
            const auto made = cpp({"new", book(QString::fromLatin1(name)), "--pages", "2"});
            QVERIFY2(made.finished && made.exit_code == 0, made.err.right(3000).constData());
        }
        const QString ops = scratch_.path() + "/anim-ops.json";
        genko::test::write_bytes(ops, R"([{"op": "set_animation", "page": 2, "frames": 4, "fps": 8}])");
        const auto applied = cpp({"apply", book("anim"), ops});
        QVERIFY2(applied.finished && applied.exit_code == 0, (applied.out + applied.err.right(2000)).constData());
        genko::test::write_bytes(scratch_.path() + "/srgb.icc", genko::render::colour::srgb_icc());
    }

    // A resolution past what an export takes (1 to 100000 dpi), for every format that takes one: refused before
    // anything is drawn or written, as a JSON error, exit 1.
    void resolutionsOutOfRange_data() {
        QTest::addColumn<QString>("book");
        QTest::addColumn<QStringList>("options");
        for (const char* format : {"png", "pdf", "tiff", "cmyk", "layers", "psd", "pack", "epub", "strip", "animation"}) {
            for (const char* dpi : {"-5", "100001"}) {
                QStringList options{"--format", format, "--dpi", dpi};
                if (std::string(format) == "animation") options << "--page" << "2";
                QTest::newRow(qPrintable(QStringLiteral("%1 %2").arg(format, dpi))) << (std::string(format) == "animation" ? "anim" : "mono") << options;
            }
        }
    }

    void resolutionsOutOfRange() {
        QFETCH(QString, book);
        QFETCH(QStringList, options);
        const QString dir = scratch_.path() + "/resolution/" + QString::fromUtf8(QTest::currentDataTag()).replace(' ', '_');
        const auto got = cpp(QStringList{"export", this->book(book), dir + "/o"} + options);
        QVERIFY(got.finished);
        QCOMPARE(got.exit_code, 1);
        QCOMPARE(genko::core::parse_python_json(got.out.toStdString()),
                 (Json{{"ok", false}, {"error", "the resolution must be between 1 and 100000 dpi"}, {"code", "value"}}));
        QVERIFY2(!QFileInfo::exists(dir), qPrintable(dir));
    }

    // Each format written: exit 0 (its reply in JSON with --json), something written where it was asked.
    void formats_data() {
        QTest::addColumn<QString>("book");
        QTest::addColumn<QString>("out");
        QTest::addColumn<QStringList>("options");
        const QString icc = QString::fromUtf8(GENKO_TEST_DATA) + "/colour/photocraft-coated-cmyk.icc";
        QTest::newRow("png") << "mono" << "o" << QStringList{"--dpi", "40"};
        QTest::newRow("png-gray-trim-json") << "mono" << "o" << QStringList{"--format", "png", "--dpi", "40", "--color", "gray", "--area", "trim", "--json"};
        QTest::newRow("png-bleed") << "mono" << "o" << QStringList{"--dpi=30", "--area=bleed"};
        QTest::newRow("pdf") << "mono" << "o" << QStringList{"--format", "pdf", "--dpi", "40", "--area", "bleed"};
        QTest::newRow("pdf-gray") << "mono" << "o" << QStringList{"--format", "pdf", "--dpi", "40", "--color", "gray"};
        QTest::newRow("pdf-cmyk") << "mono" << "o" << QStringList{"--format", "pdf", "--dpi", "30", "--color", "cmyk", "--icc", icc};
        QTest::newRow("tiff") << "mono" << "o" << QStringList{"--format", "tiff", "--dpi", "40"};
        QTest::newRow("tiff-screen") << "mono" << "o" << QStringList{"--format", "tiff", "--dpi", "50", "--screen-lpi", "30", "--screen-shape", "ellipse"};
        QTest::newRow("cmyk-icc") << "mono" << "o" << QStringList{"--format", "cmyk", "--dpi", "40", "--icc", icc, "--area", "trim"};
        QTest::newRow("cmyk") << "mono" << "o" << QStringList{"--format", "cmyk", "--dpi", "30"};
        QTest::newRow("strip-folder") << "mono" << "o" << QStringList{"--format", "strip", "--dpi", "30"};
        QTest::newRow("strip-file") << "mono" << "s.png" << QStringList{"--format", "strip"};
        QTest::newRow("psd-folder") << "mono" << "o" << QStringList{"--format", "psd", "--dpi", "40"};
        QTest::newRow("psd-file") << "mono" << "one.psd" << QStringList{"--format", "psd", "--dpi", "30"};
        QTest::newRow("epub-folder") << "mono" << "o" << QStringList{"--format", "epub", "--dpi", "30"};
        QTest::newRow("epub-file") << "mono" << "b.epub" << QStringList{"--format", "epub", "--dpi", "30"};
        QTest::newRow("kindle") << "mono" << "o" << QStringList{"--format", "kindle", "--long-edge", "300"};
        QTest::newRow("kindle-file") << "mono" << "k.epub" << QStringList{"--format", "kindle", "--long-edge", "260"};
        QTest::newRow("webtoon") << "mono" << "o" << QStringList{"--format", "webtoon", "--width", "200", "--max-height", "300"};
        QTest::newRow("webtoon-jpeg-json") << "mono" << "o" << QStringList{"--format", "webtoon", "--width", "150", "--jpeg", "--json"};
        QTest::newRow("sns") << "mono" << "o" << QStringList{"--format", "sns", "--long-edge", "240", "--spreads"};
        QTest::newRow("sns-jpeg") << "mono" << "o" << QStringList{"--format", "sns", "--long-edge", "200", "--jpeg", "--spreads"};
        QTest::newRow("pack") << "mono" << "o" << QStringList{"--format", "pack", "--dpi", "40"};
        QTest::newRow("layers") << "mono" << "o" << QStringList{"--format", "layers", "--dpi", "40", "--area", "bleed"};
        QTest::newRow("animation") << "anim" << "o" << QStringList{"--format", "animation", "--page", "2", "--dpi", "30"};
        QTest::newRow("animation-apng") << "anim" << "a.png" << QStringList{"--format", "animation", "--page", "2", "--dpi", "30", "--json"};
        QTest::newRow("animation-frames") << "anim" << "f.frames" << QStringList{"--format", "animation", "--page", "2", "--dpi", "25"};
        // (a resolution the format does not take: left alone)
        QTest::newRow("sns-dpi") << "mono" << "o" << QStringList{"--format", "sns", "--long-edge", "120", "--dpi", "-5"};
    }

    void formats() {
        QFETCH(QString, book);
        QFETCH(QString, out);
        QFETCH(QStringList, options);
        const QString dir = scratch_.path() + "/formats/" + QString::fromUtf8(QTest::currentDataTag());
        const auto got = cpp(QStringList{"export", this->book(book), dir + "/" + out} + options);
        QVERIFY2(got.finished && got.exit_code == 0, (got.out + got.err.right(3000)).constData());
        if (options.contains("--json")) QCOMPARE(genko::core::parse_python_json(got.out.toStdString()).value("ok", Json()), Json(true));
        QVERIFY2(QDir(dir).exists() && !QDir(dir).isEmpty(), qPrintable(dir));
    }

    // The refusals of an export: exit 1, the error in JSON (and its code), nothing written.
    void refusals_data() {
        QTest::addColumn<QString>("book");
        QTest::addColumn<QString>("out");
        QTest::addColumn<QStringList>("options");
        QTest::addColumn<QString>("error");
        QTest::addColumn<QString>("code");
        QTest::newRow("not-animation") << "mono" << "o" << QStringList{"--format", "animation"} << "the page is not an animation (set_animation first)" << "value";
        QTest::newRow("no-page") << "anim" << "o" << QStringList{"--format", "animation", "--page", "9"} << "no page 9" << "usage";
        QTest::newRow("no-recording") << "mono" << "o" << QStringList{"--format", "timelapse"} << "nothing has been recorded yet (turn the timelapse on and work for a while)" << "value";
        QTest::newRow("cmyk-png") << "mono" << "o" << QStringList{"--dpi", "40", "--color", "cmyk"} << "CMYK is written as TIFF or PDF" << "value";
        QTest::newRow("screen-lpi") << "mono" << "o" << QStringList{"--format", "tiff", "--dpi", "40", "--screen-lpi", "5"} << "screen lpi must be between 10 and 150" << "value";
        QTest::newRow("rgb-profile") << "mono" << "o" << QStringList{"--format", "pdf", "--color", "cmyk", "--icc", "$SRGB", "--dpi", "40"} << "the profile is not a CMYK printing profile"
                                     << "value";
        QTest::newRow("animation-jpeg") << "anim" << "a.jpeg" << QStringList{"--format", "animation", "--page", "2"} << "format must be one of gif, webp, png, mp4, frames" << "value";
        QTest::newRow("no-book") << "nowhere" << "o" << QStringList{"--dpi", "40"} << "not found: [Errno 2] No such file or directory: '$BOOKS/nowhere.genko/project.json'" << "not_found";
    }

    void refusals() {
        QFETCH(QString, book);
        QFETCH(QString, out);
        QFETCH(QStringList, options);
        QFETCH(QString, error);
        QFETCH(QString, code);
        options.replaceInStrings(QStringLiteral("$SRGB"), scratch_.path() + "/srgb.icc");
        error.replace(QStringLiteral("$BOOKS"), books_);
        const QString dir = scratch_.path() + "/refused/" + QString::fromUtf8(QTest::currentDataTag());
        const auto got = cpp(QStringList{"export", this->book(book), dir + "/" + out} + options);
        QVERIFY(got.finished);
        QVERIFY2(got.exit_code == 1, (got.out + got.err.right(3000)).constData());
        // (the reply on stdout; a page that is not there is said on stderr, as the command line's other usage errors)
        const bool on_stderr = code == QStringLiteral("usage");
        QVERIFY2(on_stderr ? got.out.isEmpty() : !got.out.isEmpty(), (got.out + got.err.right(3000)).constData());
        const Json reply = genko::core::parse_python_json((on_stderr ? got.err.trimmed().split('\n').last() : got.out).toStdString());
        QCOMPARE(reply.value("ok", Json()), Json(false));
        QCOMPARE(QString::fromStdString(reply.value("error", std::string())), error);
        QCOMPARE(QString::fromStdString(reply.value("code", std::string())), code);
        QDirIterator written(dir, QDir::Files, QDirIterator::Subdirectories);  // (nothing written; a folder may be made)
        QVERIFY2(!written.hasNext(), qPrintable(dir));
    }

    // The time-lapse of a book recorded: a GIF at 4 fps, and one page's frames for a second (a WebP in the folder).
    void timelapse() {
        const QString lapse = scratch_.path() + "/lapse.genko";
        genko::test::copy_tree(book("mono"), lapse);
        for (const char* ops : {R"([{"op": "set_timelapse", "on": true}])",
                                R"([{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[10, 10], [50, 60]]}])",
                                R"([{"op": "add_stroke", "page": 2, "layer": "ink", "points": [[20, 70], [40, 20]]}])"}) {
            const QString file = scratch_.path() + "/lapse-ops.json";
            genko::test::write_bytes(file, ops);
            const auto applied = cpp({"apply", lapse, file});
            QVERIFY2(applied.finished && applied.exit_code == 0, (applied.out + applied.err.right(2000)).constData());
        }
        const QString gif = scratch_.path() + "/lapse/t.gif";
        const auto all = cpp({"export", lapse, gif, "--format", "timelapse", "--fps", "4"});
        QVERIFY2(all.finished && all.exit_code == 0, (all.out + all.err.right(2000)).constData());
        QCOMPARE(all.out.trimmed(), gif.toUtf8());
        QVERIFY(genko::test::read_bytes(gif).starts_with("GIF8"));
        const QString page = scratch_.path() + "/lapse/o";
        const auto one = cpp({"export", lapse, page, "--format", "timelapse", "--page", "2", "--seconds", "1"});
        QVERIFY2(one.finished && one.exit_code == 0, (one.out + one.err.right(2000)).constData());
        QCOMPARE(one.out.trimmed(), (page + "/timelapse.webp").toUtf8());
        QVERIFY(genko::test::read_bytes(page + "/timelapse.webp").starts_with("RIFF"));
    }

    // A title of 80 letters (240 bytes in UTF-8): the files are named within what a file system takes (249–254 bytes).
    void aLongTitle() {
        const QString long_book = scratch_.path() + "/long.genko";
        const auto made = cpp({"new", long_book, "--title", QString(80, QChar(0x3042)), "--pages", "2", "--paper", "a5"});
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(3000).constData());
        for (const QStringList& options : {QStringList{"--dpi", "20"}, QStringList{"--format", "pdf", "--dpi", "20"}}) {
            const QString dir = scratch_.path() + "/long-" + options.join('_');
            const auto got = cpp(QStringList{"export", long_book, dir + "/o"} + options);
            QVERIFY2(got.finished && got.exit_code == 0, (got.out + got.err.right(3000)).constData());
            QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
            int files = 0;
            while (it.hasNext()) {
                const QByteArray name = QFileInfo(it.next()).fileName().toUtf8();
                QVERIFY2(name.size() >= 249 && name.size() <= 254, name.constData());
                ++files;
            }
            QVERIFY(files >= 1);
        }
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
        const auto got = cpp(full);
        QCOMPARE(got.exit_code, 2);
        QVERIFY(got.out.isEmpty());
        QVERIFY(!QFileInfo::exists(out));
    }
};

QTEST_GUILESS_MAIN(TestContractExportCli)
#include "test_contract_export_cli.moc"
