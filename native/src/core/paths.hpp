#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace genko::core {

// UTF-8 text and file paths (docs/cpp-migration/ARCHITECTURE.md §3): the only conversions between the two, so
// Windows never goes through the ANSI code page.
std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path& path);

// Python's Path(path).resolve() (strict=False): absolute, every symbolic link followed where it is met, name by name,
// so "..'" after a link climbs from where the link leads and a name that is not there is taken as written. (Not
// std::filesystem::weakly_canonical, which stops following links at the first name that is not there:
// "gone/../link/x" came out as "link/x" with the link never followed.) Windows: as ntpath.realpath, the ".." taken
// as written first (the system does the same), then the links of what is there.
std::filesystem::path resolve_path(const std::filesystem::path& path);
// Whether a path resolve_path gave ends in a loop of symbolic links (stat says ELOOP): Python 3.12's Path.resolve()
// (strict=False) raises RuntimeError("Symlink loop from …") then. Never on Windows.
bool symlink_loop(const std::filesystem::path& resolved);

}  // namespace genko::core
