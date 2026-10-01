#pragma once

#include <stdexcept>
#include <string>

namespace genko::core {

// An error with a machine-readable code, an English message and, when it is about a place in a JSON
// document, a JSON pointer (docs/cpp-migration/ARCHITECTURE.md §8). The CLI, HTTP and MCP layers turn it
// into {"ok": false, "error": message, "code": code}.
//
// Codes used by core and storage: "format" (a document that cannot be read as Genko data), "json" (text that
// is not JSON), "not_found", "io", "unsupported_version", "read_only", "value" (a bad argument), "key" (no such
// frame/page/…).
class Error : public std::runtime_error {
public:
    Error(std::string code, const std::string& message, std::string path = {});

    const std::string& code() const noexcept { return code_; }
    const std::string& path() const noexcept { return path_; }

private:
    std::string code_;
    std::string path_;
};

}  // namespace genko::core
