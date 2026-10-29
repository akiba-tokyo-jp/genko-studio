#include "storage/writer.hpp"

#include <algorithm>
#include <vector>

#include "core/adjust.hpp"
#include "core/brushes.hpp"
#include "core/channels.hpp"
#include "core/error.hpp"
#include "core/color_raster.hpp"
#include "core/exposure.hpp"
#include "core/ids.hpp"
#include "core/lut3d.hpp"
#include "core/ink.hpp"
#include "core/masks.hpp"
#include "core/pyconv.hpp"
#include "core/vector.hpp"
#include "core/file_info.hpp"
#include "core/psd_keep.hpp"
#include "core/ruby.hpp"
#include "core/filter_kinds.hpp"
#include "core/vanishing.hpp"
#include "core/smart.hpp"
#include "core/strokes.hpp"
#include "core/version.hpp"
#include "core/brush_dabs.hpp"
#include "core/layer_styles.hpp"
// PC5 J-0 (制作の補助)
#include "core/video.hpp"
#include "core/patterns.hpp"
#include "core/groups.hpp"
#include "core/type_text.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"

namespace genko::storage {

using core::Json;

namespace {

Json opt_string(const std::optional<std::string>& value) { return value ? Json(*value) : Json(nullptr); }
Json opt_num(const std::optional<core::Num>& value) { return value ? value->json() : Json(nullptr); }
Json opt_json(const std::optional<Json>& value) { return value ? *value : Json(nullptr); }

Json point_json(const core::Point& p) { return Json::array({p.x.json(), p.y.json()}); }

Json points_json(const std::vector<core::Point>& points) {
    Json out = Json::array();
    for (const auto& p : points) out.push_back(point_json(p));
    return out;
}

Json nums_json(const core::NumList& values) {
    Json out = Json::array();
    for (const auto& v : values) out.push_back(v.json());
    return out;
}

Json ints_json(const std::vector<std::int64_t>& values) {
    Json out = Json::array();
    for (const auto v : values) out.push_back(v);
    return out;
}

bool truthy(const std::optional<Json>& value) { return value && core::py_truthy(*value); }

// The asset ref of picture bytes, stored in `store` (put_bytes; the remembered ref when the bytes are unchanged).
std::string put_blob(AssetStore& store, const core::Bytes& bytes, const core::BlobMemo& memo, std::string_view suffix) {
    if (const std::string* ref = memo.ref_for(bytes)) {
        store.put_known(*ref, *bytes, suffix);
        return *ref;
    }
    return store.put_bytes(*bytes, suffix);
}

std::string put_png(AssetStore& store, const core::Bytes& png, const core::BlobMemo& memo) { return put_blob(store, png, memo, ".png"); }

// color-mode@1, color-model@1: a mode's profile written as its asset (the bytes held, or the asset already in the book).
void put_profile(AssetStore& store, const std::string& profile, const core::Bytes& icc) {
    if (profile.empty()) return;
    if (icc) {
        if (core::bytes_ref(*icc) != profile) {
            throw core::Error("value", "the ICC profile held is not " + profile + ": the book is not written");
        }
        store.put_known(profile, *icc, core::kProfileSuffix);
    } else if (!store.has(profile, core::kProfileSuffix)) {
        throw core::Error("value", "the ICC profile " + profile + " is not held: the book is not written");
    }
}

// A page's or the book's `color`: its mode of inks or its RGB mode (with its profile written), or none (null).
Json color_json(AssetStore& store, const std::optional<core::PageColor>& inks, const std::optional<core::RgbMode>& rgb) {
    if (inks) {
        if (inks->space != core::InkSpace::Lab && !inks->profile.empty()) put_profile(store, inks->profile, inks->icc);
        return core::to_json(*inks);
    }
    if (rgb && !rgb->is_default()) {
        put_profile(store, rgb->profile, rgb->icc);
        return core::to_json(*rgb);
    }
    return Json();
}

// io._frame_to_dict
Json frame_json(const core::Frame& frame) {
    Json out = Json::object();
    out["id"] = frame.id;
    out["rect"] = core::rect_to_json(frame.rect);
    out["split_axis"] = opt_string(frame.split_axis);
    out["clip"] = frame.clip;
    out["bleed"] = frame.bleed;
    out["border_mm"] = frame.border_mm;
    Json children = Json::array();
    for (const auto& child : frame.children) children.push_back(frame_json(child));
    out["children"] = std::move(children);
    out["panel"] = opt_json(frame.panel);
    if (frame.poly && !frame.poly->empty()) out["poly"] = points_json(*frame.poly);
    if (truthy(frame.split)) out["split"] = *frame.split;
    if (frame.custom) out["custom"] = true;
    if (frame.curves && !frame.curves->empty()) {
        Json curves = Json::array();
        for (const double c : *frame.curves) curves.push_back(c);
        out["curves"] = std::move(curves);
    }
    if (truthy(frame.line)) out["line"] = *frame.line;
    if (frame.corner_mm != 0.0) out["corner_mm"] = frame.corner_mm;
    return out;
}

Json record_json(const core::StoryLine& record);  // (type@1, PC5 G-0: below)

// io._layer_to_dict(layer, strokes=False) without "strokes" and "raster_relpath", then io._layer_to_v3.
Json layer_json(const core::Layer& layer, AssetStore& store, const std::string& space) {
    const bool placed = layer.kind == core::LayerKind::Placed;
    Json out = Json::object();
    out["id"] = layer.id;
    out["role"] = core::to_string(layer.role);
    out["kind"] = core::to_string(layer.kind);
    out["visible"] = layer.visible;
    out["exportable"] = layer.exportable;
    out["fill_rgb"] = layer.fill_rgb && !layer.fill_rgb->empty() ? nums_json(*layer.fill_rgb) : Json(nullptr);
    out["lpi"] = opt_num(layer.lpi);
    out["density"] = opt_num(layer.density);
    out["region"] = layer.region ? points_json(*layer.region) : Json(nullptr);
    out["opacity"] = layer.opacity;
    out["material_id"] = opt_string(layer.material_id);
    out["angle"] = layer.angle;
    out["title"] = layer.title;
    out["blend"] = layer.blend;
    out["clip"] = layer.clip;
    out["lock_alpha"] = layer.lock_alpha;
    out["parent_id"] = layer.parent_id;
    if (layer.color_raster) {  // (in tiles: a change to part of it adds only the tiles it changed)
        const core::ColorRasterView view(*layer.color_raster);
        std::vector<std::string> refs;
        for (const std::string& tile : core::color_tiles(*layer.color_raster)) refs.push_back(store.put_bytes(tile, core::kColorRasterSuffix));
        out["color_raster"] = core::tiled_metadata(view, refs, space);
    }
    if (!layer.patches.empty()) {
        Json patches = Json::array();
        for (const auto& patch : layer.patches) {
            if (placed) {
                // (_layer_to_v3 returns before the patches of a placed layer: they keep the refs they were read with)
                Json item = patch.attrs;
                if (patch.asset) item["asset"] = *patch.asset;
                for (const auto& [key, value] : patch.after_asset.items()) item[key] = value;
                patches.push_back(std::move(item));
            } else if (patch.png && !patch.png->empty()) {
                Json item = patch.attrs;
                item["asset"] = put_png(store, patch.png, patch.memo);
                for (const auto& [key, value] : patch.after_asset.items()) item[key] = value;
                patches.push_back(std::move(item));
            }
        }
        out["patches"] = std::move(patches);
    }
    if (layer.locked) out["locked"] = true;
    if (!layer.panel_clip) out["panel_clip"] = false;
    if (layer.panel_each) out["panel_each"] = true;
    if (truthy(layer.tone)) out["tone"] = *layer.tone;
    if (layer.mask) {
        Json mask = Json::object();
        mask["enabled"] = layer.mask->enabled;
        if (layer.mask->png && !layer.mask->png->empty()) mask["asset"] = put_png(store, layer.mask->png, layer.mask->memo);
        core::write_mask_settings(*layer.mask, mask);  // (masks@1, PC3: only what is not the default)
        out["mask"] = std::move(mask);
    }
    if (layer.color && !layer.color->empty()) out["color"] = ints_json(*layer.color);
    if (layer.reference) out["reference"] = true;
    if (truthy(layer.fill)) out["fill"] = *layer.fill;
    if (truthy(layer.adjust)) out["adjust"] = *layer.adjust;
    if (truthy(layer.effect)) out["effect"] = *layer.effect;
    if (layer.color_prints) out["color_prints"] = true;
    if (truthy(layer.screen)) out["screen"] = *layer.screen;
    if (truthy(layer.source) && !placed) out["source"] = *layer.source;
    // (PC3: vector@1's vector mask, transform@1's link and position lock — none: nothing written)
    if (layer.vector_mask) out["vector_mask"] = core::vector::to_json(*layer.vector_mask);
    if (layer.link) out["link"] = *layer.link;
    if (layer.lock_position) out["lock_position"] = true;
    if (layer.native_psd) out["native_psd"] = core::to_json(*layer.native_psd);  // (psd-keep@1, PC4)
    if (layer.style) out["style"] = *layer.style;  // (layer-styles@1, PC4 E6)
    if (layer.group) out["group"] = true;  // (groups@1, PC5 I-b)
    if (placed) {
        out["asset"] = opt_string(layer.asset);
        out["frame_id"] = opt_string(layer.frame_id);
        out["placement_mm"] = layer.placement_mm ? core::rect_to_json(*layer.placement_mm) : Json(nullptr);
        out["fit"] = layer.fit;
        out["clip_to"] = layer.clip_to;
        out["source"] = opt_json(layer.source);
        out["finish"] = opt_json(layer.finish);
        return out;
    }
    if (layer.kind == core::LayerKind::Smart) {  // (smart-objects@1: its panel as a placed picture's, and its settings)
        if (layer.frame_id) out["frame_id"] = *layer.frame_id;
        out["clip_to"] = layer.clip_to;
        if (truthy(layer.finish)) out["finish"] = *layer.finish;
        out["smart"] = layer.smart ? *layer.smart : Json::object();
        return out;
    }
    if (layer.kind == core::LayerKind::Type) {  // (type@1, PC5 G-0: its record; no pixels or lines of its own)
        if (layer.type) out["type"] = record_json(*layer.type);
        return out;
    }
    if (layer.raster_png && !layer.raster_png->empty()) {
        out["asset"] = put_png(store, layer.raster_png, layer.raster_memo);
    }
    if (layer.ink_raster) out["ink_raster"] = put_blob(store, layer.ink_raster, layer.ink_memo, core::kInkRasterSuffix);
    if (layer.stroke_count() > 0) {
        const core::StrokeList& list = *layer.strokes;
        std::string ref;
        if (!list.blob_ref.empty() && store.has(list.blob_ref, ".strokes.json")) {
            ref = list.blob_ref;  // the same strokes as the blob they were read from (Python's blobcache)
        } else {
            ref = store.put_bytes(core::strokes_blob(list), ".strokes.json");
        }
        out["strokes_blob"] = ref;
        out["stroke_count"] = static_cast<std::int64_t>(list.items.size());
    }
    // (pen-shapes@1, PC3: a pen layer's shape parts as their own asset, as its strokes are — none: nothing written)
    if (layer.shapes && !layer.shapes->items.empty()) {
        const core::vector::ShapeList& list = *layer.shapes;
        std::string ref;
        if (!list.blob_ref.empty() && store.has(list.blob_ref, core::vector::kShapesSuffix)) {
            ref = list.blob_ref;
        } else {
            ref = store.put_bytes(core::vector::shapes_blob(list), core::vector::kShapesSuffix);
        }
        out["shapes_blob"] = ref;
        out["shape_count"] = static_cast<std::int64_t>(list.items.size());
    }
    return out;
}

// io._line_to_dict
Json line_json(const core::StoryLine& line) {
    Json out = Json::object();
    out["id"] = line.id;
    out["page_index"] = line.page_index.json();
    out["text"] = line.text;
    out["speaker"] = line.speaker;
    out["frame_id"] = opt_string(line.frame_id);
    out["ruby"] = line.ruby;
    out["x_mm"] = line.x_mm.json();
    out["y_mm"] = line.y_mm.json();
    out["w_mm"] = line.w_mm.json();
    out["h_mm"] = line.h_mm.json();
    out["balloon"] = line.balloon;
    out["tail"] = line.tail ? point_json(*line.tail) : Json(nullptr);
    out["wrap"] = line.wrap;
    Json runs = Json::array();
    for (const auto& run : line.ruby_runs) runs.push_back(run);
    out["ruby_runs"] = std::move(runs);
    if (line.ruby_spans) out["ruby_spans"] = core::ruby::to_json(*line.ruby_spans);  // (ruby-layout@1 only)
    out["path"] = line.path ? points_json(*line.path) : Json(nullptr);
    if (!line.emphasis_runs.empty()) {
        Json marks = Json::array();
        for (const auto& mark : line.emphasis_runs) marks.push_back(mark);
        out["emphasis_runs"] = std::move(marks);
    }
    if (!line.style_runs.empty()) {
        Json styles = Json::array();
        for (const auto& [words, style] : line.style_runs) styles.push_back(Json::array({words, style}));
        out["style_runs"] = std::move(styles);
    }
    if (core::py_truthy(line.style)) out["style"] = line.style;
    if (!line.tails.empty()) {
        Json tails = Json::array();
        for (const auto& tail : line.tails) tails.push_back(tail);
        out["tails"] = std::move(tails);
    }
    if (line.native_type) out["native_type"] = *line.native_type;  // (type@1, PC5 G-0)
    return out;
}

// type@1 (PC5 G-0): a type layer's record — a line's, without what only the story has.
Json record_json(const core::StoryLine& record) {
    Json out = line_json(record);
    for (const char* key : {"id", "page_index", "speaker", "frame_id"}) out.erase(key);
    return out;
}

Json page_json(const core::Page& page, AssetStore& store) {
    Json out = Json::object();
    out["id"] = page.id;
    out["index"] = page.index.json();
    out["art_ok"] = page.art_ok;
    out["plan"] = opt_json(page.plan);
    out["note"] = page.note;
    out["name_ok"] = page.name_ok;
    out["stage"] = page.stage;
    out["spread_with"] = opt_num(page.spread_with);
    out["numero"] = page.numero;
    out["onion_from"] = opt_num(page.onion_from);
    out["lt_threshold"] = opt_num(page.lt_threshold);
    out["effects"] = page.effects;
    out["ruler"] = opt_json(page.ruler);
    out["rulers"] = page.rulers;
    out["prims"] = page.prims;
    Json frames = Json::array();
    for (const auto& frame : page.frames) frames.push_back(frame_json(frame));
    out["frames"] = std::move(frames);
    Json layers = Json::array();
    const std::string space = core::color_raster_space(page.rgb);  // (color-model@1: its precise rasters' space)
    for (const auto& layer : page.layers) layers.push_back(layer_json(layer, store, space));
    out["layers"] = std::move(layers);
    Json fills = Json::object();
    for (const auto& [role, rgb] : page.fills) fills[std::string(core::to_string(role))] = nums_json(rgb);
    out["fills"] = std::move(fills);
    if (Json color = color_json(store, page.color, page.rgb); !color.is_null()) out["color"] = std::move(color);
    if (!page.channels.empty()) {  // (channels@1: each one's raster as its asset)
        Json channels = Json::array();
        for (const core::Channel& channel : page.channels) {
            const std::string ref = channel.raster ? put_blob(store, channel.raster, channel.memo, core::kInkRasterSuffix) : std::string();
            channels.push_back(core::to_json(channel, ref));
        }
        out["channels"] = std::move(channels);
    }
    // (PC3: vector@1's paths, page-spec@1's own paper — none: nothing written)
    if (!page.paths.empty()) out["paths"] = core::vector::to_json(page.paths);
    if (page.work_path) out["work_path"] = core::vector::to_json(*page.work_path);
    if (page.clipping_path) out["clipping_path"] = core::vector::to_json(*page.clipping_path);
    if (page.own_spec) {
        Json spec = spec_to_json(page.spec);
        if (page.spec.trim_origin_mm) spec["trim_origin_mm"] = Json::array({page.spec.trim_origin_mm->first, page.spec.trim_origin_mm->second});
        out["native_spec"] = std::move(spec);
    }
    // (PC4: file-info@1's source file, psd-keep@1's kept resources and blocks — none: nothing written)
    if (page.native_source) out["native_source"] = core::to_json(*page.native_source);
    if (page.native_psd) out["native_psd"] = core::to_json(*page.native_psd);
    // (PC5 J-0: layer-comps@1's comps, slices@1's slices, annotations@1's notes, measure@1's measuring — none or empty:
    // nothing written)
    if (page.layer_comps && !page.layer_comps->empty()) out[std::string(core::kLayerCompsKey)] = core::to_json(*page.layer_comps);
    if (page.slices && !page.slices->empty()) out[std::string(core::kSlicesKey)] = core::to_json(*page.slices);
    if (!page.annotations.empty()) out[std::string(core::kAnnotationsKey)] = core::to_json(page.annotations);
    if (page.measure && !page.measure->empty()) out[std::string(core::kMeasureKey)] = core::to_json(*page.measure);
    for (const auto& [key, value] : page.extra.items()) {
        if (!is_known_page_key(key)) out[key] = value;
    }
    return out;
}

}  // namespace

Json writer_info() {
    Json out = Json::object();
    out["app"] = "genko-native";
    out["version"] = std::string(genko::version());
    return out;
}

Json spec_to_json(const core::PageSpec& spec) {
    Json out = Json::object();
    out["width_mm"] = spec.width_mm.json();
    out["height_mm"] = spec.height_mm.json();
    out["dpi"] = spec.dpi.json();
    out["bleed_mm"] = spec.bleed_mm.json();
    out["inner_margin_mm"] = spec.inner_margin_mm.json();
    out["expression"] = spec.expression;
    out["preset"] = opt_string(spec.preset);
    if (spec.trim_w_mm && spec.trim_w_mm->truthy()) {
        out["trim_w_mm"] = spec.trim_w_mm->json();
        out["trim_h_mm"] = opt_num(spec.trim_h_mm);
    }
    if (spec.margins_mm && !spec.margins_mm->empty()) out["margins_mm"] = nums_json(*spec.margins_mm);
    if (spec.grid) out["grid"] = *spec.grid;  // (image-grid@1, PC4)
    return out;
}

Json project_payload_v4(const core::Document& doc, AssetStore& store) {
    if (!doc.deferred.empty()) {
        // (pages whose strokes and pictures were not read would be written without them)
        throw core::Error("partial", "the book is not wholly read yet (" + std::to_string(doc.deferred.size()) +
                                         " page(s) without their strokes and pictures): it is not written");
    }
    if (!core::is_book_id(doc.book_id)) {
        throw core::Error("value", "the book has no valid book_id (32 hex digits): " + core::py_repr_str(doc.book_id));
    }
    core::validate_color_document(doc);
    std::vector<std::string> features = doc.features;
    for (const auto& page : doc.pages) for (const auto& layer : page->layers) {
        if (layer.color_raster) {
            features.emplace_back(core::kColorRasterFeature);
            features.emplace_back(core::kColorTilesFeature);
        }
        if (core::has_color_strokes(layer)) features.emplace_back(core::kColorStrokeFeature);
        if (layer.kind == core::LayerKind::Adjust && layer.adjust && layer.adjust->value("kind", core::Json()) == "exposure")
            features.emplace_back(core::kExposureFeature);
    }
    // BRUSH-01: the pictures of the papers the book's brushes keep (only those), and the feature that says so
    for (const std::string& ref : core::paper_refs(doc.brush_custom)) {
        const auto picture = doc.papers.find(ref);
        if (picture == doc.papers.end() || !picture->second) {
            throw core::Error("value", "the paper picture " + ref + " of the book's brushes is not held: the book is not written");
        }
        store.put_known(ref, *picture->second, ".png");
        features.emplace_back(core::kPaperFeature);
    }
    for (const auto& line : doc.story) {
        if (core::ruby::uses_feature(line)) features.emplace_back(core::kRubyLayoutFeature);
    }
    // (PC5 G-a review P3-3: a type layer's record holding its rubies by place says so too)
    for (const core::smart::WrittenPage& written : core::smart::written_pages(doc)) {
        for (const core::Layer& layer : written.page->layers) {
            if (layer.type && core::ruby::uses_feature(*layer.type)) features.emplace_back(core::kRubyLayoutFeature);
        }
    }
    // PC2a (adjustments@1): the 3D lookup tables the book's color_lookup corrections use (only those) — on its pages and
    // (PC4 F-f review P1-3) on the pages of the smart objects' contents its pages show, which the reader reads too
    std::vector<std::string> lut_refs = core::lut3d::refs_of(doc);
    for (const std::string& id : core::smart::used_contents(doc)) {
        const auto found = doc.smart_contents.find(id);
        if (found == doc.smart_contents.end() || !found->second || !found->second->page) continue;
        for (const std::string& ref : core::lut3d::refs_of(*found->second->page)) {
            if (std::find(lut_refs.begin(), lut_refs.end(), ref) == lut_refs.end()) lut_refs.push_back(ref);
        }
    }
    for (const std::string& ref : lut_refs) {
        const auto table = doc.luts.find(ref);
        if (table == doc.luts.end() || !table->second) {
            throw core::Error("value", "the lookup table " + ref + " of a color_lookup correction is not held: the book is not written");
        }
        store.put_known(ref, *table->second, core::lut3d::kSuffix);
    }
    // smart-objects@1: the contents the book's smart objects show (only those), and the files ops were given for them
    core::smart::validate_smart(doc);
    Json smart_contents = Json::object();
    for (const std::string& id : core::smart::used_contents(doc)) {
        const auto found = doc.smart_contents.find(id);
        if (found == doc.smart_contents.end() || !found->second) {
            throw core::Error("value", "the smart object contents " + id + " are not held: the book is not written");
        }
        const core::SmartContents& contents = *found->second;
        if (contents.kind == "page") {
            Json item = Json::object();
            item["kind"] = "page";
            item["name"] = contents.name;
            item["spec"] = spec_to_json(contents.page->spec);
            item["page"] = page_json(*contents.page, store);
            smart_contents[id] = std::move(item);
            continue;
        }
        if (const auto file = doc.smart_files.find(contents.asset); file != doc.smart_files.end() && file->second) {
            store.put_known(contents.asset, *file->second, contents.suffix);
        }
        if (const auto icc = contents.profile.is_object() ? contents.profile.find("icc") : contents.profile.end();
            icc != contents.profile.end() && icc->is_string()) {
            if (const auto file = doc.smart_files.find(icc->get<std::string>()); file != doc.smart_files.end() && file->second) {
                store.put_known(icc->get<std::string>(), *file->second, ".icc");
            }
        }
        // (PC3 A3: a PSD's linked-layer item, without the file's bytes)
        if (contents.psd && contents.psd->is_object() && contents.psd->contains("item") && (*contents.psd)["item"].is_string()) {
            const std::string ref = (*contents.psd)["item"].get<std::string>();
            if (const auto file = doc.smart_files.find(ref); file != doc.smart_files.end() && file->second) store.put_known(ref, *file->second, ".bin");
        }
        smart_contents[id] = core::smart::file_json(contents);
    }
    // (PC3 A2: the filter masks ops made for the smart objects, as the book's files)
    const auto filter_masks = [&](const core::Page& page) {
        for (const core::Layer& layer : page.layers) {
            if (layer.kind != core::LayerKind::Smart || !layer.smart || !layer.smart->is_object()) continue;
            const auto mask = layer.smart->find("filter_mask");
            if (mask == layer.smart->end() || !mask->is_object() || !mask->contains("asset") || !mask->at("asset").is_string()) continue;
            const std::string ref = mask->at("asset").get<std::string>();
            if (const auto file = doc.smart_files.find(ref); file != doc.smart_files.end() && file->second) store.put_known(ref, *file->second, ".png");
        }
        // (PC3 A3: a PSD's placed layer block and Photoshop's picture of it)
        for (const core::Layer& layer : page.layers) {
            if (layer.kind != core::LayerKind::Smart || !layer.smart || !layer.smart->is_object()) continue;
            const auto psd = layer.smart->find("psd");
            if (psd == layer.smart->end() || !psd->is_object()) continue;
            const auto put = [&](const core::Json& in, const char* key, std::string_view suffix) {
                const auto ref = in.find(key);
                if (ref == in.end() || !ref->is_string()) return;
                if (const auto file = doc.smart_files.find(ref->get<std::string>()); file != doc.smart_files.end() && file->second)
                    store.put_known(ref->get<std::string>(), *file->second, suffix);
            };
            put(*psd, "raw", ".bin");
            if (const auto shown = psd->find("shown"); shown != psd->end() && shown->is_object()) {
                put(*shown, "asset", ".png");
                put(*shown, "ink_raster", core::kInkRasterSuffix);
            }
        }
    };
    for (const auto& page : doc.pages) filter_masks(*page);
    for (const std::string& id : core::smart::used_contents(doc)) {
        if (const auto found = doc.smart_contents.find(id); found != doc.smart_contents.end() && found->second && found->second->page)
            filter_masks(*found->second->page);
    }
    std::erase(features, std::string(core::kSmartObjectsFeature));  // (named while a smart object is shown)
    if (!smart_contents.empty()) features.emplace_back(core::kSmartObjectsFeature);
    // PC4 節 D (filters@1): the files ops gave the smart filters (a displacement map), and the feature while a smart filter
    // is one of フィルター's kinds or holds a file
    core::validate_filters(doc);
    const auto filter_files = core::filter_file_refs(doc);
    for (const auto& [ref, suffix] : filter_files) {
        if (const auto file = doc.filter_files.find(ref); file != doc.filter_files.end() && file->second) store.put_known(ref, *file->second, suffix);
    }
    std::erase(features, std::string(core::kFiltersFeature));
    if (core::uses_filters(doc) || !filter_files.empty()) features.emplace_back(core::kFiltersFeature);
    // PC4 節 D6 (vanishing-point@1): a page's planes of 消点 (its "vanishing"), the feature while a page keeps them
    core::validate_vanishing(doc);
    std::erase(features, std::string(core::kVanishingFeature));
    if (core::uses_vanishing(doc)) features.emplace_back(core::kVanishingFeature);
    core::validate_ink_document(doc);
    core::validate_color_model(doc);
    bool modes = doc.color.has_value() || doc.rgb.has_value() || doc.text_gamma.has_value();
    for (const auto& page : doc.pages) modes = modes || page->color.has_value() || page->rgb.has_value();
    if (modes) features.emplace_back(core::kColorModeFeature);
    if (core::uses_color_model(doc)) features.emplace_back(core::kColorModelFeature);
    core::validate_channels(doc);
    std::erase(features, std::string(core::kChannelsFeature));  // (named while a page has a channel)
    if (core::uses_channels(doc)) features.emplace_back(core::kChannelsFeature);
    core::validate_adjustments(doc);
    std::erase(features, std::string(core::kAdjustmentsFeature));  // (named while a correction uses it)
    if (core::uses_adjustments(doc)) features.emplace_back(core::kAdjustmentsFeature);
    // PC3: masks@1, vector@1, pen-shapes@1, transform@1, page-spec@1 (each named while the book uses it)
    core::validate_masks(doc);
    core::vector::validate_vector(doc);
    const std::pair<std::string_view, bool> pc3[] = {{core::kMasksFeature, core::uses_masks(doc)},
                                                     {core::vector::kVectorFeature, core::vector::uses_vector(doc)},
                                                     {core::vector::kPenShapesFeature, core::vector::uses_pen_shapes(doc)},
                                                     {core::vector::kTransformFeature, core::vector::uses_transform(doc)},
                                                     {core::vector::kPageSpecFeature, core::vector::uses_page_spec(doc)}};
    for (const auto& [feature, used] : pc3) {
        std::erase(features, std::string(feature));
        if (used) features.emplace_back(feature);
    }
    // PC4 節 F: file-info@1, image-grid@1, psd-keep@1, formats@1, camera-raw@1 (each named while the book uses it), and the
    // metadata and kept PSD blocks it names that only the book in memory holds (a book opened from a file)
    core::validate_file_info(doc);
    core::validate_psd_keep(doc);
    const std::pair<std::string_view, bool> pc4f[] = {{core::kFileInfoFeature, core::uses_file_info(doc)},
                                                      {core::kImageGridFeature, core::uses_image_grid(doc)},
                                                      {core::kPsdKeepFeature, core::uses_psd_keep(doc)},
                                                      {core::kFormatsFeature, core::smart::uses_formats(doc)},
                                                      {core::kCameraRawFeature, core::smart::uses_camera_raw(doc)}};
    for (const auto& [feature, used] : pc4f) {
        std::erase(features, std::string(feature));
        if (used) features.emplace_back(feature);
    }
    for (const auto& named : {core::file_info_assets(doc), core::psd_keep_assets(doc)}) {
        for (const auto& [ref, suffix] : named) {
            if (const auto held = doc.native_files.find(ref); held != doc.native_files.end() && held->second) store.put_known(ref, *held->second, suffix);
        }
    }
    // PC4 E-0: brushes@1 (the dab brushes and their pictures — only those —, the lines' inputs, the brush state's kind and
    // background) and patterns@1 (the book's patterns and their pictures), each named while the book uses it
    core::validate_brushes(doc);
    for (const std::string& ref : core::dab_asset_refs(doc.brush_custom)) {
        const auto picture = doc.brush_tips.find(ref);
        if (picture == doc.brush_tips.end() || !picture->second) {
            throw core::Error("value", "the brush picture " + ref + " of the book's dab brushes is not held: the book is not written");
        }
        store.put_known(ref, *picture->second, ".png");
    }
    core::validate_patterns(doc);
    for (const core::Pattern& pattern : doc.patterns) store.put_known(pattern.asset, *doc.pattern_files.at(pattern.asset), core::pattern_suffix(pattern));
    // PC4 E6: layer-styles@1 (the layers' styles and the global light), named while the book uses it
    core::validate_layer_styles(doc);
    const std::pair<std::string_view, bool> pc4[] = {{core::kBrushesFeature, core::uses_brushes(doc)},
                                                     {core::kPatternsFeature, core::uses_patterns(doc)},
                                                     {core::kLayerStylesFeature, core::uses_layer_styles(doc)}};
    for (const auto& [feature, used] : pc4) {
        std::erase(features, std::string(feature));
        if (used) features.emplace_back(feature);
    }
    // PC5 G-0: type@1 (lines' native_type, type layers and the book's type styles), named while the book uses it
    core::type_text::validate_type(doc);
    std::erase(features, std::string(core::kTypeFeature));
    if (core::type_text::uses_type(doc)) features.emplace_back(core::kTypeFeature);
    // PC5 I-b: groups@1 (folders drawn as groups), named while the book uses it
    core::validate_groups(doc);
    std::erase(features, std::string(core::kGroupsFeature));
    if (core::uses_groups(doc)) features.emplace_back(core::kGroupsFeature);
    // PC5 J-0 (SPEC.md PROD-01): layer-comps@1, slices@1, annotations@1, measure@1 (a page's own keys) and timeline@1,
    // video@1 (in a page's anim), each named while the book uses it
    core::validate_layer_comps(doc);
    core::validate_slices(doc);
    core::validate_annotations(doc);
    core::validate_measure(doc);  // (timeline@1, video@1: named only for a work area, an fps, a video row of their shapes)
    const std::pair<std::string_view, bool> pc5j[] = {{core::kLayerCompsFeature, core::uses_layer_comps(doc)},
                                                      {core::kSlicesFeature, core::uses_slices(doc)},
                                                      {core::kAnnotationsFeature, core::uses_annotations(doc)},
                                                      {core::kMeasureFeature, core::uses_measure(doc)},
                                                      {core::kTimelineFeature, core::uses_timeline(doc)},
                                                      {core::kVideoFeature, core::uses_video(doc)}};
    for (const auto& [feature, used] : pc5j) {
        std::erase(features, std::string(feature));
        if (used) features.emplace_back(feature);
    }
    std::sort(features.begin(), features.end());
    features.erase(std::unique(features.begin(), features.end()), features.end());

    Json out = doc.extra.is_object() ? doc.extra : Json::object();
    out["version"] = kReadableVersion;
    out["min_reader"] = kReadableVersion;
    out["writer"] = writer_info();
    out["book_id"] = doc.book_id;
    out["features"] = features;
    out["revision"] = doc.revision;
    out["title"] = doc.title;
    out["episode"] = doc.episode.json();
    out["binding"] = core::to_string(doc.binding);
    out["start_side"] = opt_string(doc.start_side);
    out["strict_gates"] = doc.strict_gates;
    out["autosave"] = doc.autosave;
    out["font_path"] = doc.font_path;
    out["page_locks"] = doc.page_locks;
    if (core::py_truthy(doc.nombre)) out["nombre"] = doc.nombre;
    Json brush = Json::object();
    brush["rgb"] = ints_json(doc.brush_rgb);
    brush["width_mm"] = doc.brush_width_mm;
    brush["stabilize"] = doc.brush_stabilize;
    brush["taper"] = doc.brush_taper;
    brush["curve"] = doc.brush_curve;
    if (core::py_truthy(doc.brush_custom)) brush["custom"] = doc.brush_custom;
    if (doc.brush_kind) brush["kind"] = *doc.brush_kind;  // (brushes@1)
    if (doc.brush_background_rgb) brush["background_rgb"] = ints_json(*doc.brush_background_rgb);
    out["brush"] = std::move(brush);
    if (!doc.patterns.empty()) {  // (patterns@1)
        Json patterns = Json::array();
        for (const core::Pattern& pattern : doc.patterns) patterns.push_back(core::pattern_to_json(pattern));
        out["patterns"] = std::move(patterns);
    }
    if (doc.global_light) out["global_light"] = core::global_light_json(*doc.global_light);  // (layer-styles@1, PC4 E6)
    if (doc.type_styles) out["type_styles"] = *doc.type_styles;  // (type@1, PC5 G-0)
    out["spec"] = spec_to_json(doc.spec);
    if (Json color = color_json(store, doc.color, doc.rgb); !color.is_null() || doc.text_gamma) {
        // (the book's text_gamma, last, under its colour model: a default RGB one written for it)
        if (color.is_null()) color = core::to_json(core::RgbMode{});
        if (doc.text_gamma) color["text_gamma"] = *doc.text_gamma;
        out["color"] = std::move(color);
    }
    Json bible = Json::object();
    bible["plot"] = doc.bible.plot;
    bible["characters"] = doc.bible.characters;
    bible["constraints"] = doc.bible.constraints;
    out["bible"] = std::move(bible);
    out["tickets"] = doc.tickets;
    out["studio"] = doc.studio;
    if (doc.file_info && !doc.file_info->empty()) out["file_info"] = core::to_json(*doc.file_info);  // (file-info@1, PC4)
    if (!smart_contents.empty()) out["smart_contents"] = std::move(smart_contents);  // (smart-objects@1)
    Json pages = Json::array();
    for (const auto& page : doc.pages) pages.push_back(page_json(*page, store));
    out["pages"] = std::move(pages);
    Json story = Json::array();
    for (const auto& line : doc.story) story.push_back(line_json(line));
    out["story"] = std::move(story);
    return out;
}

std::string project_json_v4(const core::Document& doc, AssetStore& store) {
    return core::dump_python_indent2(project_payload_v4(doc, store));
}

}  // namespace genko::storage
