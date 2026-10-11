#include "service/stylecat.hpp"

#include <QByteArray>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <chrono>
#include <ctime>
#include <set>
#include <vector>

#include "core/base64.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "formats/pillow_save.hpp"
#include "render/png.hpp"
#include "service/urls.hpp"

namespace genko::service::stylecat {

namespace {

using core::py_str;

std::string env(const char* name) {
    const QByteArray value = qgetenv(name);
    return value.toStdString();
}

std::string rstrip_slash(std::string text) {
    while (!text.empty() && text.back() == '/') text.pop_back();
    return text;
}

[[noreturn]] void unreachable() { throw CatalogError("絵柄カタログにつながらない（インターネットの接続を確かめる）"); }

// urllib's FileHandler: file://localhost/… or file:///… read from the disk (the fragment left out, the selector's %XX
// decoded as url2pathname decodes it).
std::string read_file_url(const std::string& url) {
    std::string rest = url.substr(url.find(':') + 1);
    if (const std::size_t hash = rest.find('#'); hash != std::string::npos) rest.erase(hash);
    std::string host;
    if (rest.compare(0, 2, "//") == 0) {
        const std::size_t slash = rest.find('/', 2);
        host = rest.substr(2, slash == std::string::npos ? std::string::npos : slash - 2);
        rest = slash == std::string::npos ? std::string() : rest.substr(slash);
    }
    if (!host.empty() && host != "localhost") unreachable();
    const std::string local = urls::unquote(rest);
    QFile file(QString::fromStdString(local));
    if (local.empty() || !file.open(QIODevice::ReadOnly)) unreachable();
    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) unreachable();
    return data.toStdString();
}

// urllib's DataHandler: data:[<mediatype>][;base64],<data>.
std::string read_data_url(const std::string& url) {
    const std::size_t colon = url.find(':');
    const std::string rest = url.substr(colon + 1);
    const std::size_t comma = rest.find(',');
    if (comma == std::string::npos) throw core::PyValueError("not enough values to unpack (expected 2, got 1)");
    const std::string mediatype = rest.substr(0, comma);
    const std::string data = rest.substr(comma + 1);
    if (mediatype.size() >= 7 && mediatype.compare(mediatype.size() - 7, 7, ";base64") == 0) {
        const std::string compact = urls::unquote_to_bytes(data);  // (base64.decodebytes(unquote_to_bytes(data)))
        try {
            return core::a2b_base64(compact);
        } catch (const core::Error& error) {
            throw core::PyUncaught("binascii.Error", error.what());
        }
    }
    return urls::unquote_to_bytes(data);
}

std::string http_get(const std::string& url) {
    QNetworkAccessManager manager;
    QNetworkProxyFactory::setUseSystemConfiguration(true);  // (urllib reads the proxy from the environment)
    QNetworkRequest request{QUrl(QString::fromStdString(url))};
    request.setRawHeader("User-Agent", "GenkoStudio");
    request.setRawHeader("Accept", "application/json, image/*");
    const std::string token = env("GENKO_STYLE_CATALOG_TOKEN");
    if (!token.empty()) request.setRawHeader("Authorization", QByteArray::fromStdString("Bearer " + token));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(std::chrono::seconds(kTimeoutSeconds));
    QNetworkReply* reply = manager.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished()) loop.exec();
    const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError failure = reply->error();
    reply->deleteLater();
    if (status.isValid() && status.toInt() >= 400) {
        const int code = status.toInt();
        if (code == 404) throw CatalogError("絵柄カタログにその絵柄がない（id を確かめる）");
        if (code == 401) throw CatalogError("絵柄カタログが限定公開になっている（GENKO_STYLE_CATALOG_TOKEN が要る）");
        throw CatalogError("絵柄カタログが答えなかった（HTTP " + std::to_string(code) + "）");
    }
    if (failure != QNetworkReply::NoError) unreachable();
    return body.toStdString();
}

