// PC2c (画像の演算, SPEC.md COLOR-04): the ops apply_image and calculations (design 3.4, PhotoCraft v0.2.0's
// engine/channel_cmds.rs:200-427, 1298-1393, MIT: third_party/photocraft/NOTICE.md; render/ops_adjust.hpp). A source is
// one or all colour planes, the transparency or the mask of a layer or of the page as it shows (merged), or an alpha or
// spot channel (PC1c), of a page of the same book and size (the user's decision 7: no other book), stretched onto the
// target's pixels. Apply Image blends it into a paint layer's colour planes — each plane its values as the layer holds
// them: RGB its values; CMYK and a multichannel page's plates their ink 0..1 (PhotoCraft's planes: ink, not brightness);
// Lab L/100, (a, b + 128)/255; grey its lightness — or into an alpha or spot channel. Calculations blends two grey planes
// into a new alpha channel, a new grey page or a selection.

#include <algorithm>
#include <cmath>

#include "core/channels.hpp"
#include "core/areas.hpp"
#include "core/color_model.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/ops_util.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/channels.hpp"
#include "render/image_ops.hpp"
#include "render/ops_adjust.hpp"
#include "render/png.hpp"
#include "render/selection.hpp"

namespace genko::render::adjust_ops {

namespace {

using core::Json;
using core::OpError;
using image_ops::Pixels;

constexpr int kGridDpi = 200;  // (calculations: the page's planes at the selection's and an alpha channel's 200 dpi)

const Json* field(const Json& op, const char* key) {
    const Json* v = core::get(op, key);
    return v == nullptr || v->is_null() ? nullptr : v;
}

double number_of(const Json& op, const char* key, double fallback, double lo, double hi) {
    const Json* v = field(op, key);
    if (v == nullptr) return fallback;
    if (!v->is_number() || v->is_boolean() || !std::isfinite(v->get<double>())) throw OpError(std::string(key) + " must be a number");
    return std::clamp(v->get<double>(), lo, hi);
}

bool flag_of(const Json& op, const char* key) {
    const Json* v = field(op, key);
    if (v == nullptr) return false;
    if (!v->is_boolean()) throw OpError(std::string(key) + " must be true or false");
    return v->get<bool>();
}

bool same_size(const core::Page& a, const core::Page& b) {
    return a.spec.width_mm.value() == b.spec.width_mm.value() && a.spec.height_mm.value() == b.spec.height_mm.value();
}

using Plane = std::vector<float>;

// A plane of `w` × `h` stretched onto another size (bilinear by the pixels' centres, as render/channels resizes).
Plane fitted(Plane values, std::uint32_t w, std::uint32_t h, std::uint32_t to_w, std::uint32_t to_h) {
    if (w == to_w && h == to_h) return values;
    channels::Plane from;
    from.width = w;
    from.height = h;
    from.values = std::move(values);
    return channels::resized(from, to_w, to_h).values;
}

// The grey a colour shows (Calculations takes a composite as its luminosity: 0.299 R + 0.587 G + 0.114 B).
Plane luminosity(const Pixels& p) {
    Plane out(p.size());
    if (p.kind() == Pixels::Kind::Inks && p.ink && p.ink->space == core::InkSpace::Gray) {
        for (std::size_t i = 0; i < p.size(); ++i) out[i] = p.at(i)[0];
        return out;
    }
    const image_ops::Held held(std::uint64_t(p.size()) * sizeof(image_ops::Rgba));  // (their RGBA, in the budget)
    const std::vector<image_ops::Rgba> rgb = image_ops::rgba_of(p);
    for (std::size_t i = 0; i < p.size(); ++i) out[i] = 0.299f * rgb[i][0] + 0.587f * rgb[i][1] + 0.114f * rgb[i][2];
    return out;
}

// A source {"page"?, "layer": id | "merged", "channel", "invert"?} as planes of `w` × `h` over the target page: the
// composite's colour planes (one when `gray`: its luminosity), else the one plane it names.
std::vector<Plane> source_planes(const core::Document& doc, std::size_t target, const Json* spec, std::uint32_t w, std::uint32_t h, bool gray,
                                 const std::string& where) {
    const Json given = spec != nullptr ? *spec : Json::object();
    if (!given.is_object()) throw OpError(where + " must be {\"page\"?, \"layer\": id or \"merged\", \"channel\", \"invert\"?}");
    for (const auto& [key, value] : given.items()) {
        if (key != "page" && key != "layer" && key != "channel" && key != "invert") throw OpError(where + " has no setting " + core::py_repr_str(key));
    }
    std::size_t at = target;
    if (const Json* page = field(given, "page")) {
        try {
            at = core::require_page(doc, Json{{"page", *page}});
        } catch (const core::Error& error) {
            throw OpError(where + ".page: " + error.what());
        }
        if (doc.is_deferred(at)) throw OpError(where + ".page: the page is not read yet");
    }
    const core::Page& page = doc.page(at);
    if (!same_size(page, doc.page(target)))
        throw OpError(where + ": page " + page.index.repr() + " is not the same size as page " + doc.page(target).index.repr());
    const Json* layer_value = field(given, "layer");
    if (layer_value != nullptr && !layer_value->is_string()) throw OpError(where + ".layer must be a layer's id or \"merged\"");
    const std::string layer_id = layer_value != nullptr ? layer_value->get<std::string>() : std::string("merged");
    const core::Layer* layer = nullptr;
    if (layer_id != "merged") {
        for (const core::Layer& each : page.layers) {
            if (each.id == layer_id) layer = &each;
        }
        if (layer == nullptr) throw OpError(where + ": no layer " + layer_id + " on page " + page.index.repr());
    }
    const Json* channel_value = field(given, "channel");
    if (channel_value != nullptr && !channel_value->is_string()) throw OpError(where + ".channel must be a channel's name");
    const std::string channel = channel_value != nullptr ? channel_value->get<std::string>() : std::string("composite");
    const bool invert = flag_of(given, "invert");
    std::vector<Plane> out;
    std::uint32_t sw = 0, sh = 0;
    const auto pixels = [&]() { return layer != nullptr ? image_ops::shown_layer_pixels(doc, page, *layer) : image_ops::page_pixels(doc, page); };
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    const std::size_t plate = channel == "composite" ? kNone : core::plate_index(page, channel).value_or(kNone);
    const bool one_plate = core::plate_names(page).size() == 1;  // (a grey page's one plate: its composite)
    if (channel == "composite" || (plate != kNone && one_plate)) {
        const Pixels p = pixels();
        sw = p.width();
        sh = p.height();
        if (gray) {
            out.push_back(luminosity(p));
        } else {
            for (unsigned c = 0; c < p.colours(); ++c) {
                Plane plane(p.size());
                for (std::size_t i = 0; i < p.size(); ++i) plane[i] = p.at(i)[c];
                out.push_back(std::move(plane));
            }
        }
    } else if (channel == "transparency") {
        if (layer == nullptr) throw OpError(where + ": the transparency is a layer's (give its id)");
        const Pixels p = pixels();
        sw = p.width();
        sh = p.height();
        Plane plane(p.size());
        for (std::size_t i = 0; i < p.size(); ++i) plane[i] = p.at(i)[p.colours()];
        out.push_back(std::move(plane));
    } else if (channel == "mask") {
        if (layer == nullptr || !layer->mask || !layer->mask->png) throw OpError(where + ": the layer has no mask");
        const Image mask = open_image(*layer->mask->png, kPillowOpenLimits).convert("L");
        sw = static_cast<std::uint32_t>(mask.width());
        sh = static_cast<std::uint32_t>(mask.height());
        const std::string bytes = mask.tobytes();
        Plane plane(bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) plane[i] = static_cast<unsigned char>(bytes[i]) / 255.0f;
        out.push_back(std::move(plane));
    } else if (plate != kNone) {
        const Pixels p = pixels();
        sw = p.width();
        sh = p.height();
        if (plate >= p.colours()) throw OpError(where + ": the layer holds no plate " + core::py_repr_str(channel));
        Plane plane(p.size());
        for (std::size_t i = 0; i < p.size(); ++i) plane[i] = p.at(i)[plate];
        out.push_back(std::move(plane));
    } else if (const core::Channel* found = core::find_channel(page, channel)) {
        channels::Plane plane = channels::plane_of(page, *found);
        sw = plane.width;
        sh = plane.height;
        out.push_back(std::move(plane.values));
    } else {
        throw OpError(where + ": no channel " + core::py_repr_str(channel) + " on page " + page.index.repr());
    }
    for (Plane& plane : out) {
        plane = fitted(std::move(plane), sw, sh, w, h);
        if (invert)
            for (float& v : plane) v = 1.0f - v;
    }
    return out;
}

// The op's mask (a grey source; none, or one without its channel or with channel "none": no mask), as PhotoCraft
// reads it.
std::optional<Plane> mask_plane(const core::Document& doc, std::size_t at, const Json& op, std::uint32_t w, std::uint32_t h) {
    const Json* mask = field(op, "mask");
    if (mask == nullptr || *mask == Json(false)) return std::nullopt;
    if (mask->is_object()) {
        const Json* channel = field(*mask, "channel");
        if (channel == nullptr || *channel == Json("none")) return std::nullopt;
    }
    return source_planes(doc, at, mask, w, h, true, "mask").front();
}

// --- new pages and channels, as split_channels makes them (render/ops_channels.cpp) ------------------------------------

core::Page grey_page(const core::Document& doc, const core::Page& like, const Plane& values, std::uint32_t w, std::uint32_t h,
                     const std::string& title) {
    core::Page page = core::make_page(core::Num(static_cast<std::int64_t>(doc.pages.size()) + 1), like.spec, like.binding);
    page.color.reset();
    page.rgb.reset();
    const core::BuiltinProfile* sgray = core::builtin_profile("sgray");
    core::PageColor grey;
    grey.space = core::InkSpace::Gray;
    grey.depth = core::page_depth(like);
    grey.profile = sgray->ref();
    grey.icc = sgray->bytes;
    page.color = grey;
    core::Layer layer;
    layer.id = core::new_id();
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.title = title;
    layer.lock_alpha = true;
    layer.panel_clip = false;
    core::InkPixels pixels = core::blank_ink_pixels(core::InkSpace::Gray, grey.depth, w, h);
    for (std::size_t i = 0; i < values.size(); ++i) {
        pixels.set(i, 0, std::clamp(static_cast<double>(values[i]), 0.0, 1.0));
        pixels.set(i, 1, 1.0);
    }
    layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(pixels));
    page.layers.insert(page.layers.begin() + 1, std::move(layer));  // (over the paper's layer)
    return page;
}

// The page put after the page at `after`, the book numbered again; its number.
std::int64_t insert_page(core::Document& doc, std::size_t after, core::Page made) {
    std::vector<core::Num> order;
    for (const auto& page : doc.pages) order.push_back(page->index);
    made.index = core::Num(static_cast<std::int64_t>(doc.pages.size()) + 1);
    const core::Num fresh = made.index;
    const std::string id = made.id;
    doc.pages.push_back(std::make_shared<core::Page>(std::move(made)));
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(std::min(after + 1, order.size())), fresh);
    core::reorder_pages(doc, order);
    for (const auto& page : doc.pages) {
        if (page->id == id) return core::py_int(page->index.json());
    }
    throw OpError("the page made is not in the book");
}

}  // namespace

