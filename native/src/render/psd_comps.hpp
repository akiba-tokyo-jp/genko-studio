#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/model.hpp"
#include "render/psd_production.hpp"

// PC5 J-a (layer-comps@1, SPEC.md PROD-01「レイヤーカンプ」, FILE-01「PSD・PSB」): a PSD's layer comps as the page's —
// PhotoCraft v0.2.0's crates/io/src/comps_map.rs (MIT, native/third_party/photocraft/NOTICE.md). The image resource 1065
// holds a CompList descriptor (its `list` of Comp objects — "Nm  " the name, compID, capturedInfo: 1 visibility, 2
// position, 4 appearance, `comment` — and lastAppliedComp); each layer's state in each comp is in its metadata block (shmd)
// under the key cmls: `layerSettings`, one object for a list of comp ids (compList; 0 the last document state) with `enab`
// (shown) and `Ofst` (its offset from where the layer is in the file, pixels). Photoshop keeps no appearance in a PSD (nor
// does PhotoCraft): a comp read has none of it recorded.
//
// Read (render/psd_production's row 1065): the comps onto the page — each layer's place its box's now and the offset (the
// file's pixels in mm) —, unless the page has comps already, holds an earlier PSD's blocks or only the merged picture was
// read. Written: the page's comps (the kept bytes while they are as read — the comps and the places of the layers they
// name the same —, its layers' kept shmd so too), each layer record's cmls (render/psd_production::layer_shmd) with its lyid.

namespace genko::render::psd_comps {

inline constexpr int kResource = 1065;

bool read(core::Page& page, std::string_view data, const psd_production::ReadContext& context);
std::optional<std::string> write(const core::Page& page, const psd_production::WriteContext& context);
std::string fingerprint(const core::Page& page, const core::Document* doc);
void write_layers(const core::Page& page, const psd_production::WriteContext& context, psd_production::LayerItems& out);

}  // namespace genko::render::psd_comps
