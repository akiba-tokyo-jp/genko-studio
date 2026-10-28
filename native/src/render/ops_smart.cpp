// smart-objects@1 (PC3 A0, docs/cpp-migration/SPEC.md「スマートオブジェクト」): the ops that make and change smart objects
// (this build's own: `genko schema` and the MCP tools' list stay as they are). PhotoCraft v0.2.0's
// engine/src/smart_cmds.rs:476-539 (what converting a layer moves onto the smart object), file_cmds.rs:288-331 (where a
// placed file lands), layer_menu_cmds.rs:312-326 (a stack mode, PC3 A4) and type_cmds.rs:584 (a type layer rasterized:
// here a line, PC3 A4), MIT (third_party/photocraft/NOTICE.md). The ops of PC3's other steps are registered by their
// files (A1 render/ops_smart_contents.cpp, A2 render/ops_smart_filters.cpp).

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/placement.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "core/strokes.hpp"
#include "render/color_canvas.hpp"
#include "render/imaging.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/layer_transform.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_registry.hpp"
#include "render/resample.hpp"
#include "render/ops_shapes.hpp"
#include "render/ops_smart_parts.hpp"
#include "render/shape_parts.hpp"
#include "render/page.hpp"
#include "render/page_internal.hpp"
#include "render/pcraft_read.hpp"
#include "render/placed.hpp"
#include "render/proof.hpp"
#include "render/raster.hpp"
#include "render/smart.hpp"
#include "render/screentone.hpp"
#include "render/raw.hpp"
#include "render/smart_decode.hpp"
#include "render/smart_filters.hpp"
#include "render/text/balloons.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"
#include "render/tones.hpp"
#include "render/vector_masks.hpp"
#include "storage/asset_store.hpp"

namespace genko::render {

namespace {

using core::Document;
using core::Json;
using core::Layer;
using core::LayerKind;
using core::LayerRole;
using core::Num;
using core::OpContext;
using core::OpError;
using core::Page;
using core::smart::Matrix;

std::string text_at(const Json& op, std::string_view key, std::string fallback = {}) {
    const Json* v = core::get(op, key);
    if (v == nullptr || v->is_null()) return fallback;
    if (!v->is_string()) throw OpError(std::string(key) + " must be text");
    return v->get<std::string>();
}

double number_at(const Json& op, std::string_view key, double fallback) {
    const Json* v = core::get(op, key);
    if (v == nullptr || v->is_null()) return fallback;
    if (!v->is_number() || v->is_boolean() || !std::isfinite(v->get<double>())) throw OpError(std::string(key) + " must be a finite number");
    return v->get<double>();
}

bool guide(LayerRole role) { return role == LayerRole::Name || role == LayerRole::Draft; }

// The smart object layer an op names by "id" on its page.
std::size_t smart_layer(const Page& page, const Json& op) {
    const std::size_t at = core::layer_by_id(page, core::truthy_at(op, "id") ? core::py_str(op["id"]) : std::string());
    if (page.layers[at].kind != LayerKind::Smart) throw OpError("layer " + page.layers[at].id + " is not a smart object");
    return at;
}

// A layer's new id: the op's "new_id" (not one the page has), else a new one.
std::string fresh_layer_id(const Page& page, const Json& op) {
    const std::string id = core::truthy_at(op, "new_id") ? core::py_str(op["new_id"]) : core::new_id();
    for (const Layer& layer : page.layers) {
        if (layer.id == id) throw OpError("layer " + id + " exists");
    }
    return id;
}

std::string fresh_contents_id(const Document& doc) {
    for (;;) {
        std::string id = core::smart::new_contents_id();
        if (!doc.smart_contents.contains(id)) return id;
    }
}

// The layers the op names ("ids", or "id"), a folder with every layer inside it, in the page's order.
std::vector<std::size_t> chosen_layers(const Page& page, const Json& op) {
    std::vector<std::string> ids;
    if (const Json* given = core::get(op, "ids"); given != nullptr && !given->is_null()) {
        if (!given->is_array() || given->empty()) throw OpError("ids is a list of the layers' ids");
        for (const Json& id : *given) ids.push_back(core::py_str(id));
    } else if (core::truthy_at(op, "id")) {
        ids.push_back(core::py_str(op["id"]));
    } else {
        throw OpError("id (or ids) names the layers");
    }
    std::set<std::string> wanted;
    for (const std::string& id : ids) wanted.insert(page.layers[core::layer_by_id(page, id)].id);
    // (a folder's layers come with it, and the folders in it)
    for (bool grew = true; grew;) {
        grew = false;
        for (const Layer& layer : page.layers) {
            if (layer.parent_id.is_string() && wanted.contains(layer.parent_id.get<std::string>()) && wanted.insert(layer.id).second) grew = true;
        }
    }
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (wanted.contains(page.layers[i].id)) out.push_back(i);
    }
    return out;
}

bool draws(const Layer& l) {
    return l.raster_png || l.color_raster || l.ink_raster || l.stroke_count() > 0 || !l.patches.empty() || (l.fill && core::py_truthy(*l.fill)) ||
           (l.fill_rgb && !l.fill_rgb->empty()) || l.kind == LayerKind::Tone || (l.tone && core::py_truthy(*l.tone)) || l.kind == LayerKind::Placed ||
           l.kind == LayerKind::Smart || shape_parts::any(l);
}

// A smart object's page made of the page it is on: its paper, panels and colour mode (the layers come next).
Page contents_page(const Page& page) {
    Page out;
    out.id = core::new_page_id();
    out.index = Num(1);
    out.spec = page.spec;
    out.frames = page.frames;
    out.binding = page.binding;
    out.color = page.color;
    out.rgb = page.rgb;
    return out;
}

// The asset store's file of a ref, whatever its suffix (smart_place's "asset"): its bytes and suffix.
std::pair<std::string, std::string> asset_file(const Document& doc, const std::string& ref) {
    if (!doc.asset_dir) throw OpError("placing an asset needs a saved project");
    if (!storage::AssetStore::is_ref(ref)) throw OpError("asset must be sha256:…, got " + core::py_repr_str(ref));
    const storage::AssetStore store(*doc.asset_dir);
    std::vector<core::smart::FileFormat> formats(core::smart::kFileFormats.begin(), core::smart::kFileFormats.end());
    // (PC4 F-a: formats@1's; F-e: camera-raw@1's)
    for (const core::smart::FileFormat& later : core::smart::kLaterFileFormats) formats.push_back(later);
    for (const core::smart::FileFormat& format : formats) {
        if (!store.has(ref, format.suffix)) continue;
        std::optional<std::string> bytes;
        try {
            bytes = store.get_bytes(ref, format.suffix, smart::kFileMaxBytes);
        } catch (const core::Error& error) {
            throw OpError(std::string("the asset's picture cannot be read (") + error.what() + ")");
        }
        if (!bytes) break;
        if (storage::AssetStore::ref(*bytes) != ref) throw OpError("the asset's picture cannot be read (its bytes are not what its name says)");
        return {std::move(*bytes), std::string(format.suffix)};
    }
    throw OpError("no asset " + ref + " of a picture (png, jpg, tif, webp, bmp, gif, psd, tga, ico, pnm, qoi, exr, hdr, avif, a camera RAW file)");
}

// The contents' colour use the op asks for: embedded_profile keep (its numbers), convert (from the profile the file
// carries: "embedded"), assign (from the op's profile — "profile": the book's asset, or "icc": its bytes in base64).
Json colour_use(Document& doc, const Json& op, const smart::FileInfo& info) {
    const std::string way = text_at(op, "embedded_profile", "keep");
    Json out = Json::object();
    if (way == "keep") {
        out["use"] = "keep";
    } else if (way == "convert") {
        out["use"] = info.icc ? "embedded" : "keep";  // (no profile of its own: its numbers)
    } else if (way == "assign") {
        out["use"] = "assign";
        if (core::truthy_at(op, "icc")) {
            std::string icc;
            try {
                icc = core::a2b_base64(text_at(op, "icc"));
            } catch (const core::Error&) {
                throw OpError("icc is an ICC profile's bytes in base64");
            }
            if (icc.empty() || !ink::profile_space(icc)) throw OpError("icc is not an ICC profile Little CMS reads");
            const std::string ref = core::bytes_ref(icc);
            doc.smart_files[ref] = std::make_shared<const std::string>(std::move(icc));
            out["icc"] = ref;
        } else {
            const std::string ref = text_at(op, "profile");
            if (!storage::AssetStore::is_ref(ref)) throw OpError("assign takes profile (an ICC profile's asset, sha256:…) or icc");
            if (!doc.smart_files.contains(ref) && (!doc.asset_dir || !storage::AssetStore(*doc.asset_dir).has(ref, core::kProfileSuffix)))
                throw OpError("no ICC profile " + ref + " in the book");
            out["icc"] = ref;
        }
    } else {
        throw OpError("embedded_profile is keep, convert or assign");
    }
    if (out["use"] != "keep") {
        const std::string intent = text_at(op, "intent", "relative");
        if (intent != "perceptual" && intent != "relative" && intent != "saturation" && intent != "absolute")
            throw OpError("intent is perceptual, relative, saturation or absolute");
        out["intent"] = intent;
        const Json* bpc = core::get(op, "bpc");
        if (bpc != nullptr && !bpc->is_boolean()) throw OpError("bpc is true or false");
        out["bpc"] = bpc == nullptr || bpc->get<bool>();
    }
    return out;
}

void rasterize(Document& doc, std::size_t at, std::size_t i);

}  // namespace

