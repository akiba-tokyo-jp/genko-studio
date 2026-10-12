#include "api/mcp_server.hpp"

#include <algorithm>
#include <iterator>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <string_view>
#include <utility>

#include "core/error.hpp"
#include "core/pyconv.hpp"

namespace genko::api::mcp {

namespace {

constexpr std::array<std::string_view, 4> kHandshakeVersions{"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};
constexpr std::string_view kVersionKey = "io.modelcontextprotocol/protocolVersion";
constexpr std::string_view kCapabilitiesKey = "io.modelcontextprotocol/clientCapabilities";
constexpr std::string_view kClientInfoKey = "io.modelcontextprotocol/clientInfo";
constexpr std::string_view kServerInfoKey = "io.modelcontextprotocol/serverInfo";

constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kRequestCancelled = -32800;
constexpr int kHeaderMismatch = -32020;
constexpr int kUnsupportedVersion = -32022;

// The tool calls served at once (anyio's thread limiter: a sync tool runs on one of 40 worker threads).
constexpr int kToolThreads = 40;

RpcError invalid_params() { return RpcError{kInvalidParams, "Invalid request parameters", Json("")}; }
RpcError method_not_found(const std::string& method) { return RpcError{kMethodNotFound, "Method not found", Json(method)}; }

bool is_int_like(const Json& v) {
    if (v.is_boolean() || v.is_number_integer()) return true;
    return v.is_number_float() && std::isfinite(v.get<double>()) && v.get<double>() == std::floor(v.get<double>());
}

// --- the params the SDK's request models take (the fields Genko's methods read, with their types) ----------------------

bool object_or_null(const Json& v) { return v.is_object() || v.is_null(); }
bool string_or_null(const Json& v) { return v.is_string() || v.is_null(); }

// Meta: an object (or null) whose progressToken is a str or an int.
bool meta_ok(const Json& params) {
    if (!params.is_object() || !params.contains("_meta")) return true;
    const Json& meta = params["_meta"];
    if (meta.is_null()) return true;
    if (!meta.is_object()) return false;
    if (meta.contains("progressToken") && !(meta["progressToken"].is_string() || is_int_like(meta["progressToken"]))) return false;
    return true;
}

bool implementation_ok(const Json& v) {
    if (!v.is_object()) return false;
    if (!v.contains("name") || !v["name"].is_string() || !v.contains("version") || !v["version"].is_string()) return false;
    for (const char* key : {"title", "websiteUrl", "description"}) {
        if (v.contains(key) && !string_or_null(v[key])) return false;
    }
    if (v.contains("icons") && !(v["icons"].is_array() || v["icons"].is_null())) return false;
    return true;
}

bool capabilities_ok(const Json& v) {
    if (!v.is_object()) return false;
    for (const auto& [key, value] : v.items()) {
        if (key == "experimental") {
            if (value.is_null()) continue;
            if (!value.is_object()) return false;
            for (const auto& [name, inner] : value.items()) {
                (void)name;
                if (!inner.is_object()) return false;
            }
        } else if (key == "roots" || key == "sampling" || key == "elicitation" || key == "tasks" || key == "extensions") {
            if (!object_or_null(value)) return false;
        }
    }
    return true;
}

// validate_client_request: whether the params fit the method's request model at this version. Nothing for a method the
// version does not define (the caller answers "Method not found").
std::optional<bool> params_fit(const std::string& method, const Json& raw, bool modern) {
    const Json params = raw.is_null() ? Json::object() : raw;
    if (!params.is_object()) return false;
    if (!meta_ok(params)) return false;
    if (modern) {
        // (RequestMetaObject: the envelope's capabilities and client info as their models take them)
        const Json& meta = params.contains("_meta") ? params["_meta"] : Json();
        if (!meta.is_object()) return false;
        const std::string caps(kCapabilitiesKey);
        const std::string info(kClientInfoKey);
        if (!meta.contains(caps) || !capabilities_ok(meta[caps])) return false;
        if (meta.contains(info) && !implementation_ok(meta[info])) return false;
    }
    const auto has_string = [&params](const char* key) { return params.contains(key) && params[key].is_string(); };
    const auto field = [&params](const char* key) { return params.contains(key) ? params[key] : Json(); };
    static const std::set<std::string_view> kLegacyOnly{"initialize", "ping", "logging/setLevel", "resources/subscribe", "resources/unsubscribe"};
    static const std::set<std::string_view> kModernOnly{"server/discover", "subscriptions/listen"};
    static const std::set<std::string_view> kBoth{"tools/list", "tools/call", "resources/list", "resources/templates/list", "resources/read",
                                                  "prompts/list", "prompts/get", "completion/complete"};
    if (modern ? kLegacyOnly.contains(method) : kModernOnly.contains(method)) return std::nullopt;
    if (!kLegacyOnly.contains(method) && !kModernOnly.contains(method) && !kBoth.contains(method)) return std::nullopt;
    if (method == "initialize") {
        return has_string("protocolVersion") && params.contains("capabilities") && capabilities_ok(params["capabilities"]) &&
               params.contains("clientInfo") && implementation_ok(params["clientInfo"]);
    }
    if (method == "tools/list" || method == "resources/list" || method == "resources/templates/list" || method == "prompts/list") {
        return string_or_null(field("cursor"));
    }
    if (method == "tools/call") {
        return has_string("name") && object_or_null(field("arguments")) && object_or_null(field("task")) &&
               (!modern || string_or_null(field("requestState")));
    }
    if (method == "resources/read" || method == "resources/subscribe" || method == "resources/unsubscribe") {
        return has_string("uri") && (!modern || string_or_null(field("requestState")));
    }
    if (method == "prompts/get") {
        const Json arguments = field("arguments");
        if (arguments.is_object()) {
            for (const auto& [key, value] : arguments.items()) {
                (void)key;
                if (!value.is_string()) return false;
            }
        } else if (!arguments.is_null()) {
            return false;
        }
        return has_string("name");
    }
    if (method == "logging/setLevel") {
        static const std::set<std::string> kLevels{"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"};
        return has_string("level") && kLevels.contains(params["level"].get<std::string>());
    }
    if (method == "completion/complete") {
        const Json ref = field("ref");
        const Json argument = field("argument");
        if (!ref.is_object() || !ref.contains("type") || !ref["type"].is_string()) return false;
        const std::string type = ref["type"].get<std::string>();
        if (!((type == "ref/prompt" && ref.contains("name") && ref["name"].is_string()) ||
              (type == "ref/resource" && ref.contains("uri") && ref["uri"].is_string()))) {
            return false;
        }
        return argument.is_object() && argument.contains("name") && argument["name"].is_string() && argument.contains("value") &&
               argument["value"].is_string();
    }
    if (method == "subscriptions/listen") return field("notifications").is_object();
    return true;  // (ping, server/discover: RequestParams)
}

// The methods MCPServer has a handler for.
bool served(std::string_view method) {
    static const std::set<std::string_view> kServed{"ping", "server/discover", "prompts/list", "prompts/get", "resources/list",
                                                    "resources/templates/list", "resources/read", "subscriptions/listen", "tools/list",
                                                    "tools/call"};
    return kServed.contains(method);
}

Json server_info() { return Json::object({{"name", "genko"}, {"version", ""}}); }

// The 2026 era's results: resultType, the cache hints the SDK fills for a listing, and the serverInfo stamp.
Json modern_result(Json result, bool cacheable) {
    Json out = Json::object();
    if (cacheable) out["cacheScope"] = "private";
    for (const auto& [key, value] : result.items()) out[key] = value;
    out["resultType"] = "complete";
    if (cacheable) out["ttlMs"] = 0;
    // (the generated models' fields are in name order; _meta is last)
    Json sorted = Json::object();
    std::vector<std::string> keys;
    for (const auto& [key, value] : out.items()) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    for (const std::string& key : keys) sorted[key] = out[key];
    sorted["_meta"] = Json::object({{std::string(kServerInfoKey), server_info()}});
    return sorted;
}

Json tool_entry(const ToolSpec& tool, bool modern) {
    Json schema = input_schema(tool);
    if (modern) {  // (the 2026 surface's inputSchema: type, properties, required)
        Json reordered = Json::object({{"type", schema["type"]}, {"properties", schema["properties"]}});
        if (schema.contains("required")) reordered["required"] = schema["required"];
        schema = std::move(reordered);
    }
    return Json::object({{"description", std::string(tool.description)}, {"inputSchema", std::move(schema)}, {"name", std::string(tool.name)}});
}

Json capabilities(bool modern) {
    if (modern) {
        return Json::object({{"prompts", Json::object({{"listChanged", true}})},
                             {"resources", Json::object({{"listChanged", true}, {"subscribe", true}})},
                             {"tools", Json::object({{"listChanged", true}})}});
    }
    return Json::object({{"experimental", Json::object()},
                         {"prompts", Json::object({{"listChanged", false}})},
                         {"resources", Json::object({{"listChanged", false}, {"subscribe", false}})},
                         {"tools", Json::object({{"listChanged", false}})}});
}

}  // namespace

bool handshake_version(std::string_view version) {
    return std::find(kHandshakeVersions.begin(), kHandshakeVersions.end(), version) != kHandshakeVersions.end();
}

Json RpcError::to_json() const {
    Json out = Json::object({{"code", code}, {"message", message}});
    if (data) out["data"] = *data;
    return out;
}

std::optional<Inbound> decode(std::string_view text) {
    Inbound out;
    core::ParseRepairs repairs;
    core::ParseOptions options;
    options.max_depth = 255;  // (the SDK's JSON reader stops deeper than this)
    try {
        out.body = core::parse_python_json(text, &repairs, options);
    } catch (const core::Error&) {
        return std::nullopt;
    }
    out.odd_numbers = repairs.nonfinite;
    out.odd_numbers.insert(out.odd_numbers.end(), repairs.inexact.begin(), repairs.inexact.end());
    return out;
}

Shape shape_of(const Json& body) {
    if (!body.is_object()) return Shape::Invalid;
    const auto version = body.find("jsonrpc");
    if (version == body.end() || *version != Json("2.0")) return Shape::Invalid;
    const auto id = body.find("id");
    const bool good_id = id != body.end() && (id->is_string() || (id->is_number_integer()));
    const auto method = body.find("method");
    if (method != body.end() && method->is_string()) {
        const auto params = body.find("params");
        if (params != body.end() && !params->is_object() && !params->is_null()) return Shape::Invalid;
        return good_id ? Shape::Request : Shape::Notification;
    }
    if (body.contains("result") && body["result"].is_object() && good_id) return Shape::Response;
    if (body.contains("error") && body["error"].is_object() && (good_id || (id != body.end() && id->is_null()))) return Shape::Response;
    return Shape::Invalid;
}

std::string encode(const Json& message) {
    core::DumpOptions options;
    options.item_separator = ",";
    options.key_separator = ":";
    return core::dump(message, options);
}

std::string id_key(const Json& id) {
    if (id.is_string()) {
        try {
            return "i" + std::to_string(core::py_int(id));  // (int("7"): the same request as 7)
        } catch (const std::exception&) {
            return "s" + id.get<std::string>();
        }
    }
    return "i" + core::py_str(id);
}

bool has_modern_envelope(const Json& params) {
    return params.is_object() && params.contains("_meta") && params["_meta"].is_object() && params["_meta"].contains(std::string(kVersionKey));
}

std::optional<RpcError> classify_modern(const Json& body, const std::map<std::string, std::string>* headers) {
    const Json* meta = nullptr;
    if (body.contains("params") && body["params"].is_object() && body["params"].contains("_meta")) meta = &body["params"]["_meta"];
    if (meta == nullptr || !meta->is_object()) {
        return RpcError{kInvalidParams,
                        "params._meta must be an object carrying the required 'io.modelcontextprotocol/protocolVersion' and "
                        "'io.modelcontextprotocol/clientCapabilities' envelope keys",
                        std::nullopt};
    }
    std::string missing;
    for (std::string_view key : {kVersionKey, kCapabilitiesKey}) {
        if (!meta->contains(std::string(key))) missing += (missing.empty() ? "" : ", ") + std::string(key);
    }
    if (!missing.empty()) return RpcError{kInvalidParams, "params._meta is missing the required envelope key(s): " + missing, std::nullopt};
    const Json& version = (*meta)[std::string(kVersionKey)];
    if (headers != nullptr) {
        const auto header = headers->find("mcp-protocol-version");
        if (header == headers->end() || !version.is_string() || header->second != version.get<std::string>()) {
            return RpcError{kHeaderMismatch, "mcp-protocol-version header does not match the request envelope's protocol version", std::nullopt};
        }
        const auto method = headers->find("mcp-method");
        if (method == headers->end() || !body["method"].is_string() || method->second != body["method"].get<std::string>()) {
            return RpcError{kHeaderMismatch, "mcp-method header does not match the request body's method", std::nullopt};
        }
        const std::string m = body["method"].get<std::string>();
        const char* name_key = m == "tools/call" || m == "prompts/get" ? "name" : m == "resources/read" ? "uri" : nullptr;
        if (name_key != nullptr) {
            const Json& params = body["params"];
            if (params.contains(name_key) && !params[name_key].is_null()) {
                const auto name = headers->find("mcp-name");
                std::optional<std::string> decoded;
                if (name != headers->end()) decoded = name->second;  // (the =?base64?…?= form is decoded by the transport)
                if (!decoded || !params[name_key].is_string() || *decoded != params[name_key].get<std::string>()) {
                    return RpcError{kHeaderMismatch, std::string("mcp-name header does not match the request body's '") + name_key + "' parameter",
                                    std::nullopt};
                }
            }
        }
    }
    if (!version.is_string()) return RpcError{kInvalidParams, "the protocol-version envelope value must be a string", std::nullopt};
    if (version.get<std::string>() != kModernVersion) {
        return RpcError{kUnsupportedVersion, "Unsupported protocol version",
                        Json::object({{"supported", Json::array({std::string(kModernVersion)})}, {"requested", version}})};
    }
    return std::nullopt;
}

// --- the engine ------------------------------------------------------------------------------------------------------

Engine::Engine(fs::path root, std::string actor) : tools_(std::move(root), std::move(actor)) {}

std::pair<Json, std::string> Engine::initialize(const Json& params, const std::string& version) const {
    (void)version;
    const auto fit = params_fit("initialize", params, false);
    if (!fit || !*fit) throw invalid_params();
    const std::string requested = params["protocolVersion"].get<std::string>();
    const std::string negotiated = handshake_version(requested) ? requested : std::string(kLatestHandshake);
    Json result = Json::object({{"capabilities", capabilities(false)},
                                {"instructions", std::string(instructions())},
                                {"protocolVersion", negotiated},
                                {"serverInfo", server_info()}});
    return {std::move(result), negotiated};
}

Json Engine::handle(const std::string& method, const Json& params, const std::string& version, bool initialized,
                    const std::vector<std::string>& odd_numbers) const {
    const bool modern = version == kModernVersion;
    const auto fit = params_fit(method, params, modern);
    if (fit && !*fit) throw invalid_params();
    if (!fit && (method == "initialize" || method == "ping" || method == "server/discover" || method == "subscriptions/listen" ||
                 method == "logging/setLevel" || method == "resources/subscribe" || method == "resources/unsubscribe")) {
        throw method_not_found(method);  // (a method this version does not define)
    }
    if (!served(method)) throw method_not_found(method);
    if (!modern && !initialized && method != "ping") throw invalid_params();  // (the init gate)
    const Json p = params.is_null() ? Json::object() : params;
    const auto finish = [modern](Json result, bool cacheable) { return modern ? modern_result(std::move(result), cacheable) : result; };
    if (method == "ping") return Json::object();
    if (method == "server/discover") {
        return finish(Json::object({{"capabilities", capabilities(true)},
                                    {"instructions", std::string(instructions())},
                                    {"supportedVersions", Json::array({std::string(kModernVersion)})}}),
                      true);
    }
    if (method == "tools/list") {
        Json tools = Json::array();
        for (const ToolSpec& tool : tool_specs()) tools.push_back(tool_entry(tool, modern));
        return finish(Json::object({{"tools", std::move(tools)}}), true);
    }
    if (method == "resources/list") {
        Json listed = Json::array();
        for (const ResourceSpec& r : resource_specs()) {
            listed.push_back(Json::object({{"description", std::string(r.description)},
                                           {"mimeType", std::string(r.mime_type)},
                                           {"name", std::string(r.name)},
                                           {"uri", std::string(r.uri)}}));
        }
        return finish(Json::object({{"resources", std::move(listed)}}), true);
    }
    if (method == "resources/templates/list") return finish(Json::object({{"resourceTemplates", Json::array()}}), true);
    if (method == "prompts/list") return finish(Json::object({{"prompts", Json::array()}}), true);
    if (method == "prompts/get") {
        // (MCPServer raises ValueError: the handshake era answers code 0 with its words, the 2026 era a bare internal error)
        if (modern) throw RpcError{-32603, "Internal server error", std::nullopt};
        throw RpcError{0, "Unknown prompt: " + p["name"].get<std::string>(), std::nullopt};
    }
    if (method == "resources/read") {
        const std::string uri = p["uri"].get<std::string>();
        const ResourceSpec* spec = nullptr;
        for (const ResourceSpec& r : resource_specs()) {
            if (r.uri == uri) spec = &r;
        }
        const auto text = read_resource(uri);
        if (spec == nullptr || !text) throw RpcError{kInvalidParams, "Unknown resource: " + uri, Json::object({{"uri", uri}})};
        Json contents = Json::array({Json::object({{"mimeType", std::string(spec->mime_type)}, {"text", *text}, {"uri", uri}})});
        return finish(Json::object({{"contents", std::move(contents)}}), true);
    }
    if (method == "subscriptions/listen") {
        // (served by the transports: it holds the stream open; nothing here changes, so nothing is ever sent on it)
        throw RpcError{-32603, "Internal server error", std::nullopt};
    }
    // tools/call
    std::optional<std::string> unsupported;
    for (const std::string& where : odd_numbers) {
        if (where.rfind("/params/arguments", 0) == 0) {
            unsupported = where.substr(std::string_view("/params/arguments").size());
            break;
        }
    }
    const Json arguments = p.contains("arguments") && p["arguments"].is_object() ? p["arguments"] : Json::object();
    const CallOutcome outcome = tools_.call(p["name"].get<std::string>(), arguments, unsupported);
    return finish(Json::object({{"content", outcome.content}, {"isError", outcome.is_error}}), false);
}

// --- the tool calls running now (at most kToolThreads at once) ---------------------------------------------------------

namespace {

std::mutex g_tool_slots_mutex;
std::condition_variable g_tool_slots_changed;
int g_tool_slots = 0;

struct ToolSlot {
    ToolSlot() {
        std::unique_lock<std::mutex> lock(g_tool_slots_mutex);
        g_tool_slots_changed.wait(lock, [] { return g_tool_slots < kToolThreads; });
        ++g_tool_slots;
    }
    ~ToolSlot() {
        std::lock_guard<std::mutex> lock(g_tool_slots_mutex);
        --g_tool_slots;
        g_tool_slots_changed.notify_one();
    }
    ToolSlot(const ToolSlot&) = delete;
    ToolSlot& operator=(const ToolSlot&) = delete;
};

}  // namespace

Json serve_one(const Engine& engine, const Json& id, const std::string& method, const Json& params, const std::vector<std::string>& odd_numbers) {
    try {
        std::optional<ToolSlot> slot;
        if (method == "tools/call") slot.emplace();
        return Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", engine.handle(method, params, std::string(kModernVersion), true, odd_numbers)}});
    } catch (const RpcError& error) {
        return Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"error", error.to_json()}});
    } catch (const std::exception& error) {
        std::fprintf(stderr, "modern request handler raised: %s\n", error.what());
        return Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"error", RpcError{-32603, "Internal server error", std::nullopt}.to_json()}});
    }
}

