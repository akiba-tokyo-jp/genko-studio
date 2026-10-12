#include "api/tokens.hpp"

#include <QRandomGenerator>

#include <cerrno>
#include <cstdio>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/poses.hpp"
#include "storage/fsutil.hpp"

namespace genko::api::tokens {

namespace fs = std::filesystem;
using core::Json;

namespace {

Json read_all() {
    const fs::path path = file();
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return Json::object();
    core::ParseOptions options;
    options.universal_newlines = true;  // (Path.read_text)
    return core::parse_python_json(storage::read_file(path), nullptr, options);
}

// _save: json.dumps(data, ensure_ascii=False, indent=2) into tokens.json.tmp, readable by its owner only, then over
// tokens.json. (The temporary file is made owner-only from the start: Python makes it with the usual mode and then
// narrows it.)
void save(const Json& data) {
    const fs::path path = file();
    fs::create_directories(path.parent_path());
    const fs::path tmp = path.parent_path() / (path.filename().string() + ".tmp");
    const std::string text = core::dump_python_indent2(data);
    std::error_code ec;
    fs::remove(tmp, ec);
#ifdef _WIN32
    storage::write_atomic(tmp, text);
#else
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw core::Error("io", "cannot write " + core::path_to_utf8(tmp) + ": " + std::generic_category().message(errno));
    std::size_t done = 0;
    while (done < text.size()) {
        const auto n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            throw core::Error("io", "cannot write " + core::path_to_utf8(tmp));
        }
        done += static_cast<std::size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
#endif
    fs::rename(tmp, path);
}

}  // namespace

fs::path file() { return core::poses::config_dir() / "tokens.json"; }

std::map<std::string, std::string> load() {
    const Json data = read_all();
    std::map<std::string, std::string> out;
    if (!data.is_object()) throw core::Error("format", "tokens.json is not a JSON object");
    for (const auto& [token, info] : data.items()) {
        if (!info.is_object() || !info.contains("actor") || !info["actor"].is_string()) {
            throw core::Error("format", "tokens.json: the token " + token.substr(0, 8) + "… has no actor");
        }
        out[token] = info["actor"].get<std::string>();
    }
    return out;
}

std::string add(const std::string& actor) {
    if (!(actor.starts_with("human:") || actor.starts_with("ai:"))) throw core::PyValueError("actor must be human:<name> or ai:<name>");
    Json data = read_all();
    // secrets.token_urlsafe(32): 32 random bytes, URL-safe base64 without "="
    std::string bytes(32, '\0');
    for (char& b : bytes) b = static_cast<char>(QRandomGenerator::system()->generate() & 0xFF);
    std::string token = core::b64encode(bytes);
    for (char& c : token) {
        if (c == '+') c = '-';
        if (c == '/') c = '_';
    }
    while (!token.empty() && token.back() == '=') token.pop_back();
    data[token] = Json::object({{"actor", actor}});
    save(data);
    return token;
}

Json listing() {
    const Json data = read_all();
    Json out = Json::array();
    for (const auto& [token, info] : data.items()) out.push_back(Json::object({{"id", token.substr(0, 8)}, {"actor", info.at("actor")}}));
    return out;
}

int revoke(const std::string& prefix) {
    const Json data = read_all();
    Json keep = Json::object();
    int removed = 0;
    for (const auto& [token, info] : data.items()) {
        if (token.starts_with(prefix)) {
            ++removed;
        } else {
            keep[token] = info;
        }
    }
    if (removed > 0) save(keep);
    return removed;
}

}  // namespace genko::api::tokens
