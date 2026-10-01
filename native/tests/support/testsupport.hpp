#pragma once

#include <QByteArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "core/json.hpp"
#include "core/model.hpp"

#ifndef GENKO_FAULT_INJECTION
#define GENKO_FAULT_INJECTION 0
#endif

// Helpers shared by the test executables (not part of the product).

namespace genko::test {

// project.json v4 and its assets written straight into `dir`: the writer's format, for the tests of the format
// (the product writes books only through storage::Saver). A document with a read_only_reason is refused.
void write_project(const core::Document& doc, const std::filesystem::path& dir);

// JSON values that are the same in everything Python's json.dumps can show: the same types (an int is not a float,
// a bool is not a number), the same numbers (floats bit for bit) and objects with the same keys in the same order.
// On a difference, `where` gets the JSON pointer and both values.
bool strict_equal(const core::Json& a, const core::Json& b, std::string* where = nullptr);
// The same, but the keys of objects in any order (a state is canonical JSON: its objects' keys are sorted).
bool same_content(const core::Json& a, const core::Json& b, std::string* where = nullptr);

QString repo_root();
QString test_data(const QString& relative = {});
// The genko executable of this build.
QString genko_cli();
// The genko_lock_helper executable of this build (holds a book's project lock in another process).
QString lock_helper();

// Whether this build injects faults (GENKO_FAULT, GENKO_TEST_MIGRATE_HOOK): Debug and ASan builds.
bool fault_injection();

// Who holds a lock, as another process reads it from project.lock: the agent written there — except on Windows,
// where the locked first byte cannot be read by anyone else, so it is "another process" (Python's lock.py does the
// same).
std::string holder_seen(const std::string& agent);

// The path of a book's project.lock as the messages write it (native separators).
std::string lock_file_text(const QString& book);

// The Python reference: $GENKO_PYREF, else /opt/pyref/bin/python; empty when neither exists.
QString python_ref();

struct Run {
    bool started = false;
    bool finished = false;
    int exit_code = -1;
    QByteArray out;
    QByteArray err;
};

Run run(const QString& program, const QStringList& args, const QProcessEnvironment& env, int timeout_ms = 600000,
        const QByteArray& input = {});
// The environment for the Python reference: PYTHONPATH=<repo>/src, no .pyc files, config and caches in `scratch`.
QProcessEnvironment python_env(const QString& scratch);
// tools/migration/pyref_harness.py <args> run by the Python reference.
Run harness(const QStringList& args, const QString& scratch, int timeout_ms = 600000);

// The genko command line of this build (GENKO_FAULT and GENKO_TEST_MIGRATE_HOOK from `env` only).
Run run_genko(const QStringList& args, const QProcessEnvironment& env = {}, const QByteArray& input = {});
// The environment for genko with GENKO_FAULT set to `fault` (empty: without it).
QProcessEnvironment fault_env(const QString& fault);
// The one JSON line a command printed (a string "not one line: …" when it printed something else).
core::Json one_line(const QByteArray& out);

// A process that holds something (a lock) until it is stopped: ready once it prints its first line.
class Holder {
public:
    Holder() = default;
    ~Holder();
    Holder(const Holder&) = delete;
    Holder& operator=(const Holder&) = delete;
    // Start it and wait for its first line, which is returned ("" when it printed nothing in time).
    QString start(const QString& program, const QStringList& args,
                  const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment());
    // Close its stdin and wait for it to end; what it printed after its first line.
    QString stop();

private:
    std::unique_ptr<QProcess> process_;
};

std::string read_bytes(const QString& path);
core::Json read_json(const QString& path);
void write_bytes(const QString& path, const std::string& bytes);

// Every file under `dir`: its path relative to `dir` ("/" between folders) → the sha256 of its bytes.
std::map<std::string, std::string> tree_hashes(const QString& dir, bool with_lock_file = true);
void copy_tree(const QString& from, const QString& to);

// What is wrong with a v4 book, read straight from its files (not through storage::journal): every prepared
// transaction committed or aborted exactly once, revisions going up, project.json the state, revision and bytes of
// the last commit, each approval change in studio/audit.jsonl exactly once, the states of the commits in assets/.
// "" when nothing is.
std::string book_problems(const QString& dir);

}  // namespace genko::test