// --- the threads requests run on --------------------------------------------------------------------------------------

namespace {

std::mutex g_request_mutex;
std::condition_variable g_request_changed;
int g_requests = 0;

}  // namespace

RequestPermit::RequestPermit() {
    std::unique_lock<std::mutex> lock(g_request_mutex);
    g_request_changed.wait(lock, [] { return g_requests < kRequestThreads; });
    ++g_requests;
}

RequestPermit::~RequestPermit() {
    std::lock_guard<std::mutex> lock(g_request_mutex);
    --g_requests;
    g_request_changed.notify_one();
}

std::shared_ptr<RequestPermit> RequestPermit::within(int wait_ms) {
    std::unique_lock<std::mutex> lock(g_request_mutex);
    if (!g_request_changed.wait_for(lock, std::chrono::milliseconds(wait_ms), [] { return g_requests < kRequestThreads; })) return nullptr;
    ++g_requests;
    return std::shared_ptr<RequestPermit>(new RequestPermit(Taken{}));
}

int http_status_for(int code) {
    switch (code) {
        case -32700:
        case kInvalidRequest:
        case kInvalidParams:
        case kHeaderMismatch:
        case -32021:
        case kUnsupportedVersion: return 400;
        case kMethodNotFound: return 404;
        default: return 200;
    }
}

