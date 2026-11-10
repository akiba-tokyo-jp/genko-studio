#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "core/json.hpp"

// PC5 節 J (SPEC.md PROD-01「制作の補助」): what the production's page keys share when they are read — the layer comps
// (core/layer_comps), the slices (core/slices), the notes (core/annotations), the measuring (core/measure), the timeline's
// and the video layers' (core/video). Each value is read strictly: a key not known, a value of another type, a number
// not finite or out of its range throws core::Error("value") naming where it is (the book opens read-only:
// storage/reader), so a value this build does not know is never written over.

namespace genko::core::production {

inline constexpr double kCoordinateMaxMm = 1e6;  // (a place on a page, mm, as vanishing-point@1's)

[[noreturn]] void wrong(const std::string& where, const std::string& what);
// The object's keys are among `keys` (`where` an object).
void only_keys(const Json& value, const std::string& where, std::initializer_list<std::string_view> keys);
const Json& field(const Json& object, std::string_view key, const std::string& where);  // (one it must have)
const Json* optional(const Json& object, std::string_view key);                          // (absent: nullptr)
double number(const Json& value, const std::string& where, double lo, double hi);
std::int64_t integer(const Json& value, const std::string& where, std::int64_t lo, std::int64_t hi);
bool boolean(const Json& value, const std::string& where);
// A string of at most `most` bytes (UTF-8 as JSON holds it).
std::string text(const Json& value, const std::string& where, std::size_t most);
// [x, y] (mm) on a page.
std::array<double, 2> point_mm(const Json& value, const std::string& where);
// [x, y, w, h] (mm) on a page, w and h not negative.
std::array<double, 4> rect_mm(const Json& value, const std::string& where);
// [r, g, b], each 0..255.
std::array<int, 3> rgb(const Json& value, const std::string& where);
// An asset ref: "sha256:" and 64 lowercase hex digits (as the book's asset store names a file).
bool asset_ref(std::string_view ref);

Json to_json(const std::array<double, 2>& point);
Json to_json(const std::array<double, 4>& rect);
Json to_json(const std::array<int, 3>& rgb);

}  // namespace genko::core::production
