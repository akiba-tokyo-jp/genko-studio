#include "render/preset_store.hpp"

#include <algorithm>
#include <set>
#include <system_error>

#include "core/error.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "storage/fsutil.hpp"

// PhotoCraft v0.2.0 crates/engine/src/presets/mod.rs (lines 35-45 Group, 193-276 find, unique_name, group_index,
// edit_groups), MIT: native/third_party/photocraft/NOTICE.md.

namespace genko::render::preset_store {

using core::Json;

namespace {

[[noreturn]] void bad(const std::string& why) { throw core::Error("value", why); }

// A name as it is kept: its spaces at the ends taken off, 1..200 characters.
std::string clean(const Json& value, const char* what) {
    if (!value.is_string()) bad(std::string(what) + " must be text");
    const std::string name = core::py_strip(value.get<std::string>());
    const std::size_t chars = core::code_points(name).size();
    if (chars < 1 || chars > 200) bad(std::string(what) + " must be 1 to 200 characters");
    return name;
}

bool taken(const std::vector<Group>& groups, std::string_view name) {
    for (const Group& g : groups) {
        for (const Json& item : g.items) {
            if (item.is_object() && item.value("name", std::string()) == name) return true;
        }
    }
    return false;
}

std::vector<Group>::iterator group_named(std::vector<Group>& groups, std::string_view name) {
    return std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.name == name; });
}

std::size_t count(const std::vector<Group>& groups) {
    std::size_t n = 0;
    for (const Group& g : groups) n += g.items.size();
    return n;
}

// The groups as a kind holds them: each name and item checked, the names unique.
void check(const std::vector<Group>& groups, const Kind& kind) {
    if (groups.size() > kind.max_groups) bad("there are too many groups (" + std::to_string(kind.max_groups) + " at most)");
    if (count(groups) > kind.max_items) bad("there are too many presets (" + std::to_string(kind.max_items) + " at most)");
    std::vector<std::string> group_names, item_names;
    for (const Group& g : groups) {
        group_names.push_back(clean(Json(g.name), "a group's name"));
        for (const Json& item : g.items) {
            if (!item.is_object()) bad("a preset must be an object");
            item_names.push_back(clean(item.value("name", Json()), "a preset's name"));
            if (kind.check) kind.check(item);
        }
    }
    for (std::vector<std::string>* names : {&group_names, &item_names}) {
        std::sort(names->begin(), names->end());
        if (std::adjacent_find(names->begin(), names->end()) != names->end()) bad("a name is there twice: " + *std::adjacent_find(names->begin(), names->end()));
    }
}

// The groups of the kind's file's text: core::Error for one that is not this layout, or holds what the kind refuses.
std::vector<Group> parse(const std::string& text, const Kind& kind) {
    const Json data = core::parse_python_json(text);
    if (!data.is_object() || data.value("version", Json()) != Json(kVersion)) bad(kind.file + " is not of this layout (version " + std::to_string(kVersion) + ")");
    const Json* groups = core::get(data, "groups");
    if (groups == nullptr || !groups->is_array()) bad(kind.file + " has no list of groups");
    std::vector<Group> out;
    for (const Json& g : *groups) {
        const Json* items = core::get(g, "items");
        if (items == nullptr || !items->is_array()) bad("a group has no list of presets");
        out.push_back(Group{g.value("name", std::string()), std::vector<Json>(items->begin(), items->end())});
    }
    check(out, kind);
    return out;
}

Json file_json(const std::vector<Group>& groups) {
    Json list = Json::array();
    for (const Group& g : groups) list.push_back(Json{{"name", g.name}, {"items", g.items}});
    return Json{{"version", kVersion}, {"groups", std::move(list)}};
}

std::vector<Group> builtin_of(const Kind& kind) { return kind.builtin ? kind.builtin() : std::vector<Group>{}; }

}  // namespace