// --- a connection ----------------------------------------------------------------------------------------------------

Connection::Connection(const Engine& engine, Send send, Options options)
    : engine_(engine), send_(std::move(send)), options_(options), version_(kDefaultNegotiated) {
    if (!options_.dual_era) era_ = "legacy";
}

Connection::~Connection() { close(); }

bool Connection::initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialize_accepted_;
}

bool Connection::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ > 0;
}

void Connection::receive(Inbound message) {
    const Shape shape = shape_of(message.body);
    if (shape == Shape::Invalid || shape == Shape::Response) return;  // (dropped, as the SDK drops them)
    std::string era;
    std::vector<Inbound> replay;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) return;
        if (era_.empty()) {
            if (shape != Shape::Request) {
                if (lead_.size() < 8) lead_.push_back(std::move(message));  // (kept for the era's loop; more are dropped)
                return;
            }
            const Json params = message.body.contains("params") ? message.body["params"] : Json();
            era_ = message.body["method"] != Json("initialize") && has_modern_envelope(params) ? "modern" : "legacy";
            replay = std::move(lead_);
            lead_.clear();
        }
        era = era_;
    }
    for (Inbound& early : replay) dispatch_notification(early.body);
    if (shape == Shape::Request) {
        dispatch_request(std::move(message), era);
    } else {
        dispatch_notification(message.body);
    }
}

