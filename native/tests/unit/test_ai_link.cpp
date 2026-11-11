// The Qt-free part of AI と作る (app/ai_link, app/links) and what this build keeps in a book for it, without windows:
//
// brokenPresenceLeftOut: presence entries that are not an object, or whose time is not a number, are left out, and the
// others still listed (SPEC COMP-01a: the AI dialog and the status line do not stop on them).
//
// wordsAtTheirEdges: the AIs that wrote a book (two conversations of one agent, another agent, a person — not an AI —,
// one with no time) and the status line's words at the minute's edges (rounded half to even, the 5 minutes', an hour,
// days); the minutes in words (ago), an actor's name (ai_name), the first request, what is a link (links::is_link): this
// build's words, fixed.
//
// genkoMcpKeepsItsToolLog: what `genko mcp` keeps in a book — the tool log (a failed call too, with its error; not the call
// held back for another conversation, book_in_use) and the presence of each conversation, read back by the AI dialog's
// recent_ai —, after the handshake of the protocol version the app's test connection and the clients send (2025-06-18).
//
// replyDraftsKeptByGcDoctorAndApply: the app's reply drafts (studio/reply_drafts/) in a v4 book keep their bytes through
// `genko gc` (the panel is there), `genko doctor` and `genko apply`, and are read back.

#include <QtTest>

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>

#include <set>

#include "app/ai_link.hpp"
#include "app/links.hpp"
#include "app/reply_box.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "service/studio_service.hpp"
#include "storage/reader.hpp"
#include "studio/files.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace fs = std::filesystem;
namespace ai_link = genko::app::ai_link;

namespace {

fs::path fpath(const QString& text) { return genko::core::path_from_utf8(text.toStdString()); }

void same(const Json& got, const Json& want, const std::string& what) {
    std::string where;
    QVERIFY2(genko::test::strict_equal(got, want, &where), (what + ": " + where + "\n  got:  " + got.dump() + "\n  want: " + want.dump()).c_str());
}

Json recent_json(const std::optional<fs::path>& book, double now) {
    Json got = Json::array();
    for (const auto& r : ai_link::recent_ai(book, now)) got.push_back(Json{{"actor", r.actor}, {"minutes_ago", r.minutes_ago}, {"working", r.working}});
    return got;
}

Json status_json(const std::optional<fs::path>& book, double now) {
    const auto [words, working] = ai_link::status_words(book, now);
    return Json::array({words.toStdString(), working});
}

}  // namespace

class TestAiLink : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

