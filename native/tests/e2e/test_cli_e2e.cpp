// The genko command line end to end: what `genko inspect` prints and returns for books it cannot read, as Python's
// genko/__main__.py does ({"ok": false, "error": …} on stdout; exit 2 for a book from a newer Genko, 1 for the rest),
// usage errors, --ascii, --stroke and `genko schema`; `genko studio` step by step, each a process of its own.

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <map>
#include <string>

#include "core/base64.hpp"

#include "testsupport.hpp"

using genko::core::Json;

namespace {

genko::test::Run genko_run(const QStringList& args) {
    return genko::test::run(genko::test::genko_cli(), args, QProcessEnvironment::systemEnvironment(), 120000);
}

void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

Json one_line(const QByteArray& out) {
    const std::string text = out.toStdString();
    if (text.empty() || text.back() != '\n' || text.find('\n') != text.size() - 1) return Json("not one line: " + text);
    return genko::core::parse_python_json(text);
}

QByteArray book_json(int version) {
    return QByteArray(R"({"version": )") + QByteArray::number(version) +
           R"(, "title": "t", "episode": 1, "spec": {"width_mm": 210, "height_mm": 297, "dpi": 600, "bleed_mm": 3,)"
           R"( "inner_margin_mm": 10}, "pages": []})";
}

}  // namespace

class TestCliE2e : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

