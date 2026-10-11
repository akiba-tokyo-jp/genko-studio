#pragma once

#include <stdexcept>
#include <string>

#include "core/json.hpp"

// The manga style catalog's branch as a book keeps it (Python's genko/stylecat.py, its part without the network: the
// id check and validate, what set_style_catalog takes). Reading the catalog over HTTPS is the studio service's (a later
// group of M5).

namespace genko::core::stylecat {

// stylecat.CatalogError: the catalog's words for what is wrong (Japanese, as Python's).
class CatalogError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// stylecat.check_id: str(style_id or "").strip(), which must be 1 to 121 of a-z, 0-9 and "-" (not "-" first); the id
// so stripped. Throws CatalogError「絵柄の id「…」は使えない（英小文字・数字・ハイフン）」.
std::string check_id(const Json& style_id);

// stylecat.validate: the kept branch as an op carries it, checked (an object; its id; a prompt with words in ja; a sample
// that is an asset ref or null), with only the keys a book keeps (id, title, path, summary, level, expression, prompt,
// avoid, sample, sample_url, version, updated_at, terms, source, taken_at; in this order). Throws CatalogError.
Json validate(const Json& catalog);

}  // namespace genko::core::stylecat
