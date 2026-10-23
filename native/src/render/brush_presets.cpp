#include "render/brush_presets.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>

#include "core/base64.hpp"
#include "core/brush_dabs.hpp"
#include "core/brushes.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "render/brushes.hpp"
#include "render/dabs/lines.hpp"
#include "render/dabs/procedural.hpp"
#include "render/dabs/tile.hpp"
#include "render/image_ops.hpp"
#include "render/layer_masks.hpp"
#include "render/raster.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"

namespace genko::render::brush_presets {

namespace {

using core::Json;
using D = core::DabDynamic;
using C = core::DabControl;

// --- the built-in dab brushes (upstream's presets.rs: its sizes are pixels of the working 200 dpi) --------------------

double mm(double px) { return px * abr_map::kMmPerPixel; }

D pressure() {
    D d;
    d.control = C::Pressure;
    return d;
}
D pressure_min(double minimum) {
    D d = pressure();
    d.minimum = minimum;
    return d;
}
D jitter(double j) {
    D d;
    d.jitter = j;
    return d;
}
D controlled(C c) {
    D d;
    d.control = c;
    return d;
}
core::DabShape size_dyn(D size) {
    core::DabShape s;
    s.enabled = true;
    s.size = size;
    return s;
}
core::DabTransfer transfer(D opacity, D flow) {
    core::DabTransfer t;
    t.enabled = true;
    t.opacity = opacity;
    t.flow = flow;
    return t;
}
core::DabTexture paper(std::int64_t seed, double depth, std::int64_t size = 128) {
    core::DabTexture t;
    t.enabled = true;
    t.pattern.procedural = "paper";
    t.pattern.size = size;
    t.pattern.seed = seed;
    t.depth = depth;
    return t;
}
core::DabTexture procedural(const char* style, std::int64_t size, std::int64_t seed, double depth, core::DabMaskMode mode) {
    core::DabTexture t;
    t.enabled = true;
    t.pattern.procedural = style;
    t.pattern.size = size;
    t.pattern.seed = seed;
    t.depth = depth;
    t.mode = mode;
    return t;
}

struct Built {
    std::vector<Group> groups;
    std::map<std::string, core::Bytes> pictures;
};

Built build() {
    Built out;
    const auto tip = [&out](const dabs::GrayTile& t) {
        auto png = std::make_shared<const std::string>(dabs::png_of_tile(t));
        std::string ref = storage::AssetStore::ref(*png);
        out.pictures.emplace(ref, std::move(png));
        return ref;
    };
    core::DabSettings base;
    base.pressure_size = false;
    base.spacing = 0.25;
    std::vector<Preset>* items = nullptr;
    const auto group = [&out, &items](const char* name) {
        out.groups.push_back(Group{name, true, {}});
        items = &out.groups.back().items;
    };
    const auto add = [&items](const char* slug, const char* name, const core::DabSettings& s) {
        const Json settings{{"label", name},
                            {"width_mm", std::clamp(s.size_mm, 0.05, 50.0)},
                            {"engine", std::string(core::kDabEngine)},
                            {"dabs", core::dabs_to_json(s)}};
        items->push_back(Preset{std::string(kBuiltinPrefix) + slug, name, std::string(), true,
                                core::brush_to_dict(core::brush_from_dict(std::string(kBuiltinPrefix) + slug, settings, std::nullopt, {}))});
    };
    // Genko: the brushes every book knows
    out.groups.push_back(Group{std::string(kGenkoGroup), true, {}});
    for (const core::Brush& b : core::builtin_brushes()) {
        out.groups.back().items.push_back(Preset{b.key, b.label, std::string(kGenkoGroup), true, core::brush_to_dict(b)});
    }
    using M = core::DabMaskMode;
    {
        group("一般");
        auto s = base;
        const auto with = [&](double size, double hardness) {
            auto b = base;
            b.size_mm = mm(size);
            b.hardness = hardness;
            return b;
        };
        add("hard_round", "ハード円ブラシ", with(30, 1));
        add("soft_round", "ソフト円ブラシ", with(45, 0));
        s = with(5, 1);
        s.spacing = 0.1;
        add("hard_round_fine", "ハード円ブラシ（細）", s);
        s = with(300, 0);
        s.spacing = 0.15;
        add("soft_round_large", "ソフト円ブラシ（大）", s);
        s = with(30, 1);
        s.shape = size_dyn(pressure());
        add("hard_round_psize", "ハード円ブラシ 筆圧でサイズ", s);
        s = with(45, 0);
        s.shape = size_dyn(pressure());
        add("soft_round_psize", "ソフト円ブラシ 筆圧でサイズ", s);
        s = with(30, 1);
        s.transfer = transfer(pressure(), D{});
        add("hard_round_popacity", "ハード円ブラシ 筆圧で不透明度", s);
        s = with(45, 0);
        s.transfer = transfer(pressure(), D{});
        add("soft_round_popacity", "ソフト円ブラシ 筆圧で不透明度", s);
        s = with(30, 0.9);
        s.transfer = transfer(pressure(), pressure_min(0.2));
        add("hard_round_popacity_flow", "ハード円ブラシ 筆圧で不透明度と流量", s);
        s = with(80, 0);
        s.flow = 0.1;
        s.spacing = 0.1;
        s.build_up = true;
        s.build_up_rate = 25;
        add("airbrush_soft", "エアブラシ（ソフト）", s);
        s = with(120, 0);
        s.flow = 0.15;
        s.spacing = 0.1;
        s.build_up = true;
        s.build_up_rate = 30;
        s.transfer = transfer(D{}, pressure());
        add("airbrush_pflow", "エアブラシ 筆圧で流量", s);
        s = with(28, 1);
        s.roundness = 0.2;
        s.angle = 45;
        s.spacing = 0.05;
        add("calligraphy_flat", "カリグラフィ（平筆）", s);
    }
    {
        group("ドライメディア");
        auto s = base;
        s.size_mm = mm(3);
        s.hardness = 1;
        s.aliased = true;
        s.spacing = 0.1;
        add("hard_pencil", "鉛筆（ハード）", s);
        s = base;
        s.size_mm = mm(6);
        s.hardness = 0.6;
        s.spacing = 0.08;
        s.shape = size_dyn(pressure_min(0.4));
        s.transfer = transfer(pressure_min(0.15), D{});
        s.texture = paper(21, 0.8);
        s.texture.scale = 0.5;
        add("soft_pencil", "鉛筆（ソフト）", s);
        s = base;
        s.size_mm = mm(40);
        s.tip = tip(dabs::chalk_tip(64, 7));
        s.spacing = 0.2;
        s.shape.enabled = true;
        s.shape.angle = jitter(1.0);
        s.shape.size = jitter(0.15);
        s.texture = paper(3, 0.6);
        add("chalk", "チョーク", s);
        s = base;
        s.size_mm = mm(45);
        s.tip = tip(dabs::charcoal_tip(64, 4));
        s.spacing = 0.12;
        s.shape.enabled = true;
        s.shape.size = pressure_min(0.5);
        s.shape.angle = controlled(C::Direction);
        s.transfer = transfer(pressure_min(0.3), D{});
        s.texture = paper(5, 0.7);
        s.texture.mode = M::Subtract;
        add("charcoal", "木炭", s);
        s = base;
        s.size_mm = mm(50);
        s.tip = tip(dabs::chalk_tip(64, 31));
        s.spacing = 0.15;
        s.shape.enabled = true;
        s.shape.angle = jitter(1.0);
        s.shape.roundness = jitter(0.3);
        s.shape.roundness.minimum = 0.5;
        s.scatter.enabled = true;
        s.scatter.scatter = jitter(0.25);
        s.texture = procedural("canvas", 64, 2, 0.55, M::Subtract);
        add("pastel", "パステル", s);
        s = base;
        s.size_mm = mm(22);
        s.hardness = 0.85;
        s.roundness = 0.7;
        s.spacing = 0.08;
        s.shape.enabled = true;
        s.shape.angle = controlled(C::Direction);
        s.texture = paper(17, 0.9);
        s.texture.each_tip = true;
        s.texture.mode = M::HardMix;
        s.texture.depth_jitter = jitter(0.3);
        add("wax_crayon", "クレヨン", s);
        s = base;
        s.size_mm = mm(50);
        s.tip = tip(dabs::bristle_tip(64, 5));
        s.spacing = 0.04;
        s.shape.enabled = true;
        s.shape.angle = controlled(C::Direction);
        s.transfer.enabled = true;
        s.transfer.flow.control = C::Fade;
        s.transfer.flow.fade_steps = 400;
        s.transfer.flow.minimum = 0.1;
        add("dry_bristle", "乾いた剛毛", s);
        s = base;
        s.size_mm = mm(60);
        s.tip = tip(dabs::rake_tip(64, 3, 7));
        s.spacing = 0.03;
        s.shape.enabled = true;
        s.shape.angle = controlled(C::Direction);
        s.transfer = transfer(pressure_min(0.25), D{});
        add("rake", "レーキ", s);
    }
    {
        group("ウェットメディア");
        auto s = base;
        s.size_mm = mm(60);
        s.hardness = 0.3;
        s.wet_edges = true;
        s.texture = paper(9, 0.35, 256);
        s.texture.scale = 1.5;
        add("watercolor_wet_edges", "水彩（にじみの縁）", s);
        s = base;
        s.size_mm = mm(140);
        s.hardness = 0;
        s.flow = 0.35;
        s.spacing = 0.12;
        s.wet_edges = true;
        s.shape = size_dyn(pressure_min(0.35));
        {
            D f = pressure();
            f.jitter = 0.2;
            s.transfer = transfer(D{}, f);
        }
        s.texture = paper(13, 0.25, 256);
        s.texture.scale = 2.0;
        add("watercolor_wash", "水彩（ウォッシュ）", s);
        s = base;
        s.size_mm = mm(8);
        s.hardness = 1;
        s.spacing = 0.05;
        s.shape = size_dyn(pressure_min(0.15));
        add("ink_pen", "インクペン", s);
        s = base;
        s.size_mm = mm(70);
        s.tip = tip(dabs::bristle_tip(64, 41));
        s.spacing = 0.03;
        s.shape.enabled = true;
        s.shape.angle = controlled(C::Direction);
        s.shape.size = pressure_min(0.6);
        s.color.enabled = true;
        s.color.per_tip = false;
        s.color.fg_bg = jitter(0.15);
        s.color.brightness_jitter = 0.05;
        s.texture = procedural("canvas", 64, 4, 0.3, M::Multiply);
        add("oil_bristle", "油彩（剛毛）", s);
        s = base;
        s.size_mm = mm(40);
        s.hardness = 0.9;
        s.roundness = 0.35;
        s.spacing = 0.05;
        s.shape.enabled = true;
        s.shape.angle = controlled(C::Direction);
        {
            D f;
            f.control = C::Fade;
            f.fade_steps = 600;
            f.minimum = 0.35;
            s.transfer = transfer(D{}, f);
        }
        add("gouache_flat", "ガッシュ（平筆）", s);
        s = base;
        s.size_mm = mm(70);
        s.tip = tip(dabs::sponge_tip(96, 8));
        s.spacing = 0.3;
        s.flow = 0.6;
        s.wet_edges = true;
        s.shape.enabled = true;
        s.shape.angle = jitter(1.0);
        s.shape.size = jitter(0.2);
        add("ink_wash_sponge", "墨のスポンジ", s);
    }
    {
        group("特殊効果");
        auto s = base;
        s.size_mm = mm(60);
        s.tip = tip(dabs::spatter_tip(64, 11, 14));
        s.spacing = 0.6;
        s.shape.enabled = true;
        s.shape.size = jitter(0.6);
        s.shape.size.minimum = 0.2;
        s.shape.angle = jitter(1.0);
        s.shape.flip_x_jitter = true;
        s.shape.flip_y_jitter = true;
        s.scatter.enabled = true;
        s.scatter.scatter = jitter(1.5);
        s.scatter.both_axes = true;
        s.scatter.count = 2;
        s.scatter.count_jitter = jitter(0.5);
        add("spatter", "スパッタリング", s);
        s = base;
        s.size_mm = mm(40);
        s.tip = tip(dabs::spatter_tip(64, 77, 40));
        s.spacing = 0.5;
        s.shape.enabled = true;
        s.shape.angle = jitter(1.0);
        s.shape.flip_x_jitter = true;
        s.scatter.enabled = true;
        s.scatter.scatter = jitter(0.6);
        s.scatter.both_axes = true;
        s.transfer = transfer(pressure(), D{});
        add("stipple", "点描", s);
        s = base;
        s.size_mm = mm(24);
        s.hardness = 1;
        s.roundness = 0.5;
        s.spacing = 1.2;
        s.shape.enabled = true;
        s.shape.angle = jitter(1.0);
        s.shape.size = jitter(0.5);
        s.scatter.enabled = true;
        s.scatter.scatter = jitter(3.0);
        s.scatter.both_axes = true;
        s.scatter.count = 3;
        s.color.enabled = true;
        s.color.per_tip = true;
        s.color.hue_jitter = 1.0;
        s.color.saturation_jitter = 0.2;
        s.color.purity = 0.5;
        add("confetti", "紙吹雪", s);
        s = base;
        s.size_mm = mm(50);
        s.tip = tip(dabs::leaf_tip(64));
        s.spacing = 0.35;
        s.shape.enabled = true;
        s.shape.size = jitter(0.5);
        s.shape.size.minimum = 0.3;
        s.shape.angle = jitter(1.0);
        s.shape.roundness = jitter(0.4);
        s.shape.roundness.minimum = 0.4;
        s.shape.flip_y_jitter = true;
        s.scatter.enabled = true;
        s.scatter.scatter = jitter(1.2);
        s.scatter.both_axes = true;
        s.scatter.count = 2;
        s.scatter.count_jitter = jitter(0.5);
        s.color.enabled = true;
        s.color.per_tip = true;
        s.color.fg_bg = jitter(0.6);
        s.color.hue_jitter = 0.04;
        s.color.brightness_jitter = 0.12;
        add("foliage", "葉", s);
        s = base;
        s.size_mm = mm(70);
        s.tip = tip(dabs::grass_tip(96, 6, 11));
        s.spacing = 0.25;
        s.shape.enabled = true;
        s.shape.size = pressure();
        s.shape.size.jitter = 0.4;
        s.shape.size.minimum = 0.4;
        s.shape.angle = jitter(0.06);
        s.shape.flip_x_jitter = true;
        s.scatter.enabled = true;
        s.scatter.scatter = jitter(0.3);
        s.color.enabled = true;
        s.color.per_tip = true;
        s.color.fg_bg = jitter(0.7);
        s.color.hue_jitter = 0.03;
        add("grass", "草", s);
        s = base;
        s.size_mm = mm(40);
        s.tip = tip(dabs::star_tip(64));
        s.spacing = 1.5;
        s.shape.enabled = true;
        s.shape.size = jitter(0.8);
        s.shape.size.minimum = 0.15;
        s.scatter.enabled = true;
        s.scatter.scatter = jitter(2.5);
        s.scatter.both_axes = true;
        s.transfer = transfer(jitter(0.5), D{});
        add("sparkle", "きらめき", s);
        s = base;
        s.size_mm = mm(60);
        s.tip = tip(dabs::sponge_tip(64, 19));
        s.spacing = 0.45;
        s.shape.enabled = true;
        s.shape.angle = jitter(1.0);
        s.transfer = transfer(jitter(0.3), D{});
        add("sponge_texture", "スポンジの質感", s);
        s = base;
        s.size_mm = mm(50);
        s.hardness = 0.8;
        s.texture = procedural("canvas", 64, 1, 0.7, M::Subtract);
        add("canvas_texture", "キャンバスの質感", s);
        s = base;
        s.size_mm = mm(80);
        s.hardness = 0.5;
        s.texture = procedural("dots", 64, 1, 1.0, M::HardMix);
        add("halftone_dots", "網点", s);
        s = base;
        s.size_mm = mm(50);
        s.hardness = 0.6;
        s.noise = true;
        s.dual.enabled = true;
        s.dual.tip = tip(dabs::spatter_tip(48, 23, 20));
        s.dual.size_mm = mm(30);
        s.dual.spacing = 0.3;
        s.dual.scatter = 1.0;
        s.dual.both_axes = true;
        s.dual.count = 2;
        s.dual.flip = true;
        add("dual_grain", "デュアルの粒", s);
    }
    for (Group& g : out.groups) {
        for (Preset& p : g.items) p.group = g.name;
    }
    return out;
}

const Built& built() {
    static const Built b = [] {
        Built made = build();
        dabs::keep_pictures(made.pictures);  // (the engine draws them without the library)
        return made;
    }();
    return b;
}

// --- the person's brushes ------------------------------------------------------------------------------------------

std::string clean(const std::string& text, std::size_t most) {
    std::string out = text;
    while (!out.empty() && (out.front() == ' ' || out.front() == '\t' || out.front() == '\n')) out.erase(out.begin());
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' || out.back() == '\n')) out.pop_back();
    // (at most `most` characters: whole UTF-8 characters)
    std::size_t at = 0, count = 0;
    while (at < out.size() && count < most) {
        const auto lead = static_cast<unsigned char>(out[at]);
        at += lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        ++count;
    }
    return out.substr(0, std::min(at, out.size()));
}

std::size_t characters(const std::string& text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xc0) != 0x80) ++n;
    }
    return n;
}

