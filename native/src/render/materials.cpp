// Python's genko/materials/__init__.py: the built-in materials and the person's own library.

#include "render/materials.hpp"

#include <QFile>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <system_error>

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"
#include "render/zip.hpp"
#include "storage/fsutil.hpp"

// Q_INIT_RESOURCE declares the resource symbol in the current namespace; keep this function global.
static void initialize_material_catalog() { Q_INIT_RESOURCE(genko_materials); }

namespace genko::render::materials {

namespace fs = std::filesystem;
using core::Json;

namespace {

constexpr std::int64_t kMaxPictureBytes = 64ll << 20;  // (one picture read into the library or out of it)
constexpr std::int64_t kMaxListBytes = 16ll << 20;     // (library.json, folders.json, pack.json)

fs::path library_file(const fs::path& config_dir) { return library_dir(config_dir) / "library.json"; }
fs::path folders_file(const fs::path& config_dir) { return library_dir(config_dir) / "folders.json"; }

[[noreturn]] void unreadable(const fs::path& path) {
    throw core::PyUncaught("OSError", "the material library cannot be read, so it is left as it is: " + core::path_to_utf8(path));
}

// The library folder, when it may be read or written: missing, or a real folder (not a link, nor reached through one).
bool safe_folder(const fs::path& config_dir) {
    const fs::path dir = library_dir(config_dir);
    std::error_code ec;
    const auto status = fs::symlink_status(dir, ec);
    if (ec || status.type() == fs::file_type::not_found) return true;
    if (status.type() != fs::file_type::directory) return false;
    const fs::path canonical = fs::canonical(dir, ec);
    return !ec && canonical == fs::absolute(dir).lexically_normal();
}

void need_safe_folder(const fs::path& config_dir) {
    if (!safe_folder(config_dir)) {
        throw core::PyUncaught("OSError", "the material library folder is a link or not a folder, so it is left as it is: " +
                                              core::path_to_utf8(library_dir(config_dir)));
    }
}

// A JSON file of the library: nothing when it is not there; `strict`: refused when it is there but cannot be read
// (Python reads it as empty), else nothing.
std::optional<Json> read_json(const fs::path& path, bool strict) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found) return std::nullopt;  // (libstdc++ sets ec for a missing file too)
    const auto fail = [&]() -> std::optional<Json> {
        if (strict) unreadable(path);
        return std::nullopt;
    };
    if (ec || status.type() != fs::file_type::regular) return fail();
    const auto size = fs::file_size(path, ec);
    if (ec || static_cast<std::int64_t>(size) > kMaxListBytes) return fail();
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail();
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad() || core::utf8_error(text)) return fail();
    try {
        core::ParseRepairs repairs;
        Json value = core::parse_python_json(text, &repairs, core::ParseOptions{.universal_newlines = true});
        // (read as Python reads it, NaN as null; but a change would write it back otherwise: refused)
        if (strict && (!repairs.nonfinite.empty() || !repairs.inexact.empty())) return fail();
        return value;
    } catch (const core::Error&) {
        return fail();
    }
}

// The library's entries as they are kept: strictly (to change them), a list of objects.
Json read_entries(const fs::path& config_dir) {
    need_safe_folder(config_dir);
    const auto value = read_json(library_file(config_dir), true);
    if (!value) return Json::array();
    if (!value->is_array()) unreadable(library_file(config_dir));
    for (const Json& item : *value) {
        if (!item.is_object()) unreadable(library_file(config_dir));
    }
    Json out = Json::array();
    for (const Json& item : *value) out.push_back(normalize(item));
    return out;
}

void save(const fs::path& config_dir, const Json& items) {
    need_safe_folder(config_dir);
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    const std::string text = core::dump(items, options);
    try {
        storage::write_atomic(library_file(config_dir), text);
    } catch (const core::Error& error) {
        throw core::PyUncaught("OSError", "the material library cannot be written: " + core::path_to_utf8(library_file(config_dir)) +
                                              " (" + error.what() + ")");
    }
}

