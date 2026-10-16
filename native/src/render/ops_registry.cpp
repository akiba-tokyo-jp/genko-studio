#include "render/ops_registry.hpp"

#include <algorithm>

#include "core/channels.hpp"
#include "core/command_bus.hpp"
#include "core/ops_util.hpp"
#include "core/pyops.hpp"
#include "core/studio.hpp"
#include "render/bookpages.hpp"
#include "render/channels.hpp"
#include "render/color_edit.hpp"
#include "render/ink_edit.hpp"
#include "render/ops_3d.hpp"
#include "render/paper.hpp"
#include "render/placed.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/raster_ops.hpp"

namespace genko::render {

namespace {

// The RGB pages of a profile an op may work on (as ink_pages_of).
std::vector<std::size_t> profiled_pages_of(const core::OpContext& c) {
    const auto profiled = [&](std::size_t i) { return !c.doc.is_deferred(i) && c.doc.page(i).rgb && !c.doc.page(i).rgb->profile.empty(); };
    std::vector<std::size_t> out;
    const core::Json* page = core::get(c.op, "page");
    if (page == nullptr || page->is_null()) {
        for (std::size_t i = 0; i < c.doc.pages.size(); ++i) {
            if (profiled(i)) out.push_back(i);
        }
        return out;
    }
    try {
        const std::size_t at = core::require_page(c.doc, c.op);
        if (profiled(at)) out.push_back(at);
    } catch (const std::exception&) {
    }
    return out;
}

// The ops that bring new colours (sRGB) onto a paint layer's pixels: those lending ink layers their pictures, and the
// pictures laid down.
bool brings_colours(std::string_view op, const core::Json& args) {
    return ink_edit::lends(op, args) || op == "paste" || op == "stamp_material" || op == "fill_area" || op == "fill" || op == "fill_enclosed";
}

// color-model@1 (SPEC.md COLOR-03): an op that brings new colours onto an RGB page of a profile works its 8-bit paint
// layers as the sRGB they show, then puts what it changed back into the page's values (sRGB → the page, by its intent
// and black point compensation); a pixel it left as it was keeps its value, a layer it left keeps its bytes.
class RgbLending {
public:
    RgbLending(core::Document& doc, std::size_t at) {
        const core::Page& seen = doc.page(at);
        page_id_ = seen.id;
        mode_ = *seen.rgb;
        core::Page& page = doc.edit_page(at);
        for (core::Layer& layer : page.layers) {
            if (layer.kind != core::LayerKind::Raster || !layer.raster_png || layer.raster_png->empty()) continue;
            Loan loan;
            loan.layer = layer.id;
            loan.png = layer.raster_png;
            const Image values = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
            const Image shown = convert(values, mode_.icc, core::RgbMode{});
            loan.shown = shown.tobytes();
            loan.lent = std::make_shared<const std::string>(write_png(shown, 1));
            layer.raster_png = loan.lent;
            layer.raster_memo = {};
            loans_.push_back(std::move(loan));
        }
    }

    void take_back(core::Document& doc) const {
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            if (doc.is_deferred(i) || doc.page(i).id != page_id_ || !doc.page(i).rgb || doc.page(i).rgb->profile != mode_.profile) continue;
            core::Page& page = doc.edit_page(i);
            for (core::Layer& layer : page.layers) {
                if (!layer.raster_png || layer.raster_png->empty()) continue;
                const auto loan = std::find_if(loans_.begin(), loans_.end(), [&](const Loan& l) { return l.layer == layer.id; });
                if (loan != loans_.end() && (layer.raster_png == loan->lent || *layer.raster_png == *loan->lent)) {
                    layer.raster_png = loan->png;  // (as it was lent: its own bytes)
                    layer.raster_memo = {};
                    continue;
                }
                const Image now = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
                Image values = convert(now, nullptr, mode_);
                if (loan != loans_.end()) {  // (what the op left as it was: its own value)
                    const Image before = open_image(*loan->png, kPillowOpenLimits).convert("RGBA");
                    if (before.size() == now.size()) {
                        const std::string was = loan->shown, kept = before.tobytes(), seen = now.tobytes();
                        std::string made = values.tobytes();
                        for (std::size_t k = 0; k + 3 < made.size(); k += 4) {
                            if (std::equal(seen.begin() + std::ptrdiff_t(k), seen.begin() + std::ptrdiff_t(k + 4), was.begin() + std::ptrdiff_t(k))) {
                                std::copy_n(kept.begin() + std::ptrdiff_t(k), 4, made.begin() + std::ptrdiff_t(k));
                            }
                        }
                        values = Image::frombytes("RGBA", now.size(), made);
                    }
                }
                layer.raster_png = std::make_shared<const std::string>(write_png(values));
                layer.raster_memo = {};
            }
        }
    }

private:
    // An 8-bit RGBA picture from one RGB profile (null: sRGB) into another's (its intent and black point compensation).
    static Image convert(const Image& rgba, const core::Bytes& from, const core::RgbMode& to) {
        std::string bytes = rgba.tobytes();
        std::vector<float> values(bytes.size());
        for (std::size_t k = 0; k < bytes.size(); ++k) values[k] = static_cast<unsigned char>(bytes[k]) / 255.0f;
        ink::rgb_to_rgb(values, from, to);
        for (std::size_t k = 0; k < bytes.size(); ++k) {
            bytes[k] = static_cast<char>(std::lround(std::clamp(values[k], 0.0f, 1.0f) * 255.0f));
        }
        return Image::frombytes("RGBA", rgba.size(), bytes);
    }

