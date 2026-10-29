#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// layer-comps@1 (PC5 節 J、SPEC.md PROD-01「レイヤーカンプ」・schema-v4.md「制作の補助」): a page's layer comps (Photoshop's
// ウィンドウ → レイヤーカンプ: Photoshop's document is Genko's page). The page's `layer_comps`:
//   {"comps": [{"id": 1..2^31-1, "name": str (1..255 characters), "comment": str,
//               "apply": {"visibility": bool, "position": bool, "appearance": bool},
//               "layers": [{"layer": layer id, "visible"?: bool, "position"?: [x_mm, y_mm] | null,
//                           "appearance"?: {"blend": str, "opacity": 0..1, "style": layer style | null}}]}],
//    "last_applied": id | null,
//    "last_state": {"layers": […the same…]} | null}        (最後のドキュメントの状態: before the first comp was applied)
// at most kLayerCompsMax comps. A layer's position is the top left of its content's bounds (render/layer_bounds, mm; null:
// a layer without them — a correction, a fill, an empty layer, a plain folder). The shapes after PhotoCraft v0.2.0's
// doc/src/comps.rs (MIT). The book names layer-comps@1 while a page has one (an empty one is not written). PC5 J-0 is the
// frame: the types, their JSON and checks; capturing and applying them, their ops and panel are J-a's
// (render/ops_layer_comps, app/layer_comps_panel, render/psd_production 1065 and the layers' cmls).

namespace genko::core {

struct Document;

inline constexpr std::string_view kLayerCompsFeature = "layer-comps@1";
inline constexpr std::string_view kLayerCompsKey = "layer_comps";
inline constexpr std::size_t kLayerCompsMax = 256;
inline constexpr std::size_t kLayerCompNameMax = 255;   // characters
inline constexpr std::size_t kLayerCompTextMax = 65536;  // bytes of a comment
inline constexpr std::int64_t kLayerCompIdMax = 2147483647;

struct CompAppearance {
    std::string blend = "normal";
    double opacity = 1.0;
    std::optional<Json> style;  // (layer-styles@1's style, as a layer holds it)

    bool operator==(const CompAppearance&) const = default;
};

struct CompLayerState {
    std::string layer;
    std::optional<bool> visible;                         // (none: not recorded)
    bool has_position = false;                           // (false: not recorded)
    std::optional<std::array<double, 2>> position;       // (recorded, none: the layer has no bounds)
    std::optional<CompAppearance> appearance;            // (none: not recorded)

    bool operator==(const CompLayerState&) const = default;
};

struct LayerComp {
    std::int64_t id = 1;
    std::string name;
    std::string comment;
    bool apply_visibility = true;
    bool apply_position = true;
    bool apply_appearance = true;
    std::vector<CompLayerState> layers;

    bool operator==(const LayerComp&) const = default;
};

struct LayerComps {
    std::vector<LayerComp> comps;
    std::optional<std::int64_t> last_applied;
    std::optional<std::vector<CompLayerState>> last_state;

    bool empty() const { return comps.empty() && !last_applied && !last_state; }
    bool operator==(const LayerComps&) const = default;
};

Json to_json(const LayerComps& comps);
// core::Error("value") says what is not so.
LayerComps layer_comps_from_json(const Json& value);
void check_layer_comps(const LayerComps& comps);

bool uses_layer_comps(const Document& doc);
void validate_layer_comps(const Document& doc);

}  // namespace genko::core
