// The C++ reader and writer against the Python baseline (src/genko run by the reference Python):
//   1. the legacy books (v1, v2, v3): snapshot(full=True) equal to Python's, as JSON with the same types, key order
//      and float bits (ids made while reading come from the same counting source on both sides);
//   2. the same books written as v4 and read again: the same snapshot; project.json without the five v4 keys (and
//      with version 3) equal to Python's save_episode of the book (revision aside); the same asset bytes;
//   3. fifty random books made by Python (new_episode and direct field assignment, saved as v3): read, written as
//      v4, read and written again: nothing lost or changed;
//   4. the numbers and JSON underneath: repr, round, sum, hypot/dist, format(…, "g"), json.dumps, json.loads errors.
// Skipped when there is no reference Python ($GENKO_PYREF or /opt/pyref/bin/python).

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <bit>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
using genko::core::Json;
using genko::core::Num;

namespace {

constexpr int kRandomBooks = 50;

fs::path to_path(const QString& path) { return genko::storage::path_from_utf8(path.toStdString()); }

double from_bits(const Json& hex) { return std::bit_cast<double>(std::stoull(hex.get<std::string>(), nullptr, 16)); }

std::string bits_of(double v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(v)));
    return buf;
}

// project.json v4 as Python's v3 writer would write it: without min_reader, writer, book_id and features, with
// version 3; and without revision (the C++ plain save keeps it, Python's save adds one).
Json as_v3(Json payload) {
    for (const char* key : {"min_reader", "writer", "book_id", "features", "revision"}) payload.erase(key);
    payload["version"] = 3;
    return payload;
}

// Hand-made old books that lean on Python's conversions while reading (float("3.5"), str(None or ""), int("7"),
// tuple(color)[:3], a v1 page painted from its fills, page texts without a story, a strokes blob holding all three
// stroke forms, …). Every id that is missing is made while reading, so both sides count ids from 1.
const char* kCraftedV1 = R"({
  "title": "crafted v1", "episode": 2, "binding": "left",
  "spec": {"width_mm": 182, "height_mm": 257, "dpi": 350, "bleed_mm": 0, "inner_margin_mm": 12.5},
  "page_locks": {"1": "human:a", "2": "ai:b", "9": "x"},
  "pages": [
    {"index": 1, "note": "n", "name_ok": true, "stage": "name",
     "frames": [{"id": "r1", "rect": {"x": 12.5, "y": 12.5, "width": 157, "height": 232}, "split_axis": "vertical",
                 "children": [{"id": "a", "rect": {"x": 12.5, "y": 12.5, "width": 76.5, "height": 232}, "children": []},
                              {"id": "b", "rect": {"x": 93, "y": 12.5, "width": 76.5, "height": 232}, "children": []}]}],
     "fills": {"bg": [255, 255, 255], "draft": [200, 200, 255], "tone": [1, 2, 3]},
     "name_strokes": [[[1, 2, 0.5], [3, 4, 0.75]], [[5, 6]]],
     "ink_strokes": [[[10.5, 20.25], [30, 40, 0.3]]],
     "texts": [{"id": "t1", "page_index": 1, "text": "ページ1"}, {"id": "t2", "page_index": 2, "text": "次の頁へ"}]},
    {"index": 2, "frames": [{"id": "r2", "rect": {"x": 12.5, "y": 12.5, "width": 157, "height": 232}}],
     "texts": [{"id": "t3", "page_index": 2, "text": "ページ2", "speaker": "B"}]}
  ]
})";

const char* kCraftedV2 = R"({
  "version": 2, "title": "crafted v2", "episode": 1,
  "brush": {"rgb": [1.5, "2", 3], "width_mm": "0.5", "stabilize": "3", "taper": 1, "curve": null},
  "spec": {"width_mm": 210, "height_mm": 297, "dpi": 600, "bleed_mm": 3, "inner_margin_mm": 10, "preset": "a4-mono"},
  "page_locks": {"1": "human:作者"},
  "story": [{"id": "s1", "page_index": 1, "text": "本文", "x_mm": "3.5", "tail": [], "ruby_runs": null, "style": null,
             "tails": null, "emphasis_runs": [1, 2], "style_runs": [["強", {"bold": true}], ["x"]]}],
  "pages": [{"index": 1,
             "frames": [{"id": "f", "rect": {"x": 13, "y": 13, "width": 184, "height": 271}, "corner_mm": null,
                         "curves": "abc", "line": [1], "split": {}, "poly": [], "border_mm": 1}],
             "texts": [{"id": "ignored", "page_index": 1, "text": "x"}],
             "layers": [
               {"id": "bg1", "role": "bg", "kind": "raster", "raster_relpath": "pages/001/bg.png"},
               {"id": "n1", "role": "name", "strokes": [{"points": [[1, 2], [3, 4]], "pressure": [0.1, 0.2]},
                                                         {"id": "keep", "points": [[5, 6, 0.9]], "opacity": 0.5, "rgb": [1, 2, 3]}]},
               {"role": "ink", "title": null, "blend": "", "opacity": 1, "angle": 30, "color": [1, 2, 3, 4], "lpi": 60,
                "density": 0.5, "region": [[1, 2], [3.5, 4]], "strokes": [[[1, 1], [2, 2]]]}
             ],
             "art_ok": 1, "effects": null, "rulers": null, "prims": null}]
})";

