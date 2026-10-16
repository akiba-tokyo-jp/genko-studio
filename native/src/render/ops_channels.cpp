// channels@1: the ops of a page's alpha and spot channels (SPEC.md COLOR-03, docs/cpp-migration/schema-v4.md「チャンネル」):
// add_channel, duplicate_channel, delete_channel, rename_channel, move_channel, set_channel, merge_spot_channel,
// split_channels and merge_channels. The C++ build's own (the public list, `genko schema`, stays Python's). Their
// defaults, names and renumbering are PhotoCraft v0.2.0's (crates/engine/src/channel_cmds.rs:781-1296, MIT,
// third_party/photocraft/NOTICE.md); its "documents" are this build's pages. The pixel ops' "channel" (the channel or
// colour plate they work on) is render/ops_registry.cpp's wrap, a selection from a channel render/selection's {"channel"}.

#include <algorithm>
#include <cmath>
#include <optional>

#include "core/channels.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/channels.hpp"
#include "render/ink.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"

namespace genko::render {

namespace {

using core::Channel;
using core::ChannelKind;
using core::Document;
using core::Json;
using core::OpContext;
using core::OpError;
using core::Page;
using channels::Plane;

constexpr int kPlaneDpi = 200;  // (the page's planes: as a paint layer's pixels, raster.WORKING_DPI)

const Json* field(const Json& op, const char* key) {
    const auto it = op.find(key);
    return it == op.end() || it->is_null() ? nullptr : &*it;
}

std::string text_of(const Json& op, const char* key, std::string fallback) {
    const Json* v = field(op, key);
    if (v == nullptr) return fallback;
    if (!v->is_string()) throw OpError(std::string(key) + " must be a string");
    return v->get<std::string>();
}

bool flag_of(const Json& op, const char* key, bool fallback) {
    const Json* v = field(op, key);
    if (v == nullptr) return fallback;
    if (!v->is_boolean()) throw OpError(std::string(key) + " is true or false");
    return v->get<bool>();
}

std::optional<double> unit_of(const Json& op, const char* key) {
    const Json* v = field(op, key);
    if (v == nullptr) return std::nullopt;
    if (!v->is_number() || !std::isfinite(v->get<double>()) || v->get<double>() < 0 || v->get<double>() > 1) {
        throw OpError(std::string(key) + " is a number 0..1");
    }
    return v->get<double>();
}

std::optional<std::array<int, 3>> colour_of(const Json& op, const char* key) {
    const Json* v = field(op, key);
    if (v == nullptr) return std::nullopt;
    if (!v->is_array() || v->size() != 3) throw OpError(std::string(key) + " is [r, g, b] (0..255 each)");
    std::array<int, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!(*v)[i].is_number_integer() || (*v)[i].get<std::int64_t>() < 0 || (*v)[i].get<std::int64_t>() > 255) {
            throw OpError(std::string(key) + " is [r, g, b] (0..255 each)");
        }
        out[i] = static_cast<int>((*v)[i].get<std::int64_t>());
    }
    return out;
}

