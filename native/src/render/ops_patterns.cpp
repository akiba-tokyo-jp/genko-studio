// PC4 節 E (SPEC.md COLOR-09, patterns@1): the ops of the book's patterns (E3) — a pattern the book keeps
// (core::Pattern, Document::patterns and pattern_files) added from its picture, renamed, and deleted when nothing uses it.
// PhotoCraft v0.2.0 crates/engine/src/pattern_cmds.rs (ensure_in_doc, rename, delete), MIT:
// native/third_party/photocraft/NOTICE.md; the book keeps its own copy, the library is the machine's
// (render/pattern_library).

#include "render/ops_patterns.hpp"

#include <algorithm>
#include <cmath>

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
#include "render/page.hpp"
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
    if (!bytes) throw OpError("the pattern has no picture");
    if (const auto why = core::check_pattern_file(p, *bytes)) throw OpError("the pattern's picture: " + *why);
    if (const core::Pattern* kept = same(doc, p)) return {kept->id, false};
    if (p.id.empty() || core::find_pattern(doc, p.id) != nullptr) p.id = core::next_pattern_id(doc);
    try {
        (void)core::pattern_from_json(core::pattern_to_json(p));
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
    if (doc.patterns.size() >= core::kMaxPatterns) throw OpError("a book keeps at most " + std::to_string(core::kMaxPatterns) + " patterns");
    doc.pattern_files[p.asset] = bytes;
    doc.patterns.push_back(p);
    return {p.id, true};
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
    // (a picture on a pen layer, or on a paint layer of an 8-bit page; a paint layer of a precise page or of inks — one
    // without pixels yet too — gets them)
    const bool deep_page = page.color.has_value() || (page.rgb && page.rgb->depth > 8);
    const bool eight = !layer.color_raster && !layer.ink_raster && !core::has_color_strokes(layer) &&
                       (layer.kind == core::LayerKind::Strokes || !deep_page);
    if (mode != "normal" && layer.kind != core::LayerKind::Raster) {
        throw OpError("blend paints a paint layer's pixels (a pen layer's fills are pictures laid on it: leave blend out)");
    }
    const Json report{{"pattern", spec["id"]}};
    if (eight && mode == "normal" && !keep) {
        // (a picture patch at 300 dpi, as fill_area lays a colour: the pattern over the page, by the coverage and opacity)
        const int patch_dpi = fills::kFillDpi;
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
        Image picture = patterns::rgba8(*laid, area);
        const std::string cut = placed_cover.crop(*box).tobytes();
        std::string alpha = picture.getchannel(3).tobytes();
        for (std::size_t i = 0; i < alpha.size() && i < cut.size(); ++i) {
            alpha[i] = static_cast<char>(std::lround(static_cast<unsigned char>(alpha[i]) * (static_cast<unsigned char>(cut[i]) / 255.0) * opacity));
        }
        picture.putalpha(Image::frombytes("L", picture.size(), alpha));
        core::Patch templ;
        templ.attrs["mode"] = "image";
        templ.attrs["opacity"] = 1.0;
        auto patch = selection::to_patch(picture, area.x0, area.y0, templ, patch_dpi);
        if (!patch) throw OpError("the area is empty");
        core::Page& edited = doc.edit_page(at);
        edited.layers[ti].patches.push_back(std::move(*patch));
        return report;
    }
    // (into the layer's pixels on its own grid: the coverage placed over the page and laid on the grid)
    const std::string id = layer.id;
    {  // (a paint layer of a precise RGB page with no pixels yet: clear precise pixels of the page's precision first, as
       // fill_area's precise pieces start one)
        const core::Page& was = doc.page(at);
        const core::Layer& bare = was.layers[ti];
        if (!was.color && was.rgb && was.rgb->depth > 8 && bare.kind == core::LayerKind::Raster && !bare.color_raster && !bare.ink_raster &&
            (!bare.raster_png || bare.raster_png->empty()) && bare.stroke_count() == 0 && bare.patches.empty() && !shape_parts::any(bare)) {
            const Size size{mm_to_px(was.spec.width_mm.value(), raster::kWorkingDpi), mm_to_px(was.spec.height_mm.value(), raster::kWorkingDpi)};
            ColorCanvas clear(Image::create("RGBA", size, Ink{0, 0, 0, 0}), RgbSpace::of(was));
            std::string pixels = clear.color_raster(was.rgb->depth == 16 ? "u16" : "f32");
            doc.edit_page(at).layers[ti].color_raster = std::make_shared<const std::string>(std::move(pixels));
            if (std::find(doc.features.begin(), doc.features.end(), core::kColorRasterFeature) == doc.features.end()) {
                doc.features.emplace_back(core::kColorRasterFeature);
            }
        }
    }
    image_ops::Window found;
    (void)image_ops::layer_pixels(c, at, id, Box{0, 0, 0, 0}, &found);
    const core::Page& now = doc.page(at);
    const Size on_page{mm_to_px(now.spec.width_mm.value(), dpi), mm_to_px(now.spec.height_mm.value(), dpi)};
    Image placed = Image::create("L", on_page, Ink(0));
    placed.paste(coverage, offset);
    const Image on_grid = placed.size() == found.grid ? placed
                                                      : placed.resize(found.grid, found.grid.width < on_page.width ? Resample::Box : Resample::Bilinear);
    const auto box = on_grid.getbbox();
    if (!box) throw OpError("the area is empty");
    image_ops::Window window;
    image_ops::Pixels p = image_ops::layer_pixels(c, at, id, *box, &window);
    const std::string share = on_grid.crop(window.box).tobytes();
    const double grid_dpi = found.grid.width / (now.spec.width_mm.value() / 25.4);
    const bool inks = p.kind() == image_ops::Pixels::Kind::Inks;
    const bool direct = !inks || mode == "normal";  // (inks in a blend: through the colours they mean)
    const patterns::Target target = inks ? (direct ? patterns::Target::inks(*now.color) : patterns::Target::srgb()) : patterns::Target::rgb(now);
    const std::optional<patterns::Laid> laid = patterns::laid(&doc, spec, target, grid_dpi);
    if (!laid) throw OpError("the book has no picture of the pattern " + spec["id"].get<std::string>());
    const unsigned n = p.colours();
    const std::uint32_t w = static_cast<std::uint32_t>(window.box.width());
    float v[core::kPlatesMax + 1];
    std::vector<image_ops::Rgba> rgba;
    if (!direct) rgba = image_ops::rgba_of(p);
    for (std::size_t i = 0; i < p.size(); ++i) {
        const float k = static_cast<float>(static_cast<unsigned char>(share[i]) / 255.0 * opacity);
        if (!(k > 0)) continue;
        laid->at(window.box.x0 + static_cast<int>(i % w), window.box.y0 + static_cast<int>(i / w), v);
        if (!direct) {  // (a blend on inks: the colours they mean, back by the page's rules)
            image_ops::Rgba out = image_ops::composite(mode, rgba[i], image_ops::Rgba{v[0], v[1], v[2], v[3] * k}, 1.0f);
            if (keep) out[3] = rgba[i][3];
            rgba[i] = out;
            continue;
        }
        float* px = p.at(i);
        if (inks) {  // (normal, in the page's own values: the pattern's inks laid over the layer's)
            const float sa = v[n] * k, da = px[n];
            const float oa = sa + da * (1 - sa);
            if (keep) {
                if (!(da > 0)) continue;
                for (unsigned ch = 0; ch < n; ++ch) px[ch] = (v[ch] * sa + px[ch] * da * (1 - sa)) / oa;
                continue;
            }
            if (oa > 0)
                for (unsigned ch = 0; ch < n; ++ch) px[ch] = (v[ch] * sa + px[ch] * da * (1 - sa)) / oa;
            px[n] = oa;
            continue;
        }
        image_ops::Rgba out = image_ops::composite(mode, image_ops::Rgba{px[0], px[1], px[2], px[3]}, image_ops::Rgba{v[0], v[1], v[2], v[3] * k}, 1.0f);
        if (keep) out[3] = px[3];
        for (unsigned ch = 0; ch < 4; ++ch) px[ch] = out[ch];
    }
    if (!direct) image_ops::from_rgba(p, rgba);
    image_ops::put_layer_pixels(doc, at, id, p, window.box);
    return report;
}

void register_pattern_ops(core::OpRegistry& registry) {
    // {op: add_pattern, name?, png | gkcr | gkir (base64) | asset, mode?, depth?, dpi?, id?} → {"pattern": id, "added"};
    // {op: rename_pattern, id, name}; {op: delete_pattern, id} (refused while a fill uses it)
    registry.add("add_pattern", add_pattern);
    registry.add("rename_pattern", rename_pattern);
    registry.add("delete_pattern", delete_pattern);
}

}  // namespace genko::render
