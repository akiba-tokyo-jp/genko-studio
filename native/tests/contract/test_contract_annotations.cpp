// PC5 J-c (annotations@1, SPEC.md PROD-01「注釈」) as an agent or a person uses it: through the command line — notes added,
// edited and deleted with `genko apply` (a person's and an agent's, the agent refused on the person's), undone with
// `genko undo`, the book naming annotations@1 only while a page has notes (none: its features and project.json as
// before), `genko inspect --annotations` —; the PSD's global block Anno as psd-tools reads it (the kind, open flag, rects,
// colour, author, date, and the text in UTF-16 with its lines ending in CR), read back as the page's notes (where they
// were), written again as read while unchanged (the same bytes), anew when changed, not at all when gone; keep.psd's
// note read and written back as it was; a sound note psd-tools wrote kept and written back after the text notes; a
// .pcraft's notes written and read back (with $GENKO_PCRAFT_CHECK, upstream's reader reads it); ファイル → 読み込み →
// 注釈…'s reading of a PSD's and a .pcraft's notes.

#include <QtTest>

#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>

#include "core/annotations.hpp"
#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "formats/export.hpp"
#include "formats/notes_import.hpp"
#include "formats/open_file.hpp"
#include "formats/pcraft_write.hpp"
#include "render/ops_registry.hpp"
#include "render/psd.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

core::Document run(const core::Document& doc, const Json& op, const char* actor = "human:test") {
    return core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor(actor)).doc;
}

Json op(const char* name, Json more = Json::object()) {
    Json out{{"op", name}, {"page", 1}};
    for (auto& [k, v] : more.items()) out[k] = v;
    return out;
}

