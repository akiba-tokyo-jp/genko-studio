#include "testsupport.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#include <bit>
#include <cstdint>
#include <vector>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/writer.hpp"

namespace genko::test {

void write_project(const core::Document& doc, const std::filesystem::path& dir) {
    if (!doc.read_only_reason.empty()) throw core::Error("read_only", doc.read_only_reason);
    storage::AssetStore store(dir);
    storage::write_atomic(dir / "project.json", storage::project_json_v4(doc, store));
}

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

QString lock_helper() { return QString::fromUtf8(GENKO_LOCK_HELPER); }

bool fault_injection() { return GENKO_FAULT_INJECTION != 0; }

std::string holder_seen(const std::string& agent) {
#ifdef Q_OS_WIN
    (void)agent;
    return "another process";
#else
    return agent;
#endif
}

std::string lock_file_text(const QString& book) {
    return storage::path_to_utf8(storage::path_from_utf8(book.toStdString()) / "project.lock");
}

bool same_content(const core::Json& a, const core::Json& b, std::string* where) {
    const std::string x = core::dump_canonical(a);
    const std::string y = core::dump_canonical(b);
    if (x == y) return true;
    if (where != nullptr) {
        std::size_t at = 0;
        while (at < x.size() && at < y.size() && x[at] == y[at]) ++at;
        const std::size_t from = at > 120 ? at - 120 : 0;
        *where = "differ at byte " + std::to_string(at) + ": …" + x.substr(from, 240) + " != …" + y.substr(from, 240);
    }
    return false;
}

QString python_ref() {
    const QString configured = qEnvironmentVariable("GENKO_PYREF");
    if (!configured.isEmpty()) return QFileInfo(configured).isExecutable() ? configured : QString();
    const QString fallback = QStringLiteral("/opt/pyref/bin/python");
    return QFileInfo(fallback).isExecutable() ? fallback : QString();
}

Run run(const QString& program, const QStringList& args, const QProcessEnvironment& env, int timeout_ms,
        const QByteArray& input) {
    QProcess process;
    process.setProcessEnvironment(env);
    process.start(program, args);
    Run result;
    result.started = process.waitForStarted(60000);
    if (!result.started) return result;
    if (!input.isEmpty()) process.write(input);
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

Run run_genko(const QStringList& args, const QProcessEnvironment& env, const QByteArray& input) {
    QProcessEnvironment full = QProcessEnvironment::systemEnvironment();
    full.remove(QStringLiteral("GENKO_FAULT"));
    full.remove(QStringLiteral("GENKO_TEST_MIGRATE_HOOK"));
    for (const QString& key : env.keys()) full.insert(key, env.value(key));
    return run(genko_cli(), args, full, 300000, input);
}

QProcessEnvironment fault_env(const QString& fault) {
    QProcessEnvironment env;
    if (!fault.isEmpty()) env.insert(QStringLiteral("GENKO_FAULT"), fault);
    return env;
}

core::Json one_line(const QByteArray& out) {
    const std::string text = out.toStdString();
    if (text.empty() || text.back() != '\n' || text.find('\n') != text.size() - 1) return core::Json("not one line: " + text);
    try {
        return core::parse_python_json(text);
    } catch (const core::Error& error) {
        return core::Json(std::string("not JSON: ") + error.what() + ": " + text);
    }
}

Holder::~Holder() { stop(); }

QString Holder::start(const QString& program, const QStringList& args, const QProcessEnvironment& env) {
    stop();
    process_ = std::make_unique<QProcess>();
    process_->setProcessEnvironment(env);
    process_->start(program, args);
    if (!process_->waitForStarted(60000)) return {};
    QByteArray line;
    for (int i = 0; i < 600 && !line.contains('\n'); ++i) {
        if (!process_->waitForReadyRead(100) && process_->state() != QProcess::Running) {
            line += process_->readAllStandardOutput();
            break;
        }
        line += process_->readAllStandardOutput();
    }
    const qsizetype end = line.indexOf('\n');
    QByteArray first = end < 0 ? line : line.left(end);
    if (first.endsWith('\r')) first.chop(1);  // (Windows' text-mode stdout)
    return QString::fromUtf8(first);
}

QString Holder::stop() {
    if (!process_) return {};
    process_->closeWriteChannel();
    if (!process_->waitForFinished(30000)) {
        process_->kill();
        process_->waitForFinished(5000);
    }
    const QString rest = QString::fromUtf8(process_->readAllStandardOutput());
    process_.reset();
    return rest;
}

std::string read_bytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray data = file.readAll();
    return std::string(data.constData(), static_cast<std::size_t>(data.size()));
}

core::Json read_json(const QString& path) { return core::parse_python_json(read_bytes(path)); }

void write_bytes(const QString& path, const std::string& bytes) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(bytes.data(), static_cast<qint64>(bytes.size()));
}

std::map<std::string, std::string> tree_hashes(const QString& dir, bool with_lock_file) {
    std::map<std::string, std::string> out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString rel = QDir(dir).relativeFilePath(file);
        if (!with_lock_file && rel == QLatin1String("project.lock")) continue;
        out[rel.toStdString()] = QCryptographicHash::hash(QByteArray::fromStdString(read_bytes(file)), QCryptographicHash::Sha256)
                                     .toHex()
                                     .toStdString();
    }
    return out;
}

void copy_tree(const QString& from, const QString& to) {
    QDir().mkpath(to);
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString target = to + QLatin1Char('/') + QDir(from).relativeFilePath(path);
        if (QFileInfo(path).isDir()) {
            QDir().mkpath(target);
        } else {
            QDir().mkpath(QFileInfo(target).path());
            QFile::copy(path, target);
        }
    }
}