std::string name_of(const std::string& given) {
    try {
        return core::channel_name(given);
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
}

void check_free_name(const Page& page, const std::string& name, const Channel* self = nullptr) {
    for (const Channel& channel : page.channels) {
        if (&channel != self && channel.name == name) throw OpError("the page has a channel named " + core::py_repr_str(name) + " already");
    }
}

// The alpha or spot channel op["channel"] names on the page (its id or name).
std::size_t channel_arg(const Page& page, const Json& op) {
    const Json* given = field(op, "channel");
    if (given == nullptr || !given->is_string()) throw OpError("channel (a channel's id or name) is required");
    const std::size_t at = core::channel_index(page, given->get<std::string>());
    if (at >= page.channels.size()) {
        if (core::plate_index(page, given->get<std::string>()) || given->get<std::string>() == "composite") {
            throw OpError("the colour plate " + core::py_repr_str(given->get<std::string>()) + " is not an alpha or spot channel");
        }
        throw OpError("no channel " + core::py_repr_str(given->get<std::string>()) + " on page " + page.index.repr());
    }
    return at;
}

// A name free on the page: `base`, else "base 2", "base 3", ….
std::string free_name(const Page& page, const std::string& base) {
    const auto taken = [&](const std::string& name) {
        return std::any_of(page.channels.begin(), page.channels.end(), [&](const Channel& c) { return c.name == name; });
    };
    if (!taken(base)) return base;
    for (std::size_t n = 2;; ++n) {
        const std::string name = base + " " + std::to_string(n);
        if (!taken(name)) return name;
    }
}

std::string composite_label(const Page& page) {
    if (page.color) {
        switch (page.color->space) {
            case core::InkSpace::Cmyk: return "CMYK";
            case core::InkSpace::Lab: return "Lab";
            case core::InkSpace::Gray: return "グレー";
            default: break;
        }
    }
    return "RGB";
}

bool same_size(const Page& a, const Page& b) {
    return a.spec.width_mm.value() == b.spec.width_mm.value() && a.spec.height_mm.value() == b.spec.height_mm.value();
}

core::PageColor grey_color(int depth) {
    const core::BuiltinProfile* sgray = core::builtin_profile("sgray");
    core::PageColor out;
    out.space = core::InkSpace::Gray;
    out.depth = depth;
    out.profile = sgray->ref();
    out.icc = sgray->bytes;
    return out;
}

// A page like `like` (its paper and binding; no panels, no lines) with one paint layer over it, made by `fill`.
Page new_page(const Document& doc, const Page& like, const std::string& title) {
    Page page = core::make_page(core::Num(static_cast<std::int64_t>(doc.pages.size()) + 1), like.spec, like.binding);
    page.color.reset();
    page.rgb.reset();
    core::Layer layer;
    layer.id = core::new_id();
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.title = title;
    layer.lock_alpha = true;
    layer.panel_clip = false;
    page.layers.insert(page.layers.begin() + 1, std::move(layer));  // (over the paper's layer)
    return page;
}

core::Layer& picture_layer(Page& page) { return page.layers[1]; }

// A grey page (sGray, the depth) whose paint layer is the plane, opaque (its lightness: the plane's values).
Page grey_page(const Document& doc, const Page& like, const Plane& plane, int depth, const std::string& title) {
    Page page = new_page(doc, like, title);
    page.color = grey_color(depth);
    core::InkPixels pixels = core::blank_ink_pixels(core::InkSpace::Gray, depth, plane.width, plane.height);
    for (std::size_t i = 0; i < plane.values.size(); ++i) {
        pixels.set(i, 0, std::clamp(double(plane.values[i]), 0.0, 1.0));
        pixels.set(i, 1, 1.0);
    }
    picture_layer(page).ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(pixels));
    return page;
}

// The pages made put after the page at `after` (a position in doc.pages), the book numbered again.
void insert_pages(Document& doc, std::size_t after, std::vector<Page> made) {
    std::vector<core::Num> order;
    for (const auto& page : doc.pages) order.push_back(page->index);
    std::vector<core::Num> fresh;
    for (Page& page : made) {
        page.index = core::Num(static_cast<std::int64_t>(doc.pages.size()) + 1);
        fresh.push_back(page.index);
        doc.pages.push_back(std::make_shared<Page>(std::move(page)));
    }
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(std::min(after + 1, order.size())), fresh.begin(), fresh.end());
    core::reorder_pages(doc, order);
}

// A page taken away as delete_page takes it (its lines, its lock; the others numbered again).
void remove_page(OpContext& c, const std::string& page_id) {
    for (const auto& page : c.doc.pages) {
        if (page->id != page_id) continue;
        const Json op{{"op", "delete_page"}, {"page", page->index.json()}};
        OpContext inner{c.doc, op, c.actor};
        (*core::OpRegistry::builtin().find("delete_page"))(inner);
        return;
    }
}

std::int64_t number_of(const Document& doc, const std::string& page_id) {
    for (const auto& page : doc.pages) {
        if (page->id == page_id) return core::py_int(page->index.json());
    }
    throw OpError("the page made is not in the book");
}

// --- the ops -------------------------------------------------------------------------------------------------------------