// (PC3 A1's ops share these: render/ops_smart_parts.hpp)
namespace smart_ops {
std::pair<std::string, std::string> asset_file(const Document& doc, const std::string& ref) { return render::asset_file(doc, ref); }
Json colour_use(Document& doc, const Json& op, const smart::FileInfo& info) { return render::colour_use(doc, op, info); }
std::string fresh_contents_id(const Document& doc) { return render::fresh_contents_id(doc); }
void rasterize(Document& doc, std::size_t at, std::size_t i) { render::rasterize(doc, at, i); }
}  // namespace smart_ops

namespace {

// Where placed art goes (place_asset's rule): under the page's ink layer.
std::size_t insert_under_ink(Page& page, Layer layer) {
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (page.layers[i].role == LayerRole::Ink) {
            page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(i), std::move(layer));
            return i;
        }
    }
    page.layers.push_back(std::move(layer));
    return page.layers.size() - 1;
}

// --- smart_convert ---------------------------------------------------------------------------------------------------

// A placed picture as a smart object of its file: its contents the asset (a PNG), its matrix the place it is laid at (the
// same pixels), its panel, finish and where it came from kept.
Layer placed_to_smart(Document& doc, Layer placed, const std::string& id) {
    if (!placed.asset || !placed.placement_mm) throw OpError("the placed picture has no picture or place");
    const auto [bytes, suffix] = asset_file(doc, *placed.asset);
    smart::FileInfo info;
    try {
        info = smart::look_at(bytes, suffix);
    } catch (const NotYetPorted&) {
        throw;
    } catch (const core::Error& error) {
        throw OpError(std::string("the asset's picture cannot be read (") + error.what() + ")");
    }
    auto contents = std::make_shared<core::SmartContents>();
    contents->kind = "file";
    contents->name = placed.title;
    contents->asset = *placed.asset;
    contents->suffix = suffix;
    contents->format = info.format;
    contents->width_px = info.width;
    contents->height_px = info.height;
    contents->dpi = info.dpi > 0 ? info.dpi : 72.0;
    contents->depth = info.depth;
    contents->mode = info.mode;
    contents->profile = Json::object({{"use", "keep"}});  // (the picture's numbers, as the placed picture showed them)
    const std::string contents_id = fresh_contents_id(doc);
    doc.smart_contents[contents_id] = contents;
    const auto [cw, ch] = core::smart::size_mm(*contents);
    const core::Rect& r = *placed.placement_mm;
    const Matrix m{r.width.value() / cw, 0, 0, r.height.value() / ch, r.x.value(), r.y.value()};
    Layer out = std::move(placed);
    out.id = id;
    out.kind = LayerKind::Smart;
    out.smart = core::smart::settings(contents_id, m, "lanczos");
    out.asset.reset();
    out.placement_mm.reset();
    out.fit = "cover";
    out.raster_png = nullptr;
    out.raster_relpath.reset();
    out.strokes = core::empty_strokes();
    out.patches.clear();
    return out;
}

void smart_convert(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::vector<std::size_t> chosen = chosen_layers(doc.page(at), c.op);
    if (chosen.empty()) throw OpError("no layers to convert");
    const Page& seen = doc.page(at);
    if (std::none_of(chosen.begin(), chosen.end(), [&](std::size_t i) { return draws(seen.layers[i]); })) throw OpError("the layers have nothing to draw");
    const std::string id = fresh_layer_id(seen, c.op);
    std::string contents_id;
    Layer smart;
    if (chosen.size() == 1 && seen.layers[chosen.front()].kind == LayerKind::Placed) {  // (a placed picture: its file)
        smart = placed_to_smart(doc, seen.layers[chosen.front()], id);
        contents_id = *core::smart::contents_id(smart);
    } else {
        Page inner = contents_page(seen);
        for (const std::size_t i : chosen) inner.layers.push_back(seen.layers[i]);
        const Layer& top = seen.layers[chosen.back()];
        smart.id = id;
        smart.kind = LayerKind::Smart;
        smart.title = top.title.empty() ? std::string("スマートオブジェクト") : top.title;
        smart.parent_id = seen.layers[chosen.front()].parent_id;
        // (one layer: its visibility, opacity, blend, clipping and lock are the smart object's, and it is plain inside —
        // as PhotoCraft's layer_to_smart; its mask, effects and screen stay with it inside)
        if (chosen.size() == 1) {
            Layer& only = inner.layers.front();
            smart.visible = only.visible;
            smart.opacity = only.opacity;
            smart.blend = only.blend.empty() ? "normal" : only.blend;
            smart.clip = only.clip;
            smart.locked = only.locked;
            smart.exportable = only.exportable;
            if (guide(only.role)) smart.role = only.role;
            only.visible = true;
            only.opacity = 1.0;
            only.blend = "normal";
            only.clip = false;
            only.locked = false;
        } else {
            smart.exportable = std::any_of(chosen.begin(), chosen.end(), [&](std::size_t i) { return seen.layers[i].exportable; });
            if (std::all_of(chosen.begin(), chosen.end(), [&](std::size_t i) { return guide(seen.layers[i].role); })) smart.role = top.role;
        }
        // (the layers at the top of the contents are in no folder)
        std::set<std::string> inside;
        for (const Layer& layer : inner.layers) inside.insert(layer.id);
        for (Layer& layer : inner.layers) {
            if (!(layer.parent_id.is_string() && inside.contains(layer.parent_id.get<std::string>()))) layer.parent_id = nullptr;
        }
        auto contents = std::make_shared<core::SmartContents>();
        contents->kind = "page";
        contents->name = smart.title;
        contents->page = std::make_shared<Page>(std::move(inner));
        contents_id = fresh_contents_id(doc);
        doc.smart_contents[contents_id] = contents;
        smart.smart = core::smart::settings(contents_id, Matrix{}, "bicubic");
    }
    // (in the place of the topmost of them; the lines set under one of them are set under it)
    std::set<std::string> moved;
    for (const std::size_t i : chosen) moved.insert(seen.layers[i].id);
    Page& page = doc.edit_page(at);
    const std::size_t place = chosen.back() + 1 - chosen.size();
    std::vector<Layer> layers;
    for (std::size_t i = 0; i < page.layers.size(); ++i) {
        if (!moved.contains(page.layers[i].id)) layers.push_back(std::move(page.layers[i]));
    }
    layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(std::min(place, layers.size())), std::move(smart));
    page.layers = std::move(layers);
    for (core::StoryLine& line : doc.story) {
        if (!(line.page_index == page.index) || !line.style.is_object()) continue;
        const Json* under = core::get(line.style, "below_layer");
        if (under != nullptr && under->is_string() && moved.contains(under->get<std::string>())) line.style["below_layer"] = id;
    }
    c.report = Json{{"id", id}, {"contents", contents_id}};
}