const char* kCraftedV3 = R"({
  "version": 3, "revision": "7", "title": "crafted v3", "episode": 5, "start_side": "left", "strict_gates": 1,
  "autosave": 0, "font_path": null, "page_locks": null, "tickets": null, "nombre": null, "studio": null,
  "bible": {"plot": "p", "characters": null, "constraints": null},
  "spec": {"width_mm": 210.0, "height_mm": 297, "dpi": 600, "bleed_mm": 3, "inner_margin_mm": 10, "trim_w_mm": 0,
           "trim_h_mm": 291, "margins_mm": [10, "12", 14, 16], "expression": "color"},
  "pages": [{"id": "pg_x", "index": 1, "frames": [{"id": "f", "rect": {"x": 13, "y": 13, "width": 184, "height": 271}}],
             "layers": [{"id": "L", "role": "ink", "strokes_blob": "BLOB", "stroke_count": 99},
                        {"id": "M", "role": "user", "kind": "placed", "asset": "sha256:00", "placement_mm": {"x": 1, "y": 2.5, "width": 3, "height": 4},
                         "fit": "", "clip_to": null, "source": [1], "finish": {"mono": 1}}],
             "fills": {"ink": [9, 9, 9]}}]
})";

const char* kCraftedBlob =
    R"([{"xy": "AAAAAAAA8D8AAAAAAAAAQA==", "p": "", "kind": null, "width_mm": 1, "rgb": [1.9, 2, 3]}, )"
    R"({"points": [[1, 2, 3]], "id": "d", "rotation": [5, 6]}, [[7, 8], [9, 10, 0.5]]])";

// Every asset a project.json refers to: {ref, suffix}.
void collect_refs(const Json& payload, std::set<std::pair<std::string, std::string>>& out) {
    for (const Json& page : payload["pages"]) {
        for (const Json& layer : page["layers"]) {
            if (layer.contains("strokes_blob")) out.emplace(layer["strokes_blob"].get<std::string>(), ".strokes.json");
            if (layer.value("kind", "") != "placed" && layer.contains("asset") && layer["asset"].is_string()) {
                out.emplace(layer["asset"].get<std::string>(), ".png");
            }
            if (layer.contains("mask") && layer["mask"].contains("asset")) out.emplace(layer["mask"]["asset"].get<std::string>(), ".png");
            if (layer.contains("patches")) {
                for (const Json& patch : layer["patches"]) {
                    if (patch.contains("asset")) out.emplace(patch["asset"].get<std::string>(), ".png");
                }
            }
        }
    }
}

}  // namespace

