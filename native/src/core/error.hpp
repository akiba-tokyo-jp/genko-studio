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

// Python's exceptions where C++ does what Python does (its conversions, math and checks, core/pynum.hpp and
// core/pyops.hpp): ValueError and TypeError, and the others by their type name (OverflowError, IndexError, …).
class PyValueError : public Error {
public:
    explicit PyValueError(const std::string& message) : Error("value", message) {}
};

class PyTypeError : public Error {
public:
    explicit PyTypeError(const std::string& message) : Error("type", message) {}
};

// An exception Python's apply_ops lets through (its type, e.g. "IndexError", and message).
class PyUncaught : public Error {
public:
    PyUncaught(std::string type, const std::string& message);
    const std::string& type() const noexcept { return type_; }

private:
    std::string type_;
};

}  // namespace genko::core
