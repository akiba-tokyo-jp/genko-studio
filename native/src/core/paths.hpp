#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace genko::core {

// UTF-8 text and file paths (docs/cpp-migration/ARCHITECTURE.md §3): the only conversions between the two, so
// Windows never goes through the ANSI code page.
std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path& path);

}  // namespace genko::core