void Connection::dispatch_notification(const Json& body) {
    const std::string method = body["method"].get<std::string>();
    const Json params = body.contains("params") ? body["params"] : Json();
    std::lock_guard<std::mutex> lock(mutex_);
    if (method == "notifications/cancelled") {
        if (params.is_object() && params.contains("requestId")) {
            const Json& rid = params["requestId"];
            if (rid.is_string() || rid.is_number_integer()) {
                const auto found = in_flight_.find(id_key(rid));
                if (found != in_flight_.end()) found->second->cancelled = true;
            }
        }
        return;
    }
    if (method == "notifications/initialized" && era_ == "legacy") initialize_accepted_ = true;
    // (every other notification has no handler here: dropped)
}

void Connection::dispatch_request(Inbound message, const std::string& era) {
    Json id = message.body["id"];
    const std::string method = message.body["method"].get<std::string>();
    Json params = message.body.contains("params") ? message.body["params"] : Json();
    auto flight = std::make_shared<InFlight>();
    flight->id = id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        in_flight_[id_key(id)] = flight;  // (a repeated id replaces the earlier one, as the SDK's table does)
    }
    if (era == "legacy" && method == "initialize") {
        // (inline: the next message waits until the handshake is answered)
        run_request(std::move(id), method, std::move(params), std::move(message.odd_numbers), flight, era);
        return;
    }
    std::vector<Worker> ended;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // (the threads of requests that ended are joined here, so a long connection does not keep them)
        auto keep = std::partition(workers_.begin(), workers_.end(), [](const Worker& w) { return !w.done->load(); });
        std::move(keep, workers_.end(), std::back_inserter(ended));
        workers_.erase(keep, workers_.end());
        ++running_;  // (counted while it waits for a thread too: close() waits for it)
    }
    for (Worker& w : ended) w.thread.join();
    auto permit = std::make_shared<RequestPermit>();  // (waits while kRequestThreads requests run)
    std::lock_guard<std::mutex> lock(mutex_);
    auto done = std::make_shared<std::atomic<bool>>(false);
    workers_.push_back(Worker{std::thread([this, id = std::move(id), method, params = std::move(params), odd = std::move(message.odd_numbers), flight,
                                           era, done, permit]() mutable {
                                  run_request(std::move(id), std::move(method), std::move(params), std::move(odd), flight, std::move(era));
                                  permit.reset();
                                  std::lock_guard<std::mutex> finished(mutex_);
                                  --running_;
                                  done->store(true);
                                  idle_.notify_all();
                              }),
                              done});
}

