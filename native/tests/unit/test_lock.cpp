// project.lock (storage::ProjectLock): the OS lock between two locks of one process and between processes, its
// content (Python's: token, agent, pid, host, acquired_at; "released" after), an older Genko's content, the
// converter's lock that neither makes nor writes the file, and acquire() with a timeout. (Against Python's
// genko.lock.ProjectLock: test_contract_save.)

#include <QtTest>

#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/pynum.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "testsupport.hpp"

namespace fs = std::filesystem;
using genko::core::Json;
using genko::storage::LockedError;
using genko::storage::ProjectLock;

namespace {

fs::path to_path(const QString& path) { return genko::storage::path_from_utf8(path.toStdString()); }

std::string locked_message(const std::function<void()>& f) {
    try {
        f();
    } catch (const LockedError& error) {
        return error.what();
    } catch (const genko::core::Error& error) {
        return "other error " + error.code() + ": " + error.what();
    }
    return "(not locked)";
}

}  // namespace

class TestLock : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

private slots:
    void twoLocksInOneProcess() {
        const fs::path dir = to_path(tmp_.path()) / "one.genko";
        ProjectLock first(dir, "human:作者");
        first.try_acquire();
        QVERIFY(first.held());
        QVERIFY(fs::is_regular_file(dir / "project.lock"));  // (the folder and the file are made)
#ifndef Q_OS_WIN  // (on Windows no one else can read the locked byte while it is held)
        const Json content = ProjectLock::read_content(dir / "project.lock");
        QCOMPARE(content["token"].get<std::string>(), first.token());
        QCOMPARE(content["agent"].get<std::string>(), std::string("human:作者"));
        QVERIFY(content["pid"].is_number_integer());
        QVERIFY(content["host"].is_string());
        QVERIFY(content["acquired_at"].is_number_float());
        // Python's json.dumps: ASCII only, ", " and ": "
        const std::string raw = genko::test::read_bytes(QString::fromStdString(genko::storage::path_to_utf8(dir / "project.lock")));
        QVERIFY(raw.starts_with("{\"token\": \""));
        QVERIFY(raw.find("\"agent\": \"human:\\u4f5c\\u8005\"") != std::string::npos);
#endif

        ProjectLock second(dir, "ai:other");
        QCOMPARE(locked_message([&] { second.try_acquire(); }),
                 "project locked: " + genko::storage::path_to_utf8(dir / "project.lock") + " (by " +
                     genko::test::holder_seen("human:作者") + ")");
        QVERIFY(!second.held());
        first.release();
        const Json released = ProjectLock::read_content(dir / "project.lock");
        QCOMPARE(released["released"], Json(true));
        QCOMPARE(released["agent"].get<std::string>(), std::string("human:作者"));
        second.try_acquire();
        QVERIFY(second.held());
        second.release();
        second.release();  // (twice: nothing)
    }

    void acquireWaits() {
        const fs::path dir = to_path(tmp_.path()) / "wait.genko";
        auto first = std::make_unique<ProjectLock>(dir, "first");
        first->try_acquire();
        ProjectLock second(dir, "second");
        const auto started = std::chrono::steady_clock::now();
        QVERIFY(locked_message([&] { second.acquire(std::chrono::milliseconds(300)); }).starts_with("project locked:"));
        QVERIFY(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(300));
        std::thread giver([&first] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            first->release();
        });
        second.acquire(std::chrono::seconds(10));
        giver.join();
        QVERIFY(second.held());
    }

    void anotherProcess() {
        const QString dir = tmp_.path() + "/other.genko";
        genko::test::Holder holder;
        QCOMPARE(holder.start(genko::test::lock_helper(), {dir, "helper-agent"}), QString("locked"));
        ProjectLock mine(to_path(dir), "me");
        QCOMPARE(locked_message([&] { mine.try_acquire(); }),
                 "project locked: " + genko::test::lock_file_text(dir) + " (by " + genko::test::holder_seen("helper-agent") + ")");
        QVERIFY(holder.stop().contains("released"));
        mine.try_acquire();
        QVERIFY(mine.held());
        // and the other way round: the helper cannot take it while this process holds it
        genko::test::Holder blocked;
        const QString first = blocked.start(genko::test::lock_helper(), {dir, "helper-agent"});
        QVERIFY2(first.startsWith("busy: project locked:") &&
                     first.endsWith(QString::fromStdString("(by " + genko::test::holder_seen("me") + ")")),
                 qPrintable(first));
        mine.release();
    }

    void olderGenkoContent() {
        const QString dir = tmp_.path() + "/older.genko";
        const double now = genko::storage::journal::now_seconds();
        // an older Genko wrote its name and time, without the OS lock (no token): respected for 15 minutes
        genko::test::write_bytes(dir + "/project.lock",
                                 "{\"agent\": \"old genko\", \"acquired_at\": " + genko::core::py_float_repr(now - 60) + "}");
        ProjectLock lock(to_path(dir), "me");
        QCOMPARE(locked_message([&] { lock.try_acquire(); }), "project locked: " + genko::test::lock_file_text(dir) + " (by old genko)");
        genko::test::write_bytes(dir + "/project.lock", "{\"acquired_at\": " + genko::core::py_float_repr(now - 10) + "}");
        QVERIFY(locked_message([&] { lock.try_acquire(); }).ends_with("(by an older Genko)"));
        genko::test::write_bytes(dir + "/project.lock",
                                 "{\"agent\": \"old genko\", \"acquired_at\": " + genko::core::py_float_repr(now - 16 * 60) + "}");
        lock.try_acquire();  // stale
        lock.release();
        genko::test::write_bytes(dir + "/project.lock", "{\"agent\": \"old genko\", \"released\": true, \"acquired_at\": " +
                                                            genko::core::py_float_repr(now) + "}");
        lock.try_acquire();  // released
        lock.release();
        genko::test::write_bytes(dir + "/project.lock", "not json");
        lock.try_acquire();  // unreadable content: only the OS lock counts
        lock.release();
    }

    void converterLockNeitherMakesNorWrites() {
        const fs::path dir = to_path(tmp_.path()) / "source.genko";
        fs::create_directories(dir);
        ProjectLock::Options options;
        options.create = false;
        options.write_content = false;
        ProjectLock absent(dir, "converter", options);
        try {
            absent.try_acquire();
            QFAIL("locked a file that does not exist");
        } catch (const genko::core::Error& error) {
            QCOMPARE(error.code(), std::string("not_found"));
        }
        QVERIFY(!fs::exists(dir / "project.lock"));
        const std::string content = "{\"released\": true, \"agent\": \"python\", \"released_at\": 1.5}";
        const QString file = QString::fromStdString(genko::storage::path_to_utf8(dir / "project.lock"));
        genko::test::write_bytes(file, content);
        fs::permissions(dir / "project.lock", fs::perms::owner_read | fs::perms::group_read);  // (read-only is enough)
        ProjectLock source(dir, "converter", options);
        source.try_acquire();
        QVERIFY(source.held());
        fs::permissions(dir / "project.lock", fs::perms::owner_read | fs::perms::owner_write);
        ProjectLock writer(dir, "writer");
        QCOMPARE(locked_message([&] { writer.try_acquire(); }),  // (the last holder's name)
                 "project locked: " + genko::storage::path_to_utf8(dir / "project.lock") + " (by " + genko::test::holder_seen("python") + ")");
        source.release();
        QCOMPARE(genko::test::read_bytes(file), content);
        writer.try_acquire();  // free again
        QVERIFY(writer.held());
    }
};

QTEST_GUILESS_MAIN(TestLock)
#include "test_lock.moc"
