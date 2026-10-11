#pragma once

#include <optional>
#include <string>
#include <string_view>

// Python's urllib.parse as the studio tools use it (urlsplit / urlparse of 3.12, quote, unquote, unquote_to_bytes,
// parse_qs), on UTF-8 text.

namespace genko::service::urls {

struct Parts {
    std::string scheme;  // lowercased
    std::string netloc;
    std::string path;
    std::string params;  // urlparse only (a scheme that uses them: the last segment's ";…")
    std::string query;
    std::string fragment;
};

// urllib.parse.urlsplit(url). core::PyValueError "Invalid IPv6 URL" for unbalanced brackets in the netloc.
Parts urlsplit(std::string_view url);
// urllib.parse.urlparse(url)
Parts urlparse(std::string_view url);
// urllib.parse.quote(text) (safe "/"): UTF-8 bytes outside A-Z a-z 0-9 _.-~/ as %XX.
std::string quote(std::string_view text);
// urllib.parse.unquote(text): %XX decoded as UTF-8 (bytes that are not UTF-8 become U+FFFD).
std::string unquote(std::string_view text);
// urllib.parse.unquote_to_bytes(text)
std::string unquote_to_bytes(std::string_view text);
// parse_qs(query).get(name)[0]: the first non-blank value of `name` ("+" is a space), nothing when there is none.
std::optional<std::string> parse_qs_first(std::string_view query, std::string_view name);
// str.lower() of ASCII letters (netlocs).
std::string lower(std::string_view text);

}  // namespace genko::service::urls
