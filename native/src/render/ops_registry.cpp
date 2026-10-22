#include "render/ops_registry.hpp"

#include <algorithm>
#include <unordered_map>

#include "core/channels.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ops_util.hpp"
#include "core/pyops.hpp"
#include "core/strokes.hpp"
#include "core/studio.hpp"
#include "render/bookpages.hpp"
#include "render/color_canvas.hpp"
#include "render/channels.hpp"
#include "render/color_edit.hpp"
#include "render/ink_edit.hpp"
#include "render/ops_3d.hpp"
#include "render/ops_adjust.hpp"
#include "render/paper.hpp"
#include "render/placed.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/raster_ops.hpp"
#include "render/area_kinds.hpp"
#include "render/layer_masks.hpp"
#include "render/ops_pc3.hpp"
#include "render/ops_paint_tools.hpp"

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

// The ops that bring new colours (sRGB) onto an RGB page of a profile through 8-bit tools working its pictures: those
// lending ink layers their pictures (render/ink_edit), but the merges — they composite the page's values themselves
// (render::composite_color_layers) — and a conversion of precise lines or pixels (no 8-bit picture in it).
bool lends_rgb(std::string_view op, const core::Json& args, const core::Document& doc, std::size_t at) {
    if (!ink_edit::lends(op, args)) return false;
    if (op == "merge_down" || op == "merge_layers" || op == "merge_visible") return false;
    if (op == "convert_layer") {
        const core::Json* id = core::get(args, "id");
        for (const core::Layer& layer : doc.page(at).layers) {
            if (id != nullptr && id->is_string() && layer.id == id->get<std::string>() && (layer.color_raster || core::has_color_strokes(layer))) {
                return false;
            }
        }
    }
    return true;
}

// color-model@1 (SPEC.md COLOR-03): an op that brings new colours onto an RGB page of a profile through 8-bit tools works
// the page as the sRGB it shows: while it runs, the page is an sRGB page (its mode set aside) and every paint pixel of it
// (core::holds_page_values) is lent as the sRGB colours it shows. Then the mode is put back and what the op left is
// taken back into the page's values: a layer it did not touch keeps its bytes; a pixel shown within 2 a channel of how
// it was shown there keeps its own value (straight alpha's rounding, a mix that barely reached it), as does a colour the
// lent pictures showed (moved: its own value, a colour past sRGB's gamut too); the rest, the colours the op brought or
// mixed, come in from sRGB (the page's intent and black point compensation).
class RgbLending {
public:
    RgbLending(core::Document& doc, std::size_t at) {
        const core::Page& seen = doc.page(at);
        page_id_ = seen.id;
        mode_ = *seen.rgb;
        const RgbSpace space(mode_);
        core::Page& page = doc.edit_page(at);
        for (core::Layer& layer : page.layers) {
            before_.emplace_back(layer.id, layer.raster_png);
            if (!core::holds_page_values(page, layer)) continue;
            Loan loan;
            loan.layer = layer.id;
            loan.png = layer.raster_png;
            const Image values = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
            loan.own = values.tobytes();
            const Image shown = srgb_from(values, space);
            loan.shown = shown.tobytes();
            loan.width = shown.width();
            loan.height = shown.height();
            loan.lent = std::make_shared<const std::string>(write_png(shown, 1));
            layer.raster_png = loan.lent;
            layer.raster_memo = {};
            loans_.push_back(std::move(loan));
        }
        page.rgb.reset();  // (an sRGB page while the op runs)
    }

    void take_back(core::Document& doc) const {
        std::size_t at = doc.pages.size();
        for (std::size_t i = 0; i < doc.pages.size(); ++i) {
            if (!doc.is_deferred(i) && doc.page(i).id == page_id_) at = i;
        }
        if (at == doc.pages.size() || doc.page(at).color) return;
        core::Page& page = doc.edit_page(at);
        if (!page.rgb) page.rgb = mode_;
        const RgbSpace space(mode_);
        const Own own = own_colours();
        for (core::Layer& layer : page.layers) {
            if (!layer.raster_png || layer.raster_png->empty()) continue;
            const auto loan = std::find_if(loans_.begin(), loans_.end(), [&](const Loan& l) { return l.layer == layer.id; });
            if (loan != loans_.end() && (layer.raster_png == loan->lent || *layer.raster_png == *loan->lent)) {
                layer.raster_png = loan->png;  // (as it was lent: its own bytes)
                layer.raster_memo = {};
                continue;
            }
            if (loan == loans_.end()) {  // (not lent: a layer the op did not touch keeps its bytes)
                const auto was = std::find_if(before_.begin(), before_.end(), [&](const auto& b) { return b.first == layer.id; });
                if (was != before_.end() && was->second == layer.raster_png) continue;
            }
            const Image now = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
            layer.raster_png = std::make_shared<const std::string>(write_png(into_values(now, loan != loans_.end() ? &*loan : nullptr, own, space)));
            layer.raster_memo = {};
        }
    }

private:
    struct Loan {
        std::string layer;
        core::Bytes png;    // (its own: the page's values)
        core::Bytes lent;   // (the sRGB it was lent)
        std::string own;    // (its values' RGBA bytes)
        std::string shown;  // (the picture lent: RGBA bytes)
        int width = 0, height = 0;
    };
    using Own = std::unordered_map<std::uint32_t, std::uint32_t>;  // (a colour shown: the value it showed, RGB)

