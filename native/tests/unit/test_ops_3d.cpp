// The 3D ops without the Python reference (the same batches against Python's apply_ops: test_contract_3d_ops,
// test_contract_3d_random and test_contract_3d_cli): for each op a batch that succeeds and what it leaves, input it
// refuses with the book left as it was, a page another agent has locked, strict_gates where the op draws on a
// printed layer, dry runs, Undo and Redo through the journal, the book saved and read back; what the C++ build refuses
// where Python would keep a broken book; the pose library; 3D guides drawn in the proof and the name, never printed.

#include <QtTest>

#include <QTemporaryDir>

#include <bit>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/linalg3.hpp"
#include "core/model.hpp"
#include "core/poses.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"
#include "test3d.hpp"

namespace fs = std::filesystem;
namespace render = genko::render;
using genko::core::Actor;
using genko::core::ApplyError;
using genko::core::Document;
using genko::core::Json;
using genko::core::Layer;
using genko::core::LayerKind;
using genko::core::LayerRole;
using genko::core::Num;

namespace {

const genko::core::CommandBus& bus() {
    static const genko::core::CommandBus b(render::ops_registry());
    return b;
}

Json ops(const std::string& text) { return genko::core::parse_python_json(text); }

const char* const kCube = R"("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\nf 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 2 3 7 6\nf 3 4 8 7\nf 4 1 5 8")";

// Three pages; page 1 cut in two, with a picture layer, a pen layer and a locked one, a figure, a head, a hand, a
// box, a mannequin and a perspective ruler from the box.
Document base() {
    Document doc = genko::core::new_episode("3D", Num(1), 3, genko::core::PageSpec::b5_doujin());
    genko::core::Page& page = doc.edit_page(0);
    page.split_frame(page.frames[0].id, "horizontal", Num(0.5), Num(4));
    for (const auto& [id, kind, locked] : {std::tuple{"paint", LayerKind::Raster, false}, std::tuple{"pen", LayerKind::Strokes, false},
                                          std::tuple{"frozen", LayerKind::Strokes, true}}) {
        Layer layer;
        layer.id = id;
        layer.role = LayerRole::User;
        layer.kind = kind;
        layer.locked = locked;
        page.layers.push_back(layer);
    }
    return bus()
        .apply(doc, ops(R"([
            {"op": "add_figure", "page": 1, "id": "fig", "pos": [60, 120, 0], "preset": "walk"},
            {"op": "add_head", "page": 1, "id": "head", "pos": [140, 60, 0]},
            {"op": "add_hand", "page": 1, "id": "hand", "pos": [120, 90, 0], "side": "l", "pose": "peace"},
            {"op": "add_prim3d", "page": 1, "id": "box", "kind": "box", "pos": [100, 150, 0], "size": [30, 20, 25], "rot": [0.3, 0.6, 0]},
            {"op": "add_mannequin", "page": 1, "id": "man", "pos": [150, 160, 0], "height_mm": 70},
            {"op": "ruler_from_3d", "page": 1, "prim_id": "box", "id": "pr"}])"),
               Actor("genko"))
        .doc;
}

const Json* prim(const Document& doc, const std::string& id, std::size_t page = 0) {
    for (const Json& p : doc.page(page).prims) {
        if (p.value("id", std::string()) == id) return &p;
    }
    return nullptr;
}

const Layer* layer(const Document& doc, const std::string& id, std::size_t page = 0) {
    for (const Layer& l : doc.page(page).layers) {
        if (l.id == id) return &l;
    }
    return nullptr;
}

// What a book holds as compared here: its full snapshot and its 3D state (prims, rulers, camera and light, every
// layer's strokes by their bytes and pictures by their pixels).
Json held(const Document& doc) {
    return Json::object({{"full", genko::storage::snapshot(doc, true)}, {"state", genko::test::state_of(doc)}});
}

struct OpCase {
    const char* op;
    const char* good;   // a batch on page 1 that succeeds
    const char* bad;    // an op it refuses (put after the good batch: nothing of the batch stays)
    const char* error;  // the reason given
    std::function<void(const Document&)> check;  // what the good batch leaves
};

