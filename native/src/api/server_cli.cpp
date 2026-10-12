#include "api/server_cli.hpp"

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

}  // namespace genko::api