// --- smart_place -----------------------------------------------------------------------------------------------------

// smart_place's `to` (art, bg, draft, name) as the layer's role, the strict gates asked of art and bg.
LayerRole place_role(const OpContext& c, const Document& doc, std::size_t at, const std::string& to) {
    LayerRole role = LayerRole::User;
    if (to == "bg") role = LayerRole::Bg;
    else if (to == "draft") role = LayerRole::Draft;
    else if (to == "name") role = LayerRole::Name;
    else if (to != "art") throw OpError("to must be art, bg, draft or name");
    const Page& seen = doc.page(at);
    if ((to == "art" || to == "bg") && doc.strict_gates && !seen.name_ok) {
        throw OpError("page " + seen.index.repr() + ": approve the name first (strict_gates)");
    }
    (void)c;
    return role;
}

// Where smart_place lays contents of cw × ch mm (w × h pixels: their shape for `fit`): covering a panel, or stretched over
// the box given (as place_asset lays a picture — the same place, the same pixels); else at its own size in the middle of
// the page, made smaller to fit the page when it is larger (PhotoCraft's), or at `scale` and `center_mm`. The layer's
// frame_id and clip_to set so.
Matrix place_matrix(const OpContext& c, const Page& seen, Layer& layer, double cw, double ch, std::int64_t w, std::int64_t h) {
    Matrix m;
    const bool as_placed = core::truthy_at(c.op, "frame_id") || core::truthy_at(c.op, "placement_mm") || core::truthy_at(c.op, "fit") ||
                           core::truthy_at(c.op, "clip_to");
    if (as_placed) {
        core::Rect placement;
        if (core::truthy_at(c.op, "placement_mm")) {
            const std::vector<double> box = core::unpack_floats(c.op["placement_mm"], 4);
            placement = core::Rect{Num(box[0]), Num(box[1]), Num(box[2]), Num(box[3])};
            layer.clip_to = text_at(c.op, "clip_to", "none");
        } else {
            const core::Frame* frame = nullptr;
            if (core::truthy_at(c.op, "frame_id")) {
                frame = seen.find_frame(core::py_str(c.op["frame_id"]));
                if (frame == nullptr) throw core::OpKeyError(core::py_repr(c.op["frame_id"]));
                if (!frame->children.empty()) throw OpError("frame " + frame->id + " is not a panel (it is split)");
                layer.frame_id = frame->id;
            }
            const std::string fit = text_at(c.op, "fit", "cover");
            layer.clip_to = text_at(c.op, "clip_to", frame != nullptr ? "frame" : "none");
            core::Rect target;
            if (frame == nullptr) target = core::Rect{Num(0), Num(0), seen.spec.width_mm, seen.spec.height_mm};
            else if (layer.clip_to == "bleed") target = core::default_target(seen, *frame, layer.clip_to);
            else target = frame->rect;
            placement = core::fit_rect(target, w, h, fit, 0.0, 0.0, 1.0);
        }
        core::require_finite(core::rect_to_json(placement), "placement_mm");
        m = Matrix{placement.width.value() / cw, 0, 0, placement.height.value() / ch, placement.x.value(), placement.y.value()};
    } else {
        const double pw = seen.spec.width_mm.value(), ph = seen.spec.height_mm.value();
        double s = number_at(c.op, "scale", 0) / 100.0;
        if (s <= 0) s = std::min({1.0, pw / cw, ph / ch});
        double cx = pw / 2, cy = ph / 2;
        if (const Json* center = core::get(c.op, "center_mm"); center != nullptr && !center->is_null()) {
            const std::vector<double> xy = core::unpack_floats(*center, 2);
            cx = xy[0];
            cy = xy[1];
        }
        m = Matrix{s, 0, 0, s, cx - s * cw / 2, cy - s * ch / 2};
        layer.clip_to = "none";
    }
    if (std::abs(m.det()) <= core::smart::kDetMin) throw OpError("the picture would be laid too small");
    return m;
}

