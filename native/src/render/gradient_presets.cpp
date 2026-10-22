#include "render/gradient_presets.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>

#include "core/error.hpp"

// PhotoCraft v0.2.0 crates/engine/src/presets/gradients.rs (lines 17-175: the preset, its stops and transparency, the
// built-in groups and their colours), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::gradient_presets {

using core::Json;

namespace {

[[noreturn]] void bad(const std::string& why) { throw core::Error("value", why); }

std::array<int, 3> hex(std::string_view text) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6 || !std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; })) {
        bad("a colour is [r, g, b], \"#rrggbb\", \"foreground\" or \"background\"");
    }
    std::array<int, 3> out{};
    for (std::size_t k = 0; k < 3; ++k) out[k] = std::stoi(std::string(text.substr(k * 2, 2)), nullptr, 16);
    return out;
}

// A stop's colour as kept: [r, g, b] 0..255, "foreground" or "background".
Json colour_of(const Json& v) {
    if (v == Json("foreground") || v == Json("background")) return v;
    if (v.is_string()) {
        const auto rgb = hex(v.get<std::string>());
        return Json::array({rgb[0], rgb[1], rgb[2]});
    }
    if (!v.is_array() || v.size() != 3) bad("a colour is [r, g, b], \"#rrggbb\", \"foreground\" or \"background\"");
    Json out = Json::array();
    for (const Json& c : v) {
        if (!c.is_number_integer() || c.get<std::int64_t>() < 0 || c.get<std::int64_t>() > 255) bad("a colour's r, g, b are whole numbers 0..255");
        out.push_back(c.get<std::int64_t>());
    }
    return out;
}

double place(const Json& v, const char* what) {
    if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < 0 || v.get<double>() > 1) bad(std::string(what) + " is 0..1");
    return std::round(v.get<double>() * 10000.0) / 10000.0;
}

Json stops_of(const Json& given) {
    if (!given.is_array()) bad("stops is [[position 0..1, colour], …]");
    std::vector<std::pair<double, Json>> stops;
    for (std::size_t i = 0; i < given.size(); ++i) {
        const Json& s = given[i];
        if (s.is_array() && s.size() == 2 && s[0].is_number()) {
            stops.emplace_back(place(s[0], "a stop's position"), colour_of(s[1]));
        } else if (s.is_string() || (s.is_array() && s.size() == 3 && s[0].is_number_integer())) {
            // (a list of colours, spaced evenly: upstream's)
            stops.emplace_back(given.size() > 1 ? static_cast<double>(i) / static_cast<double>(given.size() - 1) : 0.0, colour_of(s));
        } else {
            bad("stops is [[position 0..1, colour], …]");
        }
    }
    if (stops.size() < 2 || stops.size() > 16) bad("a gradient has 2 to 16 colour stops");
    std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    Json out = Json::array();
    for (auto& [t, colour] : stops) out.push_back(Json::array({t, std::move(colour)}));
    return out;
}

Json transparency_of(const Json& given, bool percent) {
    if (!given.is_array()) bad("transparency is [[position 0..1, opacity 0..1], …]");
    std::vector<std::pair<double, double>> stops;
    for (const Json& s : given) {
        if (!s.is_array() || s.size() != 2 || !s[1].is_number()) bad("transparency is [[position 0..1, opacity 0..1], …]");
        double opacity = s[1].get<double>();
        if (percent) opacity /= 100.0;
        if (!std::isfinite(opacity) || opacity < 0 || opacity > 1) bad("a transparency stop's opacity is 0..1");
        stops.emplace_back(place(s[0], "a transparency stop's position"), std::round(opacity * 10000.0) / 10000.0);
    }
    if (stops.size() < 2 || stops.size() > 16) bad("a gradient has 2 to 16 transparency stops");
    std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    Json out = Json::array();
    for (const auto& [t, opacity] : stops) out.push_back(Json::array({t, opacity}));
    return out;
}

void check(const Json& item) {
    for (const auto& [key, value] : item.items()) {
        if (key != "name" && key != "stops" && key != "transparency") bad("a gradient preset has no " + key);
    }
    const Json* stops = core::get(item, "stops");
    if (stops == nullptr) bad("a gradient preset has its stops");
    if (stops_of(*stops) != *stops) bad("a gradient preset's stops are [[position, colour], …] in order");
    if (const Json* t = core::get(item, "transparency"); t != nullptr && transparency_of(*t, false) != *t) {
        bad("a gradient preset's transparency is [[position, opacity], …] in order");
    }
}

