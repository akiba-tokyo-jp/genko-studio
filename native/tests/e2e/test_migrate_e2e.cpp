// `genko migrate` end to end (ACCEPTANCE.md AC-DATA, schema-v4 §6): v1, v2 and v3 books converted into new v4
// books that another process reads, the old book's files byte for byte the same before and after; legacy/ (the old
// journal, audit and v2 backup as they were, map.json, the report), the assets byte for byte, v2's pages/ picture as
// an asset; Undo and Redo into the old history right after the conversion and after a new edit (an AI cannot undo an
// approval); an asset only the old history refers to; a book being written meanwhile (source_changed), held by
// another process (source_locked), needing repairs (refused, then accepted); versions and folders refused; Unicode,
// spaces and long paths; CRLF project.json; GC after a conversion.

#include <QtTest>

#include <QDir>
#include <QSettings>
#include <QTemporaryDir>

#include <filesystem>
#include <string>

#include "core/json.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/reader.hpp"
#include "storage/state.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
using genko::core::Json;
using genko::storage::AssetStore;
using genko::test::one_line;
using genko::test::run_genko;

namespace {

fs::path to_path(const QString& path) { return genko::storage::path_from_utf8(path.toStdString()); }

Json project(const QString& book) { return genko::test::read_json(book + "/project.json"); }

QStringList staging_left(const QString& parent) {
    return QDir(parent).entryList({"*.migrating-*"}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
}

}  // namespace

class TestMigrateE2e : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

    QString copy_of(const QString& version, const QString& name) {
        const QString dir = tmp_.path() + "/" + name;
        genko::test::copy_tree(genko::test::test_data("legacy/book-" + version + ".genko"), dir);
        return dir;
    }

private slots:
    void eachVersion_data() {
        QTest::addColumn<QString>("version");
        for (const char* v : {"v1", "v2", "v3"}) QTest::newRow(v) << QString(v);
    }