class TestContractStorage : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString py_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    void run_harness(const QStringList& args) {
        const auto result = genko::test::harness(args, path("pyenv"));
        QVERIFY2(result.started && result.finished && result.exit_code == 0,
                 qPrintable(QString::fromUtf8(result.err).right(4000)));
    }

    void expect_equal(const Json& got, const Json& want, const std::string& what) {
        std::string where;
        QVERIFY2(genko::test::strict_equal(got, want, &where), (what + ": " + where).c_str());
    }

    // The checks of one book: C++ reads it like Python, writes v4, reads that back, writes it again.
    void check_book(const QString& book, const QString& name, bool counting_ids) {
        const Json py_snapshot = genko::test::read_json(path("py/" + name + ".full.json"));
        const Json py_project = genko::test::read_json(path("py/" + name + ".genko/project.json"));

        genko::storage::LoadResult first;
        {
            std::optional<genko::core::ScopedIdSource> ids;
            if (counting_ids) ids.emplace(genko::core::counting_ids());
            first = genko::storage::load_document(to_path(book));
        }
        QVERIFY2(first.report.clean(), genko::core::dump_python(first.report.to_json()).c_str());
        QVERIFY(first.document.read_only_reason.empty());
        expect_equal(genko::storage::snapshot(first.document, true), py_snapshot, name.toStdString() + " snapshot");

        const fs::path v4 = to_path(path("cpp/" + name + ".genko"));
        genko::test::write_project(first.document, v4);
        const auto second = genko::storage::load_document(v4);
        QVERIFY2(second.report.clean(), genko::core::dump_python(second.report.to_json()).c_str());
        QCOMPARE(second.report.source_version, 4);
        expect_equal(genko::storage::snapshot(second.document, true), py_snapshot, name.toStdString() + " v4 snapshot");

        const Json cpp_project = genko::core::parse_python_json(genko::storage::read_file(v4 / "project.json"));
        QCOMPARE(cpp_project["version"].get<int>(), 4);
        QCOMPARE(cpp_project["min_reader"].get<int>(), 4);
        QCOMPARE(cpp_project["book_id"].get<std::string>(), first.document.book_id);
        Json py_without_revision = py_project;
        py_without_revision.erase("revision");
        expect_equal(as_v3(cpp_project), py_without_revision, name.toStdString() + " project.json");

        // the same bytes for every asset (strokes blobs: the same sha256)
        std::set<std::pair<std::string, std::string>> refs;
        collect_refs(py_project, refs);
        const genko::storage::AssetStore cpp_store(v4);
        const genko::storage::AssetStore py_store(to_path(path("py/" + name + ".genko")));
        for (const auto& [ref, suffix] : refs) {
            const auto mine = cpp_store.get_bytes(ref, suffix);
            const auto theirs = py_store.get_bytes(ref, suffix);
            QVERIFY2(mine.has_value() && theirs.has_value(), (name.toStdString() + " asset " + ref + suffix).c_str());
            QVERIFY2(*mine == *theirs, (name.toStdString() + " asset bytes " + ref + suffix).c_str());
            QCOMPARE(genko::storage::AssetStore::ref(*mine), ref);
        }

        // written again from the v4 book: the same file
        const fs::path again = to_path(path("cpp2/" + name + ".genko"));
        genko::test::write_project(second.document, again);
        QCOMPARE(genko::storage::read_file(again / "project.json"), genko::storage::read_file(v4 / "project.json"));
    }