    static std::uint32_t key(const unsigned char* p) { return (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2]; }

    // The colours the lent pictures showed: each one's first value.
    Own own_colours() const {
        Own out;
        for (const Loan& loan : loans_) {
            const auto* shown = reinterpret_cast<const unsigned char*>(loan.shown.data());
            const auto* own = reinterpret_cast<const unsigned char*>(loan.own.data());
            for (std::size_t i = 0; i + 3 < loan.shown.size(); i += 4) {
                if (shown[i + 3] == 0) continue;
                out.try_emplace(key(shown + i), key(own + i));
            }
        }
        return out;
    }

    // The nearest colour shown within 2 a channel.
    static const std::uint32_t* near_own(const Own& own, const unsigned char* p) {
        const std::uint32_t* best = nullptr;
        int far = 7;
        for (int dr = -2; dr <= 2; ++dr) {
            for (int dg = -2; dg <= 2; ++dg) {
                for (int db = -2; db <= 2; ++db) {
                    const int r = p[0] + dr, g = p[1] + dg, b = p[2] + db, d = std::abs(dr) + std::abs(dg) + std::abs(db);
                    if (r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255 || d >= far) continue;
                    if (const auto it = own.find((std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | std::uint32_t(b)); it != own.end()) {
                        best = &it->second;
                        far = d;
                    }
                }
            }
        }
        return best;
    }

    // What an op left (sRGB, RGBA) in the page's values (see the class).
    static Image into_values(const Image& now, const Loan* loan, const Own& own, const RgbSpace& space) {
        std::string out = now.tobytes();
        auto* p = reinterpret_cast<unsigned char*>(out.data());
        const bool same = loan != nullptr && loan->width == now.width() && loan->height == now.height();
        const auto* shown = same ? reinterpret_cast<const unsigned char*>(loan->shown.data()) : nullptr;
        const auto* values = same ? reinterpret_cast<const unsigned char*>(loan->own.data()) : nullptr;
        std::vector<double> rgb;
        std::vector<std::size_t> where;
        for (std::size_t i = 0; i + 3 < out.size(); i += 4) {
            unsigned char* q = p + i;
            if (q[3] == 0) continue;
            if (shown != nullptr) {
                const unsigned char* was = shown + i;
                if (was[3] != 0 && std::abs(was[0] - q[0]) <= 2 && std::abs(was[1] - q[1]) <= 2 && std::abs(was[2] - q[2]) <= 2) {
                    const unsigned char* own_value = values + i;
                    std::copy_n(own_value, 3, q);
                    if (was[3] == q[3]) q[3] = own_value[3];
                    continue;
                }
            }
            const auto it = own.find(key(q));
            const std::uint32_t* found = it != own.end() ? &it->second : near_own(own, q);
            if (found != nullptr) {
                q[0] = static_cast<unsigned char>(*found >> 16);
                q[1] = static_cast<unsigned char>(*found >> 8);
                q[2] = static_cast<unsigned char>(*found);
                continue;
            }
            for (unsigned c = 0; c < 3; ++c) rgb.push_back(q[c] / 255.0);
            where.push_back(i);
        }
        space.from_srgb(rgb.data(), where.size());
        for (std::size_t k = 0; k < where.size(); ++k) {
            for (unsigned c = 0; c < 3; ++c) p[where[k] + c] = static_cast<unsigned char>(std::lround(std::clamp(rgb[k * 3 + c], 0.0, 1.0) * 255.0));
        }
        return Image::frombytes("RGBA", now.size(), out);
    }

    std::string page_id_;
    core::RgbMode mode_;
    std::vector<Loan> loans_;
    std::vector<std::pair<std::string, core::Bytes>> before_;  // (every layer's picture before the op)
};

// color-model@1 (PC1d, SPEC.md COLOR-03): what an op left on a page of a mode of its own, made that mode's — a bitmap
// page's pixels 0 or full at 50% (its alpha too: 2 階調), an indexed page's colours its table's nearest (no dither), an
// RGB page of 16 or 32 bits' new 8-bit pixels on a paint layer widened to its depth exactly (v × 257, v / 255: the
// layer then composites in precise colour as the others). Only the layers the op changed (their pixels another object
// than before, or all of a page whose mode it changed) are looked at; the rest keep their bytes. A bitmap or indexed
// page's name and draft layers (kept as layers by the flatten, never printed) keep their pixels as they are: the page
// shows them through its last map — the table's colours, two — as it shows the rest.
void keep_page_modes(core::Document& doc, const std::vector<core::PagePtr>& before) {
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        if (doc.is_deferred(p)) continue;
        const core::Page& seen = doc.page(p);
        const bool bitmap = seen.color && seen.color->bitmap;
        const bool indexed = !seen.color && seen.rgb && seen.rgb->table;
        const bool deep = !seen.color && seen.rgb && seen.rgb->depth > 8 && !seen.rgb->table;
        if (!bitmap && !indexed && !deep) continue;
        const core::Page* was = nullptr;
        for (const core::PagePtr& b : before) {
            if (b && b->id == seen.id) was = b.get();
        }
        if (was == &seen) continue;  // (not edited)
        // (a page made bitmap or indexed now: all its layers; a deep page's only the pixels an op made — a change of its
        // depth or profile converts its layers itself, set_color_mode, and assign_profile keeps their bytes)
        const bool same_mode = was != nullptr && (deep || (was->color == seen.color && was->rgb == seen.rgb));
        const auto changed = [&](const core::Layer& layer) {
            if (!same_mode) return true;
            for (const core::Layer& old : was->layers) {
                if (old.id == layer.id) return old.raster_png != layer.raster_png || old.ink_raster != layer.ink_raster;
            }
            return true;
        };
        std::vector<std::size_t> todo;
        for (std::size_t i = 0; i < seen.layers.size(); ++i) {
            const core::Layer& layer = seen.layers[i];
            const bool pixels = bitmap ? layer.ink_raster != nullptr : layer.raster_png && !layer.raster_png->empty();
            if (!pixels || !changed(layer)) continue;
            if ((bitmap || indexed) && (layer.role == core::LayerRole::Name || layer.role == core::LayerRole::Draft)) continue;
            // (deep: a paint layer a precise raster can hold — render/page's and set_color_mode's rule)
            if (deep && (layer.kind != core::LayerKind::Raster || layer.role == core::LayerRole::Tone || layer.stroke_count() || !layer.patches.empty() ||
                         layer.panel_clip || layer.effect || layer.screen || layer.color)) {
                continue;
            }
            todo.push_back(i);
        }
        if (todo.empty()) continue;
        core::Page& page = doc.edit_page(p);
        for (const std::size_t i : todo) {
            core::Layer& layer = page.layers[i];
            if (bitmap) {
                core::InkPixels pixels = core::decode_ink_raster(*layer.ink_raster);
                if (pixels.depth != 8 || pixels.space != core::InkSpace::Gray) continue;
                bool any = false;
                for (std::uint16_t& s : pixels.samples) {
                    const std::uint16_t two = s >= 128 ? 255 : 0;
                    any = any || two != s;
                    s = two;
                }
                if (!any) continue;
                layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(pixels));
                layer.ink_memo = {};
                continue;
            }
            const Image picture = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
            std::string bytes = picture.tobytes();
            if (indexed) {
                const core::ColorTable& table = *page.rgb->table;
                std::unordered_map<std::uint32_t, std::size_t> nearest;
                bool any = false;
                for (std::size_t at = 0; at + 3 < bytes.size(); at += 4) {
                    auto* q = reinterpret_cast<unsigned char*>(bytes.data() + at);
                    if (q[3] == 0) continue;
                    const std::uint32_t key = (std::uint32_t(q[0]) << 16) | (std::uint32_t(q[1]) << 8) | q[2];
                    auto it = nearest.find(key);
                    if (it == nearest.end()) it = nearest.emplace(key, table.nearest(q[0], q[1], q[2])).first;
                    const std::array<int, 3>& c = table.colors[it->second];
                    for (unsigned k = 0; k < 3; ++k) {
                        any = any || q[k] != c[k];
                        q[k] = static_cast<unsigned char>(c[k]);
                    }
                }
                if (!any) continue;
                layer.raster_png = std::make_shared<const std::string>(write_png(Image::frombytes("RGBA", picture.size(), bytes)));
                layer.raster_memo = {};
                layer.raster_relpath.reset();
                continue;
            }
            const int depth = page.rgb->depth;
            const auto* q = reinterpret_cast<const unsigned char*>(bytes.data());
            layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(
                static_cast<std::uint32_t>(picture.width()), static_cast<std::uint32_t>(picture.height()), depth == 16 ? "u16" : "f32",
                [&](std::size_t at) {
                    return std::array<double, 4>{q[at * 4] / 255.0, q[at * 4 + 1] / 255.0, q[at * 4 + 2] / 255.0, q[at * 4 + 3] / 255.0};
                }));
            layer.raster_png = nullptr;
            layer.raster_memo = {};
            layer.raster_relpath.reset();
        }
    }
}

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
    // the corrections' own ops (PC2, adjustments@1: render/ops_adjust)
    register_adjust_ops(registry);
    // smart objects (PC3, smart-objects@1: render/ops_smart)
    register_smart_ops(registry);
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
    // PC3 (選択・マスク・修復・パス・変形・判型): the new ops (each group writes its own in its file) and the kinds and
    // modifiers of an area this build adds, made known to the bus (render/area_kinds)
    register_select_ops(registry);
    register_retouch_ops(registry);
    register_vector2_ops(registry);
    register_transform_ops(registry);
    register_pagesize_ops(registry);
    area_kinds::use_keys();
    // PC4 節 E (描く道具・パターン): the new ops, frames until their groups write them (render/ops_paint_tools.hpp)
    register_paint_tool_ops(registry);
    register_pattern_ops(registry);
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
                // move or clear pixels work the inks themselves, render/raster_ops. color-model@1: on an RGB page of a
                // profile, the same through sRGB: RgbLending. Then the pictures an op left: lifted page values laid
                // into the pixels, lines and fills into precise pixels and inks.)
                if (ink_edit::lends(name, c.op)) {
                    std::vector<std::size_t> pages = profiled_pages_of(c);
                    std::erase_if(pages, [&](std::size_t at) { return !lends_rgb(name, c.op, c.doc, at); });
                    if (!pages.empty()) {
                        std::vector<RgbLending> lent;
                        lent.reserve(pages.size());
                        for (const std::size_t at : pages) lent.emplace_back(c.doc, at);
                        plain(c);
                        for (const RgbLending& each : lent) each.take_back(c.doc);
                        color_edit::bake_page_values(c.doc);
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
                        color_edit::bake_page_values(c.doc);
                        color_edit::bake_marks(c.doc);
                        ink_edit::bake_marks(c.doc);
                        return;
                    }
                }
                plain(c);
                color_edit::bake_page_values(c.doc);
                color_edit::bake_marks(c.doc);
                ink_edit::bake_marks(c.doc);
            };
            // (channels@1: a pixel op's "channel" — a page's alpha or spot channel lent to it as a paint layer of its own,
            // or one colour plate of its layer, the others put back after it: render/channels; and a page's channels
            // kept at its depth)
            r.add(name, [plain, op, name](core::OpContext& c) {
                const std::vector<core::PagePtr> before = c.doc.pages;  // (color-model@1: what the op changed, keep_page_modes)
                const std::optional<channels::Target> target = core::channel_target_op(name) ? channels::target_of(c.doc, c.op) : std::nullopt;
                // (masks@1, PC3: "layer_mask": true — the op's layer's mask lent to it as a layer to paint, render/layer_masks)
                const bool lends_mask = layer_masks::lends_to(name) && core::truthy_at(c.op, "layer_mask");
                if (!target && lends_mask) {
                    const std::size_t at = core::require_page(c.doc, c.op);
                    core::Json lent_op = c.op;
                    lent_op.erase("layer_mask");
                    const layer_masks::MaskLending lending(c.doc, at, lent_op, name);
                    core::OpContext inner{c.doc, lent_op, c.actor};
                    plain(inner);
                    (void)lending.take_back(c.doc);
                    c.report = std::move(inner.report);
                    color_edit::bake_marks(c.doc);
                    ink_edit::bake_marks(c.doc);
                    return;
                }
                if (!target) {
                    op(c);
                    channels::follow_depth(c.doc);
                    keep_page_modes(c.doc, before);
                    return;
                }
                const std::size_t at = core::require_page(c.doc, c.op);
                if (lends_mask) throw core::OpError("layer_mask and channel cannot be given together");
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
                keep_page_modes(c.doc, before);
            });
        }
        return r;
    }();
    return registry;
}

}  // namespace genko::render