const OpCase kOps[] = {
    {"add_figure", R"([{"op": "add_figure", "page": 1, "id": "f2", "preset": "run", "height_mm": 120, "pos": [40, 150, 0]}])",
     R"({"op": "add_figure", "page": 1, "height_mm": 5})", "height_mm must be between 10 and 400",
     [](const Document& d) {
         const Json* p = prim(d, "f2");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("figure"));
         QCOMPARE((*p)["size"], Json::array({60.0, 120.0, 30.0}));
         QCOMPARE((*p)["pos"], Json::array({40.0, 150.0, 0.0}));
         QVERIFY(!(*p)["joints"].empty());  // (the preset's)
     }},
    {"add_hand", R"([{"op": "add_hand", "page": 1, "id": "h2", "side": "r", "pose": "fist", "size_mm": 30}])",
     R"({"op": "add_hand", "page": 1, "side": "x"})", "side is l or r",
     [](const Document& d) {
         const Json* p = prim(d, "h2");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("hand"));
         QCOMPARE((*p)["side"], Json("r"));
         QCOMPARE((*p)["pose"], Json("fist"));
         QCOMPARE((*p)["size"], Json::array({30.0, 30.0, 30.0}));
     }},
    {"add_head", R"([{"op": "add_head", "page": 1, "id": "h3", "rot": [0, 0.5, 0]}])", R"({"op": "add_head", "page": 1, "pos": "x"})",
     "pos is [x, y, z]",
     [](const Document& d) {
         const Json* p = prim(d, "h3");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("head"));
         QCOMPARE((*p)["size"], Json::array({30.0, 30.0, 30.0}));
         QCOMPARE((*p)["rot"], Json::array({0.0, 0.5, 0.0}));
     }},
    {"add_mannequin", R"([{"op": "add_mannequin", "page": 1, "id": "m2", "preset": "walk", "height_mm": 100}])",
     R"({"op": "add_mannequin", "page": 1, "preset": "dance"})", "preset must be one of",
     [](const Document& d) {
         const Json* p = prim(d, "m2");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("mannequin"));
         QCOMPARE((*p)["size"], Json::array({50.0, 100.0, 25.0}));
         QVERIFY((*p)["joints"].is_object() && !(*p)["joints"].empty());
     }},
    {"add_prim3d", R"([{"op": "add_prim3d", "page": 1, "id": "c1", "kind": "cylinder", "size": [20, 40, 20]}])",
     R"({"op": "add_prim3d", "page": 1, "kind": "torus"})", "kind must be box, cylinder, stairs, floor, sphere, cone or prop",
     [](const Document& d) {
         const Json* p = prim(d, "c1");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("cylinder"));
         QCOMPARE((*p)["size"], Json::array({20.0, 40.0, 20.0}));
     }},
    {"add_scene", R"([{"op": "add_scene", "page": 1, "id": "s1", "kind": "classroom"}])", R"({"op": "add_scene", "page": 1, "kind": "castle"})",
     "scene kind must be one of",
     [](const Document& d) {
         const Json* p = prim(d, "s1");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["scene"], Json("classroom"));
     }},
    {"camera_from_ruler", R"([{"op": "camera_from_ruler", "page": 1, "id": "pr", "prim_id": "box"}])",
     R"({"op": "camera_from_ruler", "page": 1, "id": "nope"})", "no ruler nope",
     [](const Document& d) {
         const Json& camera = d.page(0).extra["camera"];
         QVERIFY(camera.is_object());
         for (const char* key : {"turn", "tip", "focal_mm"}) QVERIFY2(camera.contains(key) && camera[key].is_number(), key);
     }},
    {"delete_prim", R"([{"op": "delete_prim", "page": 1, "id": "box"}])", R"({"op": "delete_prim", "page": 1, "id": "nothing"})", "no 3D",
     [](const Document& d) {
         QVERIFY(prim(d, "box") == nullptr);
         QVERIFY(prim(d, "fig") != nullptr);
     }},
    {"edit_prim", R"([{"op": "edit_prim", "page": 1, "id": "box", "pos": [90, 140, 10], "rot": [0, 0.3, 0]}])",
     R"({"op": "edit_prim", "page": 1, "id": "nothing"})", "no 3D figure or box nothing",
     [](const Document& d) {
         const Json* p = prim(d, "box");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["pos"], Json::array({90.0, 140.0, 10.0}));
         QCOMPARE((*p)["rot"], Json::array({0.0, 0.3, 0.0}));
     }},
    {"import_model", R"([{"op": "import_model", "page": 1, "id": "m1", "obj": $CUBE, "size_mm": 40}])",
     R"({"op": "import_model", "page": 1, "obj": "v 0 0 0"})", "the OBJ file has no faces",
     [](const Document& d) {
         const Json* p = prim(d, "m1");
         QVERIFY(p != nullptr);
         QCOMPARE((*p)["kind"], Json("mesh"));
         QCOMPARE((*p)["size"], Json::array({40.0, 40.0, 40.0}));
         QCOMPARE((*p)["mesh"]["v"].size(), std::size_t{24});
         QCOMPARE((*p)["mesh"]["f"].size(), std::size_t{6});
     }},
    {"pose_figure", R"([{"op": "pose_figure", "page": 1, "id": "fig", "joints": {"r_arm": {"z": -1.5}}}])",
     R"({"op": "pose_figure", "page": 1, "id": "nope"})", "no figure/hand nope",
     [](const Document& d) { QCOMPARE((*prim(d, "fig"))["joints"]["r_arm"]["z"], Json(-1.5)); }},
    {"pose_mannequin", R"([{"op": "pose_mannequin", "page": 1, "id": "man", "joints": {"l_arm": {"yaw": 0.5}}}])",
     R"({"op": "pose_mannequin", "page": 1, "id": "nope"})", "no mannequin nope",
     [](const Document& d) { QCOMPARE((*prim(d, "man"))["joints"]["l_arm"]["yaw"], Json(0.5)); }},
    {"render_prims", R"([{"op": "render_prims", "page": 1, "layer_id": "paint"}])", R"({"op": "render_prims", "page": 1, "layer_id": "frozen"})",
     "the layer is locked",
     [](const Document& d) {
         const Layer* l = layer(d, "paint");
         QVERIFY(l != nullptr && l->raster_png && !l->raster_png->empty());
         const render::Image picture = render::open_image(*l->raster_png);
         QCOMPARE(picture.width(), static_cast<int>(std::nearbyint(d.page(0).spec.width_mm.value() / 25.4 * 200)));  // (raster.WORKING_DPI)
         QCOMPARE(picture.height(), static_cast<int>(std::nearbyint(d.page(0).spec.height_mm.value() / 25.4 * 200)));
     }},
    {"ruler_from_3d", R"([{"op": "ruler_from_3d", "page": 1, "prim_id": "box", "id": "r2", "grid": 10}])",
     R"({"op": "ruler_from_3d", "page": 1, "prim_id": "nope"})", "no 3D nope",
     [](const Document& d) {
         const Json& rulers = d.page(0).rulers;
         QVERIFY(std::any_of(rulers.begin(), rulers.end(), [](const Json& r) {
             return r.value("id", std::string()) == "r2" && r.value("kind", std::string()) == "perspective" && r.value("grid", 0) == 10;
         }));
     }},
    {"set_camera", R"([{"op": "set_camera", "page": 1, "turn": 0.4, "tip": 0.15, "focal_mm": 300}])",
     R"({"op": "set_camera", "page": 1, "focal_mm": 5})", "focal_mm must be between 20 and 5000",
     [](const Document& d) {
         const Json& camera = d.page(0).extra["camera"];
         QCOMPARE(camera["turn"], Json(0.4));
         QCOMPARE(camera["tip"], Json(0.15));
         QCOMPARE(camera["focal_mm"], Json(300.0));
         QCOMPARE(camera["target"].size(), std::size_t{2});  // (the page's middle)
     }},
    {"set_light", R"([{"op": "set_light", "page": 1, "dir": [0.2, -1, -0.5], "ambient": 0.3}])",
     R"({"op": "set_light", "page": 1, "dir": [0, 0, 0]})", "dir must point somewhere",
     [](const Document& d) {
         const Json& light = d.page(0).extra["light"];
         QCOMPARE(light["dir"], Json::array({0.2, -1.0, -0.5}));
         QCOMPARE(light["ambient"], Json(0.3));
     }},
    {"trace_prims", R"([{"op": "trace_prims", "page": 1, "layer_id": "pen", "ids": ["box"], "kind": "gpen"}])",
     R"({"op": "trace_prims", "page": 1, "layer_id": "frozen"})", "the layer is locked",
     [](const Document& d) {
         const Layer* l = layer(d, "pen");
         QVERIFY(l != nullptr && l->stroke_count() >= 1);  // (the box's edges, joined into lines)
         std::size_t points = 0;
         for (const auto& s : l->strokes->items) {
             QCOMPARE(s->kind, std::string("gpen"));
             points += s->points.size();
         }
         QVERIFY(points >= 7);  // (the corners of the box seen)
     }},
};

