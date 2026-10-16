#include "api/headless_server.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <system_error>

#include "api/tokens.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "core/ops_schema.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "formats/export.hpp"
#include "formats/exporting.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/timelapse.hpp"
#include "service/importer.hpp"
#include "service/jobs.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "studio/files.hpp"

namespace genko::api::headless {

namespace {

namespace fs = std::filesystem;
using core::Json;

constexpr std::size_t kMaxBody = 64ULL * 1024 * 1024;  // (MAX_BODY)
constexpr std::size_t kJobsKept = 1024;                // (the apply replies /v1/jobs/{id} answers: the latest ones)

// Forbidden: 403.
struct Forbidden {
    std::string message;
};

Reply json_reply(const Json& payload, int status = 200) { return Reply{status, core::dump_python(payload), {}}; }
Reply failure(const std::string& message, int status) { return json_reply(Json::object({{"ok", false}, {"error", message}}), status); }

Json openapi_spec() {
    const auto path_param = [](const char* name, const char* in, bool required = false) {
        Json p = Json::object({{"name", name}, {"in", in}});
        if (required) p["required"] = true;
        return p;
    };
    Json paths = Json::object();
    paths["/health"] = Json::object({{"get", Json::object({{"security", Json::array()}, {"responses", Json::object({{"200", Json::object({{"description", "ok"}})}})}})}});
    paths["/schema"] = Json::object({{"get", Json::object({{"responses", Json::object({{"200", Json::object({{"description", "ops"}})}})}})}});
    paths["/openapi.json"] = Json::object({{"get", Json::object({{"responses", Json::object({{"200", Json::object({{"description", "spec"}})}})}})}});
    paths["/v1/inspect"] = Json::object({{"get", Json::object({{"parameters", Json::array({path_param("path", "query", true)})}})}});
    paths["/v1/pages/{n}.png"] = Json::object({{"get", Json::object({{"parameters", Json::array({path_param("path", "query")})}})}});
    Json types = Json::object();
    for (const char* t : {"image/png", "image/jpeg", "image/webp"}) types[t] = Json::object();
    paths["/v1/assets"] = Json::object(
        {{"post", Json::object({{"parameters", Json::array({path_param("path", "query")})}, {"requestBody", Json::object({{"content", types}})}})}});
    paths["/v1/requests/{id}/files/{name}"] = Json::object({{"get", Json::object({{"parameters", Json::array({path_param("path", "query")})}})}});
    paths["/v1/pages/{n}/frames/{frame_id}.png"] = Json::object(
        {{"get", Json::object({{"parameters", Json::array({path_param("path", "query"), path_param("dpi", "query"), path_param("mode", "query")})}})}});
    for (const char* p : {"/v1/new", "/v1/apply", "/v1/export"}) {
        paths[p] = Json::object({{"post", Json::object({{"requestBody", Json::object({{"required", true}})}})}});
    }
    paths["/v1/jobs/{id}"] = Json::object({{"get", Json::object({{"parameters", Json::array({path_param("id", "path")})}})}});
    return Json::object({{"openapi", "3.0.3"},
                         {"info", Json::object({{"title", "Genko Headless"}, {"version", "0.3.0"}})},
                         {"components", Json::object({{"securitySchemes", Json::object({{"bearer", Json::object({{"type", "http"}, {"scheme", "bearer"}})}})}})},
                         {"security", Json::array({Json::object({{"bearer", Json::array()}})})},
                         {"paths", std::move(paths)}});
}

// urlparse(target): the path and the query ("http://host/p?q", "//host/p" and "/p?q" alike).
std::pair<std::string, std::string> split_target(const std::string& target) {
    std::string rest = target;
    const std::size_t hash = rest.find('#');
    if (hash != std::string::npos) rest = rest.substr(0, hash);
    const std::size_t scheme = rest.find("://");
    if (scheme != std::string::npos && rest.find_first_of("/?") > scheme) {
        rest = rest.substr(scheme + 1);  // ("//host/…")
    }
    if (rest.rfind("//", 0) == 0) {
        const std::size_t slash = rest.find_first_of("/?", 2);
        rest = slash == std::string::npos ? std::string() : rest.substr(slash);
    }
    const std::size_t question = rest.find('?');
    if (question == std::string::npos) return {rest, ""};
    return {rest.substr(0, question), rest.substr(question + 1)};
}

// The route: the path without trailing "/" ("/" for none).
std::string route_of(const std::string& path) {
    std::string route = path;
    while (!route.empty() && route.back() == '/') route.pop_back();
    return route.empty() ? std::string("/") : route;
}

// {key: values[-1]} of parse_qs (blank values left out, "+" as a space).
std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> out;
    std::size_t start = 0;
    while (start <= query.size()) {
        const std::size_t amp = query.find('&', start);
        const std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            const std::string value = http::percent_decode(pair.substr(eq + 1), true);
            if (!value.empty()) out[http::percent_decode(pair.substr(0, eq), true)] = value;
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return out;
}

// Path.resolve() (strict=False): symbolic links followed as far as the path exists, the rest normalised.
fs::path resolved(const fs::path& path) {
    fs::path out = core::resolve_path(path);
    // (Python 3.12's Path.resolve: a loop of symbolic links is RuntimeError, never a path to serve)
    if (core::symlink_loop(out)) throw Forbidden{"path cannot be resolved: " + core::path_to_utf8(path)};
    return out;
}

bool inside(const fs::path& root, const fs::path& path) {
    auto r = root.begin();
    auto p = path.begin();
    for (; r != root.end(); ++r, ++p) {
        if (p == path.end() || *r != *p) return false;
    }
    return true;
}

// confine: a request's path inside --root (relative paths from root), %XX decoded, links followed.
fs::path confine(const fs::path& root, const Json& raw) {
    const std::string text = http::percent_decode(core::py_truthy(raw) ? core::py_str(raw) : std::string());
    if (text.empty()) throw Forbidden{"path is required"};
    if (text.find('\0') != std::string::npos) throw core::PyValueError("embedded null byte");
    const fs::path candidate = core::path_from_utf8(text);
    const fs::path full = resolved(candidate.is_absolute() ? candidate : root / candidate);
    if (full != root && !inside(root, full)) throw Forbidden{"path outside --root: " + text};
    return full;
}

// _confine_ops: the local files an op reads inside --root, in the ops for_pages repeats too (Python looks only at the
// top level). put_raster's and import_psd's path and a font_path are read as their text whatever they are (str(x), as
// Python's confine reads them); another op's path that is not a text is no file (add_line's points) and is left alone.
// import_pages' book (from) must be a book inside --root: its project.json there (Python reads any book).
Json confine_ops(const fs::path& root, const Json& ops) {
    Json out = Json::array();
    for (const Json& item : ops) {
        if (!item.is_object()) {
            out.push_back(item);
            continue;
        }
        Json op = item;
        const Json name = op.contains("op") ? op["op"] : Json();
        const bool reads_path = name == Json("put_raster") || name == Json("import_psd");
        if (op.contains("path") && core::py_truthy(op["path"]) && (reads_path || op["path"].is_string())) {
            op["path"] = core::path_to_utf8(confine(root, Json(core::py_str(op["path"]))));
        }
        if (op.contains("font_path") && core::py_truthy(op["font_path"])) {
            op["font_path"] = core::path_to_utf8(confine(root, Json(core::py_str(op["font_path"]))));
        }
        if (name == Json("import_pages") && op.contains("from") && core::py_truthy(op["from"])) {
            const std::string text = core::py_str(op["from"]);
            const fs::path book = confine(root, Json(text));
            std::error_code ec;
            if (!fs::is_regular_file(book / "project.json", ec)) {
                throw core::ApplyError("import_pages: from must be a book inside --root (a folder with its project.json): " + text);
            }
            op["from"] = core::path_to_utf8(book);
        }
        if (op.contains("ops") && op["ops"].is_array()) op["ops"] = confine_ops(root, op["ops"]);
        out.push_back(std::move(op));
    }
    return out;
}

// The JSON pointers of the integers past the signed 64 bits in `value` (2^63 to 2^64 - 1: the ones the parser keeps).
void big_integers(const Json& value, const std::string& at, std::vector<std::string>& out) {
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        out.push_back(at);
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) big_integers(value[i], at + "/" + std::to_string(i), out);
    } else if (value.is_object()) {
        for (const auto& [key, item] : value.items()) big_integers(item, at + "/" + core::json_pointer_token(key), out);
    }
}

