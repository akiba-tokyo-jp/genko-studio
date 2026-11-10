// PC5 I-c (artboards@1): add_artboard, artboard_from_layers, set_artboard (render/ops_artboards.hpp). After PhotoCraft
// v0.2.0's engine/src/artboard_cmds.rs (146-320, MIT, native/third_party/photocraft/NOTICE.md): there an artboard is a
// board on one canvas; here a page of the book (the user's decision 3).

#include "render/ops_artboards.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/anim.hpp"
#include "core/artboards.hpp"
#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/layer_bounds.hpp"
#include "render/op_limits.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"

namespace genko::render {

namespace {

using core::Document;
using core::Json;
using core::Num;
using core::OpError;
using core::Page;

const Json* given(const Json& op, std::string_view key) {
    const Json* value = core::get(op, key);
    return value == nullptr || value->is_null() ? nullptr : value;
}

std::optional<std::int64_t> pixels_at(const Json& op, std::string_view key) {
    const Json* value = given(op, key);
    if (value == nullptr) return std::nullopt;
    if (!value->is_number_integer() || value->is_boolean()) throw OpError(std::string(key) + " is a whole number of pixels");
    const std::int64_t v = value->get<std::int64_t>();
    if (v < 1 || v > limits::kSide) throw OpError(std::string(key) + " is 1 to " + std::to_string(limits::kSide) + " pixels");
    return v;
}

std::string text_at(const Json& op, std::string_view key) {
    const Json* value = given(op, key);
    if (value == nullptr) return {};
    if (!value->is_string()) throw OpError(std::string(key) + " must be text");
    return value->get<std::string>();
}

void run(Document& doc, const Json& sub, const core::Actor& actor, std::vector<std::string>& notes, std::optional<Json>* report = nullptr) {
    core::OpContext inner{doc, sub, actor};
    (*ops_registry().find(sub.at("op").get<std::string>()))(inner);
    notes.insert(notes.end(), inner.notes.begin(), inner.notes.end());
    if (report != nullptr) *report = std::move(inner.report);
}

// The background an op gives over `into` (background white | black | transparent | custom, rgb [r, g, b]: custom's; an rgb
// alone makes it custom, PhotoCraft's "color").
void background_into(const Json& op, core::Artboard& into) {
    const std::string background = text_at(op, "background");
    const Json* rgb = given(op, "rgb");
    if (!background.empty()) {
        if (background != "white" && background != "black" && background != "transparent" && background != "custom") {
            throw OpError("background is white, black, transparent or custom");
        }
        if (background == "custom" && rgb == nullptr) throw OpError("a custom background takes its colour: rgb [r, g, b]");
        if (background != "custom" && rgb != nullptr) throw OpError("rgb is a custom background's colour");
        into.background = background;
    } else if (rgb != nullptr) {
        into.background = "custom";
    }
    if (into.background == "custom" && rgb != nullptr) {
        const std::vector<std::int64_t> v = core::rgb3(*rgb, "rgb");
        into.rgb = std::array<int, 3>{static_cast<int>(v[0]), static_cast<int>(v[1]), static_cast<int>(v[2])};
    }
    if (into.background != "custom") into.rgb.reset();
}

std::string name_at(const Json& op) {
    const std::string name = text_at(op, "name");
    if (name.size() > core::kArtboardNameMax) throw OpError("name is at most " + std::to_string(core::kArtboardNameMax) + " bytes");
    return name;
}

// The paper of an artboard of `w` × `h` pixels at `dpi`: its finished size the paper, no bleed, its basic frame the paper.
core::PageSpec artboard_spec(const core::PageSpec& book, std::int64_t w, std::int64_t h, double dpi) {
    core::PageSpec spec = book;
    spec.width_mm = Num(static_cast<double>(w) * 25.4 / dpi);
    spec.height_mm = Num(static_cast<double>(h) * 25.4 / dpi);
    spec.bleed_mm = Num(std::int64_t{0});
    spec.inner_margin_mm = Num(std::int64_t{0});
    spec.trim_w_mm = spec.width_mm;
    spec.trim_h_mm = spec.height_mm;
    spec.margins_mm = core::NumList{Num(0.0), Num(0.0), Num(0.0), Num(0.0)};
    spec.trim_origin_mm.reset();
    spec.preset = "custom";
    spec.expression = "color";
    return spec;
}

// The page's pixels across and down at its resolution.
std::array<std::int64_t, 2> page_pixels(const Page& page) {
    const int dpi = static_cast<int>(std::llround(page.spec.dpi.value()));
    return {mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
}

// The size an op gives (preset, then width_px and height_px over it), or `fallback`; the preset's name in `preset`.
std::array<std::int64_t, 2> size_given(const Json& op, double dpi, std::array<std::int64_t, 2> fallback, std::optional<std::string>& preset) {
    std::array<std::int64_t, 2> size = fallback;
    if (const std::string name = text_at(op, "preset"); !name.empty()) {
        const core::ArtboardPreset* found = core::artboard_preset(name);
        if (found == nullptr) throw OpError("no artboard preset " + name);
        size = core::preset_pixels(*found, dpi);
        preset = std::string(found->name);
    }
    if (const auto w = pixels_at(op, "width_px")) size[0] = *w;
    if (const auto h = pixels_at(op, "height_px")) size[1] = *h;
    limits::check_picture(size[0], size[1], "the artboard");
    return size;
}

double book_dpi(const Document& doc, const Json& op) {
    const double dpi = doc.spec.dpi.value();
    if (const Json* value = given(op, "dpi")) {
        if (!value->is_number() || value->is_boolean() || std::fabs(value->get<double>() - dpi) > 1e-9) {
            throw OpError("the resolution is the whole book's (" + doc.spec.dpi.repr() + " dpi): change it with resize_pages");
        }
    }
    return dpi;
}

std::size_t index_of_id(const Document& doc, const std::string& id) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.pages[i]->id == id) return i;
    }
    throw OpError("the new page is gone");
}