Library load(const std::filesystem::path& config_dir, const Kind& kind) {
    Library out;
    const std::filesystem::path file = config_dir / kind.file;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        out.groups = builtin_of(kind);
        return out;
    }
    try {
        out.groups = parse(storage::read_file_bounded(file, kFileMaxBytes), kind);
        out.from_file = true;
    } catch (const std::exception& error) {
        out.groups = builtin_of(kind);
        out.problem = error.what();
    }
    return out;
}

void save(const std::filesystem::path& config_dir, const Kind& kind, const std::vector<Group>& groups) {
    check(groups, kind);
    const std::filesystem::path file = config_dir / kind.file;
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        try {
            (void)parse(storage::read_file_bounded(file, kFileMaxBytes), kind);
        } catch (const std::exception& error) {
            throw core::Error("io", kind.file + " cannot be read, and is not written over (" + error.what() + ")");
        }
    }
    const std::string text = core::dump_canonical(file_json(groups));
    if (text.size() > kFileMaxBytes) bad("the presets are too large to keep (" + std::to_string(kFileMaxBytes >> 20) + " MB at most)");
    std::filesystem::create_directories(config_dir, ec);
    storage::write_atomic(file, text);
}

Json list(const std::vector<Group>& groups) {
    Json out = Json::array();
    for (const Group& g : groups) {
        Json names = Json::array();
        for (const Json& item : g.items) names.push_back(item.value("name", std::string()));
        out.push_back(Json{{"name", g.name}, {"items", std::move(names)}});
    }
    return Json{{"groups", std::move(out)}};
}

const Json* find(const std::vector<Group>& groups, std::string_view name, std::optional<std::string_view> group) {
    for (const Group& g : groups) {
        if (group && g.name != *group) continue;
        for (const Json& item : g.items) {
            if (item.is_object() && item.value("name", std::string()) == name) return &item;
        }
    }
    return nullptr;
}

std::string unique_name(const std::vector<Group>& groups, std::string_view base) {
    if (!taken(groups, base)) return std::string(base);
    for (int n = 2;; ++n) {
        const std::string name = std::string(base) + " " + std::to_string(n);
        if (!taken(groups, name)) return name;
    }
}

Json add(std::vector<Group>& groups, const Kind& kind, Json item, std::optional<std::string_view> group, std::string_view fallback_name) {
    if (!item.is_object()) bad("a preset must be an object");
    const Json given = item.contains("name") ? item["name"] : Json(std::string(fallback_name));
    const std::string base = clean(given, "a preset's name");
    if (count(groups) >= kind.max_items) bad("there are too many presets (" + std::to_string(kind.max_items) + " at most)");
    const std::string where = clean(Json(group ? std::string(*group) : kind.default_group), "a group's name");
    item["name"] = unique_name(groups, base);
    if (kind.check) kind.check(item);
    auto it = group_named(groups, where);
    if (it == groups.end()) {
        if (groups.size() >= kind.max_groups) bad("there are too many groups (" + std::to_string(kind.max_groups) + " at most)");
        groups.push_back(Group{where, {}});
        it = groups.end() - 1;
    }
    const std::string made = item["name"].get<std::string>();
    it->items.push_back(std::move(item));
    return Json{{"name", made}, {"group", where}};
}

