#include <algorithm>
#include "core/color_raster.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"

namespace genko::core {
void register_color_ops(OpRegistry& registry) {
    registry.add("put_color_raster", [](OpContext& ctx) {
        const auto pi = require_page(ctx.doc, ctx.op);
        const auto& before = ctx.doc.page(pi);
        if (gated(ctx.doc) && !before.name_ok) throw OpError("put_color_raster needs name_ok (strict_gates)");
        const auto raw = encode_color_raster(ctx.op);
        std::size_t size = raw.size();
        for (const auto& p : ctx.doc.pages) for (const auto& layer : p->layers)
            if (layer.color_raster) {
                if (layer.color_raster->size() > kColorRasterBookBytes - size) throw OpError("color raster book budget exceeded");
                size += layer.color_raster->size();
            }
        Layer layer;
        layer.id = new_id();
        layer.role = LayerRole::User;
        layer.kind = LayerKind::Raster;
        layer.title = "カラー素材";
        layer.panel_clip = false;
        layer.color_prints = true;
        layer.color_raster = std::make_shared<const std::string>(raw);
        ctx.doc.edit_page(pi).layers.push_back(std::move(layer));
        if (std::find(ctx.doc.features.begin(), ctx.doc.features.end(), kColorRasterFeature) == ctx.doc.features.end())
            ctx.doc.features.emplace_back(kColorRasterFeature);
    });
}
} // namespace genko::core