std::string book_problems(const QString& dir) {
    std::vector<std::string> problems;
    struct Attempt {
        core::Json prepare;
        std::string txn;
        int commits = 0;
        int aborts = 0;
    };
    std::vector<Attempt> attempts;
    const std::string journal = read_bytes(dir + "/studio/journal.jsonl");
    if (!journal.empty() && journal.back() != '\n') problems.push_back("the journal ends with a cut line");
    std::size_t start = 0;
    std::size_t number = 0;
    while (start < journal.size()) {
        std::size_t end = journal.find('\n', start);
        if (end == std::string::npos) end = journal.size();
        const std::string line = journal.substr(start, end - start);
        start = end + 1;
        ++number;
        if (line.empty()) continue;
        core::Json value;
        try {
            value = core::parse_python_json(line);
        } catch (const core::Error&) {
            problems.push_back("journal line " + std::to_string(number) + " is not JSON");
            continue;
        }
        if (value.value("v", 0) != 4) {
            problems.push_back("journal line " + std::to_string(number) + " is not a v4 line");
            continue;
        }
        const std::string kind = value.value("kind", "");
        const std::string txn = value.value("txn", "");
        if (kind == "prepare") {
            attempts.push_back(Attempt{value, txn});
            continue;
        }
        Attempt* open = nullptr;
        for (auto it = attempts.rbegin(); it != attempts.rend(); ++it) {
            if (it->txn == txn && it->commits == 0 && it->aborts == 0) {
                open = &*it;
                break;
            }
        }
        if (open == nullptr) {
            problems.push_back("journal line " + std::to_string(number) + ": a " + kind + " of " + txn + " that is not pending");
            continue;
        }
        if (kind == "commit") {
            ++open->commits;
            if (value.value("rev", -1) != open->prepare.value("rev", -2)) problems.push_back("commit rev differs from prepare: " + txn);
        } else if (kind == "abort") {
            ++open->aborts;
        } else {
            problems.push_back("journal line " + std::to_string(number) + " has kind " + kind);
        }
    }
    std::int64_t last_rev = 0;
    const Attempt* last = nullptr;
    std::map<std::string, int> committed_txns;
    for (const Attempt& a : attempts) {
        if (a.commits + a.aborts == 0) problems.push_back("transaction " + a.txn + " is pending");
        if (a.commits == 0) continue;
        if (++committed_txns[a.txn] > 1) problems.push_back("transaction " + a.txn + " committed twice");
        const std::int64_t rev = a.prepare.value("rev", std::int64_t{0});
        if (rev <= last_rev) problems.push_back("revision " + std::to_string(rev) + " after " + std::to_string(last_rev));
        last_rev = rev;
        last = &a;
        const std::string after = a.prepare.value("after", "");
        if (after.size() == 71 && !QFileInfo::exists(dir + "/assets/" + QString::fromStdString(after.substr(7, 2)) + "/" +
                                                   QString::fromStdString(after.substr(7)) + ".state.json")) {
            problems.push_back("the state of " + a.txn + " is missing");
        }
    }
    const std::string project = read_bytes(dir + "/project.json");
    if (last != nullptr) {
        const std::string sha = QCryptographicHash::hash(QByteArray::fromStdString(project), QCryptographicHash::Sha256).toHex().toStdString();
        if (sha != last->prepare.value("project_sha256", "")) problems.push_back("project.json is not the bytes of the last commit");
        try {
            core::Json payload = core::parse_python_json(project);
            if (payload.value("revision", std::int64_t{-1}) != last_rev) problems.push_back("project.json's revision is not the last commit's");
            payload.erase("revision");
            payload.erase("writer");
            const std::string state = "sha256:" + QCryptographicHash::hash(QByteArray::fromStdString(core::dump_canonical(payload)),
                                                                           QCryptographicHash::Sha256).toHex().toStdString();
            if (state != last->prepare.value("after", "")) problems.push_back("project.json is not the state of the last commit");
        } catch (const core::Error&) {
            problems.push_back("project.json is not JSON");
        }
    }
    std::map<std::string, std::vector<core::Json>> audit_by_txn;
    const std::string audit = read_bytes(dir + "/studio/audit.jsonl");
    if (!audit.empty() && audit.back() != '\n') problems.push_back("the audit ends with a cut line");
    start = 0;
    while (start < audit.size()) {
        std::size_t end = audit.find('\n', start);
        if (end == std::string::npos) end = audit.size();
        const std::string line = audit.substr(start, end - start);
        start = end + 1;
        if (line.empty()) continue;
        try {
            const core::Json value = core::parse_python_json(line);
            if (value.value("v", 0) == 4 && value.contains("txn")) audit_by_txn[value["txn"].get<std::string>()].push_back(value);
        } catch (const core::Error&) {
            problems.push_back("an audit line is not JSON");
        }
    }
    for (const auto& [txn, lines] : audit_by_txn) {
        if (lines.size() > 1) problems.push_back("the audit records " + txn + " " + std::to_string(lines.size()) + " times");
    }
    for (const Attempt& a : attempts) {
        const auto it = audit_by_txn.find(a.txn);
        const bool changes = a.prepare.contains("audit");
        if (a.commits > 0 && changes) {
            if (it == audit_by_txn.end()) {
                problems.push_back("the approval changes of " + a.txn + " are not in the audit");
            } else if (!(it->second.front()["changes"] == a.prepare["audit"])) {
                problems.push_back("the audit line of " + a.txn + " differs from its prepare");
            }
        }
        if (a.commits == 0 && it != audit_by_txn.end() && committed_txns.find(a.txn) == committed_txns.end()) {
            problems.push_back("the audit records " + a.txn + ", which was aborted");
        }
    }
    std::string out;
    for (const auto& problem : problems) out += (out.empty() ? "" : "; ") + problem;
    return out;
}

}  // namespace genko::test
