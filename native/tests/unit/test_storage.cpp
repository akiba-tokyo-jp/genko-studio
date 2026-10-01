// Storage without the Python reference: the asset store, atomic writes, what the reader refuses or reports (versions,
// missing and broken assets, values it cannot hold, unknown keys and features) and v4 round trips.

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>

#include <filesystem>
#include <map>
#include <string>

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "core/strokes.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
using genko::core::Json;
using genko::storage::AssetStore;
using genko::storage::LoadResult;

namespace {

fs::path to_path(const QString& path) { return genko::storage::path_from_utf8(path.toStdString()); }

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    QFile file(QString::fromStdString(genko::storage::path_to_utf8(path)));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes.data(), static_cast<qint64>(bytes.size()));
}

// A book folder from a payload and some files (relative path → bytes).
fs::path make_book(const QTemporaryDir& tmp, const std::string& name, const Json& payload,
                   const std::map<std::string, std::string>& files = {}) {
    const fs::path dir = to_path(tmp.path()) / name;
    fs::create_directories(dir);
    write_file(dir / "project.json", genko::core::dump_python_indent2(payload));
    for (const auto& [rel, bytes] : files) write_file(dir / genko::storage::path_from_utf8(rel), bytes);
    return dir;
}

Json minimal_book(int version = 3) {
    Json book = genko::core::parse_python_json(R"({
        "title": "t", "episode": 1,
        "spec": {"width_mm": 210, "height_mm": 297, "dpi": 600, "bleed_mm": 3, "inner_margin_mm": 10},
        "pages": [{"index": 1, "frames": [{"id": "f1", "rect": {"x": 13, "y": 13, "width": 184, "height": 271}}],
                   "layers": [{"id": "l1", "role": "ink", "kind": "strokes"}]}]})");
    book["version"] = version;
    return book;
}

std::string load_error(const fs::path& dir) {
    try {
        genko::storage::load_document(dir);
    } catch (const genko::storage::UnsupportedProjectVersion& error) {
        return "unsupported: " + std::string(error.what());
    } catch (const genko::core::Error& error) {
        return error.code() + ": " + error.what();
    }
    return "(no error)";
}

bool has_issue(const LoadResult& result, const std::string& kind, const std::string& pointer) {
    for (const auto& issue : result.report.issues) {
        if (issue.kind == kind && issue.pointer == pointer) return true;
    }
    qWarning("no %s at %s in %s", kind.c_str(), pointer.c_str(), genko::core::dump_python(result.report.to_json()).c_str());
    return false;
}

void copy_tree(const QString& from, const QString& to) {
    QDirIterator it(from, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString target = to + file.mid(from.size());
        QDir().mkpath(QFileInfo(target).path());
        QVERIFY(QFile::copy(file, target));
    }
}

std::vector<std::string> keys_of(const Json& object) {
    std::vector<std::string> out;
    for (const auto& [key, value] : object.items()) out.push_back(key);
    return out;
}

}  // namespace