Json reply_json(const std::string& path) {
    const std::string raw = get(base() + path);
    Json data;
    try {
        data = core::parse_python_json(raw, nullptr, core::ParseOptions{});  // (json.loads(raw.decode("utf-8")))
    } catch (const core::Error&) {
        throw CatalogError("絵柄カタログの返事が読めない");
    }
    if (!data.is_object()) throw CatalogError("絵柄カタログの返事が読めない");
    return data;
}

// Python's dict keyed by a node's id (an unhashable id is TypeError, as the dict comprehension raises it).
struct Node {
    Json id;
    const Json* node;
};

const Json* node_by_id(const std::vector<Node>& nodes, const Json& id) {
    core::require_hashable(id);
    const Json* found = nullptr;
    for (const Node& n : nodes) {
        if (core::py_equals(n.id, id)) found = n.node;  // (the last one of an id wins, as the dict keeps it)
    }
    return found;
}

// stylecat._with_paths(nodes): each node with "path", the titles above it and its own.
std::vector<Json> with_paths(const Json& nodes) {
    const std::vector<Json> items = core::iterate(nodes);
    std::vector<Node> by_id;
    for (const Json& n : items) {
        const Json id = core::subscript(n, "id");
        core::require_hashable(id);
        by_id.push_back(Node{id, &n});
    }
    std::vector<Json> out;
    for (const Json& node : items) {
        std::vector<std::string> chain;
        std::vector<Json> seen;
        const Json* cur = &node;
        while (cur != nullptr) {
            const Json id = core::subscript(*cur, "id");
            bool met = false;
            for (const Json& s : seen) met = met || core::py_equals(s, id);
            if (met) break;
            seen.push_back(id);
            const Json title = core::py_get(*cur, "title");
            chain.push_back(py_str(core::py_truthy(title) ? title : id));
            cur = node_by_id(by_id, core::py_get(*cur, "parent"));
        }
        Json path = Json::array();
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) path.push_back(*it);
        Json entry = node;
        entry["path"] = std::move(path);
        out.push_back(std::move(entry));
    }
    return out;
}

Json pick(const Json& node, std::initializer_list<const char*> keys) {
    Json out = Json::object();
    for (const char* key : keys) out[key] = core::py_get(node, key);
    return out;
}

}  // namespace

std::string base() {
    const std::string set = env("GENKO_STYLE_CATALOG");
    return rstrip_slash(set.empty() ? std::string(kBase) : set);
}

std::string page_url(const std::string& style_id) { return base() + "/n/" + urls::quote(style_id) + "?lang=ja"; }

std::string get(const std::string& url) {
    const urls::Parts parts = urls::urlsplit(url);
    if (parts.scheme.empty()) throw core::PyValueError("unknown url type: " + core::py_repr_str(url));
    if (parts.scheme == "file") return read_file_url(url);
    if (parts.scheme == "data") return read_data_url(url);
    if (parts.scheme == "http" || parts.scheme == "https") return http_get(url);
    unreachable();  // (urllib: "unknown url type", a URLError)
}

Json tree() {
    const Json data = reply_json("/v1/tree");
    const Json nodes = core::py_get(data, "nodes");
    if (!nodes.is_array()) throw CatalogError("絵柄カタログの一覧が読めない");
    Json out = Json::array();
    for (const Json& n : nodes) {
        if (n.is_object() && core::truthy_at(n, "id")) out.push_back(n);
    }
    return out;
}

Json find(const Json& words, const Json* nodes) {
    const Json given = core::py_or(words, Json(""));
    if (!given.is_string()) core::raise_attribute_error(given, "strip");
    const std::string key = core::py_strip(given.get<std::string>());
    const Json all = nodes == nullptr ? tree() : *nodes;
    Json out = Json::array();
    for (const Json& n : with_paths(all)) {
        const Json title = core::py_get(n, "title");
        if (!key.empty() && py_str(core::py_truthy(title) ? title : Json("")).find(key) != std::string::npos) {
            out.push_back(pick(n, {"id", "title", "summary", "path"}));
        }
    }
    return out;
}

