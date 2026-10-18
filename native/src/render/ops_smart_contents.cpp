// smart-objects@1 (PC3 A1, docs/cpp-migration/SPEC.md COLOR-05「中身とリンク」): the ops that change what a smart
// object's contents are — another file (smart_replace), a linked file's new place or its new copy (smart_relink,
// smart_update), embedded or linked (smart_embed, smart_link), a file made a page of its own to edit (smart_to_page).
// Every layer showing the contents shows the change (they are the book's, shared). The ops never read or write a path:
// the window and the command line read the file and give its bytes (an asset of the book, or "data"), and write a file
// before smart_link records it. A linked file's contents are always its last copy (the book draws, saves and undoes by
// itself, and draws when the file is gone). PhotoCraft v0.2.0's engine/src/smart_cmds.rs:582-601 and 840-880 (a smart
// object's source changed: its transform and filters kept, a PSD's record let go, a link made embedded keeping it), MIT
// (third_party/photocraft/NOTICE.md).

#include <algorithm>
#include <cmath>
#include <set>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_registry.hpp"
#include "render/ops_smart_parts.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/smart.hpp"
#include "render/smart_decode.hpp"

namespace genko::render {

namespace {

using core::Document;
using core::Json;
using core::Layer;
using core::LayerKind;
using core::OpContext;
using core::OpError;
using core::Page;
using core::SmartContents;
using core::smart::Matrix;

std::string text_at(const Json& op, std::string_view key, std::string fallback = {}) {
    const Json* v = core::get(op, key);
    if (v == nullptr || v->is_null()) return fallback;
    if (!v->is_string()) throw OpError(std::string(key) + " must be text");
    return v->get<std::string>();
}

// The smart object an op names ("page", "id"), its contents' id and its contents.
struct Target {
    std::size_t at = 0;
    std::size_t index = 0;
    std::string id;
    const SmartContents* contents = nullptr;
};

Target target(const Document& doc, const Json& op) {
    Target t;
    t.at = core::require_page(doc, op);
    const Page& page = doc.page(t.at);
    t.index = core::layer_by_id(page, core::truthy_at(op, "id") ? core::py_str(op["id"]) : std::string());
    const Layer& layer = page.layers[t.index];
    if (layer.kind != LayerKind::Smart) throw OpError("layer " + layer.id + " is not a smart object");
    const auto id = core::smart::contents_id(layer);
    t.contents = core::smart::contents_of(doc, layer);
    if (!id || t.contents == nullptr) throw OpError("the smart object has no contents");
    t.id = *id;
    return t;
}

// A file an op gives: its bytes ("data" in base64, or "asset": a file of the book's), what it is, and its asset ref.
struct Given {
    std::string bytes;
    std::string ref;
    smart::FileInfo info;
    bool data = false;
};

std::optional<Given> given_file(const Document& doc, const Json& op) {
    Given g;
    std::string suffix;
    if (core::truthy_at(op, "data")) {
        try {
            g.bytes = core::a2b_base64(text_at(op, "data"));
        } catch (const core::Error&) {
            throw OpError("data is the file's bytes in base64");
        }
        if (g.bytes.size() > smart::kFileMaxBytes) throw OpError("the file is larger than 256 MiB");
        g.data = true;
    } else if (core::truthy_at(op, "asset")) {
        const std::string ref = text_at(op, "asset");
        if (!ref.starts_with("sha256:")) throw OpError("the file is an asset ref (sha256:…) or data, not a path");
        if (const auto held = doc.smart_files.find(ref); held != doc.smart_files.end() && held->second) {
            g.bytes = *held->second;  // (given to an op before, not in assets/ yet)
        } else {
            std::tie(g.bytes, suffix) = smart_ops::asset_file(doc, ref);
        }
    } else {
        return std::nullopt;
    }
    try {
        g.info = smart::look_at(g.bytes, suffix);
    } catch (const NotYetPorted&) {
        throw;
    } catch (const core::Error& error) {
        throw OpError(std::string("the asset's picture cannot be read (") + error.what() + ")");
    }
    g.ref = core::bytes_ref(g.bytes);
    if (core::truthy_at(op, "asset") && g.ref != text_at(op, "asset")) throw OpError("the asset's picture cannot be read (its bytes are not what its name says)");
    return g;
}

Given required_file(const Document& doc, const Json& op) {
    auto g = given_file(doc, op);
    if (!g) throw OpError("asset (sha256:…) or data gives the file");
    return std::move(*g);
}

// The file's name from a path ("/a/b/photo.tif" → "photo.tif").
std::string base_name(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

Json smart_ops::link_of(const Json& op, std::string_view bytes) {
    const Json* given = core::get(op, "link");
    if (given == nullptr || !given->is_object()) throw OpError("link is {path, relative?}");
    const std::string path = text_at(*given, "path");
    if (path.empty()) throw OpError("link names the file's path");
    std::optional<std::string> relative;
    if (const Json* r = core::get(*given, "relative"); r != nullptr && !r->is_null()) relative = text_at(*given, "relative");
    std::string sha = core::bytes_ref(bytes).substr(7);
    if (const Json* s = core::get(*given, "sha256"); s != nullptr && !s->is_null()) sha = text_at(*given, "sha256");
    std::int64_t size = static_cast<std::int64_t>(bytes.size());
    if (const Json* b = core::get(*given, "bytes"); b != nullptr && !b->is_null()) {
        if (!b->is_number_integer()) throw OpError("link's bytes is the file's size");
        size = b->get<std::int64_t>();
    }
    Json out = core::smart::link_json(path, relative, sha, size);
    try {
        core::smart::check_link(out);
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
    return out;
}

namespace {

using smart_ops::link_of;

// The contents of a file given: what it is, its colour use as the op asks (keep by default), its dpi the file's (the
// page's when it says none).
SmartContents file_contents(Document& doc, const Json& op, const Given& g, std::string name, double page_dpi) {
    SmartContents c;
    c.kind = "file";
    c.name = name.empty() ? std::string("画像") + g.info.suffix : std::move(name);
    c.asset = g.ref;
    c.suffix = g.info.suffix;
    c.format = g.info.format;
    c.width_px = g.info.width;
    c.height_px = g.info.height;
    c.dpi = g.info.dpi > 0 ? g.info.dpi : page_dpi;
    c.depth = g.info.depth;
    c.mode = g.info.mode;
    c.profile = smart_ops::colour_use(doc, op, g.info);
    return c;
}

// A file of more than 8 bits for a page of 16 or 32 bits showing it: within what such a page keeps whole.
void fits_deep_pages(const Document& doc, const std::string& id, const smart::FileInfo& info) {
    if (info.depth <= 8 || static_cast<std::uint64_t>(info.width) * info.height <= core::kColorRasterMaxPixels) return;
    for (const std::size_t at : core::smart::pages_using(doc, id)) {
        const Page& page = doc.page(at);
        if (!page.color && page.rgb && page.rgb->depth > 8) {
            throw OpError("the picture is too large to keep its 16 or 32 bits on this page (at most " + std::to_string(core::kColorRasterMaxPixels) +
                          " pixels)");
        }
    }
}

// Each smart object layer showing these contents — on the book's pages and on other contents' pages —, to change.
template <class Each>
void each_showing(Document& doc, const std::string& id, Each each) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.is_deferred(i)) continue;
        const Page& seen = doc.page(i);
        if (std::none_of(seen.layers.begin(), seen.layers.end(), [&](const Layer& l) { return l.kind == LayerKind::Smart && core::smart::contents_id(l) == id; }))
            continue;
        for (Layer& layer : doc.edit_page(i).layers) {
            if (layer.kind == LayerKind::Smart && core::smart::contents_id(layer) == id) each(layer);
        }
    }
    std::vector<std::string> inside;
    for (const auto& [other, contents] : doc.smart_contents) {
        if (other == id || !contents || !contents->page) continue;
        const auto& layers = contents->page->layers;
        if (std::any_of(layers.begin(), layers.end(), [&](const Layer& l) { return l.kind == LayerKind::Smart && core::smart::contents_id(l) == id; }))
            inside.push_back(other);
    }
    for (const std::string& other : inside) {
        for (Layer& layer : doc.edit_contents_page(other).layers) {
            if (layer.kind == LayerKind::Smart && core::smart::contents_id(layer) == id) each(layer);
        }
    }
}

// The contents changed: every page showing them looks different (their pages are new values, so a view that keeps the
// page it drew sees it), and a PSD's own picture of them is let go (PC3 A3: shown until the contents change).
void changed(Document& doc, const std::string& id, bool keep_psd = false) {
    if (!keep_psd) {
        each_showing(doc, id, [](Layer& layer) {
            if (layer.smart && layer.smart->is_object() && layer.smart->contains("psd")) layer.smart->erase("psd");
        });
    }
    core::smart::renew_containers(doc, id);
    for (const std::size_t at : core::smart::pages_using(doc, id)) {
        if (!doc.is_deferred(at)) (void)doc.edit_page(at);
    }
}

Json report(const Document& doc, const Target& t) {
    const SmartContents& c = *doc.smart_contents.at(t.id);
    Json out{{"id", doc.page(t.at).layers[t.index].id}, {"contents", t.id}, {"kind", c.kind}};
    if (c.kind == "file") {
        out["size_px"] = Json::array({c.width_px, c.height_px});
        out["depth"] = c.depth;
        out["linked"] = c.link.has_value();
    }
    return out;
}

// --- smart_replace (コンテンツを置換…) -----------------------------------------------------------------------------------

void smart_replace(OpContext& c) {
    Document& doc = c.doc;
    const Target t = target(doc, c.op);
    const Given g = required_file(doc, c.op);
    fits_deep_pages(doc, t.id, g.info);
    SmartContents next = file_contents(doc, c.op, g, text_at(c.op, "name"), doc.page(t.at).spec.dpi.value());
    if (g.data) doc.smart_files[g.ref] = std::make_shared<const std::string>(g.bytes);
    doc.edit_contents(t.id) = std::move(next);  // (embedded now: a file placed in it, its transform and filters kept)
    changed(doc, t.id);
    c.report = report(doc, t);
}

// --- smart_relink (ファイルに再リンク…) ---------------------------------------------------------------------------------

void smart_relink(OpContext& c) {
    Document& doc = c.doc;
    const Target t = target(doc, c.op);
    const Given g = required_file(doc, c.op);
    const Json link = link_of(c.op, g.bytes);
    if (t.contents->kind == "file" && t.contents->asset == g.ref) {  // (the same bytes in another place: only the record)
        doc.edit_contents(t.id).link = link;
        c.report = report(doc, t);
        return;
    }
    fits_deep_pages(doc, t.id, g.info);
    std::string name = text_at(c.op, "name");
    if (name.empty()) name = base_name(link["path"].get<std::string>());
    SmartContents next = file_contents(doc, c.op, g, std::move(name), doc.page(t.at).spec.dpi.value());
    next.link = link;
    if (g.data) doc.smart_files[g.ref] = std::make_shared<const std::string>(g.bytes);
    doc.edit_contents(t.id) = std::move(next);
    changed(doc, t.id);
    c.report = report(doc, t);
}

// --- smart_update (変更されたコンテンツを更新・変更されたすべてのコンテンツを更新) --------------------------------------------

void smart_update(OpContext& c) {
    Document& doc = c.doc;
    const Target t = target(doc, c.op);
    if (t.contents->kind != "file" || !t.contents->link) throw OpError("the smart object is not linked to a file");
    const Given g = required_file(doc, c.op);
    Json link = *t.contents->link;
    link["sha256"] = g.ref.substr(7);
    link["bytes"] = static_cast<std::int64_t>(g.bytes.size());
    if (g.ref == t.contents->asset) {  // (the file as the book holds it: nothing to update)
        if (link != *t.contents->link) doc.edit_contents(t.id).link = link;
        c.report = report(doc, t);
        (*c.report)["updated"] = false;
        return;
    }
    fits_deep_pages(doc, t.id, g.info);
    const smart::FileInfo& info = g.info;
    if (g.data) doc.smart_files[g.ref] = std::make_shared<const std::string>(g.bytes);
    SmartContents& next = doc.edit_contents(t.id);
    next.asset = g.ref;
    next.suffix = info.suffix;
    next.format = info.format;
    next.width_px = info.width;
    next.height_px = info.height;
    next.dpi = info.dpi > 0 ? info.dpi : doc.page(t.at).spec.dpi.value();
    next.depth = info.depth;
    next.mode = info.mode;
    next.link = std::move(link);
    next.psd.reset();
    // (its colour use as it was — from the profile the file now carries — unless the op says; one it no longer carries:
    // its numbers)
    if (core::truthy_at(c.op, "embedded_profile")) {
        next.profile = smart_ops::colour_use(doc, c.op, info);
    } else if (core::py_get(next.profile, "use", Json("keep")) == Json("embedded") && !info.icc) {
        next.profile = Json::object({{"use", "keep"}});
    }
    changed(doc, t.id);
    c.report = report(doc, t);
    (*c.report)["updated"] = true;
}

// --- smart_embed (埋め込みに変換) -------------------------------------------------------------------------------------

void smart_embed(OpContext& c) {
    Document& doc = c.doc;
    const Target t = target(doc, c.op);
    if (t.contents->kind != "file" || !t.contents->link) throw OpError("the smart object is not linked to a file");
    doc.edit_contents(t.id).link.reset();  // (its copy is its contents now; a PSD's record kept)
    c.report = report(doc, t);
}

// --- smart_link (リンクに変換…) ---------------------------------------------------------------------------------------

void smart_link(OpContext& c) {
    Document& doc = c.doc;
    const Target t = target(doc, c.op);
    const std::optional<Given> g = given_file(doc, c.op);
    if (t.contents->kind == "page") {
        // (contents of a page are linked as the file they were written to — a PSD the window or the command line wrote —,
        // laid as large as they were: their transform keeps its place)
        if (!g) throw OpError("contents of a page are linked as the file they were written to: asset (sha256:…) or data gives it");
        fits_deep_pages(doc, t.id, g->info);
        const double w_mm = core::smart::size_mm(*t.contents).first;
        const Json link = link_of(c.op, g->bytes);
        SmartContents next = file_contents(doc, c.op, *g, base_name(link["path"].get<std::string>()), doc.page(t.at).spec.dpi.value());
        if (w_mm > 0) next.dpi = static_cast<double>(next.width_px) * 25.4 / w_mm;
        next.link = link;
        if (g->data) doc.smart_files[g->ref] = std::make_shared<const std::string>(g->bytes);
        doc.edit_contents(t.id) = std::move(next);
        changed(doc, t.id);
        c.report = report(doc, t);
        return;
    }
    if (g && g->ref != t.contents->asset) throw OpError("a file's contents are linked as they are: the file written holds their bytes");
    const core::Bytes bytes = smart::file_bytes(doc, *t.contents);
    if (!bytes) throw OpError("the smart object's file is not in the book");
    doc.edit_contents(t.id).link = link_of(c.op, *bytes);
    c.report = report(doc, t);
}

// --- smart_to_page (コンテンツを編集 の前に: ファイルをページに) ------------------------------------------------------------

// A paint layer drawn on a page a pixel wider on every side, cut to the `width` × `height` pixels inside that margin (its
// PNG, its precise raster or its inks, as they are), for the page `page`.
void cut_margin(const Page& page, Layer& layer, std::uint32_t width, std::uint32_t height) {
    if (layer.ink_raster) {
        const core::InkPixels wide = core::decode_ink_raster(*layer.ink_raster);
        core::InkPixels cut = core::blank_ink_pixels(wide.space, wide.depth, width, height, wide.planes);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) cut.copy_pixel(std::size_t(y) * width + x, wide, std::size_t(y + 1) * wide.width + x + 1);
        }
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(cut));
        layer.ink_memo = {};
    } else if (layer.color_raster) {
        const core::ColorRasterView wide(*layer.color_raster);
        const std::string precision = page.rgb && page.rgb->depth == 16 ? "u16" : "f32";  // (as rasterize_layer writes it)
        layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(
            width, height, precision, [&](std::size_t i) { return wide.pixel((i / width + 1) * wide.width() + i % width + 1); }));
    } else if (layer.raster_png) {
        const Image wide = open_image(*layer.raster_png, kPillowOpenLimits).convert("RGBA");
        layer.raster_png = nullptr;
        layer.raster_memo = {};
        raster::save_raster(page, layer, wide.crop(Box{1, 1, static_cast<int>(width) + 1, static_cast<int>(height) + 1}));
    }
}

