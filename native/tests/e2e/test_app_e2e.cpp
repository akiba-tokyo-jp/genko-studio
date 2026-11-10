// The desktop app as a person runs it, in processes of its own (M2-G1 試験 6; AC-SAVE 1 and 8 across processes): `genko
// app <book>` (and `genko` alone, the start screen) on the offscreen platform, driven by a test script
// (GENKO_TEST_SCRIPT: the pen on the canvas, the menu commands, the questions answered — app/test_script.hpp).
//
//   open → draw (mouse, pen) → 元に戻す → 保存 → Genko を終わる → open again in a new process: the line kept is there
//   (its id, points and pressures), and the journal's undo and redo work across the two processes.
//   AC-SAVE 1: after the autosave alone (no 保存), the process ends at once; a new process reads back the text and ruby
//   the book had, the line drawn and the panels cut (coordinates), exactly.
//   AC-SAVE 8: a book that cannot be written (GENKO_FAULT): the recovery copy is written; the process ends; the next one
//   offers it, takes it, and saves it as a new revision.
//
// Test scripts exist only in builds with fault injection (Debug, ASan): skipped in a release configuration.

#include <QtTest>

#include <QProcess>
#include <QTemporaryDir>

#include <filesystem>

#include "core/model.hpp"
#include "core/paths.hpp"
#include "storage/fault.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"

namespace fs = std::filesystem;
using genko::core::Json;

namespace {

fs::path path_of(const QString& text) { return genko::core::path_from_utf8(text.toStdString()); }
QString qpath(const fs::path& path) { return QString::fromStdString(genko::core::path_to_utf8(path)); }

void write_book(const fs::path& dir, const genko::core::Document& doc) {
    genko::storage::ProjectLock lock(dir, "genko");
    lock.try_acquire();
    genko::storage::SaveRequest request;
    request.actor = "genko";
    request.ops = Json::array();
    genko::storage::Saver(lock).save(doc, request);
}

genko::core::Document read_book(const fs::path& dir) { return genko::storage::load_document(dir).document; }

const genko::core::Layer& ink_of(const genko::core::Page& page) {
    for (const auto& layer : page.layers) {
        if (layer.role == genko::core::LayerRole::Ink) return layer;
    }
    throw std::runtime_error("no ink layer");
}

struct Run {
    int exit_code = -1;
    std::vector<Json> log;
    QString err;
    const Json* step(const std::string& what, const std::string& tag = {}) const {
        for (const Json& line : log) {
            if (line.value("do", "") == what && (tag.empty() || line.value("tag", "") == tag)) return &line;
        }
        return nullptr;
    }
    bool done() const { return !log.empty() && log.back().value("done", false); }
};

// Run the app with a script (and its arguments: "app" and the book, or nothing for `genko` alone).
Run run_app(const QTemporaryDir& dir, const QString& name, const QStringList& arguments, const Json& steps,
            const QProcessEnvironment& extra = QProcessEnvironment()) {
    const QString script = dir.filePath(name + QStringLiteral(".json"));
    const QString log = dir.filePath(name + QStringLiteral(".jsonl"));
    {
        QFile file(script);
        if (!file.open(QIODevice::WriteOnly)) return {};
        file.write(QByteArray::fromStdString(Json::object({{"log", log.toStdString()}, {"steps", steps}}).dump()));
    }
    QProcess app;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("GENKO_CONFIG_DIR"), dir.filePath(QStringLiteral("config")));
    env.insert(QStringLiteral("GENKO_USER"), QStringLiteral("tester"));
    env.insert(QStringLiteral("GENKO_TEST_SCRIPT"), script);
    for (const QString& key : extra.keys()) env.insert(key, extra.value(key));
    app.setProcessEnvironment(env);
    app.start(QString::fromUtf8(GENKO_CLI), arguments);
    Run run;
    if (!app.waitForFinished(180000)) {
        app.kill();
        app.waitForFinished();
    }
    run.exit_code = app.exitStatus() == QProcess::NormalExit ? app.exitCode() : -1;
    run.err = QString::fromUtf8(app.readAllStandardError());
    QFile file(log);
    if (file.open(QIODevice::ReadOnly)) {
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (!line.isEmpty()) run.log.push_back(Json::parse(line.toStdString()));
        }
    }
    return run;
}

