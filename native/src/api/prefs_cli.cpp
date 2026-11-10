#include "api/prefs_cli.hpp"

#include <cstdio>
#include <optional>
#include <string>

#include "core/commands_list.hpp"
#include "core/json.hpp"
#include "core/prefs_table.hpp"
#include "core/session_file.hpp"
#include "core/json_depth.hpp"

namespace genko::api {

namespace {

using core::Json;

void write_text(const std::string& text, std::FILE* to) {
    std::fwrite(text.data(), 1, text.size(), to);
    std::fflush(to);
}

void print_json(const Json& value, bool ascii) { write_text(core::dump_python(value, ascii) + "\n", stdout); }

int usage(const std::string& message) {
    write_text(Json{{"ok", false}, {"error", message}, {"code", "usage"}}.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n", stderr);
    return 2;
}

int answer(const core::prefs::Outcome& out, bool ascii) {
    print_json(out.json(), ascii);
    return out.ok ? 0 : 1;
}

std::optional<Json> parse(const QString& text) {
    if (core::too_deep(text.toStdString())) return std::nullopt;  // (PC6, the review's P3-2: as text that is not JSON)
    try {
        return Json::parse(text.toStdString());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

int prefs_command(const QStringList& args, bool ascii) {
    const QString verb = args.value(0);
    const QStringList rest = args.mid(1);
    if (verb == QLatin1String("get") || verb == QLatin1String("reset")) {
        if (rest.size() > 1) return usage("genko prefs " + verb.toStdString() + " [section|section.key]: one path at most");
        const std::string path = rest.value(0).toStdString();
        const auto store = core::prefs::store();
        return answer(verb == QLatin1String("get") ? core::prefs::get(*store, path) : core::prefs::reset(*store, path), ascii);
    }
    if (verb == QLatin1String("set")) {
        Json values;
        if (rest.size() == 2 && rest[0] == QLatin1String("--values")) {
            const auto given = parse(rest[1]);
            if (!given || !given->is_object()) return usage("genko prefs set --values: a JSON object, e.g. '{\"interface.theme\": \"dark\"}'");
            values = *given;
        } else if (rest.size() == 2 && !rest[0].startsWith(QLatin1String("--"))) {
            const auto given = parse(rest[1]);
            if (!given) return usage("genko prefs set " + rest[0].toStdString() + ": the value as JSON (\"dark\" with its quotes, true, 12)");
            values = Json{{rest[0].toStdString(), *given}};
        } else {
            return usage("genko prefs set <section.key> <JSON value> | --values '<JSON object>'");
        }
        const auto store = core::prefs::store();
        return answer(core::prefs::set(*store, values), ascii);
    }
    return usage("genko prefs get [path] | set <path> <value> | set --values <JSON> | reset [path]");
}

int commands_command(const QStringList& args, bool ascii) {
    QString menu;
    QString words;
    for (int i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        if ((arg == QLatin1String("--menu") || arg == QLatin1String("--search")) && i + 1 < args.size()) {
            (arg == QLatin1String("--menu") ? menu : words) = args[++i];
        } else if (arg == QLatin1String("--menu") || arg == QLatin1String("--search")) {
            return usage("genko commands: argument " + arg.toStdString() + ": expected one argument");
        } else {
            return usage("genko commands: unrecognized arguments: " + arg.toStdString());
        }
    }
    const auto store = core::prefs::store();
    print_json(core::commands::list(*store, menu.toStdString(), words.toStdString()), ascii);
    return 0;
}

int session_command(const QStringList& args, bool ascii) {
    if (!args.isEmpty()) return usage("genko session: unrecognized arguments: " + args.join(QLatin1Char(' ')).toStdString());
    print_json(core::session_file::read(), ascii);
    return 0;
}

}  // namespace genko::api