// A brush's name no other brush has: `base`, else "base 2", "base 3"… (40 characters at most: the base cut to fit).
std::string unique_name(const std::set<std::string>& taken, const std::string& base_in) {
    const std::string base = clean(base_in, 40).empty() ? std::string("ブラシ") : clean(base_in, 40);
    if (!taken.contains(base)) return base;
    for (int n = 2;; ++n) {
        const std::string suffix = " " + std::to_string(n);
        const std::string name = clean(base, 40 - suffix.size()) + suffix;
        if (!taken.contains(name)) return name;
    }
}

std::string group_name(const std::string& given) {
    const std::string g = clean(given, 200);
    if (g.empty()) throw core::Error("value", "a group's name is 1 to 200 characters");
    for (const Group& b : built().groups) {
        if (b.name == g) throw core::Error("value", "the group " + g + " is the built-in brushes'; give one's own brushes another group");
    }
    return g;
}

// The library file as it is read (a brush that is not an object left out), and why it cannot be read (else "").
struct Own {
    Json brushes = Json::object();  // (key → entry, the file's order)
    std::vector<std::string> order;
    std::string problem;
};
Own read_own(const std::filesystem::path& config_dir) {
    Own out;
    const std::filesystem::path path = brushes::library_path(config_dir);
    std::error_code missing;
    if (!std::filesystem::exists(path, missing)) return out;
    try {
        const std::string text = storage::read_file_bounded(path, std::size_t(256) << 20);
        if (core::utf8_error(text)) throw core::Error("format", "it is not UTF-8");
        const Json data = core::parse_python_json(text);
        if (!data.is_object()) throw core::Error("format", "it is not an object");
        const auto found = data.find("brushes");
        if (found != data.end() && !found->is_null() && !found->is_object()) throw core::Error("format", "its brushes are not an object");
    } catch (const core::Error& error) {
        out.problem = "the brush library " + core::path_to_utf8(path) + " cannot be read (" + error.what() + "): it is left as it is";
        return out;
    }
    for (const auto& [key, entry] : brushes::load_library(config_dir).items()) {
        if (entry.is_object()) out.brushes[key] = entry;
    }
    out.order = brushes::library_groups(config_dir);
    return out;
}