class TestStorage : public QObject {
    Q_OBJECT

private slots:
    void testDataMatchesItsManifests() {
        // (docs/cpp-migration/FIXTURES.md: fixed test assets are listed with their sha256; no test may change them)
        for (const char* folder : {"legacy", "pyref"}) {
            const QString root = genko::test::test_data(folder);
            const Json manifest = genko::test::read_json(root + "/MANIFEST.json");
            QSet<QString> listed;
            for (const auto& [rel, sha] : manifest["files"].items()) {
                const std::string bytes = genko::test::read_bytes(root + "/" + QString::fromStdString(rel));
                QVERIFY2(AssetStore::ref(bytes) == "sha256:" + sha.get<std::string>(), rel.c_str());
                listed.insert(QString::fromStdString(rel));
            }
            QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString rel = it.next().mid(root.size() + 1);
                if (rel == "MANIFEST.json" || rel.endsWith("project.lock")) continue;
                QVERIFY2(listed.contains(rel), qPrintable("not in " + root + "/MANIFEST.json: " + rel));
            }
        }
    }

    void assetRefsAndPaths() {
        const std::string empty = AssetStore::ref("");
        QCOMPARE(empty, std::string("sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
        QCOMPARE(AssetStore::relpath(empty, ".png"),
                 std::string("assets/e3/e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855.png"));
        for (const char* bad : {"sha256:XYZ", "md5:abc", "sha256:E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855",
                                "sha256:../../../../../../../../../../../../../../../../../../../../../../etc/passwd", ""}) {
            QVERIFY2(!AssetStore::is_ref(bad), bad);
            QVERIFY_THROWS_EXCEPTION(genko::core::Error, AssetStore::relpath(bad, ".png"));
        }
        for (const char* bad : {"png", "", ".a/b", ".a\\b", "..png", ".x..y"}) {
            QVERIFY2(!AssetStore::is_suffix(bad), bad);
            QVERIFY_THROWS_EXCEPTION(genko::core::Error, AssetStore::relpath(empty, bad));
        }
        QVERIFY(AssetStore::is_suffix(".strokes.json") && AssetStore::is_suffix(".state.json"));
    }

    void assetStorePutGetAll() {
        QTemporaryDir tmp;
        AssetStore store(to_path(tmp.path()));
        const std::string ref = store.put_bytes("hello", ".png");
        QCOMPARE(ref, AssetStore::ref("hello"));
        QVERIFY(store.has(ref, ".png"));
        QVERIFY(!store.has(ref, ".strokes.json"));
        QCOMPARE(store.get_bytes(ref, ".png").value(), std::string("hello"));
        QCOMPARE(store.put_bytes("hello", ".png"), ref);  // already there: not written again
        QVERIFY(!store.get_bytes(AssetStore::ref("other"), ".png").has_value());
        write_file(store.root() / "ab" / "leftover.png.123.tmp", "x");
        const auto files = store.all_files();
        QCOMPARE(files.size(), std::size_t{1});
        QCOMPARE(files[0], store.path(ref, ".png"));
        store.put_known(AssetStore::ref("x"), "x", ".png");
        QCOMPARE(store.all_files().size(), std::size_t{2});
    }

    void writeAtomic() {
        QTemporaryDir tmp;
        const fs::path target = to_path(tmp.path()) / "a" / "b" / "file.json";
        genko::storage::write_atomic(target, "one");
        QCOMPARE(genko::storage::read_file(target), std::string("one"));
        genko::storage::write_atomic(target, "two");
        QCOMPARE(genko::storage::read_file(target), std::string("two"));
        QCOMPARE(QDir(QString::fromStdString(genko::storage::path_to_utf8(target.parent_path()))).entryList(QDir::Files),
                 QStringList({"file.json"}));  // no temporary file left behind
    }

    void readFileErrors() {
        QTemporaryDir tmp;
        const fs::path missing = to_path(tmp.path()) / "nope" / "project.json";
        try {
            genko::storage::read_file(missing);
            QFAIL("no error");
        } catch (const genko::core::Error& error) {
            QCOMPARE(error.code(), std::string("not_found"));
            QCOMPARE(std::string(error.what()),
                     "[Errno 2] No such file or directory: '" + genko::storage::path_to_utf8(missing) + "'");
        }
        try {
            genko::storage::read_file(to_path(tmp.path()));
            QFAIL("no error");
        } catch (const genko::core::Error& error) {
            QCOMPARE(error.code(), std::string("io"));
            QVERIFY(std::string(error.what()).starts_with("[Errno 21] Is a directory"));
        }
    }

    void versions() {
        QTemporaryDir tmp;
        Json book = minimal_book();
        book["version"] = 99;
        QCOMPARE(load_error(make_book(tmp, "v99", book)),
                 std::string("unsupported: project.json version 99 is newer than this build supports (4); update Genko"));
        book["version"] = "3";
        QCOMPARE(load_error(make_book(tmp, "vs", book)),
                 std::string("unsupported: project.json version 3 is newer than this build supports (4); update Genko"));
        book["version"] = 3.0;
        QVERIFY(load_error(make_book(tmp, "vf", book)).starts_with("unsupported: project.json version 3.0"));
        book["version"] = genko::core::parse_python_json("18446744073709551616");
        QVERIFY(load_error(make_book(tmp, "vbig", book)).starts_with("unsupported:"));
        book["version"] = true;  // (Python: a bool is an int, so this is version 1)
        QCOMPARE(genko::storage::load_document(make_book(tmp, "vb", book)).report.source_version, 1);
        book.erase("version");
        QCOMPARE(genko::storage::load_document(make_book(tmp, "v1", book)).report.source_version, 1);
        book["version"] = 4;
        book["min_reader"] = 5;
        QVERIFY(load_error(make_book(tmp, "mr5", book)).starts_with("unsupported: project.json needs Genko that reads version 5"));
        book["min_reader"] = "4";
        QVERIFY(load_error(make_book(tmp, "mrs", book)).starts_with("unsupported:"));
        book["min_reader"] = 4;
        const auto v4 = genko::storage::load_document(make_book(tmp, "v4", book));
        QCOMPARE(v4.report.source_version, 4);
        QVERIFY(has_issue(v4, "repair", "/book_id"));  // a v4 book must carry its id
        QVERIFY(!v4.document.read_only_reason.empty());
        QVERIFY(genko::core::is_book_id(v4.document.book_id));
    }

    void malformedFiles() {
        QTemporaryDir tmp;
        const fs::path dir = to_path(tmp.path()) / "bad";
        write_file(dir / "project.json", "{\"title\": ");
        QCOMPARE(load_error(dir), std::string("json: Expecting value: line 1 column 11 (char 10)"));
        write_file(dir / "project.json", "\xEF\xBB\xBF{}");
        QCOMPARE(load_error(dir), std::string("json: Unexpected UTF-8 BOM (decode using utf-8-sig): line 1 column 1 (char 0)"));
        write_file(dir / "project.json", "[]");
        QVERIFY(load_error(dir).starts_with("format: project.json must hold an object"));
        Json book = minimal_book();
        book.erase("spec");
        QVERIFY(load_error(make_book(tmp, "nospec", book)).starts_with("format: missing key 'spec'"));
        book = minimal_book();
        book["binding"] = "up";
        QVERIFY(load_error(make_book(tmp, "binding", book)).starts_with("format: 'up' is not a valid Binding"));
        book = minimal_book();
        book["pages"][0]["layers"][0]["role"] = "hero";
        QVERIFY(load_error(make_book(tmp, "role", book)).starts_with("format: 'hero' is not a valid LayerRole"));
        book = minimal_book();
        book["pages"][0]["layers"][0]["opacity"] = "half";
        QVERIFY(load_error(make_book(tmp, "opacity", book)).starts_with("format: could not convert string to float: 'half'"));
    }

    void missingAndBrokenAssetsAreReported() {
        QTemporaryDir tmp;
        const std::string png = "\x89PNG picture";
        const std::string ref = AssetStore::ref(png);
        const std::string absent = AssetStore::ref("absent");
        Json book = minimal_book();
        Json& layers = book["pages"][0]["layers"];
        layers.push_back(Json::object({{"id", "r1"}, {"role", "bg"}, {"kind", "raster"}, {"asset", absent}}));
        layers.push_back(Json::object({{"id", "r2"}, {"role", "bg"}, {"kind", "raster"}, {"asset", "sha256:zz"}}));
        layers.push_back(Json::object({{"id", "r3"}, {"role", "bg"}, {"kind", "raster"}, {"asset", ref},
                                       {"mask", Json::object({{"enabled", true}, {"asset", absent}})},
                                       {"patches", Json::array({Json::object({{"id", "p1"}, {"asset", absent}}),
                                                                Json::object({{"id", "p2"}, {"asset", ref}})})}}));
        layers.push_back(Json::object({{"id", "s1"}, {"role", "ink"}, {"kind", "strokes"}, {"strokes_blob", absent}}));
        const auto result = genko::storage::load_document(make_book(tmp, "assets", book, {{AssetStore::relpath(ref, ".png"), png}}));
        QVERIFY(has_issue(result, "missing_asset", "/pages/0/layers/1/asset"));
        QVERIFY(has_issue(result, "broken_ref", "/pages/0/layers/2/asset"));
        QVERIFY(has_issue(result, "missing_asset", "/pages/0/layers/3/mask/asset"));
        QVERIFY(has_issue(result, "missing_asset", "/pages/0/layers/3/patches/0/asset"));
        QVERIFY(has_issue(result, "missing_asset", "/pages/0/layers/4/strokes_blob"));
        QCOMPARE(result.report.issues.size(), std::size_t{5});
        const auto& page = result.document.page(0);
        QVERIFY(!page.layers[1].raster_png && !page.layers[2].raster_png);
        QCOMPARE(*page.layers[3].raster_png, png);
        QVERIFY(!page.layers[3].mask);                  // (Python drops a mask whose picture is missing)
        QCOMPARE(page.layers[3].patches.size(), std::size_t{2});
        QVERIFY(!page.layers[3].patches[0].png && page.layers[3].patches[1].png);
        QCOMPARE(page.layers[4].stroke_count(), std::size_t{0});
        QVERIFY(result.document.read_only_reason.find("needs repairs") != std::string::npos);
        genko::storage::ProjectLock lock(to_path(tmp.path()) / "out");
        lock.try_acquire();
        QVERIFY_THROWS_EXCEPTION(genko::core::Error,
                                 genko::storage::Saver(lock).save(result.document, genko::storage::SaveRequest{}));
        QVERIFY(!QFile::exists(tmp.path() + "/out/project.json"));
    }

    void brokenAssetsAreReported() {
        QTemporaryDir tmp;
        const std::string png = "real bytes";
        const std::string wrong = AssetStore::ref("other bytes");
        const std::string blob = "[{\"xy\": \"not base64!\"}]";
        const std::string blob_ref = AssetStore::ref(blob);
        Json book = minimal_book();
        Json& layers = book["pages"][0]["layers"];
        layers.push_back(Json::object({{"id", "r1"}, {"role", "bg"}, {"kind", "raster"}, {"asset", wrong}}));
        layers[0]["strokes_blob"] = blob_ref;
        const auto result = genko::storage::load_document(make_book(
            tmp, "broken", book,
            {{AssetStore::relpath(wrong, ".png"), png}, {AssetStore::relpath(blob_ref, ".strokes.json"), blob}}));
        QVERIFY(has_issue(result, "hash_mismatch", "/pages/0/layers/1/asset"));
        QVERIFY(has_issue(result, "broken_asset", "/pages/0/layers/0/strokes_blob"));
        QCOMPARE(*result.document.page(0).layers[1].raster_png, png);  // used, as Python uses it
        QVERIFY(!result.document.read_only_reason.empty());
    }

    void valuesThatCannotBeHeldAreRepairs() {
        QTemporaryDir tmp;
        Json book = minimal_book();
        book["future"] = Json::object({{"big", 1}});
        std::string text = genko::core::dump_python_indent2(book);
        // NaN where a float belongs, an integer beyond 64 bits in an unknown key, a string where a bool belongs
        text.replace(text.find("\"kind\": \"strokes\""), 17, "\"kind\": \"strokes\", \"opacity\": NaN, \"visible\": \"yes\"");
        text.replace(text.find("\"big\": 1"), 8, "\"big\": 123456789012345678901234567890");
        const fs::path dir = to_path(tmp.path()) / "repairs";
        write_file(dir / "project.json", text);
        const auto result = genko::storage::load_document(dir);
        QVERIFY2(has_issue(result, "repair", "/pages/0/layers/0/opacity"), genko::core::dump_python(result.report.to_json()).c_str());
        QVERIFY(has_issue(result, "repair", "/future/big"));
        QVERIFY(has_issue(result, "repair", "/pages/0/layers/0/visible"));
        const auto& layer = result.document.page(0).layers[0];
        QCOMPARE(layer.opacity, 1.0);  // the default instead of NaN
        QVERIFY(layer.visible);        // "yes" is true
        QVERIFY(!result.document.read_only_reason.empty());
    }

    void unknownKeysAndFeatures() {
        QTemporaryDir tmp;
        Json book = minimal_book(4);
        book["book_id"] = genko::core::new_book_id();
        book["features"] = Json::array({"ruby-layout@1", "ruby-layout@1", "zeta@2"});
        book["pages"][0]["layers"][0]["mystery"] = 1;
        book["pages"][0]["frames"][0]["zzz"] = true;
        book["pages"][0]["layers"][0]["fit"] = "cover";  // a placed layer's key on a strokes layer
        book["pages"][0]["future_page_key"] = "kept";
        book["future_top"] = Json::array({1});
        const auto result = genko::storage::load_document(make_book(tmp, "unknown", book));
        QVERIFY(has_issue(result, "unknown_key", "/pages/0/layers/0/mystery"));
        QVERIFY(has_issue(result, "unknown_key", "/pages/0/frames/0/zzz"));
        QVERIFY(has_issue(result, "unknown_key", "/pages/0/layers/0/fit"));
        QCOMPARE(result.document.features, std::vector<std::string>({"ruby-layout@1", "zeta@2"}));
        QVERIFY(result.document.read_only_reason.starts_with("this book uses features this build does not support"));
        QCOMPARE(result.document.page(0).extra["future_page_key"].get<std::string>(), std::string("kept"));
        QCOMPARE(result.document.extra["future_top"].size(), std::size_t{1});
        try {
            genko::storage::ProjectLock lock(to_path(tmp.path()) / "out");
            lock.try_acquire();
            genko::storage::Saver(lock).save(result.document, genko::storage::SaveRequest{});
            QFAIL("a read-only book was saved");
        } catch (const genko::core::Error& error) {
            QCOMPARE(error.code(), std::string("read_only"));
        }
    }

    void placedLayersKeepOnlyTheirPicture() {
        QTemporaryDir tmp;
        Json book = minimal_book();
        book["pages"][0]["layers"].push_back(genko::core::parse_python_json(R"({"id": "p", "role": "user", "kind": "placed",
            "asset": "sha256:00", "strokes": [[[0, 0], [1, 1]]], "patches": [{"id": "x"}], "fit": "", "source": [1]})"));
        const auto result = genko::storage::load_document(make_book(tmp, "placed", book));
        QVERIFY(has_issue(result, "unknown_key", "/pages/0/layers/1/strokes"));
        QVERIFY(has_issue(result, "unknown_key", "/pages/0/layers/1/patches"));
        QCOMPARE(result.report.issues.size(), std::size_t{2});
        const auto& layer = result.document.page(0).layers[1];
        QCOMPARE(layer.stroke_count(), std::size_t{1});  // (as Python: read and counted, but never written)
        QVERIFY(layer.patches.empty());
        QCOMPARE(layer.asset.value(), std::string("sha256:00"));
        QCOMPARE(layer.fit, std::string("cover"));
        QCOMPARE(genko::core::dump_python(layer.source.value()), std::string("[1]"));
    }

    void legacyRasterPathsStayInside() {
        QTemporaryDir tmp;
        write_file(to_path(tmp.path()) / "outside.png", "secret");
        Json book = minimal_book(2);
        Json& layers = book["pages"][0]["layers"];
        layers.push_back(Json::object({{"id", "a"}, {"role", "bg"}, {"kind", "raster"}, {"raster_relpath", "../outside.png"}}));
        layers.push_back(Json::object({{"id", "b"}, {"role", "bg"}, {"kind", "raster"}, {"raster_relpath", "pages/001/b.png"}}));
        layers.push_back(Json::object({{"id", "c"}, {"role", "bg"}, {"kind", "raster"}, {"raster_relpath", "pages/001/c.png"}}));
        const auto result = genko::storage::load_document(make_book(tmp, "legacy", book, {{"pages/001/c.png", "C"}}));
        QVERIFY(has_issue(result, "broken_ref", "/pages/0/layers/1/raster_relpath"));
        QVERIFY(has_issue(result, "missing_asset", "/pages/0/layers/2/raster_relpath"));
        const auto& page = result.document.page(0);
        QVERIFY(!page.layers[1].raster_png);
        QCOMPARE(*page.layers[3].raster_png, std::string("C"));
    }

    void v4RoundTrip() {
        QTemporaryDir tmp;
        const auto first = genko::storage::load_document(to_path(genko::test::test_data("legacy/book-v3.genko")));
        QVERIFY2(first.report.clean(), genko::core::dump_python(first.report.to_json()).c_str());
        QCOMPARE(first.report.source_version, 3);
        const fs::path out = to_path(tmp.path()) / "book.genko";
        genko::test::write_project(first.document, out);
        const std::string text = genko::storage::read_file(out / "project.json");
        const Json payload = genko::core::parse_python_json(text);
        QCOMPARE(keys_of(payload),
                 std::vector<std::string>({"future_top", "version", "min_reader", "writer", "book_id", "features", "revision",
                                           "title", "episode", "binding", "start_side", "strict_gates", "autosave",
                                           "font_path", "page_locks", "brush", "spec", "bible", "tickets", "studio", "pages",
                                           "story"}));
        QCOMPARE(payload["version"].get<int>(), 4);
        QCOMPARE(payload["min_reader"].get<int>(), 4);
        QCOMPARE(genko::core::dump_python(payload["writer"]), std::string("{\"app\": \"genko-native\", \"version\": \"0.1.0\"}"));
        QCOMPARE(payload["features"], Json::array());
        QCOMPARE(payload["book_id"].get<std::string>(), first.document.book_id);
        QCOMPARE(payload["future_top"]["n"].get<std::int64_t>(), std::int64_t{12345678901234});
        QVERIFY(text.back() == '}' && text.find('\r') == std::string::npos);

        const auto second = genko::storage::load_document(out);
        QVERIFY2(second.report.clean(), genko::core::dump_python(second.report.to_json()).c_str());
        QCOMPARE(second.report.source_version, 4);
        QCOMPARE(second.document.book_id, first.document.book_id);
        QCOMPARE(second.document.revision, first.document.revision);
        std::string where;
        QVERIFY2(genko::test::strict_equal(genko::storage::snapshot(second.document, true),
                                           genko::storage::snapshot(first.document, true), &where),
                 where.c_str());
        // writing the same book again changes nothing and adds no asset
        AssetStore store(out);
        const auto files = store.all_files();
        genko::test::write_project(second.document, out);
        QCOMPARE(genko::storage::read_file(out / "project.json"), text);
        QCOMPARE(store.all_files(), files);
    }

    void loadedBlobsAreReusedInTheirOwnBook() {
        QTemporaryDir tmp;
        // a blob as an older writer might have left it (an int width): the same folder keeps it, as Python's
        // blobcache does; a new folder gets the canonical blob of the strokes
        const std::string blob = "[{\"id\":\"s1\",\"kind\":\"gpen\",\"width_mm\":1,\"xy\":\"AAAAAAAA8D8AAAAAAAAAQA==\"}]";
        const std::string ref = AssetStore::ref(blob);
        Json book = minimal_book();
        book["pages"][0]["layers"][0]["strokes_blob"] = ref;
        const fs::path dir = make_book(tmp, "blob", book, {{AssetStore::relpath(ref, ".strokes.json"), blob}});
        const auto loaded = genko::storage::load_document(dir);
        QVERIFY(loaded.report.clean());
        const auto& strokes = *loaded.document.page(0).layers[0].strokes;
        QCOMPARE(strokes.blob_ref, ref);
        AssetStore same(dir);
        QCOMPARE(genko::storage::project_payload_v4(loaded.document, same)["pages"][0]["layers"][0]["strokes_blob"].get<std::string>(), ref);
        AssetStore other(to_path(tmp.path()) / "elsewhere");
        const std::string fresh =
            genko::storage::project_payload_v4(loaded.document, other)["pages"][0]["layers"][0]["strokes_blob"].get<std::string>();
        QVERIFY(fresh != ref);
        QCOMPARE(other.get_bytes(fresh, ".strokes.json").value(),
                 std::string("[{\"id\":\"s1\",\"kind\":\"gpen\",\"width_mm\":1.0,\"xy\":\"AAAAAAAA8D8AAAAAAAAAQA==\"}]"));
        // an edited list is written afresh
        auto doc = loaded.document;
        auto items = doc.page(0).layers[0].strokes->items;
        items.pop_back();
        doc.edit_page(0).layers[0].strokes = genko::core::make_strokes(items);
        QVERIFY(!genko::storage::project_payload_v4(doc, same)["pages"][0]["layers"][0].contains("strokes_blob"));
    }

    void newBooksKeepTheirNumbers() {
        QTemporaryDir tmp;
        auto doc = genko::core::new_episode("新しい本", 2, 2, genko::core::PageSpec::b4_comic(), genko::core::Binding::Left);
        doc.revision = 1;
        auto& page = doc.edit_page(0);
        page.split_frame(page.frames[0].id, "vertical", 0.4, 5);
        doc.add_line(1, "台詞", "A");
        const fs::path out = to_path(tmp.path()) / "new.genko";
        genko::test::write_project(doc, out);
        const auto loaded = genko::storage::load_document(out);
        QVERIFY(loaded.report.clean());
        const Json payload = genko::core::parse_python_json(genko::storage::read_file(out / "project.json"));
        QVERIFY(payload["spec"]["width_mm"].is_number_integer());       // the preset's ints stay ints
        QVERIFY(payload["spec"]["margins_mm"][0].is_number_integer());
        QVERIFY(payload["story"][0]["x_mm"].is_number_integer());       // add_line's 0, as Python writes it
        QVERIFY(loaded.document.story[0].x_mm.same(genko::core::Num(0.0)));  // and read back as a float (float())
        QCOMPARE(loaded.document.page(0).leaf_frames().size(), std::size_t{2});
        QCOMPARE(payload["binding"].get<std::string>(), std::string("left"));
    }

    void projectJsonFromCopiedLegacyBookInPlace() {
        // the v3 test book copied, then written as v4 in its own folder: every asset is reused
        QTemporaryDir tmp;
        const QString copy = tmp.path() + "/book-v3.genko";
        copy_tree(genko::test::test_data("legacy/book-v3.genko"), copy);
        const auto loaded = genko::storage::load_document(to_path(copy));
        AssetStore store(to_path(copy));
        const auto before = store.all_files();
        genko::test::write_project(loaded.document, to_path(copy));
        QCOMPARE(store.all_files(), before);
    }
};

QTEST_GUILESS_MAIN(TestStorage)
#include "test_storage.moc"
