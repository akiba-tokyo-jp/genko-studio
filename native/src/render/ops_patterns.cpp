// PC4 節 E (SPEC.md COLOR-09, patterns@1): the ops of the book's patterns (E3) — a pattern the book keeps
// (core::Pattern, Document::patterns and pattern_files) added from its picture, renamed, and deleted when nothing uses it.
// PhotoCraft v0.2.0 crates/engine/src/pattern_cmds.rs (ensure_in_doc, rename, delete), MIT:
// native/third_party/photocraft/NOTICE.md; the book keeps its own copy, the library is the machine's
// (render/pattern_library).

#include "render/ops_patterns.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/patterns.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "core/color_model.hpp"
#include "core/strokes.hpp"
#include "render/color_canvas.hpp"
#include "render/fill.hpp"
#include "render/image_ops.hpp"
#include "render/op_limits.hpp"
#include "render/ops_paint_tools.hpp"
#include "render/page_internal.hpp"
#include "render/pattern_library.hpp"
#include "render/page.hpp"
#include "render/paint_fill.hpp"
#include "render/patterns.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"
#include "render/selection.hpp"
#include "render/shape_parts.hpp"

namespace genko::render {

using core::Json;
using core::OpError;

namespace {

const Json* given_key(const Json& op, const char* key) {
    const auto v = op.find(key);
    return v != op.end() && !v->is_null() ? &*v : nullptr;
}

std::string text_of(const Json& op, const char* key) {
    const Json* v = given_key(op, key);
    if (v == nullptr) return {};
    if (!v->is_string()) throw OpError(std::string(key) + " must be text");
    return v->get<std::string>();
}

std::string decoded(const Json& op, const char* key) {
    const std::string text = text_of(op, key);
    try {
        return core::a2b_base64(text);
    } catch (const core::Error&) {
        throw OpError(std::string(key) + " must be base64");
    }
}

// The book's pattern of the same picture, mode, depth and dpi.
const core::Pattern* same(const core::Document& doc, const core::Pattern& p) {
    for (const core::Pattern& kept : doc.patterns) {
        if (kept.asset == p.asset && kept.mode == p.mode && kept.depth == p.depth && kept.dpi == p.dpi && kept.width == p.width &&
            kept.height == p.height) {
            return &kept;
        }
    }
    return nullptr;
}

void add_pattern(core::OpContext& c) {
    const auto [id, added] = add_pattern_to(c.doc, c.op);
    c.report = Json{{"pattern", id}, {"added", added}};
}

void rename_pattern(core::OpContext& c) {
    const std::string id = text_of(c.op, "id");
    const std::string name = text_of(c.op, "name");
    auto it = std::find_if(c.doc.patterns.begin(), c.doc.patterns.end(), [&](const core::Pattern& p) { return p.id == id; });
    if (it == c.doc.patterns.end()) throw OpError("the book has no pattern " + core::py_repr_str(id));
    const std::size_t chars = core::ruby::code_points(name).size();
    if (chars < 1 || chars > 255) throw OpError("name must be 1 to 255 characters");
    it->name = name;
    c.report = Json{{"pattern", id}, {"name", name}};
}

void delete_pattern(core::OpContext& c) {
    const std::string id = text_of(c.op, "id");
    auto it = std::find_if(c.doc.patterns.begin(), c.doc.patterns.end(), [&](const core::Pattern& p) { return p.id == id; });
    if (it == c.doc.patterns.end()) throw OpError("the book has no pattern " + core::py_repr_str(id));
    if (core::patterns_used(c.doc).contains(id)) {
        throw OpError("the pattern " + id + " is used by a fill (change the fill first: a pattern in use is never taken out)");
    }
    const std::string asset = it->asset;
    c.doc.patterns.erase(it);
    if (std::none_of(c.doc.patterns.begin(), c.doc.patterns.end(), [&](const core::Pattern& p) { return p.asset == asset; })) {
        c.doc.pattern_files.erase(asset);
    }
    c.report = Json{{"deleted", id}};
}

}  // namespace

std::pair<std::string, bool> add_pattern_to(core::Document& doc, const Json& op) {
    if (!op.is_object()) throw OpError("the pattern is {name?, png | gkcr | gkir | asset, mode?, depth?, dpi?}");
    int sources = 0;
    for (const char* key : {"png", "gkcr", "gkir", "asset"}) sources += given_key(op, key) != nullptr ? 1 : 0;
    if (sources != 1) throw OpError("give the pattern's picture as one of png, gkcr, gkir (base64) and asset");
    std::string mode = text_of(op, "mode");
    if (!mode.empty() && mode != "rgb" && mode != "gray" && mode != "cmyk" && mode != "lab") throw OpError("mode must be rgb, gray, cmyk or lab");
    core::Pattern p;
    std::string bytes;
    if (given_key(op, "png") != nullptr) {
        Image picture;
        try {
            picture = read_png(decoded(op, "png"), PngLimits{core::kMaxPatternPixels, core::kMaxPatternSide});
        } catch (const core::Error& error) {
            throw OpError(std::string("png: ") + error.what());
        }
        const bool grey_picture = picture.mode() == "L" || picture.mode() == "LA" || picture.mode() == "1" || picture.mode() == "I;16";
        if (mode.empty()) mode = grey_picture ? "gray" : "rgb";
        if (mode != "rgb" && mode != "gray") throw OpError("a PNG pattern is rgb or gray (cmyk and lab are given as gkir)");
        const Image rgba = picture.mode() == "RGBA" || picture.mode() == "LA" ? picture : picture.convert(picture.mode() == "L" || picture.mode() == "1" ? "LA" : "RGBA");
        const Image taken = mode == "rgb" ? (rgba.mode() == "RGBA" ? rgba : rgba.convert("RGBA")) : (rgba.mode() == "LA" ? rgba : rgba.convert("LA"));
        bytes = write_png(taken);
        p.mode = mode;
        p.depth = 8;
        p.width = taken.width();
        p.height = taken.height();
    } else if (given_key(op, "gkcr") != nullptr) {
        bytes = decoded(op, "gkcr");
        try {
            const core::ColorRasterView view(bytes);
            const Json meta = view.metadata("");
            const Json* precision = core::get(meta, "precision");
            p.depth = precision != nullptr && precision->is_string() && precision->get<std::string>() == "f32" ? 32 : 16;
            p.width = view.width();
            p.height = view.height();
        } catch (const core::Error& error) {
            throw OpError(std::string("gkcr: ") + error.what());
        }
        if (!mode.empty() && mode != "rgb") throw OpError("a GKCR pattern is rgb");
        p.mode = "rgb";
    } else if (given_key(op, "gkir") != nullptr) {
        bytes = decoded(op, "gkir");
        core::InkRasterHeader header;
        try {
            header = core::ink_raster_header(bytes);
        } catch (const core::Error& error) {
            throw OpError(std::string("gkir: ") + error.what());
        }
        const std::string space = header.space == core::InkSpace::Cmyk  ? "cmyk"
                                  : header.space == core::InkSpace::Lab ? "lab"
                                  : header.space == core::InkSpace::Gray ? "gray"
                                                                          : "";
        if (space.empty()) throw OpError("a GKIR pattern is grey, CMYK or Lab");
        if (!mode.empty() && mode != space) throw OpError("the GKIR picture is " + space + ", not " + mode);
        p.mode = space;
        p.depth = header.depth;
        p.width = header.width;
        p.height = header.height;
        if (p.width > core::kMaxPatternSide || p.height > core::kMaxPatternSide || p.width * p.height > core::kMaxPatternPixels) {
            throw OpError("a pattern is at most 8192 pixels a side and 64 million pixels");  // (before its pixels are read)
        }
        if (p.mode == "gray" && p.depth == 8) {  // (an 8-bit grey pattern is kept as an LA PNG)
            patterns::Tile t;
            try {
                const core::InkPixels inks = core::decode_ink_raster(bytes);
                t.width = inks.width;
                t.height = inks.height;
                t.colours = 1;
                t.values.resize(inks.pixel_count() * 2);
                for (std::size_t i = 0; i < inks.pixel_count(); ++i) {
                    t.values[i * 2] = static_cast<float>(inks.value(i, 0));
                    t.values[i * 2 + 1] = static_cast<float>(inks.value(i, 1));
                }
            } catch (const core::Error& error) {
                throw OpError(std::string("gkir: ") + error.what());
            }
            t.mode = "gray";
            t.depth = 8;
            bytes = patterns::encode(t);
        }
    } else {
        const std::string asset = text_of(op, "asset");
        const auto held = std::find_if(doc.patterns.begin(), doc.patterns.end(), [&](const core::Pattern& kept) { return kept.asset == asset; });
        if (held == doc.patterns.end() || !doc.pattern_files.contains(asset)) throw OpError("the book holds no pattern picture " + asset);
        p = *held;
        bytes = *doc.pattern_files.at(asset);
    }
    p.asset = core::bytes_ref(bytes);
    p.dpi = 300.0;
    if (const Json* dpi = given_key(op, "dpi")) {
        if (!dpi->is_number() || dpi->is_boolean() || !std::isfinite(dpi->get<double>()) || dpi->get<double>() < 50 || dpi->get<double>() > 2400) {
            throw OpError("dpi must be a number between 50 and 2400");
        }
        p.dpi = dpi->get<double>();
    }
    if (const Json* depth = given_key(op, "depth")) {
        if (!depth->is_number_integer() || depth->get<std::int64_t>() != p.depth) {
            throw OpError("depth: the picture is of " + std::to_string(p.depth) + " bits");
        }
    }
    p.id = text_of(op, "id");
    p.name = text_of(op, "name");
    if (p.name.empty()) p.name = "パターン " + std::to_string(doc.patterns.size() + 1);
    if (p.width > core::kMaxPatternSide || p.height > core::kMaxPatternSide || p.width * p.height > core::kMaxPatternPixels) {
        throw OpError("a pattern is at most 8192 pixels a side and 64 million pixels");
    }
    if (const auto why = core::check_pattern_file(p, bytes)) throw OpError("the pattern's picture: " + *why);
    if (p.id.empty()) {
        if (const core::Pattern* kept = same(doc, p)) return {kept->id, false};
        p.id = core::next_pattern_id(doc);
    } else if (const core::Pattern* kept = core::find_pattern(doc, p.id)) {
        if (kept->asset == p.asset && kept->mode == p.mode && kept->depth == p.depth && kept->dpi == p.dpi) return {kept->id, false};
        throw OpError("the book has another pattern " + p.id);
    }
    try {
        (void)core::pattern_from_json(core::pattern_to_json(p));
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
    if (doc.patterns.size() >= core::kMaxPatterns) throw OpError("a book keeps at most " + std::to_string(core::kMaxPatterns) + " patterns");
    doc.pattern_files[p.asset] = std::make_shared<const std::string>(std::move(bytes));
    doc.patterns.push_back(p);
    return {p.id, true};
}

std::pair<std::string, bool> keep_pattern(core::Document& doc, core::Pattern p, const core::Bytes& bytes) {
    try {
        return core::carry_pattern(doc, std::move(p), bytes);
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
}

Json fill_pattern(core::Document& doc, const Json& given) {
    if (!given.is_object()) throw OpError("pattern is {id, scale?, angle?, phase_mm?, link?} or a picture {png | gkcr | gkir | asset, …}");
    Json placement = Json::object(), picture = Json::object();
    for (const auto& [key, value] : given.items()) {
        if (key == "id" || key == "scale" || key == "angle" || key == "phase_mm" || key == "link") {
            placement[key] = value;
        } else if (key == "png" || key == "gkcr" || key == "gkir" || key == "asset" || key == "name" || key == "mode" || key == "depth" || key == "dpi") {
            picture[key] = value;
        } else {
            throw OpError("pattern has no key " + core::py_repr_str(key));
        }
    }
    if (!picture.empty()) {
        if (placement.contains("id")) picture["id"] = placement["id"];
        placement["id"] = add_pattern_to(doc, picture).first;
    }
    Json spec;
    try {
        spec = core::pattern_fill_spec(placement);
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
    if (core::find_pattern(doc, spec["id"].get<std::string>()) == nullptr) {
        throw OpError("the book has no pattern " + spec["id"].get<std::string>() + " (add_pattern puts one in)");
    }
    return spec;
}

int pattern_fill_dpi(const core::Page& page) {
    // (PC4 F-b, image-grid@1: a page of grid "finest" — an opened picture's — at its grid, design 2.2; any other at 300 dpi
    // as a colour's fill)
    if (page.spec.grid && *page.spec.grid == "finest") return raster::working_dpi(page);
    return fills::kFillDpi;
}

Json fill_with_pattern(core::OpContext& c, std::size_t at, std::size_t ti, const Image& coverage, Point offset, int dpi) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const Json spec = fill_pattern(doc, op["pattern"]);
    const core::Page& page = doc.page(at);
    const core::Layer& layer = page.layers[ti];
    if (layer.kind == core::LayerKind::Tone) throw OpError("a pattern fills a pen or paint layer (a tone layer holds a tone)");
    double opacity = 1.0;
    if (const Json* o = given_key(op, "opacity")) {
        if (!o->is_number() || o->is_boolean() || !std::isfinite(o->get<double>())) throw OpError("opacity must be a number (0..1)");
        opacity = std::clamp(o->get<double>(), 0.0, 1.0);
    }
    std::string mode = "normal";
    if (const Json* b = given_key(op, "blend")) {
        if (!b->is_string() || !image_ops::is_blend(b->get<std::string>())) throw OpError("blend is one of the blends (normal, multiply, screen, …)");
        mode = b->get<std::string>();
    }
    const bool keep = core::truthy_at(op, "preserve_transparency") || layer.lock_alpha;
    const auto drawn = coverage.getbbox();
    if (!drawn) throw OpError("the area is empty");
    if (mode != "normal" && layer.kind != core::LayerKind::Raster) {
        throw OpError("blend paints a paint layer's pixels (a pen layer's fills are pictures laid on it: leave blend out)");
    }
    const bool pen = layer.kind == core::LayerKind::Strokes;
    const bool finest = page.spec.grid && *page.spec.grid == "finest";
    // (a paint layer with no pixels yet that a precise RGB page's precise pixels can hold: keep_page_modes' rule — no
    // effect, screen or colour, nothing drawn on it; cut at the panels or not since PC5-0 —, E4's
    // paint_fill::becomes_precise since the merge)
    const bool to_precise = paint_fill::becomes_precise(page, layer);
    // (where a picture is laid, as a colour's fill lays one: a pen layer — whatever its lines and the page —, and a paint
    // layer of 8 bits on a page that is not of inks nor of the finest grid, or of a precise page it cannot be precise on)
    const bool picture = mode == "normal" && !keep &&
                         (pen || (!layer.color_raster && !layer.ink_raster && !core::has_color_strokes(layer) && !page.color && !finest && !to_precise));
    const Json report{{"pattern", spec["id"]}};
    if (picture) {
        // (a picture patch at 300 dpi — at the grid of a page of the finest grid —: the pattern over the page, by the coverage
        // and opacity)
        const int patch_dpi = pattern_fill_dpi(page);
        const Image placed_cover = [&] {
            if (dpi == patch_dpi) return coverage;
            const Size size{std::max(1, static_cast<int>(std::lround(coverage.width() * double(patch_dpi) / dpi))),
                            std::max(1, static_cast<int>(std::lround(coverage.height() * double(patch_dpi) / dpi)))};
            return coverage.resize(size, Resample::Bilinear);
        }();
        const Point corner{static_cast<int>(std::lround(offset.x * double(patch_dpi) / dpi)), static_cast<int>(std::lround(offset.y * double(patch_dpi) / dpi))};
        const auto box = placed_cover.getbbox();
        if (!box) throw OpError("the area is empty");
        const Box area{corner.x + box->x0, corner.y + box->y0, corner.x + box->x1, corner.y + box->y1};
        limits::check_picture(area.width(), area.height(), "the area");
        const std::optional<patterns::Laid> laid = patterns::laid(&doc, spec, patterns::Target::srgb(), patch_dpi);
        if (!laid) throw OpError("the book has no picture of the pattern " + spec["id"].get<std::string>());
        Image picture_made = patterns::rgba8(*laid, area);
        const std::string cut = placed_cover.crop(*box).tobytes();
        std::string alpha = picture_made.getchannel(3).tobytes();
        for (std::size_t i = 0; i < alpha.size() && i < cut.size(); ++i) {
            alpha[i] = static_cast<char>(std::lround(static_cast<unsigned char>(alpha[i]) * (static_cast<unsigned char>(cut[i]) / 255.0) * opacity));
        }
        picture_made.putalpha(Image::frombytes("L", picture_made.size(), alpha));
        core::Patch templ;
        templ.attrs["mode"] = "image";
        templ.attrs["opacity"] = 1.0;
        auto patch = selection::to_patch(picture_made, area.x0, area.y0, templ, patch_dpi);
        if (!patch) throw OpError("the area is empty");
        core::Page& edited = doc.edit_page(at);
        edited.layers[ti].patches.push_back(std::move(*patch));
        return report;
    }
    // (into the layer's pixels on its own grid — render/paint_fill's way since the merge, a colour's fill's and a gradient's:
    // a layer becomes_precise() names made precise first, the coverage placed over the page, laid on the grid and cut at
    // the panels when the layer is; an ink layer's lines kept as lines in the 通常 mode)
    const std::string id = layer.id;
    paint_fill::Coverage cover;
    cover.picture = coverage;
    cover.offset = offset;
    cover.dpi = dpi;
    paint_fill::How how;
    how.blend = mode;
    how.opacity = opacity;
    how.keep_transparency = core::truthy_at(op, "preserve_transparency");
    paint_fill::LaidValues values;
    const std::string pattern_id = spec["id"].get<std::string>();
    values.make = [&doc, &spec, &pattern_id, at](paint_fill::LaidValues::Kind kind, double grid_dpi) -> std::function<void(int, int, float*)> {
        const core::Page& now = doc.page(at);
        const patterns::Target target = kind == paint_fill::LaidValues::Kind::Inks   ? patterns::Target::inks(*now.color)
                                        : kind == paint_fill::LaidValues::Kind::Srgb ? patterns::Target::srgb()
                                                                                     : patterns::Target::rgb(now);
        std::optional<patterns::Laid> made = patterns::laid(&doc, spec, target, grid_dpi);
        if (!made) throw OpError("the book has no picture of the pattern " + pattern_id);
        auto held = std::make_shared<const patterns::Laid>(std::move(*made));
        return [held](int x, int y, float* v) { held->at(x, y, v); };
    };
    if (!paint_fill::paint_values(c, at, id, cover, values, how)) throw OpError("the area is empty");
    return report;
}

Json patterns_of_parts(const core::Document& book, const std::vector<core::vector::ShapePart>& parts) {
    std::set<std::string> ids;
    for (const core::vector::ShapePart& part : parts) {
        for (const Json* fill : {&part.fill, part.stroke ? &part.stroke->fill : nullptr}) {
            if (fill == nullptr) continue;
            if (const Json* p = core::pattern_of_fill(*fill); p != nullptr && p->is_object()) {
                if (const Json* id = core::get(*p, "id"); id != nullptr && id->is_string()) ids.insert(id->get<std::string>());
            }
        }
    }
    Json out = Json::array();
    for (const std::string& id : ids) {
        const core::Pattern* p = core::find_pattern(book, id);
        if (p == nullptr) continue;
        const auto file = book.pattern_files.find(p->asset);
        if (file == book.pattern_files.end() || !file->second) continue;
        Json one = pattern_library::add_op(*p, file->second);
        one.erase("op");
        one["id"] = id;
        out.push_back(std::move(one));
    }
    return out;
}

void bring_part_patterns(core::Document& doc, std::vector<core::vector::ShapePart>& parts, const Json& patterns) {
    if (!patterns.is_array() || patterns.empty() || parts.empty()) return;
    std::map<std::string, std::string> ids;
    for (const Json& given : patterns) {
        if (!given.is_object()) throw OpError("items' patterns is a list of patterns ({id, name?, png | gkcr | gkir, mode?, depth?, dpi?})");
        const Json* id = core::get(given, "id");
        if (id == nullptr || !id->is_string()) throw OpError("an item's pattern needs the id its parts name");
        Json picture = given;
        picture.erase("id");
        ids[id->get<std::string>()] = add_pattern_to(doc, picture).first;  // (the book's own of the same picture, else a new one)
    }
    core::Page carrier;  // (the parts' fills pointed at the book's patterns, as a page's are)
    core::Layer holder;
    holder.kind = core::LayerKind::Strokes;
    holder.shapes = core::vector::make_shapes(std::move(parts));
    carrier.layers.push_back(std::move(holder));
    (void)core::repoint_patterns(carrier, ids);
    parts = carrier.layers.front().shapes->items;
}

void register_pattern_ops(core::OpRegistry& registry) {
    // {op: add_pattern, name?, png | gkcr | gkir (base64) | asset, mode?, depth?, dpi?, id?} → {"pattern": id, "added"};
    // {op: rename_pattern, id, name}; {op: delete_pattern, id} (refused while a fill uses it)
    registry.add("add_pattern", add_pattern);
    registry.add("rename_pattern", rename_pattern);
    registry.add("delete_pattern", delete_pattern);
}

}  // namespace genko::render
