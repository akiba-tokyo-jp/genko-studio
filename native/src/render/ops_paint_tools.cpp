// PC4 節 E (SPEC.md COLOR-08): the ops of the brushes that change pixels — registered here (E-0); E5 writes their work in
// this file: each a stroke of `points` (mm) on a paint layer's own grid with brushwork's diameter, hardness, opacity, flow
// and spacing (render/ops_retouch's), laid dab by dab on the layer's tiles (render/tiled_pixels,
// retouch::apply_dab_stroke) in the layer's own values on 8-bit, precise and ink layers.

#include <string>

#include "core/pyops.hpp"
#include "render/ops_paint_tools.hpp"

namespace genko::render {

core::OpFunction not_yet_pc4(const char* op, const char* group) {
    const std::string message = std::string(op) + " is not in the C++ build yet (PC4 " + group + ")";
    return [message](core::OpContext&) { core::not_yet_ported(message); };
}

void register_paint_tool_ops(core::OpRegistry& registry) {
    // E5: Dodge and Burn ({page, layer_id, points, width_mm, hardness, opacity, flow, spacing, area?, brush?, range,
    // exposure, protect_tones}), Sponge ({…, mode: desaturate | saturate, vibrance}), Blur and Sharpen ({…, strength,
    // protect_detail}), Smudge ({…, strength, finger_painting, rgb?}), the Mixer Brush ({…, wet, load, mix, flow, rgb,
    // reservoir?, sample_all, clean_after, load_after} → {"reservoir"}), the Background Eraser ({…, sampling, limits,
    // tolerance, protect_foreground, foreground_rgb?, swatch_rgb?}) and Color Replacement ({…, mode, sampling, limits,
    // tolerance, antialias, rgb})
    for (const char* op : {"dodge", "burn", "sponge", "blur_brush", "sharpen_brush", "smudge_brush", "mixer_brush", "background_eraser",
                           "color_replacement"}) {
        registry.add(op, not_yet_pc4(op, "E5"));
    }
}

}  // namespace genko::render
