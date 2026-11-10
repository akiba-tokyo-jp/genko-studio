// camera-raw@1 (PC4 F-e): a camera RAW file's development settings (core/raw_develop.hpp).

#include "core/raw_develop.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/adjust.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core::raw {

namespace {

template <std::size_t N>
bool in(const std::array<std::string_view, N>& set, std::string_view value) {
    return std::find(set.begin(), set.end(), value) != set.end();
}

template <std::size_t N>
std::string listed(const std::array<std::string_view, N>& set) {
    std::string out;
    for (std::size_t i = 0; i < N; ++i) out += (i == 0 ? "" : i + 1 == N ? " or " : ", ") + std::string(set[i]);
    return out;
}

double rounded(double v) { return std::round(v * 1e6) / 1e6; }

std::string text_of(const Json& value, const char* key) {
    if (!value.is_string()) throw OpError(std::string("develop.") + key + " is text");
    return value.get<std::string>();
}

double number_of(const Json& value, const char* key, double low, double high) {
    if (!value.is_number() || value.is_boolean()) throw OpError(std::string("develop.") + key + " is a number");
    const double v = value.get<double>();
    if (!std::isfinite(v)) throw OpError(std::string("develop.") + key + " is a finite number");
    return std::clamp(v, low, high);
}

template <std::size_t N>
std::string choice_of(const Json& value, const char* key, const std::array<std::string_view, N>& set) {
    const std::string text = text_of(value, key);
    if (!in(set, text)) throw OpError(std::string("develop.") + key + " is " + listed(set) + ", not " + py_repr_str(text));
    return text;
}

// camera_raw's settings with nothing that changes the picture (its seed apart).
const Json& adjust_defaults() {
    static const Json defaults = [] {
        Json out = normalize_filter("camera_raw", Json::object(), nullptr);
        out.erase("seed");
        return out;
    }();
    return defaults;
}

Json read_adjust(const Json& adjust) {
    if (!adjust.is_object()) throw OpError("adjust is camera_raw's settings (an object)");
    return normalize_filter("camera_raw", adjust, nullptr);
}

bool changes_nothing(const Json& adjust) {
    Json without = adjust;
    without.erase("seed");
    return without == adjust_defaults();
}

void check_camera(const Json& camera, bool op) {
    const auto refuse = [op](const std::string& message) {
        if (op) throw OpError(message);
        throw Error("value", message);
    };
    if (!camera.is_object()) refuse("camera is {make, model, format, libraw}");
    for (const auto& [key, value] : camera.items()) {
        if (!in(kCameraKeys, key)) refuse("camera's " + py_repr_str(key) + " is not known to this build");
        if (!value.is_string() || value.get_ref<const std::string&>().size() > kCameraTextMax) refuse("camera's " + key + " is text of at most 255 bytes");
    }
}

}  // namespace

Json to_json(const Develop& d) {
    Json out = Json::object();
    out["white_balance"] = d.white_balance;
    if (d.white_balance == "custom") {
        out["temperature_k"] = rounded(d.temperature_k);
        out["tint"] = rounded(d.tint);
    }
    out["exposure_ev"] = rounded(d.exposure_ev);
    out["demosaic"] = d.demosaic;
    out["highlights"] = d.highlights;
    out["space"] = d.space;
    out["depth"] = d.depth;
    out["orientation"] = d.orientation;
    out["sharpen"] = d.sharpen;
    return out;
}