void Connection::run_request(Json id, std::string method, Json params, std::vector<std::string> odd, std::shared_ptr<InFlight> flight,
                             std::string era) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (flight->cancelled) {  // (cancelled while it waited for a thread: never started)
            lock.unlock();
            finish(flight, Json());
            return;
        }
    }
    Json reply;
    try {
        if (era == "modern") {
            if (method == "initialize") {
                const Json requested = params.is_object() && params.contains("protocolVersion") ? params["protocolVersion"] : Json();
                Json data = requested.is_string()
                                ? Json::object({{"supported", Json::array({std::string(kModernVersion)})}, {"requested", requested}})
                                : Json::object({{"supported", Json::array({std::string(kModernVersion)})}});
                throw RpcError{kUnsupportedVersion, "connection is serving the 2026-07-28 protocol; the initialize handshake is not accepted",
                               std::move(data)};
            }
            Json body = Json::object({{"method", method}});
            if (!params.is_null()) body["params"] = params;
            if (auto rejection = classify_modern(body, nullptr)) throw *rejection;
            std::optional<ToolSlot> slot;
            if (method == "tools/call") slot.emplace();
            reply = Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", engine_.handle(method, params, std::string(kModernVersion), true, odd)}});
        } else {
            if (method != "initialize" && has_modern_envelope(params)) {
                throw RpcError{kInvalidRequest,
                               "this connection serves the handshake protocol era; requests carrying the 2026-07-28 envelope are not accepted on it",
                               std::nullopt};
            }
            std::string version;
            bool initialized = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                version = version_;
                initialized = initialize_accepted_;
            }
            if (method == "initialize") {
                auto [result, negotiated] = engine_.initialize(params, version);
                reply = Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
                std::lock_guard<std::mutex> lock(mutex_);
                version_ = negotiated;
                initialize_accepted_ = true;
            } else {
                std::optional<ToolSlot> slot;
                if (method == "tools/call") slot.emplace();
                reply = Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", engine_.handle(method, params, version, initialized, odd)}});
            }
        }
    } catch (const RpcError& error) {
        reply = Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"error", error.to_json()}});
    } catch (const std::exception& error) {
        // (a handler that raised: the SDK answers code 0 with its words in the handshake era, an internal error in 2026's)
        const RpcError e = era == "modern" ? RpcError{-32603, "Internal server error", std::nullopt} : RpcError{0, error.what(), std::nullopt};
        std::fprintf(stderr, "handler for '%s' raised: %s\n", method.c_str(), error.what());
        reply = Json::object({{"jsonrpc", "2.0"}, {"id", id}, {"error", e.to_json()}});
    }
    finish(flight, reply);
}

void Connection::finish(const std::shared_ptr<InFlight>& flight, const Json& reply) {
    bool cancelled = false;
    bool closing = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = in_flight_.find(id_key(flight->id));
        if (found != in_flight_.end() && found->second == flight) in_flight_.erase(found);
        cancelled = flight->cancelled;
        closing = closing_;
    }
    if (cancelled) {
        // (the peer cancelled it: no answer — except on the 2025-era Streamable HTTP, whose stream ends with an error)
        if (options_.answer_cancelled) {
            send_(Json::object({{"jsonrpc", "2.0"},
                                {"id", flight->id},
                                {"error", RpcError{kRequestCancelled, "Request cancelled", std::nullopt}.to_json()}}),
                  flight->id);
        }
        return;
    }
    if (closing) {
        // (the input ended while it ran: the SDK lets its worker thread finish, and nothing is written for it any more)
        return;
    }
    send_(reply, flight->id);
}

void Connection::close() {
    std::vector<Worker> workers;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        closing_ = true;
        idle_.wait(lock, [this] { return running_ == 0; });
        workers.swap(workers_);
    }
    for (Worker& w : workers) {
        if (w.thread.joinable()) w.thread.join();
    }
}

}  // namespace genko::api::mcp
