#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "formats/copy.hpp"

// ファイル → 生成 → 画像アセット (file.generate.imageAssets; PC5 I-f, SPEC.md AUTO-01「画像アセット」; PhotoCraft v0.2.0's
// engine/src/web_cmds.rs generate_assets and assets_dir, MIT): each layer — or group — of the book named as files
// (core::image_assets::parse_name: "logo.png", "200% logo@2x.png", "48x48 icons/a.png8", "photo.jpg80%") written into the
// book's assets folder, `<the book's name>-assets` beside its folder, as its pixels alone over nothing, cut to what it
// holds (a layer: formats::write_copy's layer alone; a group: its layers drawn alone, render_contents), sized as its name
// asks — and again for each variant of a "default …" layer —, its colours sRGB, no metadata. Never written: inside a book,
// over a picture a book was opened from (formats::check_target). The window writes them after each save the person makes
// (never the automatic save); `genko export <book> <folder> --format image-assets` writes them at once.

namespace genko::formats::image_assets {

// <the book's folder's parent>/<its name without .genko>-assets.
std::filesystem::path assets_dir(const std::filesystem::path& book);

struct Result {
    std::filesystem::path dir;
    std::vector<std::filesystem::path> files;  // written, in the order of the pages and their layers
    core::Json errors = core::Json::array();   // {"page", "layer", "asset", "error"}: one that could not be written
    std::vector<std::string> notes;            // what a file could not keep (write_copy's notes), each once
    // {"dir", "files", "errors", "notes"}
    core::Json json() const;
};

// Every asset of `doc` written into `dir` (made when there is one to write). A page not read yet is core::Error
// ("page_not_loaded") before anything is written; one asset that fails is said in `errors` and the others written.
Result generate(const core::Document& doc, const std::filesystem::path& dir, const WriteGuard& guard);

// The layers that name assets (their count): 生成 → 画像アセット says when none does.
std::size_t named_layers(const core::Document& doc);

}  // namespace genko::formats::image_assets
