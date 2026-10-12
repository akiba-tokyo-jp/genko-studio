#include "service/service_util.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <system_error>
#include <vector>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/book.hpp"
#include "storage/fsutil.hpp"

namespace genko::service::util {

Json item(const Json& object, std::string_view key) {
    if (object.is_object()) {
        const auto it = object.find(std::string(key));
        if (it == object.end()) throw core::PyUncaught("KeyError", core::py_repr_str(key));
        return *it;
    }
    return core::subscript(object, key);
}

Json or_dict(const Json& value, std::string_view method) {
    if (!core::py_truthy(value)) return Json::object();
    if (!value.is_object()) core::raise_attribute_error(value, method);
    return value;
}

std::optional<Json> name_of(const core::Document& doc, const Json& index) {
    const auto at = page_at(doc, index);
    if (!at) return std::nullopt;
    const core::Page& page = doc.page(*at);
    if (!page.plan || !core::py_truthy(*page.plan) || !page.plan->is_object() || !page.plan->contains("name")) return std::nullopt;
    return *page.plan;
}

std::string zero3(const Json& n) {
    if (n.is_boolean()) return n.get<bool>() ? "001" : "000";
    if (n.is_number_integer()) {
        const std::int64_t v = n.get<std::int64_t>();
        char buf[32];
        std::snprintf(buf, sizeof buf, "%03lld", static_cast<long long>(v));
        return buf;
    }
    if (n.is_number_float() || n.is_string()) {
        throw core::PyValueError("Unknown format code 'd' for object of type '" + core::py_type_name(n) + "'");
    }
    throw core::PyTypeError("unsupported format string passed to " + core::py_type_name(n) + ".__format__");
}

std::string path_text(const std::filesystem::path& path) { return core::path_to_utf8(path); }

std::string name_part(std::string_view id) {
    std::string out(id);
    for (char& c : out) {
        if (c == '/' || c == '\\' || c == '\0') c = '_';
    }
    return out;
}

bool inside(const std::filesystem::path& folder, const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::weakly_canonical(folder, ec);
    if (ec) base = folder.lexically_normal();
    std::filesystem::path where = std::filesystem::weakly_canonical(path, ec);
    if (ec) where = path.lexically_normal();
    auto w = where.begin();
    for (auto b = base.begin(); b != base.end(); ++b, ++w) {
        if (b->empty() && std::next(b) == base.end()) break;  // (a folder given with a trailing "/")
        if (w == where.end() || *w != *b) return false;
    }
    for (; w != where.end(); ++w) {
        if (!w->empty()) return true;
    }
    return false;
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    storage::write_atomic(path, bytes);
}

Json merged(const Json& a, const Json& b) {
    for (const Json* side : {&a, &b}) {
        if (!side->is_object()) throw core::PyTypeError("'" + core::py_type_name(*side) + "' object is not a mapping");
    }
    Json out = a;
    for (const auto& [key, value] : b.items()) out[key] = value;
    return out;
}

Json sorted_unique(const Json& values) {
    std::vector<Json> out;
    for (const Json& v : core::iterate(values)) {
        core::require_hashable(v);
        bool seen = false;
        for (const Json& o : out) seen = seen || core::py_equals(o, v);
        if (!seen) out.push_back(v);
    }
    std::stable_sort(out.begin(), out.end(), [](const Json& x, const Json& y) { return core::py_less(x, y); });
    return Json(out);
}

}  // namespace genko::service::util