    void eachVersion() {
        QFETCH(QString, version);
        const QString source = copy_of(version, "each-" + version + ".genko");
        const QString book = tmp_.path() + "/each-" + version + "-v4.genko";
        const auto before = genko::test::tree_hashes(source);
        const auto r = run_genko({"migrate", source, book, "--as", "human:作者"});
        QVERIFY2(r.exit_code == 0, r.out.constData());
        const Json report = one_line(r.out);
        QVERIFY(genko::test::tree_hashes(source) == before);  // (the old book: every file as it was)
        QVERIFY(!QFileInfo::exists(source + "/project.lock"));
        QCOMPARE(staging_left(tmp_.path()), QStringList());

        const Json p = project(book);
        QCOMPARE(p["version"], Json(4));
        QCOMPARE(p["min_reader"], Json(4));
        QCOMPARE(p["revision"], Json(1));
        QCOMPARE(p["features"], Json::array());
        QCOMPARE(p["writer"]["app"], Json("genko-native"));
        QCOMPARE(p["book_id"], report["destination"]["book_id"]);
        QCOMPARE(report["source"]["version"], Json(version.mid(1).toInt()));
        QCOMPARE(report["destination"]["revision"], Json(1));
        QCOMPARE(report["issues"], Json::array());
        // the report is legacy/report.json
        std::string where;
        QVERIFY2(genko::test::strict_equal(genko::test::read_json(book + "/legacy/report.json"), report, &where), where.c_str());
        // the manifest of the fixed copy is the old book's
        const Json manifest = genko::test::read_json(book + "/legacy/source-manifest.json");
        QCOMPARE(manifest["files"].size(), before.size());
        for (const auto& [rel, sha] : before) QCOMPARE(manifest["files"][rel].get<std::string>(), sha);
        // the journal starts with the migration
        const auto lines = genko::storage::journal::read_lines(to_path(book) / "studio" / "journal.jsonl").values;
        QCOMPARE(lines.size(), std::size_t{2});
        QCOMPARE(lines[0]["action"], Json("migrate"));
        QCOMPARE(lines[0]["actor"], Json("human:作者"));
        QVERIFY(lines[0]["before"].is_null());
        QCOMPARE(genko::test::book_problems(book), std::string());
        const Json audit_last = genko::storage::journal::audit_entries(to_path(book)).back();
        QCOMPARE(audit_last["kind"], Json("migrated"));
        QCOMPARE(audit_last["from_version"], Json(version.mid(1).toInt()));
        QCOMPARE(audit_last["actor"], Json("human:作者"));
        // another process reads it, and finds nothing wrong
        QCOMPARE(run_genko({"inspect", book}).exit_code, 0);
        const auto doctor = run_genko({"doctor", book});
        QVERIFY2(doctor.exit_code == 0, doctor.out.constData());
        const auto loaded = genko::storage::load_document(to_path(book));
        QVERIFY(loaded.report.clean());

        if (version == "v2") {
            const std::string bg = genko::test::read_bytes(source + "/pages/001/bg.png");
            QCOMPARE(report["assets"]["pages"],
                     Json::array({Json::object({{"path", "pages/001/bg.png"}, {"ref", AssetStore::ref(bg)}})}));
            const auto& layers = loaded.document.page(0).layers;
            const auto bg_layer = std::find_if(layers.begin(), layers.end(), [](const auto& l) { return l.role == genko::core::LayerRole::Bg; });
            QCOMPARE(*bg_layer->raster_png, bg);
            QCOMPARE(p["pages"][0]["layers"][0]["asset"].get<std::string>(), AssetStore::ref(bg));
            QVERIFY(!QFileInfo::exists(book + "/pages"));
        }
        if (version == "v3") {
            QCOMPARE(report["assets"]["imported"], Json(6));
            for (const auto& [rel, sha] : before) {
                if (rel.starts_with("assets/")) QCOMPARE(genko::test::read_bytes(book + "/" + QString::fromStdString(rel)),
                                                         genko::test::read_bytes(source + "/" + QString::fromStdString(rel)));
            }
            QCOMPARE(genko::test::read_bytes(book + "/legacy/journal.jsonl"), genko::test::read_bytes(source + "/studio/journal.jsonl"));
            QCOMPARE(genko::test::read_bytes(book + "/legacy/audit.jsonl"), genko::test::read_bytes(source + "/studio/audit.jsonl"));
            const std::string audit = genko::test::read_bytes(book + "/studio/audit.jsonl");
            QVERIFY(audit.starts_with(genko::test::read_bytes(source + "/studio/audit.jsonl")));  // (the old approvals, as they were)
            QCOMPARE(report["map_entries"], Json(6));
            QCOMPARE(report["history"]["consistent"], Json(true));
            const Json map = genko::test::read_json(book + "/legacy/map.json");
            QCOMPARE(map["source_version"], Json(3));
            QCOMPARE(map["boundary_rev"], Json(1));
            QCOMPARE(map["entries"].size(), std::size_t{6});
            const Json old_journal = genko::core::parse_python_json(
                "[" + [&] {
                    std::string s = genko::test::read_bytes(source + "/studio/journal.jsonl");
                    for (std::size_t at = s.find('\n'); at != std::string::npos && at + 1 < s.size(); at = s.find('\n', at + 1)) s[at] = ',';
                    return s;
                }() + "]");
            for (std::size_t i = 0; i < 6; ++i) {
                const Json& entry = map["entries"][i];
                QCOMPARE(entry["old_rev"], old_journal[i]["rev"]);
                QCOMPARE(entry["before_old"], old_journal[i]["before"]);
                QCOMPARE(entry["after_old"], old_journal[i]["after"]);
                QCOMPARE(entry["kind"], old_journal[i]["kind"]);
                for (const char* side : {"before", "after"}) {
                    if (entry[side].is_null()) continue;
                    QVERIFY(AssetStore(to_path(book)).has(entry[side].get<std::string>(), ".state.json"));
                }
            }
            QCOMPARE(map["entries"][5]["after"], lines[0]["after"]);  // (the old book's last state is the new book's first)
        } else {
            QCOMPARE(report["map_entries"], Json(0));
            QVERIFY(!QFileInfo::exists(book + "/legacy/journal.jsonl"));
        }
    }