Develop develop_from(const Json* develop) {
    Develop out;
    if (develop == nullptr || develop->is_null()) return out;
    if (!develop->is_object()) throw OpError("develop is an object");
    static constexpr std::string_view kKeys[] = {"white_balance", "temperature_k", "tint", "exposure_ev", "demosaic", "highlights",
                                                 "space", "depth", "orientation", "sharpen"};
    for (const auto& [key, value] : develop->items()) {
        if (std::find(std::begin(kKeys), std::end(kKeys), key) == std::end(kKeys)) throw OpError("develop's " + py_repr_str(key) + " is not known to this build");
    }
    if (const Json* v = get(*develop, "white_balance")) out.white_balance = choice_of(*v, "white_balance", kWhiteBalances);
    if (const Json* v = get(*develop, "temperature_k")) out.temperature_k = number_of(*v, "temperature_k", kTemperatureMin, kTemperatureMax);
    if (const Json* v = get(*develop, "tint")) out.tint = number_of(*v, "tint", -kTintMax, kTintMax);
    if (const Json* v = get(*develop, "exposure_ev")) out.exposure_ev = number_of(*v, "exposure_ev", -kExposureMax, kExposureMax);
    if (const Json* v = get(*develop, "demosaic")) out.demosaic = choice_of(*v, "demosaic", kDemosaics);
    if (const Json* v = get(*develop, "highlights")) out.highlights = choice_of(*v, "highlights", kHighlights);
    if (const Json* v = get(*develop, "space")) out.space = choice_of(*v, "space", kSpaces);
    if (const Json* v = get(*develop, "depth")) {
        if (!v->is_number_integer() || (v->get<std::int64_t>() != 8 && v->get<std::int64_t>() != 16)) throw OpError("develop.depth is 8 or 16");
        out.depth = static_cast<int>(v->get<std::int64_t>());
    }
    if (const Json* v = get(*develop, "orientation")) out.orientation = choice_of(*v, "orientation", kOrientations);
    if (const Json* v = get(*develop, "sharpen")) out.sharpen = choice_of(*v, "sharpen", kSharpens);
    if (out.white_balance != "custom") {  // (only a custom white names its temperature)
        out.temperature_k = Develop{}.temperature_k;
        out.tint = 0;
    }
    return out;
}

Json normal_raw(const Json& raw) {
    if (!raw.is_object()) throw OpError("raw is {develop, adjust?, camera}");
    for (const auto& [key, value] : raw.items()) {
        if (key != "develop" && key != "adjust" && key != "camera") throw OpError("raw's " + py_repr_str(key) + " is not known to this build");
    }
    Json out = Json::object();
    out["develop"] = to_json(develop_from(get(raw, "develop")));
    if (const Json* adjust = get(raw, "adjust"); adjust != nullptr && !adjust->is_null()) {
        Json read = read_adjust(*adjust);
        if (!changes_nothing(read)) out["adjust"] = std::move(read);
    }
    Json camera = Json::object();
    if (const Json* given = get(raw, "camera"); given != nullptr && !given->is_null()) {
        check_camera(*given, true);
        camera = *given;
    }
    out["camera"] = std::move(camera);
    return out;
}

void check_raw(const Json& raw) {
    if (!raw.is_object()) throw Error("value", "its raw is {develop, adjust?, camera}");
    for (const auto& [key, value] : raw.items()) {
        if (key != "develop" && key != "adjust" && key != "camera") throw Error("value", "its raw's " + py_repr_str(key) + " is not known to this build");
    }
    try {
        (void)develop_from(get(raw, "develop"));
        if (const Json* adjust = get(raw, "adjust"); adjust != nullptr && !adjust->is_null()) (void)read_adjust(*adjust);
    } catch (const OpError& error) {
        throw Error("value", std::string("its raw: ") + error.what());
    }
    if (const Json* camera = get(raw, "camera"); camera != nullptr && !camera->is_null()) check_camera(*camera, false);
}

Develop develop_of(const std::optional<Json>& raw) {
    if (!raw || !raw->is_object()) return Develop{};
    return develop_from(get(*raw, "develop"));
}

std::optional<Json> adjust_of(const std::optional<Json>& raw) {
    if (!raw || !raw->is_object()) return std::nullopt;
    const Json* adjust = get(*raw, "adjust");
    if (adjust == nullptr || adjust->is_null()) return std::nullopt;
    Json read = read_adjust(*adjust);
    if (changes_nothing(read)) return std::nullopt;
    return read;
}

}  // namespace genko::core::raw