// _dpi: int(value) in 36–1200 (an integer past 64 bits is past 1200 too).
int dpi_of(const Json& value) {
    if (core::py_big_int_text(value)) throw core::PyValueError("dpi must be 36-1200");
    const std::int64_t dpi = core::py_int(value);
    if (dpi < 36 || dpi > 1200) throw core::PyValueError("dpi must be 36-1200");
    return static_cast<int>(dpi);
}

// int(text) for a page's number in the route ("invalid literal for int() with base 10: …"); one past 64 bits names no
// page (Python's int takes it and finds none): nothing.
std::optional<core::Num> route_int(const std::string& text) {
    if (core::py_big_int_text(Json(text))) return std::nullopt;
    return core::Num(core::py_int(Json(text)));
}

// int(data.get(key, fallback)) for a number in the body; an integer past 64 bits (a number or a text) is refused as
// the body's other odd numbers are.
std::int64_t body_int(const Json& data, const char* key, const Json& fallback) {
    const Json value = data.contains(key) ? data[key] : fallback;
    if (core::py_big_int_text(value)) {
        throw core::ApplyError("the body must not hold NaN, Infinity or an integer past 64 bits (at /" + core::json_pointer_token(key) + ")");
    }
    return core::py_int(value);
}

// html.escape(text, quote=False)
std::string html_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '&') {
            out += "&amp;";
        } else if (c == '<') {
            out += "&lt;";
        } else if (c == '>') {
            out += "&gt;";
        } else {
            out += c;
        }
    }
    return out;
}