Json namesakes(const Json& style_id, const Json& title, const Json* nodes) {
    const Json all = nodes == nullptr ? tree() : *nodes;
    Json out = Json::array();
    for (const Json& n : with_paths(all)) {
        if (core::py_equals(core::py_get(n, "title"), title) && !core::py_equals(core::subscript(n, "id"), style_id)) {
            out.push_back(pick(n, {"id", "path"}));
        }
    }
    return out;
}

Json full_path(const Json& data) {
    std::vector<Json> steps;
    for (const Json& p : core::iterate(core::py_or(core::py_get(data, "path"), Json::array()))) {
        if (p.is_object() || p.is_string()) steps.push_back(p);
    }
    const std::string own_id = py_str(core::py_or(core::py_get(data, "id"), Json("")));
    const std::string own_title =
        py_str(core::py_or(core::py_or(core::py_get(data, "title"), core::py_get(data, "id")), Json("")));
    if (!steps.empty() && steps.back().is_object() && py_str(core::py_or(core::py_get(steps.back(), "id"), Json(""))) == own_id) {
        steps.pop_back();
    }
    std::vector<std::string> titles;
    for (const Json& p : steps) {
        titles.push_back(p.is_object() ? py_str(core::py_or(core::py_get(p, "title"), core::py_get(p, "id"))) : py_str(p));
    }
    if (!titles.empty() && titles.back() == own_title) titles.pop_back();
    Json out = Json::array();
    for (const std::string& t : titles) out.push_back(t);
    if (!own_title.empty()) out.push_back(own_title);
    return out;
}

Json style(const Json& style_id) {
    Json data = reply_json("/v1/styles/" + core::stylecat::check_id(style_id));
    const Json prompt = core::py_get(data, "prompt");
    if (!prompt.is_object() || !core::truthy_at(prompt, "ja")) throw CatalogError("この絵柄には言葉（prompt）が無い");
    return data;
}

namespace {

// samples[index] as Python indexes the samples it was given (a list's item; a str's character; a dict has no int key).
Json sample_at(const Json& samples, std::int64_t index) {
    if (samples.is_object()) throw core::PyUncaught("KeyError", std::to_string(index));
    return core::subscript(samples, index);
}

bool python_int(const Json& value) { return value.is_number_integer() || value.is_boolean(); }

std::int64_t int_of(const Json& value) { return value.is_boolean() ? (value.get<bool>() ? 1 : 0) : value.get<std::int64_t>(); }

}  // namespace

std::optional<std::string> sample_png(const Json& data) {
    const Json samples = core::py_or(core::py_get(data, "samples"), Json::array());
    const Json reference = core::py_get(data, "reference");
    std::int64_t index = 0;
    if (python_int(reference) && 0 <= int_of(reference) && int_of(reference) < static_cast<std::int64_t>(core::length(samples))) {
        index = int_of(reference);
    }
    if (!core::py_truthy(samples)) return std::nullopt;
    const Json sample = sample_at(samples, index);
    if (!sample.is_object() || !core::truthy_at(sample, "url")) return std::nullopt;
    const std::string raw = get(py_str(core::subscript(sample, "url")));
    render::Image picture;
    try {
        picture = render::open_image(raw);
    } catch (const std::exception&) {
        throw CatalogError("絵柄の見本の絵が読めない");
    }
    formats::PngSave save;
    save.optimize = true;
    const bool mono = core::py_equals(core::py_get(data, "expression"), Json("mono"));
    return formats::png_bytes(picture.convert(mono ? "L" : "RGB"), save);
}

