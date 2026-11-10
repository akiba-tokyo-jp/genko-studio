// `genko apply` against `python -m genko apply` with the 3D ops: the books of geom3d_harness.py fixtures (for the C++
// build a v4 copy made by `genko migrate`: it writes v4 books only), the same ops files and flags (--dry-run, --agent)
// give the same exit code and the same JSON — ok, applied, the snapshot and warnings, or the same error text — and the
// books as saved then hold the same: the full snapshot and the 3D state (prims, rulers, camera and light, every
// layer's strokes, pixels and screen). New ids are random on both sides (job_id, the strokes trace_prims draws, a prim
// made without one): they are compared by where they first appear. The C++ reply also has revision and txn (schema
// v4), not compared. Skipped without the Python reference.

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "core/model.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "test3d.hpp"

using genko::core::Json;

namespace {

struct CliCase {
    const char* book;   // a, strict or locked
    const char* agent;  // "" for none (Python's own choice: genko, or legacy:unknown for a studio book)
    bool dry_run;
    const char* ops;
};

// $F1, $F2: page 1's panels in reading order; $NAME: a model file of geom3d_harness.py fixture_models
const CliCase kCases[] = {
    {"a", "", false, R"([{"op": "add_figure", "page": 1, "id": "c1", "preset": "walk", "frame_id": "$F1", "pos": [40, 120, 0]}])"},
    {"a", "human:作者", false,
     R"([{"op": "add_head", "page": 1, "id": "c2", "pos": [100, 40, 0]}, {"op": "add_hand", "page": 1, "id": "c3", "side": "r", "pose": "fist"},
         {"op": "set_camera", "page": 1, "turn": 0.4, "tip": 0.15, "focal_mm": 300}])"},
    {"a", "", false, R"([{"op": "import_model", "page": 2, "id": "c4", "obj": $CUBE, "size_mm": 40, "pos": [90, 100, 0]}])"},
    {"a", "", false, R"([{"op": "import_model", "page": 2, "id": "c5", "glb": $GLB_NODES}])"},
    {"a", "", false, R"([{"op": "import_model", "page": 2, "id": "c6", "gltf": $GLTF, "size_mm": 25}])"},
    {"a", "", false, R"([{"op": "import_model", "page": 2, "id": "c7", "obj": "v 0 0 0"}])"},
    {"a", "", false, R"([{"op": "import_model", "page": 2, "id": "c8", "glb": $GLB_SHORT}])"},
    {"a", "", false, R"([{"op": "import_model", "page": 2, "id": "c8", "gltf": $GLTF_EXTERNAL}])"},
    {"a", "", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "drag": {"handle": "r_hand", "ik": true, "to": [80, 90]}}])"},
    {"a", "", false,
     R"([{"op": "pose_mannequin", "page": 1, "id": "man", "preset": "sit"},
         {"op": "pose_mannequin", "page": 1, "id": "man", "drag": {"handle": "l_hand", "to": [140, 120]}}])"},
    {"a", "", false,
     R"([{"op": "edit_prim", "page": 1, "id": "box", "pos": [90, 140, 10], "size": [20, 20, 20], "rot": [0.3, 0.2, 0]},
         {"op": "delete_prim", "page": 1, "id": "chair"}])"},
    {"a", "", false, R"([{"op": "render_prims", "page": 1, "layer_id": "paint"}])"},
    {"a", "", false, R"([{"op": "render_prims", "page": 2, "layer_id": "paint2", "surfaces": false, "tone": {"lpi": 50}}])"},
    {"a", "", false, R"([{"op": "trace_prims", "page": 1, "layer_id": "pen", "kind": "gpen", "ids": ["box", "fig"]}])"},
    {"a", "", false, R"([{"op": "trace_prims", "page": 2, "layer": "ink"}])"},
    {"a", "", false, R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "box", "id": "c9", "grid": 10}])"},
    {"a", "", false, R"([{"op": "camera_from_ruler", "page": 1, "id": "pr3", "prim_id": "stairs"}])"},
    {"a", "", false, R"([{"op": "set_light", "page": 1, "dir": [0.2, -1, -0.5], "ambient": 0.3}])"},
    {"a", "", false, R"([{"op": "add_scene", "page": 2, "id": "c10", "kind": "street", "size": [150, 80, 200]}])"},
    {"a", "", false, R"([{"op": "add_mannequin", "page": 2, "id": "c11", "preset": "jump", "height_mm": 90}])"},
    {"a", "", false, R"([{"op": "add_prim3d", "page": 2, "id": "c12", "kind": "cylinder", "size": [20, 40, 20], "rot": [0, 0.4, 0]}])"},
    {"a", "", false, R"([{"op": "add_figure", "page": 2, "pos": [30, 30, 0]}, {"op": "trace_prims", "page": 2, "layer": "ink"}])"},
    {"a", "", true, R"([{"op": "add_figure", "page": 1, "id": "c13"}, {"op": "render_prims", "page": 1, "layer_id": "paint"}])"},
    {"a", "", false, R"([{"op": "add_figure", "page": 1, "id": "c14"}, {"op": "pose_figure", "page": 1, "id": "nope", "preset": "walk"}])"},
    {"a", "", false, R"([{"op": "set_camera", "page": 1, "target": [5]}])"},
    {"a", "", false, R"([{"op": "add_prim3d", "page": 1, "id": "c15", "kind": "torus"}])"},
    {"a", "", false, R"([{"op": "add_figure", "page": 1, "id": "fig"}])"},
    {"a", "", false, R"([{"op": "trace_prims", "page": 1, "layer_id": "frozen"}])"},
    {"a", "", false, R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "nope"}])"},
    {"a", "", false, R"([{"op": "camera_from_ruler", "page": 1, "id": "ln"}])"},
    {"strict", "ai:hermes", false, R"([{"op": "render_prims", "page": 2, "layer_id": "paint2"}])"},
    {"strict", "ai:hermes", false, R"([{"op": "trace_prims", "page": 1, "layer_id": "pen"}])"},
    {"strict", "", false, R"([{"op": "add_figure", "page": 1, "id": "c16", "frame_id": "$F2"}])"},
    {"locked", "ai:hermes", false, R"([{"op": "add_figure", "page": 1, "id": "c17"}])"},
    {"locked", "ai:hermes", false, R"([{"op": "add_figure", "page": 2, "id": "c18"}, {"op": "set_light", "page": 2, "ambient": 0.6}])"},
    {"locked", "human:作者", false, R"([{"op": "pose_figure", "page": 1, "id": "fig", "preset": "run"}])"},
};

