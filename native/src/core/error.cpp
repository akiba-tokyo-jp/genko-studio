#include "core/error.hpp"

#include <utility>

namespace genko::core {

Error::Error(std::string code, const std::string& message, std::string path)
    : std::runtime_error(message), code_(std::move(code)), path_(std::move(path)) {}

PyUncaught::PyUncaught(std::string type, const std::string& message)
    : Error("python_error", message), type_(std::move(type)) {}

}  // namespace genko::core