// The empty folders' list: lenient as Python reads it, or strictly.
std::vector<std::string> empty_folders(const fs::path& config_dir, bool strict) {
    if (strict) need_safe_folder(config_dir);
    else if (!safe_folder(config_dir)) return {};
    const auto value = read_json(folders_file(config_dir), strict);
    std::vector<std::string> out;
    if (!value) return out;
    if (value->is_array()) {
        for (const Json& name : *value) {
            if (name.is_string()) out.push_back(name.get<std::string>());
            else if (strict) unreadable(folders_file(config_dir));
        }
    } else if (value->is_object()) {
        for (const auto& [name, _] : value->items()) out.push_back(name);  // (list(a dict): its keys)
    } else if (strict) {
        unreadable(folders_file(config_dir));
    }
    return out;
}

// x.strip() for a name or a folder (AttributeError for one that is not text).
std::string stripped(const Json& value) {
    if (!value.is_string()) throw core::PyUncaught("AttributeError", "'" + core::py_type_name(value) + "' object has no attribute 'strip'");
    return core::py_strip(value.get_ref<const std::string&>());
}

// A plain file name inside the library (no folders, not "." or ".."): the only pictures read or deleted.
bool plain_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\") == std::string::npos &&
           !(name.size() >= 2 && name[1] == ':');
}

std::string read_picture(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec || status.type() == fs::file_type::not_found) {
        throw core::PyUncaught("FileNotFoundError", "[Errno 2] No such file or directory: " + core::py_repr_str(core::path_to_utf8(path)));
    }
    if (status.type() != fs::file_type::regular) throw core::PyValueError("the picture is a link or not a file: " + core::path_to_utf8(path));
    const auto size = fs::file_size(path, ec);
    if (ec || static_cast<std::int64_t>(size) > kMaxPictureBytes) throw core::PyValueError("the picture is too large (at most 64 MB): " + core::path_to_utf8(path));
    std::ifstream file(path, std::ios::binary);
    if (!file) throw core::PyUncaught("PermissionError", "[Errno 13] Permission denied: " + core::py_repr_str(core::path_to_utf8(path)));
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) throw core::PyUncaught("OSError", "the picture cannot be read: " + core::path_to_utf8(path));
    return bytes;
}

// PurePath.suffix and .stem of a file name.
std::pair<std::string, std::string> stem_suffix(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0 && dot < name.size() - 1) return {name.substr(0, dot), name.substr(dot)};
    return {name, ""};
}

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

// import_image for a picture already read: copied as an RGBA PNG, then registered.
Json import_picture(const fs::path& config_dir, const std::string& bytes, const Json& name, const std::string& fallback_name,
                    const Json& folder, const Json& width_mm) {
    const Image rgba = selection::open_picture(bytes).convert("RGBA");
    Json items = read_entries(config_dir);  // (a library that cannot be read is refused before anything is written)
    const Json given = core::py_truthy(name) ? name : Json(fallback_name);
    const std::string clean = stripped(given);
    if (clean.empty()) throw core::PyValueError("a material needs a name");
    std::string where = stripped(folder);
    if (where.empty()) where = "マイ素材";
    const double width = core::py_truthy(width_mm) ? core::py_float(width_mm) : 60.0;
    Json item = Json::object();
    item["id"] = "u-" + core::new_id();
    item["name"] = clean;
    item["folder"] = where;
    item["kind"] = "image";
    item["width_mm"] = width;
    const std::string file = item["id"].get<std::string>() + ".png";
    try {
        storage::write_atomic(library_dir(config_dir) / file, write_png(rgba));
    } catch (const core::Error& error) {
        throw core::PyUncaught("OSError", "the picture cannot be written into the material library (" + std::string(error.what()) + ")");
    }
    item["file"] = file;
    item["aspect"] = core::py_round(static_cast<double>(rgba.height()) / std::max(1, rgba.width()), 5);
    items.push_back(item);
    save(config_dir, items);
    return get_material(config_dir, item["id"].get<std::string>());
}

// The files of a pack, by their path inside it ("a/b.png"), and how to read one.
struct PackFiles {
    std::vector<std::string> names;
    std::function<std::optional<std::string>(const std::string&)> read;  // nothing: no such file
};

// The parts of a path inside a pack, as sorted(Path.rglob(…)) compares them.
std::vector<std::string> parts_of(const std::string& name) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = name.find('/', start);
        parts.push_back(name.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}

