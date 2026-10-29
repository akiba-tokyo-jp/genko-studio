// PC5 I-d (variables@1): set_variables, set_data_sets, apply_data_set (render/ops_variables.hpp) — PhotoCraft v0.2.0's
// crates/engine/src/variables_cmds.rs (155-262: applying a data set, placing a picture by its method and align), MIT
// (native/third_party/photocraft/NOTICE.md).

#include "render/ops_variables.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/type_text.hpp"
#include "core/variables.hpp"
#include "render/image_ops.hpp"
#include "render/layer_bounds.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_registry.hpp"
#include "render/ops_smart_parts.hpp"
#include "render/resample.hpp"
#include "render/smart_decode.hpp"
#include "storage/asset_store.hpp"

namespace genko::render {

namespace {

namespace fs = std::filesystem;
using core::Document;
using core::Json;
using core::Layer;
using core::LayerKind;
using core::OpContext;
using core::OpError;
using core::Page;

// The position of the page whose id this is (none: gone).
std::optional<std::size_t> page_by_id(const Document& doc, const std::optional<std::string>& id) {
    if (!id) return std::nullopt;
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (doc.pages[i]->id == *id) return i;
    }
    return std::nullopt;
}

const Layer* layer_of(const Page& page, const std::optional<std::string>& id) {
    if (!id) return nullptr;
    for (const Layer& layer : page.layers) {
        if (layer.id == *id) return &layer;
    }
    return nullptr;
}

const core::StoryLine* line_of(const Document& doc, const std::optional<std::string>& id) {
    if (!id) return nullptr;
    for (const core::StoryLine& line : doc.story) {
        if (line.id == *id) return &line;
    }
    return nullptr;
}

// Does a def of `type` take that layer (visibility: any; text: a type layer; pixels: a paint layer or a smart object)?
bool takes(const std::string& type, const Layer& layer) {
    if (type == "visibility") return true;
    if (type == "text") return core::type_text::is_type_layer(layer);
    return layer.kind == LayerKind::Raster || layer.kind == LayerKind::Smart;
}

// Where the def's target is now: its page and layer, or its line (none: gone, or no longer of its kind — `why` says).
struct Found {
    std::optional<std::size_t> at;
    const Layer* layer = nullptr;
    const core::StoryLine* line = nullptr;
};
std::optional<Found> find_target(const Document& doc, const core::VariableDef& def, std::string* why = nullptr) {
    Found f;
    if (def.line) {
        f.line = line_of(doc, def.line);
        if (f.line == nullptr) {
            if (why != nullptr) *why = "missing";
            return std::nullopt;
        }
        return f;
    }
    f.at = page_by_id(doc, def.page);
    f.layer = f.at ? layer_of(doc.page(*f.at), def.layer) : nullptr;
    if (f.layer == nullptr) {
        if (why != nullptr) *why = "missing";
        return std::nullopt;
    }
    if (!takes(def.type, *f.layer)) {
        if (why != nullptr) *why = "kind";
        return std::nullopt;
    }
    return f;
}

std::string text_at(const Json& data, const char* key, const std::string& what) {
    const Json* v = core::get(data, key);
    if (v == nullptr || !v->is_string()) throw OpError(what + ": " + key + " must be text");
    return v->get<std::string>();
}

// The bytes of a picture the book keeps (held in memory, or in its folder) and its suffix.
std::pair<std::string, std::string> picture_bytes(const Document& doc, const std::string& ref) {
    if (const auto held = doc.smart_files.find(ref); held != doc.smart_files.end() && held->second) {
        return {*held->second, std::string()};
    }
    return smart_ops::asset_file(doc, ref);
}

// What a picture takes of the book's 4 GiB: its bytes held, or its file's size in the book's folder.
std::uint64_t picture_size(const Document& doc, const core::PictureValue& picture) {
    if (const auto held = doc.smart_files.find(picture.asset); held != doc.smart_files.end() && held->second) return held->second->size();
    if (!doc.asset_dir) return 0;
    std::error_code ec;
    const auto size = fs::file_size(storage::AssetStore(*doc.asset_dir).path(picture.asset, picture.suffix), ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

// --- set_variables ---------------------------------------------------------------------------------------------------

core::VariableDef def_given(const Document& doc, const Json& item, std::size_t i, const core::Variables& before) {
    const std::string what = "defs[" + std::to_string(i) + "]";
    if (!item.is_object()) throw OpError(what + " must be an object");
    for (const auto& [key, value] : item.items()) {
        (void)value;
        static const char* const kKeys[] = {"name", "type", "page", "layer", "line", "method", "align", "clip", "box_mm"};
        if (std::none_of(std::begin(kKeys), std::end(kKeys), [&](const char* k) { return key == k; })) throw OpError(what + " has an unknown key: " + key);
    }
    core::VariableDef def;
    def.name = text_at(item, "name", what);
    def.type = text_at(item, "type", what);
    if (def.type != "visibility" && def.type != "text" && def.type != "pixels") throw OpError(what + ": type must be visibility, text or pixels");
    if (core::has(item, "line")) {
        if (def.type != "text") throw OpError(what + ": a line takes a text variable only");
        if (core::has(item, "layer") || core::has(item, "page")) throw OpError(what + ": line, or page and layer, not both");
        const core::StoryLine* line = line_of(doc, text_at(item, "line", what));
        if (line == nullptr) throw OpError(what + ": no line " + core::py_str(item["line"]));
        def.line = line->id;
    } else {
        if (!core::has(item, "layer")) throw OpError(what + ": page and layer (or, for text, line) name what it changes");
        const std::size_t at = core::require_page(doc, item);
        const Page& page = doc.page(at);
        const Layer& layer = page.layers[core::layer_by_id(page, text_at(item, "layer", what))];
        if (!takes(def.type, layer)) {
            throw OpError(what + ": layer " + layer.id +
                          (def.type == "text" ? " is not a type layer" : " is not a paint layer or a smart object"));
        }
        def.page = page.id;
        def.layer = layer.id;
    }
    const bool pixels = def.type == "pixels";
    for (const char* key : {"method", "align", "clip", "box_mm"}) {
        if (!pixels && core::has(item, key)) throw OpError(what + ": " + key + " is a pixels variable's");
    }
    if (pixels) {
        if (core::has(item, "method")) def.method = text_at(item, "method", what);
        if (core::has(item, "align")) def.align = text_at(item, "align", what);
        if (core::has(item, "clip")) {
            if (!item["clip"].is_boolean()) throw OpError(what + ": clip must be true or false");
            def.clip = item["clip"].get<bool>();
        }
        const auto old = std::find_if(before.defs.begin(), before.defs.end(), [&](const core::VariableDef& d) {
            return d.name == def.name && d.type == "pixels" && d.page == def.page && d.layer == def.layer;
        });
        if (core::has(item, "box_mm")) {
            const std::vector<double> box = core::unpack_floats(item["box_mm"], 4);
            std::copy(box.begin(), box.end(), def.box_mm.begin());
        } else if (old != before.defs.end()) {
            def.box_mm = old->box_mm;  // (its box kept: a picture put in it earlier does not move it)
        } else {
            const Page& page = doc.page(*page_by_id(doc, def.page));
            def.box_mm = {0, 0, page.spec.width_mm.value(), page.spec.height_mm.value()};
            if (const auto held = layer_bounds::of_layer(doc, page, *def.layer); held && (*held)[2] > (*held)[0] && (*held)[3] > (*held)[1]) def.box_mm = *held;
        }
    }
    return def;
}

// The variables made the book's (OpError for what core::variables_from_json refuses).
void keep(Document& doc, const core::Variables& variables) {
    try {
        (void)core::variables_from_json(core::variables_to_json(variables));
    } catch (const core::Error& error) {
        std::string what = error.what();
        if (what.starts_with("variables: ")) what = what.substr(11);
        throw OpError(what);
    }
    core::set_variables(doc, variables);
}

void set_variables_op(OpContext& c) {
    const Json* defs = core::get(c.op, "defs");
    if (defs == nullptr || !defs->is_array()) throw OpError("defs is a list of {name, type, page, layer | line, …}");
    if (defs->size() > core::kVariableDefsMax) throw OpError("at most " + std::to_string(core::kVariableDefsMax) + " variables");
    core::Variables variables = core::variables_of(c.doc);
    std::vector<core::VariableDef> made;
    for (std::size_t i = 0; i < defs->size(); ++i) made.push_back(def_given(c.doc, (*defs)[i], i, variables));
    variables.defs = std::move(made);
    keep(c.doc, variables);
    c.report = variables_listing(c.doc);
}

// --- set_data_sets ---------------------------------------------------------------------------------------------------

core::PictureValue picture_given(Document& doc, const Json& given, const std::string& what) {
    core::PictureValue picture;
    std::string bytes;
    std::string suffix;
    if (core::has(given, "data")) {
        try {
            bytes = core::a2b_base64(text_at(given, "data", what));
        } catch (const core::Error&) {
            throw OpError(what + ": data is the picture's bytes in base64");
        }
        if (bytes.empty()) throw OpError(what + ": data holds no picture");
        if (bytes.size() > smart::kFileMaxBytes) throw OpError(what + ": the picture is larger than 256 MiB");
    } else if (core::has(given, "asset")) {
        const std::string ref = text_at(given, "asset", what);
        if (!ref.starts_with("sha256:")) throw OpError(what + ": the picture is an asset ref (sha256:…) or data, not a path");
        std::tie(bytes, suffix) = picture_bytes(doc, ref);
    } else {
        throw OpError(what + ": a picture is {data: base64, name?} or {asset: sha256:…, name?}");
    }
    smart::FileInfo info;
    try {
        info = smart::look_at(bytes, suffix);
    } catch (const NotYetPorted&) {
        throw;
    } catch (const core::Error& error) {
        throw OpError(what + ": the picture cannot be read (" + error.what() + ")");
    }
    picture.asset = core::bytes_ref(bytes);
    picture.suffix = info.suffix;
    if (core::has(given, "name")) picture.name = text_at(given, "name", what);
    if (picture.name.size() > 1024) throw OpError(what + ": the picture's name is too long");
    if (!doc.smart_files.contains(picture.asset) && !(doc.asset_dir && storage::AssetStore(*doc.asset_dir).has(picture.asset, picture.suffix))) {
        doc.smart_files[picture.asset] = std::make_shared<const std::string>(std::move(bytes));
    }
    return picture;
}

void set_data_sets_op(OpContext& c) {
    const Json* sets = core::get(c.op, "data_sets");
    if (sets == nullptr || !sets->is_array()) throw OpError("data_sets is a list of {name, values: [{variable, value}]}");
    const Json* append = core::get(c.op, "append");
    if (append != nullptr && !append->is_null() && !append->is_boolean()) throw OpError("append is true or false");
    const bool adding = append != nullptr && append->is_boolean() && append->get<bool>();
    core::Variables variables = core::variables_of(c.doc);
    if (variables.defs.empty() && !sets->empty()) throw OpError("the book has no variables: define them first (set_variables)");
    std::vector<core::DataSet> made;
    for (std::size_t i = 0; i < sets->size(); ++i) {
        const Json& item = (*sets)[i];
        const std::string what = "data_sets[" + std::to_string(i) + "]";
        if (!item.is_object()) throw OpError(what + " must be an object");
        for (const auto& [key, value] : item.items()) {
            (void)value;
            if (key != "name" && key != "values") throw OpError(what + " has an unknown key: " + key);
        }
        core::DataSet set;
        set.name = text_at(item, "name", what);
        const Json* values = core::get(item, "values");
        if (values == nullptr || !values->is_array()) throw OpError(what + ": values is a list of {variable, value}");
        for (std::size_t j = 0; j < values->size(); ++j) {
            const Json& v = (*values)[j];
            const std::string where = what + ".values[" + std::to_string(j) + "]";
            if (!v.is_object() || !v.contains("variable") || !v.contains("value")) throw OpError(where + " is {variable, value}");
            core::DataValue value;
            value.variable = text_at(v, "variable", where);
            const core::VariableDef* def = core::find_def(variables, value.variable);
            if (def == nullptr) throw OpError(where + ": no variable " + value.variable);
            const Json& given = v["value"];
            if (def->type == "visibility") {
                if (!given.is_boolean()) throw OpError(where + ": " + value.variable + " (visibility) takes true or false");
                value.value = given.get<bool>();
            } else if (def->type == "text") {
                if (!given.is_string()) throw OpError(where + ": " + value.variable + " (text) takes text");
                value.value = given.get<std::string>();
            } else {
                if (!given.is_object()) throw OpError(where + ": " + value.variable + " (pixels) takes a picture {data, name?} or {asset, name?}");
                value.value = picture_given(c.doc, given, where);
            }
            set.values.push_back(std::move(value));
        }
        made.push_back(std::move(set));
    }
    if (adding) {
        for (core::DataSet& set : made) variables.data_sets.push_back(std::move(set));
    } else {
        variables.data_sets = std::move(made);
        variables.active.reset();
    }
    // (the pictures together: the book's limit)
    std::uint64_t total = 0;
    std::set<std::string> seen;
    for (const core::DataSet& set : variables.data_sets) {
        for (const core::DataValue& value : set.values) {
            const auto* picture = std::get_if<core::PictureValue>(&value.value);
            if (picture != nullptr && seen.insert(picture->asset).second) total += picture_size(c.doc, *picture);
        }
    }
    if (total > core::kVariablePicturesMax) throw OpError("the data sets' pictures are larger than 4 GiB together");
    keep(c.doc, variables);
    c.report = variables_listing(c.doc);
}

// --- apply_data_set --------------------------------------------------------------------------------------------------

// Another op on the book as the bus makes it — its checks (page locks, strict_gates, the layer's lock), its rules —, the
// book taken from it; its error said of the variable.
void run_op(OpContext& c, const Json& op, const std::string& variable) {
    static const core::CommandBus bus(ops_registry());
    try {
        core::ApplyResult result = bus.apply(c.doc, Json::array({op}), c.actor);
        // (what the op said — "ops[0] <op>: …" among the book's warnings — said of the variable; the book's own warnings are the
        // outer batch's, made again at its end)
        for (const Json& warning : result.warnings) {
            if (!warning.is_string()) continue;
            const std::string& text = warning.get_ref<const std::string&>();
            if (text.starts_with("ops[0] ")) c.notes.push_back("variable " + variable + ": " + text.substr(7));
        }
        const std::int64_t revision = c.doc.revision;
        c.doc = std::move(result.doc);
        c.doc.revision = revision;
    } catch (const core::ApplyError& error) {
        std::string what = error.what();
        if (what.starts_with("ops[0] ")) what = what.substr(7);
        throw OpError("variable " + variable + ": " + what);
    }
}

// A picture as a paint layer of the page's kind takes it (the page's numbers kept: an 8-bit page's RGBA, a precise page's
// floats, an ink page's inks), straight, its colours then its alpha; read one pixel at a time (`get`, its place kept within the
// picture: past an edge, the edge's pixel) so that only the part of it that is placed is ever held as floats.
struct Picture {
    int width = 0, height = 0;
    unsigned channels = 4;
    bool hdr = false;
    std::optional<Image> eight;                // (an 8-bit picture: its RGBA bytes)
    std::optional<image_ops::Pixels> pixels;   // (an ink page's: its inks)
    std::vector<float> values;                 // (a precise page's, or one made smaller: width × height × channels)

    void get(int x, int y, float* out) const {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        const std::size_t i = std::size_t(y) * std::size_t(width) + std::size_t(x);
        if (eight) {
            const auto* p = reinterpret_cast<const unsigned char*>(bytes.data()) + i * 4;
            for (int k = 0; k < 4; ++k) out[k] = p[k] / 255.0f;
        } else if (pixels) {
            std::copy_n(pixels->at(i), channels, out);
        } else {
            std::copy_n(values.data() + i * channels, channels, out);
        }
    }
    // (made smaller by whole factors: each pixel the mean of its block, its colours weighted by their alpha)
    void reduce(int fx, int fy) {
        if (fx <= 1 && fy <= 1) return;
        if (eight) {
            eight = eight->reduce(fx, fy);
            width = eight->width();
            height = eight->height();
            bytes = eight->tobytes();
            return;
        }
        const int w = std::max(1, (width + fx - 1) / fx), h = std::max(1, (height + fy - 1) / fy);
        std::vector<float> out(std::size_t(w) * std::size_t(h) * channels, 0.0f);
        std::vector<float> v(channels);
        std::vector<double> sum(channels);
        const unsigned a = channels - 1;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                std::fill(sum.begin(), sum.end(), 0.0);
                int n = 0;
                for (int yy = y * fy; yy < std::min(height, (y + 1) * fy); ++yy) {
                    for (int xx = x * fx; xx < std::min(width, (x + 1) * fx); ++xx) {
                        get(xx, yy, v.data());
                        for (unsigned k = 0; k < a; ++k) sum[k] += double(v[k]) * v[a];
                        sum[a] += v[a];
                        ++n;
                    }
                }
                float* o = out.data() + (std::size_t(y) * std::size_t(w) + std::size_t(x)) * channels;
                o[a] = n > 0 ? float(sum[a] / n) : 0.0f;
                for (unsigned k = 0; k < a; ++k) o[k] = sum[a] > 0 ? float(sum[k] / sum[a]) : 0.0f;
            }
        }
        pixels.reset();
        values = std::move(out);
        width = w;
        height = h;
    }
    std::string bytes;  // (eight's bytes)
};

// The most pixels a picture of more than 8 bits (on a precise page) or an ink page's picture is taken in at (its floats held
// whole while it is made smaller: some 16-20 bytes a pixel — 32 million pixels, at most about 1.3 GB at once).
constexpr std::int64_t kDeepPictureMost = 32'000'000;

Picture picture_of(const std::string& bytes, const Page& page, const image_ops::Pixels& like) {
    Picture p;
    const auto too_large = [](std::int64_t w, std::int64_t h) {
        if (w * h > kDeepPictureMost) {
            throw OpError("the picture is too large to put in at the page's depth (at most " + std::to_string(kDeepPictureMost / 1'000'000) +
                          " million pixels)");
        }
    };
    const auto take_eight = [&] {
        p.eight = smart::eight_bit_picture(bytes).convert("RGBA");
        p.width = p.eight->width();
        p.height = p.eight->height();
        p.bytes = p.eight->tobytes();
        p.channels = 4;
    };
    switch (like.kind()) {
        case image_ops::Pixels::Kind::Eight:
            take_eight();
            return p;
        case image_ops::Pixels::Kind::Inks: {
            const core::InkPixels inks = smart::file_inks(bytes, *page.color, "keep");
            too_large(inks.width, inks.height);
            p.pixels = image_ops::from_inks(inks, *page.color);
            p.width = static_cast<int>(p.pixels->width());
            p.height = static_cast<int>(p.pixels->height());
            p.channels = p.pixels->all();
            return p;
        }
        case image_ops::Pixels::Kind::Precise: {
            p.hdr = like.floating;
            if (smart::stored_depth(bytes) <= 8) {  // (8 bits a sample: its values as the 8-bit page reads them, 4 bytes a pixel)
                take_eight();
                return p;
            }
            const smart::DeepPicture deep = smart::deep_picture(bytes);
            too_large(deep.width, deep.height);
            p.values = smart::srgb_floats(deep, std::nullopt, "relative", true);
            for (std::size_t i = 0; i < p.values.size(); ++i) {
                const float x = p.values[i];
                p.values[i] = std::isfinite(x) ? (like.floating && i % 4 < 3 ? std::max(x, 0.0f) : std::clamp(x, 0.0f, 1.0f)) : 0.0f;
            }
            p.width = static_cast<int>(deep.width);
            p.height = static_cast<int>(deep.height);
            p.channels = 4;
            return p;
        }
    }
    throw OpError("the layer's pixels are of no kind this build knows");
}

// PhotoCraft's replace_pixels: the layer's pixels replaced by the picture, sized into the def's box by its method (fit: within
// it, its proportions kept; fill: over it; conform: its size; as_is: a pixel of the picture a pixel of the page) and placed by
// its align, cut to the box with clip — on the layer's own grid, of its kind.
void replace_pixels(OpContext& c, std::size_t at, const core::VariableDef& def, const std::string& bytes) {
    const Page& page = c.doc.page(at);
    const Layer& layer = page.layers[core::layer_by_id(page, *def.layer)];
    // (strict_gates: a printed layer's pixels change once the name is approved, as drawing on it does)
    if (c.doc.strict_gates && layer.role != core::LayerRole::Name && layer.role != core::LayerRole::Draft && layer.exportable && !page.name_ok) {
        throw OpError("variable " + def.name + ": a printed layer's pixels change only after name_ok on page " + page.index.repr() + " (strict_gates)");
    }
    image_ops::Pixels target = image_ops::layer_pixels(c, at, *def.layer);
    // (a paint layer of 8 bits on a precise page — an empty one —: the picture kept at the page's depth, as a new layer is made
    // there; its old pixels are replaced whole — unless the book's precise pixels would pass their budget: then at 8 bits, as a
    // new layer is, Z0)
    bool deepen = target.kind() == image_ops::Pixels::Kind::Eight && !page.color && page.rgb && page.rgb->depth > 8;
    if (deepen) {
        std::set<const std::string*> seen;
        std::uint64_t held = 0;
        for (const core::PagePtr& each : c.doc.pages) {
            for (const Layer& l : each->layers) {
                if (l.color_raster && seen.insert(l.color_raster.get()).second) held += l.color_raster->size();
            }
        }
        const std::uint64_t made = std::uint64_t(target.width()) * target.height() * 4 * (page.rgb->depth == 32 ? 4 : 2);
        if (held + made > core::kColorRasterBookBytes) deepen = false;
    }
    if (deepen) {
        image_ops::Pixels deep(image_ops::Pixels::Kind::Precise, target.width(), target.height(), 3);
        deep.floating = page.rgb->depth == 32;
        target = std::move(deep);
    }
    Picture source = picture_of(bytes, page, target);
    const double page_w = page.spec.width_mm.value();
    const double page_h = page.spec.height_mm.value();
    const double kx = target.width() / page_w;  // (the grid's pixels a mm)
    const double ky = target.height() / page_h;
    const double dpi = page.spec.dpi.value() > 0 ? page.spec.dpi.value() : 300.0;
    const double iw = source.width * 25.4 / dpi;  // (the picture's mm at the page's resolution)
    const double ih = source.height * 25.4 / dpi;
    const auto [x0, y0, x1, y1] = def.box_mm;
    const double tw = x1 - x0;
    const double th = y1 - y0;
    double sx = 1.0, sy = 1.0;
    if (def.method == "fit") sx = sy = std::min(tw / iw, th / ih);
    else if (def.method == "fill") sx = sy = std::max(tw / iw, th / ih);
    else if (def.method == "conform") sx = tw / iw, sy = th / ih;
    const double sw = iw * sx;
    const double sh = ih * sy;
    const std::string& a = def.align;
    const double fx = a.ends_with("left") ? 0.0 : a.ends_with("right") ? 1.0 : 0.5;
    const double fy = a.starts_with("top") ? 0.0 : a.starts_with("bottom") ? 1.0 : 0.5;
    const double px = x0 + (tw - sw) * fx;  // (the picture's top left, mm)
    const double py = y0 + (th - sh) * fy;
    // (a picture put in at less than half its size made smaller first, by whole factors, to about twice what is placed: the
    // resampling then reads no more of it than it needs)
    const double placed_w = sw * kx, placed_h = sh * ky;  // (its size on the grid)
    source.reduce(std::max(1, static_cast<int>(std::floor(source.width / std::max(1.0, 2.0 * placed_w)))),
                  std::max(1, static_cast<int>(std::floor(source.height / std::max(1.0, 2.0 * placed_h)))));
    const double ax = placed_w / source.width;  // (the grid's pixels a picture's pixel)
    const double ay = placed_h / source.height;
    // the window of the grid it covers (the box with clip): the pixels whose centres it holds
    double wx0 = px * kx, wy0 = py * ky, wx1 = (px + sw) * kx, wy1 = (py + sh) * ky;
    if (def.clip) {
        wx0 = std::max(wx0, x0 * kx), wy0 = std::max(wy0, y0 * ky), wx1 = std::min(wx1, x1 * kx), wy1 = std::min(wy1, y1 * ky);
    }
    const auto edge = [](double v, std::uint32_t most) { return static_cast<int>(std::clamp<double>(std::round(v), 0.0, static_cast<double>(most))); };
    const int gx0 = edge(wx0, target.width());
    const int gy0 = edge(wy0, target.height());
    const int gx1 = edge(wx1, target.width());
    const int gy1 = edge(wy1, target.height());
    image_ops::Pixels out(target.kind(), target.width(), target.height(), target.colours());
    out.floating = target.floating;
    out.ink = target.ink;
    if (gx1 > gx0 && gy1 > gy0) {
        // the part of the picture the window shows, and kPad of its pixels round it — its own edge carried on past it, so
        // the picture keeps a clean edge where it ends (no clear rim drawn into it) — as floats: the only plane of it held
        constexpr int kPad = 4;
        const int bx0 = static_cast<int>(std::floor((gx0 - px * kx) / ax)) - kPad;
        const int by0 = static_cast<int>(std::floor((gy0 - py * ky) / ay)) - kPad;
        const int bx1 = static_cast<int>(std::ceil((gx1 - px * kx) / ax)) + kPad;
        const int by1 = static_cast<int>(std::ceil((gy1 - py * ky) / ay)) + kPad;
        const std::int64_t part = std::int64_t(bx1 - bx0) * std::int64_t(by1 - by0);
        if (part > 4 * std::int64_t(target.width()) * std::int64_t(target.height()) + 1'000'000) {
            throw OpError("variable " + def.name + ": the picture is too large to put in");  // (not reached: it is made smaller above)
        }
        resample::Plane region(bx1 - bx0, by1 - by0, static_cast<int>(source.channels), true);
        region.hdr = source.hdr;
        for (int y = 0; y < region.height; ++y)
            for (int x = 0; x < region.width; ++x) source.get(bx0 + x, by0 + y, region.at(x, y));
        const resample::Homography h = resample::Homography::affine({ax, 0, 0, ay, px * kx + bx0 * ax, py * ky + by0 * ay});
        const resample::Plane placed = resample::project(region, h, gx1 - gx0, gy1 - gy0, resample::Interp::Bicubic, gx0, gy0);
        const std::size_t n = out.all();
        for (int y = 0; y < placed.height; ++y) {
            for (int x = 0; x < placed.width; ++x) {
                std::copy_n(placed.at(x, y), n, out.at(std::size_t(gy0 + y) * target.width() + std::size_t(gx0 + x)));
            }
        }
    }
    image_ops::put_layer_pixels(c.doc, at, *def.layer, out);
    if (deepen) {
        core::Page& edited = c.doc.edit_page(at);
        core::Layer& made = edited.layers[core::layer_by_id(edited, *def.layer)];
        made.raster_png = nullptr;  // (its pixels the precise ones now)
        made.raster_memo = {};
        made.raster_relpath.reset();
        if (std::find(c.doc.features.begin(), c.doc.features.end(), core::kColorRasterFeature) == c.doc.features.end()) {
            c.doc.features.emplace_back(core::kColorRasterFeature);
        }
    }
}

void apply_data_set_op(OpContext& c) {
    core::Variables variables = core::variables_of(c.doc);
    std::size_t index = 0;
    if (core::has(c.op, "name")) {
        const std::string name = text_at(c.op, "name", "apply_data_set");
        const auto found = core::find_data_set(variables, name);
        if (!found) throw OpError("no data set " + name);
        index = *found;
    } else if (core::has(c.op, "index")) {
        const Json& given = c.op["index"];
        if (!given.is_number_integer() || given.get<std::int64_t>() < 0 || static_cast<std::uint64_t>(given.get<std::int64_t>()) >= variables.data_sets.size()) {
            throw OpError("no data set at index " + core::py_str(given));
        }
        index = static_cast<std::size_t>(given.get<std::int64_t>());
    } else {
        throw OpError("name or index names the data set");
    }
    const core::DataSet set = variables.data_sets[index];
    Json skipped = Json::array();
    const auto skip = [&](const std::string& variable, const char* reason) { skipped.push_back(Json{{"variable", variable}, {"reason", reason}}); };
    for (const core::DataValue& value : set.values) {
        const core::VariableDef* def = core::find_def(variables, value.variable);
        if (def == nullptr) {
            skip(value.variable, "no_variable");
            continue;
        }
        if (!core::value_fits(def->type, value)) {
            skip(value.variable, "value_type");
            continue;
        }
        std::string why;
        const auto found = find_target(c.doc, *def, &why);
        if (!found) {
            skip(value.variable, why == "kind" ? "kind" : "missing");
            continue;
        }
        if (def->type == "visibility") {
            const Page& page = c.doc.page(*found->at);
            run_op(c, Json{{"op", "set_layer"}, {"page", page.index.json()}, {"id", *def->layer}, {"visible", std::get<bool>(value.value)}}, def->name);
        } else if (def->type == "text") {
            const std::string& text = std::get<std::string>(value.value);
            if (found->line != nullptr) {
                // (a line's text: edit_line's — its rubies and ranges followed; a line of Photoshop's type shape, edit_type's — its
                // box fitted again)
                if (found->line->native_type) run_op(c, Json{{"op", "edit_type"}, {"line", found->line->id}, {"text", text}}, def->name);
                else run_op(c, Json{{"op", "edit_line"}, {"id", found->line->id}, {"text", text}}, def->name);
            } else {
                const Page& page = c.doc.page(*found->at);
                run_op(c, Json{{"op", "edit_type"}, {"page", page.index.json()}, {"layer_id", *def->layer}, {"text", text}}, def->name);
            }
        } else {
            const core::PictureValue& picture = std::get<core::PictureValue>(value.value);
            const Page& page = c.doc.page(*found->at);
            if (found->layer->kind == LayerKind::Smart) {
                Json op{{"op", "smart_replace"}, {"page", page.index.json()}, {"id", *def->layer}, {"asset", picture.asset}};
                if (!picture.name.empty()) op["name"] = picture.name;
                run_op(c, op, def->name);
            } else {
                // (a page another holds: as the bus asks of an op on it)
                const Json* owner = core::get(c.doc.page_locks, page.id);
                if (owner != nullptr && core::py_truthy(*owner) && !(owner->is_string() && owner->get<std::string>() == c.actor.name())) {
                    throw OpError("variable " + def->name + ": page " + page.index.repr() + " locked by " + core::py_str(*owner));
                }
                const std::string bytes = picture_bytes(c.doc, picture.asset).first;
                replace_pixels(c, *found->at, *def, bytes);
            }
        }
    }
    variables = core::variables_of(c.doc);  // (the ops left them as they were; read again for the active one)
    variables.active = static_cast<std::int64_t>(index);
    core::set_variables(c.doc, variables);
    c.report = Json{{"applied", set.name}, {"data_set", index}, {"skipped", std::move(skipped)}};  // ("index" is the batch's: the op's place)
}

}  // namespace

