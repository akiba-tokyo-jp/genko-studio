#include "render/tool_presets.hpp"

#include <algorithm>
#include <cmath>

#include "core/error.hpp"

// PhotoCraft v0.2.0 crates/engine/src/presets/tools.rs (a preset's form, its options, the built-in presets: Genko's own
// here, in the window's tools and mm), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::tool_presets {

using core::Json;

namespace {

[[noreturn]] void bad(const std::string& why) { throw core::Error("value", why); }

bool tool_name(std::string_view tool) {
    if (tool.empty() || tool.size() > 40) return false;
    return std::all_of(tool.begin(), tool.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

void check_options(const Json& options) {
    if (!options.is_object()) bad("a tool preset's options are an object");
    if (options.dump().size() > (std::size_t(64) << 10)) bad("a tool preset's options are at most 64 KB");
    for (const auto& [key, value] : options.items()) {
        if (key == "brush") {
            if (!value.is_string() || value.get<std::string>().empty() || value.get<std::string>().size() > 40) bad("options.brush is a brush's key (1..40 characters)");
        } else if (key == "fields") {
            if (!value.is_object()) bad("options.fields is an object of the tool's settings");
            for (const auto& [name, v] : value.items()) {
                if (name.empty() || name.size() > 64) bad("a field's name is 1..64 characters");
                if (!(v.is_boolean() || v.is_string() || (v.is_number() && std::isfinite(v.get<double>())))) {
                    bad("the field " + name + " is a number, a text or a yes/no");
                }
            }
        } else if (key == "foreground") {
            if (!value.is_array() || value.size() != 3) bad("options.foreground is [r, g, b] (0..255)");
            for (const Json& c : value) {
                if (!c.is_number_integer() || c.get<std::int64_t>() < 0 || c.get<std::int64_t>() > 255) bad("options.foreground is [r, g, b] (0..255)");
            }
        } else {
            bad("a tool preset's options are brush, fields and foreground (not " + key + ")");
        }
    }
}

void check(const Json& item) {
    for (const auto& [key, value] : item.items()) {
        if (key != "name" && key != "tool" && key != "options") bad("a tool preset is {name, tool, options} (not " + key + ")");
    }
    const auto tool = item.find("tool");
    if (tool == item.end() || !tool->is_string() || !tool_name(tool->get<std::string>())) bad("a tool preset's tool is a tool's name (a-z, 0-9, _)");
    if (const auto options = item.find("options"); options != item.end()) check_options(*options);
}

std::vector<preset_store::Group> builtin() {
    const auto brush = [](const char* key, double mm) { return Json{{"brush", key}, {"fields", Json{{"brush_size", mm}}}}; };
    return {preset_store::Group{"ツールプリセット",
                                {Json{{"name", "G ペン 0.6 mm"}, {"tool", "pen"}, {"options", brush("gpen", 0.6)}},
                                 Json{{"name", "ソフト円ブラシ 10 mm"}, {"tool", "pen"}, {"options", brush("my_genko_soft_round", 10.0)}},
                                 Json{{"name", "ハード円ブラシ 1.5 mm"}, {"tool", "pen"}, {"options", brush("my_genko_hard_round", 1.5)}},
                                 Json{{"name", "やわらかい消しゴム 6 mm"},
                                      {"tool", "eraser"},
                                      {"options", Json{{"fields", Json{{"eraser_size", 6.0}, {"eraser_texture", "soft"}}}}}},
                                 Json{{"name", "円形グラデーション"}, {"tool", "gradient"}, {"options", Json{{"fields", Json{{"gradient_shape", "radial"}}}}}}}}};
}

}  // namespace

const preset_store::Kind& kind() {
    static const preset_store::Kind k = [] {
        preset_store::Kind made;
        made.file = "tool_presets.json";
        made.default_group = "ツールプリセット";
        made.builtin = builtin;
        made.check = check;
        return made;
    }();
    return k;
}

Json item_of(std::string_view tool, std::optional<std::string> name, const Json& options) {
    if (!tool_name(tool)) bad("a tool preset's tool is a tool's name (a-z, 0-9, _)");
    Json item{{"name", name && !name->empty() ? *name : std::string(tool)}, {"tool", std::string(tool)},
              {"options", options.is_null() ? Json::object() : options}};
    check(item);
    return item;
}

Json list(const std::vector<preset_store::Group>& groups, std::optional<std::string_view> tool) {
    Json presets = Json::array();
    for (const preset_store::Group& g : groups) {
        for (const Json& item : g.items) {
            const std::string t = item.value("tool", std::string());
            if (tool && t != *tool) continue;
            presets.push_back(Json{{"name", item.value("name", std::string())}, {"tool", t}, {"group", g.name}});
        }
    }
    return Json{{"presets", std::move(presets)}};
}

const Json& select(const std::vector<preset_store::Group>& groups, std::string_view name) {
    const Json* found = preset_store::find(groups, name);
    if (found == nullptr) throw core::Error("key", "no tool preset named " + std::string(name) + " (see tool-presets list)");
    return *found;
}

}  // namespace genko::render::tool_presets
