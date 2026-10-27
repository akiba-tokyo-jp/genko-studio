#include "render/preset_files.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

#include "core/base64.hpp"
#include "core/brush_dabs.hpp"
#include "core/brushes.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/vector.hpp"
#include "render/brush_presets.hpp"
#include "render/brushes.hpp"
#include "render/dabs/lines.hpp"
#include "render/gradient_presets.hpp"
#include "render/paper.hpp"
#include "render/pattern_library.hpp"
#include "render/preset_store.hpp"
#include "render/shape_presets.hpp"
#include "render/tool_presets.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"

namespace genko::render::preset_files {

namespace {

using core::Json;
namespace store = preset_store;
namespace vec = core::vector;

// One machine's presets of each kind (a library read, or a file).
struct Collection {
    std::set<std::string> kinds;
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, Json>>>> brushes;  // (group: its brushes' keys and settings)
    std::map<std::string, core::Bytes> brush_pictures;
    std::vector<store::Group> gradients, patterns, tool_presets;
    std::map<std::string, core::Bytes> pattern_pictures;
    std::vector<shape_presets::Group> shapes;
};

std::vector<std::string> asked(const std::vector<std::string>& wanted) {
    if (wanted.empty()) return kinds();
    for (const std::string& k : wanted) {
        if (std::find(kinds().begin(), kinds().end(), k) == kinds().end()) {
            throw core::Error("value", "the kinds of presets are brushes, gradients, patterns, shapes and tool_presets (not " + k + ")");
        }
    }
    return wanted;
}

bool wants(const std::vector<std::string>& kinds_asked, const char* kind) {
    return std::find(kinds_asked.begin(), kinds_asked.end(), kind) != kinds_asked.end();
}

// (review P3-13: a kind kept by render/preset_store; another is refused — never written to another kind's file)
const store::Kind& store_kind(const std::string& kind) {
    if (kind == "gradients") return gradient_presets::kind();
    if (kind == "patterns") return pattern_library::kind();
    if (kind == "tool_presets") return tool_presets::kind();
    throw core::Error("value", "the kinds of presets are brushes, gradients, patterns, shapes and tool_presets (not " + kind + ")");
}

// (what each kind is called on the screen: the one table — kinds() is its keys)
const std::vector<std::pair<std::string, std::string>>& kind_table() {
    static const std::vector<std::pair<std::string, std::string>> t{
        {"brushes", "ブラシ"}, {"gradients", "グラデーション"}, {"patterns", "パターン"}, {"shapes", "シェイプ"}, {"tool_presets", "ツールプリセット"}};
    return t;
}

bool file_there(const std::filesystem::path& dir, const std::string& name) {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / name, ec);
}

// --- a settings folder read ---------------------------------------------------------------------------------------

Collection from_folder(const std::filesystem::path& dir, const std::vector<std::string>& kinds_asked, bool builtins) {
    Collection c;
    if (wants(kinds_asked, "brushes")) {
        c.kinds.insert("brushes");
        const brush_presets::Library lib = brush_presets::load(dir);
        if (!lib.problem.empty()) throw core::Error("io", lib.problem);
        const Json papers = brushes::load_own_brushes(dir);  // (with their papers, their pictures made known)
        for (const brush_presets::Group& g : lib.groups) {
            if (g.builtin && !builtins) continue;
            std::vector<std::pair<std::string, Json>> items;
            for (const brush_presets::Preset& p : g.items) {
                Json settings = p.settings;
                if (const auto own = papers.find(p.key); own != papers.end() && own->contains("paper")) {
                    settings["paper"] = (*own)["paper"];
                    try {
                        settings = brushes::with_paper_picture(settings);
                    } catch (const core::Error&) {
                        settings.erase("paper");  // (its picture is not there: the brush without it)
                    }
                }
                if (core::is_dab_brush(settings)) {
                    try {
                        const Json& d = settings["dabs"];
                        bool all = true;
                        for (const std::string& ref : core::dab_assets(core::dabs_from_json(d.is_null() ? Json::object() : d))) {
                            core::Bytes png = brushes::library_tip(dir, ref);
                            if (!png) png = dabs::picture(ref);
                            if (!png) {
                                all = false;
                                break;
                            }
                            c.brush_pictures[ref] = png;
                        }
                        if (!all) continue;  // (a brush whose pictures are not there cannot be drawn elsewhere)
                    } catch (const core::Error&) {
                        continue;
                    }
                }
                items.emplace_back(p.key, settings);
            }
            c.brushes.emplace_back(g.name, std::move(items));
        }
    }
    for (const char* kind : {"gradients", "tool_presets"}) {
        if (!wants(kinds_asked, kind)) continue;
        c.kinds.insert(kind);
        const store::Library lib = store::load(dir, store_kind(kind));
        if (!lib.problem.empty()) throw core::Error("io", lib.problem);
        if (!lib.from_file && !builtins) continue;
        (std::string(kind) == "gradients" ? c.gradients : c.tool_presets) = lib.groups;
    }
    if (wants(kinds_asked, "patterns")) {
        c.kinds.insert("patterns");
        const store::Library lib = pattern_library::load(dir);
        if (!lib.problem.empty()) throw core::Error("io", lib.problem);
        if (lib.from_file || builtins) {
            for (const store::Group& g : lib.groups) {
                store::Group kept{g.name, {}};
                for (const Json& item : g.items) {
                    try {
                        const core::Pattern p = pattern_library::pattern_of(item);
                        const core::Bytes bytes = pattern_library::picture(dir, p);
                        if (!bytes) continue;
                        c.pattern_pictures[p.asset] = bytes;
                        kept.items.push_back(item);
                    } catch (const core::Error&) {
                    }
                }
                c.patterns.push_back(std::move(kept));
            }
        }
    }
    if (wants(kinds_asked, "shapes")) {
        c.kinds.insert("shapes");
        if (file_there(dir, std::string(shape_presets::kFile)) || builtins) {
            std::string problem;
            c.shapes = shape_presets::load(dir, &problem);
            if (!problem.empty()) throw core::Error("io", problem);
        }
    }
    return c;
}

// --- a file -------------------------------------------------------------------------------------------------------

Json pictures_json(const std::map<std::string, core::Bytes>& pictures) {
    Json out = Json::object();
    for (const auto& [ref, png] : pictures) out[ref] = core::b64encode(*png);
    return out;
}

std::map<std::string, core::Bytes> pictures_of(const Json& given, const char* what) {
    std::map<std::string, core::Bytes> out;
    if (given.is_null()) return out;
    if (!given.is_object()) throw core::Error("format", std::string(what) + " is an object of pictures");
    for (const auto& [ref, text] : given.items()) {
        if (!text.is_string()) throw core::Error("format", std::string(what) + " holds base64 pictures");
        auto bytes = std::make_shared<const std::string>(core::a2b_base64(text.get_ref<const std::string&>()));
        if (storage::AssetStore::ref(*bytes) != ref) continue;  // (a picture whose bytes are not its name: left out)
        out.emplace(ref, std::move(bytes));
    }
    return out;
}

Json groups_json(const std::vector<store::Group>& groups) {
    Json out = Json::array();
    for (const store::Group& g : groups) out.push_back(Json{{"name", g.name}, {"items", g.items}});
    return out;
}

std::vector<store::Group> groups_of(const Json& given, const char* what) {
    std::vector<store::Group> out;
    if (given.is_null()) return out;
    if (!given.is_array()) throw core::Error("format", std::string(what) + " is a list of groups");
    for (const Json& g : given) {
        if (!g.is_object() || !g.contains("name") || !g["name"].is_string() || !g.contains("items") || !g["items"].is_array()) {
            throw core::Error("format", std::string(what) + "'s groups are {name, items}");
        }
        out.push_back(store::Group{g["name"].get<std::string>(), std::vector<Json>(g["items"].begin(), g["items"].end())});
    }
    return out;
}

Json to_json(const Collection& c) {
    Json out{{"genko_presets", kVersion}};
    if (c.kinds.contains("brushes")) {
        Json groups = Json::array();
        for (const auto& [name, items] : c.brushes) {
            Json list = Json::array();
            for (const auto& [key, settings] : items) list.push_back(Json{{"key", key}, {"settings", settings}});
            groups.push_back(Json{{"name", name}, {"brushes", std::move(list)}});
        }
        out["brushes"] = std::move(groups);
        out["brush_pictures"] = pictures_json(c.brush_pictures);
    }
    if (c.kinds.contains("gradients")) out["gradients"] = groups_json(c.gradients);
    if (c.kinds.contains("patterns")) {
        out["patterns"] = groups_json(c.patterns);
        out["pattern_pictures"] = pictures_json(c.pattern_pictures);
    }
    if (c.kinds.contains("shapes")) {
        Json groups = Json::array();
        for (const shape_presets::Group& g : c.shapes) {
            Json shapes = Json::array();
            for (const shape_presets::Preset& p : g.shapes) shapes.push_back(Json{{"name", p.name}, {"path", vec::to_json(p.path)}});
            groups.push_back(Json{{"name", g.name}, {"shapes", std::move(shapes)}});
        }
        out["shapes"] = std::move(groups);
    }
    if (c.kinds.contains("tool_presets")) out["tool_presets"] = groups_json(c.tool_presets);
    return out;
}

Collection from_json(const Json& data, const std::vector<std::string>& kinds_asked) {
    if (!data.is_object() || data.value("genko_presets", Json()) != Json(kVersion)) {
        throw core::Error("format", "not a Genko presets file (genko_presets 1)");
    }
    Collection c;
    if (wants(kinds_asked, "brushes") && data.contains("brushes")) {
        c.kinds.insert("brushes");
        const Json& groups = data["brushes"];
        if (!groups.is_array()) throw core::Error("format", "brushes is a list of groups");
        for (const Json& g : groups) {
            if (!g.is_object() || !g.contains("name") || !g["name"].is_string() || !g.contains("brushes") || !g["brushes"].is_array()) {
                throw core::Error("format", "brushes' groups are {name, brushes}");
            }
            std::vector<std::pair<std::string, Json>> items;
            for (const Json& b : g["brushes"]) {
                if (!b.is_object() || !b.contains("key") || !b["key"].is_string() || !b.contains("settings") || !b["settings"].is_object()) {
                    throw core::Error("format", "a brush is {key, settings}");
                }
                items.emplace_back(b["key"].get<std::string>(), b["settings"]);
            }
            c.brushes.emplace_back(g["name"].get<std::string>(), std::move(items));
        }
        c.brush_pictures = pictures_of(data.value("brush_pictures", Json()), "brush_pictures");
    }
    if (wants(kinds_asked, "gradients") && data.contains("gradients")) {
        c.kinds.insert("gradients");
        c.gradients = groups_of(data["gradients"], "gradients");
    }
    if (wants(kinds_asked, "patterns") && data.contains("patterns")) {
        c.kinds.insert("patterns");
        c.patterns = groups_of(data["patterns"], "patterns");
        c.pattern_pictures = pictures_of(data.value("pattern_pictures", Json()), "pattern_pictures");
    }
    if (wants(kinds_asked, "shapes") && data.contains("shapes")) {
        c.kinds.insert("shapes");
        const Json& groups = data["shapes"];
        if (!groups.is_array()) throw core::Error("format", "shapes is a list of groups");
        for (const Json& g : groups) {
            if (!g.is_object() || !g.contains("name") || !g["name"].is_string() || !g.contains("shapes") || !g["shapes"].is_array()) {
                throw core::Error("format", "shapes' groups are {name, shapes}");
            }
            shape_presets::Group group{g["name"].get<std::string>(), {}};
            for (const Json& s : g["shapes"]) {
                if (!s.is_object() || !s.contains("name") || !s["name"].is_string()) throw core::Error("format", "a shape is {name, path}");
                try {
                    group.shapes.push_back(shape_presets::Preset{s["name"].get<std::string>(), vec::path_from_json(s.value("path", Json()))});
                } catch (const core::Error& error) {
                    throw core::Error("format", std::string("a shape's path cannot be read: ") + error.what());
                }
            }
            c.shapes.push_back(std::move(group));
        }
    }
    if (wants(kinds_asked, "tool_presets") && data.contains("tool_presets")) {
        c.kinds.insert("tool_presets");
        c.tool_presets = groups_of(data["tool_presets"], "tool_presets");
    }
    return c;
}

// --- adding to this machine's library ------------------------------------------------------------------------------

// `whole`: プリセットを移行 — a group the library has (by its name) is left out whole; else every preset is added (one the
// library has as it is: not again).
Json merge(const std::filesystem::path& config_dir, const Collection& c, bool whole) {
    Json added = Json::object();
    if (c.kinds.contains("brushes")) {
        const brush_presets::Library lib = brush_presets::load(config_dir);
        if (!lib.problem.empty()) throw core::Error("io", lib.problem);
        std::set<std::string> names, keys, groups;
        std::vector<std::string> order;
        std::size_t own_count = 0;
        // (each brush's settings as one form, made once: a file of thousands is compared in one pass — review P3-6)
        const auto form_of = [](const Json& settings) -> std::string {
            try {
                return core::brush_to_dict(core::brush_from_dict("my_a", settings, std::nullopt, {})).dump();
            } catch (const core::Error&) {
                return {};
            }
        };
        std::set<std::string> own_forms;
        for (const brush_presets::Group& g : lib.groups) {
            if (!g.builtin) order.push_back(g.name);
            for (const brush_presets::Preset& p : g.items) {
                names.insert(p.name);
                if (!g.builtin) {
                    keys.insert(p.key);
                    ++own_count;
                    if (!whole) own_forms.insert(form_of(p.settings));
                }
            }
            groups.insert(g.name);
        }
        const std::vector<std::string> order_before = order;
        std::vector<std::pair<std::string, std::optional<Json>>> changes;
        std::int64_t n = 0;
        for (const auto& [group, items] : c.brushes) {
            if (whole && groups.contains(group)) continue;
            bool builtin_group = false;
            for (const brush_presets::Group& g : brush_presets::builtin_groups()) builtin_group = builtin_group || g.name == group;
            std::int64_t here = 0;
            for (const auto& [key, given] : items) {
                if (brush_presets::builtin(key) != nullptr || brush_presets::builtin_dab_of(key) != nullptr) continue;  // (every machine has them)
                Json settings;
                try {
                    settings = brushes::with_paper_taken_in(given);
                    settings = core::brush_to_dict(core::brush_from_dict(key, settings, std::nullopt, {}));
                } catch (const core::Error&) {
                    continue;  // (settings a brush cannot have: left out)
                }
                if (!whole && own_forms.contains(form_of(settings))) continue;
                if (core::is_dab_brush(settings)) {  // (its pictures with it, or held here: else it cannot be drawn)
                    bool all = true;
                    const Json& d = settings["dabs"];
                    for (const std::string& ref : core::dab_assets(core::dabs_from_json(d.is_null() ? Json::object() : d))) {
                        all = all && (c.brush_pictures.contains(ref) || brushes::library_tip(config_dir, ref) || dabs::picture(ref));
                    }
                    if (!all) continue;
                }
                const std::string made_key = keys.contains(key) || !brush_presets::is_own(key) ? "my_" + core::new_id() : key;
                keys.insert(made_key);
                std::string name = settings.value("label", std::string("ブラシ"));
                for (int k = 2; names.contains(name); ++k) name = settings.value("label", std::string("ブラシ")) + " " + std::to_string(k);
                names.insert(name);
                settings["label"] = name;
                const std::string kept_group = builtin_group ? std::string(brush_presets::kOwnGroup) : group;
                if (kept_group != brush_presets::kOwnGroup) settings["group"] = kept_group;
                if (std::find(order.begin(), order.end(), kept_group) == order.end()) order.push_back(kept_group);
                changes.emplace_back(made_key, settings);
                ++here;
            }
            if (whole && here == 0 && !builtin_group && std::find(order.begin(), order.end(), group) == order.end()) order.push_back(group);
            n += here;
        }
        if (own_count + changes.size() > brush_presets::kMaxOwn) {
            throw core::Error("value", "the library holds " + std::to_string(brush_presets::kMaxOwn) + " brushes of one's own at most");
        }
        if (!changes.empty() || order != order_before) {
            const bool plain = order.empty() || (order.size() == 1 && order.front() == brush_presets::kOwnGroup);
            const bool kept_order = !brushes::library_groups(config_dir).empty();
            brushes::save_many(config_dir, changes, plain && !kept_order ? std::nullopt : std::optional<std::vector<std::string>>(order), c.brush_pictures);
        }
        added["brushes"] = n;
    }
    for (const char* kind : {"gradients", "tool_presets", "patterns"}) {
        if (!c.kinds.contains(kind)) continue;
        const std::string k = kind;
        const std::vector<store::Group>& source = k == "gradients" ? c.gradients : k == "patterns" ? c.patterns : c.tool_presets;
        store::Library lib = k == "patterns" ? pattern_library::load(config_dir) : store::load(config_dir, store_kind(k));
        if (!lib.problem.empty()) throw core::Error("io", lib.problem);
        std::int64_t n = 0;
        bool grouped = false;  // (a group made though nothing was added to it: written too)
        for (const store::Group& g : source) {
            const bool there = std::any_of(lib.groups.begin(), lib.groups.end(), [&](const store::Group& have) { return have.name == g.name; });
            if (whole && there) continue;
            grouped = grouped || (whole && !there);
            std::vector<Json> items;
            for (const Json& item : g.items) {
                const bool held = std::any_of(lib.groups.begin(), lib.groups.end(), [&](const store::Group& have) {
                    return std::find(have.items.begin(), have.items.end(), item) != have.items.end();
                });
                if (!whole && held) continue;
                items.push_back(item);
            }
            if (k == "patterns") {
                if (whole && items.empty()) store::add_many(lib.groups, store_kind(k), {}, g.name, "パターン");
                for (const Json& item : items) {
                    try {
                        core::Pattern p = pattern_library::pattern_of(item);
                        const auto bytes = c.pattern_pictures.find(p.asset);
                        if (bytes == c.pattern_pictures.end()) continue;
                        p.name = item.value("name", std::string("パターン"));
                        pattern_library::add(config_dir, lib.groups, p, bytes->second, g.name);
                        ++n;
                    } catch (const core::Error&) {
                    }
                }
            } else {
                std::vector<Json> kept;
                for (const Json& item : items) {
                    try {
                        if (store_kind(k).check) store_kind(k).check(item);
                        kept.push_back(item);
                    } catch (const core::Error&) {
                    }
                }
                if (!kept.empty() || whole) store::add_many(lib.groups, store_kind(k), kept, g.name, k == "gradients" ? "グラデーション" : "ツールプリセット");
                n += static_cast<std::int64_t>(kept.size());
            }
        }
        if (n > 0 || grouped) {  // (nothing added: the file as it was — review P3-1)
            if (k == "patterns") {
                pattern_library::save(config_dir, lib.groups);
                pattern_library::tidy(config_dir, lib.groups);
            } else {
                store::save(config_dir, store_kind(k), lib.groups);
            }
        }
        added[kind] = n;
    }
    if (c.kinds.contains("shapes")) {
        std::string problem;
        std::vector<shape_presets::Group> groups = shape_presets::load(config_dir, &problem);
        if (!problem.empty()) throw core::Error("io", problem);
        std::int64_t n = 0;
        for (const shape_presets::Group& g : c.shapes) {
            const bool there = std::any_of(groups.begin(), groups.end(), [&](const shape_presets::Group& have) { return have.name == g.name; });
            if (whole && there) continue;
            if (whole) groups.push_back(shape_presets::Group{g.name, {}});
            for (const shape_presets::Preset& p : g.shapes) {
                const bool held = std::any_of(groups.begin(), groups.end(), [&](const shape_presets::Group& have) {
                    return std::any_of(have.shapes.begin(), have.shapes.end(),
                                       [&](const shape_presets::Preset& q) { return q.name == p.name && vec::to_json(q.path) == vec::to_json(p.path); });
                });
                if (!whole && held) continue;
                shape_presets::add(groups, p.name, g.name, p.path);
                ++n;
            }
        }
        shape_presets::save(config_dir, groups);
        added["shapes"] = n;
    }
    return added;
}

}  // namespace

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> k = [] {
        std::vector<std::string> out;
        for (const auto& [kind, title] : kind_table()) out.push_back(kind);
        return out;
    }();
    return k;
}

