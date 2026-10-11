#include "studio/files.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <system_error>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/studio.hpp"
#include "storage/fsutil.hpp"

namespace genko::studio {

namespace fs = std::filesystem;
using core::Json;

namespace {

// The first `n` code points of a UTF-8 text (Python's text[:n]).
std::string first_code_points(const std::string& text, std::size_t n) {
    std::size_t at = 0;
    std::size_t count = 0;
    while (at < text.size() && count < n) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) ++at;
        ++count;
    }
    return text.substr(0, at);
}

std::size_t code_points(const std::string& text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// A file's text as Path.read_text gives it (nothing when it cannot be read).
std::optional<std::string> read_text(const fs::path& path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return std::nullopt;
    try {
        return storage::read_file(path);
    } catch (const core::Error&) {
        return std::nullopt;
    }
}

// json.loads of a file's text (nothing when it cannot be read or is not JSON: Python's (OSError, ValueError)).
std::optional<Json> read_json_quietly(const fs::path& path) {
    const auto text = read_text(path);
    if (!text) return std::nullopt;
    try {
        core::ParseOptions options;
        options.universal_newlines = true;
        return core::parse_python_json(*text, nullptr, options);
    } catch (const core::Error&) {
        return std::nullopt;
    }
}

// float(x.get(key, 0)) of a value read from a studio file, or nothing when Python's float() would fail there (C++ only:
// Python stops with the error; a broken entry counts as long ago here).
std::optional<double> float_at(const Json& item, const char* key) {
    if (!item.is_object()) return std::nullopt;
    const auto it = item.find(key);
    if (it == item.end()) return 0.0;
    try {
        return core::py_float(*it);
    } catch (const core::Error&) {
        return std::nullopt;
    }
}

// A name a studio file is kept under (an item id, a request's file): nothing that leaves its folder.
bool safe_name(const std::string& name) {
    if (name.empty() || name.find('\0') != std::string::npos) return false;
    const fs::path path = core::path_from_utf8(name);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const fs::path& part : path) {
        if (part == ".." || part == ".") return false;
    }
    return name.find('\\') == std::string::npos;
}

}  // namespace

// --- jsonutil --------------------------------------------------------------------------------------------------------------

std::string canonical_json(const Json& value) { return core::dump_canonical(value); }

std::string content_hash(const Json& value) { return core::content_hash(value); }

void write_json(const fs::path& path, const Json& value) {
    core::DumpOptions options;
    options.indent = 2;
    options.item_separator = ",";
    options.key_separator = ": ";
    options.sort_keys = true;
    storage::write_atomic(path, core::dump(value, options) + "\n");
}

Json read_json(const fs::path& path, const Json& fallback) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return fallback;
    core::ParseOptions options;
    options.universal_newlines = true;
    return core::parse_python_json(storage::read_file(path), nullptr, options);
}

// --- presence --------------------------------------------------------------------------------------------------------------

namespace {

fs::path presence_file(const fs::path& project) { return project / "studio" / "presence.json"; }

Json read_presence(const fs::path& project) {
    const auto data = read_json_quietly(presence_file(project));
    if (!data || !data->is_object()) return Json::object({{"actors", Json::object()}, {"acks", Json::object()}});
    return *data;
}

void write_presence(const fs::path& project, const Json& data) {
    storage::write_atomic(presence_file(project), core::dump_python(data));
}

}  // namespace

std::string actor_for(const std::string& base, const std::optional<std::string>& session) {
    if (!session || session->empty()) return base;
    const std::string trimmed = core::py_strip(*session);
    static const QRegularExpression kSession(QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9_.:@-]{1,40}")));
    if (!kSession.match(QString::fromStdString(trimmed)).hasMatch()) {
        throw core::Error("value", "session は 1〜40 文字の英数字と _ . : @ - だけ（例 9204、telegram-9204）");
    }
    return base + "/" + trimmed;
}

std::vector<Other> others(const fs::path& project, const std::string& actor, double now) {
    const Json data = read_presence(project);
    const Json* actors = core::get(data, "actors");
    std::vector<Other> out;
    if (actors == nullptr || !actors->is_object()) return out;
    std::vector<std::string> names;
    for (const auto& [name, item] : actors->items()) names.push_back(name);
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const auto at = float_at((*actors)[name], "at");
        if (name == actor || !at || !(now - *at < kActiveSeconds)) continue;
        out.push_back(Other{name, core::py_round((now - *at) / 60, 1)});
    }
    return out;
}

