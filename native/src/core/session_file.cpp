#include "core/session_file.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#endif

#include "core/config_dir.hpp"
#include "core/json_depth.hpp"

namespace genko::core::session_file {

std::filesystem::path path() { return config_dir() / "session.json"; }

bool process_alive(std::int64_t pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) return false;
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) != 0 && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    if (pid > INT32_MAX) return false;
    // (0: no signal sent — whether it is there; EPERM: there, another person's)
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

Json read() {
    const Json none{{"running", false}};
    std::ifstream in(path(), std::ios::binary);
    if (!in) return none;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (too_deep(text)) return none;  // (PC6, the review's P3-2: deeper than any reader of it goes, as not JSON)
    Json kept;
    try {
        kept = Json::parse(text);
    } catch (const std::exception&) {
        return none;
    }
    if (!kept.is_object() || !kept.contains("pid") || !kept["pid"].is_number_integer() || !process_alive(kept["pid"].get<std::int64_t>())) return none;
    Json out{{"running", true}};
    for (const auto& [key, value] : kept.items()) out[key] = value;
    return out;
}

}  // namespace genko::core::session_file
