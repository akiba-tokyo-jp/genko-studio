#include "core/paths.hpp"

#include <map>
#include <optional>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>

#include <cerrno>
#endif

namespace genko::core {

std::filesystem::path path_from_utf8(std::string_view utf8) {
    const std::u8string text(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(text);
}

std::string path_to_utf8(const std::filesystem::path& path) {
    // (the generic form: "/" between names on every OS, so messages and reports read the same on Windows and Linux;
    // Windows accepts "/" in paths)
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

namespace {

namespace fs = std::filesystem;

#ifndef _WIN32
// posixpath._joinrealpath: `rest` joined onto the resolved `path`, links followed. A loop stops it: the rest is then
// joined as written (false), as Python does.
bool join_real(fs::path& path, const fs::path& rest, std::map<fs::path, std::optional<fs::path>>& seen) {
    if (rest.is_absolute()) path = rest.root_path();
    const fs::path parts = rest.relative_path();
    for (auto it = parts.begin(); it != parts.end(); ++it) {
        const fs::path& name = *it;
        if (name.empty() || name == ".") continue;
        if (name == "..") {
            if (path.has_relative_path()) path = path.parent_path();
            continue;
        }
        const fs::path next = path / name;
        std::error_code ec;
        const fs::file_status st = fs::symlink_status(next, ec);
        const fs::path target = !ec && fs::is_symlink(st) ? fs::read_symlink(next, ec) : fs::path();
        if (target.empty()) {
            path = next;
            continue;
        }
        bool ok = true;
        if (const auto known = seen.find(next); known != seen.end()) {
            if (known->second) {
                path = *known->second;
                continue;
            }
            path = next;  // (a loop)
            ok = false;
        } else {
            seen[next] = std::nullopt;
            ok = join_real(path, target, seen);
            if (ok) seen[next] = path;
        }
        if (!ok) {
            for (auto left = std::next(it); left != parts.end(); ++left) path /= *left;
            return false;
        }
    }
    return true;
}
#endif

}  // namespace

std::filesystem::path resolve_path(const std::filesystem::path& path) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    if (ec) return path.lexically_normal();
#ifdef _WIN32
    const fs::path normal = absolute.lexically_normal();
    const fs::path out = fs::weakly_canonical(normal, ec);
    return ec ? normal : out;
#else
    std::map<fs::path, std::optional<fs::path>> seen;
    fs::path out = absolute.root_path();
    join_real(out, absolute, seen);
    return out.lexically_normal();
#endif
}

bool symlink_loop(const std::filesystem::path& resolved) {
#ifdef _WIN32
    (void)resolved;
    return false;
#else
    struct stat st {};
    return ::stat(resolved.c_str(), &st) != 0 && errno == ELOOP;
#endif
}

}  // namespace genko::core