// A 96 × 72 mm page at 200 dpi, without panels.
core::Document book() {
    auto doc = core::new_episode("注釈", core::Num(1), 1, core::PageSpec::custom(96, 72, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    return doc;
}

// Two notes: a person's (two lines of Japanese and an emoji, its box) and one closed.
core::Document noted() {
    core::Document doc = book();
    doc = run(doc, op("add_annotation", {{"at_mm", Json::array({12.7, 25.4})}, {"text", "ここを直す\n2 行目 😀"}, {"author", "editor"},
                                         {"color", Json::array({255, 128, 0})}, {"popup_mm", Json::array({24, 32, 40, 24})},
                                         {"modified", "2026-10-29T08:00:00Z"}}));
    doc = run(doc, op("add_annotation", {{"at_mm", Json::array({50.8, 50.8})}, {"text", "閉じた"}, {"open", false}, {"modified", ""}}));
    return doc;
}

const core::Annotation* titled(const core::Page& page, const std::string& text) {
    for (const core::Annotation& a : page.annotations)
        if (a.text == text) return &a;
    return nullptr;
}

}  // namespace

class TestContractAnnotations : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    int views_ = 0;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }
    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    Json apply(const QString& book, const QString& name, const Json& ops, const char* agent = "human:test") {
        genko::test::write_bytes(path(name), ops.dump());
        int exit = -1;
        const Json out = genko({QStringLiteral("apply"), book, path(name), QStringLiteral("--agent"), QString::fromLatin1(agent)}, &exit);
        if (exit != 0) return Json{{"failed", out}};
        return out;
    }
    Json project(const QString& book) { return Json::parse(genko::test::read_bytes(book + QStringLiteral("/project.json"))); }
    static bool names_notes(const Json& project) {
        const Json features = project.value("features", Json::array());
        return std::find(features.begin(), features.end(), Json("annotations@1")) != features.end();
    }
    Json notes_tool(const QStringList& args) {
        const auto ran = genko::test::run(genko::test::python_ref(),
                                          QStringList{genko::test::repo_root() + QStringLiteral("/tools/migration/psd_notes_check.py")} + args,
                                          genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) return Json{{"error", ran.err.toStdString()}};
        return Json::object();
    }
    Json notes_view(const QString& psd) {
        const QString out = path(QStringLiteral("notes-%1.json").arg(++views_));
        const Json ran = notes_tool({QStringLiteral("view"), out, psd});
        if (ran.contains("error")) return ran;
        return genko::test::read_json(out);
    }
    static std::string anno_of(const QString& psd) {
        const render::psd::File file = render::psd::File::read(genko::test::read_bytes(psd));
        for (const auto& g : file.global_blocks())
            if (g.key == "Anno") return std::string(g.data);
        return {};
    }
    QString export_psd(const core::Document& doc, const QString& name) {
        const QString file = path(name);
        (void)formats::export_page_psd(doc, doc.page(0), fs::path(file.toStdString()), 200);
        return file;
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    // Through the command line: a person's note and an agent's, the agent refused on the person's, saved with
    // annotations@1, listed, undone; all deleted: the book's project.json as before.
    void throughTheCommandLine() {
        const QString book_path = path(QStringLiteral("book.genko"));
        genko::test::write_project(book(), fs::path(book_path.toStdString()));
        const std::string before = genko::test::read_bytes(book_path + QStringLiteral("/project.json"));
        QVERIFY(!names_notes(project(book_path)));
        Json done = apply(book_path, QStringLiteral("a.json"), Json::array({op("add_annotation", {{"at_mm", Json::array({10, 10})}, {"text", "人の注釈"}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        Json saved = project(book_path);
        QVERIFY2(names_notes(saved), saved.value("features", Json::array()).dump().c_str());
        done = apply(book_path, QStringLiteral("b.json"), Json::array({op("add_annotation", {{"at_mm", Json::array({20, 20})}, {"text", "AI の注釈"}})}),
                     "ai:claude");
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        int exit = -1;
        Json listed = genko({QStringLiteral("inspect"), book_path, QStringLiteral("--annotations")}, &exit);
        QVERIFY2(exit == 0, listed.dump().c_str());
        QCOMPARE(listed["pages"].size(), std::size_t(1));
        QCOMPARE(listed["pages"][0]["page"], Json(1));
        const Json& notes = listed["pages"][0]["annotations"];
        QCOMPARE(notes.size(), std::size_t(2));
        QCOMPARE(notes[0]["text"], Json("人の注釈"));
        QCOMPARE(notes[0]["actor"], Json("human:test"));
        QCOMPARE(notes[0]["by_person"], Json(true));
        QCOMPARE(notes[1]["actor"], Json("ai:claude"));
        QCOMPARE(notes[1]["by_person"], Json(false));
        const std::string person = notes[0]["id"].get<std::string>(), agent = notes[1]["id"].get<std::string>();
        // (the agent: its own note yes, the person's no — the book as it was)
        done = apply(book_path, QStringLiteral("c.json"), Json::array({op("set_annotation", {{"id", agent}, {"text", "直した"}})}), "ai:claude");
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const std::string kept = genko::test::read_bytes(book_path + QStringLiteral("/project.json"));
        done = apply(book_path, QStringLiteral("d.json"), Json::array({op("delete_annotations", {{"ids", Json::array({person})}})}), "ai:claude");
        QVERIFY2(done.contains("failed") && done.dump().find("only a person can delete it") != std::string::npos, done.dump().c_str());
        QCOMPARE(genko::test::read_bytes(book_path + QStringLiteral("/project.json")), kept);
        genko({QStringLiteral("inspect"), book_path, QStringLiteral("--annotations"), QStringLiteral("--page"), QStringLiteral("7")}, &exit);
        QVERIFY(exit != 0);
        genko({QStringLiteral("inspect"), book_path, QStringLiteral("--annotations"), QStringLiteral("--comps")}, &exit);
        QVERIFY(exit != 0);
        // all deleted by the person: no annotations@1, project.json as before
        done = apply(book_path, QStringLiteral("e.json"), Json::array({op("delete_annotations", {{"all", true}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        saved = project(book_path);
        QVERIFY(!names_notes(saved));
        QVERIFY(saved.dump().find("\"annotations\"") == std::string::npos);
        QCOMPARE(saved["pages"], Json::parse(before)["pages"]);
        const Json undone = genko({QStringLiteral("undo"), book_path, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QVERIFY2(exit == 0, undone.dump().c_str());
        QVERIFY(names_notes(project(book_path)));
        listed = genko({QStringLiteral("inspect"), book_path, QStringLiteral("--annotations"), QStringLiteral("--page"), QStringLiteral("1")}, &exit);
        QCOMPARE(listed["pages"][0]["annotations"].size(), std::size_t(2));
    }

    // PSD: Anno as psd-tools reads it; read back; written as read while unchanged, anew when changed, not when gone.
    void psdNotes() {
        const core::Document doc = noted();
        const QString first = export_psd(doc, QStringLiteral("notes.psd"));
        const Json view = notes_view(first);
        QVERIFY2(!view.contains("error") && view["notes"].is_array(), view.dump().c_str());
        QCOMPARE(view["notes"].size(), std::size_t(2));
        const Json& a = view["notes"][0];
        QCOMPARE(a["kind"], Json("txtA"));
        QCOMPARE(a["open"], Json(1));
        QCOMPARE(a["text"], Json("ここを直す\n2 行目 😀"));
        QVERIFY(a["data"].get<std::string>().find("000d") != std::string::npos);  // (its lines end in CR)
        QVERIFY(a["data"].get<std::string>().starts_with("feff"));
        QCOMPARE(a["author"], Json("editor"));
        QCOMPARE(a["modified"], Json("D:20261029080000Z"));
        QCOMPARE(a["color_space"], Json(0));
        QCOMPARE(a["color"], Json::array({65535, 32896, 0, 0}));
        QCOMPARE(a["icon"][0], Json(200));  // (top: 25.4 mm at 200 dpi)
        QCOMPARE(a["icon"][1], Json(100));  // (left: 12.7 mm)
        QCOMPARE(a["popup"], Json::array({252, 189, 441, 504}));  // (top, left, bottom, right of 24, 32, 40 × 24 mm: 7.875 px a mm)
        QCOMPARE(view["notes"][1]["open"], Json(0));
        QCOMPARE(view["notes"][1]["modified"], Json(""));
        // read back: the page's notes where they were
        formats::Opened opened = formats::open_file(genko::test::read_bytes(first), "notes.psd");
        const core::Document back = opened.doc;
        const core::Page& page = back.page(0);
        QCOMPARE(page.annotations.size(), std::size_t(2));
        const core::Annotation* one = titled(page, "ここを直す\n2 行目 😀");
        QVERIFY(one != nullptr);
        QVERIFY(std::fabs(one->at_mm[0] - 12.7) < 0.2 && std::fabs(one->at_mm[1] - 25.4) < 0.2);
        QCOMPARE(one->author, std::string("editor"));
        QCOMPARE(one->color, (std::array<int, 3>{255, 128, 0}));
        QCOMPARE(one->modified, std::string("2026-10-29T08:00:00Z"));
        QVERIFY(one->open && one->popup_mm);
        QCOMPARE(one->actor, std::string("genko"));
        QVERIFY(page.native_psd && std::any_of(page.native_psd->copied.begin(), page.native_psd->copied.end(),
                                                [](const core::PsdCopied& c) { return c.resource < 0 && c.global == "Anno"; }));
        // written again unchanged: Anno as read
        const QString again = export_psd(back, QStringLiteral("again.psd"));
        QCOMPARE(anno_of(again), anno_of(first));
        // changed: anew
        const core::Document changed = run(back, op("set_annotation", {{"id", one->id}, {"text", "直した"}}));
        const Json changed_view = notes_view(export_psd(changed, QStringLiteral("changed.psd")));
        QCOMPARE(changed_view["notes"][0]["text"], Json("直した"));
        QCOMPARE(changed_view["notes"].size(), std::size_t(2));
        // gone: no Anno
        const core::Document gone = run(back, op("delete_annotations", {{"all", true}}));
        QVERIFY(anno_of(export_psd(gone, QStringLiteral("gone.psd"))).empty());
        QVERIFY(notes_view(path(QStringLiteral("gone.psd")))["notes"].is_null());
        // (a page without notes: no Anno, as before)
        QVERIFY(anno_of(export_psd(book(), QStringLiteral("plain.psd"))).empty());
    }

    // keep.psd's note read (its author, text, colour) and written back as it was; a sound note psd-tools wrote kept and
    // written back after the page's text notes when they change.
    void keptAndSoundNotes() {
        const QString keep = genko::test::repo_root() + QStringLiteral("/native/tests/data/psd_keep/keep.psd");
        formats::Opened kept = formats::open_file(genko::test::read_bytes(keep), "keep.psd");
        QCOMPARE(kept.doc.page(0).annotations.size(), std::size_t(1));
        QCOMPARE(kept.doc.page(0).annotations[0].text, std::string("メモ"));
        QCOMPARE(kept.doc.page(0).annotations[0].author, std::string("author"));
        QCOMPARE(kept.doc.page(0).annotations[0].modified, std::string());
        QCOMPARE(anno_of(export_psd(kept.doc, QStringLiteral("keep-again.psd"))), anno_of(keep));
        // a sound note
        const QString made = path(QStringLiteral("sound.psd"));
        const Json ran = notes_tool({QStringLiteral("make"), made});
        QVERIFY2(!ran.contains("error"), ran.dump().c_str());
        formats::Opened opened = formats::open_file(genko::test::read_bytes(made), "sound.psd");
        const core::Page& page = opened.doc.page(0);
        QCOMPARE(page.annotations.size(), std::size_t(1));  // (the text note; the sound note is the file's)
        QCOMPARE(page.annotations[0].text, std::string("音声の前\n2 行目"));
        QCOMPARE(page.annotations[0].color, (std::array<int, 3>{255, 0, 0}));
        QCOMPARE(page.annotations[0].modified, std::string("2024-01-02T03:04:05Z"));
        QCOMPARE(anno_of(export_psd(opened.doc, QStringLiteral("sound-again.psd"))), anno_of(made));
        const core::Document edited = run(opened.doc, op("set_annotation", {{"id", page.annotations[0].id}, {"text", "変えた"}}));
        const Json view = notes_view(export_psd(edited, QStringLiteral("sound-edited.psd")));
        QVERIFY2(view["notes"].is_array() && view["notes"].size() == 2, view.dump().c_str());
        QCOMPARE(view["notes"][0]["text"], Json("変えた"));
        QCOMPARE(view["notes"][1]["kind"], Json("sndM"));
        QCOMPARE(view["notes"][1]["data"], notes_view(made)["notes"][1]["data"]);
        // (every text note deleted: the sound note alone)
        const core::Document none = run(opened.doc, op("delete_annotations", {{"all", true}}));
        const Json alone = notes_view(export_psd(none, QStringLiteral("sound-alone.psd")));
        QVERIFY2(alone["notes"].is_array() && alone["notes"].size() == 1 && alone["notes"][0]["kind"] == Json("sndM"), alone.dump().c_str());
    }

    // .pcraft: written and read back; upstream's reader reads it.
    void pcraftNotes() {
        const core::Document doc = noted();
        std::vector<std::string> notes;
        const std::string bytes = formats::pcraft_bytes(doc, 0, {}, &notes);
        const QString file = path(QStringLiteral("notes.pcraft"));
        genko::test::write_bytes(file, bytes);
        formats::Opened opened = formats::open_file(bytes, "notes.pcraft");
        const core::Page& page = opened.doc.page(0);
        QCOMPARE(page.annotations.size(), std::size_t(2));
        const core::Annotation* one = titled(page, "ここを直す\n2 行目 😀");
        QVERIFY(one != nullptr);
        QVERIFY(std::fabs(one->at_mm[0] - 12.7) < 0.2 && std::fabs(one->at_mm[1] - 25.4) < 0.2);
        QCOMPARE(one->modified, std::string("2026-10-29T08:00:00Z"));
        QCOMPARE(one->color, (std::array<int, 3>{255, 128, 0}));
        for (const std::string& n : opened.notes) QVERIFY2(n.find("注釈") == std::string::npos, n.c_str());
        const QString tool = qEnvironmentVariable("GENKO_PCRAFT_CHECK");
        if (tool.isEmpty()) return;
        QProcess process;
        process.start(tool, QStringList{QStringLiteral("check"), file});
        QVERIFY(process.waitForFinished(600000));
        QVERIFY2(process.exitCode() == 0, process.readAllStandardOutput().constData());
    }

    // ファイル → 読み込み → 注釈…: a PSD's and a .pcraft's notes read (by the file's resolution), put on another page as
    // ops (one Undo); a file without notes and one of another kind refused.
    void importNotes() {
        const core::Document doc = noted();
        const QString psd = export_psd(doc, QStringLiteral("import.psd"));
        const auto from_psd = formats::read_annotations(genko::test::read_bytes(psd));
        QCOMPARE(from_psd.size(), std::size_t(2));
        const core::Document target = book();
        const core::Document after = core::CommandBus(render::ops_registry())
                                         .apply(target, formats::annotation_ops(from_psd, target.page(0)), core::Actor("human:test"))
                                         .doc;
        QCOMPARE(after.page(0).annotations.size(), std::size_t(2));
        const core::Annotation* one = titled(after.page(0), "ここを直す\n2 行目 😀");
        QVERIFY(one != nullptr && std::fabs(one->at_mm[0] - 12.7) < 0.2);
        QCOMPARE(one->modified, std::string("2026-10-29T08:00:00Z"));
        QCOMPARE(one->actor, std::string("human:test"));
        std::vector<std::string> said;
        const auto from_pcraft = formats::read_annotations(formats::pcraft_bytes(doc, 0, {}, &said));
        QCOMPARE(from_pcraft.size(), std::size_t(2));
        bool refused = false;
        try {
            (void)formats::read_annotations(genko::test::read_bytes(export_psd(book(), QStringLiteral("empty.psd"))));
        } catch (const core::Error& error) {
            refused = std::string(error.what()).find("the file has no notes") != std::string::npos;
        }
        QVERIFY(refused);
    }
};

QTEST_GUILESS_MAIN(TestContractAnnotations)
#include "test_contract_annotations.moc"
