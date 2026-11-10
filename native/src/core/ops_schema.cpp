#include "core/ops_schema.hpp"

#include <cstddef>

namespace genko::core {

namespace detail {
extern const unsigned char kOpsSchemaJson[];
extern const std::size_t kOpsSchemaJsonSize;
}  // namespace detail

std::string_view ops_schema_text() {
    return {reinterpret_cast<const char*>(detail::kOpsSchemaJson), detail::kOpsSchemaJsonSize};
}

const Json& ops_schema() {
    static const Json schema = parse_python_json(ops_schema_text());
    return schema;
}

}  // namespace genko::core
