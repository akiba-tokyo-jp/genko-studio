#include "app/ai_link.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <system_error>

#include "app/dialogs.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "studio/files.hpp"

namespace genko::app::ai_link {

using core::Json;

namespace {

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

std::optional<QStringList>& command_override() {
    static std::optional<QStringList> base;
    return base;
}

std::optional<Probe>& probe_store() {
    static std::optional<Probe> probe;
    return probe;
}

// _quote: a part with a space, a tab or a quote in double quotes.
QString quoted(const QString& part) {
    if (part.contains(QLatin1Char(' ')) || part.contains(QLatin1Char('\t')) || part.contains(QLatin1Char('"'))) {
        return QStringLiteral("\"%1\"").arg(part);
    }
    return part;
}

QString joined(const QStringList& parts) {
    QStringList out;
    for (const QString& part : parts) out << quoted(part);
    return out.join(QLatin1Char(' '));
}

Json json_strings(const QStringList& parts) {
    Json out = Json::array();
    for (const QString& part : parts) out.push_back(part.toStdString());
    return out;
}

double now_seconds() { return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0; }

// Python's round(x) of a float: the nearest whole number, ties to even.
std::int64_t py_round_int(double x) { return static_cast<std::int64_t>(std::nearbyint(x)); }

// The last `limit` bytes of a file (all of it when shorter); nothing when it cannot be read.
std::optional<QByteArray> tail(const fs::path& path, qint64 limit) {
    QFile file(qpath(path));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    const qint64 size = file.size();
    if (size > limit) file.seek(size - limit);
    return file.read(limit);
}

std::optional<ToolCall> call_from(const QByteArray& line, const QString& actor) {
    try {
        core::ParseOptions options;
        const Json entry = core::parse_python_json(line.toStdString(), nullptr, options);
        if (!entry.is_object()) return std::nullopt;
        ToolCall call;
        call.actor = QString::fromStdString(entry.contains("actor") ? core::py_str(entry["actor"]) : std::string());
        if (!actor.isEmpty() && call.actor != actor) return std::nullopt;
        call.tool = QString::fromStdString(entry.contains("tool") ? core::py_str(entry["tool"]) : std::string());
        call.at = entry.contains("at") && entry["at"].is_number() ? core::py_float(entry["at"]) : 0.0;
        call.ok = !entry.contains("ok") || core::py_truthy(entry["ok"]);
        if (entry.contains("error") && core::py_truthy(entry["error"])) {
            call.error = QString::fromStdString(core::py_str(entry["error"]));
        } else if (entry.contains("codes") && entry["codes"].is_array()) {
            QStringList codes;
            for (const Json& code : entry["codes"]) codes << QString::fromStdString(core::py_str(code));
            call.error = codes.join(QStringLiteral(", "));
        }
        return call;
    } catch (const core::Error&) {
        return std::nullopt;
    }
}

}  // namespace

const std::vector<Client>& clients() {
    static const std::vector<Client> list = {
        {"claude-desktop", "Claude Desktop"},
        {"claude-code", "Claude Code"},
        {"hermes", "Hermes Agent"},
        {"other", "そのほかの MCP クライアント"},
    };
    return list;
}

QString agent_of(const QString& client) {
    return QStringLiteral("ai:%1").arg(client == QLatin1String("other") ? QStringLiteral("agent") : client);
}

QStringList command() {
    if (const auto& base = command_override()) return *base;
    return {QCoreApplication::applicationFilePath()};
}

void set_command_for_tests(const std::optional<QStringList>& base) { command_override() = base; }

QString root_text(const QString& typed) {
    QString text = QString::fromStdString(core::py_strip(typed.toStdString()));
    if (text.isEmpty()) text = QStringLiteral(".");
#ifdef _WIN32
    text.replace(QLatin1Char('/'), QLatin1Char('\\'));
    const QChar sep = QLatin1Char('\\');
#else
    const QChar sep = QLatin1Char('/');
#endif
    QString root;
#ifdef _WIN32
    if (text.size() >= 2 && text[1] == QLatin1Char(':')) {  // (a drive: C: or C:\)
        root = text.left(2);
        text = text.mid(2);
    }
    if (text.startsWith(QStringLiteral("\\\\"))) {  // (a share: \\server\share)
        root += QStringLiteral("\\\\");
        text = text.mid(2);
    } else if (text.startsWith(sep)) {
        root += sep;
    }
#else
    if (text.startsWith(QStringLiteral("//")) && !text.startsWith(QStringLiteral("///"))) {
        root = QStringLiteral("//");  // (POSIX keeps exactly two leading slashes)
    } else if (text.startsWith(sep)) {
        root = sep;
    }
#endif
    QStringList parts;
    for (const QString& part : text.split(sep, Qt::SkipEmptyParts)) {
        if (part != QLatin1String(".")) parts << part;
    }
    const QString out = root + parts.join(sep);
    return out.isEmpty() ? QStringLiteral(".") : out;
}

Snippet snippet(const QString& client, const QString& root) {
    const QStringList base = command();
    QStringList args = base.mid(1);
    args << QStringLiteral("mcp") << QStringLiteral("--root") << root << QStringLiteral("--agent") << agent_of(client);
    Snippet out;
    out.server = QStringList{base.value(0)} + args;
    QStringList line = base;
    line << args.mid(static_cast<int>(base.size()) - 1);
    if (client == QLatin1String("claude-desktop")) {
        const Json config = {{"mcpServers", {{"genko", {{"command", base.value(0).toStdString()}, {"args", json_strings(args)}}}}}};
        out.where = QStringLiteral("Claude Desktop の「設定 → 開発者 → 設定を編集」で開くファイル（claude_desktop_config.json）に足します");
        out.text = QString::fromStdString(core::dump_python_indent2(config));
        return out;
    }
    if (client == QLatin1String("claude-code")) {
        out.where = QStringLiteral("ターミナルで 1 回だけ実行します（Claude Code に genko を登録します）");
        out.text = QStringLiteral("claude mcp add genko -- %1").arg(joined(line));
        return out;
    }
    if (client == QLatin1String("hermes")) {
        out.where = QStringLiteral("Hermes Agent の設定ファイル（~/.hermes/config.yaml）に足します");
        out.text = QStringLiteral("mcp_servers:\n  genko:\n    command: %1\n    args: %2\n    timeout: 60\n")
                       .arg(QString::fromStdString(core::dump_python(Json(base.value(0).toStdString()))),
                            QString::fromStdString(core::dump_python(json_strings(args))));
        return out;
    }
    out.where = QStringLiteral("MCP クライアントに stdio のサーバーとして、次のコマンドを登録します");
    out.text = joined(line);
    return out;
}

QString first_request(const std::optional<fs::path>& book, const QString& session) {
    QString name = QStringLiteral("新しい原稿");
    if (book) {  // (Path(book).name: a trailing separator does not count)
        const fs::path last = book->has_filename() ? book->filename() : book->parent_path().filename();
        name = QString::fromStdString(core::path_to_utf8(last));
    }
    QString text = QStringLiteral("Genko の「%1」で、4 ページの短い漫画を作ってください。まず genko://guide/skill を読んで手順に従い、"
                                  "企画書と脚本を作ったら私に見せてください。決めることは承認箱で聞いてください。")
                       .arg(name);
    const QString chosen = QString::fromStdString(core::py_strip(session.toStdString()));
    if (!chosen.isEmpty()) {
        text += QStringLiteral("この会話では、どの道具にも session に「%1」を渡してください。").arg(chosen);
    }
    return text;
}

std::vector<RecentAi> recent_ai(const std::optional<fs::path>& book, std::optional<double> now) {
    std::vector<RecentAi> out;
    if (!book) return out;
    const double t = now.value_or(now_seconds());
    for (const auto& [name, at] : studio::presence_actors(*book)) {
        if (name.rfind("ai:", 0) != 0) continue;
        out.push_back(RecentAi{name, std::max<std::int64_t>(0, py_round_int((t - at) / 60)), t - at < kRecentSeconds});
    }
    std::stable_sort(out.begin(), out.end(), [](const RecentAi& a, const RecentAi& b) { return a.minutes_ago < b.minutes_ago; });
    return out;
}

QString ai_name(const QString& actor) {
    const QString name = actor.startsWith(QLatin1String("ai:")) ? actor.mid(3) : actor;
    const qsizetype slash = name.indexOf(QLatin1Char('/'));
    if (slash < 0) return name;
    const QString base = name.left(slash);
    const QString session = name.mid(slash + 1);
    return session.isEmpty() ? base : QStringLiteral("%1（%2）").arg(base, session);
}

QString ago(std::int64_t minutes) {
    if (minutes < 60) return QStringLiteral("%1 分前").arg(minutes);
    if (minutes < 60 * 48) return QStringLiteral("%1 時間前").arg(minutes / 60);
    return QStringLiteral("%1 日前").arg(minutes / (60 * 24));
}

std::pair<QString, bool> status_words(const std::optional<fs::path>& book, std::optional<double> now) {
    const auto found = recent_ai(book, now);
    if (found.empty()) return {QStringLiteral("AI と作る"), false};
    const RecentAi& top = found.front();
    const QString when = top.working ? QStringLiteral("作業中") : ago(top.minutes_ago);
    const QString more = found.size() > 1 ? QStringLiteral(" ほか %1").arg(found.size() - 1) : QString();
    return {QStringLiteral("AI: %1 %2%3").arg(ai_name(QString::fromStdString(top.actor)), when, more), top.working};
}

fs::path default_root(const std::optional<fs::path>& book) {
    const auto resolved_parent = [](const fs::path& path) {
        std::error_code error;
        fs::path full = fs::weakly_canonical(fs::absolute(path, error), error);
        if (error) full = fs::absolute(path, error);
        return full.parent_path();
    };
    if (book) return resolved_parent(*book);
    const auto recent = recent_projects();
    if (!recent.empty()) return resolved_parent(recent.front());
    return core::path_from_utf8(QDir::homePath().toStdString());
}

QStringList conversations(const std::optional<fs::path>& book, const QString& client) {
    QStringList out;
    if (!book) return out;
    const std::string prefix = agent_of(client).toStdString() + "/";
    auto actors = studio::presence_actors(*book);
    std::stable_sort(actors.begin(), actors.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [name, at] : actors) {
        (void)at;
        if (name.rfind(prefix, 0) == 0 && name.size() > prefix.size()) out << QString::fromStdString(name.substr(prefix.size()));
    }
    out.removeDuplicates();
    return out;
}

QString session_problem(const QString& session) {
    try {
        studio::actor_for("ai:agent", session.toStdString());
        return {};
    } catch (const core::Error& error) {
        return QString::fromUtf8(error.what());
    }
}

QString link_words(Link link) {
    switch (link) {
        case Link::Unset: return QStringLiteral("未設定");
        case Link::Configured: return QStringLiteral("設定済み・未接続");
        case Link::Connected: return QStringLiteral("接続済み");
        case Link::Working: return QStringLiteral("処理中");
        case Link::Waiting: return QStringLiteral("人の確認待ち");
        case Link::Failed: return QStringLiteral("失敗");
    }
    return {};
}

std::optional<ToolCall> last_call(const fs::path& book, const QString& actor) {
    for (const char* name : {"tools.jsonl", "tools.1.jsonl"}) {
        const auto bytes = tail(book / "studio" / "logs" / name, 256 * 1024);
        if (!bytes) continue;
        QList<QByteArray> lines = bytes->split('\n');  // (only "\n" ends a line: SPEC COMP-01a, M5①)
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (it->trimmed().isEmpty()) continue;
            if (auto call = call_from(*it, actor)) return call;
        }
    }
    return std::nullopt;
}