void apply_image(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const Json* id = field(op, "id");
    const Json* channel = field(op, "channel");
    if ((id == nullptr) == (channel == nullptr)) throw OpError("give the layer (id) or the alpha or spot channel (channel) to apply the image to");
    if (id != nullptr && !id->is_string()) throw OpError("id must be a layer's id");
    if (channel != nullptr && !channel->is_string()) throw OpError("channel must be an alpha or spot channel's id or name");
    const image_ops::Blending blending = image_ops::blending_of(op);
    const auto opacity = static_cast<float>(number_of(op, "opacity", 100, 0, 100) / 100.0);
    const bool preserve = flag_of(op, "preserve_transparency");
    const std::optional<Json> area = core::truthy_at(op, "area") ? std::optional<Json>(core::op_area(op)) : std::nullopt;
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    if (channel != nullptr) {  // (a grey target: a colour source as the mean of its planes, as PhotoCraft takes it)
        const core::Page& page = doc.page(at);
        const std::size_t index = core::channel_index(page, channel->get<std::string>());
        if (index >= page.channels.size()) {
            throw OpError("no alpha or spot channel " + core::py_repr_str(channel->get<std::string>()) + " on page " + page.index.repr() +
                          " (a colour plate: give the layer)");
        }
        channels::Plane plane = channels::plane_of(page, page.channels[index]);
        const std::vector<Plane> src = source_planes(doc, at, field(op, "source"), plane.width, plane.height, false, "source");
        const std::optional<Plane> mask = mask_plane(doc, at, op, plane.width, plane.height);
        const std::optional<std::vector<float>> cover =
            area ? std::optional<std::vector<float>>(image_ops::coverage(page, *area, plane.width, plane.height, false)) : std::nullopt;
        for (std::size_t i = 0; i < plane.values.size(); ++i) {
            const float w = opacity * (mask ? (*mask)[i] : 1.0f) * (cover ? (*cover)[i] : 1.0f);
            if (w <= 0.0f) continue;
            float top = 0;
            for (const Plane& s : src) top += s[i];
            top /= static_cast<float>(src.size());
            float& b = plane.values[i];
            b += (blending.apply(b, top) - b) * w;
        }
        doc.edit_page(at).channels[index].raster = channels::encode(plane, core::page_depth(page));
        doc.edit_page(at).channels[index].memo = {};
        c.report = Json{{"channel", doc.page(at).channels[index].id}};
        return;
    }
    const std::string layer_id = id->get<std::string>();
    Pixels p = image_ops::layer_pixels(c, at, layer_id);
    const std::vector<Plane> src = source_planes(doc, at, field(op, "source"), p.width(), p.height(), false, "source");
    const std::optional<Plane> mask = mask_plane(doc, at, op, p.width(), p.height());
    const std::optional<std::vector<float>> cover =
        area ? std::optional<std::vector<float>>(image_ops::coverage(doc.page(at), *area, p.width(), p.height(), p.kind() == Pixels::Kind::Eight))
             : std::nullopt;
    const unsigned n = p.colours();
    for (std::size_t i = 0; i < p.size(); ++i) {
        float* v = p.at(i);
        float w = opacity * (mask ? (*mask)[i] : 1.0f) * (cover ? (*cover)[i] : 1.0f);
        if (preserve) w *= std::clamp(v[n], 0.0f, 1.0f);
        if (w <= 0.0f) continue;
        for (unsigned k = 0; k < n; ++k) {
            const float top = src[k < src.size() ? k : 0][i];
            v[k] += (blending.apply(v[k], top) - v[k]) * w;
        }
        if (!preserve) v[n] = std::max(v[n], w);  // (a clear pixel painted becomes opaque, as Apply Image does on layers)
    }
    image_ops::put_layer_pixels(doc, at, layer_id, p);
    c.report = Json{{"layer", layer_id}};
}