Json variables_listing(const Document& doc) {
    const core::Variables variables = core::variables_of(doc);
    const Json stored = core::variables_to_json(variables);
    Json defs = Json::array();
    for (std::size_t i = 0; i < variables.defs.size(); ++i) {
        const core::VariableDef& def = variables.defs[i];
        Json item = stored["defs"][i];
        std::string why;
        const auto found = find_target(doc, def, &why);
        if (def.line) {
            const core::StoryLine* line = line_of(doc, def.line);
            item["page"] = line != nullptr ? line->page_index.json() : Json();
            item["target"] = line != nullptr ? Json(line->text) : Json();
        } else {
            const auto at = page_by_id(doc, def.page);
            item["page"] = at ? doc.page(*at).index.json() : Json();
            const Layer* layer = at ? layer_of(doc.page(*at), def.layer) : nullptr;
            item["target"] = layer != nullptr ? Json(layer->title) : Json();
        }
        item["missing"] = !found.has_value();
        defs.push_back(std::move(item));
    }
    return Json{{"defs", std::move(defs)}, {"data_sets", stored["data_sets"]}, {"active", variables.active ? Json(*variables.active) : Json()}};
}

void register_variable_ops(core::OpRegistry& registry) {
    registry.add("set_variables", set_variables_op);
    registry.add("set_data_sets", set_data_sets_op);
    registry.add("apply_data_set", apply_data_set_op);
}

}  // namespace genko::render