QString dump(const Run& run) {
    QString text = QStringLiteral("exit %1\n").arg(run.exit_code);
    for (const Json& line : run.log) text += QString::fromStdString(line.dump()).left(400) + QStringLiteral("\n");
    return text + run.err.right(2000);
}

Json stroke_step(const Json& points, const char* device) { return Json::object({{"do", "stroke"}, {"device", device}, {"points", points}}); }

}  // namespace

class TestAppE2e : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        if (!genko::storage::fault::compiled_in()) QSKIP("test scripts are only in builds with fault injection (not a release configuration)");
    }

    void drawUndoSaveQuitAndOpenAgain() {
        QTemporaryDir dir;
        const fs::path book = path_of(dir.filePath("原稿.genko"));
        write_book(book, genko::core::new_episode("試し", genko::core::Num(1), 4, genko::core::PageSpec::a4_mono()));
        const Json first = Json::array({
            Json::object({{"do", "wait_book"}, {"path", genko::core::path_to_utf8(book)}}),
            Json::object({{"do", "tool"}, {"name", "pen"}}),
            stroke_step(Json::array({Json::array({60, 80}), Json::array({70, 84}), Json::array({85, 92}), Json::array({100, 95})}), "mouse"),
            stroke_step(Json::array({Json::array({60, 120, 0.3}), Json::array({75, 126, 0.6}), Json::array({90, 130, 0.9})}), "tablet"),
            Json::object({{"do", "report"}, {"tag", "drawn"}}),
            Json::object({{"do", "action"}, {"name", "act_undo"}}),
            Json::object({{"do", "report"}, {"tag", "undone"}}),
            Json::object({{"do", "action"}, {"name", "act_save"}}),
            Json::object({{"do", "wait_saved"}}),
            Json::object({{"do", "report"}, {"tag", "saved"}}),
            Json::object({{"do", "quit"}}),
        });
        const Run one = run_app(dir, QStringLiteral("first"), {QStringLiteral("app"), qpath(book)}, first);
        QVERIFY2(one.exit_code == 0 && one.done(), qPrintable(dump(one)));
        const Json& drawn = *one.step("report", "drawn");
        QCOMPARE(drawn["lines"].size(), std::size_t{2});
        QCOMPARE(drawn["status"].get<std::string>(), std::string("dirty"));
        const Json& undone = *one.step("report", "undone");
        QCOMPARE(undone["lines"].size(), std::size_t{1});
        QVERIFY(undone["can_redo"].get<bool>());
        const Json& saved = *one.step("report", "saved");
        QCOMPARE(saved["status"].get<std::string>(), std::string("saved"));
        QCOMPARE(saved["revision"].get<std::int64_t>(), std::int64_t{2});
        QCOMPARE(one.step("quit")->at("windows_left").get<int>(), 0);
        const Json kept = saved["lines"][0];
        QCOMPARE(kept["id"], drawn["lines"][0]["id"]);
        // the book on disk (read here, in another process): that one line, as the window had it
        const genko::core::Document disk = read_book(book);
        QCOMPARE(disk.revision, std::int64_t{2});
        const genko::core::Layer& ink = ink_of(disk.page(0));
        QCOMPARE(ink.stroke_count(), std::size_t{1});
        const genko::core::Stroke& stroke = *ink.strokes->items[0];
        QCOMPARE(stroke.id, kept["id"].get<std::string>());
        QCOMPARE(stroke.points.size(), kept["points"].size());
        for (std::size_t i = 0; i < stroke.points.size(); ++i) {
            QCOMPARE(stroke.points[i].x, kept["points"][i][0].get<double>());
            QCOMPARE(stroke.points[i].y, kept["points"][i][1].get<double>());
        }
        QCOMPARE(Json(stroke.pressure), kept["pressure"]);
        QVERIFY(std::abs(stroke.points.front().x - 60) < 0.01 && std::abs(stroke.points.front().y - 80) < 0.01);
        // a new process: the line is there; undo and redo go through the book's journal (once the book's pages are all
        // read: the first page is shown first)
        const Json second = Json::array({
            Json::object({{"do", "wait_book"}}),
            Json::object({{"do", "wait_read"}}),
            Json::object({{"do", "report"}, {"tag", "reopened"}}),
            Json::object({{"do", "action"}, {"name", "act_undo"}}),
            Json::object({{"do", "wait_saved"}}),
            Json::object({{"do", "report"}, {"tag", "undone"}}),
            Json::object({{"do", "action"}, {"name", "act_redo"}}),
            Json::object({{"do", "wait_saved"}}),
            Json::object({{"do", "report"}, {"tag", "redone"}}),
            Json::object({{"do", "quit"}}),
        });
        const Run two = run_app(dir, QStringLiteral("second"), {QStringLiteral("app"), qpath(book)}, second);
        QVERIFY2(two.exit_code == 0 && two.done(), qPrintable(dump(two)));
        const Json& reopened = *two.step("report", "reopened");
        QCOMPARE(reopened["lines"], saved["lines"]);
        QCOMPARE(reopened["status"].get<std::string>(), std::string("saved"));
        QVERIFY(!reopened["loading"].get<bool>());
        QVERIFY(reopened["can_undo"].get<bool>());
        QCOMPARE(two.step("report", "undone")->at("lines").size(), std::size_t{0});
        QCOMPARE(two.step("report", "redone")->at("lines"), saved["lines"]);
        QCOMPARE(ink_of(read_book(book).page(0)).stroke_count(), std::size_t{1});
    }

    // AC-SAVE 1: the autosave alone, then the process ends; text, ruby, the line and the panels read back
    void theAutosaveOutlivesTheProcess() {
        QTemporaryDir dir;
        const fs::path book = path_of(dir.filePath("book.genko"));
        genko::core::Document doc = genko::core::new_episode("試し", genko::core::Num(1), 2, genko::core::PageSpec::a4_mono());
        genko::core::StoryLine& line = doc.add_line(genko::core::Num(1), "東京へ行く。", "ユキ", std::nullopt, "", genko::core::Num(30),
                                                     genko::core::Num(40), genko::core::Num(36), genko::core::Num(60));
        line.ruby_runs.push_back(Json::array({"東京", "とうきょう"}));
        const std::string root = doc.page(0).frames[0].id;
        write_book(book, doc);
        const Json steps = Json::array({
            Json::object({{"do", "wait_book"}}),
            Json::object({{"do", "tool"}, {"name", "pen"}}),
            stroke_step(Json::array({Json::array({50, 150, 0.4}), Json::array({70, 160, 0.8}), Json::array({95, 158, 0.5})}), "tablet"),
            Json::object({{"do", "ops"},
                          {"ops", Json::array({Json::object({{"op", "split_frame"}, {"page", 1}, {"frame_id", root}, {"axis", "horizontal"},
                                                             {"gutter_mm", 6.0}})})}}),
            Json::object({{"do", "wait_saved"}}),  // (the autosave: no 保存 is chosen)
            Json::object({{"do", "report"}, {"tag", "saved"}}),
            Json::object({{"do", "exit_now"}}),
        });
        const Run run = run_app(dir, QStringLiteral("autosave"), {QStringLiteral("app"), qpath(book)}, steps);
        QVERIFY2(run.exit_code == 0 && run.step("exit_now") != nullptr, qPrintable(dump(run)));
        QVERIFY(run.step("action") == nullptr);
        const Json& saved = *run.step("report", "saved");
        QCOMPARE(saved["status"].get<std::string>(), std::string("saved"));
        // read back in this process: the text and its ruby as they were, the line and the panels as the window had them
        const genko::core::Document disk = read_book(book);
        QCOMPARE(disk.story.size(), std::size_t{1});
        QCOMPARE(disk.story[0].text, std::string("東京へ行く。"));
        QCOMPARE(disk.story[0].speaker, std::string("ユキ"));
        QCOMPARE(disk.story[0].ruby_runs.size(), std::size_t{1});
        QCOMPARE(disk.story[0].ruby_runs[0], Json::array({"東京", "とうきょう"}));
        QVERIFY(disk.story[0].x_mm == genko::core::Num(30) && disk.story[0].y_mm == genko::core::Num(40));
        const genko::core::Layer& ink = ink_of(disk.page(0));
        QCOMPARE(ink.stroke_count(), std::size_t{1});
        const Json& line_shown = saved["lines"][0];
        QCOMPARE(ink.strokes->items[0]->id, line_shown["id"].get<std::string>());
        for (std::size_t i = 0; i < ink.strokes->items[0]->points.size(); ++i) {
            QCOMPARE(ink.strokes->items[0]->points[i].x, line_shown["points"][i][0].get<double>());
            QCOMPARE(ink.strokes->items[0]->points[i].y, line_shown["points"][i][1].get<double>());
        }
        Json frames = Json::array();
        for (const genko::core::Frame* frame : disk.page(0).leaf_frames()) {
            frames.push_back(Json::object({{"id", frame->id}, {"rect", genko::core::rect_to_json(frame->rect)}}));
        }
        QCOMPARE(frames.size(), std::size_t{2});
        QCOMPARE(frames, saved["frames"]);
        // and the app reads it back the same in a new process
        const Run again = run_app(dir, QStringLiteral("again"), {QStringLiteral("app"), qpath(book)},
                                  Json::array({Json::object({{"do", "wait_book"}}), Json::object({{"do", "report"}, {"tag", "opened"}}),
                                               Json::object({{"do", "quit"}})}));
        QVERIFY2(again.exit_code == 0 && again.done(), qPrintable(dump(again)));
        QCOMPARE(again.step("report", "opened")->at("lines"), saved["lines"]);
        QCOMPARE(again.step("report", "opened")->at("frames"), saved["frames"]);
    }

    // AC-SAVE 8 across processes: the recovery copy written when the book could not be, offered and taken next time
    void theRecoveryCopyIsOfferedInTheNextProcess() {
        QTemporaryDir dir;
        const fs::path book = path_of(dir.filePath("book.genko"));
        write_book(book, genko::core::new_episode("試し", genko::core::Num(1), 2, genko::core::PageSpec::a4_mono()));
        QProcessEnvironment full;
        full.insert(QStringLiteral("GENKO_FAULT"), QStringLiteral("assets:fail"));
        const Run broken = run_app(dir, QStringLiteral("broken"), {QStringLiteral("app"), qpath(book)},
                                   Json::array({Json::object({{"do", "wait_book"}}), Json::object({{"do", "tool"}, {"name", "pen"}}),
                                                stroke_step(Json::array({Json::array({50, 150}), Json::array({90, 160})}), "mouse"),
                                                Json::object({{"do", "wait_status"}, {"kind", "recovery_only"}}),
                                                Json::object({{"do", "report"}, {"tag", "failed"}}), Json::object({{"do", "exit_now"}})}),
                                   full);
        QVERIFY2(broken.exit_code == 0 && broken.step("exit_now") != nullptr, qPrintable(dump(broken)));
        QCOMPARE(broken.step("report", "failed")->at("status_words").get<std::string>(), std::string("復旧用コピーのみ（原稿には未保存）"));
        QCOMPARE(ink_of(read_book(book).page(0)).stroke_count(), std::size_t{0});
        const Run next = run_app(dir, QStringLiteral("next"), {QStringLiteral("app"), qpath(book)},
                                 Json::array({Json::object({{"do", "answers"}, {"questions", Json::object({{"復旧用のコピー", true}})}}),
                                              Json::object({{"do", "wait_book"}}), Json::object({{"do", "wait_saved"}}),
                                              Json::object({{"do", "report"}, {"tag", "recovered"}}), Json::object({{"do", "quit"}})}));
        QVERIFY2(next.exit_code == 0 && next.done(), qPrintable(dump(next)));
        bool asked = false;
        for (const Json& line : next.log) {
            if (line.value("asked", "") == "復旧用のコピー") asked = line.value("answer", false);
        }
        QVERIFY(asked);
        QCOMPARE(next.step("report", "recovered")->at("lines"), broken.step("report", "failed")->at("lines"));
        QCOMPARE(ink_of(read_book(book).page(0)).stroke_count(), std::size_t{1});
        QVERIFY(read_book(book).revision > 1);  // (taken as a new revision)
    }

    // `genko` alone: the start screen; closing it ends Genko, choosing a book opens it
    void genkoAloneShowsTheStartScreen() {
        QTemporaryDir dir;
        const Run closed = run_app(dir, QStringLiteral("start"), {}, Json::array({Json::object({{"do", "start_dialog"}, {"close", true}})}));
        QVERIFY2(closed.exit_code == 0, qPrintable(dump(closed)));
        QCOMPARE(closed.step("start_dialog")->at("cards").get<int>(), 3);
        const fs::path book = path_of(dir.filePath("book.genko"));
        write_book(book, genko::core::new_episode("始める", genko::core::Num(1), 2, genko::core::PageSpec::a4_mono()));
        const Run chosen = run_app(dir, QStringLiteral("chosen"), {},
                                   Json::array({Json::object({{"do", "start_dialog"}, {"choose", genko::core::path_to_utf8(book)}}),
                                                Json::object({{"do", "wait_book"}}), Json::object({{"do", "report"}, {"tag", "opened"}}),
                                                Json::object({{"do", "quit"}})}));
        QVERIFY2(chosen.exit_code == 0 && chosen.done(), qPrintable(dump(chosen)));
        QCOMPARE(chosen.step("report", "opened")->at("title").get<std::string>(), std::string("始める"));
    }

    void badArgumentsAreAUsageError() {
        QProcess app;
        app.start(QString::fromUtf8(GENKO_CLI), {QStringLiteral("app"), QStringLiteral("--frobnicate")});
        QVERIFY(app.waitForFinished(60000));
        QCOMPARE(app.exitCode(), 2);
        app.start(QString::fromUtf8(GENKO_CLI), {QStringLiteral("app"), QStringLiteral("a"), QStringLiteral("b")});
        QVERIFY(app.waitForFinished(60000));
        QCOMPARE(app.exitCode(), 2);
        // Qt must not consume app-invalid options before Genko validates its own CLI.
        QTemporaryDir dir;
        for (const QStringList& args : {QStringList{QStringLiteral("app"), QStringLiteral("-reverse")},
                                        QStringList{QStringLiteral("app"), QStringLiteral("-platform"), QStringLiteral("offscreen")}}) {
            const Run run = run_app(dir, QStringLiteral("qt-option"), args,
                                    Json::array({Json::object({{"do", "start_dialog"}, {"close", true}})}));
            QCOMPARE(run.exit_code, 2);
        }
    }

    void helpDoesNotRequireADisplay() {
        QProcess app;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("genko-no-such-platform"));
        app.setProcessEnvironment(env);
        app.start(QString::fromUtf8(GENKO_CLI), {QStringLiteral("app"), QStringLiteral("--help")});
        QVERIFY(app.waitForFinished(60000));
        QCOMPARE(app.exitStatus(), QProcess::NormalExit);
        QCOMPARE(app.exitCode(), 0);
        QVERIFY(app.readAllStandardOutput().contains("genko app [book]"));
    }
};

QTEST_GUILESS_MAIN(TestAppE2e)
#include "test_app_e2e.moc"
