// PC2 (補正・色管理, SPEC.md COLOR-04): the registry of the C++ build's own correction ops and filter_raster's way of
// its own kinds (render/ops_adjust.hpp). PC2a writes set_adjustment's and auto_adjust's bodies and equalize here; the
// other groups' ops are in their own files.

#include "render/ops_adjust.hpp"

#include "core/adjust_kinds.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/adjust.hpp"
#include "render/color_filters.hpp"
#include "render/ink_filters.hpp"

namespace genko::render {

namespace adjust_ops {

namespace {

using core::Json;

const LocalFilters& local_filters() {
    static const LocalFilters all = [] {
        LocalFilters out = adjust_local_filters();
        for (auto& each : tone_local_filters()) out.push_back(std::move(each));
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

void set_adjustment(core::OpContext&) { core::adjust_not_yet("the op set_adjustment", "PC2a"); }
void auto_adjust(core::OpContext&) { core::adjust_not_yet("the op auto_adjust", "PC2a"); }
LocalFilters adjust_local_filters() { return {}; }

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
    work.params = core::normalize_filter(kind, params, &page);
    if (python) {
        Json adjust = work.params;
        adjust["kind"] = std::string(kind);
        if (core::python_way(adjust)) {
            work.python = true;
            work.params = core::without_native(params);
            return work;
        }
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
    work.function = core::filter_function(kind, work.params, page, &doc);
    if (!work.function) throw core::OpError("the filter " + std::string(kind) + " cannot be used with these settings");
    return work;
}

Image filter_eight(const FilterWork& work, const core::Document& doc, const core::Page& page, const Image& rgba, std::string_view kind,
                   const Json* area) {
    if (work.function) return adjust::apply_8bit(rgba, *work.function);
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
    registry.add("auto_adjust", adjust_ops::auto_adjust);
    registry.add("hdr_toning", adjust_ops::hdr_toning);
    registry.add("apply_image", adjust_ops::apply_image);
    registry.add("calculations", adjust_ops::calculations);
    registry.add("trap", adjust_ops::trap);
    registry.add("fade_layer", adjust_ops::fade_layer);
}

}  // namespace genko::render