std::string kind_title(std::string_view kind) {
    for (const auto& [k, title] : kind_table()) {
        if (k == kind) return title;
    }
    return std::string(kind);
}

Json manager_list(const std::filesystem::path& config_dir, const std::vector<std::string>& wanted) {
    Json out = Json::object();
    for (const std::string& kind : asked(wanted)) {
        Json groups = Json::array();
        std::string problem;
        if (kind == "brushes") {
            const brush_presets::Library lib = brush_presets::load(config_dir);
            problem = lib.problem;
            for (const brush_presets::Group& g : lib.groups) {
                Json names = Json::array();
                for (const brush_presets::Preset& p : g.items) names.push_back(p.name);
                groups.push_back(Json{{"name", g.name}, {"builtin", g.builtin}, {"presets", std::move(names)}});
            }
        } else if (kind == "shapes") {
            for (const shape_presets::Group& g : shape_presets::load(config_dir, &problem)) {
                Json names = Json::array();
                for (const shape_presets::Preset& p : g.shapes) names.push_back(p.name);
                groups.push_back(Json{{"name", g.name}, {"presets", std::move(names)}});
            }
        } else {
            const store::Library lib = kind == "patterns" ? pattern_library::load(config_dir) : store::load(config_dir, store_kind(kind));
            problem = lib.problem;
            for (const store::Group& g : lib.groups) {
                Json names = Json::array();
                for (const Json& item : g.items) names.push_back(item.value("name", std::string()));
                groups.push_back(Json{{"name", g.name}, {"presets", std::move(names)}});
            }
        }
        Json entry{{"groups", std::move(groups)}};
        if (!problem.empty()) entry["problem"] = problem;
        out[kind] = std::move(entry);
    }
    return out;
}