Json import_files(const fs::path& config_dir, const PackFiles& pack, const std::string& folder) {
    Json added = Json::array();
    if (const auto manifest = pack.read("pack.json")) {
        if (core::utf8_error(*manifest)) throw core::PyValueError(*core::utf8_error(*manifest));
        const Json entries = core::parse_python_json(*manifest, nullptr, core::ParseOptions{.universal_newlines = true});
        if (!entries.is_array()) return added;
        for (const Json& entry : entries) {
            const Json kind = core::py_get(entry, "kind");
            const auto& known = kinds();
            if (!kind.is_string() || std::find(known.begin(), known.end(), kind.get<std::string>()) == known.end() ||
                !core::py_truthy(core::py_get(entry, "name"))) {
                continue;
            }
            Json data = Json::object();
            for (const auto& [key, value] : entry.items()) {
                if (key != "id" && key != "name" && key != "folder" && key != "kind" && key != "file" && key != "builtin") data[key] = value;
            }
            const Json own = core::py_get(entry, "folder");
            const Json where = core::py_truthy(own) ? own : Json(folder);
            Json item;
            if (kind == "image") {
                const std::string file = core::py_str(core::py_truthy(core::py_get(entry, "file")) ? entry["file"] : Json(""));
                const auto picture = file.empty() || file.back() == '/' ? std::nullopt : pack.read(file);
                if (!picture) continue;  // (not a file in the pack)
                item = import_picture(config_dir, *picture, entry["name"], "", where, core::py_get(entry, "width_mm"));
                if (core::py_truthy(core::py_get(data, "tags"))) {
                    item = update_material(config_dir, item["id"].get<std::string>(), Json{{"tags", data["tags"]}});
                }
            } else {
                item = add_material(config_dir, entry["name"], kind.get<std::string>(), where, data);
            }
            added.push_back(item);
        }
        return added;
    }
    static const std::vector<std::string> pictures{".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif"};
    std::vector<std::string> names = pack.names;
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return parts_of(a) < parts_of(b); });
    for (const std::string& name : names) {
        const std::size_t slash = name.rfind('/');
        const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
        const auto [stem, suffix] = stem_suffix(base);
        if (std::find(pictures.begin(), pictures.end(), ascii_lower(suffix)) == pictures.end()) continue;
        const auto bytes = pack.read(name);
        if (!bytes) continue;
        const std::string where = slash == std::string::npos ? folder : folder + "/" + name.substr(0, slash);
        added.push_back(import_picture(config_dir, *bytes, Json(stem), stem, Json(where), Json()));
    }
    if (added.empty()) throw core::PyValueError("the pack has no materials (pack.json or pictures)");
    return added;
}

}  // namespace

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> all{"tone", "effect", "image", "lines", "lettering", "brush", "prim"};
    return all;
}

std::string kind_words(const std::string& kind) {
    static const std::map<std::string, std::string> words{{"tone", "トーン"}, {"effect", "効果線"}, {"image", "画像"},
                                                          {"lines", "パーツ 線"}, {"lettering", "描き文字 効果音"},
                                                          {"brush", "ブラシ"}, {"prim", "3d 立体"}};
    const auto found = words.find(kind);
    return found == words.end() ? std::string() : found->second;
}

const Json& catalog() {
    static const Json items = [] {
        initialize_material_catalog();
        QFile file(QStringLiteral(":/genko/materials/catalog.json"));
        if (!file.open(QIODevice::ReadOnly)) throw core::Error("io", "cannot read built-in materials");
        Json all = core::parse_python_json(file.readAll().toStdString());
        for (Json& item : all) {
            if (item.is_object()) item.erase("preview");  // (the picture packaged for the panel is not part of the material)
        }
        return all;
    }();
    return items;
}

fs::path library_dir(const fs::path& config_dir) { return config_dir / "materials"; }

Json normalize(const Json& item) {
    if (!item.is_object()) return item;
    const Json kind = core::py_get(item, "kind");
    if (kind == "dot" || kind == "noise") {
        Json tone = Json::object();
        tone["pattern"] = kind == "noise" ? "noise" : "dot";
        tone["density"] = item.contains("density") ? item["density"] : Json(0.3);
        if (core::py_truthy(core::py_get(item, "lpi"))) tone["lpi"] = item["lpi"];
        Json out = item;
        out["kind"] = "tone";
        out["tone"] = tone;
        return out;
    }
    if (kind == "effect") {
        Json out = Json::object();
        out["params"] = Json::object();
        for (const auto& [key, value] : item.items()) out[key] = value;
        out["effect"] = item.contains("effect") ? item["effect"] : Json("speed");
        return out;
    }
    return item;
}

