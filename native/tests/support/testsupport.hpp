#pragma once

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <string>

#include "core/json.hpp"

// Helpers shared by the test executables (not part of the product).

namespace genko::test {

// JSON values that are the same in everything Python's json.dumps can show: the same types (an int is not a float,
// a bool is not a number), the same numbers (floats bit for bit) and objects with the same keys in the same order.
// On a difference, `where` gets the JSON pointer and both values.
bool strict_equal(const core::Json& a, const core::Json& b, std::string* where = nullptr);

QString repo_root();
QString test_data(const QString& relative = {});
// The genko executable of this build.
QString genko_cli();

// The Python reference: $GENKO_PYREF, else /opt/pyref/bin/python; empty when neither exists.
QString python_ref();

struct Run {
    bool started = false;
    bool finished = false;
    int exit_code = -1;
    QByteArray out;
    QByteArray err;
};

Run run(const QString& program, const QStringList& args, const QProcessEnvironment& env, int timeout_ms = 600000);
// The environment for the Python reference: PYTHONPATH=<repo>/src, no .pyc files, config and caches in `scratch`.
QProcessEnvironment python_env(const QString& scratch);
// tools/migration/pyref_harness.py <args> run by the Python reference.
Run harness(const QStringList& args, const QString& scratch, int timeout_ms = 600000);

std::string read_bytes(const QString& path);
core::Json read_json(const QString& path);

}  // namespace genko::test
