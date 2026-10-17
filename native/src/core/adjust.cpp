#include "core/adjust.hpp"

#include <algorithm>
#include <string>

#include "core/adjust_kinds.hpp"
#include "core/color_adjust.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

std::string kind_of(const Json& adjust) {
    if (!adjust.is_object()) return {};
    const auto it = adjust.find("kind");
    if (it == adjust.end() || !py_truthy(*it)) return {};
    return it->is_string() ? it->get<std::string>() : py_str(*it);
}

const Json* native_in(const Json& adjust) {
    if (!adjust.is_object()) return nullptr;
    const auto it = adjust.find("native");
    return it == adjust.end() || it->is_null() ? nullptr : &*it;
}

// The adjust's own settings: without its kind and its native.
Json settings_of(const Json& adjust) {
    Json out = Json::object();
    for (const auto& [key, value] : adjust.items()) {
        if (key != "kind" && key != "native") out[key] = value;
    }
    return out;
}

// A blend the page composites (a Lab or multichannel page: as its layers' blends, core/ink validate_ink_document).
void check_page_blend(const Json& native, const Page* page, const std::string& where) {
    const auto it = native.find("blend");
    if (page == nullptr || !page->color || it == native.end() || !it->is_string()) return;
    const std::string& blend = it->get_ref<const std::string&>();
    if (page->color->space == InkSpace::Lab && !lab_blend(blend))
        throw OpError(where + ": a Lab page cannot composite the blend " + py_repr_str(blend));
    if (page->color->space == InkSpace::Multi && !multi_blend(blend))
        throw OpError(where + ": a multichannel page cannot composite the blend " + py_repr_str(blend));
}

// A kind's native read (only the keys given, in the table's order). Op: an op's (a channel record's space filled in
// from the page when not given); Stored: a book's.
Json read_native(const AdjustKind& kind, const Json& given, ReadMode mode, const Page* page) {
    const std::string where = "adjust " + std::string(kind.name) + " native";
    Json native = read_params(kind.native, given, mode, where);
    if (native.contains("channels")) {
        if (page != nullptr && page->color && page->color->space == InkSpace::Multi)
            throw OpError(where + ".channels: a multichannel page has no channel records");
        if (!native.contains("space")) {
            if (mode == ReadMode::Stored) throw OpError(where + ".channels needs its space");
            // (its space said: a page that changes its mode keeps the records' meaning)
            Json ordered = Json::object();
            for (auto& [key, value] : native.items()) {
                if (key == "channels") ordered["space"] = std::string(default_tone_space(page));
                ordered[key] = value;
            }
            native = std::move(ordered);
        }
        native["channels"] = read_tone_channels(kind.name, native["channels"], native["space"].get<std::string>(), mode, where + ".channels");
    }
    check_page_blend(native, page, where);
    return native;
}

// A kind of the C++ build's own: its settings read (Op: clamped, the defaults filled; Stored: as they are) and
// normalized by its group's hook.
Json read_own(const AdjustKind& kind, const Json& settings, ReadMode mode, const Page* page) {
    Json read = read_params(kind.params, settings, mode, "adjust " + std::string(kind.name));
    if (mode == ReadMode::Op && kind.hooks.normalize) read = kind.hooks.normalize(std::move(read), page);
    return read;
}

// The whole of an adjust read by its kind (the caller found it): Python's settings as given, a kind of the C++ build's
// own in its normal form, and its native.
Json read_adjust(const AdjustKind& kind, const Json& adjust, ReadMode mode, const Page* page) {
    Json out = Json::object();
    out["kind"] = std::string(kind.name);
    if (kind.group == "Python") {
        for (const auto& [key, value] : adjust.items()) {
            if (key != "kind" && key != "native") out[key] = value;
        }
    } else {
        const Json own = read_own(kind, settings_of(adjust), mode, page);
        for (const auto& [key, value] : own.items()) out[key] = value;
    }
    if (const Json* given = native_in(adjust)) {
        Json native = read_native(kind, *given, mode, page);
        if (!native.empty()) out["native"] = std::move(native);
    }
    return out;
}

// The formula is there: a kind of the C++ build's own, or a native that changes Python's formula, has its group's
// function.
void require_function(const AdjustKind& kind, const Json& adjust, std::string_view what) {
    if (kind.hooks.function) return;
    if (kind.group == "Python" && python_way(adjust)) return;
    if (kind.group == "Python") adjust_not_yet("the native settings of " + std::string(what) + " " + std::string(kind.name), kind.native_group);
    adjust_not_yet(std::string(what) + " " + std::string(kind.name), kind.group);
}

}  // namespace

void adjust_not_yet(std::string_view what, std::string_view group) {
    throw Error("not_yet_ported", std::string(what) + " is not in the C++ build yet (" + std::string(group) + ")");
}

bool uses_adjustments(const Json& adjust) {
    if (!adjust.is_object()) return false;
    if (adjust.contains("native")) return true;
    const AdjustKind* kind = adjust_kind(kind_of(adjust));
    return kind != nullptr && kind->group != "Python";
}

bool uses_adjustments(const Document& doc) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        for (const Layer& layer : doc.page(i).layers) {
            if (layer.kind == LayerKind::Adjust && layer.adjust && uses_adjustments(*layer.adjust)) return true;
        }
    }
    return false;
}