bool is_png(std::string_view bytes) { return bytes.size() >= 4 && bytes.substr(0, 4) == "\x89PNG"; }

void refuse_legacy(const fs::path& dir) {
    core::ParseOptions options;
    options.universal_newlines = true;
    const int version = storage::project_version(core::parse_python_json(storage::read_file(dir / "project.json"), nullptr, options));
    if (version < 4) throw storage::NeedsMigration(dir, version);
}

// The book at `dir` read whole (its project.json must be there).
core::Document load(const fs::path& dir) { return storage::load_document(dir).document; }

const core::Page* page_at(const core::Document& doc, const core::Num& index) {
    for (const auto& page : doc.pages) {
        if (page->index == index) return page.get();
    }
    return nullptr;
}

Reply png_reply(const render::Image& image) { return Reply{200, render::write_png(image), {}}; }

}  // namespace

// --- the API -------------------------------------------------------------------------------------------------------------

namespace {

std::mutex g_jobs_mutex;
std::map<std::string, Json> g_jobs;
std::deque<std::string> g_job_order;

struct TokenCache {
    std::mutex mutex;
    std::optional<std::string> bytes;                         // tokens.json as last read (nothing: no file)
    std::vector<std::pair<std::string, std::string>> tokens;  // (the token's SHA-256, hex; its actor)
};
TokenCache g_tokens;

// Whether two texts of the same length are the same, looking at every byte whatever they hold.
bool same_bytes(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char differ = 0;
    for (std::size_t i = 0; i < a.size(); ++i) differ |= static_cast<unsigned char>(a[i] ^ b[i]);
    return differ == 0;
}

// The actor of a token. tokens.json is read on each request and parsed again whenever its bytes changed (its time and
// size may stay the same); the token sent is compared by its SHA-256 with every token's, all of them, so the time an
// answer takes says nothing about how much of a token was right.
std::optional<std::string> actor_of(const std::string& token) {
    std::lock_guard<std::mutex> lock(g_tokens.mutex);
    std::optional<std::string> bytes;
    try {
        std::error_code ec;
        if (fs::is_regular_file(tokens::file(), ec)) bytes = storage::read_file(tokens::file());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "genko serve: tokens.json cannot be read (%s): no token is taken\n", error.what());
        bytes = std::string();
    }
    if (bytes != g_tokens.bytes) {
        g_tokens.tokens.clear();
        try {
            if (bytes) {
                for (const auto& [known, actor] : tokens::parse(*bytes)) g_tokens.tokens.emplace_back(storage::sha256_hex(known), actor);
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "genko serve: tokens.json cannot be read (%s): no token is taken\n", error.what());
            g_tokens.tokens.clear();
        }
        g_tokens.bytes = std::move(bytes);
    }
    const std::string digest = storage::sha256_hex(token);
    std::optional<std::string> found;
    for (const auto& [known, actor] : g_tokens.tokens) {
        if (same_bytes(known, digest)) found = actor;  // (no early return: every token is looked at)
    }
    return found;
}

}  // namespace

Api::Api(Options options) : options_(std::move(options)) {
    try {
        options_.root = resolved(fs::absolute(options_.root));
    } catch (const Forbidden&) {
        throw core::Error("value", "--root " + core::path_to_utf8(options_.root) + " cannot be resolved (a link to itself, or a folder that cannot be read)");
    }
}

void Api::bound(std::uint16_t port) { port_ = port; }

