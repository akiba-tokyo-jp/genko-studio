#include "core/stylecat.hpp"

#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core::stylecat {

namespace {

// ID_RE = ^[a-z0-9][a-z0-9-]{0,120}$ (re.match: from the start; the id is stripped, so no "\n" before its end)
bool good_id(const std::string& id) {
    if (id.empty() || id.size() > 121) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && !(i > 0 && c == '-')) return false;
    }
    return true;
}

}  // namespace

std::string check_id(const Json& style_id) {
    const std::string id = py_strip(py_str(py_or(style_id, Json(""))));
    if (!good_id(id)) throw CatalogError("絵柄の id「" + id + "」は使えない（英小文字・数字・ハイフン）");
    return id;
}

Json validate(const Json& catalog) {
    if (!catalog.is_object()) throw CatalogError("catalog はオブジェクト");
    check_id(get_or(catalog, "id", Json("")));
    const Json* prompt = get(catalog, "prompt");
    if (prompt == nullptr || !prompt->is_object() || py_strip(py_str(py_or(get_or(*prompt, "ja", Json()), Json("")))).empty()) {
        throw CatalogError("catalog.prompt.ja が要る");
    }
    if (const Json* sample = get(catalog, "sample"); sample != nullptr && !sample->is_null() && !py_str(*sample).starts_with("sha256:")) {
        throw CatalogError("catalog.sample は sha256:… の asset");
    }
    Json kept = Json::object();
    for (const char* key : {"id", "title", "path", "summary", "level", "expression", "prompt", "avoid", "sample", "sample_url",
                            "version", "updated_at", "terms", "source", "taken_at"}) {
        if (const Json* value = get(catalog, key)) kept[key] = *value;
    }
    return kept;
}

}  // namespace genko::core::stylecat