void calculations(core::OpContext& c) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const std::size_t at = core::require_page(doc, op);
    const image_ops::Blending blending = image_ops::blending_of(op);
    const auto opacity = static_cast<float>(number_of(op, "opacity", 100, 0, 100) / 100.0);
    const Json* result_value = field(op, "result");
    if (result_value != nullptr && !result_value->is_string()) throw OpError("result must be channel, page or selection");
    const std::string result = result_value != nullptr ? result_value->get<std::string>() : std::string("channel");
    if (result != "channel" && result != "page" && result != "selection") throw OpError("result must be channel, page or selection");
    const Json* name_value = field(op, "name");
    if (name_value != nullptr && !name_value->is_string()) throw OpError("name must be a string");
    const core::Page& page = doc.page(at);
    const channels::Plane grid = channels::blank(page, kGridDpi);
    const std::uint32_t w = grid.width, h = grid.height;
    const ImageAllocationBudget budget{2ULL * 1024 * 1024 * 1024};
    const Plane s1 = source_planes(doc, at, field(op, "source1"), w, h, true, "source1").front();
    const Plane s2 = source_planes(doc, at, field(op, "source2"), w, h, true, "source2").front();
    const std::optional<Plane> mask = mask_plane(doc, at, op, w, h);
    // (source 1 blended onto source 2)
    channels::Plane out;
    out.width = w;
    out.height = h;
    out.values.resize(s2.size());
    for (std::size_t i = 0; i < s2.size(); ++i) out.values[i] = s2[i] + (blending.apply(s2[i], s1[i]) - s2[i]) * opacity * (mask ? (*mask)[i] : 1.0f);
    if (result == "selection") {  // (the book as it is: the selection's words)
        const Image picture = channels::mask_image(out, Size{static_cast<int>(w), static_cast<int>(h)});
        const auto area = selection::from_mask(picture, selection::kSelDpi);
        c.report = Json{{"area", area ? *area : Json()}};
        return;
    }
    if (result == "page") {
        const std::string title = name_value != nullptr ? name_value->get<std::string>() : std::string("演算");
        const std::int64_t number = insert_page(doc, at, grey_page(doc, page, out.values, w, h, title));
        c.report = Json{{"page", number}};
        return;
    }
    if (page.channels.size() >= core::kChannelsMax) throw OpError("a page has at most " + std::to_string(core::kChannelsMax) + " channels");
    std::string name;
    try {
        name = name_value != nullptr ? core::channel_name(name_value->get<std::string>()) : core::next_channel_name(page, core::ChannelKind::Alpha);
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
    for (const core::Channel& each : page.channels) {
        if (each.name == name) throw OpError("the page has a channel named " + core::py_repr_str(name) + " already");
    }
    core::Channel made;
    made.id = core::new_channel_id();
    made.name = name;
    made.kind = core::ChannelKind::Alpha;
    made.raster = channels::encode(out, core::page_depth(page));
    doc.edit_page(at).channels.push_back(made);
    c.report = Json{{"channel", made.id}};
}

}  // namespace genko::render::adjust_ops
