#include "core/paths.hpp"

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

}  // namespace genko::core