// A file's name without its folders and its suffix (the layer's title).
std::string name_stem(std::string stem) {
    if (const auto slash = stem.find_last_of("/\\"); slash != std::string::npos) stem = stem.substr(slash + 1);
    if (const auto dot = stem.rfind('.'); dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
    return stem;
}

// PC4 F-f (design 2.8): 埋め込みを配置… of a .pcraft — its page read (render/pcraft_read) made the contents of a smart object
// (PC3's contents of kind page: a page of its own, its layers, channels and paths), laid as smart_place lays a picture
// (at its own size in the middle of the page, made smaller to fit; or over a panel or a box). What it could not keep is
// said in the op's notes.
void place_pcraft(OpContext& c, std::size_t at, const std::string& bytes, const std::string& name) {
    Document& doc = c.doc;
    const std::string to = text_at(c.op, "to", "art");
    const LayerRole role = place_role(c, doc, at, to);
    std::vector<std::string> notes;
    std::string contents_id;
    try {
        contents_id = pcraft::place_as_contents(doc, bytes, name.empty() ? std::string("contents.pcraft") : name, notes);
    } catch (const NotYetPorted&) {
        throw;
    } catch (const core::Error& error) {
        throw OpError(std::string("the .pcraft cannot be read (") + error.what() + ")");
    }
    for (std::string& note : notes) c.notes.push_back(std::move(note));
    const core::SmartContents& contents = *doc.smart_contents.at(contents_id);
    const auto [cw, ch] = core::smart::size_mm(contents);
    const core::PageSpec& spec = contents.page->spec;
    const std::int64_t w = std::llround(spec.width_mm.value() / 25.4 * spec.dpi.value()), h = std::llround(spec.height_mm.value() / 25.4 * spec.dpi.value());
    const Page& seen = doc.page(at);
    Layer layer;
    layer.id = fresh_layer_id(seen, c.op);
    layer.role = role;
    layer.kind = LayerKind::Smart;
    layer.exportable = to == "art" || to == "bg";
    const std::string stem = name_stem(name);
    layer.title = text_at(c.op, "title", stem.empty() ? std::string("スマートオブジェクト") : stem);
    layer.source = Json::object({{"to", to}, {"by", c.actor.name()}});
    const Matrix m = place_matrix(c, seen, layer, cw, ch, std::max<std::int64_t>(1, w), std::max<std::int64_t>(1, h));
    layer.smart = core::smart::settings(contents_id, m, "bicubic");
    const std::size_t placed = insert_under_ink(doc.edit_page(at), std::move(layer));
    c.report = Json{{"id", doc.page(at).layers[placed].id}, {"contents", contents_id}, {"size_px", Json::array({w, h})}};
    if (!c.notes.empty()) (*c.report)["notes"] = c.notes;
}

void smart_place(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::string name = text_at(c.op, "name");
    std::string bytes, suffix;
    if (core::truthy_at(c.op, "data")) {
        try {
            bytes = core::a2b_base64(text_at(c.op, "data"));
        } catch (const core::Error&) {
            throw OpError("data is the file's bytes in base64");
        }
        if (bytes.size() > smart::kFileMaxBytes) throw OpError("the file is larger than 256 MiB");
        // (PC4 F-e: a TIFF-shaped camera RAW — NEF, ARW, PEF… — is known by its name's suffix alone)
        if (const auto named = core::smart::suffix_of_name(name); named && raw::raw_suffix(*named)) suffix = *named;
    } else {
        const std::string ref = text_at(c.op, "asset");
        if (!ref.starts_with("sha256:")) throw OpError("smart_place takes an asset ref (sha256:…) or data, not a path");
        // (PC4 F-f: a .pcraft kept as the book's asset)
        if (doc.asset_dir && storage::AssetStore::is_ref(ref) && storage::AssetStore(*doc.asset_dir).has(ref, ".pcraft")) {
            const std::optional<std::string> got = storage::AssetStore(*doc.asset_dir).get_bytes(ref, ".pcraft", smart::kFileMaxBytes);
            if (!got || storage::AssetStore::ref(*got) != ref) throw OpError("the asset's .pcraft cannot be read (its bytes are not what its name says)");
            bytes = *got;
            suffix = ".pcraft";
        } else {
            std::tie(bytes, suffix) = asset_file(doc, ref);
        }
    }
    if (suffix == ".pcraft" || pcraft::is_pcraft(bytes)) {  // (PC4 F-f)
        place_pcraft(c, at, bytes, name);
        return;
    }
    smart::FileInfo info;
    try {
        info = smart::look_at(bytes, suffix);
    } catch (const NotYetPorted&) {
        throw;
    } catch (const core::Error& error) {
        throw OpError(std::string("the asset's picture cannot be read (") + error.what() + ")");
    }
    const std::string to = text_at(c.op, "to", "art");
    const LayerRole role = place_role(c, doc, at, to);
    const Page& seen = doc.page(at);
    const bool precise_page = !seen.color && seen.rgb && seen.rgb->depth > 8;
    if (precise_page && info.depth > 8 && static_cast<std::uint64_t>(info.width) * info.height > core::kColorRasterMaxPixels) {
        throw OpError("the picture is too large to keep its 16 or 32 bits on this page (at most " + std::to_string(core::kColorRasterMaxPixels) +
                      " pixels)");
    }
    const std::string ref = core::bytes_ref(bytes);
    if (core::truthy_at(c.op, "asset") && ref != text_at(c.op, "asset")) throw OpError("the asset's picture cannot be read (its bytes are not what its name says)");
    auto contents = std::make_shared<core::SmartContents>();
    contents->kind = "file";
    contents->name = name.empty() ? std::string("画像") + info.suffix : name;
    contents->asset = ref;
    contents->suffix = info.suffix;
    contents->format = info.format;
    contents->width_px = info.width;
    contents->height_px = info.height;
    contents->dpi = info.dpi > 0 ? info.dpi : seen.spec.dpi.value();
    contents->depth = info.depth;
    contents->mode = info.mode;
    contents->profile = colour_use(doc, c.op, info);
    if (info.format == "raw") {
        // (PC4 F-e, camera-raw@1: developed with camera-raw@1's defaults until set_camera_raw — its colours its output
        // space's, which the developed picture carries)
        try {
            contents->raw = raw::default_raw(bytes);
        } catch (const core::Error& error) {
            throw OpError(std::string("the asset's picture cannot be read (") + error.what() + ")");
        }
        contents->profile = Json::object({{"use", "embedded"}, {"intent", "relative"}, {"bpc", true}});
    }
    if (const Json* link = core::get(c.op, "link"); link != nullptr && !link->is_null()) contents->link = smart_ops::link_of(c.op, bytes);  // (PC3 A1)
    if (core::truthy_at(c.op, "data")) doc.smart_files[ref] = std::make_shared<const std::string>(std::move(bytes));
    const auto [cw, ch] = core::smart::size_mm(*contents);

    Layer layer;
    layer.id = fresh_layer_id(seen, c.op);
    layer.role = role;
    layer.kind = LayerKind::Smart;
    layer.exportable = to == "art" || to == "bg";
    const std::string stem = name_stem(name);
    layer.title = text_at(c.op, "title", stem.empty() ? std::string("画像") : stem);
    layer.source = Json::object({{"asset", ref}, {"to", to}, {"by", c.actor.name()}});
    const Matrix m = place_matrix(c, seen, layer, cw, ch, info.width, info.height);
    const std::string contents_id = fresh_contents_id(doc);
    doc.smart_contents[contents_id] = contents;
    layer.smart = core::smart::settings(contents_id, m, "lanczos");
    const std::size_t placed = insert_under_ink(doc.edit_page(at), std::move(layer));
    c.report = Json{{"id", doc.page(at).layers[placed].id}, {"contents", contents_id}, {"size_px", Json::array({info.width, info.height})},
                    {"depth", info.depth}};
}

// --- smart_profile, smart_copy, smart_transform ---------------------------------------------------------------------------

void smart_profile(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Layer& layer = doc.page(at).layers[smart_layer(doc.page(at), c.op)];
    const auto id = core::smart::contents_id(layer);
    const core::SmartContents* contents = core::smart::contents_of(doc, layer);
    if (!id || contents == nullptr) throw OpError("the smart object has no contents");
    if (contents->kind != "file") throw OpError("the smart object's contents are a page, not a file: their colours are their layers'");
    if (contents->format == "raw") {  // (PC4 F-e)
        throw OpError("the smart object's contents are a camera RAW file: their colours are their development's output space (set_camera_raw)");
    }
    const core::Bytes bytes = smart::file_bytes(doc, *contents);
    if (!bytes) throw OpError("the smart object's file is not in the book");
    smart::FileInfo info;
    info.icc = proof::embedded_profile(*bytes);
    doc.edit_contents(*id).profile = colour_use(doc, c.op, info);
    // (PC3 A3: a PSD's picture of the smart objects showing them is no longer theirs — drawn from the file again; their
    // blocks, of the same file at the same place, kept)
    const auto drop_shown = [&](Page& on) {
        for (Layer& l : on.layers) {
            if (core::smart::contents_id(l) != id || !l.smart->contains("psd") || !(*l.smart)["psd"].is_object()) continue;
            (*l.smart)["psd"].erase("shown");
        }
    };
    for (const std::size_t i : core::smart::pages_using(doc, *id)) drop_shown(doc.edit_page(i));
    std::vector<std::string> inside;
    for (const auto& [other, held] : doc.smart_contents) {
        if (held && held->page && core::smart::shown_on(*held->page).contains(*id)) inside.push_back(other);
    }
    for (const std::string& other : inside) drop_shown(doc.edit_contents_page(other));
}

void smart_copy(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::size_t i = smart_layer(doc.page(at), c.op);
    const Layer& source = doc.page(at).layers[i];
    const core::SmartContents* contents = core::smart::contents_of(doc, source);
    if (contents == nullptr) throw OpError("the smart object has no contents");
    auto copy = std::make_shared<core::SmartContents>(*contents);
    if (copy->page) {  // (its own page: changing one never changes the other)
        copy->page = std::make_shared<Page>(*copy->page);
        copy->page->id = core::new_page_id();
    }
    copy->psd.reset();
    const std::string contents_id = fresh_contents_id(doc);
    doc.smart_contents[contents_id] = copy;
    Layer twin = source;
    twin.id = fresh_layer_id(doc.page(at), c.op);
    twin.title = (source.title.empty() ? std::string("スマートオブジェクト") : source.title) + " のコピー";
    (*twin.smart)["contents"] = contents_id;
    if (twin.smart->contains("psd")) twin.smart->erase("psd");
    Page& page = doc.edit_page(at);
    page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(i + 1), std::move(twin));
    c.report = Json{{"id", page.layers[i + 1].id}, {"contents", contents_id}};
}

// The homography taking a smart object's corners where they were onto where they are (none: they did not move, or one
// of them is no quad).
std::optional<resample::Homography> corners_moved(const core::smart::Corners& was, const core::smart::Corners& now) {
    if (was == now) return std::nullopt;
    const auto quad = [](const core::smart::Corners& c) {
        return std::array<std::array<double, 2>, 4>{{{c[0].first, c[0].second}, {c[1].first, c[1].second}, {c[2].first, c[2].second}, {c[3].first, c[3].second}}};
    };
    const auto unit = resample::Homography::rect_to_quad({0, 0, 1, 1}, quad(was));
    const auto to = resample::Homography::rect_to_quad({0, 0, 1, 1}, quad(now));
    if (!unit || !to) return std::nullopt;
    const auto back = unit->inverse();
    if (!back) return std::nullopt;
    resample::Homography h = to->mul(*back);
    if (std::abs(h.m[6]) < 1e-12 && std::abs(h.m[7]) < 1e-12 && std::abs(h.m[8]) > 1e-12) {  // (an affine map, exactly)
        const double k = h.m[8];
        for (double& v : h.m) v /= k;
        h.m[6] = 0;
        h.m[7] = 0;
        h.m[8] = 1;
    }
    return h;
}

// The keys of smart_transform that place it (its matrix, corners and warp).
constexpr const char* kPlacing[] = {"set_matrix", "matrix", "perspective", "warp"};