Json edit(std::vector<Group>& groups, const Kind& kind, const Json& params) {
    if (!params.is_object()) bad("the edit is {action, …}");
    const std::string action = params.value("action", std::string());
    const Json* g = core::get(params, "group");
    const std::optional<std::string> group = g != nullptr && g->is_string() ? std::optional(g->get<std::string>()) : std::nullopt;
    const auto presets = [&] {
        std::vector<std::string> names;
        const Json* p = core::get(params, "preset");
        if (p != nullptr && p->is_string()) names.push_back(p->get<std::string>());
        else if (p != nullptr && p->is_array())
            for (const Json& v : *p) names.push_back(clean(v, "preset"));
        if (names.empty()) bad("preset names the preset (or presets)");
        return names;
    };
    const auto locate = [&](const std::string& name) {
        for (Group& each : groups) {
            if (group && each.name != *group) continue;
            for (std::size_t i = 0; i < each.items.size(); ++i) {
                if (each.items[i].value("name", std::string()) == name) return std::pair<Group*, std::size_t>{&each, i};
            }
        }
        bad("no preset named " + name);
    };
    if (action == "rename") {
        const auto names = presets();
        const std::string to = clean(params.value("name", Json()), "name");
        auto [in, i] = locate(names.front());
        if (to != names.front() && taken(groups, to)) bad("a preset named " + to + " is there already");
        in->items[i]["name"] = to;
        return Json{{"name", to}};
    }
    if (action == "delete") {
        std::int64_t n = 0;
        for (const std::string& name : presets()) {
            auto [in, i] = locate(name);
            in->items.erase(in->items.begin() + static_cast<std::ptrdiff_t>(i));
            ++n;
        }
        return Json{{"deleted", n}};
    }
    if (action == "move") {
        const std::string to = clean(params.value("to", Json()), "to");
        if (group_named(groups, to) == groups.end()) bad("no group named " + to);
        std::vector<Json> moving;
        for (const std::string& name : presets()) {
            auto [in, i] = locate(name);
            moving.push_back(in->items[i]);
            in->items.erase(in->items.begin() + static_cast<std::ptrdiff_t>(i));
        }
        const auto target = group_named(groups, to);
        std::size_t at = target->items.size();
        if (const Json* index = core::get(params, "index"); index != nullptr && index->is_number_integer())
            at = static_cast<std::size_t>(std::clamp<std::int64_t>(index->get<std::int64_t>(), 0, static_cast<std::int64_t>(at)));
        target->items.insert(target->items.begin() + static_cast<std::ptrdiff_t>(at), moving.begin(), moving.end());
        return Json{{"moved", static_cast<std::int64_t>(moving.size())}, {"group", to}};
    }
    if (action == "new_group") {
        const std::string name = clean(params.value("name", Json()), "name");
        if (group_named(groups, name) != groups.end()) bad("a group named " + name + " is there already");
        if (groups.size() >= kind.max_groups) bad("there are too many groups (" + std::to_string(kind.max_groups) + " at most)");
        groups.push_back(Group{name, {}});
        return Json{{"group", name}};
    }
    if (action == "rename_group") {
        if (!group) bad("group names the group");
        const std::string name = clean(params.value("name", Json()), "name");
        const auto it = group_named(groups, *group);
        if (it == groups.end()) bad("no group named " + *group);
        if (name != *group && group_named(groups, name) != groups.end()) bad("a group named " + name + " is there already");
        it->name = name;
        return Json{{"group", name}};
    }
    if (action == "delete_group") {
        if (!group) bad("group names the group");
        const auto it = group_named(groups, *group);
        if (it == groups.end()) bad("no group named " + *group);
        const auto items = static_cast<std::int64_t>(it->items.size());
        groups.erase(it);
        return Json{{"deleted", items}};
    }
    bad("action is rename, delete, move, new_group, rename_group or delete_group");
}

std::vector<Group> reset(const std::vector<Group>& groups, const Kind& kind, bool append) {
    std::vector<Group> made = builtin_of(kind);
    if (!append) return made;
    std::vector<Group> out = groups;
    std::set<std::string> names;  // (every item's: a name the person's presets have gets a number on the built-in one)
    for (const Group& g : out) {
        for (const Json& item : g.items) names.insert(item.value("name", std::string()));
    }
    for (Group& g : made) {
        std::string name = g.name;
        for (int n = 2; group_named(out, name) != out.end(); ++n) name = g.name + " " + std::to_string(n);
        g.name = name;
        for (Json& item : g.items) {
            const std::string base = item.value("name", std::string());
            std::string unique = base;
            for (int n = 2; names.contains(unique); ++n) unique = base + " " + std::to_string(n);
            names.insert(unique);
            item["name"] = unique;
        }
        out.push_back(std::move(g));
    }
    return out;
}

}  // namespace genko::render::preset_store