std::string group_of(const Json& entry) {
    const auto g = entry.find("group");
    if (g != entry.end() && g->is_string() && !g->get<std::string>().empty()) return g->get<std::string>();
    return std::string(kOwnGroup);
}

// The groups of the person's brushes in order: the file's "groups", then each brush's in the order they come.
std::vector<std::string> groups_of(const Own& own) {
    std::vector<std::string> out = own.order;
    for (const auto& [key, entry] : own.brushes.items()) {
        const std::string g = group_of(entry);
        if (std::find(out.begin(), out.end(), g) == out.end()) out.push_back(g);
    }
    return out;
}

std::set<std::string> names_taken(const Own& own, std::string_view except = {}) {
    std::set<std::string> out;
    for (const Group& g : built().groups) {
        for (const Preset& p : g.items) out.insert(p.name);
    }
    for (const auto& [key, entry] : own.brushes.items()) {
        if (key == except) continue;
        const auto label = entry.find("label");
        if (label != entry.end() && label->is_string()) out.insert(label->get<std::string>());
    }
    return out;
}

void refuse(const std::string& problem) {
    if (!problem.empty()) throw core::Error("io", problem);
}

// The key of the person's brush a key or a name names (its key first). core::Error("value") for a built-in one,
// ("key") for one not there.
std::string own_key(const Own& own, const std::string& key_or_name) {
    if (builtin(key_or_name) != nullptr || builtin_dab_of(key_or_name) != nullptr) {
        throw core::Error("value", "the built-in brush " + key_or_name + " cannot be changed (make one's own from it)");
    }
    if (own.brushes.contains(key_or_name)) return key_or_name;
    for (const auto& [key, entry] : own.brushes.items()) {
        const auto label = entry.find("label");
        if (label != entry.end() && label->is_string() && label->get<std::string>() == key_or_name) return key;
    }
    for (const Group& g : built().groups) {
        for (const Preset& p : g.items) {
            if (p.name == key_or_name) throw core::Error("value", "the built-in brush " + key_or_name + " cannot be changed (make one's own from it)");
        }
    }
    throw core::Error("key", "no brush of one's own named " + key_or_name);
}