// Every string of a JSON value (the ids a book has before the ops).
void strings_of(const Json& value, std::set<std::string>& out) {
    if (value.is_string()) {
        out.insert(value.get<std::string>());
    } else if (value.is_array() || value.is_object()) {
        for (const auto& item : value) strings_of(item, out);
    }
}

// New ids (12 hex digits the book did not have) replaced by the order in which they first appear.
void plain_ids(Json& value, const std::set<std::string>& known, std::map<std::string, std::string>& seen) {
    static const std::regex hex12("[0-9a-f]{12}");
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (!known.contains(text) && std::regex_match(text, hex12)) {
            auto it = seen.find(text);
            if (it == seen.end()) it = seen.emplace(text, "<new id " + std::to_string(seen.size() + 1) + ">").first;
            value = it->second;
        }
    } else if (value.is_array()) {
        for (auto& item : value) plain_ids(item, known, seen);
    } else if (value.is_object()) {
        for (auto& [key, item] : value.items()) plain_ids(item, known, seen);
    }
}

// The book after the ops as compared: its full snapshot and 3D state, the strokes' ids replaced by their order (the
// strokes' bytes then compared whole), new ids as plain_ids.
Json saved_state(const QString& book, const std::set<std::string>& known) {
    genko::core::Document doc = genko::storage::load_document(genko::storage::path_from_utf8(book.toStdString())).document;
    std::map<std::string, std::string> seen;
    int n = 0;
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        genko::core::Page& page = doc.edit_page(p);
        for (auto& layer : page.layers) {
            std::vector<genko::core::StrokePtr> items;
            for (const auto& stroke : layer.strokes->items) {
                auto copy = std::make_shared<genko::core::Stroke>(*stroke);
                if (!known.contains(copy->id)) copy->id = "s" + std::to_string(++n);
                items.push_back(std::move(copy));
            }
            if (!items.empty()) layer.strokes = genko::core::make_strokes(std::move(items));
        }
    }
    Json out = Json::object({{"full", genko::storage::snapshot(doc, true)}, {"state", genko::test::state_of(doc)}});
    plain_ids(out, known, seen);
    return out;
}

}  // namespace