std::string good_of(const OpCase& c) { return genko::test::replace_all(c.good, "$CUBE", kCube); }

std::string error_of(const Document& doc, const Json& batch, const std::string& actor = "genko") {
    try {
        (void)bus().apply(doc, batch, Actor(actor));
    } catch (const ApplyError& error) {
        return error.what();
    }
    return "(applied)";
}

fs::path to_path(const QString& path) { return genko::storage::path_from_utf8(path.toStdString()); }

// A v4 book on disk holding `doc` (its first revision).
fs::path make_book(const fs::path& dir, const Document& doc) {
    genko::storage::ProjectLock lock(dir);
    lock.try_acquire();
    genko::storage::SaveRequest request;
    request.ops = Json::array();
    genko::storage::Saver(lock).save(doc, request);
    return dir;
}

// `genko apply`: read under the lock, apply, save as the next revision with the ops in the journal.
void edit(const fs::path& dir, const Json& batch, const std::string& actor = "genko") {
    genko::storage::ProjectLock lock(dir, actor);
    lock.try_acquire();
    genko::storage::journal::repair(dir);
    const auto loaded = genko::storage::load_document(dir);
    const auto result = bus().apply(loaded.document, batch, Actor(actor));
    genko::storage::SaveRequest request;
    request.actor = actor;
    request.base_revision = loaded.document.revision;
    request.ops = result.journal_ops;
    genko::storage::Saver(lock).save(result.doc, request);
}