namespace {

Reply upload_asset(const fs::path& root, const std::map<std::string, std::string>& query, const std::string& body) {
    fs::path project;
    try {
        const auto path = query.find("path");
        project = confine(root, path == query.end() ? Json("") : Json(path->second));
    } catch (const Forbidden& forbidden) {
        return failure(forbidden.message, 403);
    }
    std::error_code ec;
    if (!fs::is_regular_file(project / "project.json", ec)) return failure("project not found", 404);
    refuse_legacy(project);  // (a v1–v3 book is not written: genko migrate first)
    std::string blob;
    std::pair<int, int> size;
    try {
        auto normalized = service::importer::normalize(body);
        blob = std::move(std::get<0>(normalized));
        size = std::get<1>(normalized);
    } catch (const service::importer::ImportError& error) {
        return failure(error.what(), 400);
    }
    const std::string ref = storage::AssetStore(project).put_bytes(blob, ".png");
    return json_reply(Json::object({{"ok", true}, {"asset", ref}, {"px", Json::array({size.first, size.second})}}));
}

Reply request_file(const fs::path& root, const std::map<std::string, std::string>& query, const std::string& route) {
    fs::path project;
    try {
        const auto path = query.find("path");
        project = confine(root, path == query.end() ? Json("") : Json(path->second));
    } catch (const Forbidden& forbidden) {
        return failure(forbidden.message, 403);
    }
    const std::size_t files = route.find("/files/");
    const std::string head = route.substr(0, files);
    const std::string name = route.substr(files + 7);
    const std::string request_id = head.substr(head.rfind('/') + 1);
    if (!studio::read_request(project, request_id)) return failure("unknown request", 404);
    const fs::path folder = resolved(project / "studio" / "requests" / core::path_from_utf8(request_id));
    const fs::path target = resolved(folder / core::path_from_utf8(http::percent_decode(name)));
    std::error_code ec;
    // (the request's folder itself inside --root: a link out of studio/requests/ serves nothing)
    if (!inside(root, folder) || target == folder || !inside(folder, target) || !fs::is_regular_file(target, ec)) return failure("unknown file", 404);
    return Reply{200, storage::read_file(target), {}};
}

}  // namespace

bool Api::allowed_origin(const std::string& origin) const {
    return std::find(options_.allow_origins.begin(), options_.allow_origins.end(), origin) != options_.allow_origins.end();
}