class TestContract3dCli : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    Json models_;
    std::map<std::string, std::vector<std::string>> frames_;
    std::map<std::string, std::set<std::string>> known_;  // book → every string it holds (ids among them)

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

    std::string expand(const CliCase& c) {
        std::string text = c.ops;
        std::vector<std::string> names;  // (the longest first: $GLB_NODES before $GLB)
        for (const auto& [name, value] : models_.items()) names.push_back(name);
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        for (const std::string& name : names) text = genko::test::replace_all(text, "$" + name, genko::core::dump_python(models_[name]));
        const auto& frames = frames_[c.book];
        for (std::size_t i = 0; i < frames.size(); ++i) text = genko::test::replace_all(text, "$F" + std::to_string(i + 1), frames[i]);
        return text;
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        const auto r = genko::test::geom3d_harness({"fixtures", path("books")}, path("py-home"));
        QVERIFY2(r.finished && r.exit_code == 0, r.err.right(3000).constData());
        models_ = genko::test::read_json(path("books/models.json"));
        for (const char* book : {"a", "strict", "locked"}) {
            const QString dir = path(QStringLiteral("books/%1.genko").arg(book));
            const auto loaded = genko::storage::load_document(genko::storage::path_from_utf8(dir.toStdString()));
            for (const genko::core::Frame* f : loaded.document.page(0).leaf_frames()) frames_[book].push_back(f->id);
            strings_of(genko::storage::snapshot(loaded.document, true), known_[book]);
            strings_of(genko::test::state_of(loaded.document), known_[book]);
            for (const auto& page : loaded.document.pages) {
                for (const auto& layer : page->layers) {
                    known_[book].insert(layer.id);
                    for (const auto& stroke : layer.strokes->items) known_[book].insert(stroke->id);
                }
            }
            // the C++ build's copy, in v4
            const auto migrated = genko::test::run_genko({"migrate", dir, path(QStringLiteral("v4/%1.genko").arg(book))});
            QVERIFY2(migrated.finished && migrated.exit_code == 0, (migrated.out + migrated.err).constData());
        }
        QDir().mkpath(path("ops"));
    }

    void applyMatchesPython() {
        int failures = 0;
        int ok = 0;
        int refused = 0;
        for (std::size_t n = 0; n < std::size(kCases); ++n) {
            const CliCase& c = kCases[n];
            const QString ops = path(QStringLiteral("ops/%1.json").arg(n));
            genko::test::write_bytes(ops, expand(c));
            const QString py_book = path(QStringLiteral("py/%1.genko").arg(n));
            const QString cpp_book = path(QStringLiteral("cpp/%1.genko").arg(n));
            genko::test::copy_tree(path(QStringLiteral("books/%1.genko").arg(c.book)), py_book);
            genko::test::copy_tree(path(QStringLiteral("v4/%1.genko").arg(c.book)), cpp_book);
            QStringList flags;
            if (c.dry_run) flags << "--dry-run";
            if (*c.agent) flags << "--agent" << QString::fromUtf8(c.agent);
            const auto py = genko::test::run(genko::test::python_ref(), QStringList{"-m", "genko", "apply", py_book, ops} + flags,
                                             genko::test::python_env(path("py-home")));
            const auto cpp = genko::test::run_genko(QStringList{"apply", cpp_book, ops} + flags);
            const std::string label = "case " + std::to_string(n) + " " + std::string(c.ops).substr(0, 120);
            QVERIFY2(py.finished && cpp.finished, label.c_str());
            if (py.exit_code != cpp.exit_code) {
                qWarning("%s: exit %d, Python %d: %s", label.c_str(), cpp.exit_code, py.exit_code, (cpp.out + py.err.right(600)).constData());
                ++failures;
                continue;
            }
            Json mine = genko::core::parse_python_json(cpp.out.toStdString());
            Json theirs = genko::core::parse_python_json(py.out.toStdString());
            for (const char* key : {"job_id", "revision", "txn"}) {
                mine.erase(key);
                theirs.erase(key);
            }
            mine.erase("code");  // (the C++ build names its errors too)
            std::map<std::string, std::string> seen_mine;
            std::map<std::string, std::string> seen_theirs;
            plain_ids(mine, known_[c.book], seen_mine);
            plain_ids(theirs, known_[c.book], seen_theirs);
            std::string where;
            if (!genko::test::strict_equal(mine, theirs, &where)) {
                qWarning("%s: the replies differ: %s", label.c_str(), where.c_str());
                ++failures;
                continue;
            }
            (cpp.exit_code == 0 ? ok : refused) += 1;
            if (!genko::test::strict_equal(saved_state(cpp_book, known_[c.book]), saved_state(py_book, known_[c.book]), &where)) {
                qWarning("%s: the books as saved differ: %s", label.c_str(), where.c_str());
                ++failures;
            }
        }
        qInfo("genko apply: %zu cases, %d applied, %d refused", std::size(kCases), ok, refused);
        QVERIFY(ok >= 20);
        QVERIFY(refused >= 8);
        QCOMPARE(failures, 0);
    }
};

QTEST_GUILESS_MAIN(TestContract3dCli)
#include "test_contract_3d_cli.moc"
