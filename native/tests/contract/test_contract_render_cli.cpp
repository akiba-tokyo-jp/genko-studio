// `genko render`'s refusals: a page that is not there is an error in JSON (exit 1) and writes nothing; usage errors
// exit with 2.

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include <string>

#include "core/json.hpp"
#include "testsupport.hpp"

using genko::core::Json;

class TestContractRenderCli : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString book_;

    genko::test::Run cpp(const QStringList& args) {
        return genko::test::run(genko::test::genko_cli(), args, QProcessEnvironment::systemEnvironment());
    }

private slots:
    void initTestCase() {
        QVERIFY(scratch_.isValid());
        book_ = scratch_.path() + QStringLiteral("/book.genko");
        const auto made = cpp({"new", book_, "--pages", "2"});
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(3000).constData());
    }

    void errors() {
        // a page that is not there: an error in JSON
        const QString nowhere = scratch_.path() + QStringLiteral("/missing.png");
        const auto none = cpp({"render", book_, "--page", "99", "--out", nowhere});
        QCOMPARE(none.exit_code, 1);
        const Json n = genko::core::parse_python_json(none.out.toStdString());
        QVERIFY2(n.value("ok", true) == false && n.value("code", "") == "not_found", none.out.constData());
        QVERIFY(!QFile::exists(nowhere));
        // usage errors exit with 2
        QCOMPARE(cpp({"render", book_}).exit_code, 2);
        QCOMPARE(cpp({"render", book_, "--out", "x.png", "--page", "one"}).exit_code, 2);
        QCOMPARE(cpp({"render", book_, "--out", "x.png", "--mode", "draft"}).exit_code, 2);
    }
};

QTEST_GUILESS_MAIN(TestContractRenderCli)
#include "test_contract_render_cli.moc"