Json preset(const char* name, std::initializer_list<const char*> colours) {
    Json list = Json::array();
    for (const char* c : colours) list.push_back(c);
    return item_of(Json{{"stops", list}}, name);
}

std::vector<preset_store::Group> builtin() {
    const auto family = [](const char* prefix, std::initializer_list<std::initializer_list<const char*>> sets) {
        std::vector<Json> items;
        int n = 1;
        for (const auto& colours : sets) {
            char name[64];
            std::snprintf(name, sizeof name, "%s %02d", prefix, n++);
            items.push_back(preset(name, colours));
        }
        return items;
    };
    std::vector<preset_store::Group> out;
    out.push_back({"基本",
                   {Json{{"name", "描画色から背景色へ"}, {"stops", Json::array({Json::array({0.0, "foreground"}), Json::array({1.0, "background"})})}},
                    Json{{"name", "描画色から透明に"},
                         {"stops", Json::array({Json::array({0.0, "foreground"}), Json::array({1.0, "foreground"})})},
                         {"transparency", Json::array({Json::array({0.0, 1.0}), Json::array({1.0, 0.0})})}},
                    preset("黒、白", {"#000000", "#ffffff"})}});
    out.push_back({"ブルー", family("ブルー", {{"#0b3d91", "#4fa3e0"},
                                               {"#1c2a5a", "#3f6fd8", "#a8d4ff"},
                                               {"#00b4d8", "#03045e"},
                                               {"#5ec8f2", "#e6f6ff"},
                                               {"#253b80", "#20b2aa"},
                                               {"#89c2ff", "#1d4ed8", "#0a1a40"}})});
    out.push_back({"パープル", family("パープル", {{"#3c096c", "#c77dff"},
                                                   {"#5a189a", "#e0aaff"},
                                                   {"#240046", "#7b2cbf", "#f3d9ff"},
                                                   {"#9d4edd", "#4361ee"},
                                                   {"#6a0572", "#ab83a1"},
                                                   {"#2d0b59", "#ff6fd8"}})});
    out.push_back({"ピンク", family("ピンク", {{"#ff4d8d", "#ffd1e1"},
                                               {"#c9184a", "#ff8fab"},
                                               {"#ff70a6", "#ffd670"},
                                               {"#8e2c5a", "#f7a1c4", "#fff0f5"},
                                               {"#ff5fa2", "#a855f7"},
                                               {"#ffc2d6", "#ff2e63"}})});
    out.push_back({"レッド", family("レッド", {{"#7f0000", "#ff3b30"},
                                               {"#d00000", "#ffba08"},
                                               {"#370617", "#9d0208", "#f48c06"},
                                               {"#e5383b", "#f5f3f4"},
                                               {"#a4161a", "#0b090a"},
                                               {"#ff595e", "#ffca3a"}})});
    out.push_back({"オレンジ", family("オレンジ", {{"#ff6d00", "#ffd60a"},
                                                   {"#e85d04", "#ffba08"},
                                                   {"#9c3d0b", "#ff9e40", "#fff1d6"},
                                                   {"#ff7b00", "#ff0054"},
                                                   {"#fb8500", "#023047"},
                                                   {"#ffb703", "#fb5607"}})});
    out.push_back({"グリーン", family("グリーン", {{"#004b23", "#70e000"},
                                                   {"#2d6a4f", "#b7e4c7"},
                                                   {"#1b4332", "#40916c", "#d8f3dc"},
                                                   {"#38b000", "#ccff33"},
                                                   {"#0b6e4f", "#08a0a8"},
                                                   {"#606c38", "#dda15e"}})});
    out.push_back({"ブラウン", family("ブラウン", {{"#3e2412", "#a47148"},
                                                   {"#6f4518", "#e6ccb2"},
                                                   {"#2b1708", "#7f5539", "#ddb892"},
                                                   {"#8b5e34", "#ffd8a8"},
                                                   {"#582f0e", "#b6ad90"},
                                                   {"#4a3728", "#c08552", "#f3e9dc"}})});
    out.push_back({"グレー", family("グレー", {{"#212529", "#f8f9fa"},
                                               {"#495057", "#dee2e6"},
                                               {"#000000", "#6c757d", "#ffffff"},
                                               {"#343a40", "#adb5bd", "#343a40"},
                                               {"#8d99ae", "#2b2d42"},
                                               {"#e9ecef", "#ced4da", "#868e96"}})});
    return out;
}