int running_jobs(const fs::path& book, std::optional<double> now) {
    const double t = now.value_or(now_seconds());
    const QDir folder(qpath(book / "studio" / "jobs"));
    int count = 0;
    for (const QString& name : folder.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile file(folder.filePath(name));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) continue;
        try {
            const Json record = core::parse_python_json(file.readAll().toStdString());
            if (!record.is_object() || !(record.value("status", Json()) == Json("running"))) continue;
            const double started = record.contains("started") && record["started"].is_number() ? core::py_float(record["started"]) : 0.0;
            const double beat = record.contains("beat") && record["beat"].is_number() ? core::py_float(record["beat"]) : started;
            // (jobs.status: a job silent past STALE_S, or older than LOST_S, died with its server)
            if (t - started > 6 * 3600 || (record.contains("beat") && t - beat > 90.0)) continue;
            ++count;
        } catch (const core::Error&) {
        }
    }
    return count;
}

Link link_state(const Facts& facts) {
    // the newer of the test and the last call says whether it failed
    std::optional<bool> latest_ok;
    double latest_at = -1;
    if (facts.probe) {
        latest_ok = facts.probe->ok;
        latest_at = static_cast<double>(facts.probe->at.toMSecsSinceEpoch()) / 1000.0;
    }
    if (facts.call && facts.call->at >= latest_at) latest_ok = facts.call->ok;
    if (latest_ok && !*latest_ok) return Link::Failed;
    if (facts.waiting > 0) return Link::Waiting;
    if (facts.working) return Link::Working;
    if ((facts.probe && facts.probe->ok) || (facts.call && facts.call->ok)) return Link::Connected;
    if (facts.configured) return Link::Configured;
    return Link::Unset;
}

const std::optional<Probe>& last_probe() { return probe_store(); }

void set_last_probe(const Probe& probe) { probe_store() = probe; }

}  // namespace genko::app::ai_link