std::vector<Other> check(const fs::path& project, const std::string& actor, double now) {
    const std::vector<Other> found = others(project, actor, now);
    if (found.empty()) return {};
    Json data = read_presence(project);
    std::vector<std::string> known;
    if (const Json* acks = core::get(data, "acks"); acks != nullptr && acks->is_object()) {
        if (const Json* ack = core::get(*acks, actor); ack != nullptr && ack->is_object()) {
            const auto at = float_at(*ack, "at");
            const Json* seen = core::get(*ack, "others");
            if (at && now - *at < kAckSeconds && seen != nullptr && seen->is_array()) {
                for (const Json& name : *seen) {
                    if (name.is_string()) known.push_back(name.get<std::string>());
                }
            }
        }
    }
    std::vector<Other> fresh;
    for (const Other& item : found) {
        if (std::find(known.begin(), known.end(), item.actor) == known.end()) fresh.push_back(item);
    }
    if (!fresh.empty()) {
        std::vector<std::string> all = known;
        for (const Other& item : found) all.push_back(item.actor);
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        if (!data.contains("acks") || !data["acks"].is_object()) data["acks"] = Json::object();
        data["acks"][actor] = Json::object({{"others", all}, {"at", now}});
        write_presence(project, data);
    }
    return fresh;
}

void touch(const fs::path& project, const std::string& actor, double now) {
    Json data = read_presence(project);
    if (!data.contains("actors") || !data["actors"].is_object()) data["actors"] = Json::object();
    data["actors"][actor] = Json::object({{"at", now}});
    Json kept = Json::object();
    for (const auto& [name, item] : data["actors"].items()) {
        const auto at = float_at(item, "at");
        if (at && now - *at < 24 * 3600) kept[name] = item;
    }
    data["actors"] = std::move(kept);
    write_presence(project, data);
}

// --- toollog ---------------------------------------------------------------------------------------------------------------

void record_tool(const fs::path& project, const std::string& actor, const std::string& tool, const Json& kwargs,
                 const std::optional<Json>& result, const std::optional<std::string>& error, double ms, double at) {
    const fs::path path = project / "studio" / "logs" / "tools.jsonl";
    try {
        std::error_code status;
        if (fs::is_regular_file(path, status) && fs::file_size(path, status) > kToolLogBytes && !status) {
            fs::rename(path, path.parent_path() / "tools.1.jsonl");
        }
        Json args = Json::object();  // _summary: numbers, bools and short texts only
        if (kwargs.is_object()) {
            for (const auto& [key, value] : kwargs.items()) {
                if (value.is_boolean() || value.is_number() || (value.is_string() && code_points(value.get<std::string>()) <= 64)) {
                    args[key] = value;
                }
            }
        }
        Json entry = Json::object();
        entry["at"] = core::py_round(at, 3);
        entry["actor"] = actor;
        entry["tool"] = tool;
        entry["args"] = std::move(args);
        entry["ms"] = core::py_round(ms, 1);
        const bool given = result && core::py_truthy(*result) && result->is_object();
        if (error) {
            entry["ok"] = false;
            entry["error"] = first_code_points(*error, 300);
        } else {
            entry["ok"] = given ? core::truthy_at(*result, "ok") : true;
            std::vector<std::string> codes;
            if (given) {
                if (const Json* issues = core::get(*result, "issues"); issues != nullptr && issues->is_array()) {
                    for (const Json& issue : *issues) {
                        if (!issue.is_object() || !core::is_one_of(issue.value("severity", Json()), {"error"})) continue;
                        const Json code = issue.value("code", Json());
                        if (code.is_string()) codes.push_back(code.get<std::string>());
                    }
                }
            }
            std::sort(codes.begin(), codes.end());
            codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
            if (!codes.empty()) entry["codes"] = codes;
            if (given && !core::truthy_at(*result, "ok") && core::truthy_at(*result, "error")) {
                entry["error"] = first_code_points(core::py_str((*result)["error"]), 300);
            }
        }
        storage::append_durable(path, core::dump_python(entry) + "\n");
    } catch (const std::exception&) {
        // logging never breaks a tool
    }
}

