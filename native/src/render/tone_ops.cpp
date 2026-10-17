// PC2c (局所の補正): the local corrections of filter_raster — shadows_highlights, replace_color, match_color — and the op
// hdr_toning (design 3.4; render/ops_adjust.hpp). Not in this build yet: hdr_toning is refused and no local filter is
// filled (each is refused where it would be used, not_yet_ported naming it).

#include "render/ops_adjust.hpp"

namespace genko::render::adjust_ops {

void hdr_toning(core::OpContext&) { core::adjust_not_yet("the op hdr_toning", "PC2c"); }

LocalFilters tone_local_filters() { return {}; }

}  // namespace genko::render::adjust_ops