// A colour as 0..255 doubles: [r, g, b] or a precise colour (0..1 × 255).
std::array<double, 3> levels(const Json& colour) {
    std::array<double, 3> out{};
    if (colour.is_object() && colour.contains("values") && colour["values"].is_array() && colour["values"].size() == 3) {
        for (std::size_t k = 0; k < 3; ++k) out[k] = colour["values"][k].get<double>() * 255.0;
    } else if (colour.is_array() && colour.size() >= 3) {
        for (std::size_t k = 0; k < 3; ++k) out[k] = colour[k].get<double>();
    }
    return out;
}

}  // namespace

const preset_store::Kind& kind() {
    static const preset_store::Kind k = [] {
        preset_store::Kind made;
        made.file = "gradients.json";
        made.default_group = "基本";
        made.builtin = builtin;
        made.check = check;
        return made;
    }();
    return k;
}

Json item_of(const Json& params, std::string_view name, bool percent) {
    if (!params.is_object()) bad("a gradient is {stops, transparency?}");
    const Json* stops = core::get(params, "stops");
    if (stops == nullptr) bad("a gradient needs its stops");
    Json out{{"name", std::string(name)}, {"stops", stops_of(*stops)}};
    if (const Json* t = core::get(params, "transparency"); t != nullptr && !t->is_null() && !(t->is_array() && t->empty())) {
        out["transparency"] = transparency_of(*t, percent);
    }
    return out;
}

Json stops_for(const Json& item, const Json& foreground, const Json& background, bool transparency) {
    Json stops = Json::array();
    for (const Json& s : item.value("stops", Json::array())) {
        const Json& c = s[1];
        stops.push_back(Json::array({s[0], c == Json("foreground") ? foreground : c == Json("background") ? background : c, 1.0}));
    }
    Json out{{"stops", std::move(stops)}};
    if (transparency && item.contains("transparency")) out["transparency"] = item["transparency"];
    return out;
}

std::array<int, 4> sample(const Json& item, double t, const std::array<int, 3>& foreground, const std::array<int, 3>& background) {
    const Json fg = Json::array({foreground[0], foreground[1], foreground[2]});
    const Json bg = Json::array({background[0], background[1], background[2]});
    const auto interp = [t](const std::vector<std::pair<double, std::array<double, 4>>>& stops) {
        if (stops.empty()) return std::array<double, 4>{0, 0, 0, 1};
        if (t <= stops.front().first) return stops.front().second;
        for (std::size_t i = 1; i < stops.size(); ++i) {
            if (t <= stops[i].first) {
                const double span = stops[i].first - stops[i - 1].first;
                const double k = span > 0 ? (t - stops[i - 1].first) / span : 1.0;
                std::array<double, 4> out{};
                for (std::size_t c = 0; c < 4; ++c) out[c] = stops[i - 1].second[c] + (stops[i].second[c] - stops[i - 1].second[c]) * k;
                return out;
            }
        }
        return stops.back().second;
    };
    std::vector<std::pair<double, std::array<double, 4>>> colours, opacity;
    for (const Json& s : stops_for(item, fg, bg)["stops"]) {
        const auto rgb = levels(s[1]);
        colours.push_back({s[0].get<double>(), {rgb[0], rgb[1], rgb[2], 1}});
    }
    for (const Json& s : item.value("transparency", Json::array())) opacity.push_back({s[0].get<double>(), {0, 0, 0, s[1].get<double>()}});
    const auto c = interp(colours);
    const double a = opacity.empty() ? 1.0 : interp(opacity)[3];
    return {static_cast<int>(std::lround(std::clamp(c[0], 0.0, 255.0))), static_cast<int>(std::lround(std::clamp(c[1], 0.0, 255.0))),
            static_cast<int>(std::lround(std::clamp(c[2], 0.0, 255.0))), static_cast<int>(std::lround(std::clamp(a, 0.0, 1.0) * 255.0))};
}

}  // namespace genko::render::gradient_presets