void smart_transform(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::size_t i = smart_layer(doc.page(at), c.op);
    if (doc.page(at).layers[i].locked) throw OpError("the layer is locked");
    // (its position locked: not moved, as transform_layers refuses it — independent review A P2-2)
    const bool moves = std::any_of(std::begin(kPlacing), std::end(kPlacing), [&](std::string_view key) {
        const Json* v = core::get(c.op, std::string(key));
        return v != nullptr && (!v->is_null() || key == "perspective" || key == "warp");
    });
    if (moves && doc.page(at).layers[i].lock_position) throw OpError("the position of the layer " + doc.page(at).layers[i].id + " is locked");
    const core::SmartContents* contents = core::smart::contents_of(doc, doc.page(at).layers[i]);
    if (contents == nullptr) throw OpError("the smart object has no contents");
    Page& page = doc.edit_page(at);
    Layer& layer = page.layers[i];
    // (its filters that name places still in the page's mm — a book of before — kept in its own first, so that they go with
    // it: independent review A P2-1)
    if (moves) smart_filters::settle_places(doc, page, layer);
    const core::smart::Corners was = core::smart::corners_on_page(layer, *contents);
    Matrix m = core::smart::matrix_of(layer);
    if (const Json* set = core::get(c.op, "set_matrix"); set != nullptr && !set->is_null()) {
        try {
            m = core::smart::matrix_from(*set);
        } catch (const core::Error& error) {
            throw OpError(error.what());
        }
        core::smart::put_setting(layer, "perspective", Json());  // (its matrix places it now)
    }
    if (const Json* by = core::get(c.op, "matrix"); by != nullptr && !by->is_null()) {
        Matrix more;
        try {
            more = core::smart::matrix_from(*by);
        } catch (const core::Error& error) {
            throw OpError(error.what());
        }
        m = core::smart::multiply(more, m);
        if (Json* corners = layer.smart->contains("perspective") ? &(*layer.smart)["perspective"] : nullptr; corners != nullptr && corners->is_array()) {
            for (Json& point : *corners) {  // (its corners go the same way)
                if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number()) continue;
                const auto [x, y] = more.apply(point[0].get<double>(), point[1].get<double>());
                point = Json::array({x, y});
            }
        }
    }
    m = core::smart::snapped(m);
    if (std::abs(m.det()) <= core::smart::kDetMin) throw OpError("the transform squashes the smart object flat");
    core::smart::set_matrix(layer, m);
    layer.smart->erase("psd");  // (PC3 A3: a PSD's picture and block no longer say where it is; drawn from its contents)
    // (PC3 A5: its corners on the page — null: its matrix places it —, its warp in its contents' mm — null: none)
    if (const Json* quad = core::get(c.op, "perspective"); quad != nullptr) {
        if (quad->is_null()) {
            core::smart::put_setting(layer, "perspective", Json());
        } else {
            if (!quad->is_array() || quad->size() != 4) throw OpError("perspective is the contents' four corners on the page [[x, y] × 4]");
            core::smart::Corners corners;
            for (std::size_t k = 0; k < 4; ++k) {
                const Json& p = (*quad)[k];
                if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
                    throw OpError("perspective is the contents' four corners on the page [[x, y] × 4]");
                corners[k] = {p[0].get<double>(), p[1].get<double>()};
            }
            try {
                core::smart::set_corners(layer, *contents, corners);
            } catch (const core::Error& error) {
                throw OpError(error.what());
            }
        }
    }
    if (const Json* warp = core::get(c.op, "warp"); warp != nullptr) {
        const auto [w, h] = core::smart::size_mm(*contents);
        std::optional<core::page_geometry::Warp> kept;
        if (!warp->is_null()) kept = layer_transform::warp_from_json(*warp, {0, 0, w, h});
        core::smart::put_setting(layer, "warp", kept && !kept->is_identity() ? layer_transform::warp_to_json(*kept) : Json());
    }
    // (its filter mask moved as its corners moved: PC3 A5; its mask and vector mask too when they are linked, as
    // transform_layers moves them: independent review A P2-2)
    if (const auto moved = corners_moved(was, core::smart::corners_on_page(layer, *contents))) {
        layer_transform::Map map;
        map.kind = layer_transform::Map::Kind::Projective;
        map.projective = *moved;
        if (moved->is_affine()) map = layer_transform::affine_map({moved->m[0], moved->m[3], moved->m[1], moved->m[4], moved->m[2], moved->m[5]}, {});
        layer_transform::move_filter_mask(doc, page, layer, map);
        layer_transform::move_linked_masks(page, layer, map);
    }
    if (const Json* interp = core::get(c.op, "interp"); interp != nullptr && !interp->is_null()) {
        const std::string name = core::py_str(*interp);
        if (std::find(core::smart::kInterps.begin(), core::smart::kInterps.end(), name) == core::smart::kInterps.end())
            throw OpError("interp is lanczos, bicubic, bilinear or nearest");
        (*layer.smart)["interp"] = name;
    }
}

// --- rasterize_layer ---------------------------------------------------------------------------------------------------

bool rasterizable(const Layer& layer, const std::set<std::string>& kinds) {
    if (layer.kind == LayerKind::Smart) return kinds.contains("smart");
    if (layer.kind == LayerKind::Placed) return kinds.contains("placed");
    if (layer.kind == LayerKind::Type) return kinds.contains("type");  // (type@1, PC5 G-0: its text drawn in)
    if (layer.kind == LayerKind::Fill && ((layer.fill && core::py_truthy(*layer.fill)) || (layer.fill_rgb && !layer.fill_rgb->empty())))
        return kinds.contains("fill");
    if (layer.kind == LayerKind::Tone || (layer.tone && core::py_truthy(*layer.tone))) return kinds.contains("tone");
    if (layer.kind == LayerKind::Strokes && (layer.stroke_count() > 0 || shape_parts::any(layer)) && !core::has_color_strokes(layer))
        return kinds.contains("pen");  // (pen-shapes@1: its shape parts with its lines)
    return false;
}

// A tone layer rasterized: its tone screened as it printed (トーン化) — the same pattern, lines per inch (within the
// screen's 10 to 150), angle, dot shape and offset, its share whole (black 0, white 1) — for dot, line, cross and noise;
// nothing for a flat tone, a motif or a picture (their pixels are the tone).
std::optional<Json> tone_screen(const Layer& layer) {
    const tones::Settings s = tones::settings(layer);
    const std::string pattern = s.pattern.is_string() ? s.pattern.get<std::string>() : std::string("dot");
    if (pattern != "dot" && pattern != "line" && pattern != "cross" && pattern != "noise") return std::nullopt;
    Json spec{{"pattern", pattern}, {"lpi", std::clamp(s.lpi, 10.0, 150.0)}, {"angle", s.angle}, {"black", 0.0}, {"white", 1.0}};
    if (s.dot_shape && s.dot_shape->is_string()) {
        const std::string shape = s.dot_shape->get<std::string>();
        if (std::find(tones::dot_shapes().begin(), tones::dot_shapes().end(), shape) != tones::dot_shapes().end()) spec["shape"] = shape;
    }
    if (s.offset_mm && core::py_truthy(*s.offset_mm)) spec["offset_mm"] = *s.offset_mm;
    return core::screen_spec(spec);
}

// A picture (placed, or a picture's smart object) on a monochrome page is shown and printed with the page's finish
// (render/placed finish_placed): rasterized, its pixels are the finish as the proof shows it (its greys and lines), screened
// as its greys printed (the finish's lines per inch and angle — FM as noise —, black 0, white 1); extracted line art, its
// black lines alone. Nothing for a picture the finish leaves as it is (a colour page, a draft or name layer).
std::optional<Json> finish_screen(const Document& doc, const Page& page, const Layer& layer) {
    if (page.spec.expression == "color" || layer.role == LayerRole::Draft || layer.role == LayerRole::Name) return std::nullopt;
    const Json source = layer.source ? core::py_or(*layer.source, Json::object()) : Json::object();
    if (core::py_get(source, "to", Json("art")) == Json("ink")) return Json(nullptr);  // (lines: no screen)
    Json merged = Json::object();
    const Json style = core::py_or(core::py_get(core::py_or(core::py_get(doc.studio, "style"), Json::object()), "finish"), Json::object());
    for (const Json* part : {&style, layer.finish ? &*layer.finish : nullptr}) {
        if (part == nullptr || !part->is_object()) continue;
        for (const auto& [key, value] : part->items()) merged[key] = value;
    }
    const screentone::Finish finish = screentone::Finish::from_dict(merged);
    return core::screen_spec(Json{{"pattern", finish.fm() ? "noise" : "dot"}, {"lpi", std::clamp(finish.number("lpi"), 10.0, 150.0)},
                                  {"angle", finish.number("angle")}, {"black", 0.0}, {"white", 1.0}});
}

