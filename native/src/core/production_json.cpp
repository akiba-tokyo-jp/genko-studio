// PC5 節 J: the strict reading the production's page keys share (core/production_json.hpp).

#include "core/production_json.hpp"

#include <algorithm>
#include <cmath>

#include "core/error.hpp"
#include "core/pyconv.hpp"

namespace genko::core::production {

namespace {

bool finite_number(const Json& v) { return v.is_number() && !v.is_boolean() && std::isfinite(v.get<double>()); }

std::string range(double lo, double hi) {
    const auto shown = [](double v) {
        if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<std::int64_t>(v));
        std::string s = std::to_string(v);
        while (!s.empty() && s.back() == '0') s.pop_back();
        return s;
    };
    return shown(lo) + ".." + shown(hi);
}

}  // namespace

void wrong(const std::string& where, const std::string& what) { throw Error("value", where + " " + what); }

void only_keys(const Json& value, const std::string& where, std::initializer_list<std::string_view> keys) {
    if (!value.is_object()) wrong(where, "must be an object");
    for (const auto& [key, v] : value.items()) {
        if (std::find(keys.begin(), keys.end(), std::string_view(key)) == keys.end()) {
            std::string known;
            for (const std::string_view k : keys) known += (known.empty() ? "" : ", ") + std::string(k);
            wrong(where, "has no setting " + py_repr_str(key) + " (" + known + ")");
        }
    }
}

const Json& field(const Json& object, std::string_view key, const std::string& where) {
    const auto it = object.find(std::string(key));
    if (it == object.end()) wrong(where, "needs " + std::string(key));
    return *it;
}

const Json* optional(const Json& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &*it;
}

double number(const Json& value, const std::string& where, double lo, double hi) {
    if (!finite_number(value)) wrong(where, "must be a number");
    const double v = value.get<double>();
    if (v < lo || v > hi) wrong(where, "must be within " + range(lo, hi));
    return v;
}

std::int64_t integer(const Json& value, const std::string& where, std::int64_t lo, std::int64_t hi) {
    if (!value.is_number_integer()) wrong(where, "must be a whole number");
    if (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) {
        wrong(where, "must be within " + range(static_cast<double>(lo), static_cast<double>(hi)));
    }
    const std::int64_t v = value.get<std::int64_t>();
    if (v < lo || v > hi) wrong(where, "must be within " + std::to_string(lo) + ".." + std::to_string(hi));
    return v;
}

bool boolean(const Json& value, const std::string& where) {
    if (!value.is_boolean()) wrong(where, "must be true or false");
    return value.get<bool>();
}

std::string text(const Json& value, const std::string& where, std::size_t most) {
    if (!value.is_string()) wrong(where, "must be a string");
    const std::string& s = value.get_ref<const std::string&>();
    if (s.size() > most) wrong(where, "is longer than " + std::to_string(most) + " bytes");
    return s;
}

std::array<double, 2> point_mm(const Json& value, const std::string& where) {
    if (!value.is_array() || value.size() != 2) wrong(where, "must be [x, y] (mm)");
    return {number(value[0], where + "[0]", -kCoordinateMaxMm, kCoordinateMaxMm), number(value[1], where + "[1]", -kCoordinateMaxMm, kCoordinateMaxMm)};
}

std::array<double, 4> rect_mm(const Json& value, const std::string& where) {
    if (!value.is_array() || value.size() != 4) wrong(where, "must be [x, y, w, h] (mm)");
    return {number(value[0], where + "[0]", -kCoordinateMaxMm, kCoordinateMaxMm), number(value[1], where + "[1]", -kCoordinateMaxMm, kCoordinateMaxMm),
            number(value[2], where + "[2]", 0, kCoordinateMaxMm), number(value[3], where + "[3]", 0, kCoordinateMaxMm)};
}

std::array<int, 3> rgb(const Json& value, const std::string& where) {
    if (!value.is_array() || value.size() != 3) wrong(where, "must be [r, g, b]");
    std::array<int, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) out[i] = static_cast<int>(integer(value[i], where + "[" + std::to_string(i) + "]", 0, 255));
    return out;
}

bool asset_ref(std::string_view ref) {
    if (ref.size() != 71 || ref.substr(0, 7) != "sha256:") return false;
    return std::all_of(ref.begin() + 7, ref.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

Json to_json(const std::array<double, 2>& point) { return Json::array({point[0], point[1]}); }
Json to_json(const std::array<double, 4>& rect) { return Json::array({rect[0], rect[1], rect[2], rect[3]}); }
Json to_json(const std::array<int, 3>& rgb) { return Json::array({rgb[0], rgb[1], rgb[2]}); }

}  // namespace genko::core::production
