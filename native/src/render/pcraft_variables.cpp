// PC5 I-d: a .pcraft's variables (render/pcraft_variables.hpp) — PhotoCraft v0.2.0's doc/src/variables.rs (its JSON: a def's
// "kind" {"type": visibility | textReplacement | pixelReplacement, method, align, clip}, a value's "kind" and "value"), MIT
// (native/third_party/photocraft/NOTICE.md).

#include "render/pcraft_variables.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/paths.hpp"
#include "core/variables.hpp"
#include "render/layer_bounds.hpp"
#include "render/not_yet_ported.hpp"
#include "render/smart_decode.hpp"

namespace genko::render::pcraft_variables {

namespace {

namespace fs = std::filesystem;
using core::Json;

// PhotoCraft's camelCase names and Genko's (asIs → as_is, topLeft → top_left).
std::string snake(std::string_view camel) {
    std::string out;
    for (const char c : camel) {
        if (std::isupper(static_cast<unsigned char>(c)) != 0) {
            out += '_';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

std::string camel(std::string_view snake_case) {
    std::string out;
    bool up = false;
    for (const char c : snake_case) {
        if (c == '_') {
            up = true;
            continue;
        }
        out += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        up = false;
    }
    return out;
}

std::string text(const Json& data, const char* key) {
    const auto it = data.find(key);
    return it != data.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// The pictures named by paths beside the .pcraft: read when a path is relative and leads — its links followed — into that
// folder, a file of at most 256 MiB this build reads as a picture (none otherwise); each file read once (by its resolved
// place), all of them together within the book's 4 GiB (one past it not read: `over` counts them).
bool holds(const Json& v) {
    if (v.is_null()) return false;
    if (v.is_array()) return !v.empty();
    if (v.is_object()) return std::any_of(v.begin(), v.end(), [](const Json& item) { return holds(item); });
    return true;
}

struct Beside {
    std::map<fs::path, std::optional<core::PictureValue>> read;  // (a file's resolved place → what it was)
    std::uint64_t total = 0;
    std::size_t over = 0;
    std::size_t reads = 0;

    std::optional<core::PictureValue> picture(const std::string& path, pcraft_production::ReadContext& context) {
        if (!context.beside || path.empty() || context.book == nullptr) return std::nullopt;
        const fs::path given = core::path_from_utf8(path);
        if (given.is_absolute() || given.has_root_name() || given.has_root_directory()) return std::nullopt;
        for (const fs::path& part : given) {
            if (part == "..") return std::nullopt;
        }
        std::error_code ec;
        const fs::path folder = fs::canonical(*context.beside, ec);
        if (ec) return std::nullopt;
        const fs::path file = fs::canonical(*context.beside / given, ec);  // (its links followed)
        if (ec) return std::nullopt;
        const fs::path inside = file.lexically_relative(folder);
        if (inside.empty() || inside.native().starts_with("..") || inside.is_absolute()) return std::nullopt;  // (out of the folder)
        if (const auto known = read.find(file); known != read.end()) {
            if (!known->second) return std::nullopt;
            core::PictureValue again = *known->second;
            again.name = core::path_to_utf8(given.filename());
            if (again.name.size() > 1024) again.name.clear();
            return again;
        }
        std::optional<core::PictureValue>& slot = read[file];
        if (!fs::is_regular_file(file, ec)) return std::nullopt;
        const auto size = fs::file_size(file, ec);
        if (ec || size == 0 || size > smart::kFileMaxBytes) return std::nullopt;
        if (total + size > core::kVariablePicturesMax) {
            ++over;
            read.erase(file);  // (said, not read)
            return std::nullopt;
        }
        std::ifstream in(file, std::ios::binary);
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return std::nullopt;
        ++reads;
        smart::FileInfo info;
        try {
            info = smart::look_at(bytes, "");
        } catch (const NotYetPorted&) {
            return std::nullopt;
        } catch (const core::Error&) {
            return std::nullopt;
        }
        core::PictureValue picture;
        picture.asset = core::bytes_ref(bytes);
        picture.suffix = info.suffix;
        picture.name = core::path_to_utf8(given.filename());
        if (picture.name.size() > 1024) picture.name.clear();
        if (!context.book->smart_files.contains(picture.asset)) {
            total += size;
            context.book->smart_files[picture.asset] = std::make_shared<const std::string>(std::move(bytes));
        }
        slot = picture;
        return picture;
    }
};

}  // namespace

void read(const Json& value, pcraft_production::ReadContext& context) {
    if (context.book == nullptr || context.page == nullptr) {  // (a .pcraft inside a smart object: its page is the contents', no book's)
        if (context.say && holds(value)) context.say("中の .pcraft の変数とデータセットは外しました");
        return;
    }
    if (!value.is_object()) {
        if (context.say && !value.is_null()) context.say("変数とデータセットは読めない形のため外しました");
        return;
    }
    Beside beside;
    const auto said = [&](const std::string& note) {
        if (context.say) context.say(note);
    };
    core::Variables variables;
    std::set<std::string> names;
    bool dropped = false;
    const Json defs = value.contains("defs") && value["defs"].is_array() ? value["defs"] : Json::array();
    for (const Json& d : defs) {
        if (!d.is_object() || variables.defs.size() >= core::kVariableDefsMax) {
            dropped = true;
            continue;
        }
        core::VariableDef def;
        def.name = text(d, "name");
        const Json kind = d.contains("kind") && d["kind"].is_object() ? d["kind"] : Json::object();
        const std::string type = text(kind, "type");
        def.type = type == "visibility" ? "visibility" : type == "textReplacement" ? "text" : type == "pixelReplacement" ? "pixels" : "";
        const Json* layer = d.contains("layer") ? &d["layer"] : nullptr;
        if (def.name.empty() || def.type.empty() || layer == nullptr || !(layer->is_number_integer() && layer->get<std::int64_t>() >= 0) || names.contains(def.name)) {
            dropped = true;
            continue;
        }
        const std::string raw = std::to_string(layer->get<std::int64_t>());
        const auto made = context.layers.find(raw);
        def.page = context.page->id;
        def.layer = made != context.layers.end() ? made->second : "pcraft-" + raw;  // (a layer gone before it was saved)
        if (def.type == "pixels") {
            def.method = snake(text(kind, "method"));
            if (std::find(std::begin(core::kVariableMethods), std::end(core::kVariableMethods), def.method) == std::end(core::kVariableMethods)) def.method = "fit";
            def.align = snake(text(kind, "align"));
            if (std::find(std::begin(core::kVariableAligns), std::end(core::kVariableAligns), def.align) == std::end(core::kVariableAligns)) def.align = "center";
            def.clip = kind.contains("clip") && kind["clip"].is_boolean() && kind["clip"].get<bool>();
            def.box_mm = {0, 0, context.page->spec.width_mm.value(), context.page->spec.height_mm.value()};
            if (made != context.layers.end()) {
                try {
                    if (const auto held = layer_bounds::of_layer(*context.book, *context.page, made->second); held && (*held)[2] > (*held)[0] && (*held)[3] > (*held)[1])
                        def.box_mm = *held;
                } catch (const std::exception&) {
                }
            }
        }
        names.insert(def.name);
        variables.defs.push_back(std::move(def));
    }
    std::size_t unread_pictures = 0;
    std::set<std::string> set_names;
    const Json sets = value.contains("data_sets") && value["data_sets"].is_array() ? value["data_sets"] : Json::array();
    for (const Json& s : sets) {
        const std::string name = s.is_object() ? text(s, "name") : std::string();
        if (name.empty() || set_names.contains(name) || variables.data_sets.size() >= core::kDataSetsMax) {
            dropped = true;
            continue;
        }
        core::DataSet set;
        set.name = name;
        std::set<std::string> given;
        for (const Json& v : s.contains("values") && s["values"].is_array() ? s["values"] : Json::array()) {
            if (!v.is_object()) continue;
            core::DataValue one;
            one.variable = text(v, "variable");
            const core::VariableDef* def = core::find_def(variables, one.variable);
            const std::string kind = text(v, "kind");
            const Json payload = v.contains("value") ? v["value"] : Json();
            if (def == nullptr || given.contains(one.variable)) {
                dropped = true;
                continue;
            }
            if (kind == "visibility" && payload.is_boolean() && def->type == "visibility") {
                one.value = payload.get<bool>();
            } else if (kind == "text" && payload.is_string() && def->type == "text" && payload.get_ref<const std::string&>().size() <= core::kVariableTextMax) {
                one.value = payload.get<std::string>();
            } else if (kind == "pixels" && payload.is_string() && def->type == "pixels") {
                const auto picture = beside.picture(payload.get<std::string>(), context);
                if (!picture) {
                    ++unread_pictures;
                    continue;
                }
                one.value = *picture;
            } else {
                dropped = true;
                continue;
            }
            given.insert(one.variable);
            set.values.push_back(std::move(one));
        }
        set_names.insert(name);
        variables.data_sets.push_back(std::move(set));
    }
    if (const auto active = value.find("active"); active != value.end() && active->is_number_integer() && active->get<std::int64_t>() >= 0 && static_cast<std::uint64_t>(active->get<std::int64_t>()) < variables.data_sets.size()) {
        variables.active = active->get<std::int64_t>();
    }
    if (variables.empty()) return;
    try {
        (void)core::variables_from_json(core::variables_to_json(variables));
        core::set_variables(*context.book, std::move(variables));
    } catch (const core::Error&) {
        said("変数とデータセットは読めない形のため外しました");
        return;
    }
    if (dropped) said("変数とデータセットの一部は読めない形のため外しました");
    if (unread_pictures > beside.over) said("データセットの画像のうち " + std::to_string(unread_pictures - beside.over) + " 個は .pcraft の隣に読めるファイルが無いため外しました");
    if (beside.over > 0) said("データセットの画像のうち " + std::to_string(beside.over) + " 個は合わせて 4 GiB を越えるため読まずに外しました");
}

std::optional<Json> write(const core::Page& page, pcraft_production::WriteContext& context) {
    if (context.book == nullptr || !core::uses_variables(*context.book)) return std::nullopt;
    const auto said = [&](const std::string& note) {
        if (context.say) context.say(note);
    };
    const core::Variables variables = core::variables_of(*context.book);
    Json defs = Json::array();
    std::set<std::string> written;
    bool left_defs = false;
    for (const core::VariableDef& def : variables.defs) {
        const auto id = def.layer ? context.layers.find(*def.layer) : context.layers.end();
        if (def.line || def.page != page.id || id == context.layers.end()) {
            left_defs = left_defs || def.line.has_value() || def.page == page.id;
            continue;
        }
        Json kind = Json::object();
        kind["type"] = def.type == "visibility" ? "visibility" : def.type == "text" ? "textReplacement" : "pixelReplacement";
        if (def.type == "pixels") {
            kind["method"] = camel(def.method);
            kind["align"] = camel(def.align);
            kind["clip"] = def.clip;
        }
        defs.push_back(Json{{"name", def.name}, {"layer", std::stoull(id->second)}, {"kind", std::move(kind)}});
        written.insert(def.name);
    }
    if (defs.empty()) {
        if (left_defs) said("変数は .pcraft に書けない（台詞の変数など）ため外しました");
        return std::nullopt;
    }
    if (left_defs) said("台詞の変数と、このページに無いレイヤーの変数は .pcraft に書けないため外しました");
    Json sets = Json::array();
    bool pictures = false;
    for (const core::DataSet& set : variables.data_sets) {
        Json values = Json::array();
        for (const core::DataValue& value : set.values) {
            if (!written.contains(value.variable)) continue;
            if (const bool* shown = std::get_if<bool>(&value.value)) {
                values.push_back(Json{{"variable", value.variable}, {"kind", "visibility"}, {"value", *shown}});
            } else if (const std::string* words = std::get_if<std::string>(&value.value)) {
                values.push_back(Json{{"variable", value.variable}, {"kind", "text"}, {"value", *words}});
            } else {
                pictures = true;  // (a .pcraft names a file by its path: the book's picture is no file beside it)
            }
        }
        sets.push_back(Json{{"name", set.name}, {"values", std::move(values)}});
    }
    if (pictures) said("データセットの画像は .pcraft に入れられないため外しました（.pcraft はファイルのパスで指すため）");
    return Json{{"defs", std::move(defs)}, {"data_sets", std::move(sets)}, {"active", variables.active ? Json(*variables.active) : Json(nullptr)}};
}

}  // namespace genko::render::pcraft_variables