// A tone's picture (layer_image's: its ink's share as the alpha) as its ink as it prints: black, its share the alpha.
Image tone_black(const Image& picture) {
    Image black = Image::create("RGBA", picture.size(), Ink{0, 0, 0, 255});
    black.putalpha(picture.getchannel(3));
    return black;
}

// The layer's look as a paint layer's pixels at the paint layers' 200 dpi (raster::kWorkingDpi): an 8-bit page's PNG, a
// page in precise colour's precise raster, a CMYK or Lab page's inks. Its mask, opacity, blend, clipping, screen and
// colour stay as they were (they are the layer's, not drawn in); its effects are drawn in. A tone layer becomes its tone's
// ink as the screen shows it — black, its share the alpha (PC3 A4: on a page in precise colour that black in its values,
// on a page of inks black by the page's rules, as the page draws a tone) — screened as it printed where its page prints
// tones in dots (tone_screen); a picture on an 8-bit monochrome page, the page's finish of it (finish_screen).
void rasterize(Document& doc, std::size_t at, std::size_t i) {
    const Page& page = doc.page(at);
    const Layer& seen = page.layers[i];
    if (seen.locked) throw OpError("the layer is locked");
    const bool tone = seen.kind == LayerKind::Tone || seen.role == LayerRole::Tone || (seen.tone && core::py_truthy(*seen.tone));
    // (screened where the page prints its tones in dots — an RGB page of a monochrome book; a colour book's page and a page
    // of inks print them flat, as the rasterized grey prints)
    const bool dotted = !page.color && page.spec.expression != "color";
    const std::optional<Json> screen = tone && dotted ? tone_screen(seen) : std::nullopt;
    const core::SmartContents* contents = seen.kind == LayerKind::Smart ? core::smart::contents_of(doc, seen) : nullptr;
    const bool picture_art = seen.kind == LayerKind::Placed || (contents != nullptr && contents->kind == "file");
    const std::optional<Json> finished = picture_art && !page.color && !(page.rgb && page.rgb->depth > 8) ? finish_screen(doc, page, seen) : std::nullopt;
    Layer look = seen;  // (drawn alone, as it shows: without what stays the layer's)
    look.mask.reset();
    look.vector_mask.reset();  // (vector@1, PC3 C-a: the layer's, as its mask)
    look.opacity = 1.0;
    look.blend = "normal";
    look.clip = false;
    look.visible = true;
    look.screen.reset();
    look.color.reset();
    look.style.reset();  // (PC4 E6: its style stays the layer's, drawn over its pixels as before)
    const int dpi = raster::kWorkingDpi;
    const Size size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    core::Bytes ink_raster, color_raster;
    std::optional<Image> picture;
    if (page.color) {
        core::InkPixels inks;
        if (tone) {  // (its black by the page's rules, as the page lays a tone's 8-bit picture over its inks)
            InkCanvas canvas(*page.color, size);
            canvas.blend_rgba(tone_black(layer_image(page, look, dpi, &doc)), 1.0, false, "normal");
            inks = canvas.ink_pixels(page.color->depth, true);
        } else {
            inks = composite_ink_layers(page, {look}, dpi, &doc, false);
        }
        ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(inks));
    } else if (page.rgb && page.rgb->depth > 8) {
        ColorCanvas canvas(Image::create("RGBA", size, Ink{0, 0, 0, 0}), RgbSpace::of(page));
        std::optional<ColorPicture> deep;
        if (look.kind == LayerKind::Smart) {
            std::vector<std::string> omitted;
            detail::Ctx ctx;
            ctx.page = &page;
            ctx.episode = &doc;
            ctx.dpi = dpi;
            ctx.size = size;
            ctx.mode = "proof";
            ctx.omitted = &omitted;
            deep = smart::picture_precise(ctx, look, Box{0, 0, size.width, size.height}, canvas.space());
        } else if (look.kind == LayerKind::Fill) {  // (patterns@1, PC4 E3: a pattern's values kept)
            deep = pattern_fill_precise(page, look, dpi, &doc, canvas.space());
        }
        if (tone) {
            canvas.blend(tone_black(layer_image(page, look, dpi, &doc)), 1.0, false);
        } else if (deep && !(look.effect && core::py_truthy(*look.effect))) {
            canvas.blend(*deep, 1.0, false);
        } else {
            canvas.blend(layer_image(page, look, dpi, &doc), 1.0, false);
        }
        color_raster = std::make_shared<const std::string>(canvas.color_raster(page.rgb->depth == 16 ? "u16" : "f32"));
    } else {
        picture = layer_image(page, look, dpi, &doc);
        if (finished) picture = finish_placed(*picture, look, page, &doc, dpi, "proof", true, Point{0, 0});  // (as the proof shows it)
        if (tone) picture = tone_black(*picture);  // (the tone's ink black, as it prints: its share is the alpha)
    }
    Layer& layer = doc.edit_page(at).layers[i];
    if (tone) {
        layer.role = layer.role == LayerRole::Tone ? LayerRole::User : layer.role;
        layer.lpi.reset();
        layer.density.reset();
        layer.region.reset();
        layer.angle = 45.0;
        if (screen) layer.screen = *screen;
    }
    if (finished && !finished->is_null() && !(layer.screen && core::py_truthy(*layer.screen))) layer.screen = *finished;
    layer.kind = LayerKind::Raster;
    layer.native_psd.reset();  // (psd-keep@1, PC4 F-d: rasterized, a PSD's effects and blocks are not its own any more)
    layer.strokes = core::empty_strokes();
    layer.patches.clear();
    layer.shapes = core::vector::empty_shapes();  // (pen-shapes@1: drawn in)
    layer.fill.reset();
    layer.fill_rgb.reset();
    layer.adjust.reset();
    layer.effect.reset();
    layer.tone.reset();
    layer.smart.reset();
    layer.type.reset();  // (type@1, PC5 G-0: its text drawn in)
    layer.asset.reset();
    layer.placement_mm.reset();
    layer.frame_id.reset();
    layer.fit = "cover";
    layer.clip_to = "frame";
    layer.finish.reset();
    layer.source.reset();
    layer.raster_png = nullptr;
    layer.raster_memo = {};
    layer.ink_raster = ink_raster;
    layer.ink_memo = {};
    layer.color_raster = color_raster;
    if (picture) raster::save_raster(doc.page(at), layer, *picture);
    if (color_raster && std::find(doc.features.begin(), doc.features.end(), core::kColorRasterFeature) == doc.features.end()) {
        doc.features.emplace_back(core::kColorRasterFeature);
    }
}