private slots:
    void initTestCase() {
        py_ = genko::test::python_ref();
        if (py_.isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        QDir().mkpath(path("py"));
        run_harness({"make-random", path("random"), "--seed", "20261002", "--count", QString::number(kRandomBooks)});
        Json jobs = Json::array();
        const auto add = [&](const QString& book, const QString& name, bool ids) {
            jobs.push_back(Json::object({{"op", "snapshot"}, {"book", book.toStdString()}, {"full", true}, {"ids", ids},
                                         {"out", path("py/" + name + ".full.json").toStdString()}}));
            jobs.push_back(Json::object({{"op", "resave"}, {"book", book.toStdString()}, {"ids", ids},
                                         {"dest", path("py/" + name + ".genko").toStdString()}}));
        };
        for (const char* v : {"v1", "v2", "v3"}) {
            add(genko::test::test_data(QStringLiteral("legacy/book-%1.genko").arg(v)), QStringLiteral("legacy-%1").arg(v), true);
        }
        const auto craft = [&](const QString& name, const std::string& text, const std::map<std::string, std::string>& files) {
            const fs::path dir = to_path(path("crafted/" + name + ".genko"));
            fs::create_directories(dir);
            genko::storage::write_atomic(dir / "project.json", text);
            for (const auto& [rel, bytes] : files) genko::storage::write_atomic(dir / genko::storage::path_from_utf8(rel), bytes);
            add(path("crafted/" + name + ".genko"), name, true);
        };
        std::string v3 = kCraftedV3;
        const std::string blob_ref = genko::storage::AssetStore::ref(kCraftedBlob);
        v3.replace(v3.find("BLOB"), 4, blob_ref);
        craft("crafted-v1", kCraftedV1, {});
        craft("crafted-v2", kCraftedV2, {{"pages/001/bg.png", "\x89PNG crafted"}});
        craft("crafted-v3", v3, {{genko::storage::AssetStore::relpath(blob_ref, ".strokes.json"), kCraftedBlob}});
        for (int i = 0; i < kRandomBooks; ++i) {
            add(path(QStringLiteral("random/book-%1.genko").arg(i, 2, 10, QLatin1Char('0'))),
                QStringLiteral("random-%1").arg(i, 2, 10, QLatin1Char('0')), false);
        }
        QFile file(path("jobs.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromStdString(genko::core::dump_python(jobs)));
        file.close();
        run_harness({"batch", path("jobs.json")});
        run_harness({"numbers", path("numbers.json"), "--seed", "4242", "--count", "4000"});
        run_harness({"json-dumps", path("dumps.json"), "--seed", "99", "--count", "400"});
        run_harness({"json-errors", path("errors.json")});
    }

    void legacyBooks_data() {
        QTest::addColumn<QString>("version");
        QTest::newRow("v1") << "v1";
        QTest::newRow("v2") << "v2";
        QTest::newRow("v3") << "v3";
    }

    void legacyBooks() {
        QFETCH(QString, version);
        check_book(genko::test::test_data("legacy/book-" + version + ".genko"), "legacy-" + version, true);
    }

    void craftedBooks_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("v1") << "crafted-v1";
        QTest::newRow("v2") << "crafted-v2";
        QTest::newRow("v3") << "crafted-v3";
    }

    void craftedBooks() {
        QFETCH(QString, name);
        check_book(path("crafted/" + name + ".genko"), name, true);
    }

    void randomBooks() {
        for (int i = 0; i < kRandomBooks; ++i) {
            const QString name = QStringLiteral("random-%1").arg(i, 2, 10, QLatin1Char('0'));
            check_book(path(QStringLiteral("random/book-%1.genko").arg(i, 2, 10, QLatin1Char('0'))), name, false);
            if (QTest::currentTestFailed()) {
                qWarning("failed on %s", qPrintable(name));
                return;
            }
        }
    }

    void numbers() {
        const Json cases = genko::test::read_json(path("numbers.json"));
        std::size_t checked = 0;
        for (const Json& row : cases["repr"]) {
            const double x = from_bits(row[0]);
            QCOMPARE(genko::core::py_float_repr(x), row[1].get<std::string>());
            QCOMPARE(genko::core::py_format_g(x), row[2].get<std::string>());
            ++checked;
        }
        for (const Json& row : cases["round"]) {
            const double x = from_bits(row[0]);
            const int n = row[1].get<int>();
            if (row[2].is_null()) {
                QVERIFY_THROWS_EXCEPTION(genko::core::Error, genko::core::py_round(x, n));
            } else {
                const double got = genko::core::py_round(x, n);
                QVERIFY2(bits_of(got) == row[2].get<std::string>(),
                         ("round(" + genko::core::py_float_repr(x) + ", " + std::to_string(n) + ") = " +
                          genko::core::py_float_repr(got) + ", Python " + genko::core::py_float_repr(from_bits(row[2])))
                             .c_str());
            }
            ++checked;
        }
        for (const Json& row : cases["sum"]) {
            std::vector<Num> items;
            for (const Json& item : row[0]) {
                items.push_back(item[0] == "i" ? Num(item[1].get<std::int64_t>()) : Num(from_bits(item[1])));
            }
            const Num want = row[1][0] == "i" ? Num(row[1][1].get<std::int64_t>()) : Num(from_bits(row[1][1]));
            const Num got = genko::core::py_sum(items);
            QVERIFY2(got.same(want), (got.repr() + " != " + want.repr()).c_str());
            ++checked;
        }
        for (const Json& row : cases["hypot"]) {
            const double a = from_bits(row[0]), b = from_bits(row[1]), c = from_bits(row[2]), d = from_bits(row[3]);
            QCOMPARE(bits_of(genko::core::py_hypot(a, b)), row[4].get<std::string>());
            QCOMPARE(bits_of(genko::core::py_dist(a, b, c, d)), row[5].get<std::string>());
            ++checked;
        }
        QVERIFY(checked > 9000);
    }

    void jsonDumps() {
        const Json cases = genko::test::read_json(path("dumps.json"));
        QVERIFY(cases.size() >= 400);
        for (const Json& item : cases) {
            const std::string text = item["text"].get<std::string>();
            const Json value = genko::core::parse_python_json(text);
            QCOMPARE(genko::core::dump_python_indent2(value), item["indent2"].get<std::string>());
            QCOMPARE(genko::core::dump_python(value), item["default"].get<std::string>());
            QCOMPARE(genko::core::dump_canonical(value), item["canonical"].get<std::string>());
            QCOMPARE(genko::core::dump_python(value, true), item["ascii"].get<std::string>());
        }
    }

    void jsonErrors() {
        const Json cases = genko::test::read_json(path("errors.json"));
        QVERIFY(cases.size() >= 40);
        for (const Json& item : cases) {
            const std::string text = item[0].get<std::string>();
            std::string message = "(no error)";
            try {
                genko::core::parse_python_json(text);
            } catch (const genko::core::Error& error) {
                message = error.what();
            }
            const std::string want = item[1].is_null() ? "(no error)" : item[1].get<std::string>();
            QVERIFY2(message == want, (genko::core::py_repr_str(text) + ": " + message + " != " + want).c_str());
        }
    }
};

QTEST_GUILESS_MAIN(TestContractStorage)
#include "test_contract_storage.moc"