Json manager_edit(const std::filesystem::path& config_dir, const std::string& kind, const Json& params) {
    (void)asked({kind});
    if (kind == "brushes") return brush_presets::edit(config_dir, params);
    if (kind == "shapes") {
        std::string problem;
        std::vector<shape_presets::Group> groups = shape_presets::load(config_dir, &problem);
        if (!problem.empty()) throw core::Error("io", problem);
        Json out = shape_presets::edit(groups, params);
        shape_presets::save(config_dir, groups);
        return out;
    }
    store::Library lib = kind == "patterns" ? pattern_library::load(config_dir) : store::load(config_dir, store_kind(kind));
    if (!lib.problem.empty()) throw core::Error("io", lib.problem);
    Json out = store::edit(lib.groups, store_kind(kind), params);
    if (kind == "patterns") {
        pattern_library::save(config_dir, lib.groups);
        pattern_library::tidy(config_dir, lib.groups);
    } else {
        store::save(config_dir, store_kind(kind), lib.groups);
    }
    return out;
}

std::string export_text(const std::filesystem::path& config_dir, const std::vector<std::string>& wanted, bool builtins, Json* counts) {
    const Collection c = from_folder(config_dir, asked(wanted), builtins);
    if (counts != nullptr) {
        *counts = Json::object();
        const auto items = [](const std::vector<store::Group>& groups) {
            std::int64_t n = 0;
            for (const store::Group& g : groups) n += static_cast<std::int64_t>(g.items.size());
            return n;
        };
        if (c.kinds.contains("brushes")) {
            std::int64_t n = 0;
            for (const auto& g : c.brushes) n += static_cast<std::int64_t>(g.second.size());
            (*counts)["brushes"] = n;
        }
        if (c.kinds.contains("gradients")) (*counts)["gradients"] = items(c.gradients);
        if (c.kinds.contains("patterns")) (*counts)["patterns"] = items(c.patterns);
        if (c.kinds.contains("shapes")) {
            std::int64_t n = 0;
            for (const auto& g : c.shapes) n += static_cast<std::int64_t>(g.shapes.size());
            (*counts)["shapes"] = n;
        }
        if (c.kinds.contains("tool_presets")) (*counts)["tool_presets"] = items(c.tool_presets);
    }
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    std::string text = core::dump(to_json(c), options);
    if (text.size() > kMaxFileBytes) throw core::Error("too_large", "the presets are larger than " + std::to_string(kMaxFileBytes >> 20) + " MB together");
    return text;
}