std::vector<std::string> names_in(const Json& preset) {
    std::vector<std::string> out;
    if (preset.is_string()) {
        out.push_back(preset.get<std::string>());
    } else if (preset.is_array()) {
        for (const Json& p : preset) {
            if (!p.is_string()) throw core::Error("value", "preset names a brush by its key or name (text)");
            out.push_back(p.get<std::string>());
        }
    } else {
        throw core::Error("value", "preset names a brush by its key or name, or a list of them");
    }
    if (out.empty()) throw core::Error("value", "preset names no brush");
    return out;
}

// The settings of a brush given to the library as define_brush takes them, its pictures taken in.
Json taken_in(const std::string& key, const Json& given, std::map<std::string, core::Bytes>& pictures) {
    if (!given.is_object()) throw core::Error("value", "a brush's settings are an object");
    Json data = given;
    for (const char* k : {"op", "key", "delete", "group"}) data.erase(k);
    const Json format = data.contains("format") ? data["format"] : Json(nullptr);
    data.erase("format");
    if (!format.is_null() && format != Json("photocraft") && format != Json("genko")) {
        throw core::Error("value", "format must be photocraft (upstream's brush settings) or genko");
    }
    if (format == Json("photocraft") || data.contains("dabs")) {
        if (!data.contains("engine")) data["engine"] = std::string(core::kDabEngine);
    }
    if (core::is_dab_brush(data)) {
        Json dabs = data.contains("dabs") && !data["dabs"].is_null() ? data["dabs"] : Json::object();
        if (format == Json("photocraft")) {
            std::vector<std::string> ignored;
            dabs = dabs::dabs_from_photocraft(dabs, pictures, ignored);
        } else if (dabs.is_object()) {
            const auto take = [&](Json& holder, const char* k, bool ink, std::uint32_t edge) {
                if (!holder.is_object() || !holder.contains(k)) return;
                Json& v = holder[k];
                if (!v.is_object() || !v.contains("png")) return;
                if (!v["png"].is_string()) throw core::Error("value", std::string("the ") + k + "'s png is the picture file in base64");
                std::string file;
                try {
                    file = core::a2b_base64(v["png"].get_ref<const std::string&>());
                } catch (const core::Error&) {
                    throw core::Error("value", std::string("the ") + k + "'s picture cannot be read (png must be the picture file in base64)");
                }
                const auto made = dabs::take_in_picture(file, ink, edge);
                pictures[made.first] = made.second;
                v = Json{{"asset", made.first}};
            };
            take(dabs, "tip", true, core::kMaxTipEdge);
            if (dabs.contains("dual")) take(dabs["dual"], "tip", true, core::kMaxTipEdge);
            if (dabs.contains("texture")) take(dabs["texture"], "pattern", false, core::kMaxPatternEdge);
        }
        data["dabs"] = dabs;
    }
    data = brushes::with_paper_taken_in(data);
    core::Brush made;
    try {
        made = core::brush_from_dict(key, data, std::nullopt, {});
    } catch (const core::Error& error) {
        throw core::Error("value", error.what());
    }
    return core::brush_to_dict(made);
}

