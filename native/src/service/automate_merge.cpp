// ファイルをスタックとして読み込み・統計・Photomerge (service/automate.hpp, PC5 I-g; SPEC.md AUTO-01): PhotoCraft v0.2.0's
// engine/src/file_cmds.rs (676-712), automate_cmds.rs (359-389) and photo_cmds.rs (392-550), MIT
// (native/third_party/photocraft/NOTICE.md) — each a new book of one page in memory, its changes after it is made through the
// bus (run_steps) as the person who runs it.

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <tuple>

#include "core/actor.hpp"
#include "core/base64.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "render/codecs/codecs.hpp"
#include "render/fill.hpp"
#include "render/segment/segment.hpp"
#include "render/image.hpp"
#include "render/image_ops.hpp"
#include "render/layer_masks.hpp"
#include "render/ops_align.hpp"
#include "render/ops_filters.hpp"
#include "render/resample.hpp"
#include "render/stack.hpp"
#include "service/automate.hpp"
#include "storage/fsutil.hpp"

namespace genko::service::automate {

using core::Json;

namespace {

using render::image_ops::Pixels;

std::string stem_of(const std::string& name) {
    const fs::path p = core::path_from_utf8(name);
    const std::string stem = core::path_to_utf8(p.stem());
    return stem.empty() ? name : stem;
}

int depth_of(const core::Page& page) { return page.color ? page.color->depth : page.rgb ? page.rgb->depth : 8; }

// The page's pixels across and down (its paper at its resolution: a picture opened as a book is so many).
std::pair<std::int64_t, std::int64_t> pixels_of(const core::Page& page) {
    const double dpi = page.spec.dpi.value();
    return {std::llround(page.spec.width_mm.value() / 25.4 * dpi), std::llround(page.spec.height_mm.value() / 25.4 * dpi)};
}

// A layer the page shows as it holds it: one paint layer, plain (no mask, style, clipping, effect …).
bool plain_layer(const core::Layer& l) {
    return l.kind == core::LayerKind::Raster && l.visible && l.opacity == 1.0 && (l.blend.empty() || l.blend == "normal") && !l.mask && !l.style &&
           !l.clip && !l.vector_mask && !l.effect && !l.screen && l.stroke_count() == 0 && l.patches.empty() && l.blend_channels.empty();
}

// A picture as a book of one page of one paint layer: its own book when it is a picture opened so (one page of one plain
// layer), else its page's merged picture (formats::duplicate_page).
core::Document flat_book(const Picture& p, const formats::OpenOptions& open) {
    if (!p.doc) throw core::Error("value", "a picture to load is missing");
    const core::Page& page = p.doc->page(p.page);
    if (p.doc->pages.size() == 1 && !p.doc->asset_dir && page.layers.size() == 1 && plain_layer(page.layers.front())) return *p.doc;
    return formats::duplicate_page(*p.doc, p.page, p.name, true, open).doc;
}

core::Document applied(const core::Document& doc, const Json& ops, const core::Actor& actor) { return run_steps(doc, ops, actor).doc; }

// The colour mode a stack takes: rgb, gray, cmyk or lab (an indexed, bitmap, duotone or multichannel first picture: RGB).
std::string stack_mode(const core::Page& page) {
    const std::string mode = page_mode(page);
    return mode == "rgb" || mode == "gray" || mode == "cmyk" || mode == "lab" ? mode : "rgb";
}

// The book's one page made `mode` at `depth` (set_color_mode) when it is not.
core::Document conformed(core::Document doc, const std::string& mode, int depth, const core::Actor& actor) {
    const core::Page& page = doc.page(0);
    if (page_mode(page) == mode && depth_of(page) == depth && !(page.rgb && page.rgb->table)) return doc;
    Json set{{"op", "set_color_mode"}, {"page", 1}, {"mode", mode}};
    if (depth == 8 || depth == 16 || depth == 32) set["depth"] = mode == "lab" || mode == "cmyk" ? std::min(depth, 16) : depth;
    return applied(doc, Json::array({set}), actor);
}

// The book's page made w × h px, its pictures at the top left (set_canvas_size), when it is smaller.
core::Document widened(core::Document doc, std::int64_t w, std::int64_t h, const core::Actor& actor) {
    const auto [pw, ph] = pixels_of(doc.page(0));
    if (pw == w && ph == h) return doc;
    return applied(doc, Json::array({Json{{"op", "set_canvas_size"}, {"page", 1}, {"width_px", w}, {"height_px", h}, {"anchor", "top_left"}}}), actor);
}

// The one paint layer of a flat book (its last raster layer).
const core::Layer& only_layer(const core::Document& doc) {
    const core::Page& page = doc.page(0);
    for (auto it = page.layers.rbegin(); it != page.layers.rend(); ++it) {
        if (it->kind == core::LayerKind::Raster) return *it;
    }
    throw core::Error("value", "the picture has no pixels to load");
}

Json all_ids(const core::Document& doc) {
    Json ids = Json::array();
    for (const core::Layer& l : doc.page(0).layers) ids.push_back(l.id);
    return ids;
}

// The stack's layers made one smart object named `name`; its id.
std::string into_smart_object(core::Document& doc, const std::string& name, const core::Actor& actor) {
    const std::string id = core::new_id();
    doc = applied(doc, Json::array({Json{{"op", "smart_convert"}, {"page", 1}, {"ids", all_ids(doc)}, {"new_id", id}},
                                    Json{{"op", "set_layer"}, {"page", 1}, {"id", id}, {"name", name}}}),
                  actor);
    return id;
}

// A picture's pixels as straight RGBA floats (an ink page's: what its inks mean in RGB).
render::resample::Plane plane_of(const core::Document& book) {
    const core::Page& page = book.page(0);
    const Pixels p = render::image_ops::shown_layer_pixels(book, page, only_layer(book));
    render::resample::Plane out(static_cast<int>(p.width()), static_cast<int>(p.height()), 4, true);
    out.hdr = p.kind() == Pixels::Kind::Precise && p.floating;
    const std::size_t n = p.size();
    if (p.kind() == Pixels::Kind::Inks) {
        const std::vector<render::image_ops::Rgba> rgba = render::image_ops::rgba_of(p);
        for (std::size_t i = 0; i < n; ++i) std::copy_n(rgba[i].data(), 4, out.v.data() + i * 4);
        return out;
    }
    const unsigned c = p.colours();
    for (std::size_t i = 0; i < n; ++i) {
        const float* v = p.at(i);
        float* o = out.v.data() + i * 4;
        for (unsigned k = 0; k < 3; ++k) o[k] = v[c >= 3 ? k : 0];
        o[3] = v[c];
    }
    return out;
}

// A file opened as a book (formats::open_file), its folder given for what a .pcraft names beside it.
std::shared_ptr<const core::Document> open_book(const fs::path& file, const std::string& name, const formats::OpenOptions& open) {
    formats::OpenOptions options = open;
    options.folder = file.parent_path();
    return std::make_shared<const core::Document>(formats::open_file(storage::read_file(file), name, options).doc);
}

// The picture as a book: an open book's page as it is given, a file opened now (one at a time: let go by the caller).
Picture opened(const Picture& p, const formats::OpenOptions& open) {
    render::segment::check_stop();  // (the review's P3-3: stopped between the files)
    if (p.doc || !p.file) return p;
    Picture out = p;
    out.doc = open_book(*p.file, p.name, open);
    out.page = 0;
    return out;
}

// What a page's colours are numbers of: an RGB page's profile (empty: sRGB), a grey or CMYK page's, none for Lab and the
// special modes.
struct Profile {
    std::string ref;
    core::Bytes icc;
};
std::optional<Profile> profile_of(const core::Page& page) {
    if (page.color) {
        if (page.color->profile.empty()) return std::nullopt;
        return Profile{page.color->profile, page.color->icc};
    }
    if (page.rgb && page.rgb->table) return std::nullopt;
    return page.rgb ? Profile{page.rgb->profile, page.rgb->icc} : Profile{};
}

// convert_profile of the book's page into `to` (its ICC bytes, or a built-in profile by its ref; an empty ref is sRGB), when the
// page's own is another (the review's P2-5: the numbers of one profile never laid as another's). core::Error("value") when the
// colours cannot be converted.
core::Document in_profile(core::Document doc, const std::optional<Profile>& to, const std::string& name, const core::Actor& actor) {
    if (!to) return doc;
    const std::optional<Profile> now = profile_of(doc.page(0));
    if (now && now->ref == to->ref) return doc;
    Json op{{"op", "convert_profile"}, {"page", 1}};
    if (to->ref.empty()) op["profile"] = "srgb";
    else if (to->icc) op["icc"] = core::b64encode(*to->icc);
    else op["profile"] = to->ref;
    try {
        return applied(doc, Json::array({op}), actor);
    } catch (const std::exception& error) {
        throw core::Error("value", "the colours of the picture " + name + " cannot be converted into the first picture's profile (" + error.what() + ")");
    }
}

// Bytes in 万 pixels, for the words that refuse.
std::string man(double pixels) { return std::to_string(static_cast<long long>(pixels / 10000.0)); }

}  // namespace

std::vector<Picture> picture_files(const std::vector<fs::path>& files, const formats::OpenOptions& open) {
    if (files.size() > kMaxStackFiles) throw core::Error("value", "more than " + std::to_string(kMaxStackFiles) + " files to take: choose fewer");
    std::vector<Picture> out;
    for (const fs::path& file : files) {
        render::segment::check_stop();
        Picture p;
        p.name = core::path_to_utf8(file.filename());
        p.file = file;
        const std::string bytes = storage::read_file(file);
        if (const auto size = render::codecs::header_size(bytes)) {
            p.width = size->first;
            p.height = size->second;
        } else {
            // (a PSD, a RAW file, a .pcraft …: its page seen once, then let go)
            const auto book = open_book(file, p.name, open);
            std::tie(p.width, p.height) = pixels_of(book->page(0));
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::pair<std::int64_t, std::int64_t> picture_size(const Picture& picture) {
    if (picture.doc) return pixels_of(picture.doc->page(picture.page));
    return {picture.width, picture.height};
}

std::uint64_t precise_layers_bytes(std::uint64_t layers, std::uint64_t width, std::uint64_t height, int depth) {
    if (depth <= 8) return 0;
    const std::uint64_t each = 16 + width * height * 4 * (depth == 16 ? 2 : 4);
    return layers * each;
}

namespace {

std::uint64_t stack_bytes(const std::vector<Picture>& pictures, int bytes_per_pixel) {
    std::int64_t w = 0, h = 0, largest = 0;
    for (const Picture& p : pictures) {
        const auto [pw, ph] = picture_size(p);
        w = std::max(w, pw);
        h = std::max(h, ph);
        largest = std::max(largest, pw * ph);
    }
    // (each layer of the page as big as the largest picture, held as its pixels at the depth — the most its bytes take —, and
    // one picture at a time worked on in floats: decoded, converted, widened — four RGBA floats a pixel, twice)
    return std::uint64_t(pictures.size()) * std::uint64_t(w) * std::uint64_t(h) * std::uint64_t(bytes_per_pixel) +
           std::uint64_t(std::max(largest, w * h)) * 32u;
}

void check_stack(const std::vector<Picture>& pictures, int bytes_per_pixel) {
    const std::uint64_t bytes = stack_bytes(pictures, bytes_per_pixel);
    if (bytes > render::align::kBudget) {
        throw core::Error("value", "the stack would be too large to hold at once (about " + std::to_string(bytes >> 20) +
                                       " MiB of 2048): use fewer or smaller pictures");
    }
}

// (PC6, review C P2-1) A 16- or 32-bit RGB stack's layers — each the largest picture's size — within the book's precise
// pixels (`most`: 1 GiB), weighed once the first picture's mode and depth are read, before the others are opened.
void check_stack_book(const std::vector<Picture>& pictures, const std::string& mode, int depth, std::uint64_t most) {
    if (mode != "rgb" || depth <= 8) return;
    std::int64_t w = 0, h = 0;
    for (const Picture& p : pictures) {
        const auto [pw, ph] = picture_size(p);
        w = std::max(w, pw);
        h = std::max(h, ph);
    }
    const std::uint64_t bytes = precise_layers_bytes(pictures.size(), std::uint64_t(std::max<std::int64_t>(w, 1)), std::uint64_t(std::max<std::int64_t>(h, 1)), depth);
    if (bytes > most) {
        throw core::Error("value", "the stack's " + std::to_string(depth) + "-bit layers would pass the book's 1 GiB of precise pixels (about " +
                                       std::to_string(bytes >> 20) + " MiB): use fewer or smaller pictures, or 8-bit ones");
    }
}

}  // namespace

formats::Opened load_files_into_stack(const std::vector<Picture>& pictures, const StackOptions& options) {
    if (pictures.empty()) throw core::Error("value", "no files to load into a stack");
    if (pictures.size() > kMaxStackFiles) throw core::Error("value", "more than " + std::to_string(kMaxStackFiles) + " files to take: choose fewer");
    // (the review's P3-1: auto_align_layers takes at most 32 layers — said before any file is opened)
    if (options.align && pictures.size() > render::align::kMaxLayers) {
        throw core::Error("value", "more than " + std::to_string(render::align::kMaxLayers) + " pictures to align: choose fewer");
    }
    // (the review's P2-7: the stack weighed before any picture is decoded — 8 bits RGBA a pixel the least —, again by the
    // first picture's depth once it is open)
    check_stack(pictures, 4);
    // (the first picture's mode, depth, profile and resolution; the largest picture's size)
    core::Document base = flat_book(opened(pictures.front(), options.open), options.open);
    const std::string mode = stack_mode(base.page(0));
    const int depth = depth_of(base.page(0));
    check_stack(pictures, 4 * std::max(depth, 8) / 8);
    check_stack_book(pictures, mode, depth, options.open.precise_most);
    base = conformed(std::move(base), mode, depth, options.actor);
    const std::optional<Profile> profile = profile_of(base.page(0));
    std::int64_t w = 0, h = 0;
    for (const Picture& p : pictures) {
        const auto [pw, ph] = picture_size(p);
        w = std::max(w, pw);
        h = std::max(h, ph);
    }
    {
        const auto [bw, bh] = pixels_of(base.page(0));
        w = std::max(w, bw);
        h = std::max(h, bh);
    }
    base = widened(std::move(base), w, h, options.actor);
    // (one layer a picture, the first at the bottom, each its name; each picture opened in its turn and let go)
    std::vector<core::Layer> layers;
    {
        core::Layer first = only_layer(base);
        first.title = pictures.front().name;
        layers.push_back(std::move(first));
    }
    std::vector<std::string> notes;
    for (std::size_t i = 1; i < pictures.size(); ++i) {
        core::Document other = conformed(flat_book(opened(pictures[i], options.open), options.open), mode, depth, options.actor);
        other = in_profile(std::move(other), mode == "lab" ? std::nullopt : profile, pictures[i].name, options.actor);
        other = widened(std::move(other), w, h, options.actor);
        const auto [ow, oh] = pixels_of(other.page(0));
        if (ow != w || oh != h) throw core::Error("value", "the picture " + pictures[i].name + " could not be laid on the stack's page");
        core::Layer layer = only_layer(other);
        layer.id = core::new_id();
        layer.title = pictures[i].name;
        layer.parent_id = nullptr;
        layers.push_back(std::move(layer));
    }
    core::Page& page = base.edit_page(0);
    page.layers = std::move(layers);
    base.title = stem_of(pictures.front().name);
    if (options.align && page.layers.size() > 1) {
        base = applied(base, Json::array({Json{{"op", "auto_align_layers"}, {"page", 1}, {"ids", all_ids(base)}, {"projection", "auto"}}}), options.actor);
    }
    if (options.smart) (void)into_smart_object(base, base.title, options.actor);
    formats::Opened out;
    out.doc = std::move(base);
    out.notes = std::move(notes);
    return out;
}

formats::Opened statistics(const std::vector<Picture>& pictures, const std::string& mode, const StackOptions& options) {
    if (!render::stack::stat_of(mode)) {
        throw core::Error("value", "the stack mode is entropy, kurtosis, maximum, mean, median, minimum, range, skewness, standard_deviation, summation or variance");
    }
    StackOptions loading = options;
    loading.smart = false;
    formats::Opened out = load_files_into_stack(pictures, loading);
    const std::string id = into_smart_object(out.doc, out.doc.title, options.actor);
    out.doc = applied(out.doc, Json::array({Json{{"op", "smart_stack"}, {"page", 1}, {"id", id}, {"mode", mode}}}), options.actor);
    return out;
}

Panorama photomerge(const std::vector<Picture>& pictures, const PhotomergeOptions& options) {
    const auto layout = render::panorama::layout_from(options.layout);
    if (!layout) throw core::Error("value", "the layout is auto, perspective, cylindrical, spherical, collage or reposition");
    if (options.scale < 10 || options.scale > 100) throw core::Error("value", "the scale to read the pictures at is 10 to 100 %");
    if (pictures.size() < 2) throw core::Error("value", "Photomerge needs two or more pictures");
    if (pictures.size() > render::align::kMaxLayers) throw core::Error("value", "more than " + std::to_string(render::align::kMaxLayers) + " pictures: choose fewer");
    // (the review's P1-1: the pictures weighed before any is decoded — each held as RGBA floats at the scale read, and the
    // largest decoded and made RGBA floats at full size while it is read)
    {
        const double s = options.scale / 100.0;
        double held = 0.0, largest = 0.0, pixels = 0.0;
        for (const Picture& p : pictures) {
            const auto [w, h] = picture_size(p);
            const double n = static_cast<double>(w) * static_cast<double>(h);
            held += std::ceil(w * s) * std::ceil(h * s) * 16.0;
            largest = std::max(largest, n);
            pixels += n * s * s;
        }
        if (held + largest * 32.0 > static_cast<double>(render::align::kBudget)) {
            throw core::Error("value", "the pictures are too large to stitch at once (about " + man(pixels) +
                                           " 万 pixels in all): use fewer pictures or shrink them when they are read");
        }
    }
    const render::ImageAllocationBudget budget{render::align::kBudget};
    // (each picture opened in its turn, its colours made the first picture's profile's, flat, as RGBA held in the budget)
    std::vector<render::align::Source> sources;
    std::vector<std::unique_ptr<render::image_ops::Held>> holding;
    std::vector<double> focals;
    int depth = 8;
    std::optional<Profile> profile;
    std::optional<core::RgbMode> first_rgb;
    double dpi = 72.0;
    const core::Actor actor(std::string(core::kLegacyActor));
    for (std::size_t i = 0; i < pictures.size(); ++i) {
        const Picture p = opened(pictures[i], options.open);
        if (const auto camera = render::picture_camera(*p.doc, p.doc->page(p.page), nullptr); camera && camera->focal_length_35mm) {
            focals.push_back(*camera->focal_length_35mm);
        }
        core::Document book = flat_book(p, options.open);
        if (i == 0) {
            const core::Page& page = book.page(0);
            dpi = page.spec.dpi.value();
            const bool precise = page.color ? page.color->depth > 8 : page.rgb && page.rgb->depth > 8 && !page.rgb->table;
            depth = precise ? (page.color ? std::min(page.color->depth, 16) : page.rgb->depth) : 8;
            // (an RGB first picture's profile is the panorama's; a grey, CMYK or Lab one's colours as they show in sRGB)
            profile = !page.color && !(page.rgb && page.rgb->table) ? profile_of(page) : std::optional<Profile>(Profile{});
            if (!page.color && page.rgb && !page.rgb->table) first_rgb = page.rgb;
        }
        // (the review's P2-5: every picture's numbers made the panorama's profile's — an RGB page converted, any other made RGB
        // in it first)
        if (book.page(0).color || (book.page(0).rgb && book.page(0).rgb->table)) {
            Json set{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}};
            if (depth > 8) set["depth"] = depth;
            book = applied(book, Json::array({set}), actor);
        }
        book = in_profile(std::move(book), profile, p.name, actor);
        const auto [w, h] = pixels_of(book.page(0));
        holding.push_back(std::make_unique<render::image_ops::Held>(std::uint64_t(w) * std::uint64_t(h) * 16u));
        render::align::Source s{p.name, plane_of(book)};
        if (options.scale < 100) {
            const int sw = std::max(1, static_cast<int>(std::lround(s.plane.width * options.scale / 100.0)));
            const int sh = std::max(1, static_cast<int>(std::lround(s.plane.height * options.scale / 100.0)));
            const bool hdr = s.plane.hdr;
            auto small = std::make_unique<render::image_ops::Held>(std::uint64_t(sw) * std::uint64_t(sh) * 16u);
            s.plane = render::resample::resize(s.plane, sw, sh, render::resample::Filter::Bicubic, false);
            s.plane.hdr = hdr;
            holding.back() = std::move(small);
        }
        sources.push_back(std::move(s));
    }
    render::align::MergeOptions merge;
    merge.layout = *layout;
    merge.blend = options.blend;
    merge.vignette = options.vignette;
    merge.geometric = options.geometric;
    merge.content_aware_fill = options.content_aware_fill && options.blend;
    if (options.focal_length) {
        merge.focal35 = options.focal_length;
    } else if (!focals.empty()) {
        std::sort(focals.begin(), focals.end());
        merge.focal35 = focals[focals.size() / 2];
    }
    merge.max_pixels = kMaxPanoramaPixels;
    // (the review's P2-1: what the stitching may hold is what the budget has left beside the pictures held)
    merge.budget = render::align::kBudget > budget.live() ? render::align::kBudget - budget.live() : 1;
    render::align::Merged merged;
    try {
        merged = render::align::photomerge(sources, merge);
    } catch (const core::OpError& error) {
        throw core::Error("value", error.what());
    }
    sources.clear();
    holding.clear();
    // (the parts it made held while the book's layers are made of them, one layer's pixels at a time)
    std::uint64_t made = 0;
    for (const auto& part : merged.layers) made += std::uint64_t(part.plane.v.size()) * sizeof(float);
    for (const auto& part : merged.masks) made += std::uint64_t(part.plane.v.size()) * sizeof(float);
    if (merged.fill) made += std::uint64_t(merged.fill->v.size()) * sizeof(float);
    const render::image_ops::Held parts(made);
    // (PC6, review C P1-1: a precise panorama's layers — each of the panorama's size — within the book's precise pixels: past
    // them made at 16 bits, then at 8, and said, as a PSD's layers are read — the book can then be saved and changed)
    std::string fewer_bits;
    {
        const std::uint64_t count = merged.layers.size() + (merged.fill ? 1 : 0);
        const int was = depth;
        while (depth > 8 && precise_layers_bytes(count, std::uint64_t(std::max(1, merged.width)), std::uint64_t(std::max(1, merged.height)), depth) >
                                options.open.precise_most) {
            depth = depth == 32 ? 16 : 8;
        }
        if (depth != was) fewer_bits = "高精度の画素が本の上限（1 GiB）を越えるため、パノラマを " + std::to_string(depth) + " bit で作りました";
    }
    // (the book: an image of the panorama's size, RGB at the first picture's depth and profile, transparent)
    formats::NewImage image;
    image.title = "パノラマ";
    image.width = merged.width;
    image.height = merged.height;
    image.dpi = std::clamp(dpi, 72.0, 100000.0);
    image.mode = "rgb";
    image.depth = depth;
    image.background = "transparent";
    Panorama out;
    out.opened = formats::new_image(image, options.open);
    core::Document& doc = out.opened.doc;
    if (first_rgb && !first_rgb->profile.empty()) {
        // (the first picture's profile — 8 bits or precise —: the panorama's numbers are its numbers, every picture converted
        // into it above)
        core::RgbMode kept = *first_rgb;
        kept.depth = depth;
        kept.table.reset();
        doc.rgb = kept;
        doc.edit_page(0).rgb = kept;
    } else if (depth == 8) {
        // (sRGB: the numbers the pictures were converted into, whatever the working space)
        doc.rgb.reset();
        doc.edit_page(0).rgb.reset();
    } else {
        core::RgbMode kept = doc.page(0).rgb.value_or(core::RgbMode{});
        kept.depth = depth;
        kept.profile.clear();
        kept.icc = nullptr;
        doc.rgb = kept;
        doc.edit_page(0).rgb = kept;
    }
    core::Page& page = doc.edit_page(0);
    page.layers.clear();
    const Pixels::Kind kind = depth > 8 ? Pixels::Kind::Precise : Pixels::Kind::Eight;
    const std::size_t W = static_cast<std::size_t>(merged.width), H = static_cast<std::size_t>(merged.height);
    const render::Size grid = render::layer_masks::grid_size(doc.page(0));
    const auto put = [&](const std::string& name, const render::resample::Plane& plane, int x0, int y0, const render::resample::Plane* mask) {
        core::Layer layer;
        layer.id = core::new_id();
        layer.role = core::LayerRole::User;
        layer.kind = core::LayerKind::Raster;
        layer.title = name;
        layer.panel_each = false;
        const std::string id = layer.id;
        doc.edit_page(0).layers.push_back(std::move(layer));
        Pixels px(kind, static_cast<std::uint32_t>(W), static_cast<std::uint32_t>(H), 3);
        px.floating = depth == 32;
        for (int y = 0; y < plane.height; ++y) {
            for (int x = 0; x < plane.width; ++x) {
                const std::size_t gx = static_cast<std::size_t>(x0 + x), gy = static_cast<std::size_t>(y0 + y);
                if (gx >= W || gy >= H) continue;
                std::copy_n(plane.at(x, y), 4, px.at(gy * W + gx));
            }
        }
        render::image_ops::put_layer_pixels(doc, 0, id, px);
        if (mask == nullptr) return;
        std::string bytes(W * H, '\0');
        for (int y = 0; y < mask->height; ++y) {
            for (int x = 0; x < mask->width; ++x) {
                const std::size_t gx = static_cast<std::size_t>(x0 + x), gy = static_cast<std::size_t>(y0 + y);
                if (gx >= W || gy >= H) continue;
                bytes[gy * W + gx] = static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(*mask->at(x, y), 0.0f, 1.0f) * 255.0f)));
            }
        }
        render::Image picture = render::Image::frombytes("L", render::Size{static_cast<int>(W), static_cast<int>(H)}, bytes);
        if (picture.width() != grid.width || picture.height() != grid.height) picture = picture.resize(grid);
        core::Mask m;
        m.png = render::fills::png_bytes(picture);
        m.enabled = true;
        core::Page& on = doc.edit_page(0);
        for (core::Layer& l : on.layers) {
            if (l.id == id) l.mask = std::move(m);
        }
    };
    for (std::size_t j = 0; j < merged.layers.size(); ++j) {
        const render::align::Part& part = merged.layers[j];
        put(pictures[merged.placed[j]].name, part.plane, part.x0, part.y0, j < merged.masks.size() ? &merged.masks[j].plane : nullptr);
    }
    if (merged.fill) put("コンテンツに応じた塗りつぶし", *merged.fill, 0, 0, nullptr);
    Json placed = Json::array(), failed = Json::array();
    for (const std::size_t i : merged.placed) placed.push_back(pictures[i].name);
    for (const std::size_t i : merged.failed) failed.push_back(pictures[i].name);
    out.info = merged.info;
    out.info["layout"] = merged.layout;
    out.info["reference"] = pictures[merged.reference].name;
    out.info["placed"] = placed;
    out.info["failed"] = failed;
    out.info["width"] = merged.width;
    out.info["height"] = merged.height;
    out.info["layers"] = doc.page(0).layers.size();
    if (!fewer_bits.empty()) out.opened.notes.push_back(fewer_bits);
    if (!merged.failed.empty()) out.opened.notes.push_back("つなげなかった画像があります（" + std::to_string(merged.failed.size()) + " 枚）");
    return out;
}

}  // namespace genko::service::automate
