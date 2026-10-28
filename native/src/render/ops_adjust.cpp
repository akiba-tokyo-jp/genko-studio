// PC2 (補正・色管理, SPEC.md COLOR-04): the registry of the C++ build's own correction ops and filter_raster's way of
// its own kinds (render/ops_adjust.hpp). PC2a writes set_adjustment's and auto_adjust's bodies and equalize here; the
// other groups' ops are in their own files.

#include "render/ops_adjust.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

#include "core/adjust_kinds.hpp"
#include "core/adjust_tone.hpp"
#include "core/areas.hpp"
#include "core/color_raster.hpp"
#include "core/filters.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/adjust.hpp"
#include "render/channels.hpp"
#include "render/color_edit.hpp"
#include "render/color_filters.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/ink_edit.hpp"
#include "render/ink_filters.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"

namespace genko::render {

namespace adjust_ops {

namespace {

using core::Json;

const LocalFilters& local_filters() {
    static const LocalFilters all = [] {
        LocalFilters out = adjust_local_filters();
        for (auto& each : tone_local_filters()) out.push_back(std::move(each));
        for (auto& each : filter_local_filters()) out.push_back(std::move(each));  // (PC4 節 D)
        return out;
    }();
    return all;
}

std::string kind_in(const Json& op) {
    const Json* kind = core::get(op, "kind");
    return kind != nullptr && kind->is_string() ? kind->get<std::string>() : std::string();
}

[[noreturn]] void local_not_yet(std::string_view kind, std::string_view where) {
    const core::AdjustKind* found = core::adjust_kind(kind);
    core::adjust_not_yet("on " + std::string(where) + ", the filter " + std::string(kind), found != nullptr ? found->group : "PC2");
}

}  // namespace

// --- PC2a: layer.setAdjustment (design 3.1), image.autoTone・autoContrast・autoColor (3.3), equalize -----------------

namespace {

using Histograms = std::array<std::array<std::uint64_t, 256>, 3>;

std::size_t bin(double v) { return static_cast<std::size_t>(std::lround((std::isnan(v) ? 0.0 : std::clamp(v, 0.0, 1.0)) * 255.0)); }

// Each pixel of the layer an op works on that shows (alpha above 0) inside the area: its colour (0..1; a precise layer
// its page's values, a page of inks the colours they mean). `sampled`: a large layer every few pixels (PhotoCraft
// samples a picture of more than 4 million pixels so: edit_cmds.rs histograms).
template <typename Each>
void each_pixel(const core::Document& doc, const core::Page& page, const core::Layer& layer, const Json* area, bool sampled, Each&& each) {
    const auto stride = [sampled](std::size_t w, std::size_t h) {
        if (!sampled) return std::size_t(1);
        const std::size_t step = std::max<std::size_t>(1, (w * h) / 4000000);
        return std::max<std::size_t>(1, static_cast<std::size_t>(std::sqrt(static_cast<double>(step))));
    };
    if (layer.color_raster) {
        const core::ColorRasterView raster(*layer.color_raster);
        const std::string inside = area != nullptr ? color_edit::area_on(page, raster, *area).tobytes() : std::string();
        const std::size_t w = raster.width(), h = raster.height(), s = stride(w, h);
        for (std::size_t y = 0; y < h; y += s)
            for (std::size_t x = 0; x < w; x += s) {
                const std::size_t i = y * w + x;
                if (!inside.empty() && inside[i] == 0) continue;
                const auto p = raster.pixel(i);
                if (!(p[3] > 0)) continue;
                each(std::array<double, 3>{p[0], p[1], p[2]});
            }
        return;
    }
    if (page.color && !layer.raster_png) {
        const core::PageColor& color = *page.color;
        const core::InkPixels inks = ink_edit::with_marks(page, layer, &doc);
        const std::string inside = area != nullptr ? ink_edit::area_coverage(page, inks.width, inks.height, *area) : std::string();
        const std::size_t w = inks.width, h = inks.height, s = stride(w, h);
        const unsigned n = inks.colours();
        std::vector<double> ink(n), rgb(3);
        for (std::size_t y = 0; y < h; y += s)
            for (std::size_t x = 0; x < w; x += s) {
                const std::size_t i = y * w + x;
                if (!inside.empty() && inside[i] == 0) continue;
                if (inks.has_alpha() && !(inks.value(i, n) > 0)) continue;
                for (unsigned c = 0; c < n; ++c) ink[c] = from_canvas(color.space, c, inks.value(i, c));
                ink::meaning_of(color, ink.data(), rgb.data(), 1);
                each(std::array<double, 3>{rgb[0], rgb[1], rgb[2]});
            }
        return;
    }
    const Image picture = raster::layer_pixels(page, layer, &doc).convert("RGBA");
    std::string inside;
    if (area != nullptr) {
        const selection::AreaMask mask = selection::area_mask(*area, raster::dpi_across(page, picture.width()));  // (PC4 F-b: its own grid)
        Image full = Image::create("L", picture.size(), Ink(0));
        full.paste(mask.mask, Point{static_cast<int>(mask.x0), static_cast<int>(mask.y0)});
        inside = full.tobytes();
    }
    const std::string bytes = picture.tobytes();
    const std::size_t w = static_cast<std::size_t>(picture.width()), h = static_cast<std::size_t>(picture.height()), s = stride(w, h);
    for (std::size_t y = 0; y < h; y += s)
        for (std::size_t x = 0; x < w; x += s) {
            const std::size_t i = y * w + x;
            if (!inside.empty() && inside[i] == 0) continue;
            const auto* q = reinterpret_cast<const unsigned char*>(bytes.data() + i * 4);
            if (q[3] == 0) continue;
            each(std::array<double, 3>{q[0] / 255.0, q[1] / 255.0, q[2] / 255.0});
        }
}

// Equalize's map: each channel's value through the CDF of its bin (every depth by the same 256 bins, as Photoshop).
class Equalized : public core::AdjustFunction {
public:
    explicit Equalized(const std::array<double, 256>& table) : table_(table) {}
    std::array<double, 3> apply(const std::array<double, 3>& rgb, int, int) const override {
        return {table_[bin(rgb[0])], table_[bin(rgb[1])], table_[bin(rgb[2])]};
    }
    std::optional<core::ChannelTables> tables() const override {
        core::ChannelTables out;
        for (std::size_t i = 0; i < 256; ++i) {
            const auto v = static_cast<std::uint8_t>(std::lround(std::clamp(table_[i], 0.0, 1.0) * 255.0));
            out.rgb[0][i] = out.rgb[1][i] = out.rgb[2][i] = v;
        }
        return out;
    }

private:
    std::array<double, 256> table_;
};

// The layer's red, green and blue values counted into one histogram (PhotoCraft's equalize: one CDF for all three).
std::array<std::uint64_t, 256> counted(const Image* rgba, const core::Document& doc, const core::Page& page, const core::Layer* layer, const Json* area) {
    std::array<std::uint64_t, 256> counts{};
    const auto add = [&counts](const std::array<double, 3>& rgb) {
        for (const double v : rgb) ++counts[bin(v)];
    };
    if (rgba != nullptr) {  // (an 8-bit layer's picture, the area over the page at the working resolution)
        std::string inside;
        if (area != nullptr) {
            const selection::AreaMask mask = selection::area_mask(*area, raster::dpi_across(page, rgba->width()));  // (PC4 F-b: its own grid)
            Image full = Image::create("L", rgba->size(), Ink(0));
            full.paste(mask.mask, Point{static_cast<int>(mask.x0), static_cast<int>(mask.y0)});
            inside = full.tobytes();
        }
        const std::string bytes = rgba->convert("RGBA").tobytes();
        for (std::size_t i = 0; i * 4 < bytes.size(); ++i) {
            if (!inside.empty() && inside[i] == 0) continue;
            const auto* q = reinterpret_cast<const unsigned char*>(bytes.data() + i * 4);
            if (q[3] == 0) continue;
            for (unsigned c = 0; c < 3; ++c) ++counts[q[c]];
        }
        return counts;
    }
    each_pixel(doc, page, *layer, area, false, add);
    return counts;
}

std::shared_ptr<const core::AdjustFunction> equalized(const std::array<std::uint64_t, 256>& counts) {
    const auto table = core::equalize_table(counts);
    if (!table) return nullptr;
    return std::make_shared<Equalized>(*table);
}

// set_adjustment's reset: a Python kind's neutral settings (what its window starts from: levels 0..255, a straight tone
// curve, no hue turn …), a kind of the C++ build's own its defaults; the layer's blend and its formula kept.
Json reset_of(const Json& current) {
    const std::string kind = current.value("kind", std::string());
    Json out{{"kind", kind}};
    if (kind == "levels") {
        out["black"] = 0;
        out["white"] = 255;
    } else if (kind == "curve") {
        out["points"] = Json::array({Json::array({0, 0}), Json::array({255, 255})});
    } else if (kind == "hue") {
        out["shift"] = 0;
    } else if (kind == "posterize") {
        out["levels"] = 4;
    } else if (kind == "threshold") {
        out["threshold"] = 128;
    } else if (kind == "bitonal") {
        out["threshold"] = 180;
    } else if (kind == "brightness_contrast") {
        out["brightness"] = 0;
        out["contrast"] = 0;
    }
    if (const auto native = current.find("native"); native != current.end() && native->is_object()) {
        Json kept = Json::object();
        for (const char* key : {"blend", "method", "space"}) {
            if (native->contains(key)) kept[key] = (*native)[key];
        }
        if (!kept.empty()) out["native"] = kept;
    }
    return out;
}

// The settings given laid over the current ones: an object over an object key by key (a range of hue_saturation, the
// native and its channel records), a null taking the setting out (a channel record set back to doing nothing; Python's
// `channel` back to all three), anything else in its place.
void lay_over(Json& now, const Json& given) {
    for (const auto& [key, value] : given.items()) {
        const auto it = now.find(key);
        if (value.is_null()) {
            if (it != now.end()) now.erase(it);
        } else if (it != now.end() && it->is_object() && value.is_object()) {
            lay_over(*it, value);
        } else {
            now[key] = value;
        }
    }
}

const core::Layer& correction_layer(const core::Document& doc, const Json& op, std::size_t& page_at) {
    page_at = core::require_page(doc, op);
    const Json* id = core::get(op, "id");
    if (id == nullptr || !id->is_string()) throw core::OpError("set_adjustment needs the id of a correction layer");
    const core::Page& page = doc.page(page_at);
    const core::Layer& layer = page.layers[core::layer_by_id(page, id->get<std::string>())];
    if (layer.kind != core::LayerKind::Adjust || !layer.adjust || !layer.adjust->is_object())
        throw core::OpError("layer " + layer.id + " is not a correction layer");
    return layer;
}

}  // namespace

void set_adjustment(core::OpContext& c) {
    std::size_t at = 0;
    const core::Layer& layer = correction_layer(c.doc, c.op, at);
    Json given = Json::object();
    for (const auto& [key, value] : c.op.items()) {
        if (key != "op" && key != "page" && key != "id" && key != "layer") given[key] = value;
    }
    if (given.contains("kind")) throw core::OpError("set_adjustment keeps the correction's kind (set_layer adjust changes it)");
    Json next = *layer.adjust;
    if (given.empty()) {
        next = reset_of(next);  // (no settings: back to the defaults, as PhotoCraft's layer.setAdjustment)
    } else {
        lay_over(next, given);
    }
    // (the correction set as set_layer sets it: Python's settings checked as Python checks them, a native read, a kind of
    // the C++ build's own in its normal form, a lookup table into the book)
    const std::string id = layer.id;
    const Json inner_op{{"op", "set_layer"}, {"page", c.op.at("page")}, {"id", id}, {"adjust", next}};
    core::OpContext inner{c.doc, inner_op, c.actor};
    (*core::OpRegistry::builtin().find("set_layer"))(inner);
    const core::Page& page = c.doc.page(at);
    const core::Layer& now = page.layers[core::layer_by_id(page, id)];
    c.report = Json{{"adjust", *now.adjust}};
}

void auto_adjust(core::OpContext& c, const core::OpFunction& filter_raster) {
    const Json* mode = core::get(c.op, "mode");
    if (mode == nullptr || !mode->is_string() || (*mode != Json("tone") && *mode != Json("contrast") && *mode != Json("color")))
        throw core::OpError("auto_adjust mode must be one of tone, contrast, color");
    if (!filter_raster) throw core::Error("not_yet_ported", "auto_adjust without filter_raster");
    // the levels laid by filter_raster, on the layer it works on (a channel or a plate of it lent as it lends them)
    Json op{{"op", "filter_raster"}, {"page", c.op.contains("page") ? c.op["page"] : Json()}, {"kind", "levels"}};
    for (const char* key : {"id", "area", "channel"}) {
        if (c.op.contains(key)) op[key] = c.op[key];
    }
    Json records;
    const auto run = [&](Json& on) {
        on.erase("channel");
        const std::size_t at = core::require_page(c.doc, on);
        const core::Page& page = c.doc.page(at);
        const Json* id = core::get(on, "id");
        if (id == nullptr || !id->is_string()) throw core::OpError("auto_adjust needs the id of a layer");
        const core::Layer& layer = page.layers[core::layer_by_id(page, id->get<std::string>())];
        if (layer.locked) throw core::OpError("the layer is locked");
        const std::optional<Json> area = core::truthy_at(on, "area") ? std::optional<Json>(core::op_area(on)) : std::nullopt;
        Histograms h{};
        each_pixel(c.doc, page, layer, area ? &*area : nullptr, true, [&h](const std::array<double, 3>& rgb) {
            for (unsigned k = 0; k < 3; ++k) ++h[k][bin(rgb[k])];
        });
        // (a multichannel page: no channel records and no colours to balance — every mode the contrast's points, the
        // same for each channel, as the composite)
        const bool multi = page.color && page.color->space == core::InkSpace::Multi;
        records = core::auto_levels(h, multi ? std::string("contrast") : mode->get<std::string>());
        if (multi) {
            for (const char* key : {"black", "white", "gamma", "out_black", "out_white"}) on[key] = records["red"][key];
            on["native"] = Json{{"method", "upstream"}};
        } else {
            on["native"] = Json{{"method", "upstream"}, {"space", "rgb"}, {"channels", records}};
        }
        core::OpContext inner{c.doc, on, c.actor};
        filter_raster(inner);
    };
    const std::optional<channels::Target> target = channels::target_of(c.doc, op);
    if (!target) {
        run(op);
    } else if (target->channel) {
        Json lent_op = op;
        lent_op.erase("channel");
        const channels::ChannelLending lending(c.doc, core::require_page(c.doc, op), *target->channel, lent_op, "filter_raster");
        run(lent_op);
        (void)lending.take_back(c.doc);
    } else {
        Json plate_op = op;
        plate_op.erase("channel");
        const channels::PlateKeep keep(c.doc, core::require_page(c.doc, op), *target->plate, plate_op, "filter_raster");
        run(plate_op);
        keep.restore(c.doc);
    }
    c.report = Json{{"levels", records}};
}

LocalFilters adjust_local_filters() {
    LocalFilter equalize;
    equalize.eight = [](const Image& rgba, const Json&, const core::Page& page, const core::Document& doc) {
        const auto f = equalized(counted(&rgba, doc, page, nullptr, nullptr));
        return f ? adjust::apply_8bit(rgba, *f) : rgba;
    };
    equalize.precise = [](const core::Page& page, const core::Layer& layer, const Json&, const Json* area, const core::Document& doc) {
        const auto f = equalized(counted(nullptr, doc, page, &layer, area));
        return f ? color_filters::apply_function(page, layer, *f, area) : *layer.color_raster;
    };
    equalize.inks = [](const core::Page& page, const core::InkPixels& inks, const Json&, const Json* area, const core::Document&) {
        // (the colours the inks mean counted, inside the area)
        const core::PageColor& color = *page.color;
        const std::string inside = area != nullptr ? ink_edit::area_coverage(page, inks.width, inks.height, *area) : std::string();
        std::array<std::uint64_t, 256> counts{};
        const unsigned n = inks.colours();
        std::vector<double> ink(n), rgb(3);
        for (std::size_t i = 0; i < inks.pixel_count(); ++i) {
            if (!inside.empty() && inside[i] == 0) continue;
            if (inks.has_alpha() && !(inks.value(i, n) > 0)) continue;
            for (unsigned c = 0; c < n; ++c) ink[c] = from_canvas(color.space, c, inks.value(i, c));
            ink::meaning_of(color, ink.data(), rgb.data(), 1);
            for (const double v : rgb) ++counts[bin(v)];
        }
        const auto f = equalized(counts);
        if (!f) return core::encode_ink_raster(inks);  // (nothing shows: as it was)
        return ink_filters::apply_function(page, inks, *f, area);
    };
    return {{"equalize", equalize}};
}

// --- filter_raster's kinds of the C++ build's own (design 3.2) ------------------------------------------------------

bool handles(const Json& op) {
    const std::string kind = kind_in(op);
    return !kind.empty() && core::native_filter(kind, op);
}

FilterWork filter_work(const core::Document& doc, const core::Page& page, std::string_view kind, const Json& params_in) {
    FilterWork work;
    const bool python = core::python_adjust_kind(kind);
    Json params = params_in.is_object() ? params_in : Json::object();
    if (!python) params.erase("channel");  // (PC1c's target: render/channels lends it; not a setting of the kind)
    core::LookupTables made;  // (a color_lookup's table of the op's file text: used, not kept in the book)
    work.params = core::normalize_filter(kind, params, &page, &made);
    if (python) {
        Json adjust = work.params;
        adjust["kind"] = std::string(kind);
        if (core::python_way(adjust)) {
            work.python = true;
            work.params = core::without_native(params);
            return work;
        }
        (void)core::adjustment(kind, core::without_native(work.params));  // (Python's settings refused as Python refuses them)
    }
    const core::AdjustKind* found = core::adjust_kind(kind);
    if (found != nullptr && found->local) {
        for (const auto& [name, local] : local_filters()) {
            if (name == kind) {
                work.local = &local;
                return work;
            }
        }
        core::adjust_not_yet("the filter " + std::string(kind), found->group);
    }
    core::LookupTables luts;
    if (!made.empty()) {
        luts = doc.luts;
        luts.insert(made.begin(), made.end());
    }
    work.function = core::filter_function(kind, work.params, page, made.empty() ? &doc.luts : &luts);
    if (!work.function) throw core::OpError("the filter " + std::string(kind) + " cannot be used with these settings");
    return work;
}

Image filter_eight(const FilterWork& work, const core::Document& doc, const core::Page& page, const Image& rgba, std::string_view kind,
                   const Json* area) {
    if (work.function) return adjust::apply_8bit(rgba, *work.function);
    if (kind == "equalize" && area != nullptr) {  // (the pixels inside the area counted: the caller lays the result inside it)
        const auto f = equalized(counted(&rgba, doc, page, nullptr, area));
        return f ? adjust::apply_8bit(rgba, *f) : rgba;
    }
    if (area != nullptr && work.local != nullptr && work.local->eight_in) return work.local->eight_in(rgba, work.params, page, doc, *area);
    if (work.local == nullptr || !work.local->eight) local_not_yet(kind, "an 8-bit layer");
    return work.local->eight(rgba, work.params, page, doc);
}

std::string filter_precise(const FilterWork& work, const core::Document& doc, const core::Page& page, const core::Layer& layer,
                           std::string_view kind, const Json* area) {
    if (work.function) return color_filters::apply_function(page, layer, *work.function, area);
    if (work.local == nullptr || !work.local->precise) local_not_yet(kind, "precise pixels");
    return work.local->precise(page, layer, work.params, area, doc);
}

std::string filter_inks(const FilterWork& work, const core::Document& doc, const core::Page& page, const core::InkPixels& inks,
                        std::string_view kind, const Json* area) {
    if (work.function) return ink_filters::apply_function(page, inks, *work.function, area);
    if (work.local == nullptr || !work.local->inks) local_not_yet(kind, "a page of inks");
    return work.local->inks(page, inks, work.params, area, doc);
}

}  // namespace adjust_ops

void register_adjust_ops(core::OpRegistry& registry) {
    registry.add("set_adjustment", adjust_ops::set_adjustment);
    {  // (auto_adjust lays its levels by filter_raster as registered: render/raster_ops, before it)
        const core::OpFunction* filter = registry.find("filter_raster");
        registry.add("auto_adjust", [filter = filter != nullptr ? *filter : core::OpFunction()](core::OpContext& c) { adjust_ops::auto_adjust(c, filter); });
    }
    registry.add("hdr_toning", adjust_ops::hdr_toning);
    registry.add("apply_image", adjust_ops::apply_image);
    registry.add("calculations", adjust_ops::calculations);
    registry.add("trap", adjust_ops::trap);
    registry.add("fade_layer", adjust_ops::fade_layer);
}

}  // namespace genko::render