std::string tip_kind(const Json& settings) {
    if (core::is_dab_brush(settings)) {
        const Json& dabs = settings["dabs"];
        return dabs.is_object() && dabs.contains("tip") && dabs["tip"].is_object() ? "sampled" : "round";
    }
    const auto t = settings.find("tip");
    return t != settings.end() && t->is_string() ? t->get<std::string>() : std::string("round");
}

Json item_json(const Preset& p, bool full) {
    Json out{{"key", p.key}, {"name", p.name}, {"builtin", p.builtin}};
    const bool dab = core::is_dab_brush(p.settings);
    out["engine"] = dab ? Json(std::string(core::kDabEngine)) : Json(nullptr);
    const auto width = p.settings.find("width_mm");
    out["size_mm"] = width != p.settings.end() ? *width : Json(nullptr);
    if (dab) {
        try {
            const core::DabSettings d = core::dabs_from_json(p.settings["dabs"].is_null() ? Json::object() : p.settings["dabs"]);
            out["size_mm"] = d.size_mm;
            out["hardness"] = d.hardness;
        } catch (const core::Error&) {
        }
    }
    out["tip"] = tip_kind(p.settings);
    if (full) out["settings"] = p.settings;
    return out;
}

}  // namespace

const std::vector<Group>& builtin_groups() { return built().groups; }

const Preset* builtin(std::string_view key) {
    for (const Group& g : built().groups) {
        for (const Preset& p : g.items) {
            if (p.key == key) return &p;
        }
    }
    return nullptr;
}

const Preset* builtin_dab_of(std::string_view key) {
    if (!key.starts_with(kBuiltinPrefix)) return nullptr;
    if (const Preset* p = builtin(key); p != nullptr) return p;
    // (my_genko_<name>_<n>: the key a book took when it had another brush of the first)
    const std::size_t under = key.rfind('_');
    if (under == std::string_view::npos || under + 1 >= key.size()) return nullptr;
    for (std::size_t i = under + 1; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') return nullptr;
    }
    const Preset* p = builtin(key.substr(0, under));
    return p != nullptr && p->key.starts_with(kBuiltinPrefix) ? p : nullptr;
}

bool is_own(std::string_view key) { return key.starts_with("my_") && !key.starts_with(kBuiltinPrefix); }

bool same_settings(const Json& a, const Json& b) {
    try {
        return core::brush_to_dict(core::brush_from_dict("my_a", a, std::nullopt, {})) == core::brush_to_dict(core::brush_from_dict("my_a", b, std::nullopt, {}));
    } catch (const core::Error&) {
        return false;
    }
}

Library load(const std::filesystem::path& config_dir) {
    Library out;
    out.groups = built().groups;
    const Own own = read_own(config_dir);
    out.problem = own.problem;
    for (const std::string& name : groups_of(own)) {
        Group g{name, false, {}};
        for (const auto& [key, entry] : own.brushes.items()) {
            if (group_of(entry) != name) continue;
            Json settings = entry;
            settings.erase("group");
            const auto label = entry.find("label");
            g.items.push_back(Preset{key, label != entry.end() && label->is_string() ? label->get<std::string>() : key, name, false, settings});
        }
        out.groups.push_back(std::move(g));
    }
    return out;
}

const Preset* find(const Library& library, std::string_view key_or_name) {
    for (const Group& g : library.groups) {
        for (const Preset& p : g.items) {
            if (p.key == key_or_name) return &p;
        }
    }
    for (const Group& g : library.groups) {
        for (const Preset& p : g.items) {
            if (p.name == key_or_name) return &p;
        }
    }
    return nullptr;
}

Json list(const Library& library, bool full) {
    Json groups = Json::array();
    for (const Group& g : library.groups) {
        Json items = Json::array();
        for (const Preset& p : g.items) items.push_back(item_json(p, full));
        groups.push_back(Json{{"name", g.name}, {"builtin", g.builtin}, {"brushes", std::move(items)}});
    }
    Json out{{"groups", std::move(groups)}};
    if (!library.problem.empty()) out["problem"] = library.problem;
    return out;
}

