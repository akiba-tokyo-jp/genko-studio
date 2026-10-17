// PC2b: the codecs of the colour kinds' PSD adjustment layers (hue2, vibA, blnc, blwh, phfl, mixr, selc; PhotoCraft's
// io/adjust_map.rs) for the table of render/psd_adjust.hpp. Not in this build yet: none filled.

#include "render/psd_adjust.hpp"

namespace genko::render::psd {

CodecHooks color_codecs() { return {}; }

}  // namespace genko::render::psd
