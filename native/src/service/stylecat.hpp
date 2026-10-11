#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/json.hpp"
#include "core/stylecat.hpp"

// The manga style catalog (マンガの絵柄カタログ, https://manga.akiba.tokyo.jp) as the studio tools read it (Python's
// genko/stylecat.py, its reading part; the id check and validate are core's): a style's branch and its sample, kept in
// the book so the book's style does not move when the site changes. Only reading, over an HTTP(S) GET (Qt Network, as
// urllib reads it: redirects followed, 15 s), or from a file: URL (urllib's file handler). GENKO_STYLE_CATALOG changes
// the address (tests point it at a folder: file:///…), GENKO_STYLE_CATALOG_TOKEN is sent as a bearer token.
//
// Every failure is CatalogError with the catalog's Japanese words, as Python's.

namespace genko::service::stylecat {

using core::Json;
using core::stylecat::CatalogError;

inline constexpr const char* kBase = "https://manga.akiba.tokyo.jp";
inline constexpr int kTimeoutSeconds = 15;

// stylecat.base(): $GENKO_STYLE_CATALOG (or BASE) without trailing "/".
std::string base();
// stylecat.page_url(style_id): the branch's page on the site (for a person to look at).
std::string page_url(const std::string& style_id);
// stylecat._get(url): the reply's bytes.
std::string get(const std::string& url);
// stylecat.tree(): every published branch ({id, parent, level, title, summary, thumbnail_url}).
Json tree();
// stylecat.find(words, nodes=None): the branches whose title has these words, each with its path.
Json find(const Json& words, const Json* nodes = nullptr);
// stylecat.namesakes(style_id, title, nodes=None): other branches with exactly this title.
Json namesakes(const Json& style_id, const Json& title, const Json* nodes = nullptr);
// stylecat.full_path(data): the branch's place as titles, genre first and itself last.
Json full_path(const Json& data);
// stylecat.style(style_id): one branch as the catalog has it now (with its children).
Json style(const Json& style_id);
// stylecat.sample_png(data): the branch's best sample as PNG (as Pillow saves it with optimize=True), or nothing.
std::optional<std::string> sample_png(const Json& data);
// stylecat.saved(data, sample_asset): what the book keeps of a branch ("taken_at": the UTC time now).
Json saved(const Json& data, const std::optional<std::string>& sample_asset);
// stylecat.newer(kept): the catalog's version of the book's style when it is newer than the book's copy (null when the
// same).
Json newer(const Json& kept);
// stylecat.style_from_link(link): genko://use-style?id=… or the catalog's own page → the id (nothing for other links).
std::optional<std::string> style_from_link(const Json& link);
// stylecat.style_id_from(text): an id, a genko:// link or a page's URL → the id.
std::string style_id_from(const Json& text);

}  // namespace genko::service::stylecat