Json save(const std::filesystem::path& config_dir, std::optional<std::string> key, const Json& settings, std::optional<std::string> name,
          std::optional<std::string> group, const std::map<std::string, core::Bytes>& given_pictures) {
    const Own own = read_own(config_dir);
    refuse(own.problem);
    if (key) {
        if (builtin(*key) != nullptr || key->starts_with(kBuiltinPrefix)) {
            throw core::Error("value", "the built-in brush " + *key + " cannot be changed (make one's own from it)");
        }
        if (!key->starts_with("my_") || key->size() > 40) throw core::Error("value", "a brush of one's own has a key starting with my_ (at most 40 characters)");
    } else {
        key = "my_" + core::new_id();
    }
    const bool there = own.brushes.contains(*key);
    if (!there && own.brushes.size() >= kMaxOwn) throw core::Error("value", "the library holds " + std::to_string(kMaxOwn) + " brushes of one's own at most");
    std::map<std::string, core::Bytes> pictures = given_pictures;
    Json entry = taken_in(*key, settings, pictures);
    const std::string wanted = name ? *name : entry.value("label", std::string());
    const std::string label = unique_name(names_taken(own, *key), wanted);
    entry["label"] = label;
    const std::string kept_group = group ? group_name(*group) : there ? group_of(own.brushes[*key]) : std::string(kOwnGroup);
    if (kept_group != kOwnGroup) entry["group"] = kept_group;
    std::vector<std::string> order = groups_of(own);
    if (std::find(order.begin(), order.end(), kept_group) == order.end()) order.push_back(kept_group);
    // (only an order to keep is written: a library of 自分のブラシ alone keeps none)
    const bool ordered = !(order.size() == 1 && order.front() == kOwnGroup) || !own.order.empty();
    brushes::save_many(config_dir, {{*key, entry}}, ordered ? std::optional<std::vector<std::string>>(order) : std::nullopt, pictures);
    for (const auto& [ref, png] : pictures) dabs::keep_picture(ref, png);  // (drawn in this process at once)
    return Json{{"key", *key}, {"name", label}, {"group", kept_group}};
}

Json remove(const std::filesystem::path& config_dir, const std::vector<std::string>& keys_or_names) {
    const Own own = read_own(config_dir);
    refuse(own.problem);
    std::vector<std::pair<std::string, std::optional<Json>>> changes;
    Json deleted = Json::array();
    for (const std::string& k : keys_or_names) {
        const std::string key = own_key(own, k);
        changes.emplace_back(key, std::nullopt);
        deleted.push_back(key);
    }
    brushes::save_many(config_dir, changes);
    return Json{{"deleted", std::move(deleted)}};
}

