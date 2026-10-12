// The app's studio without Qt's windows against Python's (M5④): genko/app/review_model.py — the approval box's
// requests (their fields and titles), the process bar's counts, the ops of 承認 (a sheet with the candidate the box
// lists first, and without one) and of 差し戻し (nothing, blanks, a reason in two lines) — and the sheet candidates as the
// box numbers them, on the studio book of studio_harness.py with every kind of request (tools/migration/
// studio_gui_harness.py review, then `genko migrate`); and the Qt-free part of genko/app/ai_link.py and links.py: the
// text to paste for each client (with paths that need quotes), the first request, the AIs that wrote a book and the
// status line's words (at the minute's edges), the folder as Python writes it, links and the local socket's name.
// Where this build goes past Python (SPEC COMP-01a): a broken presence entry is left out (Python's dialog stops).

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include "app/ai_link.hpp"
#include "app/links.hpp"
#include "app/review_model.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace fs = std::filesystem;
namespace ai_link = genko::app::ai_link;
namespace review_model = genko::app::review_model;

namespace {

genko::test::Run python(const QString& script, const QStringList& args, const QString& scratch, int timeout_ms = 1800000) {
    QStringList full{genko::test::repo_root() + QStringLiteral("/tools/migration/") + script};
    full += args;
    return genko::test::run(genko::test::python_ref(), full, genko::test::python_env(scratch), timeout_ms);
}

fs::path fpath(const QString& text) { return genko::core::path_from_utf8(text.toStdString()); }

// What C++ gives for a call that Python answers with {"ops"} or {"raised", "message"}.
Json outcome(const std::function<Json()>& call) {
    try {
        return Json{{"ops", call()}};
    } catch (const genko::core::PyValueError& error) {
        return Json{{"raised", "ValueError"}, {"message", error.what()}};
    } catch (const genko::core::Error& error) {
        return Json{{"raised", error.code()}, {"message", error.what()}};
    }
}

void same(const Json& got, const Json& want, const std::string& what) {
    std::string where;
    QVERIFY2(genko::test::strict_equal(got, want, &where),
             (what + ": " + where + "\n  C++:    " + got.dump() + "\n  Python: " + want.dump()).c_str());
}

}  // namespace

class TestContractStudioGui : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