void rasterize_layer(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    if (core::truthy_at(c.op, "vector_mask")) {  // (vector@1, PC3 C-a: Layer › Rasterize › Vector Mask, render/vector_masks)
        const std::size_t i = core::layer_by_id(doc.page(at), core::truthy_at(c.op, "id") ? core::py_str(c.op["id"]) : std::string());
        const Layer& layer = doc.page(at).layers[i];
        if (layer.locked) throw OpError("the layer is locked");
        if (!layer.vector_mask) throw OpError("layer " + layer.id + " has no vector mask");
        Page& page = doc.edit_page(at);
        vector_masks::rasterize(page, page.layers[i]);
        c.report = Json{{"ids", Json::array({page.layers[i].id})}};
        return;
    }
    if (core::truthy_at(c.op, "shape")) {  // (pen-shapes@1, PC3 C-b: Layer › Rasterize › Shape — the parts as patches, render/ops_shapes)
        const std::size_t i = core::layer_by_id(doc.page(at), core::truthy_at(c.op, "id") ? core::py_str(c.op["id"]) : std::string());
        const Layer& layer = doc.page(at).layers[i];
        if (layer.locked) throw OpError("the layer is locked");
        if (!shape_parts::any(layer)) throw OpError("layer " + layer.id + " has no shape parts");
        const bool over = std::any_of(layer.shapes->items.begin(), layer.shapes->items.end(), [](const core::vector::ShapePart& p) { return p.over; });
        if (over && layer.stroke_count() > 0) {
            // (a part over the lines stays over them only in pixels: the layer drawn in whole as it shows, as Layer ›
            // Rasterize › Layer — its look kept)
            const std::string id = layer.id;
            rasterize(doc, at, i);
            c.report = Json{{"ids", Json::array({id})}, {"whole", true}};
            return;
        }
        Page& page = doc.edit_page(at);
        shape_parts::rasterize_parts(page, page.layers[i], &doc);
        c.report = Json{{"ids", Json::array({page.layers[i].id})}};
        return;
    }
    std::set<std::string> kinds{"smart", "fill", "tone", "placed", "type"};  // (type@1, PC5 G-0: a type layer)
    if (const Json* given = core::get(c.op, "kinds"); given != nullptr && !given->is_null()) {
        if (!given->is_array()) throw OpError("kinds is a list of smart, fill, tone, placed, pen, type");
        kinds.clear();
        for (const Json& kind : *given) {
            const std::string name = core::py_str(kind);
            if (name != "smart" && name != "fill" && name != "tone" && name != "placed" && name != "pen" && name != "type")
                throw OpError("kinds is a list of smart, fill, tone, placed, pen, type");
            kinds.insert(name);
        }
    }
    if (const Json* keep = core::get(c.op, "keep_mask"); keep != nullptr && !(keep->is_boolean() && keep->get<bool>())) {
        core::not_yet_ported("rasterize_layer without keeping the mask (convert_layer to paint draws it in)");
    }
    std::vector<std::string> done;
    if (core::truthy_at(c.op, "all")) {
        std::vector<std::string> ids;
        for (const Layer& layer : doc.page(at).layers) {
            if (rasterizable(layer, kinds) && !layer.locked) ids.push_back(layer.id);
        }
        for (const std::string& id : ids) {
            rasterize(doc, at, core::layer_by_id(doc.page(at), id));
            done.push_back(id);
        }
    } else {
        const std::size_t i = core::layer_by_id(doc.page(at), core::truthy_at(c.op, "id") ? core::py_str(c.op["id"]) : std::string());
        const Layer& layer = doc.page(at).layers[i];
        if (!rasterizable(layer, {"smart", "fill", "tone", "placed", "pen", "type"})) throw OpError("layer " + layer.id + " has nothing to rasterize");
        rasterize(doc, at, i);
        done.push_back(layer.id);
    }
    Json ids = Json::array();
    for (const std::string& id : done) ids.push_back(id);
    c.report = Json{{"ids", ids}};
}

// --- smart_stack (PC3 A4) ----------------------------------------------------------------------------------------------

// レイヤー → スマートオブジェクト → スタックモード: the smart object's contents' layers combined pixel by pixel by a statistic
// (render/stack), or ("none", null) composited as they are. Any smart object, as PhotoCraft's stack_mode
// (layer_menu_cmds.rs:312-326); a file is one layer.
void smart_stack(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const std::size_t i = smart_layer(doc.page(at), c.op);
    if (doc.page(at).layers[i].locked) throw OpError("the layer is locked");
    const Json* given = core::get(c.op, "mode");
    std::string mode;  // (none given, or not a name: refused below)
    if (given != nullptr && given->is_null()) mode = "none";
    else if (given != nullptr && given->is_string()) mode = given->get<std::string>();
    if (mode != "none" && std::find(core::smart::kStackModes.begin(), core::smart::kStackModes.end(), mode) == core::smart::kStackModes.end()) {
        throw OpError("mode is a stack mode (entropy, kurtosis, maximum, mean, median, minimum, range, skewness, standard_deviation, summation, "
                      "variance) or none");
    }
    Layer& layer = doc.edit_page(at).layers[i];
    core::smart::put_setting(layer, "stack", mode == "none" ? Json(nullptr) : Json(mode));
    // (a PSD's smart object shows Photoshop's picture until what it draws changes: from now on drawn from its contents)
    if (Json* psd = layer.smart->contains("psd") ? &(*layer.smart)["psd"] : nullptr; psd != nullptr && psd->is_object()) psd->erase("shown");
    c.report = Json{{"id", layer.id}, {"stack", mode}};
}

// --- rasterize_lines (PC3 A4) -------------------------------------------------------------------------------------------

// A line drawn in its balloon on the page (render_page's placed lines; the others are drawn as labels).
bool placed_line(const core::StoryLine& line) { return line.x_mm.truthy() || line.y_mm.truthy() || !line.balloon.empty(); }

std::string group_of(const core::StoryLine& line) {
    const Json* group = line.style.is_object() ? core::get(line.style, "group") : nullptr;
    return group != nullptr && core::py_truthy(*group) ? core::dump_python(*group) : std::string();
}

// What `draw` paints as a layer's picture ("RGBA", `size`): drawn on black and on white, each pixel's cover its alpha and
// its colour what it holds over black (as a page of inks takes in what is drawn in 8 bits) — laid over any page, the same
// as drawn on it.
Image drawn_alone(Size size, const std::function<void(Image&)>& draw) {
    Image black = Image::create("RGBA", size, Ink{0, 0, 0, 255});  // (opaque: as the page's picture the lines are drawn on)
    Image white = Image::create("RGBA", size, Ink{255, 255, 255, 255});
    draw(black);
    draw(white);
    const Image b = black.mode() == "RGB" ? black : black.convert("RGB");
    const Image w = white.mode() == "RGB" ? white : white.convert("RGB");
    std::string bytes(std::size_t(size.width) * size.height * 4, '\0');
    for (int y = 0; y < size.height; ++y) {
        const auto* rb = reinterpret_cast<const unsigned char*>(b.raw()->image[y]);
        const auto* rw = reinterpret_cast<const unsigned char*>(w.raw()->image[y]);
        for (int x = 0; x < size.width; ++x) {
            const unsigned char* pb = rb + x * 4;
            const unsigned char* pw = rw + x * 4;
            double alpha = 0;
            for (unsigned c = 0; c < 3; ++c) alpha += 1.0 - (static_cast<double>(pw[c]) - pb[c]) / 255.0;
            alpha = std::clamp(alpha / 3.0, 0.0, 1.0);
            if (alpha <= 0) continue;
            char* to = bytes.data() + (std::size_t(y) * size.width + x) * 4;
            for (unsigned c = 0; c < 3; ++c) to[c] = static_cast<char>(static_cast<unsigned char>(std::clamp<long>(std::lround(pb[c] / alpha), 0, 255)));
            to[3] = static_cast<char>(static_cast<unsigned char>(std::lround(alpha * 255.0)));
        }
    }
    return Image::frombytes("RGBA", size, bytes);
}

// {frame id: (x, y, w, h) mm} of the page's panels, as render_page gives draw_lines.
text::Panels panels_of(const Page& page) {
    text::Panels out;
    for (const core::Frame* f : page.leaf_frames())
        out.emplace_back(f->id, std::array<double, 4>{f->rect.x.value(), f->rect.y.value(), f->rect.width.value(), f->rect.height.value()});
    return out;
}