Document read(const fs::path& dir) { return genko::storage::load_document(dir).document; }

}  // namespace

class TestOps3d : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    Document base_;
    int books_ = 0;

    fs::path book_path() { return to_path(tmp_.path()) / ("book" + std::to_string(++books_) + ".genko"); }

    static void rows() {
        QTest::addColumn<int>("n");
        for (int n = 0; n < static_cast<int>(std::size(kOps)); ++n) QTest::newRow(kOps[n].op) << n;
    }

    void same(const Json& got, const Json& want, const char* what) {
        std::string where;
        QVERIFY2(genko::test::strict_equal(got, want, &where), (std::string(what) + ": " + where).c_str());
    }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        base_ = base();
        QCOMPARE(base_.page(0).prims.size(), std::size_t{5});
        QCOMPARE(base_.page(0).rulers.size(), std::size_t{1});
    }

    void succeeds_data() { rows(); }
    void succeeds() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        const auto result = bus().apply(base_, ops(good_of(c)), Actor("genko"));
        QCOMPARE(result.applied, Json::array({c.op}));
        c.check(result.doc);
        if (QTest::currentTestFailed()) return;
        QVERIFY(held(result.doc) != held(base_));
        // pages 2 and 3 are not copied (copy-on-write)
        QVERIFY(result.doc.pages[1].get() == base_.pages[1].get());
        QVERIFY(result.doc.pages[2].get() == base_.pages[2].get());
    }

    void refusalLeavesTheBook_data() { rows(); }
    void refusalLeavesTheBook() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        const Json before = held(base_);
        Json batch = ops(good_of(c));
        const std::size_t at = batch.size();
        batch.push_back(ops(c.bad));
        const std::string error = error_of(base_, batch);
        const std::string want = "ops[" + std::to_string(at) + "] " + c.op + ": " + c.error;
        QVERIFY2(error.starts_with(want), (error + " (wanted " + want + ")").c_str());
        same(held(base_), before, "the book");
    }

    void lockedPage_data() { rows(); }
    void lockedPage() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        Document locked = base_;
        locked.page_locks[locked.page(0).id] = "ai:other";
        const std::string error = error_of(locked, ops(good_of(c)), "ai:hermes");
        QVERIFY2(error.starts_with("ops[0] " + std::string(c.op) + ": page 1 locked by ai:other ‖ " + c.op + " takes {"), error.c_str());
        QCOMPARE(error_of(locked, ops(good_of(c)), "ai:other"), std::string("(applied)"));  // (the holder edits it)
        locked.page_locks.clear();
        locked.page_locks[locked.page(1).id] = "ai:other";  // (another page locked: no matter)
        QCOMPARE(error_of(locked, ops(good_of(c)), "ai:hermes"), std::string("(applied)"));
    }

    void strictGates_data() { rows(); }
    void strictGates() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        Document strict = base_;
        strict.strict_gates = true;
        const std::string op = c.op;
        if (op != "render_prims" && op != "trace_prims") {
            QCOMPARE(error_of(strict, ops(good_of(c)), "ai:hermes"), std::string("(applied)"));  // (no gate on placing 3D)
            return;
        }
        // drawing on a printed layer waits for the page's name to be approved, whoever draws
        for (const char* actor : {"ai:hermes", "human:作者"}) {
            const std::string printed = error_of(strict, ops(good_of(c)), actor);
            QVERIFY2(printed.starts_with("ops[0] " + op + ": " + op + " on a printed layer needs name_ok on page 1 (strict_gates)"),
                     printed.c_str());
            const std::string ink = error_of(strict, ops(R"([{"op": ")" + op + R"(", "page": 1, "layer": "ink"}])"), actor);
            QVERIFY2(ink.starts_with("ops[0] " + op + ": " + op + " on ink needs name_ok on page 1 (strict_gates)"), ink.c_str());
        }
        // the name is not printed
        QCOMPARE(error_of(strict, ops(R"([{"op": ")" + op + R"(", "page": 1, "layer": "name"}])"), "ai:hermes"), std::string("(applied)"));
        strict.edit_page(0).name_ok = true;
        QCOMPARE(error_of(strict, ops(good_of(c)), "ai:hermes"), std::string("(applied)"));
    }

    // A dry run answers with the book as the ops would leave it (Python's apply_ops snapshots its working copy) and
    // changes nothing: neither the book given nor the book on disk.
    void dryRun_data() { rows(); }
    void dryRun() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        const Json before = held(base_);
        const auto result = bus().apply(base_, ops(good_of(c)), Actor("genko"), true);
        QCOMPARE(result.applied, Json::array({c.op}));
        c.check(result.doc);
        if (QTest::currentTestFailed()) return;
        same(held(base_), before, "the book after a dry run");
        const fs::path dir = make_book(book_path(), base_);
        const auto files = genko::test::tree_hashes(QString::fromStdString(genko::storage::path_to_utf8(dir)), false);
        {
            genko::storage::ProjectLock lock(dir, "genko");
            lock.try_acquire();
            (void)bus().apply(genko::storage::load_document(dir).document, ops(good_of(c)), Actor("genko"), true);
        }
        QVERIFY(genko::test::tree_hashes(QString::fromStdString(genko::storage::path_to_utf8(dir)), false) == files);
    }

    void undoAndRedo_data() { rows(); }
    void undoAndRedo() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        const fs::path dir = make_book(book_path(), base_);
        const Json before = held(read(dir));
        edit(dir, ops(good_of(c)));
        const Document edited = read(dir);
        c.check(edited);
        if (QTest::currentTestFailed()) return;
        const Json after = held(edited);
        QVERIFY(after != before);
        // (Undo and Redo bring back the canonical state the journal keeps: its objects' keys sorted)
        std::string where;
        genko::storage::restore(dir, "genko", false, false);
        QVERIFY2(genko::test::same_content(held(read(dir)), before, &where), ("undone: " + where).c_str());
        genko::storage::restore(dir, "genko", true, false);
        QVERIFY2(genko::test::same_content(held(read(dir)), after, &where), ("redone: " + where).c_str());
        QCOMPARE(genko::test::book_problems(QString::fromStdString(genko::storage::path_to_utf8(dir))), std::string());
    }

    void savedAndReadBack_data() { rows(); }
    void savedAndReadBack() {
        QFETCH(int, n);
        const OpCase& c = kOps[n];
        const Document applied = bus().apply(base_, ops(good_of(c)), Actor("genko")).doc;
        const fs::path dir = make_book(book_path(), applied);
        const Document back = read(dir);
        // (the page size is read back as floats, as Python reads it: the pages and the 3D are compared)
        same(genko::storage::snapshot(back, true)["pages"], genko::storage::snapshot(applied, true)["pages"], "pages read back");
        same(genko::test::state_of(back), genko::test::state_of(applied), "3D read back");
        for (std::size_t p = 0; p < applied.pages.size(); ++p) {
            same(back.page(p).prims, applied.page(p).prims, "prims");  // (the numbers as they were: 0.3, 1e-05, …)
            same(back.page(p).rulers, applied.page(p).rulers, "rulers");
            same(back.page(p).extra, applied.page(p).extra, "extra");
        }
    }

    // What Python would keep and the C++ build refuses (docs/cpp-migration/SPEC.md §2; the same inputs through
    // Python: test_contract_3d_ops safeSideRefusals).
    void safeSideRefusals() {
        const double inf = std::numeric_limits<double>::infinity();
        const auto refused = [&](Json op, const std::string& reason) {
            const std::string name = op["op"].get<std::string>();
            const std::string error = error_of(base_, Json::array({std::move(op)}));
            QVERIFY2(error.starts_with("ops[0] " + name + ": " + reason), error.c_str());
        };
        refused(Json::object({{"op", "add_figure"}, {"page", 1}, {"pos", Json::array({inf, 0, 0})}}), "pos is [x, y, z]");
        refused(Json::object({{"op", "add_head"}, {"page", 1}, {"size_mm", inf}}), "size_mm must be a finite number");
        refused(Json::object({{"op", "set_camera"}, {"page", 1}, {"turn", inf}}), "turn must be a finite number");
        refused(Json::object({{"op", "set_light"}, {"page", 1}, {"dir", Json::array({inf, 0, 0})}}), "dir is [x, y, z]");
        refused(Json::object({{"op", "edit_prim"}, {"page", 1}, {"id", "box"}, {"pos", Json::array({inf, 1, 1})}}), "pos is [x, y, z]");
        refused(Json::object({{"op", "trace_prims"}, {"page", 1}, {"layer_id", "pen"}, {"width_mm", inf}}), "width_mm must be a finite number");
        refused(Json::object({{"op", "render_prims"}, {"page", 1}, {"layer_id", "paint"}, {"tone", Json::object({{"angle", inf}})}}),
                "screen angle must be a finite number");
        refused(Json::object({{"op", "add_mannequin"}, {"page", 1}, {"height_mm", -50}}), "a mannequin's height must be above 0");
        refused(Json::object({{"op", "add_mannequin"}, {"page", 1}, {"pos", Json::array({"a", "b"})}}), "pos is [x, y, z]");
        refused(Json::object({{"op", "pose_mannequin"}, {"page", 1}, {"id", "man"}, {"joints", ops(R"({"l_arm": {"yaw": "x"}})")}}),
                "each joint is {yaw, pitch} in radians");
        refused(Json::object({{"op", "pose_mannequin"}, {"page", 1}, {"id", "man"},
                              {"drag", Json::object({{"handle", "pelvis"}, {"to", Json::array({inf, 0})}})}}),
                "to is [x, y]");
        refused(ops(R"({"op": "ruler_from_3d", "page": 1, "prim_id": "box", "grid": 100})"), "grid is 0 (none) to 60 lines");
        refused(ops(R"({"op": "ruler_from_3d", "page": 1, "prim_id": "box", "grid": -1})"), "grid is 0 (none) to 60 lines");
        const Json loop = ops(R"({"scenes": [{"nodes": [0]}], "nodes": [{"children": [1]}, {"children": [0]}]})");
        refused(Json::object({{"op", "import_model"}, {"page", 1}, {"gltf", genko::core::dump_python(loop)}}), "the model's nodes contain themselves");
        std::string huge;
        for (int i = 0; i <= genko::core::mesh3d::kMaxVertices; ++i) huge += "v " + std::to_string(i % 1000) + " " + std::to_string(i / 1000) + " 0\n";
        huge += "f 1 2 3\n";
        refused(Json::object({{"op", "import_model"}, {"page", 1}, {"obj", huge}}), "the model has too many corners (at most 240000)");
        refused(Json::object({{"op", "import_model"}, {"page", 1}, {"obj", "v 1e999 0 0\nv 0 1 0\nv 0 0 1\nf 1 2 3"}}),
                "the model has a corner that is not a finite number");
        // infinities the op only works with (not kept) are taken as Python takes them
        QCOMPARE(error_of(base_, Json::array({Json::object({{"op", "render_prims"}, {"page", 1}, {"layer_id", "paint"},
                                                             {"light", Json::array({inf, 0, 0})}})})),
                 std::string("(applied)"));
        QCOMPARE(error_of(base_, Json::array({Json::object({{"op", "pose_figure"}, {"page", 1}, {"id", "fig"},
                                                             {"drag", Json::object({{"handle", "r_hand"}, {"to", Json::array({inf, 0})}})}})})),
                 std::string("(applied)"));
    }

    // What is too large to draw (the C++ build only: Python would draw a street's floor lines for as long as it takes,
    // or run out of memory for the depth picture of a 3D kilometres wide).
    void sizeLimits() {
        Document doc = base_;
        doc.edit_page(0).numero = false;  // (nombres are not drawn yet in this build)
        const Document tall =
            bus().apply(doc, ops(R"([{"op": "add_scene", "page": 1, "id": "tall", "kind": "street", "size": [150, 1000000, 200]}])"),
                        Actor("genko"))
                .doc;
        const std::string traced = error_of(tall, ops(R"([{"op": "trace_prims", "page": 1, "layer_id": "pen", "ids": ["tall"]}])"));
        QVERIFY2(traced.starts_with("ops[0] trace_prims: a value of the wrong type (the scene is too large to draw)"), traced.c_str());
        render::RenderOptions options;
        options.mode = "proof";
        try {
            (void)render::render_page(tall.page(0), 72, options, &tall);
            QFAIL("a street of 55555 floors was drawn");
        } catch (const genko::core::Error& error) {
            QCOMPARE(error.code(), std::string("image_too_large"));
        }
        // the same street at a size anyone draws
        const Document street =
            bus().apply(doc, ops(R"([{"op": "add_scene", "page": 1, "id": "s", "kind": "street", "size": [150, 80, 200]}])"), Actor("genko")).doc;
        QCOMPARE(error_of(street, ops(R"([{"op": "trace_prims", "page": 1, "layer_id": "pen", "ids": ["s"]}])")), std::string("(applied)"));
    }

    // The pose library (the same steps against Python: test_contract_3d_geometry posesFile).
    void poseLibrary() {
        QTemporaryDir config;
        QVERIFY(config.isValid());
        const QByteArray before = qgetenv("GENKO_CONFIG_DIR");
        qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
        namespace poses = genko::core::poses;
        QCOMPARE(poses::user_poses(), Json::array());  // (no file yet)
        const Json figure = ops(R"({"kind": "figure", "joints": {"l_arm": {"z": 0.5}}, "hands": {"l": "fist"}, "size": [45, 90, 22.5]})");
        const Json kept = poses::save_pose("　立ち ", figure);
        QCOMPARE(kept, ops(R"({"name": "立ち", "joints": {"l_arm": {"z": 0.5}}, "hands": {"l": "fist"}})"));
        QVERIFY(poses::find("立ち").has_value());
        (void)poses::save_pose("立ち", ops(R"({"joints": {"head": {"y": 1}}})"));  // (replaced, not added)
        QCOMPARE(poses::user_poses().size(), std::size_t{1});
        QCOMPARE((*poses::find("立ち"))["joints"], ops(R"({"head": {"y": 1}})"));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, poses::save_pose("  ", figure));
        poses::delete_pose("立ち");
        QVERIFY(!poses::find("立ち").has_value());
        genko::test::write_bytes(config.path() + "/poses.json", "not json");
        QCOMPARE(poses::user_poses(), Json::array());  // (a file that is not JSON: none)
        QCOMPARE(QString::fromStdString(genko::storage::path_to_utf8(poses::config_dir())), config.path());
        if (before.isNull()) {
            qunsetenv("GENKO_CONFIG_DIR");
        } else {
            qputenv("GENKO_CONFIG_DIR", before);
        }
    }

    // 3D guides are drawn in the proof and the name, never printed; a region drawn alone is the whole page cut.
    void drawnWhereGuidesAre() {
        Document guides = base_;
        guides.edit_page(0).numero = false;  // (nombres are not drawn yet in this build)
        Document bare = guides;
        bare.edit_page(0).prims = Json::array();
        const auto draw = [](const Document& doc, const std::string& mode, std::optional<render::RenderRegion> region = std::nullopt) {
            render::clear_render_caches();
            render::RenderOptions options;
            options.mode = mode;
            options.region = region;
            return render::render_page(doc.page(0), 72, options, &doc).image;
        };
        QVERIFY(draw(guides, "print").tobytes() == draw(bare, "print").tobytes());
        for (const char* mode : {"proof", "name"}) {
            const render::Image whole = draw(guides, mode);
            QVERIFY2(whole.tobytes() != draw(bare, mode).tobytes(), mode);
            for (const render::RenderRegion r : {render::RenderRegion{100, 200, 150, 120}, render::RenderRegion{0, 0, 37, 400},
                                                 render::RenderRegion{whole.width() - 60, whole.height() - 50, 60, 50}}) {
                const render::Image cut = whole.crop(render::Box{r.x, r.y, r.x + r.w, r.y + r.h});
                QVERIFY2(draw(guides, mode, r).tobytes() == cut.tobytes(), mode);
            }
        }
    }

    // A few of the numbers the geometry rests on (all of it against Python: test_contract_3d_geometry).
    void geometryBasics() {
        namespace la = genko::core::la;
        QCOMPARE(la::py_remainder(5.5, 2.0), -0.5);  // (math.remainder: to the nearest, halves to even)
        QCOMPARE(la::py_remainder(7.0, 2.0), -1.0);
        QCOMPARE(la::py_remainder(-7.0, 2.0), 1.0);
        QCOMPARE(la::py_hypot3(3.0, 4.0, 12.0), 13.0);
        // a page's surfaces drawn in a window are the whole picture's pixels there
        const std::vector<Json> prims(base_.page(0).prims.begin(), base_.page(0).prims.end());
        const auto whole = genko::core::mesh3d::raster(prims, 300, 400, 40.0, nullptr, nullptr, 0.35);
        QVERIFY(whole.any());
        const genko::core::mesh3d::Window window{50, 80, 210, 300};
        const auto part = genko::core::mesh3d::raster(prims, 300, 400, 40.0, nullptr, nullptr, 0.35, 0.0, 0.0, &window);
        QCOMPARE(part.x0, 50);
        QCOMPARE(part.y0, 80);
        QCOMPARE(part.width, 160);
        QCOMPARE(part.height, 220);
        int compared = 0;
        for (int y = 0; y < part.height; ++y) {
            for (int x = 0; x < part.width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * part.width + x;
                const std::size_t j = static_cast<std::size_t>(y + 80) * 300 + (x + 50);
                QCOMPARE(part.alpha(i), whole.alpha(j));
                if (!whole.alpha(j)) continue;
                QCOMPARE(std::bit_cast<std::uint32_t>(part.zbuf[i]), std::bit_cast<std::uint32_t>(whole.zbuf[j]));
                QCOMPARE(std::bit_cast<std::uint32_t>(part.shade[i]), std::bit_cast<std::uint32_t>(whole.shade[j]));
                ++compared;
            }
        }
        QVERIFY(compared > 1000);
    }
};

QTEST_GUILESS_MAIN(TestOps3d)
#include "test_ops_3d.moc"
