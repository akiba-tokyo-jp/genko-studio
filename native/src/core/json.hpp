#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace genko::core {

// Every JSON value in Genko. Objects keep their keys in the order they were read or set (Python's dicts do),
// integers are int64 (uint64 above INT64_MAX) and never pass through double.
using Json = nlohmann::ordered_json;

// How Python's json.dumps writes, for the options Genko uses.
struct DumpOptions {
    int indent = -1;                         // < 0: one line (Python's indent=None)
    std::string_view item_separator = ", ";  // Python's default; "," with an indent
    std::string_view key_separator = ": ";
    bool sort_keys = false;
    bool ensure_ascii = false;
};

// The same bytes as Python's json.dumps(value, ...): floats as repr(float) ("1.0", "1e-05", "1e+16"), strings
// with \" \\ \n \r \t \b \f and \u00XX for the other control characters, everything else as UTF-8 (or \uXXXX
// with ensure_ascii). NaN and infinities throw core::Error("value"); a string that is not UTF-8 throws
// core::Error("json").
std::string dump(const Json& value, const DumpOptions& options);

// json.dumps(value, ensure_ascii=False, indent=2): project.json.
std::string dump_python_indent2(const Json& value);

// json.dumps(value, ensure_ascii=False) (or ensure_ascii=True): one line with ", " and ": " (CLI output).
std::string dump_python(const Json& value, bool ensure_ascii = false);

// json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False): the canonical form of the JSON
// kept as assets (stroke blobs, op records, states).
std::string dump_canonical(const Json& value);

struct ParseOptions {
    // json.loads(bytes) rather than json.loads(str): a UTF-8 BOM is skipped instead of being an error.
    bool bytes = false;
    // The text was read with Path.read_text(), which turns "\r\n" and "\r" into "\n": error positions count them
    // as Python does.
    bool universal_newlines = false;
    // Deeper nesting is refused (Python stops with RecursionError at about the same depth).
    int max_depth = 1000;
};

// What the parser had to change so the document fits in a Json. Python reads NaN, Infinity and -Infinity (and
// numbers like 1e400) as non-finite floats and integers of any size; Genko never writes either.
struct ParseRepairs {
    std::vector<std::string> nonfinite;  // JSON pointers of non-finite numbers, now null
    std::vector<std::string> inexact;    // JSON pointers of integers beyond 64 bits, now the nearest double
};

// Python's json.loads for UTF-8 text: the same syntax (NaN, Infinity, any size of integer, duplicate keys where
// the last value wins in the first key's place) and, on failure, core::Error("json") with Python's message
// ("Expecting value: line 1 column 1 (char 0)", "'utf-8' codec can't decode byte 0xff in position 0: …").
// Positions count characters, not bytes, as Python's do.
Json parse_python_json(std::string_view text, ParseRepairs* repairs = nullptr, const ParseOptions& options = {});

// Python's error for bytes that are not UTF-8 ("'utf-8' codec can't decode …"), or nothing when they are.
std::optional<std::string> utf8_error(std::string_view bytes);

// JSON pointers (RFC 6901): "~" is written "~0" and "/" is written "~1".
std::string json_pointer_token(std::string_view token);
std::string json_pointer_append(std::string_view pointer, std::string_view token);
std::string json_pointer_append(std::string_view pointer, std::size_t index);

}  // namespace genko::core