// --- add_artboard ------------------------------------------------------------------------------------------------------

void add_artboard(core::OpContext& c) {
    Document& doc = c.doc;
    const double dpi = book_dpi(doc, c.op);
    std::optional<std::string> preset;
    const std::array<std::int64_t, 2> book_px{mm_to_px(doc.spec.width_mm.value(), static_cast<int>(std::llround(dpi))),
                                              mm_to_px(doc.spec.height_mm.value(), static_cast<int>(std::llround(dpi)))};
    const std::array<std::int64_t, 2> size = size_given(c.op, dpi, book_px, preset);
    if (given(c.op, "width_px") != nullptr || given(c.op, "height_px") != nullptr) {
        // (a preset's size changed by hand is no more the preset's: PhotoCraft's)
        if (preset) {
            const auto made = core::preset_pixels(*core::artboard_preset(*preset), dpi);
            if (made != size) preset.reset();
        }
    }
    core::Artboard artboard;
    artboard.name = name_at(c.op);
    background_into(c.op, artboard);
    artboard.preset = preset;
    if (artboard.name.empty()) artboard.name = core::next_artboard_name(doc);
    std::set<std::string> before;
    for (const auto& page : doc.pages) before.insert(page->id);
    Json add{{"op", "add_page"}, {"count", 1}};
    if (const Json* after = given(c.op, "after")) add["after"] = *after;
    run(doc, add, c.actor, c.notes);
    std::size_t at = doc.pages.size();
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (!before.contains(doc.pages[i]->id)) at = i;
    }
    if (at == doc.pages.size()) throw OpError("no page was added");
    {
        Page& page = doc.edit_page(at);
        page.spec = artboard_spec(doc.spec, size[0], size[1], dpi);
        page.own_spec = true;
        page.frames.clear();
        page.layers.clear();
        page.numero = false;
        page.artboard = std::move(artboard);
    }
    const Json index = doc.page(at).index.json();
    run(doc, Json{{"op", "add_layer"}, {"page", index}, {"kind", "paint"}, {"name", "レイヤー 1"}}, c.actor, c.notes);
    const Page& made = doc.page(index_of_id(doc, doc.page(at).id));
    c.report = Json{{"page", made.index.json()}, {"name", made.artboard->name}, {"size_px", Json::array({size[0], size[1]})}};
}

// --- artboard_from_layers ------------------------------------------------------------------------------------------------

