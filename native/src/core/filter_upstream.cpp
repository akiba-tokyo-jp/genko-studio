// PC4 D7 (SPEC.md COLOR-08「フィルター」, design 2.9): PhotoCraft's smart filters — a command id and its params, as a .pcraft
// keeps them (crates/doc SmartFilter) and as this build reads Photoshop's of a PSD (render/psd_filters) — and Genko's kinds
// of フィルター (core/filter_kinds.hpp), both ways. PhotoCraft v0.2.0's params (engine/src/filters.rs 41-134 params_for,
// filters_ext.rs 70-371, gallery_cmds.rs 15-79, lens_cmds.rs 40-155 and 376-422; algo/src/params_ext.rs: pins, lights and
// paths), MIT: native/third_party/photocraft/NOTICE.md.

#include "core/filter_kinds.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "core/adjust.hpp"
#include "core/error.hpp"
#include "core/gallery_kinds.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

// A setting's settings that Genko's form cannot hold (a file, a channel, a path without its canvas): the command is not
// mapped.
struct Unmapped {};

// PhotoCraft's camelCase and Genko's snake_case ("centerX" ↔ "center_x"; "correctCA" ↔ "correct_ca", "channel1" and
// "prime35" as they are).
std::string snake(std::string_view key) {
    std::string out;
    for (std::size_t i = 0; i < key.size(); ++i) {
        const char c = key[i];
        const bool upper = c >= 'A' && c <= 'Z';
        if (!upper) {
            out.push_back(c);
            continue;
        }
        const bool after_upper = i > 0 && key[i - 1] >= 'A' && key[i - 1] <= 'Z';
        const bool before_lower = i + 1 < key.size() && key[i + 1] >= 'a' && key[i + 1] <= 'z';
        if (i > 0 && key[i - 1] != '_' && (!after_upper || before_lower)) out.push_back('_');
        out.push_back(static_cast<char>(c - 'A' + 'a'));
    }
    return out;
}