AdjustNative native_of(const Json& adjust) {
    AdjustNative out;
    const Json* native = native_in(adjust);
    if (native == nullptr || !native->is_object()) return out;
    if (const auto it = native->find("blend"); it != native->end() && it->is_string()) {
        // (a name that is not a blend: as validate_adjustments refuses it, drawn as normal)
        if (const AdjustKind* any = adjust_kind("invert")) {
            const auto& blends = any->native.front().choices;
            if (std::find(blends.begin(), blends.end(), it->get<std::string>()) != blends.end()) out.blend = it->get<std::string>();
        }
    }
    if (const auto it = native->find("method"); it != native->end() && it->is_string()) out.method = it->get<std::string>();
    return out;
}

bool python_way(const Json& adjust) {
    const std::string kind = kind_of(adjust);
    if (!python_adjust_kind(kind)) return false;
    const Json* native = native_in(adjust);
    if (native == nullptr) return true;
    if (!native->is_object()) return false;
    for (const auto& [key, value] : native->items()) {
        if (key == "blend") continue;
        if (key == "method" && value == Json("genko") && (kind == "levels" || kind == "curve")) continue;
        if (key == "channels" && value.is_object() && value.empty()) continue;
        if (key == "space") continue;  // (only the channel records' space)
        if ((key == "reverse" || key == "dither") && value == Json(false) && kind == "gradient_map") continue;
        return false;
    }
    return true;
}

Json without_native(const Json& adjust) {
    if (!adjust.is_object() || !adjust.contains("native")) return adjust;
    Json out = adjust;
    out.erase("native");
    return out;
}

Json normalize_adjust(const Json& adjust, const Page* page) {
    const std::string kind = kind_of(adjust);
    const AdjustKind* found = adjust_kind(kind);
    if (found == nullptr || !found->layer) throw OpError("adjust kind " + py_repr_str(kind) + " is not a correction layer's kind");
    Json out = read_adjust(*found, adjust, ReadMode::Op, page);
    require_function(*found, out, "the correction");
    return out;
}

Json normalize_filter(std::string_view kind, const Json& params, const Page* page) {
    const AdjustKind* found = adjust_kind(kind);
    if (found == nullptr || !found->filter) throw OpError("unknown filter " + std::string(kind));
    Json adjust = params.is_object() ? params : Json::object();
    adjust["kind"] = std::string(kind);
    Json out = read_adjust(*found, adjust, ReadMode::Op, page);
    out.erase("kind");
    // (a blend lays a correction layer over what is under it: a filter changes the pixels themselves — fade_layer lays
    // them by a blend afterwards)
    if (out.contains("native") && out["native"].contains("blend"))
        throw OpError("filter " + std::string(kind) + ": native.blend is a correction layer's (fade_layer lays a filter's result in a blend)");
    return out;
}

bool native_filter(std::string_view kind, const Json& params) {
    if (params.is_object() && params.contains("native")) return true;
    const AdjustKind* found = adjust_kind(kind);
    return found != nullptr && found->group != "Python";
}

void validate_adjustments(const Document& doc) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        const Page& page = doc.page(i);
        for (const Layer& layer : page.layers) {
            if (layer.kind != LayerKind::Adjust || !layer.adjust || !uses_adjustments(*layer.adjust)) continue;
            try {
                const std::string kind = kind_of(*layer.adjust);
                const AdjustKind* found = adjust_kind(kind);
                if (found == nullptr || !found->layer) throw OpError("adjust kind " + py_repr_str(kind) + " is not a correction layer's kind");
                (void)read_adjust(*found, *layer.adjust, ReadMode::Stored, &page);
            } catch (const OpError& error) {
                throw Error("value", "the correction layer " + layer.id + ": " + error.what());
            }
        }
    }
}

AdjustPtr adjust_function(const Json& adjust, const Page& page, const Document* doc) {
    const std::string kind = kind_of(adjust);
    if (kind.empty()) return nullptr;
    const AdjustKind* found = adjust_kind(kind);
    if (found == nullptr || !found->layer) return nullptr;  // (as apply_filter's unknown filter: the layer does nothing)
    if (found->group == "Python" && python_way(adjust)) {
        Json params = settings_of(adjust);
        try {
            return std::make_shared<PreciseAdjustment>(kind, params);
        } catch (const PyValueError&) {
            return nullptr;  // (a ValueError: the layer does nothing)
        }
    }
    require_function(*found, adjust, "the correction");
    Json normal = adjust;
    if (found->group != "Python") {  // (its defaults filled: a book of another build may leave some out)
        try {
            normal = read_adjust(*found, adjust, ReadMode::Op, &page);
        } catch (const OpError&) {
            return nullptr;  // (settings it cannot read: as Python's form, the layer does nothing)
        }
    }
    return found->hooks.function(normal, page, doc);
}

AdjustPtr filter_function(std::string_view kind, const Json& params, const Page& page, const Document* doc) {
    const AdjustKind* found = adjust_kind(kind);
    if (found == nullptr || !found->filter) throw OpError("unknown filter " + std::string(kind));
    Json adjust = Json::object();
    adjust["kind"] = std::string(kind);
    for (const auto& [key, value] : params.items()) adjust[key] = value;
    if (found->group == "Python" && python_way(adjust)) return std::make_shared<PreciseAdjustment>(kind, settings_of(adjust));
    require_function(*found, adjust, "the filter");
    return found->hooks.function(adjust, page, doc);
}

}  // namespace genko::core
