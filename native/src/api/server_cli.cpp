#include "api/server_cli.hpp"

#include <QByteArray>
#include <QFile>
#include <QLocalSocket>

#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "api/headless_server.hpp"
#include "api/mcp_server.hpp"
#include "api/mcp_transport.hpp"
#include "api/tokens.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "service/control.hpp"

namespace genko::api {

namespace {

namespace fs = std::filesystem;
using core::Json;

void write_text(const std::string& text, std::FILE* to) {
    std::fwrite(text.data(), 1, text.size(), to);
    std::fflush(to);
}

int usage(const std::string& message) {
    write_text(core::dump_python(Json::object({{"ok", false}, {"error", message}, {"code", "usage"}})) + "\n", stderr);
    return 2;
}

struct Spec {
    std::string name;  // "--root"
    bool takes_value = true;
    bool append = false;
};

struct Parsed {
    std::map<std::string, std::vector<std::string>> values;
    std::vector<std::string> positional;

    std::optional<std::string> value(const std::string& name) const {
        const auto found = values.find(name);
        if (found == values.end() || found->second.empty()) return std::nullopt;
        return found->second.back();
    }
};

// argparse's rules for these commands: "--name value" or "--name=value", unique prefixes, repeated options appended.
std::optional<std::string> parse(const QStringList& args, const std::vector<Spec>& specs, Parsed& out) {
    for (qsizetype i = 0; i < args.size(); ++i) {
        const std::string arg = args[i].toStdString();
        if (!arg.starts_with("--") || arg == "--") {
            if (arg.starts_with("-") && arg != "-") return "unrecognized arguments: " + arg;
            out.positional.push_back(arg);
            continue;
        }
        const std::size_t eq = arg.find('=');
        const std::string given = arg.substr(0, eq);
        const Spec* match = nullptr;
        std::vector<const Spec*> prefixed;
        for (const Spec& spec : specs) {
            if (spec.name == given) match = &spec;
            if (spec.name.starts_with(given)) prefixed.push_back(&spec);
        }
        if (match == nullptr) {
            if (prefixed.size() == 1) {
                match = prefixed.front();
            } else if (prefixed.size() > 1) {
                std::string could;
                for (const Spec* s : prefixed) could += (could.empty() ? "" : ", ") + s->name;
                return "ambiguous option: " + given + " could match " + could;
            } else {
                return "unrecognized arguments: " + arg;
            }
        }
        std::string value;
        if (eq != std::string::npos) {
            value = arg.substr(eq + 1);
        } else {
            if (i + 1 >= args.size() || (args[i + 1].startsWith(QLatin1String("-")) && args[i + 1] != QLatin1String("-"))) {
                return "argument " + match->name + ": expected one argument";
            }
            value = args[++i].toStdString();
        }
        if (match->append) {
            out.values[match->name].push_back(value);
        } else {
            out.values[match->name] = {value};
        }
    }
    return std::nullopt;
}

std::optional<std::uint16_t> port_of(const std::string& text, std::string& error, const std::string& prog) {
    std::int64_t port = 0;
    try {
        port = core::py_int(Json(text));
    } catch (const core::Error&) {
        error = prog + ": error: argument --port: invalid int value: " + core::py_repr_str(text);
        return std::nullopt;
    }
    if (port < 0 || port > 65535) {
        error = prog + ": error: argument --port: " + text + " is not a port (0 to 65535)";
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(port);
}

}  // namespace

int mcp_command(const QStringList& args) {
    Parsed parsed;
    const std::vector<Spec> specs = {{"--root"}, {"--agent"}, {"--transport"}, {"--host"}, {"--port"}};
    if (auto error = parse(args, specs, parsed)) return usage("genko mcp: error: " + *error);
    if (!parsed.positional.empty()) return usage("genko mcp: error: unrecognized arguments: " + parsed.positional.front());
    const auto root_text = parsed.value("--root");
    if (!root_text) return usage("genko mcp: error: the following arguments are required: --root");
    const std::string agent = parsed.value("--agent").value_or("ai:agent");
    const std::string transport = parsed.value("--transport").value_or("stdio");
    if (transport != "stdio" && transport != "streamable-http") {
        return usage("genko mcp: error: argument --transport: invalid choice: " + core::py_repr_str(transport) +
                     " (choose from 'stdio', 'streamable-http')");
    }
    mcp::HttpOptions http;
    if (const auto host = parsed.value("--host")) http.host = *host;
    if (const auto port = parsed.value("--port")) {
        std::string error;
        const auto value = port_of(*port, error, "genko mcp");
        if (!value) return usage(error);
        http.port = *value;
    }
    if (transport == "stdio" && (parsed.value("--host") || parsed.value("--port"))) {
        return usage("genko mcp: error: --host and --port are for --transport streamable-http");
    }
    const fs::path root = core::path_from_utf8(*root_text);
    try {
        fs::create_directories(root);
        if (!agent.starts_with("ai:")) {
            // (build_server: the human's rights are never handed to an agent)
            throw core::ApplyError("MCP サーバーの actor は ai:<名前> にする（人間の権限はエージェントに渡さない）");
        }
        if (transport == "stdio") return mcp::serve_stdio(fs::absolute(root), agent);
        return mcp::serve_streamable_http(fs::absolute(root), agent, http);
    } catch (const core::Error& error) {
        // (nothing goes to stdout: it is the protocol's)
        write_text(core::dump_python(Json::object({{"ok", false}, {"error", error.what()}, {"code", error.code()}})) + "\n", stderr);
        return 1;
    } catch (const std::exception& error) {
        write_text(core::dump_python(Json::object({{"ok", false}, {"error", error.what()}, {"code", "internal"}})) + "\n", stderr);
        return 1;
    }
}

int serve_command(const QStringList& args) {
    Parsed parsed;
    const std::vector<Spec> specs = {{"--host"}, {"--port"}, {"--root"}, {"--allow-origin", true, true}, {"--allow-host", true, true}};
    if (auto error = parse(args, specs, parsed)) return usage("genko serve: error: " + *error);
    if (!parsed.positional.empty()) return usage("genko serve: error: unrecognized arguments: " + parsed.positional.front());
    const auto root_text = parsed.value("--root");
    if (!root_text) return usage("genko serve: error: the following arguments are required: --root");
    headless::Options options;
    options.root = core::path_from_utf8(*root_text);
    if (const auto host = parsed.value("--host")) options.host = *host;
    if (const auto port = parsed.value("--port")) {
        std::string error;
        const auto value = port_of(*port, error, "genko serve");
        if (!value) return usage(error);
        options.port = *value;
    }
    if (const auto found = parsed.values.find("--allow-origin"); found != parsed.values.end()) options.allow_origins = found->second;
    if (const auto found = parsed.values.find("--allow-host"); found != parsed.values.end()) options.allow_hosts = found->second;
    try {
        return headless::serve(options);
    } catch (const core::Error& error) {
        write_text(core::dump_python(Json::object({{"ok", false}, {"error", error.what()}, {"code", error.code()}})) + "\n", stdout);
        return 1;
    }
}

int token_command(const QStringList& args, bool ascii) {
    Parsed parsed;
    const std::vector<Spec> specs = {{"--actor"}, {"--id"}};
    if (auto error = parse(args, specs, parsed)) return usage("genko token: error: " + *error);
    if (parsed.positional.empty()) return usage("genko token: error: the following arguments are required: action");
    const std::string action = parsed.positional.front();
    if (action != "add" && action != "list" && action != "revoke") {
        return usage("genko token: error: argument action: invalid choice: " + core::py_repr_str(action) + " (choose from 'add', 'list', 'revoke')");
    }
    if (parsed.positional.size() > 1) return usage("genko token: error: unrecognized arguments: " + parsed.positional[1]);
    const auto print = [ascii](const Json& out) { write_text(core::dump_python(out, ascii) + "\n", stdout); };
    try {
        if (action == "add") {
            const std::string actor = parsed.value("--actor").value_or("");
            print(Json::object({{"ok", true}, {"actor", actor}, {"token", tokens::add(actor)}}));
        } else if (action == "list") {
            print(Json::object({{"ok", true}, {"tokens", tokens::listing()}}));
        } else {
            const std::string id = parsed.value("--id").value_or("");
            print(id.empty() ? Json::object({{"ok", false}, {"error", "--id is required"}}) : Json::object({{"ok", true}, {"revoked", tokens::revoke(id)}}));
        }
        return 0;
    } catch (const core::Error& error) {
        print(Json::object({{"ok", false}, {"error", error.what()}, {"code", error.code()}}));
        return 1;
    }
}

// (PC5 J-g) one line read from the channel: none when it closed or said nothing within the time
static std::optional<QByteArray> control_line(QLocalSocket& socket, QByteArray& buffer) {
    for (;;) {
        if (const qsizetype end = buffer.indexOf('\n'); end >= 0) {
            QByteArray line = buffer.left(end);
            buffer.remove(0, end + 1);
            return line;
        }
        if (socket.state() != QLocalSocket::ConnectedState && socket.bytesAvailable() == 0) return std::nullopt;
        if (!socket.waitForReadyRead(service::control::kTimeoutMs) && socket.bytesAvailable() == 0) return std::nullopt;
        buffer += socket.readAll();
    }
}

int control_command(const QStringList& args, bool ascii) {
    Parsed parsed;
    const std::vector<Spec> specs = {{"--token-file"}};
    if (auto error = parse(args, specs, parsed)) return usage("genko control: error: " + *error);
    if (parsed.positional.empty()) return usage("genko control: error: the following arguments are required: method (genko control methods: the list)");
    if (parsed.positional.size() > 2) return usage("genko control: error: unrecognized arguments: " + parsed.positional[2]);
    const auto print = [ascii](const Json& out) { write_text(core::dump_python(out, ascii) + "\n", stdout); };
    const std::string method = parsed.positional[0];
    Json params = Json::object();
    if (parsed.positional.size() == 2) {
        try {
            params = Json::parse(parsed.positional[1]);
        } catch (const std::exception&) {
            return usage("genko control: error: params is one JSON object ({\"ops\": [...]})");
        }
        if (!params.is_object()) return usage("genko control: error: params is one JSON object");
    }
    // the token: --token-file (its first line), else GENKO_TOKEN (never an argument: a command line is seen by others)
    std::string token;
    if (const auto file = parsed.value("--token-file")) {
        QFile in(QString::fromStdString(*file));
        if (!in.open(QIODevice::ReadOnly)) return usage("genko control: error: cannot read --token-file " + *file);
        token = QString::fromUtf8(in.readLine()).trimmed().toStdString();
    } else {
        token = qEnvironmentVariable("GENKO_TOKEN").trimmed().toStdString();
    }
    if (token.empty()) return usage("genko control: error: a token is needed: --token-file <file> or GENKO_TOKEN (genko token add --actor ai:<name>)");
    QString why;
    const QString name = service::control::server_name(&why);
    if (name.isEmpty()) {  // (no folder of the person's own: never a shared one — the review's P2-2)
        print(Json::object({{"ok", false}, {"error", "制御チャンネルにつなげません: " + why.toStdString()}, {"code", "not_listening"}}));
        return 1;
    }
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(3000)) {
        print(Json::object({{"ok", false},
                            {"error", "制御チャンネルにつながりません（Genko を開いて、環境設定 → 連携 で制御チャンネルを入れてください）"},
                            {"code", "not_listening"}}));
        return 1;
    }
    const auto send = [&socket](const Json& line) {
        socket.write(QByteArray::fromStdString(line.dump()) + '\n');
        return socket.waitForBytesWritten(service::control::kTimeoutMs) || socket.bytesToWrite() == 0;
    };
    if (!service::control::peer_is_mine(socket, &why)) {  // (the token goes to this person's Genko only)
        socket.abort();
        print(Json::object({{"ok", false}, {"error", "制御チャンネルの相手を確かめられないので、トークンを送りませんでした: " + why.toStdString()},
                            {"code", "not_mine"}}));
        return 1;
    }
    QByteArray buffer;
    Json answer;
    send(Json::object({{"id", "auth"}, {"method", "auth"}, {"params", Json::object({{"token", token}})}}));
    const auto authed = control_line(socket, buffer);
    if (!authed) {
        print(Json::object({{"ok", false}, {"error", "トークンが通りませんでした（genko token list で確かめる）"}, {"code", "not_authenticated"}}));
        return 1;
    }
    send(Json::object({{"id", 1}, {"method", method}, {"params", params}}));
    const auto line = control_line(socket, buffer);
    if (!line) {
        print(Json::object({{"ok", false}, {"error", "制御チャンネルが答えませんでした"}, {"code", "no_answer"}}));
        return 1;
    }
    try {
        answer = Json::parse(line->toStdString());
    } catch (const std::exception&) {
        print(Json::object({{"ok", false}, {"error", "制御チャンネルの答えが読めませんでした"}, {"code", "bad_answer"}}));
        return 1;
    }
    socket.disconnectFromServer();
    answer.erase("id");
    print(answer);
    return answer.value("ok", Json(false)) == Json(true) ? 0 : 1;
}

}  // namespace genko::api