private slots:
    void initTestCase() { QVERIFY(scratch_.isValid()); }

    void brokenPresenceLeftOut() {
        const QString book = path("broken/b.genko");
        QDir().mkpath(book + QStringLiteral("/studio"));
        genko::test::write_bytes(book + QStringLiteral("/studio/presence.json"),
                                 R"({"actors": {"ai:a": "x", "ai:b": {"at": "soon"}, "ai:c": {"at": 940}, "ai:d": [1]}})");
        const auto found = ai_link::recent_ai(fpath(book), 1000.0);
        QCOMPARE(found.size(), std::size_t(1));
        QCOMPARE(found[0].actor, std::string("ai:c"));
        QCOMPARE(found[0].minutes_ago, std::int64_t(1));
        QVERIFY(found[0].working);
        QCOMPARE(ai_link::status_words(fpath(book), 1000.0).first, QStringLiteral("AI: c 作業中"));
    }

    void wordsAtTheirEdges() {
        // the books the AIs wrote: two conversations of one agent, another agent, a person (not an AI), one with no time
        const QString book = path("ai/b.genko");
        QDir().mkpath(book + QStringLiteral("/studio"));
        genko::test::write_bytes(book + QStringLiteral("/studio/presence.json"),
                                 R"({"actors": {"ai:hermes/9204": {"at": 1000.0}, "ai:claude-desktop": {"at": 1450}, "human:x": {"at": 1690},)"
                                 R"( "ai:hermes": {"at": 1700.5}, "ai:old": {}}})");
        const QString lone = path("ai/one.genko");
        QDir().mkpath(lone + QStringLiteral("/studio"));
        genko::test::write_bytes(lone + QStringLiteral("/studio/presence.json"), R"({"actors": {"ai:hermes/9204": {"at": 100}}})");
        const QString none = path("ai/none.genko");
        QDir().mkpath(none);

        const Json fixed = genko::test::read_json(genko::test::repo_root() + "/native/tests/unit/ai_link_words.json");
        // the minutes in words
        const Json& agos = fixed.at("agos");
        for (const auto& [minutes, words] : agos.items()) same(Json(ai_link::ago(std::stoll(minutes)).toStdString()), words, "ago " + minutes);
        QCOMPARE(agos.size(), std::size_t(11));
        // an actor's name
        const Json& names = fixed.at("names");
        for (const auto& [actor, name] : names.items()) same(Json(ai_link::ai_name(QString::fromStdString(actor)).toStdString()), name, "ai_name " + actor);
        QCOMPARE(names.size(), std::size_t(7));
        // the AIs that wrote the book and the status line, at the minute's edges
        const Json& at = fixed.at("at");
        for (const auto& [now, want] : at.items()) {
            same(recent_json(fpath(book), std::stod(now)), want[0], "recent_ai at " + now);
            same(status_json(fpath(book), std::stod(now)), want[1], "status_words at " + now);
        }
        QCOMPARE(at.size(), std::size_t(8));
        const Json& alone = fixed.at("alone");
        same(recent_json(fpath(lone), 200.0), alone[0], "recent_ai of a book one AI wrote");
        same(status_json(fpath(lone), 200.0), alone[1], "status_words of a book one AI wrote");
        same(recent_json(fpath(none), 200.0), Json::array(), "recent_ai of a book no AI wrote");
        same(status_json(fpath(none), 200.0), alone[2], "status_words of a book no AI wrote");
        same(status_json(std::nullopt, 1.0), alone[3], "status_words with no book");
        // the first request
        const Json& requests = fixed.at("requests");
        const char* books[] = {"/a/b.genko", "/a/日本語の原稿.genko/", "rel.genko"};
        for (std::size_t i = 0; i < 3; ++i) same(Json(ai_link::first_request(fpath(QString::fromUtf8(books[i]))).toStdString()), requests[i], books[i]);
        same(Json(ai_link::first_request(std::nullopt).toStdString()), requests[3], "first_request with no book");
        // what is a link
        const Json& links = fixed.at("links");
        for (const auto& [text, is] : links.items()) same(Json(genko::app::links::is_link(QString::fromStdString(text))), is, "is_link " + text);
        QCOMPARE(links.size(), std::size_t(6));
    }

    void genkoMcpKeepsItsToolLog() {
        const fs::path root = fpath(path("mcp"));
        fs::create_directories(root);
        const Json made = genko::service::StudioService(root, "human:tester")
                              .call("create_project", Json::array(), Json{{"name", "m.genko"}, {"title", "m"}, {"pages", 2}})
                              .to_dict();
        QVERIFY2(made.value("ok", false), made.dump().c_str());
        QProcess server;
        server.setProgram(genko::test::genko_cli());
        server.setArguments({QStringLiteral("mcp"), QStringLiteral("--root"), path("mcp"), QStringLiteral("--agent"), QStringLiteral("ai:hermes")});
        server.start();
        QVERIFY(server.waitForStarted(60000));
        QByteArray buffer;
        const auto rpc = [&](int id, const std::string& method, const Json& params) -> Json {
            server.write(QByteArray::fromStdString(genko::core::dump_python(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}) + "\n"));
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < 300000) {
                const qsizetype end = buffer.indexOf('\n');
                if (end >= 0) {
                    const Json message = genko::core::parse_python_json(buffer.left(end).toStdString());
                    buffer.remove(0, end + 1);
                    if (message.value("id", Json()) == Json(id)) return message;
                    continue;
                }
                if (!server.waitForReadyRead(1000) && server.state() == QProcess::NotRunning) break;
                buffer += server.readAllStandardOutput();
            }
            return Json{{"error", "no answer"}};
        };
        const auto call = [&](int id, const char* tool, Json args) -> Json {
            const Json reply = rpc(id, "tools/call", Json{{"name", tool}, {"arguments", std::move(args)}});
            if (!reply.contains("result")) return Json{{"ok", false}, {"error", reply.dump()}};
            return genko::core::parse_python_json(reply["result"]["content"][0]["text"].get<std::string>());
        };
        const Json hello = rpc(1, "initialize", Json{{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()},
                                                     {"clientInfo", Json{{"name", "genko-app"}, {"version", "1"}}}});
        QCOMPARE(hello["result"].value("protocolVersion", Json()), Json("2025-06-18"));
        server.write(QByteArray::fromStdString(genko::core::dump_python(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}) + "\n"));
        QCOMPARE(call(2, "ask_human", Json{{"project", "m.genko"}, {"text", "どちらがいいですか"}, {"page", 1}, {"session", "9204"}}).value("ok", Json()), Json(true));
        QCOMPARE(call(3, "request_approval", Json{{"project", "m.genko"}, {"gate", "name"}, {"pages", {1}}, {"session", "9204"}}).value("ok", Json()), Json(true));
        QCOMPARE(call(4, "ask_human", Json{{"project", "m.genko"}, {"text", "x"}, {"page", 99}, {"session", "9204"}}).value("ok", Json()), Json(false));
        QCOMPARE(call(5, "ask_human", Json{{"project", "m.genko"}, {"text", "別の会話"}, {"session", "7777"}}).value("code", Json()), Json("book_in_use"));
        QCOMPARE(call(6, "ask_human", Json{{"project", "m.genko"}, {"text", "別の会話"}, {"session", "7777"}}).value("ok", Json()), Json(true));
        QCOMPARE(call(7, "tickets", Json{{"project", "m.genko"}, {"session", "9204"}}).value("ok", Json()), Json(true));
        server.closeWriteChannel();
        QVERIFY(server.waitForFinished(60000));
        const fs::path book = root / "m.genko";
        const std::vector<Json> logged = genko::studio::read_tools(book);
        QCOMPARE(logged.size(), std::size_t(5));  // (2, 3, 4 — failed —, 6 and 7: the call held back is not a call)
        QCOMPARE(logged[2].value("ok", Json()), Json(false));
        QVERIFY(logged[2].contains("error"));
        for (std::size_t i : {0, 1, 3, 4}) QCOMPARE(logged[i].value("ok", Json()), Json(true));
        const double now = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0 + 30.0;
        const Json recent = recent_json(book, now);
        QCOMPARE(recent.size(), std::size_t(2));  // (the two conversations)
        std::set<std::string> actors;
        for (const Json& r : recent) {
            actors.insert(r["actor"].get<std::string>());
            QCOMPARE(r["working"], Json(true));
        }
        QCOMPARE(actors, (std::set<std::string>{"ai:hermes/9204", "ai:hermes/7777"}));
        QVERIFY(status_json(book, now)[1].get<bool>());
    }

    void replyDraftsKeptByGcDoctorAndApply() {
        namespace reply_drafts = genko::app::reply_drafts;
        const QString v4 = path("drafts/v4.genko");
        const auto migrated = genko::test::run_genko({QStringLiteral("migrate"), genko::test::test_data(QStringLiteral("legacy/book-v3.genko")), v4});
        QVERIFY2(migrated.finished && migrated.exit_code == 0, (migrated.out + migrated.err).constData());
        const QString words = QStringLiteral("一行目\n二行目");
        // (a panel's instruction: the panel is in the book, so gc keeps its draft)
        const QString item = QStringLiteral("panel:") + QString::fromStdString(genko::storage::load_document(fpath(v4)).document.page(0).leaf_frames().at(0)->id);
        const std::string bytes = reply_drafts::encode(item, words, QDateTime(QDate(2026, 10, 1), QTime(9, 0)));
        const fs::path book = fpath(v4);
        const fs::path file = reply_drafts::file(book, item);
        fs::create_directories(file.parent_path());
        const QString file_text = QString::fromStdString(genko::core::path_to_utf8(file));
        genko::test::write_bytes(file_text, bytes);
        genko::test::write_bytes(path("drafts/ops.json"), R"([{"op": "set_note", "page": 1, "note": "メモ"}])");
        for (const QStringList& args : {QStringList{QStringLiteral("gc"), v4}, QStringList{QStringLiteral("doctor"), v4},
                                        QStringList{QStringLiteral("apply"), v4, path("drafts/ops.json"), QStringLiteral("--agent"), QStringLiteral("human:tester")}}) {
            const auto done = genko::test::run_genko(args);
            QVERIFY2(done.finished && done.exit_code == 0, (args.join(' ') + ": " + done.out + done.err).toUtf8().constData());
            QVERIFY2(genko::test::read_bytes(file_text) == bytes, qPrintable(args.join(' ')));
        }
        const reply_drafts::Read back = reply_drafts::read(book);
        QVERIFY(back.discarded.isEmpty());
        QCOMPARE(back.drafts.size(), std::size_t(1));
        QCOMPARE(back.drafts.at(item), words);
    }
};

QTEST_GUILESS_MAIN(TestAiLink)
#include "test_ai_link.moc"
