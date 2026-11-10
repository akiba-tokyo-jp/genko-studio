// The studio's own files where this build goes past Python (SPEC COMP-01a, M5) or must write what Python writes:
// broken entries of studio/presence.json and studio/claims/ taken for missing or run out (Python stops with a
// TypeError, an AttributeError or a ValueError), a claim's temporary file named apart from its actor (an actor's name
// holding "/", "\" or ".." does not put a file outside the folder), tools.jsonl's "ms" as Python's round(ms, 1)
// writes it (an int stays an int), and the files Python writes in text mode with its line ends ("\r\n" on Windows).

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QTemporaryDir>

#include <filesystem>
#include <string>

#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/pynum.hpp"
#include "storage/fsutil.hpp"
#include "studio/files.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace fs = std::filesystem;
namespace st = genko::studio;

namespace {

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    genko::storage::write_atomic(path, text);
}

std::string text_of(const fs::path& path) { return genko::storage::read_file(path); }

}  // namespace

class TestStudioFiles : public QObject {
    Q_OBJECT

private slots:
    void brokenPresenceEntries() {
        QTemporaryDir tmp;
        const fs::path book = genko::core::path_from_utf8(tmp.path().toStdString());
        const fs::path file = book / "studio" / "presence.json";
        // (an entry that is no dict, a time that is no number: Python's presence stops with a TypeError / ValueError)
        write_text(file, R"({"actors": {"x": 5, "y": {"at": "soon"}, "z": {"at": 1000.0}, "w": {"at": "1050"}}, "acks": {"ai:a": 7}})");
        const auto others = st::others(book, "ai:a", 1100.0);
        QCOMPARE(others.size(), std::size_t{2});
        QCOMPARE(QString::fromStdString(others[0].actor), QStringLiteral("w"));  // (a time in a text of digits: float() reads it)
        QCOMPARE(others[0].minutes_ago, 0.8);
        QCOMPARE(QString::fromStdString(others[1].actor), QStringLiteral("z"));
        QCOMPARE(others[1].minutes_ago, 1.7);
        const auto fresh = st::check(book, "ai:a", 1100.0);  // (the broken ack is none: both are new to ai:a)
        QCOMPARE(fresh.size(), std::size_t{2});
        QCOMPARE(genko::core::parse_python_json(text_of(file))["acks"]["ai:a"],
                 Json::parse(R"({"others": ["w", "z"], "at": 1100.0})"));
        QVERIFY(st::check(book, "ai:a", 1100.0).empty());  // (told once)
        st::touch(book, "ai:a", 1100.0);  // (the broken entries are dropped, the good ones kept)
        QCOMPARE(genko::core::parse_python_json(text_of(file))["actors"],
                 Json::parse(R"({"z": {"at": 1000.0}, "w": {"at": "1050"}, "ai:a": {"at": 1100.0}})"));
        // the file no JSON object at all, or its actors no dict
        write_text(file, R"([1, 2])");
        QVERIFY(st::others(book, "ai:a", 1100.0).empty());
        write_text(file, R"({"actors": [1, 2]})");
        QVERIFY(st::others(book, "ai:a", 1100.0).empty());
        st::touch(book, "ai:b", 1200.0);
        QCOMPARE(genko::core::parse_python_json(text_of(file))["actors"], Json::parse(R"({"ai:b": {"at": 1200.0}})"));
    }

    void brokenClaims() {
        QTemporaryDir tmp;
        const fs::path book = genko::core::path_from_utf8(tmp.path().toStdString());
        const fs::path claims = book / "studio" / "claims";
        // (an expiry that is no number: Python's float() stops with a ValueError; a claim that is no dict: AttributeError)
        write_text(claims / "a.json", R"({"actor": "ai:b", "expires": "soon"})");
        write_text(claims / "b.json", R"([1, 2])");
        write_text(claims / "c.json", R"({"actor": "ai:b", "expires": "99999999999"})");
        for (const char* item : {"a", "b"}) {
            QVERIFY(!st::holder(book, item, 1000.0));  // (taken for run out)
            QVERIFY(st::claim(book, item, "ai:a", 1000.0));  // (and taken over)
            QCOMPARE(QString::fromStdString(*st::holder(book, item, 1000.0)), QStringLiteral("ai:a"));
        }
        QCOMPARE(QString::fromStdString(*st::holder(book, "c", 1000.0)), QStringLiteral("ai:b"));  // (float("999…") reads)
        QVERIFY(!st::claim(book, "c", "ai:a", 1000.0));
    }