Json edit(const std::filesystem::path& config_dir, const Json& params) {
    if (!params.is_object()) throw core::Error("value", "an edit is an object {\"action\", …}");
    const std::string action = params.value("action", std::string());
    Own own = read_own(config_dir);
    refuse(own.problem);
    std::vector<std::string> order = groups_of(own);
    const auto text_of = [&](const char* k) {
        const auto v = params.find(k);
        if (v == params.end() || !v->is_string()) throw core::Error("value", std::string(action) + " needs " + k + " (text)");
        return v->get<std::string>();
    };
    const auto known_group = [&](const std::string& g) {
        for (const Group& b : built().groups) {
            if (b.name == g) throw core::Error("value", "the group " + g + " is the built-in brushes' and cannot be changed");
        }
        if (std::find(order.begin(), order.end(), g) == order.end()) throw core::Error("value", "no group named " + g);
    };
    // the whole library written again in a new order (each brush as the file has it: its paper stays beside)
    const auto write_all = [&](const Json& brushes_now) {
        std::vector<std::pair<std::string, std::optional<Json>>> changes;
        for (const auto& [key, entry] : own.brushes.items()) changes.emplace_back(key, std::nullopt);
        for (const auto& [key, entry] : brushes_now.items()) changes.emplace_back(key, entry);
        brushes::save_many(config_dir, changes, order);
    };
    if (action == "rename") {
        const std::vector<std::string> named = names_in(params.contains("preset") ? params["preset"] : Json());
        if (named.size() != 1) throw core::Error("value", "rename names one brush");
        const std::string key = own_key(own, named.front());
        const std::string wanted = clean(text_of("name"), 40);
        if (wanted.empty()) throw core::Error("value", "a brush's name is 1 to 40 characters");
        if (names_taken(own, key).contains(wanted)) throw core::Error("value", "a brush named " + wanted + " is there already");
        Json entry = own.brushes[key];
        entry["label"] = wanted;
        brushes::save_many(config_dir, {{key, entry}}, order);
        return Json{{"name", wanted}, {"key", key}};
    }
    if (action == "delete") {
        Json deleted = Json::array();
        std::vector<std::pair<std::string, std::optional<Json>>> changes;
        for (const std::string& n : names_in(params.contains("preset") ? params["preset"] : Json())) {
            const std::string key = own_key(own, n);
            changes.emplace_back(key, std::nullopt);
            deleted.push_back(key);
        }
        brushes::save_many(config_dir, changes, order);
        return Json{{"deleted", std::move(deleted)}};
    }
    if (action == "move") {
        std::vector<std::string> keys;
        for (const std::string& n : names_in(params.contains("preset") ? params["preset"] : Json())) keys.push_back(own_key(own, n));
        // (the group moved to: "to", as the other kinds' edits name it — or "group")
        const std::string to = params.contains("to")      ? group_name(text_of("to"))
                               : params.contains("group") ? group_name(text_of("group"))
                                                          : group_of(own.brushes[keys.front()]);
        if (std::find(order.begin(), order.end(), to) == order.end()) order.push_back(to);
        // (the group's brushes in their order; the moved ones taken out and put at `index` of what is left, else last)
        std::vector<std::string> in_group;
        for (const auto& [key, entry] : own.brushes.items()) {
            if (group_of(entry) == to && std::find(keys.begin(), keys.end(), key) == keys.end()) in_group.push_back(key);
        }
        std::size_t at = in_group.size();
        if (const auto index = params.find("index"); index != params.end() && !index->is_null()) {
            if (!index->is_number_integer() || index->get<std::int64_t>() < 0) throw core::Error("value", "index is a whole number 0 or more");
            at = std::min<std::size_t>(static_cast<std::size_t>(index->get<std::int64_t>()), in_group.size());
        }
        in_group.insert(in_group.begin() + static_cast<std::ptrdiff_t>(at), keys.begin(), keys.end());
        Json now = Json::object();
        bool placed = false;
        for (const auto& [key, entry] : own.brushes.items()) {
            if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
            if (group_of(entry) == to) {
                if (!placed) {
                    for (const std::string& k : in_group) {
                        Json e = own.brushes[k];
                        if (to == kOwnGroup) e.erase("group"); else e["group"] = to;
                        now[k] = e;
                    }
                    placed = true;
                }
                continue;
            }
            now[key] = entry;
        }
        if (!placed) {
            for (const std::string& k : in_group) {
                Json e = own.brushes[k];
                if (to == kOwnGroup) e.erase("group"); else e["group"] = to;
                now[k] = e;
            }
        }
        write_all(now);
        Json moved = Json::array();
        for (const std::string& k : keys) moved.push_back(k);
        return Json{{"moved", std::move(moved)}, {"group", to}};
    }
    if (action == "new_group") {
        const std::string g = group_name(params.contains("name") ? text_of("name") : text_of("group"));
        if (std::find(order.begin(), order.end(), g) != order.end()) throw core::Error("value", "a group named " + g + " is there already");
        order.push_back(g);
        brushes::save_many(config_dir, {}, order);
        return Json{{"group", g}};
    }
    if (action == "rename_group") {
        const std::string from = text_of("group");
        known_group(from);
        const std::string to = group_name(text_of("name"));
        if (to != from && std::find(order.begin(), order.end(), to) != order.end()) throw core::Error("value", "a group named " + to + " is there already");
        std::replace(order.begin(), order.end(), from, to);
        std::vector<std::pair<std::string, std::optional<Json>>> changes;
        for (const auto& [key, entry] : own.brushes.items()) {
            if (group_of(entry) != from) continue;
            Json e = entry;
            if (to == kOwnGroup) e.erase("group"); else e["group"] = to;
            changes.emplace_back(key, e);
        }
        brushes::save_many(config_dir, changes, order);
        return Json{{"group", to}};
    }
    if (action == "delete_group") {
        const std::string g = text_of("group");
        known_group(g);
        std::erase(order, g);
        Json deleted = Json::array();
        std::vector<std::pair<std::string, std::optional<Json>>> changes;
        for (const auto& [key, entry] : own.brushes.items()) {
            if (group_of(entry) != g) continue;
            changes.emplace_back(key, std::nullopt);
            deleted.push_back(key);
        }
        brushes::save_many(config_dir, changes, order);
        return Json{{"deleted", std::move(deleted)}, {"group", g}};
    }
    throw core::Error("value", "action must be rename, delete, move, new_group, rename_group or delete_group");
}

Json import_mapped(const std::filesystem::path& config_dir, const abr_map::Import& mapped, const std::string& group_given, bool replace) {
    const Own own = read_own(config_dir);
    refuse(own.problem);
    const std::string group = group_name(group_given.empty() ? std::string("読み込んだブラシ") : group_given);
    std::vector<std::pair<std::string, std::optional<Json>>> changes;
    std::set<std::string> forgotten;
    if (replace) {
        for (const auto& [key, entry] : own.brushes.items()) {
            if (group_of(entry) == group) {
                changes.emplace_back(key, std::nullopt);
                forgotten.insert(key);
            }
        }
    }
    if (own.brushes.size() - forgotten.size() + mapped.presets.size() > kMaxOwn) {
        throw core::Error("value", "the library holds " + std::to_string(kMaxOwn) + " brushes of one's own at most");
    }
    std::set<std::string> taken;
    for (const Group& g : built().groups) {
        for (const Preset& p : g.items) taken.insert(p.name);
    }
    for (const auto& [key, entry] : own.brushes.items()) {
        const auto label = entry.find("label");
        if (!forgotten.contains(key) && label != entry.end() && label->is_string()) taken.insert(label->get<std::string>());
    }
    Json made = Json::array();
    for (const abr_map::Preset& p : mapped.presets) {
        const std::string key = "my_" + core::new_id();
        Json entry = p.settings;
        const std::string name = unique_name(taken, p.name);
        taken.insert(name);
        entry["label"] = name;
        if (group != kOwnGroup) entry["group"] = group;
        changes.emplace_back(key, entry);
        made.push_back(Json{{"key", key}, {"name", name}});
    }
    std::vector<std::string> order = groups_of(own);
    if (std::find(order.begin(), order.end(), group) == order.end()) order.push_back(group);
    brushes::save_many(config_dir, changes, order, mapped.pictures);
    return Json{{"group", group}, {"brushes", std::move(made)}, {"warnings", mapped.warnings}, {"version", mapped.version}};
}

Json import_abr(const std::filesystem::path& config_dir, std::string_view bytes, const std::string& group, bool replace) {
    return import_mapped(config_dir, abr_map::read(bytes), group, replace);
}