// A file's contents as a page of their own to edit (one paint layer of the picture), laid as they were: the page as large
// as the picture at the paint layers' 200 dpi (a pixel of the file a pixel of the layer), in the colour mode of the page
// the smart object is on; every smart object showing them keeps its place (its matrix takes the page's size).
void smart_to_page(OpContext& c) {
    Document& doc = c.doc;
    const Target t = target(doc, c.op);
    if (t.contents->kind != "file") throw OpError("the smart object's contents are a page already");
    if (!doc.deferred.empty()) throw OpError("the book's pages are still being read: try again in a moment");
    const SmartContents was = *t.contents;
    const Page& seen = doc.page(t.at);
    const auto [old_w, old_h] = core::smart::size_mm(was);
    const double new_w = static_cast<double>(was.width_px) * 25.4 / raster::kWorkingDpi;
    const double new_h = static_cast<double>(was.height_px) * 25.4 / raster::kWorkingDpi;
    // (its page: the picture's size, no bleed, margins or panels; the colours of the page it is shown on)
    Page inner;
    inner.id = core::new_page_id();
    inner.index = core::Num(1);
    inner.spec = seen.spec;
    inner.spec.width_mm = core::Num(new_w);
    inner.spec.height_mm = core::Num(new_h);
    inner.spec.bleed_mm = core::Num(0);
    inner.spec.inner_margin_mm = core::Num(0);
    inner.spec.preset.reset();
    inner.spec.trim_w_mm.reset();
    inner.spec.trim_h_mm.reset();
    inner.spec.margins_mm.reset();
    inner.binding = seen.binding;
    inner.color = seen.color;
    inner.rgb = seen.rgb;
    // the picture drawn as rasterize_layer draws a smart object (no finish: a page's contents take none), a pixel of the
    // file a pixel of the paint layer — on a page a pixel wider on every side (a placed picture's edge on the page's edge
    // is not drawn whole), then cut to it
    Layer shown;
    shown.id = "smart";
    shown.kind = LayerKind::Smart;
    shown.role = core::LayerRole::Draft;  // (its picture as it is: the monochrome finish is the smart object's, on its page)
    shown.clip_to = "none";
    const Layer& source = seen.layers[t.index];
    const std::string interp = source.smart ? core::py_str(core::py_get(*source.smart, "interp", Json("lanczos"))) : std::string("lanczos");
    const double px = 25.4 / raster::kWorkingDpi;
    shown.smart = core::smart::settings(t.id, Matrix{new_w / old_w, 0, 0, new_h / old_h, px, px}, interp);
    Document work = doc;
    Page drawn = inner;
    drawn.spec.width_mm = core::Num(new_w + 2 * px);
    drawn.spec.height_mm = core::Num(new_h + 2 * px);
    drawn.layers.push_back(std::move(shown));
    work.pages = {std::make_shared<Page>(std::move(drawn))};
    work.deferred.clear();
    work.story.clear();
    smart_ops::rasterize(work, 0, 0);
    Layer picture = work.page(0).layers[0];
    cut_margin(inner, picture, static_cast<std::uint32_t>(was.width_px), static_cast<std::uint32_t>(was.height_px));
    picture.id = core::truthy_at(c.op, "new_id") ? core::py_str(c.op["new_id"]) : core::new_id();
    std::string title = was.name;
    if (const auto dot = title.rfind('.'); dot != std::string::npos && dot > 0) title = title.substr(0, dot);
    picture.title = title.empty() ? std::string("画像") : title;
    picture.role = core::LayerRole::User;
    picture.exportable = true;
    picture.panel_clip = false;
    picture.panel_each = false;
    picture.clip_to = "frame";
    for (const std::string& feature : work.features) {
        if (std::find(doc.features.begin(), doc.features.end(), feature) == doc.features.end()) doc.features.push_back(feature);
    }
    inner.layers.push_back(std::move(picture));
    SmartContents next;
    next.kind = "page";
    next.name = was.name;
    next.page = std::make_shared<Page>(std::move(inner));
    doc.edit_contents(t.id) = std::move(next);
    // (each smart object showing them: laid as before — the page's mm are the picture's at 200 dpi)
    const Matrix scale{old_w / new_w, 0, 0, old_h / new_h, 0, 0};
    each_showing(doc, t.id, [&](Layer& layer) {
        core::smart::set_matrix(layer, core::smart::multiply(core::smart::matrix_of(layer), scale));
        if (layer.smart->contains("psd")) layer.smart->erase("psd");
    });
    changed(doc, t.id, true);
    c.report = report(doc, t);
    (*c.report)["layer"] = doc.smart_contents.at(t.id)->page->layers.front().id;
}

}  // namespace

void register_smart_contents_ops(core::OpRegistry& registry) {
    registry.add("smart_replace", smart_replace);
    registry.add("smart_relink", smart_relink);
    registry.add("smart_update", smart_update);
    registry.add("smart_embed", smart_embed);
    registry.add("smart_link", smart_link);
    registry.add("smart_to_page", smart_to_page);
}

}  // namespace genko::render