    void undoIntoTheOldHistory() {
        const QString source = copy_of("v3", "undo.genko");
        const QString book = tmp_.path() + "/undo-v4.genko";
        QCOMPARE(run_genko({"migrate", source, book}).exit_code, 0);
        std::int64_t revision = 1;
        Json last;
        const auto step = [&](const QStringList& args, int exit_code, const char* code) {
            const auto r = run_genko(args);
            last = one_line(r.out);
            QCOMPARE(r.exit_code, exit_code);
            if (code != nullptr) QCOMPARE(last["code"], Json(code));
            if (exit_code == 0 && last.contains("revision")) {
                QVERIFY(last["revision"].get<std::int64_t>() > revision);  // (a new generation every time)
                revision = last["revision"].get<std::int64_t>();
            }
            QCOMPARE(project(book)["version"], Json(4));
        };
        // right after the conversion
        step({"undo", book, "--as", "ai:hermes"}, 0, nullptr);
        QCOMPARE(last["rev"], Json(4));  // (the AI's note)
        QCOMPARE(project(book)["pages"][1]["note"], Json(""));
        step({"undo", book, "--as", "ai:hermes"}, 1, "other_actor");
        step({"undo", book, "--as", "ai:hermes", "--force"}, 1, "needs_person");  // (it would take back an approval)
        step({"undo", book, "--as", "human:作者"}, 0, nullptr);
        QCOMPARE(last["rev"], Json(3));
        QCOMPARE(project(book)["pages"][0]["name_ok"], Json(false));
        step({"redo", book, "--as", "ai:hermes"}, 1, "needs_person");
        step({"redo", book, "--as", "human:作者"}, 0, nullptr);
        step({"redo", book, "--as", "human:作者"}, 0, nullptr);
        QCOMPARE(project(book)["pages"][1]["note"], Json("AI のメモ"));
        step({"redo", book}, 1, "nothing_to_redo");
        // a new edit after the conversion, then Undo through it into the old history
        genko::test::write_bytes(tmp_.path() + "/undo-note.json", R"([{"op": "set_note", "page": 1, "note": "新しい編集"}])");
        step({"apply", book, tmp_.path() + "/undo-note.json", "--agent", "human:作者"}, 0, nullptr);
        step({"undo", book, "--as", "human:作者"}, 0, nullptr);
        QCOMPARE(project(book)["pages"][0]["note"], Json(""));
        step({"undo", book, "--as", "ai:hermes"}, 0, nullptr);
        QCOMPARE(last["rev"], Json(4));
        step({"undo", book, "--as", "human:作者"}, 0, nullptr);
        QCOMPARE(last["rev"], Json(3));
        step({"undo", book, "--as", "human:作者"}, 0, nullptr);
        QCOMPARE(last["rev"], Json(2));
        QCOMPARE(one_line(run_genko({"inspect", book}).out)["pages"][0]["leaf_count"], Json(1));  // (before the split)
        step({"undo", book, "--as", "human:作者"}, 1, "no_before");
        QCOMPARE(genko::test::book_problems(book), std::string());
        const auto audit = genko::storage::journal::audit_entries(to_path(book));
        int via_undo = 0, via_redo = 0;
        for (const Json& line : audit) {
            via_undo += line.value("via", "") == "undo" ? 1 : 0;
            via_redo += line.value("via", "") == "redo" ? 1 : 0;
        }
        QCOMPARE(via_undo, 2);  // (the old approval undone twice and redone once, each recorded as a person's change)
        QCOMPARE(via_redo, 1);
        QCOMPARE(audit[0]["rev"], Json(3));  // (the old line is still first, as it was)
        QVERIFY(!audit[0].contains("v"));
    }