Defined define_from(const core::Document& doc, const core::Page& page, const Json* area, const std::string* layer_id, const std::string& name) {
    // (the page's working grid: an opened picture's, a precise or ink page's finest, else 200 dpi — as パターンを定義…)
    const int page_dpi = (page.color || (page.rgb && page.rgb->depth > 8)) ? layer_masks::grid_dpi(page) : raster::working_dpi(page);
    std::optional<image_ops::Pixels> held;
    if (layer_id != nullptr) {
        const core::Layer* layer = nullptr;
        for (const core::Layer& l : page.layers) {
            if (l.id == *layer_id) layer = &l;
        }
        if (layer == nullptr) throw core::Error("value", "no layer " + *layer_id + " on the page");
        held.emplace(image_ops::shown_layer_pixels(doc, page, *layer));
    } else {
        held.emplace(image_ops::page_pixels(doc, page, page_dpi));
    }
    const image_ops::Pixels& p = *held;
    const std::uint32_t w = p.width(), h = p.height();
    if (w == 0 || h == 0) throw core::Error("value", "nothing there holds paint (all white or transparent)");
    const double dpi = static_cast<double>(w) / (page.spec.width_mm.value() / 25.4);
    std::vector<float> cover;
    if (area != nullptr) cover = image_ops::coverage(page, *area, w, h, p.kind() == image_ops::Pixels::Kind::Eight);
    // the box the area covers, then its paint (darkness × alpha × the area)
    std::uint32_t bx0 = w, by0 = h, bx1 = 0, by1 = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            if (!cover.empty() && !(cover[std::size_t(y) * w + x] > 0.0f)) continue;
            bx0 = std::min(bx0, x);
            by0 = std::min(by0, y);
            bx1 = std::max(bx1, x + 1);
            by1 = std::max(by1, y + 1);
        }
    }
    if (bx1 <= bx0 || by1 <= by0) throw core::Error("value", "the area covers none of the page");
    std::vector<image_ops::Rgba> meant;
    if (p.kind() == image_ops::Pixels::Kind::Inks) meant = image_ops::rgba_of(p);  // (the colours an ink layer's inks mean)
    const std::uint32_t bw = bx1 - bx0, bh = by1 - by0;
    std::vector<float> paint(std::size_t(bw) * bh, 0.0f);
    std::uint32_t x0 = bw, y0 = bh, x1 = 0, y1 = 0;
    for (std::uint32_t y = 0; y < bh; ++y) {
        for (std::uint32_t x = 0; x < bw; ++x) {
            const std::size_t i = std::size_t(by0 + y) * w + (bx0 + x);
            float r = 0, g = 0, b = 0, a = 0;
            if (!meant.empty()) {
                r = meant[i][0];
                g = meant[i][1];
                b = meant[i][2];
                a = meant[i][3];
            } else {
                const float* v = p.at(i);
                if (p.colours() >= 3) {
                    r = v[0];
                    g = v[1];
                    b = v[2];
                } else {
                    r = g = b = v[0];
                }
                a = v[p.colours()];
            }
            const float lum = std::clamp(0.299f * r + 0.587f * g + 0.114f * b, 0.0f, 1.0f);
            const float k = cover.empty() ? 1.0f : std::clamp(cover[i], 0.0f, 1.0f);
            const float v = std::clamp((1.0f - lum) * std::clamp(a, 0.0f, 1.0f) * k, 0.0f, 1.0f);
            paint[std::size_t(y) * bw + x] = v;
            if (v > 1e-4f) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x + 1);
                y1 = std::max(y1, y + 1);
            }
        }
    }
    if (x1 <= x0 || y1 <= y0) throw core::Error("value", "nothing there holds paint (all white or transparent)");
    const std::uint32_t tw = x1 - x0, th = y1 - y0;
    if (tw > core::kMaxTipEdge || th > core::kMaxTipEdge) {
        throw core::Error("value", "the area is too large for a brush tip (" + std::to_string(tw) + " × " + std::to_string(th) + " pixels; at most " +
                                       std::to_string(core::kMaxTipEdge) + " a side)");
    }
    dabs::GrayTile tile;
    tile.width = tw;
    tile.height = th;
    tile.data.resize(std::size_t(tw) * th);
    for (std::uint32_t y = 0; y < th; ++y) {
        for (std::uint32_t x = 0; x < tw; ++x) tile.data[std::size_t(y) * tw + x] = dabs::q16(paint[std::size_t(y0 + y) * bw + (x0 + x)]);
    }
    auto png = std::make_shared<const std::string>(dabs::png_of_tile(tile));
    const std::string ref = storage::AssetStore::ref(*png);
    core::DabSettings s;
    s.size_mm = std::clamp(std::max(tw, th) * 25.4 / dpi, 0.01, 500.0);
    s.spacing = 0.25;
    s.pressure_size = false;
    s.tip = ref;
    Defined out;
    out.settings = Json{{"label", name.empty() ? std::string("サンプルブラシ") : clean(name, 40)},
                        {"width_mm", std::clamp(s.size_mm, 0.05, 50.0)},
                        {"engine", std::string(core::kDabEngine)},
                        {"dabs", core::dabs_to_json(s)}};
    out.pictures.emplace(ref, std::move(png));
    out.width = tw;
    out.height = th;
    out.dpi = dpi;
    return out;
}

std::string next_sample_name(const Library& library) {
    std::size_t n = 0;
    for (const Group& g : library.groups) {
        if (g.builtin) continue;
        for (const Preset& p : g.items) {
            if (p.name.starts_with("サンプルブラシ")) ++n;
        }
    }
    return "サンプルブラシ " + std::to_string(n + 1);
}

void use_library_pictures(const std::filesystem::path& config_dir) {
    dabs::set_picture_source([config_dir](const std::string& ref) { return brushes::library_tip(config_dir, ref); });
}

}  // namespace genko::render::brush_presets
