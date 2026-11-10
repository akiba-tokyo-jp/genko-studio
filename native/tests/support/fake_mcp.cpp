// A stand-in for `genko mcp` in the app's tests (not part of the product): the AI dialog's test connection and the
// first-setup run start it exactly as a client's settings start Genko's server — `<this> --mode M mcp --root R --agent
// ai:…` — and talk MCP to it over stdio (one JSON-RPC message a line). Nothing reaches a network.
//
//   --mode ok       answers initialize and tools/list; tools/call runs the studio's tool (StudioService of this build, as
//                   the agent and its conversation: session) and records the call (studio/logs/tools.jsonl) and the
//                   writer (studio/presence.json) as `genko mcp` does
//   --mode fail     says why on stderr and ends with 3 before reading anything
//   --mode hang     reads and never answers
//   --mode garbage  answers its first message with words that are not JSON
//   --mode reject   answers initialize with an error
// $GENKO_FAKE_MCP_LOG, when set: each method received is added to that file, one a line.

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>

#include <cstdio>
#include <iostream>
#include <set>
#include <string>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "service/studio_service.hpp"
#include "studio/files.hpp"

using genko::core::Json;

namespace {

void say(const Json& message) {
    const std::string line = genko::core::dump_python(message) + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

void log_method(const std::string& method) {
    const QByteArray where = qgetenv("GENKO_FAKE_MCP_LOG");
    if (where.isEmpty()) return;
    QFile file(QString::fromUtf8(where));
    if (file.open(QIODevice::Append)) file.write(QByteArray::fromStdString(method + "\n"));
}

// Python's READ_ONLY tools of genko mcp: the others write (their caller is recorded as using the book).
bool read_only(const std::string& tool) {
    static const std::set<std::string> names = {"projects", "status", "next", "inspect", "render", "candidates", "preflight", "check",
                                                "tickets", "proposals", "review_page", "export", "export_proof", "export_status", "style_catalog"};
    return names.count(tool) != 0;
}

Json call_tool(const std::string& root, const std::string& agent, const Json& params) {
    const std::string name = params.value("name", std::string());
    Json args = params.value("arguments", Json::object());
    std::optional<std::string> session;
    if (args.contains("session")) {
        if (args["session"].is_string()) session = args["session"].get<std::string>();
        args.erase("session");
    }
    QElapsedTimer clock;
    clock.start();
    std::string who;
    try {
        who = genko::studio::actor_for(agent, session);
    } catch (const genko::core::Error& error) {
        return Json{{"content", Json::array({Json{{"type", "text"}, {"text", genko::core::dump_python(Json{{"ok", false}, {"error", error.what()}})}}})}, {"isError", true}};
    }
    const std::filesystem::path base = genko::core::path_from_utf8(root);
    genko::service::StudioService service(base, who);
    std::optional<std::filesystem::path> path;
    if (args.contains("project")) {
        try {
            path = service.project_path(args["project"]);
        } catch (const genko::core::Error&) {
            path.reset();
        }
    }
    Json reply;
    std::optional<std::string> error;
    try {
        reply = service.call(name, Json::array(), args).to_dict();
    } catch (const std::exception& failed) {
        error = failed.what();
    }
    const double now = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
    if (path) {
        genko::studio::record_tool(*path, who, name, args, error ? std::nullopt : std::optional<Json>(reply), error,
                                   genko::core::Num(static_cast<double>(clock.nsecsElapsed()) / 1e6), now);
        const bool writes = !read_only(name) && !(args.contains("commit") && args["commit"] == Json(false));
        if (writes && !error && reply.value("ok", false)) genko::studio::touch(*path, who, now);
    }
    if (error) reply = Json{{"ok", false}, {"error", *error}};
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", genko::core::dump_python(reply)}}})}, {"isError", !reply.value("ok", false)}};
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = QCoreApplication::arguments().mid(1);
    QString mode = QStringLiteral("ok");
    std::string root = ".";
    std::string agent = "ai:agent";
    for (int i = 0; i < arguments.size(); ++i) {
        if (arguments[i] == QLatin1String("--mode") && i + 1 < arguments.size()) mode = arguments[++i];
        if (arguments[i] == QLatin1String("--root") && i + 1 < arguments.size()) root = arguments[++i].toStdString();
        if (arguments[i] == QLatin1String("--agent") && i + 1 < arguments.size()) agent = arguments[++i].toStdString();
    }
    if (mode == QLatin1String("fail")) {
        std::fputs("genko mcp: the root folder cannot be read (fake)\n", stderr);
        return 3;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        Json message;
        try {
            message = genko::core::parse_python_json(line);
        } catch (const genko::core::Error&) {
            continue;
        }
        const std::string method = message.value("method", std::string());
        log_method(method);
        if (mode == QLatin1String("hang")) continue;
        if (mode == QLatin1String("garbage")) {
            std::fputs("this is not json\n", stdout);
            std::fflush(stdout);
            continue;
        }
        if (!message.contains("id")) continue;  // (a notice: nothing to answer)
        const Json id = message["id"];
        if (method == "initialize") {
            if (mode == QLatin1String("reject")) {
                say(Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json{{"code", -32602}, {"message", "unsupported protocol version (fake)"}}}});
                continue;
            }
            const Json asked = message.value("params", Json::object()).value("protocolVersion", Json("2025-06-18"));
            say(Json{{"jsonrpc", "2.0"},
                     {"id", id},
                     {"result", Json{{"protocolVersion", asked}, {"capabilities", Json{{"tools", Json::object()}}}, {"serverInfo", Json{{"name", "genko"}, {"version", "fake"}}}}}});
        } else if (method == "tools/list") {
            Json tools = Json::array();
            for (const std::string& name : genko::service::agent_tools_sorted()) {
                tools.push_back(Json{{"name", name}, {"inputSchema", Json{{"type", "object"}}}});
            }
            say(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json{{"tools", tools}}}});
        } else if (method == "tools/call") {
            say(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", call_tool(root, agent, message.value("params", Json::object()))}});
        } else if (method == "ping") {
            say(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json::object()}});
        } else {
            say(Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json{{"code", -32601}, {"message", "method not found"}}}});
        }
    }
    return 0;
}