void add_channel(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const std::string kind_text = text_of(op, "kind", "alpha");
    const auto kind = core::channel_kind_from(kind_text);
    if (!kind) throw OpError("kind is alpha or spot");
    const Page& page = doc.page(at);
    if (page.channels.size() >= core::kChannelsMax) throw OpError("a page has at most " + std::to_string(core::kChannelsMax) + " channels");
    const std::string name = field(op, "name") != nullptr ? name_of(text_of(op, "name", "")) : core::next_channel_name(page, *kind);
    check_free_name(page, name);
    const bool area = field(op, "area") != nullptr;
    const std::string fill = text_of(op, "fill", *kind == ChannelKind::Spot && area ? "area" : "black");
    if (fill != "black" && fill != "white" && fill != "area") throw OpError("fill is black, white or area");
    if (fill == "area" && !area) throw OpError("fill area needs the area (the selection)");
    Channel channel;
    channel.id = core::new_channel_id();
    channel.name = name;
    channel.kind = *kind;
    if (const auto colour = colour_of(op, "color")) channel.color = *colour;
    if (*kind == ChannelKind::Alpha) {
        if (field(op, "solidity") != nullptr) throw OpError("solidity is a spot channel's");
        if (const auto opacity = unit_of(op, "opacity")) channel.opacity = *opacity;
        const std::string indicates = text_of(op, "indicates", "masked");
        if (indicates != "masked" && indicates != "selected") throw OpError("indicates is masked or selected");
        channel.indicates = indicates;
    } else {
        if (field(op, "opacity") != nullptr || field(op, "indicates") != nullptr) throw OpError("opacity and indicates are an alpha channel's");
        if (const auto solidity = unit_of(op, "solidity")) channel.solidity = *solidity;
    }
    const int dpi = core::channel_dpi(page, *kind);
    if (fill != "black") {
        Plane plane = channels::blank(page, dpi);
        if (fill == "white") {
            std::fill(plane.values.begin(), plane.values.end(), 1.0f);
        } else {
            const Image mask = selection::to_mask(op["area"], page, &doc, dpi);
            plane = channels::resized(channels::from_mask(mask), plane.width, plane.height);
        }
        channel.raster = channels::encode(plane, core::page_depth(page));
    }
    doc.edit_page(at).channels.push_back(channel);
    c.report = Json{{"channel", channel.id}};
}

void duplicate_channel(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Page& page = doc.page(at);
    const Json* given = field(op, "channel");
    if (given == nullptr || !given->is_string()) throw OpError("channel (a channel's id or name, a colour plate or composite) is required");
    const std::string source = given->get<std::string>();
    Channel made;
    Plane plane;
    std::string base;
    if (const std::size_t index = core::channel_index(page, source); index < page.channels.size()) {
        made = page.channels[index];
        plane = channels::plane_of(page, made);
        base = made.name;
    } else if (source == "composite") {
        plane = channels::page_plane(page, &doc, std::nullopt);
        base = composite_label(page);
    } else if (const auto plate = core::plate_index(page, source)) {
        const bool alone = core::plate_names(page).size() == 1;
        plane = channels::page_plane(page, &doc, alone ? std::optional<std::size_t>() : plate);
        base = alone ? composite_label(page) : core::plate_labels(page)[*plate];
    } else {
        throw OpError("no channel " + core::py_repr_str(source) + " on page " + page.index.repr());
    }
    if (flag_of(op, "invert", false)) {
        for (float& v : plane.values) v = 1.0f - std::clamp(v, 0.0f, 1.0f);
    }
    const Json* to = field(op, "to_page");
    if (to != nullptr && *to == Json("new")) {  // (PhotoCraft: a new document; here a grey page after this one)
        const std::string title = field(op, "name") != nullptr ? name_of(text_of(op, "name", "")) : base + " のコピー";
        Page grey = grey_page(doc, page, plane, core::page_depth(page), title);
        const std::string id = grey.id;
        insert_pages(doc, at, {std::move(grey)});
        c.report = Json{{"page", number_of(doc, id)}};
        return;
    }
    std::size_t into = at;
    if (to != nullptr) {
        if (!to->is_number_integer()) throw OpError("to_page is a page number or new");
        Json where = Json{{"page", *to}};
        into = core::require_page(doc, where);
        if (!same_size(doc.page(into), page)) throw OpError("page " + doc.page(into).index.repr() + " is not the same size as page " + page.index.repr());
    }
    const Page& target = doc.page(into);
    if (target.channels.size() >= core::kChannelsMax) throw OpError("a page has at most " + std::to_string(core::kChannelsMax) + " channels");
    if (field(op, "name") != nullptr) {
        made.name = name_of(text_of(op, "name", ""));
        check_free_name(target, made.name);
    } else {
        made.name = free_name(target, name_of(base + " のコピー"));
    }
    made.id = core::new_channel_id();
    made.memo = {};
    made.raster = channels::encode(plane, core::page_depth(target));
    doc.edit_page(into).channels.push_back(made);
    c.report = Json{{"channel", made.id}, {"page", doc.page(into).index.json()}};
}

