// The ops of M2-O1 end to end through the command line, each step its own process. A book made by `genko new` gets
// every op through `genko apply`, batch by batch (by a person and by an AI; for_pages too); after each batch
// `genko inspect` shows the book the reply showed. Batches that may not be applied (an AI approving, a page locked by
// a person, an op this build does not have yet, a stale --expect-revision, a dry run) leave the book's files as they
// were. Then `genko undo` takes the
// batches back one by one (the book each time as it was before that batch), `genko redo` puts them back, and
// [{"op": "undo"}] given to `genko apply` undoes as `genko undo` does, once for a --txn given twice.

#include <QtTest>

#include <QTemporaryDir>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "testsupport.hpp"

using genko::core::Json;

namespace {

Json one_line(const QByteArray& out) { return genko::test::one_line(out); }

}  // namespace

class TestOpsE2e : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString book_;
    int files_ = 0;

    // `genko apply` of `ops` (JSON text) as `agent`, with more arguments.
    genko::test::Run apply(const std::string& ops, const QString& agent, const QStringList& more = {}) {
        const QString file = tmp_.path() + QStringLiteral("/ops-%1.json").arg(++files_);
        genko::test::write_bytes(file, ops);
        return genko::test::run_genko(QStringList{"apply", book_, file, "--agent", agent} + more);
    }

    Json inspect(bool full = false) {
        QStringList args{"inspect", book_};
        if (full) args << "--full";
        const auto r = genko::test::run_genko(args);
        return r.exit_code == 0 ? one_line(r.out) : Json("inspect failed: " + (r.out + r.err).toStdString());
    }

    // The files of the book but its lock file (taken and given back by every command).
    std::map<std::string, std::string> files() const { return genko::test::tree_hashes(book_, false); }

    static std::string id_of(const Json& items, std::size_t n) { return items.at(n).at("id").get<std::string>(); }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        book_ = tmp_.path() + "/ops.genko";
        const auto made = genko::test::run_genko({"new", book_, "--pages", "4", "--title", "通し"});
        QVERIFY2(made.exit_code == 0, (made.out + made.err).constData());
    }

    void everyOpThroughTheCommandLine() {
        const QTest::ThrowOnFailEnabler throw_on_fail;  // (a failed check in a lambda stops the test)
        struct Done {
            Json before;    // inspect --full before the batch
            Json after;     // and after it
            QString agent;  // who applied it
            std::string what;
        };
        std::vector<Done> done;
        std::vector<std::string> ops_seen;
        std::int64_t revision = one_line(genko::test::run_genko({"inspect", book_}).out).is_object() ? 1 : -1;
        QCOMPARE(revision, std::int64_t{1});

        // apply a batch that must succeed; the reply's snapshot is what another process then reads (a failure stops
        // the test: what follows needs the batch)
        const auto stop = [](const std::string& why, int line) {
            QTest::qFail(why.c_str(), __FILE__, line);
            QTest::Internal::maybeThrowOnFail();
            throw std::runtime_error(why);
        };
        const auto step = [&](const std::string& what, const Json& ops, const QString& agent) -> Json {
            const Json before = inspect(true);
            const auto r = apply(genko::core::dump_python(ops), agent);
            const Json reply = one_line(r.out);
            if (r.exit_code != 0 || !reply.is_object() || reply.value("ok", false) != true) {
                stop(what + ": " + (r.out + r.err).toStdString(), __LINE__);
            }
            ++revision;
            if (reply["revision"] != Json(revision)) stop(what + ": revision " + reply["revision"].dump(), __LINE__);
            // (which frame is selected is not saved, as Python's writer does not save it: the reply shows it, a book
            // read again has none)
            Json shown = reply["snapshot"];
            for (Json& page : shown["pages"]) page["selected_frame_id"] = nullptr;
            std::string where;
            if (!genko::test::strict_equal(inspect(), shown, &where)) stop(what + ": inspect shows another book: " + where, __LINE__);
            for (const Json& op : ops) ops_seen.push_back(op["op"].get<std::string>());
            done.push_back({before, inspect(true), agent, what});
            return reply;
        };
        // a batch that must be refused, with nothing written
        const auto refused = [&](const std::string& what, const Json& ops, const QString& agent, const std::string& code,
                                 const std::string& words, const QStringList& more = {}) {
            const auto before = files();
            const auto r = apply(genko::core::dump_python(ops), agent, more);
            const Json reply = one_line(r.out);
            QVERIFY2(r.exit_code == 1, qPrintable(QString::fromStdString(what) + ": " + r.out + r.err));
            QCOMPARE(reply["code"], Json(code));
            QVERIFY2(reply["error"].get<std::string>().find(words) != std::string::npos, qPrintable(QString::fromStdString(what) + ": " + r.out));
            QVERIFY2(files() == before, (what + ": the book changed").c_str());
        };

        const QString person = QStringLiteral("human:作者"), ai = QStringLiteral("ai:hermes");
        Json snap = inspect();
        const std::string root1 = id_of(snap["pages"][0]["leaves"], 0);
        const std::string leaf2 = id_of(snap["pages"][1]["leaves"], 0);

        // frames
        snap = step("split", Json::array({Json::object({{"op", "split_frame"}, {"page", 1}, {"axis", "horizontal"}, {"ratio", 0.4},
                                                         {"gutter_mm", 6}})}),
                    person)["snapshot"];
        QCOMPARE(snap["pages"][0]["leaf_count"], Json(2));
        const std::string top = id_of(snap["pages"][0]["leaves"], 0), bottom = id_of(snap["pages"][0]["leaves"], 1);
        snap = step("frames", Json::parse(R"([
            {"op": "split_frame", "page": 1, "frame_id": ")" + top + R"(", "axis": "vertical", "tilt_mm": 4},
            {"op": "cut_frame", "page": 1, "frame_id": ")" + bottom + R"(", "p0": [0, 200], "p1": [210, 180], "gutter_mm": 3},
            {"op": "move_gutter", "page": 1, "frame_id": ")" + root1 + R"(", "delta_mm": 5},
            {"op": "set_frame", "page": 1, "frame_id": ")" + bottom + R"(", "border_mm": 1.2, "bleed": true},
            {"op": "resize_frame", "page": 2, "frame_id": ")" + leaf2 + R"(", "rect": {"x": 20, "y": 20, "width": 150, "height": 200}},
            {"op": "add_frame", "page": 3, "rect": [20, 20, 80, 60]},
            {"op": "add_frame", "page": 3, "points": [[110, 20], [190, 20], [190, 90], [110, 90]]}
        ])"),
                    person)["snapshot"];
        QCOMPARE(snap["pages"][0]["leaf_count"], Json(4));
        QCOMPARE(snap["pages"][2]["leaf_count"], Json(2));
        const std::string drawn = id_of(snap["pages"][2]["leaves"], 1);
        const std::string top_half = id_of(snap["pages"][0]["leaves"], 0);  // (one of the two the top panel became)
        snap = step("merge, delete and select", Json::parse(R"([
            {"op": "merge_frame", "page": 1, "frame_id": ")" + top_half + R"("},
            {"op": "delete_frame", "page": 3, "frame_id": ")" + drawn + R"("},
            {"op": "select_frame", "page": 1, "frame_id": ")" + top + R"("}
        ])"),
                    person)["snapshot"];
        QCOMPARE(snap["pages"][0]["leaf_count"], Json(3));
        QCOMPARE(snap["pages"][2]["leaf_count"], Json(1));

        // pages
        snap = step("pages", Json::parse(R"([
            {"op": "add_page", "count": 2},
            {"op": "duplicate_page", "page": 2, "next_to": true},
            {"op": "reorder", "order": [1, 2, 3, 5, 4, 6, 7]},
            {"op": "delete_page", "page": 7},
            {"op": "set_note", "page": 1, "note": "一行目\n二行目"},
            {"op": "set_meta", "title": "新しい題", "episode": 2, "binding": "right"},
            {"op": "set_autosave", "enabled": true}
        ])"),
                    person)["snapshot"];
        QCOMPARE(snap["pages"].size(), std::size_t{6});
        QCOMPARE(snap["title"], Json("新しい題"));
        QCOMPARE(snap["autosave"], Json(true));

        // strokes
        Json reply = step("strokes", Json::parse(R"([
            {"op": "add_stroke", "page": 1, "layer": "name", "points": [[10, 10], [30, 31], [50, 49], [70, 72], [90, 90]]},
            {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[20, 100], [180, 100]], "width_mm": 0.8},
            {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[20, 150], [180, 150]], "kind": "maru", "rgb": [10, 20, 30]},
            {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[20, 200], [180, 200]]},
            {"op": "edit_stroke", "page": 1, "layer": "name", "index": 0, "points": [[12, 12], [32, 30], [52, 50], [72, 70], [92, 92]]},
            {"op": "simplify_stroke", "page": 1, "layer": "name", "index": 0, "epsilon_mm": 2},
            {"op": "erase", "page": 1, "layer": "ink", "points": [[100, 90], [100, 110]], "width_mm": 2},
            {"op": "erase_raster", "page": 1, "points": [[60, 140], [60, 160]], "mode": "whole"},
            {"op": "delete_stroke", "page": 1, "layer": "ink", "index": 0}
        ])"),
                     person);
        snap = reply["snapshot"];
        QCOMPARE(snap["pages"][0]["name_stroke_count"], Json(1));
        QCOMPARE(snap["pages"][0]["ink_stroke_count"], Json(2));  // (4 - 1 deleted - 1 erased whole + 1 cut in two - 1)

        // layers
        const Json layers2 = snap["pages"][1]["layers"];
        snap = step("layers", Json::parse(R"([
            {"op": "add_layer", "page": 2, "name": "ペン", "kind": "pen", "id": "e2e-pen"},
            {"op": "add_layer", "page": 2, "name": "束", "kind": "folder", "id": "e2e-folder"},
            {"op": "duplicate_layer", "page": 2, "id": ")" + id_of(layers2, 2) + R"(", "new_id": "e2e-copy"},
            {"op": "set_layer", "page": 2, "layer": "ink", "opacity": 0.5, "blend": "multiply"},
            {"op": "set_layer", "page": 2, "layer": "user", "id": "e2e-pen", "parent": "e2e-folder", "opacity": 0.8},
            {"op": "set_layer", "page": 2, "layer": "user", "id": "e2e-folder", "locked": true},
            {"op": "set_layers", "page": 2, "ids": ["e2e-copy", ")" + id_of(layers2, 0) + R"("], "visible": false}
        ])"),
                    person)["snapshot"];
        QCOMPARE(snap["pages"][1]["layers"].size(), std::size_t{7});
        Json order = Json::array();  // (reorder_layers takes every layer of the page, each once)
        for (const Json& layer : snap["pages"][1]["layers"]) order.insert(order.begin(), layer["id"]);
        snap = step("layer order", Json::array({Json::object({{"op", "reorder_layers"}, {"page", 2}, {"order", order}}),
                                                Json::object({{"op", "delete_layer"}, {"page", 2}, {"id", "e2e-copy"}}),
                                                Json::parse(R"({"op": "add_stroke", "page": 2, "layer_id": "e2e-pen",
                                                                "points": [[30, 30], [60, 80]]})")}),
                    person)["snapshot"];
        QCOMPARE(snap["pages"][1]["layers"].size(), std::size_t{6});
        QCOMPARE(snap["pages"][1]["layers"][0]["id"], Json("e2e-folder"));

        // the brush, approvals and stages, page locks
        step("brush", Json::parse(R"([{"op": "set_brush", "rgb": [200, 10, 10], "width_mm": 0.6, "stabilize": 4, "taper": false,
                                       "curve": "linear"}])"),
             ai);
        refused("an AI approving", Json::parse(R"([{"op": "name_ok", "page": 1}])"), ai, "apply", "name_ok");
        step("approve", Json::parse(R"([{"op": "name_ok", "page": 1}, {"op": "advance", "page": 1, "to": "ink"}])"), person);
        step("lock", Json::parse(R"([{"op": "lock_page", "page": 3, "agent": "human:作者"}])"), person);
        refused("a locked page", Json::parse(R"([{"op": "set_note", "page": 3, "note": "x"}])"), ai, "apply", "locked by human:作者");
        step("newly ported tone in a batch", Json::parse(R"([{"op": "set_note", "page": 1, "note": "x"}, {"op": "add_tone", "page": 1}])"), person);
        refused("an op this build does not have yet",
                 Json::parse(R"([{"op": "set_note", "page": 1, "note": "refused prefix"}, {"op": "add_anim_folder", "page": 1}])"),
                 person, "not_yet_ported", "ops[1] add_anim_folder: add_anim_folder is not in the C++ build yet");
        step("for_pages", Json::parse(R"([{"op": "for_pages", "pages": [1, 2, 4], "ops": [{"op": "set_note", "note": "全"}]}])"), ai);
        step("unlock", Json::parse(R"([{"op": "unlock_page", "page": 3}])"), person);
        const std::int64_t last = revision;

        // every op of M2-O1 was applied
        for (const char* op : {"split_frame", "cut_frame", "move_gutter", "merge_frame", "resize_frame", "set_frame", "add_frame",
                               "delete_frame", "select_frame", "add_page", "delete_page", "duplicate_page", "reorder", "advance",
                               "name_ok", "lock_page", "unlock_page", "set_note", "set_meta", "set_autosave", "add_stroke",
                               "delete_stroke", "edit_stroke", "simplify_stroke", "erase", "erase_raster", "add_layer",
                               "delete_layer", "duplicate_layer", "set_layer", "set_layers", "reorder_layers", "set_brush"}) {
            QVERIFY2(std::find(ops_seen.begin(), ops_seen.end(), op) != ops_seen.end(), op);
        }

        // a dry run and a stale --expect-revision write nothing
        const auto before_dry = files();
        const auto dry = apply(R"([{"op": "set_note", "page": 2, "note": "試し"}])", person, {"--dry-run"});
        QCOMPARE(dry.exit_code, 0);
        QCOMPARE(one_line(dry.out)["snapshot"]["pages"][1]["note"], Json("試し"));
        QCOMPARE(one_line(dry.out)["revision"], Json(last));
        QVERIFY(files() == before_dry);
        refused("a stale revision", Json::parse(R"([{"op": "set_note", "page": 2, "note": "古い"}])"), person, "revision_conflict",
                "revision conflict: expected " + std::to_string(last - 1) + ", found " + std::to_string(last),
                {"--expect-revision", QString::number(last - 1)});
        QCOMPARE(apply(R"([{"op": "set_note", "page": 2, "note": "今"}])", person, {"--expect-revision", QString::number(last)}).exit_code, 0);
        QCOMPARE(apply(R"([{"op": "undo"}])", person).exit_code, 0);  // (and taken back, as an op)

        // Undo takes the batches back one by one, Redo puts them back
        for (std::size_t n = done.size(); n-- > 0;) {
            const auto r = genko::test::run_genko({"undo", book_, "--as", done[n].agent});
            QVERIFY2(r.exit_code == 0, qPrintable(QString::fromStdString(done[n].what) + ": " + r.out + r.err));
            std::string where;
            QVERIFY2(genko::test::strict_equal(inspect(true), done[n].before, &where), (done[n].what + ": " + where).c_str());
        }
        for (std::size_t n = 0; n < done.size(); ++n) {
            const auto r = genko::test::run_genko({"redo", book_, "--as", done[n].agent});
            QVERIFY2(r.exit_code == 0, qPrintable(QString::fromStdString(done[n].what) + ": " + r.out + r.err));
            std::string where;
            QVERIFY2(genko::test::strict_equal(inspect(true), done[n].after, &where), (done[n].what + ": " + where).c_str());
        }

        // [{"op": "undo"}] through `genko apply`, given a --txn twice: undone once
        const QString txn = QStringLiteral("0123456789abcdef0123456789abcdef");
        const auto undone = apply(R"([{"op": "undo"}])", person, {"--txn", txn});
        QCOMPARE(undone.exit_code, 0);
        const Json first = one_line(undone.out);
        QCOMPARE(first["kind"], Json("undo"));
        QCOMPARE(first["txn"], Json(txn.toStdString()));
        std::string where;
        QVERIFY2(genko::test::strict_equal(inspect(true), done.back().before, &where), where.c_str());
        const auto unchanged = files();
        const auto again = apply(R"([{"op": "undo"}])", person, {"--txn", txn});
        QCOMPARE(again.exit_code, 0);
        QCOMPARE(one_line(again.out)["already_committed"], Json(true));
        QCOMPARE(one_line(again.out)["revision"], first["revision"]);
        QVERIFY(files() == unchanged);
        QVERIFY2(genko::test::book_problems(book_).empty(), genko::test::book_problems(book_).c_str());
    }

    void onionOperationsPersistUndoAndRefuseAtomically() {
        book_ = tmp_.path() + "/onion.genko";
        QCOMPARE(genko::test::run_genko({"new", book_, "--pages", "3"}).exit_code, 0);
        const auto onion = [&]() {
            return genko::test::read_json(book_ + "/project.json")["pages"][1]["onion_from"];
        };
        QCOMPARE(onion(), Json(nullptr));
        QCOMPARE(apply(R"([{"op":"set_onion","page":2,"from":1}])", "human:作者").exit_code, 0);
        QCOMPARE(onion(), Json(1));
        QCOMPARE(apply(R"([{"op":"step_onion","page":2,"delta":100}])", "human:作者").exit_code, 0);
        QCOMPARE(onion(), Json(3));
        QCOMPARE(genko::test::run_genko({"undo", book_, "--as", "human:作者"}).exit_code, 0);
        QCOMPARE(onion(), Json(1));
        QCOMPARE(genko::test::run_genko({"undo", book_, "--as", "human:作者"}).exit_code, 0);
        QCOMPARE(onion(), Json(nullptr));
        QCOMPARE(genko::test::run_genko({"redo", book_, "--as", "human:作者"}).exit_code, 0);
        QCOMPARE(onion(), Json(1));
        QCOMPARE(genko::test::run_genko({"redo", book_, "--as", "human:作者"}).exit_code, 0);
        QCOMPARE(onion(), Json(3));
        const auto saved = files();
        for (const char* batch : {
                 R"([{"op":"set_note","page":2,"note":"前置"},{"op":"set_onion","page":2,"from":[]}])",
                 R"([{"op":"set_note","page":2,"note":"前置"},{"op":"step_onion","page":2,"delta":{"bad":1}}])",
                 R"([{"op":"set_note","page":2,"note":"前置"},{"op":"set_onion","page":9,"from":1}])"}) {
            const auto refused = apply(batch, "human:作者");
            QCOMPARE(refused.exit_code, 1);
            QCOMPARE(one_line(refused.out)["ok"], Json(false));
            QVERIFY(files() == saved);
            QCOMPARE(onion(), Json(3));
        }
        const auto dry = apply(R"([{"op":"set_onion","page":2,"from":1},{"op":"step_onion","page":2}])",
                               "ai:hermes", {"--dry-run"});
        QCOMPARE(dry.exit_code, 0);
        QVERIFY(files() == saved);
        const auto stale = apply(R"([{"op":"set_onion","page":2,"from":1}])", "human:作者",
                                 {"--expect-revision", "1"});
        QCOMPARE(stale.exit_code, 1);
        QCOMPARE(one_line(stale.out)["code"], Json("revision_conflict"));
        QVERIFY(files() == saved);
        QCOMPARE(apply(R"([{"op":"lock_page","page":2}])", "human:other").exit_code, 0);
        const auto locked = files();
        for (const char* batch : {R"([{"op":"set_onion","page":2,"from":1}])",
                                 R"([{"op":"step_onion","page":2}])"}) {
            const auto refused = apply(batch, "ai:hermes");
            QCOMPARE(refused.exit_code, 1);
            QCOMPARE(one_line(refused.out)["code"], Json("apply"));
            QVERIFY2(one_line(refused.out)["error"].get<std::string>().find("page 2 locked by human:other") != std::string::npos,
                     refused.out.constData());
            QVERIFY(files() == locked);
        }
        QVERIFY2(genko::test::book_problems(book_).empty(), genko::test::book_problems(book_).c_str());
    }

    void lightThresholdPersistsUndoAndRejectsUnsafeBatches() {
        book_ = tmp_.path() + "/light-threshold.genko";
        QCOMPARE(genko::test::run_genko({"new", book_, "--pages", "3"}).exit_code, 0);
        const auto threshold = [this]() {
            return genko::test::read_json(book_ + "/project.json")["pages"][1]["lt_threshold"];
        };
        QVERIFY(threshold().is_null());
        QCOMPARE(apply(R"([{"op":"set_lt","page":2,"threshold":"-17.25"}])", "human:作者").exit_code, 0);
        QCOMPARE(threshold(), Json(-17.25));
        QCOMPARE(genko::test::run_genko({"undo", book_, "--as", "human:作者"}).exit_code, 0);
        QVERIFY(threshold().is_null());
        QCOMPARE(genko::test::run_genko({"redo", book_, "--as", "human:作者"}).exit_code, 0);
        QCOMPARE(threshold(), Json(-17.25));
        for (const char* raw : {"NaN", "Infinity", "-Infinity", "\"nan\"", "\"inf\"", "\"-inf\""}) {
            const auto before = files();
            const QString text = QStringLiteral("[{\"op\":\"set_note\",\"page\":2,\"note\":\"前置\"},{\"op\":\"set_lt\",\"page\":2,\"threshold\":%1}]").arg(QString::fromLatin1(raw));
            const auto refused = apply(text.toStdString(), "human:作者");
            QCOMPARE(refused.exit_code, 1);
            QCOMPARE(one_line(refused.out)["code"], Json("apply"));
            const char* expected = raw[0] == '"' ? "threshold must be a finite number" : "ops must not hold NaN or Infinity (at /1/threshold)";
            QVERIFY2(one_line(refused.out)["error"].get<std::string>().find(expected) != std::string::npos, refused.out.constData());
            QVERIFY(files() == before);
        }
        const auto saved = files();
        QCOMPARE(apply(R"([{"op":"set_lt","page":2,"threshold":0.25}])", "human:作者", {"--dry-run"}).exit_code, 0);
        QVERIFY(files() == saved);
        QCOMPARE(apply(R"([{"op":"set_lt","page":2,"threshold":0.25}])", "human:作者", {"--expect-revision", "0"}).exit_code, 1);
        QVERIFY(files() == saved);
        QCOMPARE(apply(R"([{"op":"lock_page","page":2}])", "human:other").exit_code, 0);
        const auto locked = files();
        const auto refused = apply(R"([{"op":"set_lt","page":2,"threshold":0.25}])", "human:作者");
        QCOMPARE(refused.exit_code, 1);
        QVERIFY2(one_line(refused.out)["error"].get<std::string>().find("page 2 locked by human:other") != std::string::npos, refused.out.constData());
        QVERIFY(files() == locked);
        QVERIFY2(genko::test::book_problems(book_).empty(), genko::test::book_problems(book_).c_str());
    }
};

QTEST_GUILESS_MAIN(TestOpsE2e)
#include "test_ops_e2e.moc"