std::string camel(std::string_view key) {
    if (key == "correct_ca") return "correctCA";
    std::string out;
    bool up = false;
    for (const char c : key) {
        if (c == '_') {
            up = !out.empty();
            continue;
        }
        out.push_back(up && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
        up = false;
    }
    return out;
}

bool finite_number(const Json& v) { return v.is_number() && !v.is_boolean() && std::isfinite(v.get<double>()); }

// The document's pixels at `dpi` and the working 200 dpi's; and the page's mm.
double to_working(double px, double dpi) { return px * 200.0 / dpi; }
double from_working(double px, double dpi) { return px * dpi / 200.0; }
double to_mm(double px, double dpi) { return px * 25.4 / dpi; }
double from_mm(double mm, double dpi) { return mm * dpi / 25.4; }

// A length's number through `f` (anything else as it is: its kind refuses it).
void scale_at(Json& object, const std::string& key, const std::function<double(double)>& f) {
    if (!object.is_object() || !object.contains(key) || !finite_number(object[key])) return;
    object[key] = f(object[key].get<double>());
}

// The settings each kind holds in pixels (the working 200 dpi's; PhotoCraft's: the document's).
const std::vector<std::string>& lengths_of(std::string_view kind) {
    static const std::map<std::string, std::vector<std::string>, std::less<>> all{
        {"blur_gaussian", {"radius"}},
        {"blur_box", {"radius"}},
        {"blur_motion", {"distance"}},
        {"blur_surface", {"radius"}},
        {"blur_smart", {"radius"}},
        {"blur_shape", {"radius"}},
        {"blur_lens", {"radius"}},
        {"sharpen_unsharp_mask", {"radius"}},
        {"sharpen_smart", {"radius"}},
        {"noise_dust_and_scratches", {"radius"}},
        {"noise_median", {"radius"}},
        {"other_high_pass", {"radius"}},
        {"other_maximum", {"radius"}},
        {"other_minimum", {"radius"}},
        {"other_offset", {"horizontal", "vertical"}},
        {"distort_wave", {"wavelength_min", "wavelength_max", "amplitude_min", "amplitude_max"}},
        {"pixelate_color_halftone", {"max_radius"}},
        {"pixelate_crystallize", {"cell_size"}},
        {"pixelate_mosaic", {"cell_size"}},
        {"pixelate_pointillize", {"cell_size"}},
        {"stylize_emboss", {"height"}},
        {"stylize_extrude", {"size", "depth"}},
        {"blur_tilt_shift", {"blur"}},
        {"camera_raw", {"sharpen_radius"}},
    };
    static const std::vector<std::string> none;
    const auto found = all.find(kind);
    return found == all.end() ? none : found->second;
}

// A colour of PhotoCraft's — [r, g, b(, a)] 0..1 or "#rrggbb(aa)" — as Genko's [r, g, b] 0..255.
Json colour_in(const Json& v) {
    std::array<double, 3> c{};
    if (v.is_array() && v.size() >= 3 && v.size() <= 4) {
        for (std::size_t i = 0; i < 3; ++i) {
            if (!finite_number(v[i])) throw Unmapped{};
            c[i] = v[i].get<double>();
        }
    } else if (v.is_string()) {
        std::string_view hex = v.get_ref<const std::string&>();
        if (hex.starts_with('#')) hex.remove_prefix(1);
        if (hex.size() != 6 && hex.size() != 8) throw Unmapped{};
        for (std::size_t i = 0; i < 3; ++i) {
            int byte = 0;
            for (std::size_t k = 0; k < 2; ++k) {
                const char ch = hex[i * 2 + k];
                const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                if (d < 0) throw Unmapped{};
                byte = byte * 16 + d;
            }
            c[i] = byte / 255.0;
        }
    } else {
        throw Unmapped{};
    }
    Json out = Json::array();
    for (const double x : c) out.push_back(static_cast<int>(std::lround(std::clamp(x, 0.0, 1.0) * 255.0)));
    return out;
}

// Genko's [r, g, b] 0..255 as PhotoCraft's 0..1.
Json colour_out(const Json& v) {
    if (!v.is_array() || v.size() != 3) throw Unmapped{};
    Json out = Json::array();
    for (const Json& x : v) {
        if (!finite_number(x)) throw Unmapped{};
        out.push_back(x.get<double>() / 255.0);
    }
    return out;
}

const AdjustParam* param_of(const std::vector<AdjustParam>& params, std::string_view key) {
    for (const AdjustParam& p : params)
        if (p.key == key) return &p;
    return nullptr;
}

// A choice's value of PhotoCraft's as Genko's: its snake_case when that is one of the choices, else as it is (a value of
// neither: its kind refuses it).
Json choice_in(const AdjustParam& p, const Json& v, std::string_view kind) {
    if (!v.is_string()) return v;
    const std::string& given = v.get_ref<const std::string&>();
    if (kind == "lens_correction" && p.key == "edge") {  // (lens_cmds.rs: "edgeExtension" | "extension", "background" | "color")
        if (given == "edgeExtension") return Json("extension");
        if (given == "background") return Json("color");
    }
    const std::string s = snake(given);
    if (std::find(p.choices.begin(), p.choices.end(), s) != p.choices.end()) return Json(s);
    return v;
}

// The settings of the kind's table out of PhotoCraft's params (keys it has no setting for left out), each by its type.
Json table_in(std::string_view kind, const std::vector<AdjustParam>& table, const Json& given) {
    Json out = Json::object();
    if (!given.is_object()) return out;
    for (const auto& [key, value] : given.items()) {
        const std::string k = snake(key);
        const AdjustParam* p = param_of(table, k);
        if (p == nullptr) continue;
        switch (p->type) {
            case AdjustParam::Type::Choice: out[k] = choice_in(*p, value, kind); break;
            case AdjustParam::Type::Rgb: out[k] = colour_in(value); break;
            default: out[k] = value; break;
        }
    }
    return out;
}

Json table_out(const std::vector<AdjustParam>& table, const Json& read) {
    Json out = Json::object();
    for (const auto& [key, value] : read.items()) {
        const AdjustParam* p = param_of(table, key);
        if (p != nullptr && p->type == AdjustParam::Type::Rgb) out[camel(key)] = colour_out(value);
        else if (p != nullptr && p->type == AdjustParam::Type::Choice && value.is_string()) out[camel(key)] = camel(value.get_ref<const std::string&>());
        else out[camel(key)] = value;
    }
    return out;
}

// An object's keys snake_case (PhotoCraft's pins, lights and paths are serde camelCase), each through `f`.
Json keys_in(const Json& object) {
    if (!object.is_object()) throw Unmapped{};
    Json out = Json::object();
    for (const auto& [key, value] : object.items()) out[snake(key)] = value;
    return out;
}

Json keys_out(const Json& object) {
    Json out = Json::object();
    for (const auto& [key, value] : object.items()) out[camel(key)] = value;
    return out;
}

// A list PhotoCraft reads as given when it holds something (params_ext's list: else its flat settings, one of them).
const Json* listed(const Json& given, const char* key) {
    const Json* list = get(given, key);
    return list != nullptr && list->is_array() && !list->empty() ? list : nullptr;
}

// ぼかしギャラリー's pins (filters_ext.rs 297-336): a list, or one pin of the flat settings (centerX, centerY, radiusX…).
Json pins_in(std::string_view kind, const Json& given, double dpi) {
    Json pins = Json::array();
    if (const Json* list = listed(given, "pins")) {
        for (const Json& pin : *list) pins.push_back(keys_in(pin));
    } else {
        static const std::vector<std::pair<std::string_view, std::string_view>> kFlat{{"centerX", "x"},        {"centerY", "y"},     {"radiusX", "radius_x"},
                                                                                      {"radiusY", "radius_y"}, {"angle", "angle"},   {"roundness", "roundness"},
                                                                                      {"feather", "feather"},  {"blur", "blur"},     {"blurAngle", "blur_angle"}};
        const std::set<std::string_view> takes = kind == "blur_field"  ? std::set<std::string_view>{"x", "y", "blur"}
                                                 : kind == "blur_iris" ? std::set<std::string_view>{"x", "y", "radius_x", "radius_y", "angle", "roundness", "feather", "blur"}
                                                                       : std::set<std::string_view>{"x", "y", "radius_x", "radius_y", "angle", "blur_angle"};
        Json pin = Json::object();
        for (const auto& [up, genko] : kFlat) {
            if (const Json* v = get(given, up); v != nullptr && takes.contains(genko)) pin[std::string(genko)] = *v;
        }
        if (pin.empty()) return Json(nullptr);  // (upstream's one pin by default: Genko's too)
        pins.push_back(std::move(pin));
    }
    for (Json& pin : pins) scale_at(pin, "blur", [dpi](double v) { return to_working(v, dpi); });
    return pins;
}

// 照明効果's lights (filters_ext.rs 191-216): a list, or one light of the flat settings (lightType, lightX…).
Json lights_in(const Json& given) {
    Json lights = Json::array();
    if (const Json* list = listed(given, "lights")) {
        for (const Json& light : *list) lights.push_back(keys_in(light));
    } else {
        static const std::vector<std::pair<std::string_view, std::string_view>> kFlat{
            {"lightType", "kind"}, {"lightX", "x"},     {"lightY", "y"},         {"lightZ", "z"},         {"targetX", "target_x"}, {"targetY", "target_y"},
            {"angle", "angle"},    {"elevation", "elevation"}, {"color", "color"}, {"intensity", "intensity"}, {"cone", "cone"},     {"hotspot", "hotspot"},
            {"radius", "radius"}};
        Json light = Json::object();
        for (const auto& [up, genko] : kFlat) {
            if (const Json* v = get(given, up)) light[std::string(genko)] = *v;
        }
        if (light.empty()) return Json(nullptr);
        lights.push_back(std::move(light));
    }
    for (Json& light : lights) {
        if (light.contains("color")) light["color"] = colour_in(light["color"]);
    }
    return lights;
}

// パスぼかし's paths (params_ext.rs BlurPath: points shares of the canvas, speed pixels): a list, or one of the flat settings.
Json paths_in(const Json& given, double dpi, UpstreamCanvas canvas) {
    Json paths = Json::array();
    if (const Json* list = listed(given, "paths")) {
        for (const Json& path : *list) paths.push_back(keys_in(path));
    } else {
        static constexpr std::array<const char*, 6> kFlat{"startX", "startY", "endX", "endY", "speed", "taper"};
        const bool flat = std::any_of(kFlat.begin(), kFlat.end(), [&given](const char* key) { return get(given, key) != nullptr; });
        if (!flat) return Json(nullptr);  // (upstream's path across the middle: Genko's by default too)
        const auto at = [&given](const char* key, double fallback) {
            const Json* v = get(given, key);
            if (v == nullptr) return fallback;
            if (!finite_number(*v)) throw Unmapped{};
            return v->get<double>();
        };
        Json path{{"points", Json::array({Json::array({at("startX", 0.2), at("startY", 0.5)}), Json::array({at("endX", 0.8), at("endY", 0.5)})})}};
        if (const Json* v = get(given, "speed")) path["speed"] = *v;
        if (const Json* v = get(given, "taper")) path["taper"] = *v;
        paths.push_back(std::move(path));
    }
    if (!(canvas.width_mm > 0) || !(canvas.height_mm > 0)) throw Unmapped{};  // (its shares need the canvas)
    for (Json& path : paths) {
        if (const auto points = path.find("points"); points != path.end() && points->is_array()) {
            for (Json& p : *points) {
                if (!p.is_array() || p.size() != 2 || !finite_number(p[0]) || !finite_number(p[1])) throw Unmapped{};
                p = Json::array({p[0].get<double>() * canvas.width_mm, p[1].get<double>() * canvas.height_mm});
            }
        }
        scale_at(path, "speed", [dpi](double v) { return to_working(v, dpi); });
    }
    return paths;
}

// A point of the document's pixels as the page's mm, and back.
Json point_in(const Json& p, double dpi) {
    if (!p.is_array() || p.size() != 2 || !finite_number(p[0]) || !finite_number(p[1])) throw Unmapped{};
    return Json::array({to_mm(p[0].get<double>(), dpi), to_mm(p[1].get<double>(), dpi)});
}

Json point_out(const Json& p, double dpi) {
    if (!p.is_array() || p.size() != 2 || !finite_number(p[0]) || !finite_number(p[1])) throw Unmapped{};
    return Json::array({from_mm(p[0].get<double>(), dpi), from_mm(p[1].get<double>(), dpi)});
}

// One effect of the gallery (gallery_cmds.rs effect_from_json): its settings beside its key or in "params", its colours
// its own or the stack's (`fallback`).
Json effect_in(const GalleryEffectKind& effect, const Json& params, const Json& fallback, bool visible) {
    Json out{{"filter", std::string(effect.filter)}, {"visible", visible}};
    Json read = Json::object();
    if (params.is_object()) {
        for (const auto& [key, value] : params.items()) {
            const std::string k = snake(key);
            const AdjustParam* p = param_of(effect.params, k);
            if (p == nullptr) continue;
            read[k] = p->type == AdjustParam::Type::Rgb ? colour_in(value) : value;
        }
    }
    // (ネオン光彩's colour: its own, else the stack's)
    if (param_of(effect.params, "glow_color") != nullptr && !read.contains("glow_color")) {
        if (const Json* c = fallback.is_object() ? get(fallback, "glowColor") : nullptr) read["glow_color"] = colour_in(*c);
    }
    out["params"] = std::move(read);
    for (const char* key : {"foreground", "background"}) {
        if (const Json* c = params.is_object() ? get(params, key) : nullptr) out[key] = colour_in(*c);
    }
    return out;
}

Json gallery_in(std::string_view command, const Json& given) {
    Json read = Json::object();
    for (const char* key : {"foreground", "background"}) {
        if (const Json* c = get(given, key)) read[key] = colour_in(*c);
    }
    Json effects = Json::array();
    if (command == "filter.filterGallery") {
        const Json* list = get(given, "effects");
        if (list == nullptr || !list->is_array()) throw Unmapped{};
        for (const Json& item : *list) {
            if (!item.is_object()) throw Unmapped{};
            const Json* visible = get(item, "visible");
            const bool shown = !(visible != nullptr && visible->is_boolean() && !visible->get<bool>());
            const Json* key = get(item, "filter");
            const GalleryEffectKind* effect = nullptr;
            if (key != nullptr && key->is_string()) {
                for (const GalleryEffectKind& each : gallery_effects())
                    if (each.upstream == key->get_ref<const std::string&>()) effect = &each;
            }
            if (effect == nullptr) {
                if (!shown) continue;  // (upstream reads past a hidden effect without looking at it: it draws nothing)
                throw Unmapped{};      // (an effect upstream does not know: upstream draws none of the stack)
            }
            const Json* params = get(item, "params");
            effects.push_back(effect_in(*effect, params != nullptr && params->is_object() ? *params : item, given, shown));
        }
    } else {
        const std::string_view key = command.substr(std::string_view("filter.gallery.").size());
        const GalleryEffectKind* effect = nullptr;
        for (const GalleryEffectKind& each : gallery_effects())
            if (each.upstream == key) effect = &each;
        if (effect == nullptr) throw Unmapped{};
        effects.push_back(effect_in(*effect, given, Json(nullptr), true));
    }
    if (effects.empty()) throw Unmapped{};
    read["effects"] = std::move(effects);
    return read;
}

Json gallery_out(const Json& read) {
    Json effects = Json::array();
    for (const Json& effect : read.at("effects")) {
        const GalleryEffectKind* kind = gallery_effect(effect.at("filter").get<std::string>());
        if (kind == nullptr) throw Unmapped{};
        Json params = Json::object();
        const Json given = effect.value("params", Json::object());
        for (const auto& [key, value] : given.items()) {
            const AdjustParam* p = param_of(kind->params, key);
            params[camel(key)] = p != nullptr && p->type == AdjustParam::Type::Rgb ? colour_out(value) : value;
        }
        for (const char* key : {"foreground", "background"}) {
            if (effect.contains(key)) params[key] = colour_out(effect[key]);
        }
        effects.push_back(Json{{"filter", std::string(kind->upstream)}, {"params", std::move(params)}, {"visible", effect.value("visible", true)}});
    }
    Json out{{"effects", std::move(effects)}};
    for (const char* key : {"foreground", "background"}) {
        if (read.contains(key)) out[key] = colour_out(read[key]);
    }
    return out;
}

// The kind a command is (a preset: its kind and settings), none for one not in the table.
std::optional<std::pair<std::string, Json>> kind_of(std::string_view command, const Json& given) {
    for (const FilterPreset& preset : filter_presets()) {
        if (preset.upstream == command && pc4_filter(preset.kind)) return std::make_pair(std::string(preset.kind), preset.params);
    }
    if (command == "filter.filterGallery" || command.starts_with("filter.gallery.")) return std::make_pair(std::string("filter_gallery"), given);
    for (const FilterInfo& info : filter_infos()) {
        if (info.upstream == command) return std::make_pair(std::string(info.kind), given);
    }
    return std::nullopt;
}

// Upstream's smart filters this build has no kind of (distort_cmds.rs, plugin_cmds.rs).
constexpr std::array<std::string_view, 6> kUpstreamOnly{"filter.liquify",           "edit.puppetWarp",    "layer.smartObjects.puppetWarp",
                                                        "edit.perspectiveWarp",     "layer.smartObjects.perspectiveWarp", "plugin.run"};

}  // namespace