// A colour plate of a page deleted (Photoshop's and PhotoCraft's Delete Channel of a colour channel, color-model@1 PC1d):
// the page made multichannel first (set_color_mode's: its pixel layers flattened, a plate for each of its own — RGB's
// red, green and blue as Cyan, Magenta and Yellow, CMYK's four, Lab's Alpha 1..3), then that plate taken out of its
// pixels and its table of inks. A multichannel page keeps one plate at least.
void delete_plate(OpContext& c, std::size_t at, std::size_t plate) {
    Document& doc = c.doc;
    const std::string page_id = doc.page(at).id;
    Json report = Json::object();
    if (!doc.page(at).color || doc.page(at).color->space != core::InkSpace::Multi) {
        if (core::plate_names(doc.page(at)).size() < 2) throw OpError("page " + doc.page(at).index.repr() + " has only one colour plate to keep");
        const Json multi{{"op", "set_color_mode"}, {"page", doc.page(at).index.json()}, {"mode", "multichannel"}};
        OpContext inner{doc, multi, c.actor};
        (*ops_registry().find("set_color_mode"))(inner);
        if (inner.report && inner.report->is_object()) report = *inner.report;
    }
    std::size_t p = 0;
    while (p < doc.pages.size() && doc.pages[p]->id != page_id) ++p;
    Page& page = doc.edit_page(p);
    core::PageColor& color = *page.color;
    if (color.plates.size() < 2) throw OpError("a multichannel page keeps one plate at least");
    if (plate >= color.plates.size()) throw OpError("the page has no such plate");
    const unsigned n = color.planes();
    for (core::Layer& layer : page.layers) {  // (its plane out of each layer's pixels)
        if (!layer.ink_raster) continue;
        const core::InkPixels was = core::decode_ink_raster(*layer.ink_raster);
        core::InkPixels now = core::blank_ink_pixels(core::InkSpace::Multi, was.depth, was.width, was.height, n - 1);
        for (std::size_t i = 0; i < was.pixel_count(); ++i) {
            unsigned k = 0;
            for (unsigned c2 = 0; c2 <= n; ++c2) {
                if (c2 == plate) continue;
                now.set(i, k++, was.value(i, c2));
            }
        }
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(now));
        layer.ink_memo = {};
    }
    for (core::InkEntry& entry : color.inks) {
        if (plate < entry.ink.size()) entry.ink.erase(entry.ink.begin() + static_cast<std::ptrdiff_t>(plate));
    }
    color.plates.erase(color.plates.begin() + static_cast<std::ptrdiff_t>(plate));
    report["plates"] = static_cast<std::int64_t>(color.plates.size());
    c.report = std::move(report);
}

void delete_channel(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Page& page = doc.page(at);
    const Json* given = field(c.op, "channel");
    if (given != nullptr && given->is_string() && core::plate_index(page, given->get<std::string>()) &&
        core::channel_index(page, given->get<std::string>()) >= page.channels.size()) {
        const std::size_t plate = *core::plate_index(page, given->get<std::string>());
        // (a multichannel page's plate goes as it is; another page's only when asked to become multichannel)
        if (flag_of(c.op, "to_multichannel", false) || (page.color && page.color->space == core::InkSpace::Multi)) {
            delete_plate(c, at, plate);
            return;
        }
        throw OpError("a colour plate is deleted only with to_multichannel: true (the page becomes multichannel)");
    }
    const std::size_t index = channel_arg(page, c.op);
    auto& list = doc.edit_page(at).channels;
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
}

void rename_channel(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::size_t index = channel_arg(doc.page(at), c.op);
    if (field(c.op, "name") == nullptr) throw OpError("name is required");
    const std::string name = name_of(text_of(c.op, "name", ""));
    check_free_name(doc.page(at), name, &doc.page(at).channels[index]);
    doc.edit_page(at).channels[index].name = name;
}

void move_channel(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::size_t index = channel_arg(doc.page(at), c.op);
    const Json* to = field(c.op, "to");
    if (to == nullptr || !to->is_number_integer() || to->get<std::int64_t>() < 0) throw OpError("to is a place in the list (0 first)");
    const std::size_t n = doc.page(at).channels.size();
    const std::size_t place = std::min<std::size_t>(static_cast<std::size_t>(to->get<std::int64_t>()), n - 1);
    if (place != index) {
        auto& list = doc.edit_page(at).channels;
        Channel moved = std::move(list[index]);
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(place), std::move(moved));
    }
    c.report = Json{{"channel", static_cast<std::int64_t>(place)}};
}