// _guard: Host (DNS rebinding), Origin, the body's Content-Type, then the token — a refusal, or the actor.
std::optional<Reply> Api::guard(const http::Request& head, bool has_body, std::string& actor) const {
    const std::string route = route_of(split_target(head.target).first);
    {
        std::string host = head.header("host").value_or("");
        std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool host_ok = false;
        std::vector<std::string> names = {"127.0.0.1", "localhost", "[::1]"};
        for (std::string extra : options_.allow_hosts) {
            std::transform(extra.begin(), extra.end(), extra.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            names.push_back(extra);
            if (host == extra) host_ok = true;
        }
        for (const std::string& name : names) {
            if (host == name + ":" + std::to_string(port_)) host_ok = true;
        }
        if (!host_ok) return failure("unexpected Host header", 421);
    }
    const auto origin = head.header("origin");
    if (origin && !origin->empty() && !allowed_origin(*origin)) return failure("origin not allowed", 403);
    if (has_body) {
        std::string type = head.header("content-type").value_or("");
        type = type.substr(0, type.find(';'));
        const std::size_t a = type.find_first_not_of(" \t");
        const std::size_t b = type.find_last_not_of(" \t");
        type = a == std::string::npos ? std::string() : type.substr(a, b - a + 1);
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (route == "/v1/assets") {
            if (type != "image/png" && type != "image/jpeg" && type != "image/webp") {
                return failure("Content-Type must be image/png, image/jpeg or image/webp", 415);
            }
        } else if (type != "application/json") {
            return failure("Content-Type must be application/json", 415);
        }
    }
    actor = "anonymous";
    if (route != "/health") {
        const std::string auth = head.header("authorization").value_or("");
        std::string token;
        if (auth.size() >= 7) {
            std::string scheme = auth.substr(0, 7);
            std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (scheme == "bearer ") {
                token = auth.substr(7);
                const std::size_t a = token.find_first_not_of(" \t\r\n\v\f");
                const std::size_t b = token.find_last_not_of(" \t\r\n\v\f");
                token = a == std::string::npos ? std::string() : token.substr(a, b - a + 1);
            }
        }
        const auto known = token.empty() ? std::nullopt : actor_of(token);
        if (!known || known->empty()) return failure("Authorization: Bearer <token> is required", 401);
        actor = *known;
    }
    return std::nullopt;
}

bool Api::admit(http::Request& head, http::Responder& r) {
    if (head.method != "GET" && head.method != "POST") return true;  // (OPTIONS answers by itself; others get the 501 page)
    // (do_GET reads no body; do_POST reads Content-Length's bytes, and http.server knows no chunks)
    const auto length = head.header("content-length");
    const bool has_body = head.method == "POST" && length && length->find_first_not_of('0') != std::string::npos;
    std::string actor;
    if (const auto refused = guard(head, has_body, actor)) {
        answer(r, *refused);
        return false;
    }
    head.principal = actor;
    return true;
}

void Api::decorate(const http::Request& head, int status, http::Headers& headers) const {
    (void)status;
    if (head.method != "GET" && head.method != "POST" && head.method != "OPTIONS") return;  // (http.server's own pages)
    const auto origin = head.header("origin");
    if (!origin || origin->empty() || !allowed_origin(*origin)) return;
    for (const auto& [name, value] : headers) {
        (void)value;
        if (name == "Access-Control-Allow-Origin") return;
    }
    headers.emplace_back("Access-Control-Allow-Origin", *origin);
    headers.emplace_back("Vary", "Origin");
}

void Api::answer(http::Responder& r, const Reply& reply) {
    http::Headers headers;
    if (reply.status != 204) headers.emplace_back("Content-Type", is_png(reply.body) ? "image/png" : "application/json; charset=utf-8");
    for (const auto& h : reply.headers) headers.push_back(h);
    r.send(reply.status, headers, reply.body);
}

Reply Api::serve(const http::Request& request) {
    const auto [path, query_text] = split_target(request.target);
    const std::string route = route_of(path);
    const auto origin = request.header("origin");
    if (request.method == "OPTIONS") {
        if (!origin || origin->empty() || !allowed_origin(*origin)) return failure("origin not allowed", 403);
        return Reply{204,
                     "",
                     {{"Access-Control-Allow-Origin", *origin},
                      {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
                      {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
                      {"Vary", "Origin"}}};
    }
    if (request.method != "GET" && request.method != "POST") {
        return Reply{501, "", {}};  // (the handler's 501 page)
    }
    std::string actor = request.principal;
    if (actor.empty()) {  // (not admitted from its head: the guard now, the body read)
        if (const auto refused = guard(request, request.method == "POST" && !request.body.empty(), actor)) return *refused;
    }
    try {
        return route_request(request.method, route, parse_query(query_text), request.body, actor);
    } catch (const Forbidden& forbidden) {
        return failure(forbidden.message, 403);
    }
}

Reply Api::route_request(const std::string& method, const std::string& route, const std::map<std::string, std::string>& query,
                         const std::string& body, const std::string& actor) {
    try {
        if (route == "/v1/assets" && method == "POST") return upload_asset(options_.root, query, body);
        Json data = Json::object();
        std::vector<std::string> odd;
        if (!body.empty()) {
            if (core::utf8_error(body)) return failure("invalid JSON body", 400);
            core::ParseRepairs repairs;
            try {
                data = core::parse_python_json(body, &repairs);  // (deeper than 1000 levels: not taken)
            } catch (const core::Error&) {
                return failure("invalid JSON body", 400);
            }
            if (!data.is_object()) return failure("body must be a JSON object", 400);
            odd = repairs.nonfinite;
            odd.insert(odd.end(), repairs.inexact.begin(), repairs.inexact.end());
            big_integers(data, "", odd);  // (2^63 to 2^64 - 1, which the parser keeps)
        }
        return routes(method, route, query, data, odd, actor);
    } catch (const Forbidden& forbidden) {
        return failure(forbidden.message, 403);
    } catch (const storage::UnsupportedProjectVersion& error) {
        return failure(error.what(), 409);
    } catch (const render::NotYetPorted& error) {
        // (what this build does not do yet: said, never done halfway)
        return json_reply(Json::object({{"ok", false}, {"error", error.what()}, {"code", error.code()}, {"element", error.element()}}), 501);
    } catch (const core::ApplyError& error) {
        // An exception an op raised that Python's apply_ops lets through (this build's CommandBus writes it as
        // "ops[i] <op>: <Type>: <words>"): handle_request's own statuses for it, its words without the op's name.
        static const std::regex kRaised(R"(^ops\[\d+\] [A-Za-z0-9_]+: ([A-Za-z]+Error|StopIteration)(?:: ([\s\S]*))?$)");
        std::smatch raised;
        const std::string message = error.what();
        if (error.code() == "python_error" && std::regex_match(message, raised, kRaised)) {
            const std::string type = raised[1].str();
            const std::string words = raised[2].matched ? raised[2].str() : std::string();
            if (type == "FileNotFoundError") return failure("not found: " + words, 404);
            if (type == "KeyError" || type == "TypeError" || type == "ValueError" || type == "StopIteration") return failure(words, 400);
            // (Python would end the connection with a traceback: this build says what failed)
            return json_reply(Json::object({{"ok", false}, {"error", type + ": " + words}, {"code", "python_error"}}), 500);
        }
        std::string lower = error.what();
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return failure(error.what(), lower.find("locked") != std::string::npos ? 409 : 400);
    } catch (const core::Error& error) {
        const std::string& code = error.code();
        if (code == "locked" || code == "needs_migration") return json_reply(Json::object({{"ok", false}, {"error", error.what()}, {"code", code}}), 409);
        if (code == "not_found") return failure("not found: " + std::string(error.what()), 404);
        if (code == "key" || code == "value" || code == "type" || code == "json" || code == "format") return failure(error.what(), 400);
        if (code == "exists") return json_reply(Json::object({{"ok", false}, {"error", error.what()}, {"code", code}}), 409);
        // (Python would end the connection with a traceback: this build says what failed)
        const std::string message = code == "python_error" ? dynamic_cast<const core::PyUncaught*>(&error) != nullptr
                                                                  ? dynamic_cast<const core::PyUncaught&>(error).type() + ": " + error.what()
                                                                  : std::string(error.what())
                                                            : std::string(error.what());
        return json_reply(Json::object({{"ok", false}, {"error", message}, {"code", code}}), 500);
    }
}

Reply Api::routes(const std::string& method, const std::string& route, const std::map<std::string, std::string>& query, const Json& data,
                  const std::vector<std::string>& odd_numbers, const std::string& actor) {
    const fs::path& root = options_.root;
    const auto q = [&query](const char* key, const char* fallback) {
        const auto found = query.find(key);
        return found == query.end() ? std::string(fallback) : found->second;
    };
    const auto refuse_odd = [&odd_numbers](const char* what) {
        if (odd_numbers.empty()) return;
        throw core::ApplyError(std::string(what) + " must not hold NaN, Infinity or an integer past 64 bits (at " + odd_numbers.front() + ")");
    };
    if (method == "GET" && route == "/health") return json_reply(Json::object({{"ok", true}, {"service", "genko-headless"}}));
    if (method == "GET" && route == "/schema") return json_reply(Json::object({{"ok", true}, {"ops", core::ops_schema()}}));
    if (method == "GET" && route == "/openapi.json") return json_reply(openapi_spec());
    if (method == "GET" && route.rfind("/v1/jobs/", 0) == 0) {
        const std::string id = route.substr(route.rfind('/') + 1);
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        const auto found = g_jobs.find(id);
        if (found == g_jobs.end()) return failure("unknown job", 404);
        return json_reply(found->second);
    }
    if (method == "GET" && route == "/v1/inspect") {
        const core::Document doc = load(confine(root, Json(q("path", ""))));
        const std::string full = q("full", "");
        return json_reply(storage::snapshot(doc, full == "1" || full == "true" || full == "yes"));
    }
    if (method == "GET" && route.rfind("/v1/requests/", 0) == 0 && route.find("/files/") != std::string::npos) return request_file(root, query, route);
    if (method == "GET" && route.rfind("/v1/pages/", 0) == 0 && route.find("/frames/") != std::string::npos && route.ends_with(".png")) {
        const core::Document doc = load(confine(root, Json(q("path", ""))));
        const std::size_t frames = route.find("/frames/");
        const std::string head = route.substr(0, frames);
        std::string frame_id = route.substr(frames + 8);
        const std::string number = head.substr(head.rfind('/') + 1);
        const auto index = route_int(number);
        const core::Page* page = index ? page_at(doc, *index) : nullptr;
        if (page == nullptr) return failure("no page " + (index ? index->repr() : number), 404);
        frame_id = frame_id.substr(0, frame_id.size() - 4);
        render::RenderOptions options = render::proof_options();
        options.mode = q("mode", "proof");
        const int dpi = dpi_of(Json(q("dpi", "150")));
        try {
            return png_reply(render::render_frame(*page, frame_id, dpi, options, &doc));
        } catch (const core::Error& error) {
            if (error.code() != "key") throw;
            return failure("no frame " + frame_id + " on page " + index->repr(), 404);
        }
    }
    if (method == "GET" && route.rfind("/v1/pages/", 0) == 0 && route.ends_with(".png")) {
        const core::Document doc = load(confine(root, Json(q("path", ""))));
        std::string last = route.substr(route.rfind('/') + 1);
        last = last.substr(0, last.size() - 4);
        const auto index = route_int(last);
        const core::Page* page = index ? page_at(doc, *index) : nullptr;
        if (page == nullptr) return failure("", 400);  // (StopIteration: no words)
        render::RenderOptions options;
        options.mode = q("mode", "print");
        return png_reply(render::render_page(*page, dpi_of(Json(q("dpi", "150"))), options, &doc).image);
    }
    if (method == "GET" && route.rfind("/v1/spreads/", 0) == 0 && route.ends_with(".png")) {
        const core::Document doc = load(confine(root, Json(q("path", ""))));
        std::string pair = route.substr(route.rfind('/') + 1);
        pair = pair.substr(0, pair.size() - 4);
        std::vector<std::string> parts;
        std::size_t start = 0;
        for (;;) {
            const std::size_t dash = pair.find('-', start);
            parts.push_back(pair.substr(start, dash == std::string::npos ? std::string::npos : dash - start));
            if (dash == std::string::npos) break;
            start = dash + 1;
        }
        if (parts.size() != 2) {
            throw core::PyValueError(parts.size() < 2 ? "not enough values to unpack (expected 2, got " + std::to_string(parts.size()) + ")"
                                                      : "too many values to unpack (expected 2)");
        }
        const auto left = route_int(parts[0]);
        const auto right = route_int(parts[1]);
        const int dpi = dpi_of(Json(q("dpi", "150")));
        render::RenderOptions options;
        options.mode = q("mode", "print");
        // (a page's number past 64 bits names none: as a number no page has)
        return png_reply(render::render_spread(doc, left.value_or(core::Num(0)), right.value_or(core::Num(0)), dpi, options));
    }
    if (method == "POST" && route == "/v1/new") {
        refuse_odd("the body");
        if (!data.contains("dest")) throw core::Error("key", "'dest'");
        const fs::path dest = confine(root, data["dest"]);
        const core::PageSpec spec = core::py_truthy(core::get_or(data, "webtoon", Json())) ? core::PageSpec::webtoon() : core::PageSpec::a4_mono();
        const std::string title = core::py_str(data.contains("title") ? data["title"] : Json("無題"));
        const std::int64_t episode = body_int(data, "episode", Json(1));
        const std::int64_t pages = body_int(data, "pages", Json(8));
        std::error_code ec;
        const auto exists = [&] { return fs::exists(dest / "project.json", ec); };
        if (exists()) {
            throw core::Error("exists", "a book is there already: " + core::path_to_utf8(dest / "project.json") + " (/v1/new never writes over a book)");
        }
        core::Document doc = core::new_episode(title, core::Num(episode), static_cast<int>(std::clamp<std::int64_t>(pages, -1, 100000)), spec);
        storage::ProjectLock lock(dest, actor);
        lock.try_acquire();
        if (exists()) throw core::Error("exists", "a book is there already: " + core::path_to_utf8(dest / "project.json"));
        storage::SaveRequest save;
        save.actor = actor;
        save.base_revision = 0;
        save.ops = Json::array();
        storage::Saver(lock).save(doc, save);
        lock.release();
        return json_reply(Json::object({{"ok", true}, {"path", core::path_to_utf8(dest)}, {"snapshot", storage::snapshot(doc)}}));
    }
    if (method == "POST" && route == "/v1/apply") {
        if (!data.contains("path")) throw core::Error("key", "'path'");
        const fs::path project = confine(root, data["path"]);
        // data.get("ops") or []: a list as it is, a dict's keys, a str's characters (then refused one by one)
        Json ops = Json::array();
        const Json given = core::get_or(data, "ops", Json());
        if (core::py_truthy(given)) {
            if (given.is_array()) {
                ops = given;
            } else if (given.is_object() || given.is_string()) {
                ops = core::py_list(given);
            } else {
                throw core::PyTypeError("'" + core::py_type_name(given) + "' object is not iterable");
            }
        }
        ops = confine_ops(root, ops);
        for (const std::string& where : odd_numbers) {  // (the M1 words for NaN and Infinity in ops; integers past 64 bits)
            if (where.rfind("/ops/", 0) != 0) continue;
            const Json found = data.value(Json::json_pointer(where), Json());
            throw core::ApplyError(found.is_number() ? "ops must not hold an integer past 64 bits (at " + where.substr(4) + ")"
                                                     : "ops must not hold NaN or Infinity (at " + where.substr(4) + ")");
        }
        refuse_odd("the body");
        const bool dry_run = core::py_truthy(core::get_or(data, "dry_run", Json()));
        refuse_legacy(project);  // (a v1–v3 book is not written: genko migrate first; not_found when it is not a book)
        storage::ProjectLock lock(project, actor);
        lock.try_acquire();
        storage::journal::repair(project);  // (the book is read at its last consistent point)
        const storage::LoadResult loaded = storage::load_document(project);
        if (loaded.report.source_version < 4) throw storage::NeedsMigration(project, loaded.report.source_version);
        const Json expected = core::get_or(data, "expect_revision", Json());
        if (!expected.is_null() && loaded.document.revision != body_int(data, "expect_revision", Json())) {
            return failure("revision conflict: expected " + core::py_str(expected) + ", found " + std::to_string(loaded.document.revision), 409);
        }
        const core::CommandBus bus(render::ops_registry());
        const core::ApplyResult result = bus.apply(loaded.document, ops, core::Actor(actor), dry_run);
        Json out = Json::object();
        out["ok"] = true;
        out["applied"] = result.applied;
        out["snapshot"] = storage::snapshot(result.doc);
        out["job_id"] = core::new_id();
        if (result.has_warnings) out["warnings"] = result.warnings;
        if (!result.results.empty()) out["results"] = result.results;
        if (!dry_run) {
            storage::SaveRequest save;
            save.actor = actor;
            save.base_revision = loaded.document.revision;
            save.ops = result.journal_ops;
            const bool lapse = render::timelapse::is_on(result.doc);  // (タイムラプス: the pages this save changes)
            Json before;
            if (lapse) {
                try {
                    before = storage::read_disk_state(project).payload;
                } catch (const std::exception&) {  // (the timelapse never stops a save: every page is recorded then)
                }
            }
            const storage::SaveResult saved = storage::Saver(lock).save(result.doc, save);
            if (lapse && !saved.already_committed) render::timelapse::after_save(project, result.doc, before);
        }
        lock.release();
        {
            std::lock_guard<std::mutex> guard(g_jobs_mutex);
            const std::string id = out["job_id"].get<std::string>();
            g_jobs[id] = out;
            g_job_order.push_back(id);
            while (g_job_order.size() > kJobsKept) {
                g_jobs.erase(g_job_order.front());
                g_job_order.pop_front();
            }
        }
        return json_reply(out);
    }
    if (method == "POST" && route == "/v1/export") {
        refuse_odd("the body");
        if (!data.contains("path")) throw core::Error("key", "'path'");
        const fs::path project = confine(root, data["path"]);
        if (!data.contains("out")) throw core::Error("key", "'out'");
        const fs::path out = confine(root, data["out"]);
        const std::string fmt = core::py_str(data.contains("format") ? data["format"] : Json("png"));
        if (fmt != "png" && fmt != "tiff" && fmt != "pdf") return failure("format must be png, tiff or pdf", 400);
        const core::Document doc = load(project);
        const Json given = core::get_or(data, "dpi", Json());
        const int dpi = dpi_of(core::py_truthy(given) ? given : doc.spec.dpi.json());
        const std::vector<fs::path> files = formats::export_print(doc, out, fmt, dpi);
        Json listed = Json::array();
        for (const fs::path& f : files) listed.push_back(core::path_to_utf8(f));
        Json reply = Json::object({{"ok", true}, {"count", files.size()}, {"files", listed}});
        if (const auto notes = formats::export_notes(doc, fmt, "auto"); !notes.empty()) reply["notes"] = notes;  // (color-mode@1)
        return json_reply(reply);
    }
    return failure("no route " + method + " " + route, 404);
}

int serve(const Options& options) {
    http::install_stop_handlers();
    if (tokens::load().empty()) {
        const std::string first = tokens::add("human:owner");
        std::fprintf(stderr, "created token for human:owner: %s\n", first.c_str());
    }
    Api api(options);
    http::Limits limits;
    limits.max_body = kMaxBody;
    limits.too_large = [](http::Responder& r) {
        r.send(413, {{"Content-Type", "application/json; charset=utf-8"}}, core::dump_python(Json::object({{"ok", false}, {"error", "body too large"}})));
    };
    // the guard from the head, before the body is read; CORS on every answer to an allowed Origin (_send)
    limits.admit = [&api](http::Request& head, http::Responder& r) { return api.admit(head, r); };
    limits.decorate = [&api](const http::Request& head, int status, http::Headers& headers) { api.decorate(head, status, headers); };
    http::Server server(
        [&api](const http::Request& request, http::Responder& r) {
            if (request.method != "GET" && request.method != "POST" && request.method != "OPTIONS") {
                // (http.server's own page for a method its handler has no do_<METHOD> for: send_error(501, "Unsupported
                // method (%r)"), the message HTML-escaped)
                const std::string page = "<!DOCTYPE HTML>\n<html lang=\"en\">\n    <head>\n        <meta charset=\"utf-8\">\n"
                                         "        <title>Error response</title>\n    </head>\n    <body>\n        <h1>Error response</h1>\n"
                                         "        <p>Error code: 501</p>\n        <p>Message: " +
                                         html_escape("Unsupported method (" + core::py_repr_str(request.method) + ")") +
                                         ".</p>\n        <p>Error code explanation: 501 - Server does not support this operation.</p>\n"
                                         "    </body>\n</html>\n";
                r.send(501, {{"Content-Type", "text/html;charset=utf-8"}}, page);
                return;
            }
            Api::answer(r, api.serve(request));
        },
        limits);
    if (const auto error = server.listen(options.host, options.port)) {
        std::fprintf(stderr, "genko serve: %s\n", error->c_str());
        return 1;
    }
    api.bound(server.port());
    std::fprintf(stderr, "genko headless http://%s:%u root=%s\n", options.host.c_str(), static_cast<unsigned>(server.port()),
                 core::path_to_utf8(options.root).c_str());
    std::fflush(stderr);
    server.run(http::stop_flag());
    service::jobs::wait_all();
    return 0;
}

}  // namespace genko::api::headless