// レイヤー → ラスタライズ → テキスト (PhotoCraft's type.rasterize, layer.rasterize.type): lines of the page — their balloons
// and letters as the page prints them ("balloon": false: their letters, rubies and halo alone) — drawn as a new paint layer
// (200 dpi; a page in precise colour its values, a page of inks its inks), where they were drawn: over every layer, or
// under the layer they are set under (below_layer); the lines then gone (or their balloons kept, without words, set under
// the new layer: the letters over them as before). Lines joined into one balloon (style.group) come together. The layer is
// not cut by the panels (as the lines were not).
void rasterize_lines(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Page& page = doc.page(at);
    if (const Json* type_layer = core::get(c.op, "layer_id"); type_layer != nullptr && !type_layer->is_null()) {
        // (type@1, PC5 G-0: a type layer — テキストレイヤーをラスタライズ — drawn into its own pixels, its settings kept, as
        // rasterize_layer does)
        const std::size_t i = core::layer_by_id(page, core::py_str(*type_layer));
        if (page.layers[i].kind != LayerKind::Type) throw OpError("layer " + page.layers[i].id + " is not a type layer");
        const std::string id = page.layers[i].id;
        rasterize(doc, at, i);
        c.report = Json{{"id", id}, {"lines", Json::array()}};
        return;
    }
    std::vector<std::string> named;
    if (const Json* given = core::get(c.op, "ids"); given != nullptr && !given->is_null()) {
        if (!given->is_array() || given->empty()) throw OpError("ids is a list of the lines' ids");
        for (const Json& id : *given) named.push_back(core::py_str(id));
    } else if (core::truthy_at(c.op, "id")) {
        named.push_back(core::py_str(c.op["id"]));
    } else {
        throw OpError("ids (or id) names the lines");
    }
    const Json* balloon_flag = core::get(c.op, "balloon");
    if (balloon_flag != nullptr && !balloon_flag->is_null() && !balloon_flag->is_boolean()) throw OpError("balloon is true or false");
    const bool balloon = balloon_flag == nullptr || balloon_flag->is_null() || balloon_flag->get<bool>();
    std::set<std::string> chosen, groups;
    for (const std::string& id : named) {
        const auto line = std::find_if(doc.story.begin(), doc.story.end(), [&](const core::StoryLine& l) { return l.id == id; });
        if (line == doc.story.end()) throw OpError("no line " + id);
        if (!(line->page_index == page.index)) throw OpError("line " + id + " is not on page " + page.index.repr());
        if (!placed_line(*line)) throw OpError("line " + id + " has no balloon on the page");
        chosen.insert(id);
        if (const std::string group = group_of(*line); !group.empty()) groups.insert(group);
    }
    // (joined into one balloon: drawn as one, so they come together)
    for (const core::StoryLine& line : doc.story) {
        if (line.page_index == page.index && placed_line(line) && groups.contains(group_of(line))) chosen.insert(line.id);
    }
    // where they are drawn among the layers: one place for all of them
    std::vector<const core::StoryLine*> lines;
    std::optional<std::string> place;
    for (const core::StoryLine& line : doc.story) {
        if (!chosen.contains(line.id)) continue;
        const Json* under = line.style.is_object() ? core::get(line.style, "below_layer") : nullptr;
        std::string here;
        if (under != nullptr && under->is_string() &&
            std::any_of(page.layers.begin(), page.layers.end(), [&](const Layer& l) { return l.id == under->get_ref<const std::string&>(); }))
            here = under->get<std::string>();
        if (place && *place != here) throw OpError("the lines are drawn at different places among the layers (below_layer): rasterize them a place at a time");
        place = here;
        lines.push_back(&line);
    }
    // their picture, as the page prints them (no speakers' names)
    const int dpi = raster::kWorkingDpi;
    const Size size{mm_to_px(page.spec.width_mm.value(), dpi), mm_to_px(page.spec.height_mm.value(), dpi)};
    const text::Panels panels = panels_of(page);
    const text::LetterParts letters{false, true, true, true};
    const std::optional<std::string> font_path(doc.font_path);
    const auto draw = [&](Image& on) {  // (on an opaque "RGBA", as the page's picture is)
        Image rgba = on.mode() == "RGBA" ? std::move(on) : on.convert("RGBA");
        text::draw_lines(text::whole_page(rgba), lines, dpi, font_path, false, &panels, {}, nullptr, balloon ? nullptr : &letters);
        on = std::move(rgba);
    };
    Image picture;
    core::Bytes inks;
    {
        const text::TextGamma gamma(doc.text_gamma);
        if (page.color) {  // (a page of inks: taken in as it takes in the lines it draws, by its rules)
            InkCanvas canvas(*page.color, size);
            canvas.draw_8bit(draw);
            inks = std::make_shared<const std::string>(core::encode_ink_raster(canvas.ink_pixels(page.color->depth, true)));
        } else {
            picture = drawn_alone(size, draw);
        }
    }
    Layer layer;
    layer.id = fresh_layer_id(page, c.op);
    layer.role = LayerRole::User;
    layer.kind = LayerKind::Raster;
    std::u32string words = text::u32(lines.front()->text).substr(0, 24);  // (its first words, as the layered PSD names a line)
    std::replace(words.begin(), words.end(), U'\n', U' ');
    layer.title = "台詞 " + text::utf8(words);
    layer.exportable = true;
    layer.panel_clip = false;
    layer.panel_each = false;
    if (page.color) {
        layer.ink_raster = inks;
    } else if (page.rgb && page.rgb->depth > 8) {
        ColorCanvas canvas(Image::create("RGBA", size, Ink{0, 0, 0, 0}), RgbSpace::of(page));
        canvas.blend(picture, 1.0, false);
        layer.color_raster = std::make_shared<const std::string>(canvas.color_raster(page.rgb->depth == 16 ? "u16" : "f32"));
        if (std::find(doc.features.begin(), doc.features.end(), core::kColorRasterFeature) == doc.features.end()) {
            doc.features.emplace_back(core::kColorRasterFeature);
        }
    } else {
        raster::save_raster(page, layer, picture);
    }
    const std::string id = layer.id;
    Page& edited = doc.edit_page(at);
    std::size_t index = edited.layers.size();
    if (!place->empty()) index = core::layer_by_id(edited, *place);
    edited.layers.insert(edited.layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
    Json ids = Json::array();
    for (const core::StoryLine* line : lines) ids.push_back(line->id);
    if (balloon) {
        std::erase_if(doc.story, [&](const core::StoryLine& line) { return chosen.contains(line.id); });
    } else {  // (their balloons stay, without words, drawn just under the letters: set under the new layer)
        for (core::StoryLine& line : doc.story) {
            if (!chosen.contains(line.id)) continue;
            if (!line.style.is_object()) line.style = Json::object();
            line.style["below_layer"] = id;
            line.text.clear();
            line.ruby.clear();
            line.ruby_runs.clear();
            line.ruby_spans.reset();
            line.emphasis_runs.clear();
            line.style_runs.clear();
        }
    }
    c.report = Json{{"id", id}, {"lines", ids}};
}

}  // namespace

void register_smart_ops(core::OpRegistry& registry) {
    registry.add("smart_convert", smart_convert);
    registry.add("smart_place", smart_place);
    registry.add("smart_profile", smart_profile);
    registry.add("smart_copy", smart_copy);
    registry.add("smart_transform", smart_transform);
    registry.add("rasterize_layer", rasterize_layer);
    register_smart_contents_ops(registry);  // (A1: contents and links, render/ops_smart_contents.cpp)
    smart_filters::register_ops(registry);  // (A2: render/ops_smart_filters.cpp)
    registry.add("smart_stack", smart_stack);  // (A4: stack modes, render/stack)
    registry.add("rasterize_lines", rasterize_lines);  // (A4: a line's balloon and letters as pixels)
}

// (PC3 C-e: a placed picture turned with its page, render/ops_pagesize.cpp)
namespace smart_ops {
core::Layer placed_to_smart(core::Document& doc, core::Layer placed, const std::string& id) { return render::placed_to_smart(doc, std::move(placed), id); }
// (type@1, PC5 G-0: a type layer's picture, render/type_layer.cpp)
Image drawn_alone(Size size, const std::function<void(Image&)>& draw) { return render::drawn_alone(size, draw); }

void fits_deep_page(const core::Document& doc, const core::Page& page) {
    if (page.color || !page.rgb || page.rgb->depth <= 8) return;
    for (const Layer& layer : page.layers) {
        if (layer.kind != LayerKind::Smart) continue;
        const core::SmartContents* contents = core::smart::contents_of(doc, layer);
        if (contents == nullptr || contents->kind != "file" || contents->depth <= 8) continue;
        if (static_cast<std::uint64_t>(contents->width_px) * static_cast<std::uint64_t>(contents->height_px) > core::kColorRasterMaxPixels) {
            throw OpError("the picture is too large to keep its 16 or 32 bits on this page (at most " + std::to_string(core::kColorRasterMaxPixels) +
                          " pixels)");
        }
    }
}
}  // namespace smart_ops

}  // namespace genko::render