void set_channel(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const std::size_t index = channel_arg(doc.page(at), op);
    Channel channel = doc.page(at).channels[index];
    if (field(op, "name") != nullptr) {
        channel.name = name_of(text_of(op, "name", ""));
        check_free_name(doc.page(at), channel.name, &doc.page(at).channels[index]);
    }
    if (field(op, "kind") != nullptr) {
        const auto kind = core::channel_kind_from(text_of(op, "kind", ""));
        if (!kind) throw OpError("kind is alpha or spot");
        if (*kind != channel.kind) {  // (PhotoCraft's Channel Options: the other kind's settings start at their defaults)
            channel.kind = *kind;
            channel.opacity = 0.5;
            channel.indicates = "masked";
            channel.solidity = 0.0;
        }
    }
    if (const auto colour = colour_of(op, "color")) channel.color = *colour;
    if (channel.kind == ChannelKind::Alpha) {
        if (field(op, "solidity") != nullptr) throw OpError("solidity is a spot channel's");
        if (const auto opacity = unit_of(op, "opacity")) channel.opacity = *opacity;
        if (field(op, "indicates") != nullptr) {
            const std::string indicates = text_of(op, "indicates", "");
            if (indicates != "masked" && indicates != "selected") throw OpError("indicates is masked or selected");
            channel.indicates = indicates;
        }
    } else {
        if (field(op, "opacity") != nullptr || field(op, "indicates") != nullptr) throw OpError("opacity and indicates are an alpha channel's");
        if (const auto solidity = unit_of(op, "solidity")) channel.solidity = *solidity;
    }
    doc.edit_page(at).channels[index] = std::move(channel);
}

}  // namespace

namespace {

// Is the layer shown (its folders too, as merge_visible's flatten reads them: layerops._parents_visible)?
bool shown_in(const Page& page, const core::Layer& layer) {
    if (!layer.visible) return false;
    const core::Json* parent = &layer.parent_id;
    std::vector<std::string> seen;
    while (parent->is_string()) {
        const std::string id = parent->get<std::string>();
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) break;
        seen.push_back(id);
        const core::Layer* found = nullptr;
        for (const core::Layer& item : page.layers) {
            if (item.id == id) found = &item;
        }
        if (found == nullptr) break;
        if (!found->visible) return false;
        parent = &found->parent_id;
    }
    return true;
}

// Does the flatten merge or discard the layer (not a folder, nor a name or draft layer, which it keeps)?
bool flattened(const core::Layer& layer) {
    return layer.kind != core::LayerKind::Folder && layer.role != core::LayerRole::Name && layer.role != core::LayerRole::Draft;
}

// Does the layer hold something drawn (pixels, lines, patches, a fill, a correction)? A page's empty layers of its own
// roles hold nothing.
bool holds_drawing(const core::Layer& layer) {
    return (layer.raster_png && !layer.raster_png->empty()) || layer.color_raster || layer.ink_raster || layer.stroke_count() > 0 ||
           !layer.patches.empty() || (layer.kind == core::LayerKind::Fill && ((layer.fill && core::py_truthy(*layer.fill)) || layer.fill_rgb)) ||
           (layer.kind == core::LayerKind::Adjust && layer.adjust && core::py_truthy(*layer.adjust));
}

}  // namespace

// The layers merge_visible's flatten keeps in one: the first shown (as layerops picks it).
std::optional<std::string> first_shown_layer(const Page& page) {
    for (const core::Layer& layer : page.layers) {
        if (flattened(layer) && shown_in(page, layer)) return layer.id;
    }
    return std::nullopt;
}

FlattenCount flatten_count(const Page& page) {
    FlattenCount out;
    if (!first_shown_layer(page)) return out;  // (nothing shows: nothing is flattened)
    for (const core::Layer& layer : page.layers) {
        if (!flattened(layer) || !holds_drawing(layer)) continue;
        if (shown_in(page, layer)) {
            ++out.merged;
        } else {
            ++out.discarded;
        }
    }
    return out;
}

std::optional<std::string> flatten_page(OpContext& c, std::size_t at, std::int64_t* discarded) {
    const auto target = first_shown_layer(c.doc.page(at));
    if (!target) return std::nullopt;
    const int hidden = flatten_count(c.doc.page(at)).discarded;
    const Json flatten{{"op", "merge_visible"}, {"page", c.doc.page(at).index.json()}, {"copy", false}, {"flatten", true}};
    if (c.doc.strict_gates) core::check_strict(c.doc, flatten, c.actor);  // (a studio book's rules for a flatten too)
    OpContext inner{c.doc, flatten, c.actor};
    (*ops_registry().find("merge_visible"))(inner);
    if (discarded != nullptr) *discarded += hidden;
    return target;
}

