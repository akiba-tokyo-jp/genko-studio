#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"

// Materials (素材; Python's genko/materials/__init__.py): the built-in ones packaged with Genko (catalog.json) and
// the person's own library in the config folder (materials/library.json and its pictures, materials/folders.json),
// shared with the Python app and written as it writes them (json.dumps(…, ensure_ascii=False, indent=1)).
//
// Beyond Python, so a library is never lost or reached outside: every change reads the library strictly and refuses
// (PyUncaught OSError "the material library cannot be read, so it is left as it is: <path>") where Python would read
// an empty one and write over it; files are written whole or not at all; the library folder must be a real folder
// (not a link); a picture is read or deleted only by a plain name inside it; packs are read within zip::Limits and
// their links are not followed.

namespace genko::render::materials {

// KINDS, in Python's order.
const std::vector<std::string>& kinds();

// KIND_WORDS: the words a search finds a kind by.
std::string kind_words(const std::string& kind);

// load_catalog(): the built-in materials ("builtin": true; each with the "preview" picture packaged for it).
const core::Json& catalog();

// library_dir(): <config>/materials.
std::filesystem::path library_dir(const std::filesystem::path& config_dir);

// normalize(item): old catalog kinds ("dot", "noise") as tones, an effect with its "params" and "effect".
core::Json normalize(const core::Json& item);

// user_materials(): the library's entries, normalized; nothing for a library that is not there or cannot be read.
core::Json user_materials(const std::filesystem::path& config_dir);

// all_materials(): the built-in ones, then the person's.
core::Json all_materials(const std::filesystem::path& config_dir);

// folders(): every material's folder ("その他" for none), in order, then the empty folders made with add_folder.
std::vector<std::string> folders(const std::filesystem::path& config_dir);

// The empty folders made with add_folder (folders.json), as read to be listed (nothing when it cannot be read).
std::vector<std::string> empty_folders(const std::filesystem::path& config_dir);

// add_folder(name): PyValueError "a folder needs a name" for a blank one.
void add_folder(const std::filesystem::path& config_dir, const std::string& name);

// get_material(id): PyUncaught KeyError when there is none.
core::Json get_material(const std::filesystem::path& config_dir, const std::string& material_id);

// add_material(name, kind, folder, **data): the new entry (its id "u-…").
core::Json add_material(const std::filesystem::path& config_dir, const core::Json& name, const std::string& kind,
                        const core::Json& folder, const core::Json& data = core::Json::object());

// import_image(path, name, folder, width_mm): the picture copied into the library as an RGBA PNG.
core::Json import_image(const std::filesystem::path& config_dir, const std::filesystem::path& path,
                        const core::Json& name = core::Json(), const core::Json& folder = core::Json("画像"),
                        const core::Json& width_mm = core::Json());

// update_material(id, name=…, folder=…, tags=…): only the keys `change` holds.
core::Json update_material(const std::filesystem::path& config_dir, const std::string& material_id, const core::Json& change);

// delete_material(id): PyValueError "built-in materials cannot be deleted", PyUncaught KeyError for none.
void delete_material(const std::filesystem::path& config_dir, const std::string& material_id);

// image_bytes(item): a picture material's PNG, or nothing.
std::optional<std::string> image_bytes(const std::filesystem::path& config_dir, const core::Json& item);

// import_pack(path, folder): the materials of a pack (a folder or a .zip holding pack.json and its pictures, or just
// pictures, each sub-folder a folder).
core::Json import_pack(const std::filesystem::path& config_dir, const std::filesystem::path& path,
                       const std::optional<std::string>& folder = std::nullopt);

// export_pack(ids, path): the materials as a .zip pack (each picture, then pack.json).
std::filesystem::path export_pack(const std::filesystem::path& config_dir, const std::vector<std::string>& material_ids,
                                  const std::filesystem::path& path);

}  // namespace genko::render::materials
