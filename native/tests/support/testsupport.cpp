#include "testsupport.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#include <bit>
#include <cstdint>

#include "core/pyconv.hpp"

namespace genko::test {

namespace {

int kind_of(const core::Json& v) {
    switch (v.type()) {
        case core::Json::value_t::null: return 0;
        case core::Json::value_t::boolean: return 1;
        case core::Json::value_t::number_integer:
        case core::Json::value_t::number_unsigned: return 2;
        case core::Json::value_t::number_float: return 3;
        case core::Json::value_t::string: return 4;
        case core::Json::value_t::array: return 5;
        case core::Json::value_t::object: return 6;
        default: return 7;
    }
}

bool same_int(const core::Json& a, const core::Json& b) {
    if (a.is_number_unsigned() || b.is_number_unsigned()) {
        if (a.is_number_unsigned() && b.is_number_unsigned()) return a.get<std::uint64_t>() == b.get<std::uint64_t>();
        const core::Json& u = a.is_number_unsigned() ? a : b;
        const core::Json& s = a.is_number_unsigned() ? b : a;
        return s.get<std::int64_t>() >= 0 && static_cast<std::uint64_t>(s.get<std::int64_t>()) == u.get<std::uint64_t>();
    }
    return a.get<std::int64_t>() == b.get<std::int64_t>();
}

std::string brief(const core::Json& v) {
    std::string text;
    try {
        text = core::dump_python(v);
    } catch (...) {
        text = "<unprintable>";
    }
    if (text.size() > 300) text = text.substr(0, 300) + "…";
    return text;
}

bool compare(const core::Json& a, const core::Json& b, const std::string& at, std::string* where) {
    const auto differ = [&](const std::string& why) {
        if (where != nullptr) *where = (at.empty() ? "/" : at) + ": " + why + ": " + brief(a) + " != " + brief(b);
        return false;
    };
    if (kind_of(a) != kind_of(b)) return differ("types differ (" + core::py_type_name(a) + " vs " + core::py_type_name(b) + ")");
    switch (kind_of(a)) {
        case 0: return true;
        case 1: return a.get<bool>() == b.get<bool>() ? true : differ("values differ");
        case 2: return same_int(a, b) ? true : differ("values differ");
        case 3:
            return std::bit_cast<std::uint64_t>(a.get<double>()) == std::bit_cast<std::uint64_t>(b.get<double>())
                       ? true
                       : differ("floats differ");
        case 4: return a.get_ref<const std::string&>() == b.get_ref<const std::string&>() ? true : differ("strings differ");
        case 5: {
            if (a.size() != b.size()) return differ("lengths differ");
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (!compare(a[i], b[i], core::json_pointer_append(at, i), where)) return false;
            }
            return true;
        }
        case 6: {
            if (a.size() != b.size()) {
                std::string keys_a, keys_b;
                for (const auto& [k, v] : a.items()) keys_a += k + ",";
                for (const auto& [k, v] : b.items()) keys_b += k + ",";
                if (where != nullptr) *where = (at.empty() ? "/" : at) + ": keys differ: [" + keys_a + "] vs [" + keys_b + "]";
                return false;
            }
            auto ia = a.begin();
            auto ib = b.begin();
            for (; ia != a.end(); ++ia, ++ib) {
                if (ia.key() != ib.key()) {
                    if (where != nullptr) {
                        *where = (at.empty() ? "/" : at) + ": key order differs: " + ia.key() + " vs " + ib.key();
                    }
                    return false;
                }
                if (!compare(ia.value(), ib.value(), core::json_pointer_append(at, ia.key()), where)) return false;
            }
            return true;
        }
        default: return differ("unexpected value");
    }
}

}  // namespace

bool strict_equal(const core::Json& a, const core::Json& b, std::string* where) { return compare(a, b, "", where); }

QString repo_root() { return QString::fromUtf8(GENKO_REPO_ROOT); }

QString test_data(const QString& relative) {
    const QString root = QString::fromUtf8(GENKO_TEST_DATA);
    return relative.isEmpty() ? root : root + QLatin1Char('/') + relative;
}

QString genko_cli() { return QString::fromUtf8(GENKO_CLI); }

QString python_ref() {
    const QString configured = qEnvironmentVariable("GENKO_PYREF");
    if (!configured.isEmpty()) return QFileInfo(configured).isExecutable() ? configured : QString();
    const QString fallback = QStringLiteral("/opt/pyref/bin/python");
    return QFileInfo(fallback).isExecutable() ? fallback : QString();
}

Run run(const QString& program, const QStringList& args, const QProcessEnvironment& env, int timeout_ms) {
    QProcess process;
    process.setProcessEnvironment(env);
    process.start(program, args);
    Run result;
    result.started = process.waitForStarted(60000);
    if (!result.started) return result;
    process.closeWriteChannel();
    result.finished = process.waitForFinished(timeout_ms);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(5000);
    }
    result.exit_code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    result.out = process.readAllStandardOutput();
    result.err = process.readAllStandardError();
    return result;
}

QProcessEnvironment python_env(const QString& scratch) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONPATH"), repo_root() + QStringLiteral("/src"));
    env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONHASHSEED"), QStringLiteral("0"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("GENKO_CONFIG_DIR"), scratch + QStringLiteral("/config"));
    env.insert(QStringLiteral("XDG_CACHE_HOME"), scratch + QStringLiteral("/cache"));
    env.insert(QStringLiteral("MPLCONFIGDIR"), scratch + QStringLiteral("/cache"));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    QDir().mkpath(scratch + QStringLiteral("/config"));
    QDir().mkpath(scratch + QStringLiteral("/cache"));
    return env;
}

Run harness(const QStringList& args, const QString& scratch, int timeout_ms) {
    QStringList full{repo_root() + QStringLiteral("/tools/migration/pyref_harness.py")};
    full += args;
    return run(python_ref(), full, python_env(scratch), timeout_ms);
}

std::string read_bytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray data = file.readAll();
    return std::string(data.constData(), static_cast<std::size_t>(data.size()));
}

core::Json read_json(const QString& path) { return core::parse_python_json(read_bytes(path)); }

}  // namespace genko::test