namespace {

// The spot printed into a colour p (0..1): p + (s·ink + (1 − s)·p·ink − p)·v (PhotoCraft's Merge Spot Channel,
// channel_cmds.rs:1149-1198).
inline double spot_into(double p, double ink, double s, double v) { return p + (s * ink + (1 - s) * p * ink - p) * v; }

void merge_spot_channel(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    std::size_t index = 0;
    if (field(op, "channel") != nullptr) {
        index = channel_arg(doc.page(at), op);
    } else {
        const auto& list = doc.page(at).channels;
        const auto spot = std::find_if(list.begin(), list.end(), [](const Channel& ch) { return ch.kind == ChannelKind::Spot; });
        if (spot == list.end()) throw OpError("page " + doc.page(at).index.repr() + " has no spot channel");
        index = static_cast<std::size_t>(spot - list.begin());
    }
    const Channel spot = doc.page(at).channels[index];
    if (spot.kind != ChannelKind::Spot) throw OpError("the channel " + core::py_repr_str(spot.name) + " is not a spot channel");
    const std::string page_id = doc.page(at).id;
    // the page's pixel layers flattened into one (Genko's 画像をフラット化: merge_visible, its frames and lines kept as they
    // are — the user's decision: as PhotoCraft flattens), then the spot printed into it
    std::int64_t discarded = 0;
    const auto target = flatten_page(c, at, &discarded);
    if (!target) throw OpError("no layer is showing");
    std::size_t p = 0;
    while (p < doc.pages.size() && doc.pages[p]->id != page_id) ++p;
    Page& page = doc.edit_page(p);
    const auto layer = std::find_if(page.layers.begin(), page.layers.end(), [&](const core::Layer& l) { return l.id == *target; });
    if (layer == page.layers.end()) throw OpError("the flattened layer is not on the page");
    const Plane plane = channels::plane_of(page, spot);
    const double s = std::clamp(spot.solidity, 0.0, 1.0);
    const std::array<double, 3> ink_srgb{spot.color[0] / 255.0, spot.color[1] / 255.0, spot.color[2] / 255.0};
    if (layer->ink_raster && page.color) {  // (a CMYK, Lab or grey page: the colours its inks mean, and back by its rules)
        core::InkPixels pixels = core::decode_ink_raster(*layer->ink_raster);
        const Plane v = channels::resized(plane, pixels.width, pixels.height);
        const ink::Rules rules(*page.color);
        const unsigned n = pixels.colours();
        std::vector<double> inks(n), rgb(3), out(n);
        for (std::size_t i = 0; i < pixels.pixel_count(); ++i) {
            const double d = std::clamp(double(v.values[i]), 0.0, 1.0);
            if (!(d > 0)) continue;
            for (unsigned k = 0; k < n; ++k) {
                const double raw = pixels.value(i, k);
                inks[k] = page.color->space == core::InkSpace::Lab ? (k == 0 ? raw * 100.0 : raw * 255.0 - 128.0)
                          : page.color->space == core::InkSpace::Gray ? 1.0 - raw
                                                                      : raw;
            }
            rules.meaning(inks.data(), rgb.data(), 1);
            for (unsigned k = 0; k < 3; ++k) rgb[k] = std::clamp(spot_into(rgb[k], ink_srgb[k], s, d), 0.0, 1.0);
            rules.inks(rgb.data(), out.data(), 1);
            for (unsigned k = 0; k < n; ++k) {
                const double value = page.color->space == core::InkSpace::Lab ? (k == 0 ? out[k] / 100.0 : (out[k] + 128.0) / 255.0)
                                     : page.color->space == core::InkSpace::Gray ? 1.0 - out[k]
                                                                                 : out[k];
                pixels.set(i, k, value);
            }
        }
        layer->ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(pixels));
        layer->ink_memo = {};
    } else if (layer->color_raster) {  // (precise RGB: the page's values, the ink in the page's space)
        std::vector<float> ink{float(ink_srgb[0]), float(ink_srgb[1]), float(ink_srgb[2]), 1.0f};
        if (page.rgb && !page.rgb->profile.empty()) ink::rgb_to_rgb(ink, nullptr, *page.rgb);
        core::ColorRasterEdit edit{std::string(*layer->color_raster)};
        const Plane v = channels::resized(plane, edit.width(), edit.height());
        for (std::size_t i = 0; i < std::size_t(edit.width()) * edit.height(); ++i) {
            const double d = std::clamp(double(v.values[i]), 0.0, 1.0);
            if (!(d > 0)) continue;
            std::array<double, 4> px = edit.pixel(i);
            for (unsigned k = 0; k < 3; ++k) px[k] = spot_into(px[k], ink[k], s, d);
            edit.set(i, px);
        }
        layer->color_raster = std::make_shared<const std::string>(std::move(edit).take());
    } else if (layer->raster_png && !layer->raster_png->empty()) {  // (8 bits: sRGB, or the page's values on a page of a profile)
        std::vector<float> ink{float(ink_srgb[0]), float(ink_srgb[1]), float(ink_srgb[2]), 1.0f};
        if (page.rgb && !page.rgb->profile.empty()) ink::rgb_to_rgb(ink, nullptr, *page.rgb);
        const Image picture = open_image(*layer->raster_png, kPillowOpenLimits).convert("RGBA");
        std::string bytes = picture.tobytes();
        const Plane v = channels::resized(plane, static_cast<std::uint32_t>(picture.width()), static_cast<std::uint32_t>(picture.height()));
        for (std::size_t i = 0; i < v.values.size(); ++i) {
            const double d = std::clamp(double(v.values[i]), 0.0, 1.0);
            if (!(d > 0)) continue;
            for (unsigned k = 0; k < 3; ++k) {
                const double px = static_cast<unsigned char>(bytes[i * 4 + k]) / 255.0;
                bytes[i * 4 + k] = static_cast<char>(std::lround(std::clamp(spot_into(px, ink[k], s, d), 0.0, 1.0) * 255.0));
            }
        }
        layer->raster_png = std::make_shared<const std::string>(write_png(Image::frombytes("RGBA", picture.size(), bytes)));
        layer->raster_memo = {};
    } else {
        throw OpError("the flattened layer holds no pixels");
    }
    const std::size_t again = core::channel_index(page, spot.id);
    if (again < page.channels.size()) page.channels.erase(page.channels.begin() + static_cast<std::ptrdiff_t>(again));
    c.report = Json{{"layer", *target}};
    if (discarded > 0) (*c.report)["discarded"] = discarded;  // (the hidden layers the flatten left out)
}