Json import_text(const std::filesystem::path& config_dir, std::string_view text, const std::vector<std::string>& wanted) {
    if (text.size() > kMaxFileBytes) throw core::Error("too_large", "a presets file is at most " + std::to_string(kMaxFileBytes >> 20) + " MB");
    if (core::utf8_error(text)) throw core::Error("format", "not a Genko presets file (not UTF-8)");
    Json data;
    try {
        data = core::parse_python_json(text);
    } catch (const core::Error& error) {
        throw core::Error("format", std::string("not a Genko presets file: ") + error.what());
    }
    const Collection c = from_json(data, asked(wanted));
    return Json{{"added", merge(config_dir, c, false)}};
}

Json migrate(const std::filesystem::path& config_dir, const std::filesystem::path& from) {
    std::error_code ec;
    Collection c;
    if (std::filesystem::is_directory(from, ec)) {
        if (std::filesystem::equivalent(from, config_dir, ec)) throw core::Error("value", "the settings folder is this one: nothing to take from it");
        c = from_folder(from, kinds(), false);
    } else {
        const std::string text = storage::read_file_bounded(from, kMaxFileBytes);
        if (core::utf8_error(text)) throw core::Error("format", "not a Genko presets file (not UTF-8)");
        Json data;
        try {
            data = core::parse_python_json(text);
        } catch (const core::Error& error) {
            throw core::Error("format", std::string("not a Genko presets file: ") + error.what());
        }
        c = from_json(data, kinds());
    }
    const Json added = merge(config_dir, c, true);
    std::int64_t total = 0;
    for (const auto& [k, n] : added.items()) total += n.get<std::int64_t>();
    return Json{{"migrated", total}, {"added", added}};
}

}  // namespace genko::render::preset_files