    struct Loan {
        std::string layer;
        core::Bytes png;    // (its own: the page's values)
        core::Bytes lent;   // (the sRGB it was lent)
        std::string shown;  // (that picture's RGBA bytes)
    };
    std::string page_id_;
    core::RgbMode mode_;
    std::vector<Loan> loans_;
};

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
    // a page's alpha and spot channels (channels@1, render/ops_channels)
    register_channel_ops(registry);
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
            const core::OpFunction plain = *r.find(name);
            const core::OpFunction op = [plain, name](core::OpContext& c) {
                // (color-mode@1: an op that draws new colours on a CMYK or Lab page works the pictures the ink layers
                // it works on show, then puts what it left back into inks, render/ink_edit; the ops that only take,
                // move or clear pixels work the inks themselves, render/raster_ops)
                if (brings_colours(name, c.op)) {
                    if (const std::vector<std::size_t> pages = profiled_pages_of(c); !pages.empty()) {
                        std::vector<RgbLending> lent;
                        lent.reserve(pages.size());
                        for (const std::size_t at : pages) lent.emplace_back(c.doc, at);
                        plain(c);
                        for (const RgbLending& each : lent) each.take_back(c.doc);
                        color_edit::bake_marks(c.doc);
                        ink_edit::bake_marks(c.doc);
                        return;
                    }
                }
                if (ink_edit::lends(name, c.op)) {
                    if (const std::vector<std::size_t> pages = ink_pages_of(c); !pages.empty()) {
                        std::vector<ink_edit::Lending> lent;
                        lent.reserve(pages.size());
                        for (const std::size_t at : pages) lent.emplace_back(c.doc, at, name, c.op);
                        plain(c);
                        for (const ink_edit::Lending& each : lent) each.take_back(c.doc);
                        color_edit::bake_marks(c.doc);
                        ink_edit::bake_marks(c.doc);
                        return;
                    }
                }
                plain(c);
                color_edit::bake_marks(c.doc);
                ink_edit::bake_marks(c.doc);
            };
            // (channels@1: a pixel op's "channel" — a page's alpha or spot channel lent to it as a paint layer of its own,
            // or one colour plate of its layer, the others put back after it: render/channels; and a page's channels
            // kept at its depth)
            r.add(name, [plain, op, name](core::OpContext& c) {
                const std::optional<channels::Target> target = core::channel_target_op(name) ? channels::target_of(c.doc, c.op) : std::nullopt;
                if (!target) {
                    op(c);
                    channels::follow_depth(c.doc);
                    return;
                }
                const std::size_t at = core::require_page(c.doc, c.op);
                if (target->channel) {
                    core::Json lent_op = c.op;
                    lent_op.erase("channel");
                    const channels::ChannelLending lending(c.doc, at, *target->channel, lent_op, name);
                    core::OpContext inner{c.doc, lent_op, c.actor};
                    plain(inner);
                    const bool changed = lending.take_back(c.doc);
                    c.report = std::move(inner.report);
                    if (changed && lending.depth() > 8) {  // (a 16- or 32-bit channel: what the op changed, drawn at 8 bits)
                        if (!c.report || c.report->is_null()) c.report = core::Json::object();
                        if (c.report->is_object()) (*c.report)["channel_precision"] = 8;
                    }
                    color_edit::bake_marks(c.doc);
                    ink_edit::bake_marks(c.doc);
                    return;
                }
                core::Json plate_op = c.op;
                plate_op.erase("channel");
                const channels::PlateKeep keep(c.doc, at, *target->plate, plate_op, name);
                core::OpContext inner{c.doc, plate_op, c.actor};
                op(inner);
                keep.restore(c.doc);
                c.report = std::move(inner.report);
                channels::follow_depth(c.doc);
            });
        }
        return r;
    }();
    return registry;
}

}  // namespace genko::render
