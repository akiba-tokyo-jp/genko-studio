#include "api/studio_cli.hpp"

#include <QCoreApplication>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/poses.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/not_yet_ported.hpp"
#include "service/briefs.hpp"
#include "service/evaluate.hpp"
#include "service/human_service.hpp"
#include "service/jobs.hpp"
#include "service/review.hpp"
#include "service/studio_service.hpp"
#include "service/upscale.hpp"
#include "storage/fsutil.hpp"
#include "studio/files.hpp"

namespace genko::api {

namespace {

namespace fs = std::filesystem;
using core::Json;
using service::HumanService;
using service::StudioService;
using service::ToolResult;

void write_text(const std::string& text, std::FILE* to) {
    std::fwrite(text.data(), 1, text.size(), to);
    std::fflush(to);
}

// A usage error, as argparse reports it (stderr, exit 2; as JSON, as every `genko` command's).
struct UsageError {
    std::string message;
};

int usage_error(const std::string& message) {
    Json out = Json::object({{"ok", false}, {"error", message}, {"code", "usage"}});
    write_text(core::dump_python(out) + "\n", stderr);
    return 2;
}

// --- argparse (the subparsers of genko/studio/cli.py) ----------------------------------------------------------------------

enum class Type { Str, Int, Float };

struct Arg {
    std::string name;  // "--title" (an option) or the positional's name
    std::string dest;
    bool positional = false;
    char nargs = '1';  // '1', '?' or '+'
    bool flag = false;  // store_true
    bool required = false;
    Json fallback;  // the default (null: None)
    Type type = Type::Str;
    std::vector<std::string> choices;
};

struct Command {
    std::string name;
    std::vector<Arg> args;
};

Arg pos(std::string name, char nargs = '1', Json fallback = Json(), std::vector<std::string> choices = {}) {
    Arg a;
    a.dest = name;
    a.name = std::move(name);
    a.positional = true;
    a.nargs = nargs;
    a.fallback = std::move(fallback);
    a.choices = std::move(choices);
    return a;
}

Arg opt(std::string name, Json fallback = Json(), Type type = Type::Str, std::vector<std::string> choices = {}, bool required = false,
        std::string dest = {}) {
    Arg a;
    if (dest.empty()) {
        dest = name.substr(2);
        for (char& c : dest) c = c == '-' ? '_' : c;
    }
    a.name = std::move(name);
    a.dest = std::move(dest);
    a.fallback = std::move(fallback);
    a.type = type;
    a.choices = std::move(choices);
    a.required = required;
    return a;
}

Arg flag(std::string name) {
    Arg a = opt(std::move(name), false);
    a.flag = true;
    return a;
}

Arg as_actor() { return opt("--as", Json(), Type::Str, {}, false, "actor"); }

std::vector<Command> commands() {
    const auto project_cmd = [](std::string name, std::vector<Arg> more = {}) {
        Command c{std::move(name), {pos("project"), opt("--agent", "ai:cli"), opt("--session")}};
        for (Arg& a : more) c.args.push_back(std::move(a));
        return c;
    };
    std::vector<Command> out;
    out.push_back({"init", {pos("project"), opt("--title", "無題"), opt("--pages", 8, Type::Int), opt("--spec", "commercial-b4", Type::Str, {"commercial-b4", "a4-mono", "webtoon"})}});
    out.push_back(project_cmd("status"));
    out.push_back(project_cmd("next", {opt("--limit", 5, Type::Int)}));
    out.push_back(project_cmd("inspect", {pos("target"), opt("--page", Json(), Type::Int), opt("--frame")}));
    out.push_back(project_cmd("render", {opt("--page", Json(), Type::Int, {}, true), opt("--mode", "name", Type::Str, {"name", "proof", "print"}),
                                         opt("--frame"), opt("--max-px", 1024, Type::Int)}));
    out.push_back(project_cmd("import-image", {pos("image")}));
    out.push_back(project_cmd("apply", {pos("file"), flag("--commit")}));
    for (const char* name : {"set-bible", "set-script", "submit-name"}) {
        std::vector<Arg> more{pos("file"), flag("--commit")};
        if (std::string(name) == "submit-name") more.push_back(flag("--replace"));
        out.push_back(project_cmd(name, std::move(more)));
    }
    out.push_back(project_cmd("record-review", {opt("--page", Json(), Type::Int, {}, true), opt("--notes", ""), opt("--score", Json(), Type::Float)}));
    out.push_back(project_cmd("request-approval", {opt("--gate", "name", Type::Str, {"name", "art", "sheet"}), opt("--pages", ""), opt("--character"), opt("--note", "")}));
    out.push_back(project_cmd("tickets", {flag("--all")}));
    out.push_back({"approve", {pos("project"), pos("gate", '1', Json(), {"name", "art", "sheet"}), opt("--pages", ""), opt("--character"), opt("--candidate"),
                               opt("--face"), as_actor()}});
    out.push_back({"revoke", {pos("project"), pos("gate", '1', Json(), {"name", "art", "sheet"}), opt("--pages", ""), opt("--character"), opt("--reason", ""),
                              as_actor()}});
    out.push_back({"comment", {pos("project"), pos("text"), opt("--page", Json(), Type::Int, {}, true), opt("--frame"), as_actor()}});
    out.push_back({"import-name", {pos("project"), pos("files", '+'), opt("--start-page", 1, Type::Int), opt("--align", "auto", Type::Str, {"auto", "page", "live"}),
                                   as_actor()}});
    out.push_back(project_cmd("analyze", {opt("--page", Json(), Type::Int, {}, true), opt("--params")}));
    out.push_back(project_cmd("proposals", {flag("--all")}));
    out.push_back({"accept", {pos("project"), pos("proposal"), flag("--force"), as_actor()}});
    out.push_back({"reject", {pos("project"), pos("proposal"), opt("--note", ""), as_actor()}});
    out.push_back({"eval-atari", {pos("truth")}});
    out.push_back({"stats", {pos("project")}});
    out.push_back({"audit", {pos("project")}});
    out.push_back({"eval-sample", {pos("project"), opt("--out", Json(), Type::Str, {}, true), opt("--per-character", 10, Type::Int), opt("--seed", 0, Type::Int)}});
    out.push_back({"eval-score", {pos("project"), pos("answers")}});
    out.push_back({"close-ticket", {pos("project"), pos("ticket"), opt("--reply", ""), as_actor()}});
    out.push_back({"reopen-ticket", {pos("project"), pos("ticket"), opt("--note", ""), as_actor()}});
    out.push_back({"adopt-drafts", {pos("project")}});
    out.push_back({"chat-approval", {pos("project"), pos("state", '1', Json(), {"on", "off"}), as_actor()}});
    out.push_back({"export", {pos("project"), opt("--format", "png", Type::Str, {"png", "pdf", "tiff", "webtoon", "sns"}), opt("--out", Json(), Type::Str, {}, true),
                              opt("--dpi", Json(), Type::Int), flag("--allow-fixture"), flag("--force"),
                              opt("--color", "auto", Type::Str, {"auto", "gray", "bitonal", "rgb", "cmyk"}), opt("--icc"), as_actor()}});
    out.push_back({"upscaler", {pos("action", '1', Json(), {"list", "add", "remove"}), pos("name", '?'), opt("--command", Json(), Type::Str, {}, false, "command_line"),
                                opt("--scales", ""), opt("--label", "")}});
    out.push_back({"tools", {pos("action", '1', Json(), {"list", "set", "remove", "example"}), pos("tool_id", '?'), opt("--file")}});
    out.push_back(project_cmd("call", {pos("tool"), pos("args", '?')}));
    out.push_back({"review", {pos("project"), opt("--out", Json(), Type::Str, {}, true)}});
    return out;
}

bool negative_number(const std::string& text) {
    // argparse's _negative_number_matcher: ^-\d+$|^-\d*\.\d+$
    if (text.size() < 2 || text[0] != '-') return false;
    const std::string rest = text.substr(1);
    const std::size_t dot = rest.find('.');
    const auto digits = [](const std::string& s, bool allow_empty) {
        if (s.empty()) return allow_empty;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    };
    if (dot == std::string::npos) return digits(rest, false);
    return digits(rest.substr(0, dot), true) && digits(rest.substr(dot + 1), false);
}

bool looks_like_option(const std::string& text) { return text.size() > 1 && text[0] == '-' && !negative_number(text); }

std::string choices_text(const std::vector<std::string>& choices) {
    std::string out;
    for (const std::string& c : choices) out += (out.empty() ? "" : ", ") + core::py_repr_str(c);
    return out;
}

Json convert(const Arg& arg, const std::string& text) {
    const std::string who = arg.positional ? arg.name : arg.name;
    Json value = text;
    try {
        if (arg.type == Type::Int) value = core::to_int(Json(text));
        if (arg.type == Type::Float) value = core::to_float(Json(text));
    } catch (const core::Error&) {
        throw UsageError{"argument " + who + ": invalid " + std::string(arg.type == Type::Int ? "int" : "float") + " value: " + core::py_repr_str(text)};
    }
    if (!arg.choices.empty() && std::find(arg.choices.begin(), arg.choices.end(), text) == arg.choices.end()) {
        throw UsageError{"argument " + who + ": invalid choice: " + core::py_repr_str(text) + " (choose from " + choices_text(arg.choices) + ")"};
    }
    return value;
}

// parse_args for one subcommand: the namespace as a JSON object (dest → value).
Json parse(const Command& command, const std::vector<std::string>& tokens) {
    Json ns = Json::object();
    for (const Arg& a : command.args) ns[a.dest] = a.fallback;
    std::vector<std::string> positionals;
    std::vector<bool> seen(command.args.size(), false);
    bool only_positionals = false;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        if (!only_positionals && token == "--") {
            only_positionals = true;
            continue;
        }
        if (only_positionals || !looks_like_option(token)) {
            positionals.push_back(token);
            continue;
        }
        std::string name = token;
        std::optional<std::string> attached;
        if (const std::size_t eq = token.find('='); eq != std::string::npos && token.rfind("--", 0) == 0) {
            name = token.substr(0, eq);
            attached = token.substr(eq + 1);
        }
        std::size_t found = command.args.size();
        std::vector<std::size_t> prefixed;
        for (std::size_t k = 0; k < command.args.size(); ++k) {
            if (command.args[k].positional) continue;
            if (command.args[k].name == name) found = k;
            if (command.args[k].name.rfind(name, 0) == 0) prefixed.push_back(k);
        }
        if (found == command.args.size()) {
            if (name.rfind("--", 0) == 0 && prefixed.size() > 1) {
                std::string listed;
                for (std::size_t k : prefixed) listed += (listed.empty() ? "" : ", ") + command.args[k].name;
                throw UsageError{"ambiguous option: " + name + " could match " + listed};
            }
            if (name.rfind("--", 0) == 0 && prefixed.size() == 1) found = prefixed.front();
        }
        if (found == command.args.size()) throw UsageError{"unrecognized arguments: " + token};
        const Arg& arg = command.args[found];
        seen[found] = true;
        if (arg.flag) {
            if (attached) throw UsageError{"argument " + arg.name + ": ignored explicit argument " + core::py_repr_str(*attached)};
            ns[arg.dest] = true;
            continue;
        }
        std::string value;
        if (attached) {
            value = *attached;
        } else {
            if (i + 1 >= tokens.size() || looks_like_option(tokens[i + 1]) || tokens[i + 1] == "--") {
                throw UsageError{"argument " + arg.name + ": expected one argument"};
            }
            value = tokens[++i];
        }
        ns[arg.dest] = convert(arg, value);
    }
    // the positionals in order: each takes one, "?" one when there are enough, "+" all the others need not
    std::vector<const Arg*> wanted;
    for (const Arg& a : command.args) {
        if (a.positional) wanted.push_back(&a);
    }
    std::size_t at = 0;
    std::vector<std::string> missing;
    for (std::size_t k = 0; k < wanted.size(); ++k) {
        std::size_t later = 0;
        for (std::size_t m = k + 1; m < wanted.size(); ++m) later += wanted[m]->nargs == '1' || wanted[m]->nargs == '+' ? 1 : 0;
        const Arg& arg = *wanted[k];
        const std::size_t left = positionals.size() - at;
        if (arg.nargs == '1') {
            if (left == 0) {
                missing.push_back(arg.name);
                continue;
            }
            ns[arg.dest] = convert(arg, positionals[at++]);
        } else if (arg.nargs == '?') {
            if (left > later) ns[arg.dest] = convert(arg, positionals[at++]);
        } else {
            if (left <= later || left == 0) {
                missing.push_back(arg.name);
                continue;
            }
            Json values = Json::array();
            while (positionals.size() - at > later) values.push_back(convert(arg, positionals[at++]));
            ns[arg.dest] = std::move(values);
        }
    }
    for (std::size_t k = 0; k < command.args.size(); ++k) {
        if (command.args[k].required && !seen[k]) missing.push_back(command.args[k].name);
    }
    if (!missing.empty()) {
        std::string listed;
        for (const std::string& m : missing) listed += (listed.empty() ? "" : ", ") + m;
        throw UsageError{"the following arguments are required: " + listed};
    }
    if (at < positionals.size()) {
        std::string extra;
        for (std::size_t k = at; k < positionals.size(); ++k) extra += (extra.empty() ? "" : " ") + positionals[k];
        throw UsageError{"unrecognized arguments: " + extra};
    }
    return ns;
}

// --- the commands --------------------------------------------------------------------------------------------------------------

int print_reply(const Json& payload, bool ascii) {
    write_text(core::dump_python(payload, ascii) + "\n", stdout);
    return payload.is_object() && payload.contains("ok") && core::py_truthy(payload["ok"]) ? 0 : 1;
}

// Python's {"ok": False, "error": message}, with the code every failure of this build carries (ARCHITECTURE.md §8).
Json failed(const std::string& message, const std::string& code) { return Json::object({{"ok", false}, {"error", message}, {"code", code}}); }

// The most pages a --pages list names (C++: Python makes the list whatever its length, until its memory runs out).
constexpr std::uint64_t kMaxListedPages = 100000;

// _pages("1,3-5"): the pages (int()'s ValueError for a part that is not a number). C++: a list of more than
// kMaxListedPages pages is refused (core::Error "pages_too_many"), a range up to 2**63 - 1 counted without overflow.
Json pages_of(const std::string& text) {
    Json out = Json::array();
    const auto too_many = [&text] {
        throw core::Error("pages_too_many", "--pages のページが多すぎる（" + std::to_string(kMaxListedPages) + " ページまで）: " + text);
    };
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = text.find(',', start);
        const std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (const std::size_t dash = part.find('-'); dash != std::string::npos) {
            const std::int64_t a = core::to_int(Json(part.substr(0, dash)));
            const std::int64_t b = core::to_int(Json(part.substr(dash + 1)));
            if (a <= b) {  // (range(a, b + 1))
                const std::uint64_t span = static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
                if (span >= kMaxListedPages || out.size() + span + 1 > kMaxListedPages) too_many();
                for (std::int64_t p = a;; ++p) {
                    out.push_back(p);
                    if (p == b) break;
                }
            }
        } else if (!core::py_strip(part).empty()) {
            if (out.size() + 1 > kMaxListedPages) too_many();
            out.push_back(core::to_int(Json(part)));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// _load(source): the JSON of a file (or stdin for "-").
Json load(const std::string& source) {
    std::string raw;
    if (source == "-") {
        raw.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    } else {
        raw = storage::read_file(core::path_from_utf8(source));
    }
    core::ParseOptions options;
    options.universal_newlines = true;  // (read as text: "\r\n" is "\n")
    return core::parse_python_json(raw, nullptr, options);
}

// Path(text).resolve()
fs::path resolved(const std::string& text) {
    fs::path path = fs::absolute(core::path_from_utf8(text));
    std::error_code ec;
    fs::path out = fs::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : out;
}

std::string actor_or_default(const Json& ns) {
    return ns["actor"].is_string() && !ns["actor"].get<std::string>().empty() ? ns["actor"].get<std::string>() : service::briefs::default_actor();
}

Json example_tools() {
    return Json::object({{"openai:gpt-image-1", Json::object({{"label", "ChatGPT / OpenAI の画像生成"},
                                                              {"sizes_px", Json::array({Json::array({1024, 1024}), Json::array({1536, 1024}),
                                                                                        Json::array({1024, 1536})})},
                                                              {"supports", Json::object({{"references", true}, {"edit", true}, {"mask", true}, {"seed", false}})},
                                                              {"notes", "サイズと機能は版で変わる。確かめて直す"}})}});
}

int tools_command(const Json& ns, bool ascii) {
    const fs::path config = core::poses::config_dir();
    const std::string action = ns["action"].get<std::string>();
    if (action == "list") {
        return print_reply(Json::object({{"ok", true}, {"path", core::path_to_utf8(config / "tools.json")}, {"tools", studio::load_tools(config)}}), ascii);
    }
    if (action == "example") return print_reply(Json::object({{"ok", true}, {"example", example_tools()}}), ascii);
    if (!core::py_truthy(ns["tool_id"])) return print_reply(failed("tool_id が要る", "missing_argument"), ascii);
    const std::string id = ns["tool_id"].get<std::string>();
    if (action == "remove") return print_reply(Json::object({{"ok", studio::remove_tool(config, id)}}), ascii);
    Json spec;
    if (core::py_truthy(ns["file"])) {
        spec = load(ns["file"].get<std::string>());
    } else {
        const Json examples = example_tools();
        spec = examples.contains(id) ? examples[id] : studio::generic_tool();
    }
    return print_reply(Json::object({{"ok", true}, {"tool", studio::set_tool(config, id, spec)}}), ascii);
}

int upscaler_command(const Json& ns, bool ascii) {
    const std::string action = ns["action"].get<std::string>();
    if (action == "list") {
        return print_reply(Json::object({{"ok", true}, {"upscalers", service::upscale::available()}, {"registered", service::upscale::registered()}}), ascii);
    }
    if (!core::py_truthy(ns["name"])) return print_reply(failed("名前が要る", "missing_argument"), ascii);
    const std::string name = ns["name"].get<std::string>();
    if (action == "remove") return print_reply(Json::object({{"ok", service::upscale::unregister(name)}}), ascii);
    if (!core::py_truthy(ns["command_line"])) return print_reply(failed("--command が要る", "missing_argument"), ascii);
    Json spec;
    try {
        std::vector<double> scales;
        const std::string text = ns["scales"].get<std::string>();
        std::size_t start = 0;
        for (;;) {
            const std::size_t comma = text.find(',', start);
            const std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!core::py_strip(part).empty()) scales.push_back(core::to_float(Json(part)));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        spec = service::upscale::register_program(name, ns["command_line"], scales, ns["label"].get<std::string>());
    } catch (const core::PyValueError& exc) {
        return print_reply(failed(exc.what(), exc.code()), ascii);
    }
    return print_reply(Json::object({{"ok", true}, {"name", name}, {"upscaler", spec}}), ascii);
}

// Path(text) as Python keeps it (str(Path(text))): "." parts and repeated or trailing "/" dropped ("." for nothing).
fs::path pure_path(const std::string& text) {
    std::string root;
    std::size_t i = 0;
    if (text.rfind("//", 0) == 0 && text.rfind("///", 0) != 0) {
        root = "//";
        i = 2;
    } else if (!text.empty() && text[0] == '/') {
        root = "/";
    }
    std::string joined = root;
    bool first = true;
    while (i <= text.size()) {
        std::size_t slash = text.find('/', i);
        if (slash == std::string::npos) slash = text.size();
        const std::string part = text.substr(i, slash - i);
        if (!part.empty() && part != ".") {
            joined += (first ? "" : "/") + part;
            first = false;
        }
        i = slash + 1;
    }
    return core::path_from_utf8(joined.empty() ? std::string(".") : joined);
}

int run(const std::string& cmd, const Json& ns, bool ascii) {
    if (cmd == "tools") return tools_command(ns, ascii);
    if (cmd == "upscaler") return upscaler_command(ns, ascii);
    if (cmd == "eval-atari") return print_reply(service::evaluate::eval_atari(pure_path(ns["truth"].get<std::string>())), ascii);
    const fs::path path = resolved(ns["project"].get<std::string>());
    if (cmd == "approve") {
        HumanService human(path, actor_or_default(ns));
        if (ns["gate"] == "sheet") {
            if (!(core::py_truthy(ns["character"]) && core::py_truthy(ns["candidate"]))) return print_reply(failed("sheet には --character と --candidate が要る", "missing_argument"), ascii);
            Json face;
            if (core::py_truthy(ns["face"])) {
                face = Json::array();
                const std::string text = ns["face"].get<std::string>();
                std::size_t start = 0;
                for (;;) {
                    const std::size_t comma = text.find(',', start);
                    face.push_back(core::to_float(Json(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start))));
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
            return print_reply(human.approve_sheet(ns["character"], ns["candidate"], face), ascii);
        }
        const Json pages = pages_of(ns["pages"].get<std::string>());
        if (pages.empty()) return print_reply(failed("--pages が要る", "missing_argument"), ascii);
        return print_reply(ns["gate"] == "name" ? human.approve_name(pages) : human.approve_art(pages), ascii);
    }
    if (cmd == "chat-approval") return print_reply(HumanService(path, actor_or_default(ns)).chat_approval(ns["state"] == "on"), ascii);
    if (cmd == "revoke") {
        HumanService human(path, actor_or_default(ns));
        return print_reply(human.revoke(ns["gate"], pages_of(ns["pages"].get<std::string>()), ns["character"], ns["reason"]), ascii);
    }
    if (cmd == "comment") return print_reply(HumanService(path, actor_or_default(ns)).comment(ns["page"], ns["text"], ns["frame"]), ascii);
    if (cmd == "stats") return print_reply(service::evaluate::stats(path), ascii);
    if (cmd == "audit") return print_reply(service::evaluate::audit(path), ascii);
    if (cmd == "eval-sample") {
        return print_reply(service::evaluate::eval_sample(path, pure_path(ns["out"].get<std::string>()), ns["per_character"].get<std::int64_t>(),
                                                          ns["seed"].get<std::int64_t>()),
                           ascii);
    }
    if (cmd == "eval-score") return print_reply(service::evaluate::eval_score(path, pure_path(ns["answers"].get<std::string>())), ascii);
    if (cmd == "accept") {
        return print_reply(HumanService(path, actor_or_default(ns)).accept_proposal(ns["proposal"], core::py_truthy(ns["force"])), ascii);
    }
    if (cmd == "import-name") {
        StudioService importer(path.parent_path(), actor_or_default(ns));
        importer.set_confine(false);  // (Python's confine=False)
        Json files = Json::array();
        for (const Json& f : ns["files"]) files.push_back(core::path_to_utf8(resolved(f.get<std::string>())));
        return print_reply(
            importer.call("import_name", Json::array({core::path_to_utf8(path.filename()), files, ns["start_page"], ns["align"]}), Json::object()).to_dict(),
            ascii);
    }
    if (cmd == "export") {
        service::OfficialExport options;
        options.dpi = ns["dpi"];
        options.allow_fixture = core::py_truthy(ns["allow_fixture"]);
        options.force = core::py_truthy(ns["force"]);
        options.color = ns["color"].get<std::string>();
        if (ns["icc"].is_string()) options.icc = ns["icc"].get<std::string>();
        return print_reply(HumanService(path, actor_or_default(ns)).export_book(ns["format"].get<std::string>(), pure_path(ns["out"].get<std::string>()), options),
                           ascii);
    }
    if (cmd == "review") {
        const fs::path out = pure_path(ns["out"].get<std::string>());
        if (out.has_parent_path()) fs::create_directories(out.parent_path());
        storage::write_atomic(out, studio::text_mode(service::review::review_html(path)));  // (Path.write_text)
        return print_reply(Json::object({{"ok", true}, {"path", core::path_to_utf8(out)}}), ascii);
    }
    if (cmd == "reject") return print_reply(HumanService(path, actor_or_default(ns)).reject_proposal(ns["proposal"], ns["note"]), ascii);
    if (cmd == "close-ticket") return print_reply(HumanService(path, actor_or_default(ns)).close_ticket(ns["ticket"], ns["reply"]), ascii);
    if (cmd == "reopen-ticket") return print_reply(HumanService(path, actor_or_default(ns)).reopen_ticket(ns["ticket"], ns["note"]), ascii);
    if (cmd == "adopt-drafts") return print_reply(service::adopt::adopt_drafts(path), ascii);
    std::string actor;
    try {
        const Json agent = ns.contains("agent") ? ns["agent"] : Json("ai:cli");
        const Json session = ns.contains("session") ? ns["session"] : Json();
        actor = studio::actor_for(agent.get<std::string>(), session.is_string() ? std::optional<std::string>(session.get<std::string>()) : std::nullopt);
    } catch (const core::Error& exc) {
        return print_reply(failed(exc.what(), exc.code()), ascii);
    }
    StudioService service(path.parent_path(), actor);
    const Json name = core::path_to_utf8(path.filename());
    ToolResult result;
    const auto call = [&](const char* tool, Json kwargs) { return service.call(tool, Json::array(), kwargs); };
    if (cmd == "init") {
        result = call("create_project", Json::object({{"name", name}, {"title", ns["title"]}, {"pages", ns["pages"]}, {"spec_preset", ns["spec"]}}));
    } else if (cmd == "status") {
        result = call("status", Json::object({{"project", name}}));
    } else if (cmd == "next") {
        result = call("next", Json::object({{"project", name}, {"limit", ns["limit"]}}));
    } else if (cmd == "inspect") {
        result = call("inspect", Json::object({{"project", name}, {"target", ns["target"]}, {"page", ns["page"]}, {"frame_id", ns["frame"]}}));
    } else if (cmd == "render") {
        result = call("render", Json::object({{"project", name}, {"page", ns["page"]}, {"mode", ns["mode"]}, {"max_px", ns["max_px"]}, {"frame_id", ns["frame"]}}));
    } else if (cmd == "analyze") {
        Json params;
        if (core::py_truthy(ns["params"])) params = core::parse_python_json(ns["params"].get<std::string>());
        result = call("analyze_name", Json::object({{"project", name}, {"page", ns["page"]}, {"params", params}}));
    } else if (cmd == "import-image") {
        service.set_confine(false);  // (Python's confine=False)
        result = call("import_image", Json::object({{"project", name}, {"path", core::path_to_utf8(resolved(ns["image"].get<std::string>()))}}));
    } else if (cmd == "call") {
        const std::string tool = ns["tool"].get<std::string>();
        if (!service::agent_tool(tool)) {
            std::string names;
            for (const std::string& t : service::agent_tools_sorted()) names += (names.empty() ? "" : ", ") + t;
            return print_reply(failed("道具 " + tool + " はない（" + names + "）", "unknown_tool"), ascii);
        }
        Json kwargs = core::py_truthy(ns["args"]) ? load(ns["args"].get<std::string>()) : Json::object();
        if (!kwargs.is_object()) return print_reply(failed("引数は JSON オブジェクト", "bad_arguments"), ascii);
        // (Python: kwargs.setdefault("confine", False) for import_image; the keyword-only confine= of import_image and
        // import_name is this build's set_confine, not an argument — another tool given it refuses it as Python's does)
        if (tool == "import_image" || tool == "import_name") {
            service.set_confine(kwargs.contains("confine") ? core::py_truthy(kwargs["confine"]) : tool == "import_name");
            kwargs.erase("confine");
        }
        result = service.call(tool, Json::array({name}), kwargs);
    } else if (cmd == "proposals") {
        result = call("proposals", Json::object({{"project", name}, {"status", core::py_truthy(ns["all"]) ? "all" : "open"}}));
    } else if (cmd == "apply") {
        const Json payload = load(ns["file"].get<std::string>());
        Json ops;
        if (payload.is_array()) {
            ops = payload;
        } else {
            if (!payload.is_object()) core::raise_attribute_error(payload, "get");
            ops = core::py_get(payload, "ops", Json::array());
        }
        result = call("apply_ops", Json::object({{"project", name}, {"ops", ops}, {"commit", ns["commit"]}}));
    } else if (cmd == "set-bible") {
        result = call("set_bible", Json::object({{"project", name}, {"bible", load(ns["file"].get<std::string>())}, {"commit", ns["commit"]}}));
    } else if (cmd == "set-script") {
        result = call("set_script", Json::object({{"project", name}, {"script", load(ns["file"].get<std::string>())}, {"commit", ns["commit"]}}));
    } else if (cmd == "submit-name") {
        result = call("submit_name", Json::object({{"project", name}, {"plan", load(ns["file"].get<std::string>())}, {"commit", ns["commit"]},
                                                   {"replace", ns["replace"]}}));
    } else if (cmd == "record-review") {
        result = call("record_review", Json::object({{"project", name}, {"page", ns["page"]}, {"score", ns["score"]}, {"notes", ns["notes"]}}));
    } else if (cmd == "request-approval") {
        result = call("request_approval", Json::object({{"project", name}, {"gate", ns["gate"]}, {"pages", pages_of(ns["pages"].get<std::string>())},
                                                        {"note", ns["note"]}, {"character_id", ns["character"]}}));
    } else {
        result = call("tickets", Json::object({{"project", name}, {"status", core::py_truthy(ns["all"]) ? "all" : "open"}}));
    }
    return print_reply(result.to_dict(), ascii);
}

}  // namespace

int studio_command(const QStringList& args, bool ascii) {
    std::vector<std::string> tokens;
    for (const QString& a : args) tokens.push_back(a.toStdString());
    const std::vector<Command> all = commands();
    if (tokens.empty()) return usage_error("genko studio: error: the following arguments are required: cmd");
    const std::string cmd = tokens.front();
    const Command* command = nullptr;
    std::vector<std::string> names;
    for (const Command& c : all) {
        names.push_back(c.name);
        if (c.name == cmd) command = &c;
    }
    if (command == nullptr) {
        return usage_error("genko studio: error: argument cmd: invalid choice: " + core::py_repr_str(cmd) + " (choose from " + choices_text(names) + ")");
    }
    Json ns;
    try {
        ns = parse(*command, std::vector<std::string>(tokens.begin() + 1, tokens.end()));
    } catch (const UsageError& error) {
        return usage_error("genko studio " + cmd + ": error: " + error.message);
    }
    const auto caught = [ascii](const std::string& message, const std::string& code) {
        return print_reply(Json::object({{"ok", false}, {"error", message}, {"code", code}}), ascii);
    };
    // (a job a tool started in the background ends before the command does: C++ does not leave it to die; what the
    // command printed is out first, so the reply is not held back while the job runs)
    struct WaitForJobs {
        ~WaitForJobs() {
            std::fflush(stdout);
            std::fflush(stderr);
            try {
                service::jobs::wait_all();
            } catch (...) {
            }
        }
    } wait_for_jobs;
    try {
        return run(cmd, ns, ascii);
    } catch (const render::NotYetPorted& exc) {
        // (what the book carries that this build does not draw yet: nothing is written)
        return print_reply(Json::object({{"ok", false}, {"error", exc.what()}, {"code", exc.code()}, {"element", exc.element()}}), ascii);
    } catch (const core::PyUncaught& exc) {
        // (a traceback in Python: its last line)
        const std::string message = std::string(exc.what()).empty() ? exc.type() : exc.type() + ": " + exc.what();
        return caught(message, "python_error");
    } catch (const core::Error& exc) {
        // ApplyError, ValueError, TypeError, FileNotFoundError and JSONDecodeError: str(exc), as Python's main prints it
        const int code = caught(exc.what(), exc.code());
        // (C++: a book of v1–v3, which this build does not write — exit 3, as every genko command's; Python writes v3)
        return exc.code() == "needs_migration" ? 3 : code;
    } catch (const std::exception& exc) {
        return caught(exc.what(), "internal");
    }
}

}  // namespace genko::api
