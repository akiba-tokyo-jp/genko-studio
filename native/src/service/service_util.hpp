#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "core/json.hpp"
#include "core/model.hpp"

// Small Python idioms the studio tools are written in, with Python's exceptions.

namespace genko::service::util {

using core::Json;

// x[key] of a dict (PyUncaught KeyError with the key's repr when it is missing); other values as core::subscript
// (TypeError for what is not subscriptable by a str).
Json item(const Json& object, std::string_view key);
// `x or {}` of a value Python then calls a dict's method on (AttributeError "'list' object has no attribute 'get'" for
// a truthy value that is not a dict).
Json or_dict(const Json& value, std::string_view method = "get");
// state.name(episode, index) for an index as an agent gave it: the page's plan when it holds a name.
std::optional<Json> name_of(const core::Document& doc, const Json& index);
// f"{n:03d}" of an int (PyUncaught ValueError "Unknown format code 'd' for object of type 'float'" for a float, a str…).
std::string zero3(const Json& n);
// str(path): the path as text, "/" between its parts.
std::string path_text(const std::filesystem::path& path);
// A part of a file's name made of an id (a candidate's, a frame's, a layer's, a page's): its "/", "\\" and NUL written
// "_", so the name is one name in its folder whatever the id (C++: Python puts the id in the name as it is, "../" and
// all, and writes wherever that leads — SPEC COMP-01a). An ordinary id is unchanged.
std::string name_part(std::string_view id);
// Whether `path` is below `folder`, both resolved (links followed, a missing tail taken as written).
bool inside(const std::filesystem::path& folder, const std::filesystem::path& path);
// Path.write_bytes(data): the file written whole (C++: atomically, beside it first), its folders made first.
void write_file(const std::filesystem::path& path, std::string_view bytes);
// `{**a, **b}`: b's keys over a's (TypeError "'list' object is not a mapping" when one is not a dict).
Json merged(const Json& a, const Json& b);
// sorted(set(values)) of ints (or anything Python compares), each once.
Json sorted_unique(const Json& values);

}  // namespace genko::service::util
