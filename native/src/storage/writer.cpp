#include "storage/writer.hpp"

#include <algorithm>
#include <vector>

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/strokes.hpp"
#include "core/version.hpp"
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
std::string put_png(AssetStore& store, const core::Bytes& png, const core::BlobMemo& memo) {
    if (const std::string* ref = memo.ref_for(png)) {
        store.put_known(*ref, *png, ".png");
        return *ref;
    }
    return store.put_bytes(*png, ".png");
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

// io._layer_to_dict(layer, strokes=False) without "strokes" and "raster_relpath", then io._layer_to_v3.
Json layer_json(const core::Layer& layer, AssetStore& store) {
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
    out["parent_id"] = opt_string(layer.parent_id);
    if (!layer.patches.empty()) {
        Json patches = Json::array();
        for (const auto& patch : layer.patches) {
            if (placed) {
                // (_layer_to_v3 returns before the patches of a placed layer: they keep the refs they were read with)
                Json item = patch.attrs;
                if (patch.asset) item["asset"] = *patch.asset;
                patches.push_back(std::move(item));
            } else if (patch.png && !patch.png->empty()) {
                Json item = patch.attrs;
                item["asset"] = put_png(store, patch.png, patch.memo);
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
    if (layer.raster_png && !layer.raster_png->empty()) {
        out["asset"] = put_png(store, layer.raster_png, layer.raster_memo);
    }
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
    for (const auto& layer : page.layers) layers.push_back(layer_json(layer, store));
    out["layers"] = std::move(layers);
    Json fills = Json::object();
    for (const auto& [role, rgb] : page.fills) fills[std::string(core::to_string(role))] = nums_json(rgb);
    out["fills"] = std::move(fills);
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
    return out;
}

Json project_payload_v4(const core::Document& doc, AssetStore& store) {
    if (!core::is_book_id(doc.book_id)) {
        throw core::Error("value", "the book has no valid book_id (32 hex digits): " + core::py_repr_str(doc.book_id));
    }
    std::vector<std::string> features = doc.features;
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
    out["brush"] = std::move(brush);
    out["spec"] = spec_to_json(doc.spec);
    Json bible = Json::object();
    bible["plot"] = doc.bible.plot;
    bible["characters"] = doc.bible.characters;
    bible["constraints"] = doc.bible.constraints;
    out["bible"] = std::move(bible);
    out["tickets"] = doc.tickets;
    out["studio"] = doc.studio;
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