void artboard_from_layers(core::OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* ids_given = given(c.op, "ids");
    if (ids_given == nullptr || !ids_given->is_array() || ids_given->empty()) throw OpError("ids is a list of the page's layer ids");
    const Page& source = doc.page(at);
    const std::string source_id = source.id;
    std::vector<std::string> ids;
    for (const Json& id : *ids_given) {
        if (!id.is_string()) throw OpError("ids is a list of the page's layer ids");
        (void)core::layer_by_id(source, id.get<std::string>());
        if (std::find(ids.begin(), ids.end(), id.get<std::string>()) == ids.end()) ids.push_back(id.get<std::string>());
    }
    const bool remove = !given(c.op, "remove") || core::py_truthy(c.op.at("remove"));
    core::Artboard artboard;
    artboard.name = name_at(c.op);
    background_into(c.op, artboard);
    if (artboard.name.empty()) {
        const core::Layer& first = source.layers[core::layer_by_id(source, ids.front())];
        artboard.name = ids.size() == 1 && first.kind == core::LayerKind::Folder && !first.title.empty() ? first.title : core::next_artboard_name(doc);
    }
    // (what is taken: the layers, a folder with every layer in it)
    std::set<std::string> taken(ids.begin(), ids.end());
    for (const std::string& id : ids) {
        for (const std::string& inside : layer_bounds::inside(source, id)) taken.insert(inside);
    }
    // (a studio book's rules — strict_gates, the review's P2-1: the layers taken off the page are deleted from it (or, remove
    // false, copied) as delete_layers or duplicate_layers would, its name approved for a printed one; the new page has no name
    // approved, so a printed layer put on it is refused, as duplicate_layers onto a page without name_ok is)
    if (doc.strict_gates) {
        Json ids_json = Json::array();
        for (const std::string& id : ids) ids_json.push_back(id);
        core::check_strict(doc, Json{{"op", remove ? "delete_layers" : "duplicate_layers"}, {"page", source.index.json()}, {"ids", ids_json}}, c.actor);
        for (const core::Layer& layer : source.layers) {
            if (!taken.contains(layer.id) || layer.role == core::LayerRole::Name || layer.role == core::LayerRole::Draft) continue;
            throw OpError("artboard_from_layers of a printed layer onto a new page needs name_ok there (strict_gates)");
        }
    }
    // (the box: what they show, on the paper; none — nothing shown — the paper)
    const double pw = source.spec.width_mm.value(), ph = source.spec.height_mm.value();
    std::optional<layer_bounds::Box> box;
    for (const std::string& id : ids) {
        const auto held = layer_bounds::of_layer(doc, source, id);
        if (!held || (*held)[2] <= (*held)[0] || (*held)[3] <= (*held)[1]) continue;
        box = box ? layer_bounds::Box{std::min((*box)[0], (*held)[0]), std::min((*box)[1], (*held)[1]), std::max((*box)[2], (*held)[2]),
                                      std::max((*box)[3], (*held)[3])}
                  : *held;
    }
    layer_bounds::Box cut{0.0, 0.0, pw, ph};
    if (box) cut = {std::max(0.0, (*box)[0]), std::max(0.0, (*box)[1]), std::min(pw, (*box)[2]), std::min(ph, (*box)[3])};
    if (cut[2] - cut[0] <= 1e-6 || cut[3] - cut[1] <= 1e-6) cut = {0.0, 0.0, pw, ph};
    // (the new page: the page's paper and mode, the layers taken with new ids — nothing else of it: no panels, lines, rulers,
    // channels, paths, comps, slices, notes or measuring)
    Page made = source;
    made.id = core::new_page_id();
    made.index = Num(static_cast<std::int64_t>(doc.pages.size()) + 1);
    made.frames.clear();
    made.selected_frame_id = nullptr;
    made.note.clear();
    made.name_ok = false;
    made.stage = "name";
    made.art_ok = false;
    made.plan.reset();
    made.fills.clear();
    made.spread_with.reset();
    made.effects = Json::array();
    made.ruler.reset();
    made.rulers = Json::array();
    made.prims = Json::array();
    made.numero = false;
    made.onion_from.reset();
    made.lt_threshold.reset();
    for (const char* key : {"cover", "anim", "saved_areas", "quick_mask"}) made.extra.erase(key);
    // (PC6 review B P2-4: the animation folders taken — a video layer's too — bring their rows: the page's timeline (its
    // speed, length, loop and work area) with only their tracks and the cels of the light table taken. The camera work is
    // the page's view, not theirs: it stays. The rows on the page go with the folders deleted, as delete_layers takes them)
    if (const Json* anim = core::anim::spec(source)) {
        Json tracks = Json::array();
        if (const auto all = anim->find("tracks"); all != anim->end() && all->is_array()) {
            for (const Json& track : *all) {
                const Json* folder = track.is_object() ? core::get(track, "folder") : nullptr;
                if (folder != nullptr && folder->is_string() && taken.contains(folder->get<std::string>())) tracks.push_back(track);
            }
        }
        if (!tracks.empty()) {
            Json moved = *anim;
            moved["tracks"] = std::move(tracks);
            moved.erase("camera");
            if (const auto light = moved.find("light_table"); light != moved.end() && light->is_array()) {
                Json kept = Json::array();
                for (const Json& cel : *light)
                    if (cel.is_string() && taken.contains(cel.get<std::string>())) kept.push_back(cel);
                *light = std::move(kept);
            }
            made.extra["anim"] = std::move(moved);
        }
    }
    made.channels.clear();
    made.paths.clear();
    made.work_path.reset();
    made.clipping_path.reset();
    made.native_psd.reset();
    made.native_source.reset();  // (the file the page was opened from is not this new page's: the review's P3-7)
    made.layer_comps.reset();
    made.slices.reset();
    made.annotations.clear();
    made.measure.reset();
    made.artboard.reset();
    std::vector<core::Layer> layers;
    core::FrameIdMap renamed;
    for (const core::Layer& layer : source.layers) {
        if (!taken.contains(layer.id)) continue;
        core::Layer copy = layer;
        copy.id = core::new_id();
        renamed.set(layer.id, copy.id);
        copy.frame_id.reset();
        if (copy.parent_id.is_string() && !taken.contains(copy.parent_id.get<std::string>())) copy.parent_id = nullptr;
        layers.push_back(std::move(copy));
    }
    made.layers = std::move(layers);
    core::remap_layer_refs(made, {}, renamed);
    // (right after the page — after the last page that is not a cover when the page is one, as add_page puts a new page:
    // never among the covers, the review's P3-8)
    std::optional<Num> after = source.index;
    if (core::is_cover(source)) {
        after.reset();
        for (const auto& page : doc.pages)
            if (!core::is_cover(*page) && (!after || *after < page->index)) after = page->index;
    }
    const std::string made_id = made.id;
    doc.pages.push_back(std::make_shared<Page>(std::move(made)));
    {
        std::vector<Num> order;
        if (!after) order.push_back(doc.pages.back()->index);
        for (std::size_t k = 0; k + 1 < doc.pages.size(); ++k) {
            order.push_back(doc.pages[k]->index);
            if (after && doc.pages[k]->index == *after) order.push_back(doc.pages.back()->index);
        }
        core::reorder_pages(doc, order);
    }
    const Json index = doc.page(index_of_id(doc, made_id)).index.json();
    const auto r4 = [](double v) { return core::py_round(v, 4); };
    run(doc,
        Json{{"op", "crop_page"},
             {"page", index},
             {"rect", Json::array({r4(cut[0]), r4(cut[1]), r4(cut[2] - cut[0]), r4(cut[3] - cut[1])})},
             {"trim", "fit"},
             {"bleed_mm", 0},
             {"margins", Json::array({0, 0, 0, 0})}},
        c.actor, c.notes);
    {
        Page& page = doc.edit_page(index_of_id(doc, made_id));
        page.own_spec = true;
        page.artboard = std::move(artboard);
    }
    Json kept = Json::array();
    if (remove) {
        std::optional<Json> report;
        Json ids_json = Json::array();
        for (const std::string& id : ids) ids_json.push_back(id);
        run(doc, Json{{"op", "delete_layers"}, {"page", doc.page(index_of_id(doc, source_id)).index.json()}, {"ids", ids_json}}, c.actor,
            c.notes, &report);
        if (report && report->is_object() && report->contains("kept")) kept = (*report)["kept"];
    }
    const Page& done = doc.page(index_of_id(doc, made_id));
    c.report = Json{{"page", done.index.json()},
                    {"name", done.artboard->name},
                    {"rect_mm", Json::array({r4(cut[0]), r4(cut[1]), r4(cut[2] - cut[0]), r4(cut[3] - cut[1])})},
                    {"kept", std::move(kept)}};
}

