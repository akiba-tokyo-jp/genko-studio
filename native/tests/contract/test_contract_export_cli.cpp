// `genko export`'s refusals: a resolution past what an export takes, for every format that takes one, refused as a
// JSON error before anything is written; usage errors exit 2 with nothing written.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <string>

#include "core/json.hpp"
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
