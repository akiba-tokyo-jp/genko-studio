// `genko inspect` and `genko schema` against `python -m genko inspect` / `python -m genko schema` (the baseline's
// own command line). The v2 and v3 books must print the same bytes. The v1 book has no layers in its file, so both
// programs make its four default layers with fresh random ids; those ids are compared by shape (12 hex digits,
// distinct) and everything else exactly. Skipped when there is no reference Python.

#include <QtTest>

#include <QTemporaryDir>

#include <set>
#include <string>

#include "testsupport.hpp"

using genko::core::Json;

namespace {

// The ids of the layers made while reading (v1), replaced by their position; checked to be ids first.
Json without_layer_ids(Json snapshot, std::set<std::string>& ids) {
    for (Json& page : snapshot["pages"]) {
        int n = 0;
        for (Json& layer : page["layers"]) {
            const std::string id = layer["id"].get<std::string>();
            if (id.size() == 12 && id.find_first_not_of("0123456789abcdef") == std::string::npos) ids.insert(id);
            layer["id"] = "layer-" + std::to_string(n++);
        }
    }
    return snapshot;
}

}  // namespace

class TestContractCli : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) {
            QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        }
        QVERIFY(scratch_.isValid());
    }

    void inspect_data() {
        QTest::addColumn<QString>("version");
        QTest::addColumn<bool>("full");
        for (const char* v : {"v1", "v2", "v3"}) {
            QTest::newRow(qPrintable(QStringLiteral("%1").arg(v))) << QString(v) << false;
            QTest::newRow(qPrintable(QStringLiteral("%1 --full").arg(v))) << QString(v) << true;
        }
    }

    void inspect() {
        QFETCH(QString, version);
        QFETCH(bool, full);
        const QString book = genko::test::test_data("legacy/book-" + version + ".genko");
        QStringList args{"inspect", book};
        if (full) args << "--full";
        const auto py = genko::test::run(genko::test::python_ref(), QStringList{"-m", "genko"} + args,
                                         genko::test::python_env(scratch_.path()));
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        const auto cpp = genko::test::run(genko::test::genko_cli(), args, QProcessEnvironment::systemEnvironment());
        QVERIFY2(cpp.finished && cpp.exit_code == 0, (cpp.out + cpp.err).constData());
        const Json py_json = genko::core::parse_python_json(py.out.toStdString());
        const Json cpp_json = genko::core::parse_python_json(cpp.out.toStdString());
        std::string where;
        if (version == "v1") {
            std::set<std::string> py_ids, cpp_ids;
            QVERIFY2(genko::test::strict_equal(without_layer_ids(cpp_json, cpp_ids), without_layer_ids(py_json, py_ids), &where),
                     where.c_str());
            QCOMPARE(cpp_ids.size(), std::size_t{8});  // 2 pages × 4 layers, all ids, all different
            QCOMPARE(py_ids.size(), std::size_t{8});
        } else {
            QVERIFY2(genko::test::strict_equal(cpp_json, py_json, &where), where.c_str());
            QCOMPARE(cpp.out, py.out);  // the same bytes
        }
    }

    void schema() {
        const auto py = genko::test::run(genko::test::python_ref(), {"-m", "genko", "schema"},
                                         genko::test::python_env(scratch_.path()));
        QVERIFY2(py.finished && py.exit_code == 0, py.err.constData());
        const auto cpp = genko::test::run(genko::test::genko_cli(), {"schema"}, QProcessEnvironment::systemEnvironment());
        QVERIFY(cpp.finished && cpp.exit_code == 0);
        std::string where;
        QVERIFY2(genko::test::strict_equal(genko::core::parse_python_json(cpp.out.toStdString()),
                                           genko::core::parse_python_json(py.out.toStdString()), &where),
                 where.c_str());
        QCOMPARE(cpp.out, py.out);
    }
};

QTEST_GUILESS_MAIN(TestContractCli)
#include "test_contract_cli.moc"