std::optional<std::pair<std::string, Json>> from_upstream(std::string_view command, const Json& params, double dpi, UpstreamCanvas canvas) {
    if (!(dpi > 0) || !std::isfinite(dpi)) return std::nullopt;
    const Json given = params.is_object() ? params : Json::object();
    auto found = kind_of(command, given);
    if (!found) return std::nullopt;
    const std::string& kind = found->first;
    const AdjustKind* table = adjust_kind(kind);
    if (table == nullptr) return std::nullopt;
    try {
        Json read;
        if (kind == "filter_gallery") {
            read = gallery_in(command, given);
        } else {
            read = table_in(kind, table->params, found->second);
            // (an empty list upstream is its one pin, light or path by default — Genko's [] lights are none —: each list
            // read again here, not as the table copied it)
            if (kind == "blur_field" || kind == "blur_iris" || kind == "blur_spin") {
                read.erase("pins");
                if (Json pins = pins_in(kind, given, dpi); !pins.is_null()) read["pins"] = std::move(pins);
            } else if (kind == "blur_path") {
                read.erase("paths");
                if (Json paths = paths_in(given, dpi, canvas); !paths.is_null()) read["paths"] = std::move(paths);
            } else if (kind == "render_lighting_effects") {
                read.erase("lights");
                if (Json lights = lights_in(given); !lights.is_null()) read["lights"] = std::move(lights);
                if (const Json* texture = get(read, "texture"); texture != nullptr && !texture->is_string()) throw Unmapped{};
            } else if (kind == "adaptive_wide_angle") {
                if (const Json* list = get(given, "constraints"); list != nullptr && !list->is_null()) {
                    if (!list->is_array()) throw Unmapped{};
                    Json lines = Json::array();
                    for (const Json& c : *list) {
                        Json line = keys_in(c);
                        for (const char* end : {"a", "b"})
                            if (line.contains(end)) line[end] = point_in(line[end], dpi);
                        lines.push_back(std::move(line));
                    }
                    read["constraints"] = std::move(lines);
                }
            } else if (kind == "lens_correction") {
                if (const Json* line = get(given, "straighten"); line != nullptr && !line->is_null()) {
                    if (!line->is_array() || line->size() != 2) throw Unmapped{};
                    read["straighten"] = Json::array({point_in((*line)[0], dpi), point_in((*line)[1], dpi)});
                }
            } else if (kind == "distort_displace") {
                throw Unmapped{};  // (its map is a file of the person's machine upstream: a book holds its own)
            }
            for (const std::string& key : lengths_of(kind)) scale_at(read, key, [dpi](double v) { return to_working(v, dpi); });
        }
        Json normal = normalize_filter(kind, read, nullptr);
        return std::make_pair(kind, std::move(normal));
    } catch (const Unmapped&) {
        return std::nullopt;
    } catch (const std::exception&) {  // (settings its kind refuses: OpError, core::Error)
        return std::nullopt;
    }
}