private slots:
    void missingFolder() {
        const QString dir = tmp_.path() + "/nowhere.genko";
        const auto r = genko_run({"inspect", dir});
        QCOMPARE(r.exit_code, 1);
        const Json out = one_line(r.out);
        QCOMPARE(out["ok"], Json(false));
        QCOMPARE(out["error"].get<std::string>(),
                 "not found: [Errno 2] No such file or directory: '" + dir.toStdString() + "/project.json'");
        QCOMPARE(out["code"].get<std::string>(), std::string("not_found"));
        // the path as Python's pathlib writes it
        const auto relative = genko_run({"inspect", "./nowhere//x/"});
        QCOMPARE(one_line(relative.out)["error"].get<std::string>(),
                 std::string("not found: [Errno 2] No such file or directory: 'nowhere/x/project.json'"));
    }

    void brokenJson() {
        const QString dir = tmp_.path() + "/broken.genko";
        QVERIFY(QDir().mkpath(dir));
        write(dir + "/project.json", "{\"title\": \"t\",\n  oops}");
        const auto r = genko_run({"inspect", dir});
        QCOMPARE(r.exit_code, 1);
        const Json out = one_line(r.out);
        QCOMPARE(out["ok"], Json(false));
        QCOMPARE(out["error"].get<std::string>(),
                 std::string("Expecting property name enclosed in double quotes: line 2 column 3 (char 17)"));
        QCOMPARE(out["code"].get<std::string>(), std::string("json"));
        write(dir + "/project.json", "\xff\xfe{}");
        QCOMPARE(one_line(genko_run({"inspect", dir}).out)["error"].get<std::string>(),
                 std::string("'utf-8' codec can't decode byte 0xff in position 0: invalid start byte"));
    }

    void newerVersions() {
        const QString dir = tmp_.path() + "/v99.genko";
        QVERIFY(QDir().mkpath(dir));
        write(dir + "/project.json", book_json(99));
        const auto r = genko_run({"inspect", dir});
        QCOMPARE(r.exit_code, 2);
        const Json out = one_line(r.out);
        QCOMPARE(out["ok"], Json(false));
        QCOMPARE(out["error"].get<std::string>(),
                 std::string("project.json version 99 is newer than this build supports (4); update Genko"));
        QCOMPARE(out["code"].get<std::string>(), std::string("unsupported_version"));
        QByteArray needs = book_json(4);
        needs.replace("\"version\": 4", "\"version\": 4, \"min_reader\": 7");
        write(dir + "/project.json", needs);
        QCOMPARE(genko_run({"inspect", dir}).exit_code, 2);
        write(dir + "/project.json", book_json(4));
        QCOMPARE(genko_run({"inspect", dir}).exit_code, 0);  // (read-only for want of a book_id, but readable)
    }

    void notABook() {
        const QString dir = tmp_.path() + "/list.genko";
        QVERIFY(QDir().mkpath(dir));
        write(dir + "/project.json", "[]");
        auto r = genko_run({"inspect", dir});
        QCOMPARE(r.exit_code, 1);
        QCOMPARE(one_line(r.out)["code"].get<std::string>(), std::string("format"));
        write(dir + "/project.json", R"({"title": "t", "episode": 1, "pages": []})");
        r = genko_run({"inspect", dir});
        QCOMPARE(r.exit_code, 1);
        QCOMPARE(one_line(r.out)["error"].get<std::string>(), std::string("missing key 'spec'"));
        const auto file = genko_run({"inspect", dir + "/project.json"});  // a file, not a folder
        QCOMPARE(file.exit_code, 1);
#ifdef Q_OS_WIN
        // (Windows reports a file in the middle of a path as a missing path, as Python's open() does there)
        QVERIFY(one_line(file.out)["error"].get<std::string>().find("[Errno 2] No such file or directory") !=
                std::string::npos);
#else
        QVERIFY(one_line(file.out)["error"].get<std::string>().starts_with("[Errno 20] Not a directory"));
#endif
    }

    void usageErrors() {
#if !GENKO_WITH_APP
        QCOMPARE(genko_run({}).exit_code, 2);  // (with the desktop app built, `genko` alone starts it: test_app_e2e)
#endif
        QCOMPARE(genko_run({"frobnicate"}).exit_code, 2);
        QCOMPARE(genko_run({"inspect"}).exit_code, 2);
        QCOMPARE(genko_run({"inspect", genko::test::test_data("legacy/book-v3.genko"), "--fast"}).exit_code, 2);
        QCOMPARE(genko_run({"inspect", "a", "b"}).exit_code, 2);
        QCOMPARE(genko_run({"schema", "extra"}).exit_code, 2);
        const auto r = genko_run({"inspect"});
        QVERIFY(r.out.isEmpty());  // (usage goes to stderr)
        QVERIFY(!r.err.isEmpty());
    }

    void inspectOutput() {
        const QString book = genko::test::test_data("legacy/book-v3.genko");
        const auto plain = genko_run({"inspect", book});
        QCOMPARE(plain.exit_code, 0);
        const Json snapshot = one_line(plain.out);
        QCOMPARE(snapshot["title"].get<std::string>(), std::string("旧原稿 v3"));
        QVERIFY(plain.out.contains("旧原稿"));  // UTF-8, not escaped
        QVERIFY(!snapshot["pages"][0].contains("ink_strokes"));
        const auto ascii = genko_run({"--ascii", "inspect", book});
        QCOMPARE(ascii.exit_code, 0);
        for (const char c : ascii.out) QVERIFY(static_cast<unsigned char>(c) < 0x80);
        std::string where;
        QVERIFY2(genko::test::strict_equal(one_line(ascii.out), snapshot, &where), where.c_str());
        const auto full = genko_run({"inspect", "--full", book});
        QCOMPARE(one_line(full.out)["pages"][0]["ink_strokes"].size(), std::size_t{2});
    }

    void inspectStroke() {
        const QString book = genko::test::test_data("legacy/book-v3.genko");
        const auto r = genko_run({"inspect", book, "--stroke", "be2170a0e2fb"});
        QCOMPARE(r.exit_code, 0);
        QCOMPARE(r.out, QByteArray("{\"id\": \"be2170a0e2fb\", \"page\": 1, \"layer\": \"ink\", \"points\": [[100.0, 120.0], "
                                   "[140.0, 160.0]], \"kind\": \"gpen\"}\n"));
        const auto missing = genko_run({"inspect", book, "--stroke=nope"});
        QCOMPARE(missing.exit_code, 1);
        QCOMPARE(one_line(missing.out)["error"].get<std::string>(), std::string("no stroke nope"));
    }

    void schemaIsTheEmbeddedFile() {
        const auto r = genko_run({"schema"});
        QCOMPARE(r.exit_code, 0);
        const Json out = one_line(r.out);
        QCOMPARE(out["ok"], Json(true));
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/src/core/data/ops_schema.json");
        std::string where;
        QVERIFY2(genko::test::strict_equal(out["ops"], file, &where), where.c_str());
        QCOMPARE(out["ops"].size(), std::size_t{168});
        QVERIFY(r.out.startsWith("{\"ok\": true, \"ops\": [{\"op\": \"split_frame\""));
    }
    // `genko studio`, each step a process of its own (e2e/studio_cli_steps.json: this build's executable, as people run
    // it, in the root, the step's stdin given): every process started and ended in its time without crashing, with the
    // step's exit code (0, 1 or 3; 2 only for arguments the command line does not take), printing one JSON object or
    // nothing; the leases it took hold their item for 600 s from then (studio/claims/…: "expires"), and no name under the
    // root has a ":" (Windows takes none).
    void studioCommandLineAsProcesses() {
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/e2e/studio_cli_steps.json");
        const QString root = tmp_.path() + "/studio-cli";
        const QString config = tmp_.path() + "/studio-cli-config";
        QDir().mkpath(root);
        QProcessEnvironment env;
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), config);
        for (const auto& [key, value] : file["env"].items()) env.insert(QString::fromStdString(key), QString::fromStdString(value.get<std::string>()));
        const auto with_root = [&root](std::string text) {
            const std::string put = root.toStdString();
            for (std::size_t at = text.find("$root"); at != std::string::npos; at = text.find("$root", at + put.size())) text.replace(at, 5, put);
            return text;
        };
        const auto now = [] { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); };
        int ran_steps = 0;
        int usage = 0;
        int leases = 0;
        std::map<int, int> exits;
        for (std::size_t n = 0; n < file["steps"].size(); ++n) {
            const Json& step = file["steps"][n];
            if (step.contains("write")) {
                const QString target = root + "/" + QString::fromStdString(step["write"].get<std::string>());
                QDir().mkpath(QFileInfo(target).path());
                genko::test::write_bytes(target, step.contains("base64") ? genko::core::a2b_base64(step["base64"].get<std::string>())
                                                                         : step["text"].get<std::string>());
                continue;
            }
            QStringList args{QStringLiteral("studio")};
            for (const Json& a : step["cli"]) args << QString::fromStdString(with_root(a.get<std::string>()));
            const double started = now();
            const genko::test::Run ran = genko::test::run_genko(args, env, QByteArray::fromStdString(step.value("stdin", std::string())), root);
            const double ended = now();
            const std::string what = "step " + std::to_string(n) + " " + genko::core::dump_python(step).substr(0, 300) + "\n  the process: " +
                                     ran.described().toStdString();
            QVERIFY2(ran.started && ran.finished && !ran.crashed, what.c_str());
            const int expected = step.at("exit").get<int>();
            QVERIFY2(expected == 0 || expected == 1 || expected == 2 || expected == 3, what.c_str());
            QVERIFY2(ran.exit_code == expected, (what + "\n  exit " + std::to_string(expected) + " expected").c_str());
            ++exits[ran.exit_code];
            usage += ran.exit_code == 2 ? 1 : 0;
            // what it printed: one JSON object, or nothing
            const QByteArray printed = ran.out.trimmed();
            if (!printed.isEmpty()) {
                Json out;
                try {
                    out = genko::core::parse_python_json(ran.out.toStdString());
                } catch (const std::exception& error) {
                    QFAIL((what + "\n  stdout is not JSON: " + error.what()).c_str());
                }
                QVERIFY2(out.is_object(), what.c_str());
                if (ran.exit_code != 0) QVERIFY2(out.value("ok", true) == false, what.c_str());
            }
            // the leases taken or renewed by the step: for 600 s from then
            QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString claim = it.next();
                if (!claim.contains(QStringLiteral("/studio/claims/")) || !claim.endsWith(QStringLiteral(".json"))) continue;
                const Json lease = genko::core::parse_python_json(genko::test::read_bytes(claim));
                const double expires = lease.at("expires").get<double>();
                if (expires < started + 600 - 1) continue;  // (taken by an earlier step)
                QVERIFY2(expires <= ended + 600 + 0.001, (what + "\n  " + claim.toStdString() + ": expires after the lease").c_str());
                ++leases;
            }
            ++ran_steps;
        }
        // no name with a ":"
        QDirIterator all(root, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        QStringList colons;
        while (all.hasNext()) {
            const QString found = all.next();
            if (QFileInfo(found).fileName().contains(QLatin1Char(':'))) colons << found;
        }
        QVERIFY2(colons.isEmpty(), qPrintable(colons.join('\n')));
        qInfo("genko studio as processes: %d steps (%d exit 0, %d exit 1, %d usage errors, %d exit 3), %d leases", ran_steps, exits[0],
              exits[1], usage, exits[3], leases);
        QCOMPARE(ran_steps, 101);
        QVERIFY(leases >= 2);
    }

    // 新しい本のいつもの書体（利用者の決定 2026-11-14）: every book the command line makes new — `genko new` (a comic, and
    // --image), `genko studio init` — starts with 源暎アンチック as its usual face and says fonts@1; a book made of a file
    // (--from) is not made new and has none. An op applied (the book read and written again) keeps the face.
    void aNewBookStartsInGeneiAntique() {
        const QString root = tmp_.path() + "/new-face";
        QDir().mkpath(root);
        const auto project = [&](const QString& book) { return genko::core::parse_python_json(genko::test::read_bytes(book + "/project.json")); };
        const QString comic = root + "/comic.genko", image = root + "/image.genko", studio = root + "/studio.genko", opened = root + "/opened.genko";
        QCOMPARE(genko_run({"new", comic, "--pages", "2", "--plain"}).exit_code, 0);
        QCOMPARE(genko_run({"new", image, "--image", "--width", "40", "--height", "30", "--plain"}).exit_code, 0);
        QCOMPARE(genko_run({"studio", "init", studio, "--pages", "2"}).exit_code, 0);
        for (const QString& book : {comic, image, studio}) {
            const Json made = project(book);
            QCOMPARE(made["font_path"], Json("antique_genei"));
            QVERIFY2(std::find(made["features"].begin(), made["features"].end(), Json("fonts@1")) != made["features"].end(), made["features"].dump().c_str());
        }
        // (a picture opened as a book: no face of its own, as before)
        const QString png = root + "/one.png";
        write(png, QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="));
        const auto from = genko_run({"new", opened, "--from", png, "--plain"});
        QVERIFY2(from.exit_code == 0, from.out.constData());
        QCOMPARE(project(opened)["font_path"], Json(""));
        QVERIFY(project(opened)["features"].dump().find("fonts@1") == std::string::npos);
        // (read and written again: the face kept)
        const QString ops = root + "/ops.json";
        write(ops, R"([{"op": "add_line", "page": 1, "text": "漢字（かんじ）", "x_mm": 40, "y_mm": 40, "w_mm": 20, "h_mm": 40}])");
        const Json made_revision = project(comic)["revision"];
        const auto applied = genko_run({"apply", comic, ops});
        QVERIFY2(applied.exit_code == 0, applied.out.constData());
        QVERIFY(project(comic)["revision"].get<std::int64_t>() > made_revision.get<std::int64_t>());
        QCOMPARE(project(comic)["font_path"], Json("antique_genei"));
        QCOMPARE(project(comic)["features"], Json::array({"fonts@1"}));
    }

    // コマ割り K3 (SPEC LAYOUT-01): `genko studio layout-ideas` — the tool layout_ideas from the shell: the same seed
    // prints the same batch, exactly the panels asked for, the knobs as JSON and a draft plan from a file (each idea
    // carrying its plan), the book left as it was; a preset it does not know is a usage error (exit 2), a count past
    // the tool's range a refusal (exit 1); `genko studio call … layout_ideas` takes it by name too.
    void studioLayoutIdeas() {
        const QString root = tmp_.path() + "/layout-ideas";
        QDir().mkpath(root);
        const QString book = root + "/b.genko";
        QCOMPARE(genko_run({"studio", "init", book, "--pages", "4"}).exit_code, 0);
        const QByteArray before = genko::test::read_bytes(book + "/project.json").c_str();
        const QStringList asked{"studio", "layout-ideas", book, "--page", "2", "--panels", "3", "--preset", "action", "--seed", "km-00000000000000a2"};
        const auto first = genko_run(asked);
        const auto again = genko_run(asked);
        QVERIFY2(first.exit_code == 0, first.out.constData());
        QCOMPARE(first.out, again.out);
        const Json batch = one_line(first.out);
        QCOMPARE(batch["ok"], Json(true));
        QCOMPARE(batch["seed"], Json("km-00000000000000a2"));
        QCOMPARE(batch["request"]["preset"], Json("action"));
        QVERIFY(batch["ideas"].is_array() && !batch["ideas"].empty() && batch["ideas"].size() <= 12);
        for (const Json& idea : batch["ideas"]) QCOMPARE(idea["panels"], Json(3));
        // knobs as JSON, a draft plan from a file (its 3 panels), a count
        const QString plan = root + "/p002.json";
        write(plan, QByteArray::fromStdString(genko::test::read_bytes(genko::test::repo_root() + "/native/tests/fixtures/studio/demo4/p002.json")));
        const auto knobbed = genko_run({"studio", "layout-ideas", book, "--page", "2", "--knobs", R"({"slant": 0.9, "bleed": 0})", "--plan", plan, "--count", "5",
                                        "--focus", "1", "--seed", "k"});
        QVERIFY2(knobbed.exit_code == 0, knobbed.out.constData());
        const Json kb = one_line(knobbed.out);
        QCOMPARE(kb["request"]["knobs"]["slant"], Json(0.9));
        QCOMPARE(kb["request"]["panels"], Json(3));
        QVERIFY(!kb["ideas"].empty() && kb["ideas"].size() <= 5);
        for (const Json& idea : kb["ideas"]) {
            QCOMPARE(idea["largest"], Json("p1"));
            QCOMPARE(idea["plan"]["page"], Json(2));
            QCOMPARE(idea["plan"]["tiers"], idea["skeleton"]["tiers"]);
        }
        // the tool by name
        const QString args = root + "/args.json";
        write(args, R"({"page": 2, "panels": 3, "preset": "action", "seed": "km-00000000000000a2"})");
        const auto called = genko_run({"studio", "call", book, "layout_ideas", args});
        QVERIFY2(called.exit_code == 0, called.out.constData());
        QCOMPARE(one_line(called.out)["ideas"], batch["ideas"]);
        // refused: a preset it does not know (usage), a count past 48 (the tool's words)
        QCOMPARE(genko_run({"studio", "layout-ideas", book, "--page", "2", "--preset", "loud"}).exit_code, 2);
        const auto many = genko_run({"studio", "layout-ideas", book, "--page", "2", "--count", "49"});
        QCOMPARE(many.exit_code, 1);
        QCOMPARE(one_line(many.out)["issues"][0]["code"], Json("layout_ideas_invalid"));
        QCOMPARE(QByteArray(genko::test::read_bytes(book + "/project.json").c_str()), before);
    }
};

QTEST_GUILESS_MAIN(TestCliE2e)
#include "test_cli_e2e.moc"
