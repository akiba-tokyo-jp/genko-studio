#include "render/ops_registry.hpp"

#include <algorithm>

#include "core/command_bus.hpp"
#include "core/ops_util.hpp"
#include "core/pyops.hpp"
#include "core/studio.hpp"
#include "render/bookpages.hpp"
#include "render/color_edit.hpp"
#include "render/ink_edit.hpp"
#include "render/ops_3d.hpp"
#include "render/paper.hpp"
#include "render/placed.hpp"
#include "render/raster.hpp"
#include "render/raster_ops.hpp"

namespace genko::render {

namespace {

// The CMYK and Lab pages an op may work on: the page it names (as it finds it: 1, 1.0 and true alike), or every one
// when it names none (a page it cannot find: none — the op itself says why).
std::vector<std::size_t> ink_pages_of(const core::OpContext& c) {
    std::vector<std::size_t> out;
    const core::Json* page = core::get(c.op, "page");
    if (page == nullptr || page->is_null()) {
        for (std::size_t i = 0; i < c.doc.pages.size(); ++i) {
            if (!c.doc.is_deferred(i) && c.doc.page(i).color) out.push_back(i);
        }
        return out;
    }
    try {
        const std::size_t at = core::require_page(c.doc, c.op);
        if (c.doc.page(at).color) out.push_back(at);
    } catch (const std::exception&) {
    }
    return out;
}

}  // namespace

void register_render_ops(core::OpRegistry& registry) {
    register_tone_ops(registry);
    register_effect_ops(registry);
    register_3d_ops(registry);
    register_raster_ops(registry);
    register_paint_ops(registry);
    // the line ops again, now able to look at a picture in a line's style (core's refuse it with not_yet_ported)
    core::register_line_ops(registry, verify_style_picture);
    // set_page_spec and import_pages again, now able to move a paint layer's pixels and to read the other book
    render::register_bookpage_ops(registry);
    // import_psd (fileops: a PSD or PSB read as layers, render/psd)
    register_file_ops(registry);
    // the colour modes of pages (color-mode@1: CMYK and Lab, render/ops_ink)
    register_ink_ops(registry);
    // the studio ops again, place_asset and set_placement able to read an asset's picture size, and those placing a
    // picture able to see it can be drawn (render/placed)
    core::register_studio_ops(registry, core::StudioHooks{asset_picture_size, check_asset_picture});
    // add_stroke again, able to mix the colour under the line in (下地混色・色延び, render/raster)
    core::register_add_stroke(registry, raster::mixed_pieces);
    // define_brush again, able to take a paper's picture in (BRUSH-01, render/paper)
    core::register_define_brush(registry, [](std::string_view file) {
        try {
            return paper::take_in(file);
        } catch (const core::Error& error) {
            throw core::OpError(error.what());
        }
    });
}

const core::OpRegistry& ops_registry() {
    static const core::OpRegistry registry = [] {
        core::OpRegistry r;
        core::register_core_ops(r);
        register_render_ops(r);
        // a precise paint layer takes what an op drew on it into its pixels (render/color_edit), a paint layer of a CMYK
        // or Lab page into its inks (render/ink_edit)
        for (const std::string& name : r.names()) {
            const core::OpFunction op = *r.find(name);
            r.add(name, [op, name](core::OpContext& c) {
                // (color-mode@1: an op that draws new colours on a CMYK or Lab page works the pictures the ink layers
                // it works on show, then puts what it left back into inks, render/ink_edit; the ops that only take,
                // move or clear pixels work the inks themselves, render/raster_ops)
                if (ink_edit::lends(name, c.op)) {
                    if (const std::vector<std::size_t> pages = ink_pages_of(c); !pages.empty()) {
                        std::vector<ink_edit::Lending> lent;
                        lent.reserve(pages.size());
                        for (const std::size_t at : pages) lent.emplace_back(c.doc, at, name, c.op);
                        op(c);
                        for (const ink_edit::Lending& each : lent) each.take_back(c.doc);
                        color_edit::bake_marks(c.doc);
                        ink_edit::bake_marks(c.doc);
                        return;
                    }
                }
                op(c);
                color_edit::bake_marks(c.doc);
                ink_edit::bake_marks(c.doc);
            });
        }
        return r;
    }();
    return registry;
}

}  // namespace genko::render