std::optional<std::pair<std::string, Json>> to_upstream(std::string_view kind, const Json& params, double dpi, UpstreamCanvas canvas) {
    if (!(dpi > 0) || !std::isfinite(dpi)) return std::nullopt;
    const FilterInfo* info = filter_info(kind);
    const AdjustKind* table = adjust_kind(kind);
    if (info == nullptr || table == nullptr || info->upstream.empty()) return std::nullopt;
    try {
        Json read = normalize_filter(kind, params.is_object() ? params : Json::object(), nullptr);
        if (kind == "filter_gallery") return std::make_pair(std::string("filter.filterGallery"), gallery_out(read));
        if (kind == "distort_displace") return std::nullopt;  // (a file of the book's: upstream reads one of the machine's)
        for (const std::string& key : lengths_of(kind)) scale_at(read, key, [dpi](double v) { return from_working(v, dpi); });
        Json pins, lights, paths, constraints;
        if (kind == "blur_field" || kind == "blur_iris" || kind == "blur_spin") {
            pins = Json::array();
            for (const Json& pin : read.at("pins")) {
                Json one = pin;
                scale_at(one, "blur", [dpi](double v) { return from_working(v, dpi); });
                pins.push_back(keys_out(one));
            }
            read.erase("pins");
        } else if (kind == "render_lighting_effects") {
            if (const Json* texture = get(read, "texture"); texture != nullptr && !texture->is_string()) return std::nullopt;  // (a channel of the page)
            if (read.at("lights").empty()) return std::nullopt;  // (no light: upstream's empty list is its one light)
            lights = Json::array();
            for (const Json& light : read.at("lights")) {
                Json one = light;
                one["color"] = colour_out(one.at("color"));
                lights.push_back(keys_out(one));
            }
            read.erase("lights");
        } else if (kind == "blur_path" && read.contains("paths")) {
            if (!(canvas.width_mm > 0) || !(canvas.height_mm > 0)) return std::nullopt;
            if (read["paths"].empty()) return std::nullopt;  // (no path: upstream's empty list is its one path)
            paths = Json::array();
            for (const Json& path : read["paths"]) {
                Json one = path;
                for (Json& p : one["points"]) p = Json::array({p[0].get<double>() / canvas.width_mm, p[1].get<double>() / canvas.height_mm});
                scale_at(one, "speed", [dpi](double v) { return from_working(v, dpi); });
                paths.push_back(keys_out(one));
            }
            read.erase("paths");
        } else if (kind == "adaptive_wide_angle") {
            if (read.value("interpolation", std::string("bicubic")) != "bicubic") return std::nullopt;  // (upstream draws it bicubic)
            read.erase("interpolation");
            constraints = Json::array();
            for (const Json& c : read.at("constraints")) {
                Json one = c;
                for (const char* end : {"a", "b"}) one[end] = point_out(one.at(end), dpi);
                constraints.push_back(std::move(one));
            }
            read.erase("constraints");
        }
        Json out = table_out(table->params, read);
        if (!pins.is_null()) out["pins"] = std::move(pins);
        if (!lights.is_null()) out["lights"] = std::move(lights);
        if (!paths.is_null()) out["paths"] = std::move(paths);
        if (!constraints.is_null()) out["constraints"] = std::move(constraints);
        return std::make_pair(std::string(info->upstream), std::move(out));
    } catch (const Unmapped&) {
        return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool upstream_filter(std::string_view command) {
    if (std::find(kUpstreamOnly.begin(), kUpstreamOnly.end(), command) != kUpstreamOnly.end()) return true;
    if (command == "filter.filterGallery") return true;
    if (command.starts_with("filter.gallery.")) {
        const std::string_view key = command.substr(std::string_view("filter.gallery.").size());
        return std::any_of(gallery_effects().begin(), gallery_effects().end(), [key](const GalleryEffectKind& each) { return each.upstream == key; });
    }
    for (const FilterPreset& preset : filter_presets())
        if (preset.upstream == command) return true;
    return std::any_of(filter_infos().begin(), filter_infos().end(), [command](const FilterInfo& info) { return info.upstream == command; });
}

}  // namespace genko::core