void split_channels(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const bool replace = flag_of(c.op, "replace", true);  // (the user's decision: as PhotoCraft closes the original)
    const Page& page = doc.page(at);
    const std::vector<std::string> labels = core::plate_labels(page);
    if (labels.size() + page.channels.size() < 2) throw OpError("page " + page.index.repr() + " has only one channel to split");
    const int depth = core::page_depth(page);
    std::vector<Page> made;
    for (std::size_t k = 0; k < labels.size(); ++k) {
        const Plane plane = channels::page_plane(page, &doc, labels.size() == 1 ? std::optional<std::size_t>() : std::optional<std::size_t>(k));
        made.push_back(grey_page(doc, page, plane, depth, labels[k]));
    }
    for (const Channel& channel : page.channels) made.push_back(grey_page(doc, page, channels::plane_of(page, channel), depth, channel.name));
    std::vector<std::string> ids;
    for (const Page& p : made) ids.push_back(p.id);
    const std::string source = page.id;
    insert_pages(doc, at, std::move(made));
    if (replace) remove_page(c, source);
    Json numbers = Json::array();
    for (const std::string& id : ids) numbers.push_back(number_of(doc, id));
    c.report = Json{{"pages", std::move(numbers)}};
}

// A CMYK profile for merge_channels: a built-in's id, or sha256:… of one a page of the book holds (coated-cmyk: none).
core::PageColor cmyk_color(const Document& doc, const Json& op, int depth) {
    const std::string name = text_of(op, "profile", "coated-cmyk");
    core::PageColor out;
    out.space = core::InkSpace::Cmyk;
    out.depth = depth;
    if (const core::BuiltinProfile* builtin = core::builtin_profile(name); builtin != nullptr) {
        if (builtin->space != core::ProfileSpace::Cmyk) throw OpError("profile " + core::py_repr_str(name) + " is not a CMYK profile");
        out.profile = builtin->ref();
        out.icc = builtin->bytes;
        return out;
    }
    for (const auto& page : doc.pages) {
        if (page->color && page->color->space == core::InkSpace::Cmyk && page->color->profile == name && page->color->icc) {
            out.profile = name;
            out.icc = page->color->icc;
            return out;
        }
    }
    throw OpError("profile is a built-in CMYK profile's id or sha256:… of one the book holds");
}