Json user_materials(const fs::path& config_dir) {
    if (!safe_folder(config_dir)) return Json::array();
    const auto value = read_json(library_file(config_dir), false);
    Json out = Json::array();
    if (!value || !value->is_array()) return out;
    for (const Json& item : *value) {
        if (item.is_object()) out.push_back(normalize(item));
    }
    return out;
}

Json all_materials(const fs::path& config_dir) {
    Json out = catalog();
    for (const Json& item : user_materials(config_dir)) out.push_back(item);
    return out;
}

std::vector<std::string> folders(const fs::path& config_dir) {
    std::vector<std::string> seen;
    const auto add = [&](const std::string& name) {
        if (std::find(seen.begin(), seen.end(), name) == seen.end()) seen.push_back(name);
    };
    for (const Json& item : all_materials(config_dir)) {
        const Json folder = core::py_get(item, "folder");
        add(core::py_truthy(folder) ? core::py_str(folder) : std::string("その他"));
    }
    for (const std::string& extra : empty_folders(config_dir, false)) add(extra);
    return seen;
}

void add_folder(const fs::path& config_dir, const std::string& name) {
    const std::string clean = core::py_strip(name);
    if (clean.empty()) throw core::PyValueError("a folder needs a name");
    std::vector<std::string> names = empty_folders(config_dir, true);
    const std::vector<std::string> all = folders(config_dir);
    if (std::find(names.begin(), names.end(), clean) != names.end() || std::find(all.begin(), all.end(), clean) != all.end()) return;
    Json list = Json::array();
    for (const std::string& n : names) list.push_back(n);
    list.push_back(clean);
    try {
        storage::write_atomic(folders_file(config_dir), core::dump_python(list));
    } catch (const core::Error& error) {
        throw core::PyUncaught("OSError", "the material folders cannot be written: " + core::path_to_utf8(folders_file(config_dir)) + " (" +
                                              error.what() + ")");
    }
}

Json get_material(const fs::path& config_dir, const std::string& material_id) {
    for (const Json& item : all_materials(config_dir)) {
        if (item.is_object() && item.contains("id") && item["id"] == material_id) return item;
    }
    throw core::PyUncaught("KeyError", core::py_repr_str(material_id));
}

Json add_material(const fs::path& config_dir, const Json& name, const std::string& kind, const Json& folder, const Json& data) {
    const auto& known = kinds();
    if (std::find(known.begin(), known.end(), kind) == known.end()) {
        throw core::PyValueError("kind must be one of tone, effect, image, lines, lettering, brush, prim");
    }
    const std::string clean = stripped(name);
    if (clean.empty()) throw core::PyValueError("a material needs a name");
    std::string where = stripped(folder);
    if (where.empty()) where = "マイ素材";
    Json items = read_entries(config_dir);
    Json item = Json::object();
    item["id"] = "u-" + core::new_id();
    item["name"] = clean;
    item["folder"] = where;
    item["kind"] = kind;
    for (const auto& [key, value] : data.items()) item[key] = value;
    items.push_back(item);
    save(config_dir, items);
    return item;
}

Json import_image(const fs::path& config_dir, const fs::path& path, const Json& name, const Json& folder, const Json& width_mm) {
    const std::string bytes = read_picture(path);
    return import_picture(config_dir, bytes, name, stem_suffix(core::path_to_utf8(path.filename())).first, folder, width_mm);
}

Json update_material(const fs::path& config_dir, const std::string& material_id, const Json& change) {
    Json items = read_entries(config_dir);
    for (Json& entry : items) {
        if (!(entry.contains("id") && entry["id"] == material_id)) continue;
        for (const char* key : {"name", "folder"}) {
            if (change.contains(key)) entry[key] = change[key];
        }
        if (change.contains("tags")) {
            Json tags = Json::array();
            if (core::py_truthy(change["tags"])) {
                for (const Json& tag : core::py_list(change["tags"])) {
                    const std::string word = core::py_strip(core::py_str(tag));
                    if (!word.empty()) tags.push_back(word);
                }
            }
            entry["tags"] = tags;
        }
        save(config_dir, items);
        return entry;
    }
    throw core::PyUncaught("KeyError", core::py_repr_str(material_id));
}

