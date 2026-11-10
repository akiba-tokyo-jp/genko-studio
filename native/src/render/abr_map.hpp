#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/abr.hpp"

// PC4 E2 (brush.presets.importAbr, assets:adobe-presets; SPEC.md COLOR-09「ブラシのプリセット」): a Photoshop brush file's
// brushes as Genko's dab brushes — PhotoCraft v0.2.0's crates/io/src/abr_map.rs (MIT: native/third_party/photocraft/
// NOTICE.md) onto brushes@1's settings (core/brush_dabs.hpp): ブラシ先端のシェイプ, シェイプ, 散布, テクスチャ, デュアルブラシ,
// カラー, 伝達, ブラシポーズ, ノイズ, ウェットエッジ, ビルドアップ and the tool options a preset keeps (不透明度, 流量, 筆圧). A
// setting with nothing to go to here is never dropped silently: `warnings` names it. The descriptors' keys follow the
// publicly described layout of version 6 and later (there is no published specification: upstream's note).
//
// Units: Photoshop's pixels are read at the working 200 dpi (as define_brush's "format": "photocraft": 20 pixels are
// 2.54 mm), a brush's size kept within 0.01..500 mm (its line's width within 0.05..50 mm: the brush panel's). A sampled
// tip larger than 2500 pixels a side (a texture's 1024) is made smaller to it; its picture is a grey PNG of 16 bits (an
// 8-bit one when it is the same), named by its ref — the library keeps it in brush_tips/, a book in its brush pictures.

namespace genko::render::abr_map {

// One brush of the file: its name and its settings as define_brush and the library take them ({"label", "width_mm",
// "engine": "dabs", "dabs": {…}} — the dabs naming their pictures by ref).
struct Preset {
    std::string name;
    core::Json settings;
};

struct Import {
    int version = 0;
    std::vector<Preset> presets;
    std::map<std::string, core::Bytes> pictures;  // (each picture the presets name, by its ref)
    std::vector<std::string> warnings;             // (in Japanese: what could not be read or has no place here)
};

// Photoshop pixels to mm (the working 200 dpi).
inline constexpr double kMmPerPixel = 25.4 / 200.0;

// The file's brushes mapped (abr::parse's errors: core::Error "format", "too_large").
Import read(std::string_view bytes);
Import map_file(const abr::File& file);

}  // namespace genko::render::abr_map