void merge_channels(OpContext& c) {
    Document& doc = c.doc;
    const Json& op = c.op;
    const std::string mode = text_of(op, "mode", "rgb");
    if (mode != "rgb" && mode != "cmyk" && mode != "lab") throw OpError("mode is rgb, cmyk or lab");
    const std::size_t wanted = mode == "cmyk" ? 4 : 3;
    const Json* given = field(op, "pages");
    if (given == nullptr || !given->is_array()) throw OpError("pages (a list of page numbers) is required");
    if (given->size() < wanted) {
        throw OpError(mode + " needs " + std::to_string(wanted) + " grey pages, got " + std::to_string(given->size()));
    }
    std::vector<std::size_t> sources;
    for (const Json& number : *given) {
        if (!number.is_number_integer()) throw OpError("pages is a list of page numbers");
        const std::size_t at = core::require_page(doc, Json{{"page", number}});
        if (std::find(sources.begin(), sources.end(), at) != sources.end()) throw OpError("page " + doc.page(at).index.repr() + " is in pages twice");
        sources.push_back(at);
    }
    const Page& first = doc.page(sources.front());
    for (const std::size_t at : sources) {
        const Page& page = doc.page(at);
        if (!page.color || page.color->space != core::InkSpace::Gray) throw OpError("page " + page.index.repr() + " is not a grey page");
        if (!same_size(page, first)) throw OpError("page " + page.index.repr() + " is not the same size as page " + first.index.repr());
    }
    const int depth = first.color->depth;
    std::vector<Plane> planes;
    for (const std::size_t at : sources) planes.push_back(channels::page_plane(doc.page(at), &doc, std::nullopt));
    const std::uint32_t w = planes.front().width, h = planes.front().height;
    for (Plane& plane : planes) plane = channels::resized(plane, w, h);
    Page merged = new_page(doc, first, mode == "rgb" ? "RGB" : mode == "cmyk" ? "CMYK" : "Lab");
    core::Layer& layer = picture_layer(merged);
    const auto v = [&](std::size_t plane, std::size_t i) { return std::clamp(double(planes[plane].values[i]), 0.0, 1.0); };
    if (mode == "rgb") {
        if (depth == 8) {
            std::string rgba(std::size_t(w) * h * 4, '\xff');
            for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
                for (std::size_t k = 0; k < 3; ++k) rgba[i * 4 + k] = static_cast<char>(std::lround(v(k, i) * 255.0));
            }
            layer.raster_png = std::make_shared<const std::string>(write_png(Image::frombytes("RGBA", Size{int(w), int(h)}, rgba)));
        } else {
            core::RgbMode rgb;
            rgb.depth = depth;
            merged.rgb = rgb;
            layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(
                w, h, depth == 16 ? "u16" : "f32", [&](std::size_t i) { return std::array<double, 4>{v(0, i), v(1, i), v(2, i), 1.0}; }));
        }
    } else {
        core::PageColor color;
        if (mode == "cmyk") {
            color = cmyk_color(doc, op, depth);
        } else {
            color.space = core::InkSpace::Lab;
            color.depth = depth;
        }
        merged.color = color;
        core::InkPixels pixels = core::blank_ink_pixels(color.space, depth, w, h);
        for (std::size_t i = 0; i < pixels.pixel_count(); ++i) {
            for (unsigned k = 0; k < wanted; ++k) pixels.set(i, k, mode == "cmyk" ? 1.0 - v(k, i) : v(k, i));  // (CMYK: dark is ink)
            pixels.set(i, static_cast<unsigned>(wanted), 1.0);
        }
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(pixels));
    }
    // (more pages than the mode's planes: alpha channels of the page made)
    for (std::size_t k = wanted; k < planes.size(); ++k) {
        Channel channel;
        channel.id = core::new_channel_id();
        channel.name = core::next_channel_name(merged, ChannelKind::Alpha);
        channel.raster = channels::encode(planes[k], depth);
        merged.channels.push_back(std::move(channel));
    }
    const bool replace = flag_of(op, "replace", true);  // (the user's decision: as PhotoCraft closes the sources)
    std::vector<std::string> source_ids;
    for (const std::size_t at : sources) source_ids.push_back(doc.page(at).id);
    const std::string id = merged.id;
    insert_pages(doc, *std::max_element(sources.begin(), sources.end()), {std::move(merged)});
    if (replace) {
        for (const std::string& source : source_ids) remove_page(c, source);
    }
    c.report = Json{{"page", number_of(doc, id)}};
}

}  // namespace

void register_channel_ops(core::OpRegistry& registry) {
    registry.add("add_channel", add_channel);
    registry.add("duplicate_channel", duplicate_channel);
    registry.add("delete_channel", delete_channel);
    registry.add("rename_channel", rename_channel);
    registry.add("move_channel", move_channel);
    registry.add("set_channel", set_channel);
    registry.add("merge_spot_channel", merge_spot_channel);
    registry.add("split_channels", split_channels);
    registry.add("merge_channels", merge_channels);
}

}  // namespace genko::render