// --- set_artboard ----------------------------------------------------------------------------------------------------------

std::pair<double, double> anchor_factors(const std::string& anchor) {
    static const std::pair<const char*, std::pair<double, double>> kAnchors[] = {
        {"top_left", {0, 0}}, {"top", {0.5, 0}},         {"top_right", {1, 0}}, {"left", {0, 0.5}},      {"center", {0.5, 0.5}},
        {"right", {1, 0.5}},  {"bottom_left", {0, 1}},   {"bottom", {0.5, 1}},  {"bottom_right", {1, 1}}};
    for (const auto& [name, factors] : kAnchors) {
        if (anchor == name) return factors;
    }
    throw OpError("anchor is top_left, top, top_right, left, center, right, bottom_left, bottom or bottom_right");
}

void set_artboard(core::OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Page& page = doc.page(at);
    if (const Json* mark = core::get(c.op, "artboard"); mark != nullptr) {
        if (!mark->is_null()) throw OpError("artboard is null (the page an artboard no more); its settings are this op's own keys");
        for (const char* key : {"name", "preset", "background", "rgb", "width_px", "height_px", "anchor", "move_contents", "origin_px"}) {
            if (given(c.op, key) != nullptr) throw OpError(std::string(key) + " cannot go with artboard null");
        }
        if (!page.artboard) throw OpError("page " + page.index.repr() + " is not an artboard");
        doc.edit_page(at).artboard.reset();
        c.report = Json{{"page", page.index.json()}, {"artboard", nullptr}};
        return;
    }
    if (!page.artboard) throw OpError("page " + page.index.repr() + " is not an artboard (make one with add_artboard or artboard_from_layers)");
    if (const auto why = core::artboard_refusal(page)) throw OpError(*why);
    core::Artboard artboard = *page.artboard;
    if (const std::string name = name_at(c.op); !name.empty()) artboard.name = name;
    else if (given(c.op, "name") != nullptr) throw OpError("name must not be empty");
    background_into(c.op, artboard);
    if (const Json* origin = given(c.op, "origin_px")) {
        if (!origin->is_array() || origin->size() != 2 || !(*origin)[0].is_number_integer() || !(*origin)[1].is_number_integer()) {
            throw OpError("origin_px is [x, y], whole numbers of pixels");
        }
        artboard.origin_px = std::array<std::int64_t, 2>{(*origin)[0].get<std::int64_t>(), (*origin)[1].get<std::int64_t>()};
    }
    const std::string anchor = given(c.op, "anchor") != nullptr ? text_at(c.op, "anchor") : std::string("top_left");
    const auto [ax, ay] = anchor_factors(anchor);
    const Json* move_given = given(c.op, "move_contents");
    if (move_given != nullptr && !move_given->is_boolean()) throw OpError("move_contents is true or false");
    const bool move_contents = move_given == nullptr || move_given->get<bool>();
    const double dpi = page.spec.dpi.value();
    const std::array<std::int64_t, 2> now = page_pixels(page);
    std::optional<std::string> preset;
    const std::array<std::int64_t, 2> size = size_given(c.op, dpi, now, preset);
    if (preset) {
        artboard.preset = preset;
        if (given(c.op, "width_px") != nullptr || given(c.op, "height_px") != nullptr) {
            if (core::preset_pixels(*core::artboard_preset(*preset), dpi) != size) artboard.preset.reset();
        }
    } else if (size != now) {
        artboard.preset.reset();  // (a size changed by hand: PhotoCraft's)
    }
    const Json index = page.index.json();
    if (size != now) {
        // (Canvas Size from the anchor; move_contents false: the contents kept at the top left, the board's place on a
        // PSD's canvas moved by what the anchor would have moved them)
        const std::int64_t dx = std::llround(static_cast<double>(size[0] - now[0]) * ax), dy = std::llround(static_cast<double>(size[1] - now[1]) * ay);
        run(doc,
            Json{{"op", "set_canvas_size"},
                 {"page", index},
                 {"width_px", size[0]},
                 {"height_px", size[1]},
                 {"anchor", move_contents ? anchor : std::string("top_left")},
                 {"trim", "fit"},
                 {"bleed_mm", 0},
                 {"margins", Json::array({0, 0, 0, 0})}},
            c.actor, c.notes);
        if (artboard.origin_px && given(c.op, "origin_px") == nullptr) {
            (*artboard.origin_px)[0] -= dx;
            (*artboard.origin_px)[1] -= dy;
            for (std::int64_t& v : *artboard.origin_px) v = std::clamp(v, -core::kArtboardOriginMax, core::kArtboardOriginMax);
        }
    }
    try {
        core::check_artboard(artboard);
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
    Page& edited = doc.edit_page(at);
    edited.own_spec = true;
    edited.artboard = std::move(artboard);
    c.report = Json{{"page", index}, {"name", edited.artboard->name}, {"size_px", Json::array({size[0], size[1]})}};
}

}  // namespace

void register_artboard_ops(core::OpRegistry& registry) {
    registry.add("add_artboard", add_artboard);
    registry.add("artboard_from_layers", artboard_from_layers);
    registry.add("set_artboard", set_artboard);
}

}  // namespace genko::render