    void assetOnlyTheOldHistoryNeeds() {
        // the old book once had a stroke on page 2 that the last commit removed: its blob is only in a snapshot
        const QString source = copy_of("v3", "history-only.genko");
        AssetStore store(to_path(source));
        const std::string blob = R"([{"id":"histonly0001","kind":"gpen","width_mm":0.5,"xy":"AAAAAAAA8D8AAAAAAAAAQA=="}])";
        const std::string blob_ref = store.put_bytes(blob, ".strokes.json");
        const std::string current = genko::test::read_bytes(source + "/project.json");
        Json snapshot = genko::core::parse_python_json(current);
        for (Json& layer : snapshot["pages"][1]["layers"]) {
            if (layer["role"] == "ink") {
                layer["strokes_blob"] = blob_ref;
                layer["stroke_count"] = 1;
            }
        }
        const std::string snapshot_ref = store.put_bytes(genko::core::dump_python_indent2(snapshot), ".project.json");
        Json line = Json::object({{"kind", "commit"}, {"rev", 5}, {"base_rev", 4}, {"actor", "human:作者"}, {"at", 1790866500.5},
                                  {"ops", Json::array()}, {"before", snapshot_ref}, {"after", AssetStore::ref(current)}});
        genko::storage::append_durable(to_path(source) / "studio" / "journal.jsonl", genko::core::dump_python(line) + "\n");

        const QString book = tmp_.path() + "/history-only-v4.genko";
        const auto r = run_genko({"migrate", source, book});
        QVERIFY2(r.exit_code == 0, r.out.constData());
        QCOMPARE(one_line(r.out)["map_entries"], Json(7));
        QCOMPARE(genko::test::read_bytes(book + "/" + QString::fromStdString(AssetStore::relpath(blob_ref, ".strokes.json"))), blob);
        QCOMPARE(one_line(run_genko({"inspect", book}).out)["pages"][1]["ink_stroke_count"], Json(0));
        QCOMPARE(run_genko({"undo", book, "--as", "human:作者"}).exit_code, 0);
        const auto loaded = genko::storage::load_document(to_path(book));
        const auto* ink = loaded.document.page(1).first_layer(genko::core::LayerRole::Ink);
        QVERIFY(ink != nullptr && ink->stroke_count() == 1);
        QCOMPARE(ink->strokes->items[0]->id, std::string("histonly0001"));
        QCOMPARE(ink->strokes->items[0]->points[0].y, 2.0);
        QCOMPARE(project(book)["pages"][1]["layers"][2]["strokes_blob"].get<std::string>(), blob_ref);  // (the same bytes)
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void changedWhileCopied() {
        if (!genko::test::fault_injection()) QSKIP("the hook that writes mid-copy is only in builds with fault injection");
        const QString source = copy_of("v3", "changing.genko");
        const QString book = tmp_.path() + "/changing-v4.genko";
        const auto hook = [](const QString& command) {
            QProcessEnvironment env;
            env.insert("GENKO_TEST_MIGRATE_HOOK",
                       QString::fromStdString(genko::core::dump_python(Json::array({"/bin/sh", "-c", command.toStdString()}))));
            return env;
        };
        auto r = run_genko({"migrate", source, book}, hook("printf '{}\\n' >> '" + source + "/studio/journal.jsonl'"));
        QCOMPARE(r.exit_code, 1);
        Json out = one_line(r.out);
        QCOMPARE(out["code"], Json("source_changed"));
        QCOMPARE(out["report"]["changed"], Json::array({"studio/journal.jsonl"}));
        QVERIFY(!QFileInfo::exists(book));
        QCOMPARE(staging_left(tmp_.path()), QStringList());
        // a new file is a change too
        r = run_genko({"migrate", source, book}, hook("printf x > '" + source + "/assets/new.png'"));
        QCOMPARE(one_line(r.out)["code"], Json("source_changed"));
        QFile::remove(source + "/assets/new.png");
        // only project.lock: not the book's content
        r = run_genko({"migrate", source, book}, hook("printf '{\"agent\": \"x\", \"released\": true}' > '" + source + "/project.lock'"));
        QVERIFY2(r.exit_code == 0, r.out.constData());
    }

    void heldByAnotherProcess() {
        const QString source = copy_of("v3", "held.genko");
        genko::test::Holder holder;
        QCOMPARE(holder.start(genko::test::lock_helper(), {source, "ai:writer"}), QString("locked"));
        const auto before = genko::test::tree_hashes(source);
        const auto r = run_genko({"migrate", source, tmp_.path() + "/held-v4.genko"});
        QCOMPARE(r.exit_code, 1);
        const Json out = one_line(r.out);
        QCOMPARE(out["code"], Json("source_locked"));
        QVERIFY2(out["error"].get<std::string>().find("(by " + genko::test::holder_seen("ai:writer") + ")") != std::string::npos,
                 r.out.constData());
        QVERIFY(!QFileInfo::exists(tmp_.path() + "/held-v4.genko"));
        QVERIFY(genko::test::tree_hashes(source) == before);
        holder.stop();
        QCOMPARE(run_genko({"migrate", source, tmp_.path() + "/held-v4.genko"}).exit_code, 0);
    }

    void repairsNeedConsent() {
        const QString source = copy_of("v3", "damaged.genko");
        QVERIFY(QFile::remove(source + "/assets/d7/d7b4e11bbe3c84eaf6b6cfc9feb631a3f120a519cf3c8256fa9ddaf78ca77483.png"));
        const QString book = tmp_.path() + "/damaged-v4.genko";
        auto r = run_genko({"migrate", source, book});
        QCOMPARE(r.exit_code, 1);
        const Json refused = one_line(r.out);
        QCOMPARE(refused["code"], Json("needs_repairs"));
        QCOMPARE(refused["report"]["issues"].size(), std::size_t{1});
        QCOMPARE(refused["report"]["issues"][0]["kind"], Json("missing_asset"));
        QCOMPARE(refused["report"]["issues"][0]["pointer"], Json("/pages/1/layers/0/asset"));
        QVERIFY(!QFileInfo::exists(book));
        r = run_genko({"migrate", source, book, "--accept-repairs"});
        QVERIFY2(r.exit_code == 0, r.out.constData());
        const Json accepted = one_line(r.out);
        QCOMPARE(accepted["repairs_accepted"], Json(1));
        QCOMPARE(accepted["issues"][0]["kind"], Json("missing_asset"));
        QCOMPARE(run_genko({"doctor", book}).exit_code, 0);  // (the new book is whole: the picture is gone, as accepted)
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void historyThatNeedsRepairs() {
        // the book is whole, but an old snapshot refers to strokes that are gone
        const QString source = copy_of("v3", "old-damage.genko");
        const std::string current = genko::test::read_bytes(source + "/project.json");
        Json snapshot = genko::core::parse_python_json(current);
        snapshot["pages"][1]["layers"][2]["strokes_blob"] = AssetStore::ref("strokes that are gone");
        AssetStore store(to_path(source));
        const std::string snapshot_ref = store.put_bytes(genko::core::dump_python_indent2(snapshot), ".project.json");
        const Json line = Json::object({{"kind", "commit"}, {"rev", 5}, {"base_rev", 4}, {"actor", "human:作者"}, {"at", 1790866500.5},
                                        {"ops", Json::array()}, {"before", snapshot_ref}, {"after", AssetStore::ref(current)}});
        genko::storage::append_durable(to_path(source) / "studio" / "journal.jsonl", genko::core::dump_python(line) + "\n");
        // without consent: converted, the damaged snapshot left out (and said); Undo to it refused
        const QString strict = tmp_.path() + "/old-damage-strict.genko";
        auto r = run_genko({"migrate", source, strict});
        QVERIFY2(r.exit_code == 0, r.out.constData());
        Json report = one_line(r.out);
        QCOMPARE(report["history"]["consistent"], Json(true));
        QCOMPARE(report["history"]["snapshots"]["unavailable"].size(), std::size_t{1});
        QCOMPARE(report["history"]["snapshots"]["unavailable"][0]["ref"], Json(snapshot_ref));
        QVERIFY(report["history"]["snapshots"]["unavailable"][0]["reason"].get<std::string>().starts_with("needs repairs"));
        const Json map = genko::test::read_json(strict + "/legacy/map.json");
        QVERIFY(map["entries"][6]["before"].is_null());
        QCOMPARE(map["entries"][6]["before_old"], Json(snapshot_ref));
        r = run_genko({"undo", strict, "--as", "human:作者"});
        QCOMPARE(r.exit_code, 1);
        QCOMPARE(one_line(r.out), Json::object({{"ok", false}, {"error", "snapshot " + snapshot_ref + " is missing from assets/"},
                                                {"code", "missing_snapshot"}}));
        // with consent: the snapshot comes over without the strokes it had lost
        const QString accepted = tmp_.path() + "/old-damage-accepted.genko";
        r = run_genko({"migrate", source, accepted, "--accept-repairs"});
        QVERIFY2(r.exit_code == 0, r.out.constData());
        report = one_line(r.out);
        QCOMPARE(report["history"]["snapshots"]["unavailable"], Json::array());
        QCOMPARE(run_genko({"undo", accepted, "--as", "human:作者"}).exit_code, 0);
        QCOMPARE(genko::test::book_problems(accepted), std::string());
    }

    void historyChangedOutsideTheJournal() {
        // project.json no longer the old journal's last state: the history is kept in legacy/, but not undone into
        const QString source = copy_of("v3", "outside.genko");
        Json p = genko::test::read_json(source + "/project.json");
        p["title"] = "外で変えた題";
        genko::test::write_bytes(source + "/project.json", genko::core::dump_python_indent2(p));
        const QString book = tmp_.path() + "/outside-v4.genko";
        const auto r = run_genko({"migrate", source, book});
        QVERIFY2(r.exit_code == 0, r.out.constData());
        const Json report = one_line(r.out);
        QCOMPARE(report["history"]["consistent"], Json(false));
        QCOMPARE(report["map_entries"], Json(0));
        QVERIFY(report["history"]["note"].get<std::string>().find("changed outside the journal") != std::string::npos);
        QCOMPARE(genko::test::read_bytes(book + "/legacy/journal.jsonl"), genko::test::read_bytes(source + "/studio/journal.jsonl"));
        QCOMPARE(one_line(run_genko({"undo", book}).out)["code"], Json("nothing_to_undo"));
        QCOMPARE(project(book)["title"], Json("外で変えた題"));
    }

    void refusals() {
        const QString source = copy_of("v3", "refusals.genko");
        // a book from a newer Genko
        const QString newer = tmp_.path() + "/v99.genko";
        genko::test::write_bytes(newer + "/project.json", R"({"version": 99, "title": "t", "episode": 1, "pages": []})");
        auto r = run_genko({"migrate", newer, tmp_.path() + "/v99-new.genko"});
        QCOMPARE(r.exit_code, 2);
        QCOMPARE(one_line(r.out)["code"], Json("unsupported_version"));
        // a v4 book
        const QString converted = tmp_.path() + "/refusals-v4.genko";
        QCOMPARE(run_genko({"migrate", source, converted}).exit_code, 0);
        r = run_genko({"migrate", converted, tmp_.path() + "/again.genko"});
        QCOMPARE(r.exit_code, 1);
        QCOMPARE(one_line(r.out)["code"], Json("not_legacy"));
        // the new folder: not one that holds something, not inside the old book
        r = run_genko({"migrate", source, converted});
        QCOMPARE(one_line(r.out)["code"], Json("exists"));
        r = run_genko({"migrate", source, source + "/inside.genko"});
        QCOMPARE(one_line(r.out)["code"], Json("value"));
        QVERIFY(!QFileInfo::exists(source + "/inside.genko"));
        QVERIFY(QDir().mkpath(tmp_.path() + "/empty-target.genko"));
        QCOMPARE(run_genko({"migrate", source, tmp_.path() + "/empty-target.genko"}).exit_code, 0);  // (an empty folder will do)
        r = run_genko({"migrate", tmp_.path() + "/nothing-here.genko", tmp_.path() + "/x.genko"});
        QCOMPARE(r.exit_code, 1);
        QCOMPARE(one_line(r.out)["code"], Json("not_found"));
        QCOMPARE(run_genko({"migrate", source}).exit_code, 2);  // (usage)
    }

    void unicodeSpacesAndLongPaths() {
        QString deep = tmp_.path() + "/原稿 フォルダ";
#ifdef Q_OS_WIN
        // Beyond MAX_PATH (260) Windows needs long paths turned on (LongPathsEnabled; the executables say they are
        // long-path aware): checked here when it is on, and Unicode and spaces only otherwise.
        const QSettings fs_settings(QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\FileSystem"),
                                    QSettings::NativeFormat);
        const bool long_paths = fs_settings.value(QStringLiteral("LongPathsEnabled")).toInt() == 1;
        const int levels = long_paths ? 6 : 0;
        qInfo("Windows long paths: %s", long_paths ? "on" : "off (Unicode and spaces only)");
#else
        const bool long_paths = true;
        const int levels = 6;
#endif
        for (int i = 0; i < levels; ++i) deep += "/とても長い名前のフォルダ 第" + QString::number(i) + "階層 " + QString(20, QChar(u'あ'));
        const QString source = deep + "/旧 原稿.genko";
        genko::test::copy_tree(genko::test::test_data("legacy/book-v3.genko"), source);
        const QString book = deep + "/新しい 原稿 v4.genko";
        if (long_paths) QVERIFY(book.size() > 200);
        const auto before = genko::test::tree_hashes(source);
        auto r = run_genko({"migrate", source, book});
        QVERIFY2(r.exit_code == 0, r.out.constData());
        QVERIFY(genko::test::tree_hashes(source) == before);
        genko::test::write_bytes(deep + "/ops 1.json", R"([{"op": "set_note", "page": 1, "note": "長いパス"}])");
        QCOMPARE(run_genko({"apply", book, deep + "/ops 1.json", "--agent", "human:作者"}).exit_code, 0);
        QCOMPARE(run_genko({"undo", book, "--as", "human:作者"}).exit_code, 0);
        QCOMPARE(run_genko({"undo", book, "--as", "ai:hermes"}).exit_code, 0);  // (into the old history)
        const auto doctor = run_genko({"doctor", book});
        const Json report = one_line(doctor.out);
        if (QDir(book).absolutePath().size() > 150) {
            QCOMPARE(report["problems"].size(), std::size_t{1});  // (Python's warning about Windows and long paths)
            QVERIFY(report["problems"][0].get<std::string>().ends_with("characters; Windows may refuse deep asset paths"));
            QCOMPARE(doctor.exit_code, 1);
        } else {
            QCOMPARE(report["problems"], Json::array());
        }
        QCOMPARE(genko::test::book_problems(book), std::string());
    }

    void windowsLineEndings() {
        // an old book whose project.json was saved with CRLF (an older Python on Windows): still its journal's state
        const QString source = copy_of("v3", "crlf.genko");
        std::string text = genko::test::read_bytes(source + "/project.json");
        std::string crlf;
        for (const char c : text) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
        genko::test::write_bytes(source + "/project.json", crlf);
        const QString book = tmp_.path() + "/crlf-v4.genko";
        const auto r = run_genko({"migrate", source, book});
        QVERIFY2(r.exit_code == 0, r.out.constData());
        QCOMPARE(one_line(r.out)["history"]["consistent"], Json(true));
        QCOMPARE(one_line(r.out)["map_entries"], Json(6));
        QVERIFY(genko::test::read_bytes(book + "/project.json").find('\r') == std::string::npos);
        QCOMPARE(run_genko({"undo", book, "--as", "ai:hermes"}).exit_code, 0);
    }

    void gcAfterTheConversion() {
        const QString source = copy_of("v3", "gc.genko");
        const QString book = tmp_.path() + "/gc-v4.genko";
        QCOMPARE(run_genko({"migrate", source, book}).exit_code, 0);
        AssetStore store(to_path(book));
        const std::string orphan = store.put_bytes("nobody's", ".png");
        for (const fs::path& file : store.all_files()) {
            fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::hours(48));
        }
        const auto dry = run_genko({"gc", book, "--dry-run"});
        QCOMPARE(one_line(dry.out)["removed"], Json::array({AssetStore::relpath(orphan, ".png")}));
        const auto done = run_genko({"gc", book});
        QCOMPARE(one_line(done.out)["removed"], Json::array({AssetStore::relpath(orphan, ".png")}));
        // the old history is all there: back to the old book's first state, and forward again
        for (const char* actor : {"ai:hermes", "human:作者", "human:作者"}) {
            QCOMPARE(run_genko({"undo", book, "--as", actor}).exit_code, 0);
        }
        QCOMPARE(one_line(run_genko({"undo", book}).out)["code"], Json("other_actor"));  // (Python checks who first)
        QCOMPARE(one_line(run_genko({"undo", book, "--as", "human:作者"}).out)["code"], Json("no_before"));
        for (int i = 0; i < 3; ++i) QCOMPARE(run_genko({"redo", book, "--as", "human:作者"}).exit_code, 0);
        QCOMPARE(genko::test::book_problems(book), std::string());
    }
};

QTEST_GUILESS_MAIN(TestMigrateE2e)
#include "test_migrate_e2e.moc"