    void claimTemporaryFileStaysInItsFolder() {
        // (the book in a folder of the temporary one: an actor's "../" past the book's folder would land beside it)
        QTemporaryDir tmp;
        const fs::path book = genko::core::path_from_utf8((tmp.path() + "/book").toStdString());
        int n = 0;
        for (const char* actor : {"ai:../../../../../outside", "ai:a/../../x", "ai:b\\..\\..\\y", ".."}) {
            const std::string item = "item-" + std::to_string(++n);
            QVERIFY(st::claim(book, item, actor, 1000.0));  // a new claim
            QVERIFY(st::claim(book, item, actor, 1100.0));  // renewed (through a temporary file)
            QVERIFY(st::claim(book, item, "ai:other", 5000.0));  // run out, taken over (through a temporary file)
            QCOMPARE(QString::fromStdString(*st::holder(book, item, 5000.0)), QStringLiteral("ai:other"));
            QVERIFY(st::claim(book, item, actor, 9000.0));  // run out again, taken back
        }
        // nothing but the claims and their folders (no temporary file left, no folder made for one, none elsewhere)
        QStringList entries;
        QDirIterator it(tmp.path(), QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) entries << QDir(tmp.path()).relativeFilePath(it.next());
        entries.sort();
        QCOMPARE(entries, (QStringList{"book", "book/studio", "book/studio/claims", "book/studio/claims/item-1.json", "book/studio/claims/item-2.json",
                                       "book/studio/claims/item-3.json", "book/studio/claims/item-4.json"}));
    }

    void toolLogMsAsPythonWritesIt() {
        QTemporaryDir tmp;
        const fs::path book = genko::core::path_from_utf8(tmp.path().toStdString());
        using genko::core::Num;
        for (const Num& ms : {Num(3), Num(3.0), Num(3.25), Num(12.345), Num(-7)}) {
            st::record_tool(book, "ai:a", "apply", Json::object(), Json::object({{"ok", true}}), std::nullopt, ms, 1000.0);
        }
        const auto lines = st::read_tools(book);
        QCOMPARE(lines.size(), std::size_t{5});
        const std::string text = text_of(book / "studio" / "logs" / "tools.jsonl");
        // round(3, 1) is the int 3; round(3.0, 1) the float 3.0; round(3.25, 1) 3.2 (half to even on the double); …
        for (const char* written : {R"("ms": 3, )", R"("ms": 3.0, )", R"("ms": 3.2, )", R"("ms": 12.3, )", R"("ms": -7, )"}) {
            QVERIFY2(text.find(written) != std::string::npos, written);
        }
    }

    void textModeFilesHaveThePlatformsLineEnds() {
        QTemporaryDir tmp;
        const fs::path book = genko::core::path_from_utf8(tmp.path().toStdString());
        const fs::path config = book / "config";
        st::write_json(book / "studio" / "x.json", Json::parse(R"({"b": [1, 2], "a": {"c": "あ"}})"));
        st::set_tool(config, "a:b", Json::parse(R"({"label": "x", "sizes_px": [[1, 2]]})"));
        (void)st::write_request(book, Json::parse(R"({"id": "rq_1", "purpose": "art"})"), {});
        st::record_tool(book, "ai:a", "apply", Json::object(), std::nullopt, std::nullopt, genko::core::Num(1), 1.0);
        st::touch(book, "ai:a", 1.0);
        for (const fs::path& file : {book / "studio" / "x.json", config / "tools.json", book / "studio" / "requests" / "rq_1" / "request.json",
                                     book / "studio" / "logs" / "tools.jsonl"}) {
            const std::string text = text_of(file);
            QVERIFY2(text.find('\n') != std::string::npos, genko::core::path_to_utf8(file).c_str());
#ifdef _WIN32
            // (Python's text mode: every "\n" written as "\r\n")
            for (std::size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 1)) {
                QVERIFY2(at > 0 && text[at - 1] == '\r', genko::core::path_to_utf8(file).c_str());
            }
#else
            QVERIFY2(text.find('\r') == std::string::npos, genko::core::path_to_utf8(file).c_str());
#endif
        }
        // and each is read back whatever its line ends
        write_text(book / "studio" / "y.json", "{\r\n  \"a\": 1\r\n}\r\n");
        QCOMPARE(st::read_json(book / "studio" / "y.json"), Json::parse(R"({"a": 1})"));
        write_text(book / "studio" / "logs" / "tools.jsonl", "{\"at\": 1.0, \"ok\": true}\r\n{\"at\": 2.0, \"ok\": false}\r\n");
        QCOMPARE(st::read_tools(book).size(), std::size_t{2});
    }
};

QTEST_GUILESS_MAIN(TestStudioFiles)
#include "test_studio_files.moc"