private slots:
    void initTestCase() {
        QVERIFY2(!genko::test::python_ref().isEmpty(), "the reference Python is needed: set GENKO_PYREF");
        QVERIFY(scratch_.isValid());
    }

    void reviewModelLikePython() {
        const QString py = path("py.genko");
        const auto made = python(QStringLiteral("studio_harness.py"), {QStringLiteral("make-studiobook"), py}, path("pyenv"));
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        QFile::remove(py + QStringLiteral("/assets.json"));  // (not part of the book)
        const auto ran = python(QStringLiteral("studio_gui_harness.py"), {QStringLiteral("review"), py, path("review.json")}, path("pyenv"));
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const QString v4 = path("cpp.genko");
        const auto migrated = genko::test::run_genko({QStringLiteral("migrate"), py, v4});
        QVERIFY2(migrated.finished && migrated.exit_code == 0, (migrated.out + migrated.err).constData());
        const Json want = genko::test::read_json(path("review.json"));
        const genko::core::Document doc = genko::storage::load_document(fpath(v4)).document;

        // the process bar
        Json progress = Json::array();
        for (const auto& [label, count] : review_model::progress(doc)) progress.push_back(Json::array({label, count}));
        same(progress, want["progress"], "progress");

        // the approval box: every request, in Python's order
        const auto items = review_model::inbox(doc);
        QCOMPARE(items.size(), want["inbox"].size());
        QVERIFY(items.size() >= 9);  // (name, sheet ×2, help ×2, art, export, two proposals)
        std::set<std::string> kinds;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const review_model::InboxItem& item = items[i];
            const Json& w = want["inbox"][i];
            const std::string at = "item " + std::to_string(i) + " (" + w["title"].get<std::string>() + ")";
            kinds.insert(item.kind + ":" + item.gate_text());
            Json pages = Json::array();
            for (const Json& p : item.pages) pages.push_back(p);
            const Json fields = {{"ticket_id", item.ticket_id}, {"kind", item.kind}, {"gate", item.gate},       {"pages", pages},
                                 {"character_id", item.character_id}, {"frame_id", item.frame_id}, {"text", item.text}, {"by", item.by},
                                 {"name", item.name}, {"title", item.title()}};
            for (const char* key : {"ticket_id", "kind", "gate", "pages", "character_id", "frame_id", "text", "by", "name", "title"}) {
                same(fields[key], w[key], at + " " + key);
            }
            Json sheets = Json::array();
            for (const auto& [label, cand] : review_model::numbered_sheets(doc, item)) sheets.push_back(Json::array({label, cand.value("id", Json())}));
            if (!(item.kind == "gate" && item.gate_text() == "sheet")) sheets = Json::array();
            same(sheets, w["sheets"], at + " sheets");
            const Json choice = sheets.empty() ? Json() : sheets[0][1];
            same(outcome([&] { return review_model::approve_ops(doc, item, fpath(v4), choice); }), w["approve"], at + " approve");
            if (w.contains("approve_none")) {
                same(outcome([&] { return review_model::approve_ops(doc, item, fpath(v4), Json()); }), w["approve_none"], at + " approve without a candidate");
            }
            const char* texts[] = {"", "  ", "  直してください\n二行目も  "};
            for (std::size_t t = 0; t < 3; ++t) {
                same(outcome([&] { return review_model::send_back_ops(doc, item, texts[t]); }), w["send_back"][t], at + " send back " + std::to_string(t));
            }
        }
        for (const char* kind : {"gate:name", "gate:art", "gate:sheet", "gate:export", "help:", "proposal:layout", "proposal:lines"}) {
            QVERIFY2(kinds.count(kind) == 1, kind);
        }
    }

    void aiLinkLikePython() {
        // the books the AIs wrote: two conversations of one agent, another agent, a person (not an AI)
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
        Json cases = Json::array();
        const QStringList clients = {QStringLiteral("claude-desktop"), QStringLiteral("claude-code"), QStringLiteral("hermes"), QStringLiteral("other")};
        const std::vector<std::vector<std::string>> bases = {{"/opt/genko/bin/genko"},
                                                            {"/home/a b/py\"thon", "-m", "genko"},
                                                            {"C:\\Program Files\\Genko\\genko.exe"},
                                                            {"/作業/げんこう/genko", "--x\tz"}};
        const QStringList roots = {QStringLiteral("/tmp/x y"), QStringLiteral("/原稿/フォルダ"), QStringLiteral("rel/dir"), QStringLiteral("/a\"q")};
        for (const QString& client : clients) {
            for (std::size_t b = 0; b < bases.size(); ++b) {
                cases.push_back(Json{{"fn", "snippet"}, {"client", client.toStdString()}, {"root", roots[static_cast<int>(b)].toStdString()}, {"base", bases[b]}});
            }
        }
        for (const char* b : {"/a/b.genko", "/a/日本語の原稿.genko/", "rel.genko"}) cases.push_back(Json{{"fn", "first_request"}, {"book", b}});
        cases.push_back(Json{{"fn", "first_request"}, {"book", nullptr}});
        for (const char* a : {"ai:hermes/9204", "ai:hermes", "ai:x/", "human:leaf", "", "ai:", "ai:a/b/c"}) cases.push_back(Json{{"fn", "ai_name"}, {"actor", a}});
        for (const int m : {0, 1, 59, 60, 61, 119, 120, 2879, 2880, 2881, 10000}) cases.push_back(Json{{"fn", "ago"}, {"minutes", m}});
        // (the minutes rounded half to even, the 5 minutes' edge, an hour, days)
        for (const double now : {1700.5, 1750.5, 1750.0, 1999.0, 2000.5, 2000.6, 5300.0, 200000.0}) {
            cases.push_back(Json{{"fn", "recent_ai"}, {"book", book.toStdString()}, {"now", now}});
            cases.push_back(Json{{"fn", "status_words"}, {"book", book.toStdString()}, {"now", now}});
        }
        for (const QString& b : {lone, none}) {
            cases.push_back(Json{{"fn", "recent_ai"}, {"book", b.toStdString()}, {"now", 200.0}});
            cases.push_back(Json{{"fn", "status_words"}, {"book", b.toStdString()}, {"now", 200.0}});
        }
        cases.push_back(Json{{"fn", "status_words"}, {"book", nullptr}, {"now", 1.0}});
#ifndef _WIN32
        for (const char* typed : {"", "  ", ".", "./", "./a/", "a//b", "/", "//", "///x", "a/./b/..", "日本語 フォルダ/", " /tmp/x "}) {
            cases.push_back(Json{{"fn", "root_text"}, {"typed", typed}});
        }
#endif
        for (const char* text : {"genko://use-style?id=x", "GENKO:x", " genko://x", "", "http://x", "genko"}) cases.push_back(Json{{"fn", "is_link"}, {"text", text}});
        for (const char* user : {"leaf", "ユーザー x.y", "a-b_c", "...", "Ａ１"}) cases.push_back(Json{{"fn", "link_name"}, {"user", user}});
        genko::test::write_bytes(path("ai_cases.json"), genko::core::dump_python(cases));
        const auto ran = python(QStringLiteral("studio_gui_harness.py"), {QStringLiteral("ai"), path("ai_cases.json"), path("ai.json")}, path("pyenv"));
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json want = genko::test::read_json(path("ai.json"));
        QCOMPARE(want.size(), cases.size());
        const QByteArray logname = qgetenv("LOGNAME");
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const Json& c = cases[i];
            const std::string fn = c["fn"].get<std::string>();
            const std::string at = "case " + std::to_string(i) + " " + c.dump();
            Json got;
            if (fn == "snippet") {
                QStringList base;
                for (const Json& part : c["base"]) base << QString::fromStdString(part.get<std::string>());
                ai_link::set_command_for_tests(base);
                const ai_link::Snippet s = ai_link::snippet(QString::fromStdString(c["client"].get<std::string>()),
                                                            ai_link::root_text(QString::fromStdString(c["root"].get<std::string>())));
                ai_link::set_command_for_tests(std::nullopt);
                got = Json{{"where", s.where.toStdString()}, {"text", s.text.toStdString()}};
            } else if (fn == "first_request") {
                const std::optional<fs::path> b = c["book"].is_null() ? std::nullopt : std::optional<fs::path>(fpath(QString::fromStdString(c["book"].get<std::string>())));
                got = ai_link::first_request(b).toStdString();
            } else if (fn == "ai_name") {
                got = ai_link::ai_name(QString::fromStdString(c["actor"].get<std::string>())).toStdString();
            } else if (fn == "ago") {
                got = ai_link::ago(c["minutes"].get<std::int64_t>()).toStdString();
            } else if (fn == "recent_ai" || fn == "status_words") {
                const std::optional<fs::path> b = c["book"].is_null() ? std::nullopt : std::optional<fs::path>(fpath(QString::fromStdString(c["book"].get<std::string>())));
                const double now = c["now"].get<double>();
                if (fn == "recent_ai") {
                    got = Json::array();
                    for (const auto& r : ai_link::recent_ai(b, now)) got.push_back(Json{{"actor", r.actor}, {"minutes_ago", r.minutes_ago}, {"working", r.working}});
                } else {
                    const auto [words, working] = ai_link::status_words(b, now);
                    got = Json::array({words.toStdString(), working});
                }
            } else if (fn == "root_text") {
                got = ai_link::root_text(QString::fromStdString(c["typed"].get<std::string>())).toStdString();
            } else if (fn == "is_link") {
                got = genko::app::links::is_link(QString::fromStdString(c["text"].get<std::string>()));
            } else if (fn == "link_name") {
                qputenv("LOGNAME", QByteArray::fromStdString(c["user"].get<std::string>()));
                got = genko::app::links::server_name().toStdString();
            }
            same(got, want[i], at);
        }
        qputenv("LOGNAME", logname);
    }

    // C++ (SPEC COMP-01a): presence entries Python's float() stops on — an entry that is not an object, a time that is not
    // a number — are left out, and the others still listed (Python's AI dialog and status line stop with the error).
    void brokenPresenceLeftOut() {
        const QString book = path("broken/b.genko");
        QDir().mkpath(book + QStringLiteral("/studio"));
        genko::test::write_bytes(book + QStringLiteral("/studio/presence.json"),
                                 R"({"actors": {"ai:a": "x", "ai:b": {"at": "soon"}, "ai:c": {"at": 940}, "ai:d": [1]}})");
        const Json cases = Json::array({Json{{"fn", "recent_ai"}, {"book", book.toStdString()}, {"now", 1000.0}}});
        genko::test::write_bytes(path("broken_cases.json"), genko::core::dump_python(cases));
        const auto ran = python(QStringLiteral("studio_gui_harness.py"), {QStringLiteral("ai"), path("broken_cases.json"), path("broken.json")}, path("pyenv"));
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json want = genko::test::read_json(path("broken.json"));
        QVERIFY2(want[0].contains("raised"), want.dump().c_str());  // (Python stops)
        const auto found = ai_link::recent_ai(fpath(book), 1000.0);
        QCOMPARE(found.size(), std::size_t(1));
        QCOMPARE(found[0].actor, std::string("ai:c"));
        QCOMPARE(found[0].minutes_ago, std::int64_t(1));
        QVERIFY(found[0].working);
        QCOMPARE(ai_link::status_words(fpath(book), 1000.0).first, QStringLiteral("AI: c 作業中"));
    }
};

QTEST_GUILESS_MAIN(TestContractStudioGui)
#include "test_contract_studio_gui.moc"
