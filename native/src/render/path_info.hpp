#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/json.hpp"
#include "core/model.hpp"

// vector@1 (PC3 C-a, SPEC.md COLOR-07「パスを調べる・書き出す」): what `genko inspect --paths` prints (path.list,
// path.info, layer.vectorMask.info, shape.info) and Paths to Illustrator (file.export.pathsToIllustrator: PhotoCraft
// v0.2.0's engine/src/print_cmds.rs paths_to_ai in mm, MIT — native/third_party/photocraft/NOTICE.md).

namespace genko::render::path_info {

// A page's paths: {"page", "paths": [{"name", "path", "subpaths", "knots"}], "work_path": {"path", "subpaths", "knots"}
// | null, "clipping_path": {"name", "flatness"} | null, "layers": [{"id", "title", "vector_mask"?: {…, "subpaths",
// "knots"}, "shapes"?: [{"id", "name"?, "path", "subpaths", "knots", "bounds": [x0, y0, x1, y1]}]}] (the layers with
// either)}.
core::Json page_paths(const core::Page& page);
// The book's: {"pages": [page_paths of each page read that holds any]}; one page: page_paths of it (core::Error("key")
// for no such page).
core::Json book_paths(const core::Document& doc, std::optional<std::int64_t> page = std::nullopt);

// The page's paths as an Adobe Illustrator (3) file — PostScript paths in points, y up, unpainted, a compound path for a
// path of several subpaths, each named by a %%Note: — of `which`: "all" (the saved paths, then the work path), "work",
// or a saved path's name. `count`: how many paths it holds (0: none matched).
std::string illustrator(const core::Document& doc, const core::Page& page, std::string_view which, int* count = nullptr);

}  // namespace genko::render::path_info