Json saved(const Json& data, const std::optional<std::string>& sample_asset) {
    const Json prompt = core::py_or(core::py_get(data, "prompt"), Json::object());
    const Json samples = core::py_or(core::py_get(data, "samples"), Json::array());
    const Json reference = core::py_get(data, "reference");
    const std::int64_t index = python_int(reference) ? int_of(reference) : 0;
    Json sample_url;
    if (0 <= index && index < static_cast<std::int64_t>(core::length(samples))) {
        const Json sample = sample_at(samples, index);
        if (sample.is_object()) sample_url = core::py_get(sample, "url");
    }
    const Json id = core::subscript(data, "id");
    Json avoid = Json::array();
    for (const Json& a : core::iterate(core::py_or(core::py_get(data, "avoid"), Json::array()))) avoid.push_back(py_str(a));
    const Json license = core::py_or(core::py_get(data, "license"), Json::object());
    std::array<char, 32> now{};
    const std::time_t t = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    const std::size_t n = std::strftime(now.data(), now.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    Json out = Json::object();
    out["id"] = py_str(id);
    out["title"] = py_str(core::py_or(core::py_get(data, "title"), id));
    out["path"] = full_path(data);
    out["summary"] = py_str(core::py_or(core::py_get(data, "summary"), Json("")));
    out["level"] = core::py_get(data, "level");
    out["expression"] = py_str(core::py_or(core::py_get(data, "expression"), Json("mono")));
    out["prompt"] = Json::object({{"ja", py_str(core::py_or(core::py_get(prompt, "ja"), Json("")))},
                                  {"en", py_str(core::py_or(core::py_get(prompt, "en"), Json("")))},
                                  {"tags", py_str(core::py_or(core::py_get(prompt, "tags"), Json("")))}});
    out["avoid"] = std::move(avoid);
    out["sample"] = sample_asset ? Json(*sample_asset) : Json();
    out["sample_url"] = sample_url;
    out["version"] = core::py_get(data, "version");
    out["updated_at"] = core::py_get(data, "updated_at");
    out["terms"] = py_str(core::py_or(core::py_get(license, "terms"), Json("")));
    out["source"] = base() + "/v1/styles/" + py_str(id);
    out["taken_at"] = std::string(now.data(), n);
    return out;
}

Json newer(const Json& kept) {
    const Json now = style(core::subscript(kept, "id"));
    if (core::py_equals(core::py_get(now, "version"), core::py_get(kept, "version")) &&
        core::py_equals(core::py_get(now, "updated_at"), core::py_get(kept, "updated_at"))) {
        return Json();
    }
    return Json::object({{"id", core::subscript(now, "id")},
                         {"version", core::py_get(now, "version")},
                         {"updated_at", core::py_get(now, "updated_at")},
                         {"kept_version", core::py_get(kept, "version")}});
}

std::optional<std::string> style_from_link(const Json& link) {
    const urls::Parts parts = urls::urlparse(core::py_strip(py_str(core::py_or(link, Json("")))));
    if (parts.scheme == "http" || parts.scheme == "https") {
        const urls::Parts site = urls::urlparse(base());
        std::string trimmed = parts.path;
        while (!trimmed.empty() && trimmed.front() == '/') trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
        std::vector<std::string> path;
        std::size_t start = 0;
        for (;;) {
            const std::size_t slash = trimmed.find('/', start);
            path.push_back(trimmed.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        if (urls::lower(parts.netloc) != urls::lower(site.netloc) || path.size() != 2 || path[0] != "n" || path[1].empty()) {
            return std::nullopt;
        }
        return core::stylecat::check_id(urls::unquote(path[1]));
    }
    std::string trimmed = parts.path;
    while (!trimmed.empty() && trimmed.front() == '/') trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
    if (parts.scheme != "genko" || (parts.netloc.empty() ? trimmed : parts.netloc) != "use-style") return std::nullopt;
    const auto found = urls::parse_qs_first(parts.query, "id");
    return core::stylecat::check_id(found.value_or(""));
}

std::string style_id_from(const Json& text) {
    const std::string given = core::py_strip(py_str(core::py_or(text, Json(""))));
    if (given.find("://") != std::string::npos) {
        const auto found = style_from_link(given);
        if (!found) throw CatalogError("絵柄カタログのページの URL か genko://use-style のリンクではない");
        return *found;
    }
    return core::stylecat::check_id(given);
}

}  // namespace genko::service::stylecat