std::vector<Json> read_tools(const fs::path& project) {
    std::vector<Json> out;
    for (const char* name : {"tools.1.jsonl", "tools.jsonl"}) {
        const auto text = read_text(project / "studio" / "logs" / name);
        if (!text) continue;
        std::size_t start = 0;
        while (start < text->size()) {  // (lines end at "\n" only: a U+2028 inside a text is not a line's end)
            std::size_t end = text->find('\n', start);
            if (end == std::string::npos) end = text->size();
            const std::string line = text->substr(start, end - start);
            start = end + 1;
            try {
                out.push_back(core::parse_python_json(line));
            } catch (const core::Error&) {
            }
        }
    }
    return out;
}

// --- claims --------------------------------------------------------------------------------------------------------------

namespace {

fs::path claim_file(const fs::path& project, const std::string& item_id) {
    if (!safe_name(item_id) || item_id.find('/') != std::string::npos) {
        throw core::Error("value", "a work item's id must name one file: " + core::py_repr_str(item_id));
    }
    return project / "studio" / "claims" / core::path_from_utf8(item_id + ".json");
}

std::string claim_body(const std::string& actor, double now) {
    return core::dump_python(Json::object({{"actor", actor}, {"expires", now + kLeaseSeconds}}), true);
}

// os.open(path, O_CREAT | O_EXCL | O_WRONLY) and the body written: false when the file is there already.
bool write_exclusive(const fs::path& path, const std::string& body) {
    QFile file(QString::fromStdString(core::path_to_utf8(path)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        std::error_code error;
        if (fs::exists(path, error)) return false;
        throw core::Error("io", "cannot write " + core::path_to_utf8(path));
    }
    if (file.write(body.data(), static_cast<qint64>(body.size())) != static_cast<qint64>(body.size())) {
        throw core::Error("io", "cannot write " + core::path_to_utf8(path));
    }
    return true;
}

}  // namespace

std::optional<std::string> holder(const fs::path& project, const std::string& item_id, double now) {
    const auto data = read_json_quietly(claim_file(project, item_id));
    if (!data) return std::nullopt;
    const auto expires = float_at(*data, "expires");
    if (!expires || *expires < now) return std::nullopt;
    return core::py_str(data->value("actor", Json()));
}

bool claim(const fs::path& project, const std::string& item_id, const std::string& actor, double now) {
    const fs::path path = claim_file(project, item_id);
    const fs::path folder = path.parent_path();
    std::error_code error;
    fs::create_directories(folder, error);
    if (error) throw core::Error("io", "cannot make " + core::path_to_utf8(folder));
    const std::string body = claim_body(actor, now);
    const auto current = holder(project, item_id, now);
    if (current && *current != actor) return false;
    if (current == actor || fs::exists(path, error)) {
        // renew our own claim, or replace an expired one: write a fresh file, then check we won
        std::string who = actor;
        std::replace(who.begin(), who.end(), ':', '_');
        const fs::path tmp = folder / core::path_from_utf8(item_id + "." + std::to_string(QCoreApplication::applicationPid()) + "." + who + ".tmp");
        storage::write_atomic(tmp, body);
        if (!current) {
            fs::remove(path, error);
            if (!write_exclusive(path, body)) {
                fs::remove(tmp, error);
                return holder(project, item_id, now) == actor;
            }
            fs::remove(tmp, error);
            return true;
        }
        fs::rename(tmp, path);
        return true;
    }
    if (!write_exclusive(path, body)) return holder(project, item_id, now) == actor;
    return true;
}

void release(const fs::path& project, const std::string& item_id, const std::string& actor, double now) {
    if (holder(project, item_id, now) == actor) {
        std::error_code error;
        fs::remove(claim_file(project, item_id), error);
    }
}

// --- generation requests ---------------------------------------------------------------------------------------------

std::optional<Json> read_request(const fs::path& project, const std::string& request_id) {
    if (!request_id.starts_with("rq_") || request_id.find('/') != std::string::npos || request_id.find('\\') != std::string::npos ||
        request_id.find("..") != std::string::npos || request_id.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    const fs::path path = project / "studio" / "requests" / core::path_from_utf8(request_id) / "request.json";
    std::error_code error;
    if (!fs::is_regular_file(path, error)) return std::nullopt;
    core::ParseOptions options;
    options.universal_newlines = true;
    return core::parse_python_json(storage::read_file(path), nullptr, options);
}

fs::path write_request(const fs::path& project, const Json& request, const std::map<std::string, std::string>& files) {
    const Json* id = core::get(request, "id");
    if (id == nullptr || !id->is_string() || !safe_name(id->get<std::string>()) || id->get<std::string>().find('/') != std::string::npos) {
        throw core::Error("value", "a request needs an id that names one folder");
    }
    const fs::path folder = project / "studio" / "requests" / core::path_from_utf8(id->get<std::string>());
    for (const auto& [name, data] : files) {
        if (!safe_name(name)) throw core::Error("value", "a request's file must stay in its folder: " + core::py_repr_str(name));
        const fs::path path = folder / core::path_from_utf8(name);
        std::error_code error;
        if (fs::is_regular_file(path, error)) {
            try {
                if (storage::read_file(path) == data) continue;
            } catch (const core::Error&) {
            }
        }
        storage::write_atomic(path, data);
    }
    storage::write_atomic(folder / "request.json", core::dump_python_indent2(request) + "\n");
    storage::make_dirs_durable(project / "studio" / "inbox" / core::path_from_utf8(id->get<std::string>()));
    return folder;
}

// --- tools_registry --------------------------------------------------------------------------------------------------------

namespace {

Json generic_sizes() { return Json::array({Json::array({1024, 1024}), Json::array({1536, 1024}), Json::array({1024, 1536})}); }

Json generic_supports() { return Json::object({{"references", true}, {"edit", true}, {"mask", true}, {"seed", false}}); }

void write_tools(const fs::path& config, const Json& data) {
    storage::write_atomic(config / "tools.json", core::dump_python_indent2(data) + "\n");
}

}  // namespace

Json generic_tool() {
    return Json::object({{"label", "未登録のツール"}, {"sizes_px", generic_sizes()}, {"supports", generic_supports()}});
}

Json load_tools(const fs::path& config) {
    const auto data = read_json_quietly(config / "tools.json");
    return data && data->is_object() ? *data : Json::object();
}

std::pair<std::optional<std::string>, Json> get_tool(const fs::path& config, const std::optional<std::string>& tool_id) {
    if (tool_id && !tool_id->empty()) {
        const Json data = load_tools(config);
        if (const Json* found = core::get(data, *tool_id); found != nullptr && found->is_object()) {
            Json out = generic_tool();
            for (const auto& [key, value] : found->items()) out[key] = value;
            Json supports = generic_supports();
            if (const Json* given = core::get(*found, "supports"); given != nullptr && given->is_object()) {
                for (const auto& [key, value] : given->items()) supports[key] = value;
            }
            out["supports"] = std::move(supports);
            return {*tool_id, out};
        }
    }
    return {std::nullopt, generic_tool()};
}

Json set_tool(const fs::path& config, const std::string& tool_id, const Json& spec) {
    if (tool_id.empty() || !spec.is_object()) throw core::Error("value", "tool id and an object are required");
    const Json sizes = spec.contains("sizes_px") ? spec["sizes_px"] : generic_sizes();
    bool good = sizes.is_array() && !sizes.empty();
    Json kept = Json::array();
    for (std::size_t i = 0; good && i < sizes.size(); ++i) {
        const Json& size = sizes[i];
        if (!size.is_array() || size.size() != 2) {
            good = false;
            break;
        }
        std::vector<std::int64_t> pair;
        for (const Json& v : size) {
            const std::int64_t n = core::to_int(v);  // (int(v): Python's ValueError and TypeError)
            if (n <= 0) {
                good = false;
                break;
            }
            pair.push_back(n);
        }
        if (good) kept.push_back(Json::array({pair[0], pair[1]}));
    }
    if (!good) throw core::Error("value", "sizes_px must be [[w, h], …]");
    Json data = load_tools(config);
    Json entry = spec;
    entry["sizes_px"] = std::move(kept);
    data[tool_id] = entry;
    write_tools(config, data);
    return entry;
}

bool remove_tool(const fs::path& config, const std::string& tool_id) {
    Json data = load_tools(config);
    if (!data.contains(tool_id)) return false;
    data.erase(tool_id);
    write_tools(config, data);
    return true;
}

}  // namespace genko::studio