void delete_material(const fs::path& config_dir, const std::string& material_id) {
    const Json items = read_entries(config_dir);
    Json kept = Json::array();
    std::optional<Json> gone;
    for (const Json& item : items) {
        if (item.contains("id") && item["id"] == material_id) {
            if (!gone) gone = item;
        } else {
            kept.push_back(item);
        }
    }
    if (!gone) {
        for (const Json& item : catalog()) {
            if (item.contains("id") && item["id"] == material_id) throw core::PyValueError("built-in materials cannot be deleted");
        }
        throw core::PyUncaught("KeyError", core::py_repr_str(material_id));
    }
    save(config_dir, kept);  // (the entry goes first: a picture left behind is only a file, never a broken entry)
    const Json file = core::py_get(*gone, "file");
    if (core::py_truthy(file) && file.is_string() && plain_name(file.get<std::string>())) {
        const fs::path picture = library_dir(config_dir) / core::path_from_utf8(file.get<std::string>());
        std::error_code ec;
        const auto status = fs::symlink_status(picture, ec);
        if (!ec && (status.type() == fs::file_type::regular || status.type() == fs::file_type::symlink)) fs::remove(picture, ec);
    }
}

std::optional<std::string> image_bytes(const fs::path& config_dir, const Json& item) {
    const Json file = core::py_get(item, "file");
    if (core::py_get(item, "kind") != "image" || !core::py_truthy(file)) return std::nullopt;
    if (!file.is_string() || !plain_name(file.get<std::string>()) || !safe_folder(config_dir)) return std::nullopt;
    const fs::path path = library_dir(config_dir) / core::path_from_utf8(file.get<std::string>());
    std::error_code ec;
    if (fs::symlink_status(path, ec).type() != fs::file_type::regular) return std::nullopt;
    return read_picture(path);
}

Json import_pack(const fs::path& config_dir, const fs::path& path, const std::optional<std::string>& folder) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    const std::string file_name = core::path_to_utf8(path.filename());
    const auto [stem, suffix] = stem_suffix(file_name);
    if (!ec && status.type() == fs::file_type::regular && ascii_lower(suffix) == ".zip") {
        auto files = std::make_shared<std::map<std::string, std::string>>(zip::read(path));
        PackFiles pack;
        for (const auto& [name, _] : *files) pack.names.push_back(name);
        pack.read = [files](const std::string& name) -> std::optional<std::string> {
            const auto found = files->find(name);
            if (found == files->end()) return std::nullopt;
            return found->second;
        };
        return import_files(config_dir, pack, folder && !folder->empty() ? *folder : stem);
    }
    if (ec || status.type() != fs::file_type::directory) throw core::PyValueError("a material pack is a folder or a .zip");
    PackFiles pack;
    std::size_t count = 0;
    for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (++count > 10000) throw core::PyValueError("the pack holds too many files");
        const auto type = it->symlink_status(ec).type();
        if (type != fs::file_type::regular) continue;  // (links are not followed out of the pack)
        std::string name = core::path_to_utf8(it->path().lexically_relative(path));
#ifdef _WIN32
        std::replace(name.begin(), name.end(), '\\', '/');
#endif
        pack.names.push_back(std::move(name));
    }
    if (ec) throw core::PyUncaught("OSError", "the pack cannot be read: " + core::path_to_utf8(path) + " (" + ec.message() + ")");
    pack.read = [root = path](const std::string& name) -> std::optional<std::string> {
        if (name.empty()) return std::nullopt;
        for (const std::string& part : parts_of(name)) {
            if (part == ".." || part.empty()) return std::nullopt;
        }
        const fs::path file = root / core::path_from_utf8(name);
        std::error_code missing;
        if (fs::symlink_status(file, missing).type() != fs::file_type::regular) return std::nullopt;
        return read_picture(file);
    };
    return import_files(config_dir, pack, folder && !folder->empty() ? *folder : file_name);
}

fs::path export_pack(const fs::path& config_dir, const std::vector<std::string>& material_ids, const fs::path& path) {
    std::vector<std::pair<std::string, std::string>> files;
    Json entries = Json::array();
    for (const std::string& material_id : material_ids) {
        Json item = get_material(config_dir, material_id);
        item.erase("builtin");
        if (core::py_get(item, "kind") == "image") {
            const auto data = image_bytes(config_dir, item);
            if (!data || data->empty()) continue;
            const std::string name = core::py_str(item["id"]) + ".png";
            files.emplace_back(name, *data);
            item["file"] = name;
        }
        entries.push_back(item);
    }
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    files.emplace_back("pack.json", core::dump(entries, options));
    zip::write(path, files);
    return path;
}

}  // namespace genko::render::materials
