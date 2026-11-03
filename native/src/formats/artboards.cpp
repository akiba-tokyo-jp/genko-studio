// PC5 I-c (formats/artboards.hpp): the artboards' files — to files, to PDF, one PSD, and a PSD's artboards as pages. After
// PhotoCraft v0.2.0's engine/src/artboard_cmds.rs (336-430: artboards_to_files, raster_pdf, artboards_to_pdf) and
// crates/io/src/psd_import.rs (313-315: a group's artboard), MIT (native/third_party/photocraft/NOTICE.md).

#include "formats/artboards.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "core/actor.hpp"
#include "core/artboards.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/psd_keep.hpp"
#include "formats/export.hpp"
#include "formats/output.hpp"
#include "render/ops_layer_comps.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/pcraft_annotations.hpp"
#include "render/pcraft_manifest.hpp"
#include "render/png.hpp"
#include "render/psd_artboards.hpp"

namespace genko::formats {

namespace fs = std::filesystem;
using core::Json;
using render::codecs::CodecImage;
using render::codecs::Layout;
using render::codecs::Sample;

namespace {

// A name cut by whole characters to at most `most` bytes (file systems take 255 bytes a name).
std::string cut(std::string text, std::size_t most) {
    if (text.size() <= most) return text;
    text.resize(most);
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
    if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0x80) != 0) text.pop_back();
    return text;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::array<std::int64_t, 2> pixels_of(const core::Page& page) {
    const int dpi = static_cast<int>(std::llround(page.spec.dpi.value()));
    return {render::mm_to_px(page.spec.width_mm.value(), dpi), render::mm_to_px(page.spec.height_mm.value(), dpi)};
}

int dpi_of(const core::Page& page) { return std::max(1, static_cast<int>(std::llround(page.spec.dpi.value()))); }

// The number of colour components of a JPEG (its frame header's), 3 when it cannot be read.
int jpeg_components(std::string_view jpeg) {
    std::size_t at = 2;
    while (at + 4 <= jpeg.size()) {
        if (static_cast<unsigned char>(jpeg[at]) != 0xFF) return 3;
        const unsigned marker = static_cast<unsigned char>(jpeg[at + 1]);
        const std::size_t length = (std::size_t(static_cast<unsigned char>(jpeg[at + 2])) << 8) | static_cast<unsigned char>(jpeg[at + 3]);
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (at + 9 < jpeg.size()) return static_cast<unsigned char>(jpeg[at + 9]);
            return 3;
        }
        at += 2 + length;
    }
    return 3;
}

std::string fixed3(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

// A PDF of one page a JPEG (its pixels, resolution, bytes and components), each page its picture's size on paper.
// PhotoCraft's raster_pdf.
struct PdfPage {
    std::uint32_t width = 0, height = 0;
    double dpi = 72;
    std::string jpeg;
};
std::string raster_pdf(const std::vector<PdfPage>& pages) {
    std::string out = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
    std::vector<std::size_t> offsets;
    const auto obj = [&](std::string_view body) {
        offsets.push_back(out.size());
        out += std::to_string(offsets.size()) + " 0 obj\n";
        out += body;
        out += "\nendobj\n";
    };
    std::string kids;
    for (std::size_t i = 0; i < pages.size(); ++i) kids += (i == 0 ? "" : " ") + std::to_string(3 + i * 3) + " 0 R";
    obj("<< /Type /Catalog /Pages 2 0 R >>");
    obj("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages.size()) + " >>");
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const PdfPage& p = pages[i];
        const double k = 72.0 / std::max(1.0, p.dpi);
        const std::string pw = fixed3(p.width * k), ph = fixed3(p.height * k);
        const std::size_t contents = 4 + i * 3, image = 5 + i * 3;
        obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + pw + " " + ph + "] /Resources << /XObject << /Im0 " + std::to_string(image) +
            " 0 R >> >> /Contents " + std::to_string(contents) + " 0 R >>");
        const std::string stream = "q " + pw + " 0 0 " + ph + " 0 0 cm /Im0 Do Q";
        obj("<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream");
        const int components = jpeg_components(p.jpeg);
        const std::string space = components == 1 ? "/DeviceGray" : components == 4 ? "/DeviceCMYK" : "/DeviceRGB";
        std::string img = "<< /Type /XObject /Subtype /Image /Width " + std::to_string(p.width) + " /Height " + std::to_string(p.height) +
                          " /ColorSpace " + space + " /BitsPerComponent 8 /Filter /DCTDecode";
        if (components == 4) img += " /Decode [1 0 1 0 1 0 1 0]";  // (Adobe's CMYK JPEG: inverted)
        img += " /Length " + std::to_string(p.jpeg.size()) + " >>\nstream\n";
        img += p.jpeg;
        img += "\nendstream";
        obj(img);
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t o : offsets) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

// Two pages of one colour mode, their depth apart, as one PSD of artboards asks them alike: the whole of their RGB mode
// (profile, intent, an indexed page's table) or of their inks (space, profile, a duotone's inks, a bitmap's look, the
// plates…) — the review's P3-4.
bool same_mode(const core::Page& a, const core::Page& b) {
    if (a.color.has_value() != b.color.has_value()) return false;
    if (a.color) {
        core::PageColor x = *a.color, y = *b.color;
        x.depth = y.depth = 8;
        return x == y;
    }
    core::RgbMode x = a.rgb.value_or(core::RgbMode{}), y = b.rgb.value_or(core::RgbMode{});
    x.depth = y.depth = 8;
    return x == y;
}

// A name Windows keeps for its devices (CON, PRN, AUX, NUL, COM1..9, LPT1..9 — in any case, before any dot).
bool reserved_name(const std::string& stem) {
    std::string base = lower(stem.substr(0, stem.find('.')));
    while (!base.empty() && (base.back() == ' ')) base.pop_back();
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") return true;
    return base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9';
}

int depth_of(const core::Page& page) {
    if (page.color) return page.color->depth;
    if (page.rgb) return page.rgb->depth;
    return 8;
}

// The top-level layer a layer is in (itself when it is in none), by its folders.
std::string top_of(const core::Page& page, const std::string& id) {
    std::map<std::string, const core::Layer*> by_id;
    for (const core::Layer& l : page.layers) by_id.emplace(l.id, &l);
    std::string at = id;
    std::set<std::string> seen;
    while (seen.insert(at).second) {
        const auto found = by_id.find(at);
        if (found == by_id.end() || !found->second->parent_id.is_string()) return at;
        const std::string parent = found->second->parent_id.get<std::string>();
        if (!by_id.contains(parent)) return at;
        at = parent;
    }
    return at;
}

}  // namespace

std::vector<std::size_t> artboard_pages(const core::Document& doc, const std::vector<std::string>& names) {
    std::vector<std::size_t> out;
    std::set<std::string> found;
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        const core::Page& page = *doc.pages[i];
        if (!page.artboard) continue;
        if (!names.empty() && std::find(names.begin(), names.end(), page.artboard->name) == names.end()) continue;
        if (doc.is_deferred(i)) throw core::Error("page_not_loaded", "the page is not read yet");
        found.insert(page.artboard->name);
        out.push_back(i);
    }
    for (const std::string& name : names) {
        if (!found.contains(name)) throw core::Error("value", "no artboard named " + name);
    }
    if (out.empty()) throw core::Error("value", "the book has no artboards");
    return out;
}

PagePicture artboard_picture(const core::Page& page, const core::Document& doc, int dpi, std::string_view area) {
    if (!core::transparent_artboard(page)) return page_picture(page, doc, dpi, area);
    if (page.color || (page.rgb && !page.rgb->is_default())) {
        PagePicture out = page_picture(page, doc, dpi, area);
        out.notes.push_back("透明の背景は 8 bit の RGB のアートボードだけ透明に書けます。このアートボードは白の上に書きました");
        return out;
    }
    core::Page white = page, black = page;
    white.extra["paper_rgb"] = Json::array({255, 255, 255});
    black.extra["paper_rgb"] = Json::array({0, 0, 0});
    PagePicture over_white = page_picture(white, doc, dpi, area);
    const PagePicture over_black = page_picture(black, doc, dpi, area);
    const CodecImage& w = over_white.image;
    const CodecImage& b = over_black.image;
    if (w.layout() != Layout::Rgb || b.layout() != Layout::Rgb || w.sample() != Sample::U8 || w.width() != b.width() || w.height() != b.height()) {
        return over_white;
    }
    const std::string_view wb(w.data().data(), w.data().size()), bb(b.data().data(), b.data().size());
    const std::size_t n = std::size_t(w.width()) * w.height();
    std::string rgba(n * 4, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        int spread = 0;
        for (int k = 0; k < 3; ++k) {
            spread = std::max(spread, int(static_cast<unsigned char>(wb[i * 3 + k])) - int(static_cast<unsigned char>(bb[i * 3 + k])));
        }
        const int alpha = std::clamp(255 - spread, 0, 255);
        for (int k = 0; k < 3; ++k) {
            const int on_black = static_cast<unsigned char>(bb[i * 3 + k]);
            rgba[i * 4 + k] = static_cast<char>(alpha == 0 ? 0 : std::clamp((on_black * 255 + alpha / 2) / alpha, 0, 255));
        }
        rgba[i * 4 + 3] = static_cast<char>(alpha);
    }
    PagePicture out;
    out.image = CodecImage::from_bytes(w.width(), w.height(), Layout::Rgba, Sample::U8, std::move(rgba));
    out.image.icc = w.icc;
    out.image.meta = w.meta;
    out.notes = std::move(over_white.notes);
    return out;
}

std::vector<CopyResult> export_artboards(const core::Document& doc, const fs::path& folder, const ArtboardFiles& files) {
    const std::vector<std::size_t> pages = artboard_pages(doc, files.artboards);
    const std::string extension = lower(files.extension);
    const auto exts = copy_extensions();
    if (std::find(exts.begin(), exts.end(), extension) == exts.end()) throw core::Error("value", "not a format this build writes: " + extension);
    std::string prefix = files.prefix ? *files.prefix : (doc.title.empty() ? std::string("無題") : doc.title);
    if (!prefix.empty()) prefix = render::layer_comps::file_part(prefix);  // ("" given: the names alone)
    constexpr std::size_t kNameMost = 240;
    const std::size_t fixed = 1 + 1 + extension.size() + 4;  // (the _, the dot and extension, a _N)
    if (!prefix.empty()) prefix = cut(prefix, kNameMost > fixed + 16 ? kNameMost - fixed - 16 : 1);
    std::set<std::string> taken;
    std::vector<CopyResult> out;
    for (const std::size_t at : pages) {
        const core::Page& page = doc.page(at);
        const std::string part = cut(render::layer_comps::file_part(page.artboard->name), kNameMost - fixed - prefix.size());
        std::string stem = prefix.empty() ? part : prefix + "_" + part;
        if (reserved_name(stem)) stem += "_";  // (CON, NUL, COM1 …: Windows' devices — the review's P3-9)
        std::string name = stem;
        for (int n = 2; !taken.insert(lower(name)).second; ++n) name = stem + "_" + std::to_string(n);
        const fs::path dest = folder / core::path_from_utf8(name + "." + extension);
        if (files.check) files.check(dest);
        out.push_back(write_copy(doc, at, dest, files.options));  // (one artboard's picture and bytes at a time)
    }
    return out;
}

ArtboardPdfResult export_artboards_pdf(const core::Document& doc, const fs::path& file, const ArtboardPdf& pdf) {
    if (pdf.quality < 0 || pdf.quality > 12) throw core::Error("value", "the quality is 0..12");
    const std::vector<std::size_t> pages = artboard_pages(doc, pdf.artboards);
    if (pdf.check) pdf.check(file);
    ArtboardPdfResult result;
    result.file = file;
    std::vector<PdfPage> made;
    for (const std::size_t at : pages) {
        CopyOptions options;
        options.quality = pdf.quality;
        options.srgb = true;  // (a PDF's DeviceRGB: sRGB)
        options.metadata = Metadata::None;
        const PlannedCopy plan = plan_copy(doc, at, file.parent_path() / "artboard.jpg", options);  // (made in memory, written nowhere)
        PdfPage page;
        page.width = plan.width;
        page.height = plan.height;
        page.dpi = plan.dpi;
        page.jpeg = plan.bytes;
        for (const std::string& note : plan.notes) {
            if (std::find(result.notes.begin(), result.notes.end(), note) == result.notes.end()) result.notes.push_back(note);
        }
        made.push_back(std::move(page));
    }
    const std::string bytes = raster_pdf(made);
    if (file.has_parent_path()) detail::make_dirs(file.parent_path());
    detail::PartFile part(file);
    part.write(bytes);
    part.close();
    part.commit();
    result.pages = made.size();
    result.bytes = bytes.size();
    return result;
}

CopyResult export_artboards_psd(const core::Document& doc, const fs::path& file, const ArtboardPsd& psd) {
    const std::vector<std::size_t> pages = artboard_pages(doc, psd.artboards);
    if (psd.check) psd.check(file);
    // (the colour mode and profile alike; the depth the deepest's — RGB pages of sRGB made it on a copy)
    int deepest = 0;
    for (const std::size_t at : pages) {
        if (!same_mode(doc.page(at), doc.page(pages.front()))) {
            throw core::Error("value", "the artboards are not of one colour mode and profile: one PSD holds one (make them alike with Image › Mode, or write a file each)");
        }
        deepest = std::max(deepest, depth_of(doc.page(at)));
    }
    // (where each lies on the canvas: its origin_px, else right of the ones placed, 100 pixels apart)
    struct Place {
        std::int64_t x = 0, y = 0, w = 0, h = 0;
    };
    std::vector<Place> places(pages.size());
    std::optional<std::int64_t> right, top;
    for (std::size_t k = 0; k < pages.size(); ++k) {
        const core::Page& page = doc.page(pages[k]);
        const auto [w, h] = pixels_of(page);
        places[k].w = w;
        places[k].h = h;
        if (page.artboard->origin_px) {
            places[k].x = (*page.artboard->origin_px)[0];
            places[k].y = (*page.artboard->origin_px)[1];
            right = std::max(right.value_or(places[k].x + w), places[k].x + w);
            top = std::min(top.value_or(places[k].y), places[k].y);
        }
    }
    for (std::size_t k = 0; k < pages.size(); ++k) {
        if (doc.page(pages[k]).artboard->origin_px) continue;
        places[k].x = right ? *right + 100 : 0;
        places[k].y = top.value_or(0);
        right = places[k].x + places[k].w;
        top = top.value_or(places[k].y);
    }
    std::int64_t x0 = places.front().x, y0 = places.front().y;
    for (const Place& p : places) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
    }
    std::int64_t width = 0, height = 0;
    for (Place& p : places) {
        p.x -= std::min<std::int64_t>(x0, 0);
        p.y -= std::min<std::int64_t>(y0, 0);
        width = std::max(width, p.x + p.w);
        height = std::max(height, p.y + p.h);
    }
    if (width > core::kArtboardOriginMax || height > core::kArtboardOriginMax) throw core::Error("image_too_large", "the artboards' canvas is too large");
    // (on a copy of the book: each artboard's page grown to the canvas, its contents where it lies)
    core::Document work = doc;
    work.page_locks = Json::object();
    work.strict_gates = false;
    Json ops = Json::array();
    for (std::size_t k = 0; k < pages.size(); ++k) {
        const core::Page& page = doc.page(pages[k]);
        const Json index = page.index.json();
        if (depth_of(page) != deepest) {
            // (an RGB page of sRGB made deeper on the copy; another — a profile of its own, an indexed page, inks — refused)
            if (page.color || (page.rgb && (!page.rgb->profile.empty() || page.rgb->table))) {
                throw core::Error("value", "the artboards are of different depths: one PSD holds one (make them alike with Image › Mode › bits)");
            }
            ops.push_back(Json{{"op", "set_color_mode"}, {"mode", "rgb"}, {"depth", deepest}, {"pages", Json::array({index})}});
        }
        const Json fit{{"trim", "fit"}, {"bleed_mm", 0}, {"margins", Json::array({0, 0, 0, 0})}};
        Json grow{{"op", "set_canvas_size"}, {"page", index}, {"width_px", places[k].x + places[k].w}, {"height_px", places[k].y + places[k].h},
                  {"anchor", "bottom_right"}};
        Json whole{{"op", "set_canvas_size"}, {"page", index}, {"width_px", width}, {"height_px", height}, {"anchor", "top_left"}};
        for (const auto& [k2, v] : fit.items()) {
            grow[k2] = v;
            whole[k2] = v;
        }
        if (places[k].x != 0 || places[k].y != 0) ops.push_back(std::move(grow));
        ops.push_back(std::move(whole));
    }
    try {
        work = core::CommandBus(render::ops_registry()).apply(work, ops, core::Actor("human:genko")).doc;
    } catch (const core::ApplyError& error) {  // (the bus's: an op's refusal, a picture too large — the review's P3-3)
        const std::string why = error.what();
        throw core::Error(why.find("too large") != std::string::npos ? "image_too_large" : "value", why);
    }
    // (one page of them: each artboard's layers in a group of its name carrying its artb block, the first artboard's at
    // the bottom — Photoshop's Layers panel lists the last made on top)
    const std::size_t base_at = pages.front();
    core::Page base = work.page(base_at);
    // (the canvas outside the artboards: no paper of its own — a transparent artboard's page gives none, page_layers —; the
    // file marked as one of artboards this build wrote, so that opening it leaves out the panels' empty layer, the
    // review's P2-3)
    base.artboard = core::Artboard{"_", std::nullopt, "transparent", std::nullopt, std::nullopt};
    {
        const std::string mark(render::psd_artboards::kGenkoMark);
        const std::string mark_ref = core::bytes_ref(mark);
        if (!work.native_files.contains(mark_ref)) work.native_files.emplace(mark_ref, std::make_shared<const std::string>(mark));
        core::PsdPageKeep keep;
        keep.globals.push_back(core::PsdGlobal{"8BIM", std::string(render::psd_artboards::kGenkoMarkKey), mark_ref});
        base.native_psd = std::move(keep);
    }
    std::vector<core::Layer> layers;
    std::set<std::string> ids;
    std::vector<std::string> notes;
    bool left_out = false;
    for (std::size_t k = 0; k < pages.size(); ++k) {
        const core::Page& page = work.page(pages[k]);
        const core::Artboard& artboard = *doc.page(pages[k]).artboard;
        std::map<std::string, std::string> renamed;
        for (const core::Layer& layer : page.layers) {
            std::string id = layer.id;
            while (ids.contains(id)) id = core::new_id();
            ids.insert(id);
            renamed[layer.id] = id;
        }
        core::Layer folder;
        folder.id = core::new_id();
        while (ids.contains(folder.id)) folder.id = core::new_id();
        ids.insert(folder.id);
        folder.role = core::LayerRole::User;
        folder.kind = core::LayerKind::Folder;
        folder.title = artboard.name;
        folder.exportable = true;
        folder.group = true;
        folder.blend = "pass_through";
        const std::string block = render::psd_artboards::write(artboard, {places[k].x, places[k].y, places[k].x + places[k].w, places[k].y + places[k].h});
        const std::string ref = core::bytes_ref(block);
        if (!work.native_files.contains(ref)) work.native_files.emplace(ref, std::make_shared<const std::string>(block));
        core::PsdLayerKeep keep;
        keep.blocks.emplace_back("artb", ref);
        folder.native_psd = std::move(keep);
        for (const core::Layer& layer : page.layers) {
            core::Layer copy = layer;
            copy.id = renamed[layer.id];
            if (copy.parent_id.is_string()) {
                const auto in = renamed.find(copy.parent_id.get<std::string>());
                copy.parent_id = in != renamed.end() ? Json(in->second) : Json(folder.id);
            } else {
                copy.parent_id = folder.id;
            }
            layers.push_back(std::move(copy));
        }
        layers.push_back(std::move(folder));
        if (k > 0) {
            // (its lines onto the one page; what goes on a page alone stays the first artboard's)
            for (core::StoryLine& line : work.story) {
                if (line.page_index == page.index) line.page_index = base.index;
            }
            left_out = left_out || !page.rulers.empty() || !page.annotations.empty() || (page.layer_comps && !page.layer_comps->empty()) ||
                       (page.slices && !page.slices->empty()) || !page.channels.empty() || !page.paths.empty() || page.measure;
        }
    }
    base.layers = std::move(layers);
    if (base.layer_comps) base.layer_comps.reset();  // (its comps name the layers as they were on their page)
    work.pages[base_at] = std::make_shared<core::Page>(std::move(base));
    if (left_out) notes.push_back("ガイド・注釈・レイヤーカンプ・スライス・チャンネル・パスなどページに 1 つのものは、最初のアートボードの分だけを書きました");
    if (file.has_parent_path()) detail::make_dirs(file.parent_path());
    export_page_psd(work, work.page(base_at), file, dpi_of(work.page(base_at)), {}, nullptr, psd.psb);
    CopyResult result;
    result.file = file;
    result.notes = std::move(notes);
    result.width = static_cast<std::uint32_t>(width);
    result.height = static_cast<std::uint32_t>(height);
    std::error_code ec;
    result.bytes = fs::file_size(file, ec);
    return result;
}

namespace {

// A layer outside the artboards that makes no page of its own: one of no pixels at all, or the paper this build writes
// under a page's layers in every PSD (紙: white all over).
bool nothing_or_paper(const core::Layer& layer) {
    if (layer.kind != core::LayerKind::Raster || !layer.raster_png || layer.raster_png->empty() || layer.stroke_count() > 0) return false;
    try {
        const render::Image picture = render::read_png(*layer.raster_png).convert("RGBA");
        if (!picture.getbbox(true)) return true;
        if (layer.title != "紙") return false;
        for (const auto& [lo, hi] : picture.getextrema())
            if (lo < 255) return false;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct Board {
    std::string id;
    render::psd_artboards::Board board;
};

// The page's artboard groups (`boards`, the bottom of the Layers panel first: the order export_artboards_psd writes them
// in, the first page's at the bottom) each made a page: what split_psd_artboards and
// split_pcraft_artboards do once they have found them (`from`: PSD or .pcraft, for the notes).
void split_boards(core::Document& doc, const std::vector<Board>& boards, std::vector<std::string>& notes, std::string_view from, bool ours) {
    const core::Page page = doc.page(0);
    std::set<std::string> board_ids;
    for (const Board& b : boards) board_ids.insert(b.id);
    std::map<std::string, std::string> top;
    for (const core::Layer& layer : page.layers) top[layer.id] = top_of(page, layer.id);
    const double dpi = page.spec.dpi.value();
    const auto mm = [dpi](std::int64_t px) { return static_cast<double>(px) * 25.4 / dpi; };
    // (each a copy of the opened page: its layers those in the board, its own things kept — what names layers over the
    // whole file, and the blocks the file kept, left)
    const auto page_of = [&](const std::vector<core::Layer>& layers) {
        core::Page one = page;
        one.id = core::new_page_id();
        one.layers = layers;
        one.native_psd.reset();
        one.layer_comps.reset();
        one.slices.reset();
        return one;
    };
    std::vector<core::PagePtr> made;
    std::vector<std::optional<std::array<std::int64_t, 4>>> cuts;
    for (const Board& b : boards) {
        std::vector<core::Layer> layers;
        for (const core::Layer& layer : page.layers) {
            if (layer.id == b.id || top[layer.id] != b.id) continue;
            core::Layer copy = layer;
            if (copy.parent_id.is_string() && copy.parent_id.get<std::string>() == b.id) copy.parent_id = nullptr;
            layers.push_back(std::move(copy));
        }
        core::Page one = page_of(layers);
        core::Artboard artboard;
        for (const core::Layer& layer : page.layers) {
            if (layer.id == b.id) artboard.name = layer.title;
        }
        if (artboard.name.empty()) artboard.name = "アートボード " + std::to_string(made.size() + 1);
        if (artboard.name.size() > core::kArtboardNameMax) artboard.name = cut(artboard.name, core::kArtboardNameMax);
        if (!b.board.preset.empty() && b.board.preset.size() <= 256) artboard.preset = b.board.preset;
        artboard.background = b.board.background;
        artboard.rgb = b.board.rgb;
        artboard.origin_px = std::array<std::int64_t, 2>{std::clamp(b.board.rect[0], -core::kArtboardOriginMax, core::kArtboardOriginMax),
                                                         std::clamp(b.board.rect[1], -core::kArtboardOriginMax, core::kArtboardOriginMax)};
        one.artboard = std::move(artboard);
        made.push_back(std::make_shared<core::Page>(std::move(one)));
        cuts.push_back(b.board.rect);
    }
    std::vector<core::Layer> outside;
    std::size_t left_out = 0;
    for (const core::Layer& layer : page.layers) {
        if (board_ids.contains(top[layer.id])) continue;
        // (in a file of artboards this build wrote — its mark — the paper and the panels' empty layer every page's PSD has:
        // not a page of their own, said; another file's layers outside every artboard all kept, the review's P2-3)
        if (ours && nothing_or_paper(layer)) {
            ++left_out;
            continue;
        }
        outside.push_back(layer);
    }
    if (!outside.empty()) {
        core::Page rest = page_of(outside);
        rest.note = "アートボードの外";
        made.push_back(std::make_shared<core::Page>(std::move(rest)));
        cuts.push_back(std::nullopt);
    }
    const bool dropped = (page.layer_comps && !page.layer_comps->empty()) || (page.slices && !page.slices->empty());
    for (std::size_t i = 0; i < made.size(); ++i) {
        core::Page& one = *made[i];
        one.index = core::Num(static_cast<std::int64_t>(i + 1));
        if (i == 0) continue;
        // (the file it was opened from, its notes and measuring: the first page's — the lines, if any, stay on it too)
        one.native_source.reset();
        one.annotations.clear();
        one.measure.reset();
    }
    core::Document split = doc;
    split.pages = made;
    // (each artboard's page cut to its board — crop_page's way: the pixels on their grids, what is on the page with them)
    Json ops = Json::array();
    for (std::size_t i = 0; i < made.size(); ++i) {
        if (!cuts[i]) continue;
        const auto& r = *cuts[i];
        ops.push_back(Json{{"op", "crop_page"},
                           {"page", static_cast<std::int64_t>(i + 1)},
                           {"rect", Json::array({mm(r[0]), mm(r[1]), mm(r[2] - r[0]), mm(r[3] - r[1])})},
                           {"trim", "fit"},
                           {"bleed_mm", 0},
                           {"margins", Json::array({0, 0, 0, 0})}});
    }
    try {
        split = core::CommandBus(render::ops_registry()).apply(split, ops, core::Actor("human:genko")).doc;
    } catch (const std::runtime_error& error) {
        // (a board the pages cannot be cut to — too large, past the limits —: the file opened as one page, as before, said:
        // the review's P3-2)
        notes.push_back(std::string(from) + " のアートボードをページに分けられなかったため、1 ページのまま開きました（" + error.what() + "）");
        return;
    }
    for (std::size_t i = 0; i < split.pages.size(); ++i) {
        if (split.page(i).artboard) split.edit_page(i).own_spec = true;
    }
    doc = std::move(split);
    notes.push_back(std::string(from) + " のアートボード " + std::to_string(boards.size()) + " 枚を、1 枚ずつページにしました");
    if (!outside.empty()) notes.push_back("アートボードの外のレイヤーは、最後のページ（アートボードの外）にしました");
    if (dropped) notes.push_back(std::string(from) + " のレイヤーカンプとスライスは、アートボードをページに分けたため読み込みませんでした");
    if (left_out > 0) notes.push_back("Genko が書いたアートボードの PSD の紙と空のレイヤー（" + std::to_string(left_out) + " 枚）は読み込みませんでした");
}

}  // namespace

void split_psd_artboards(core::Document& doc, std::vector<std::string>& notes) {
    if (doc.pages.size() != 1) return;
    const core::Page& page = doc.page(0);
    std::vector<Board> boards;  // (the bottom of the Layers panel first)
    std::vector<std::string> nested, unread;
    for (const core::Layer& layer : page.layers) {
        if (layer.kind != core::LayerKind::Folder || !layer.native_psd) continue;
        bool has = false, read = false;
        for (const auto& [key, ref] : layer.native_psd->blocks) {
            if (!render::psd_artboards::artboard_key(key)) continue;
            has = true;
            if (layer.parent_id.is_string()) break;
            const auto held = doc.native_files.find(ref);
            if (held == doc.native_files.end() || !held->second) continue;
            if (const auto board = render::psd_artboards::read(*held->second)) {
                boards.push_back(Board{layer.id, *board});
                read = true;
                break;
            }
        }
        if (!has || read) continue;
        (layer.parent_id.is_string() ? nested : unread).push_back(layer.title);
    }
    // (the review's P3-5: an artboard in a group, or one whose block cannot be read, stays a group — said)
    const auto named = [](const std::vector<std::string>& names) {
        std::string out;
        for (const std::string& n : names) out += (out.empty() ? "" : "、") + n;
        return out;
    };
    if (!nested.empty()) notes.push_back("入れ子のアートボードは普通のグループにしました（" + named(nested) + "）");
    if (!unread.empty()) notes.push_back("読めないアートボードは普通のグループにしました（" + named(unread) + "）");
    if (boards.empty()) return;
    const bool ours = page.native_psd && std::any_of(page.native_psd->globals.begin(), page.native_psd->globals.end(), [](const core::PsdGlobal& g) {
                          return g.key == render::psd_artboards::kGenkoMarkKey;
                      });
    split_boards(doc, boards, notes, "PSD", ours);
}

void split_pcraft_artboards(core::Document& doc, const std::vector<std::pair<std::string, Json>>& artboards, std::vector<std::string>& notes) {
    if (doc.pages.size() != 1 || artboards.empty()) return;
    const core::Page& page = doc.page(0);
    // (PhotoCraft's Artboard: {"rect": {x0, y0, x1, y1}, "background": "white" | "black" | "transparent" | {"custom": a
    // colour}, "preset"})
    std::map<std::string, render::psd_artboards::Board> read;
    for (const auto& [id, value] : artboards) {
        try {
            const Json& rect = value.at("rect");
            for (const char* k : {"x0", "y0", "x1", "y1"})
                if (!rect.at(k).is_number_integer()) throw core::Error("value", "a board's place is whole pixels");  // (the review's P3-1)
            render::psd_artboards::Board board;
            board.rect = {rect.at("x0").get<std::int64_t>(), rect.at("y0").get<std::int64_t>(), rect.at("x1").get<std::int64_t>(),
                          rect.at("y1").get<std::int64_t>()};
            if (board.rect[2] <= board.rect[0] || board.rect[3] <= board.rect[1]) throw core::Error("value", "an empty board");
            for (const std::int64_t v : board.rect)
                if (v < -2 * core::kArtboardOriginMax || v > 2 * core::kArtboardOriginMax) throw core::Error("value", "a board out of range");
            if (const auto preset = value.find("preset"); preset != value.end() && preset->is_string() && preset->get<std::string>().size() <= 256)
                board.preset = preset->get<std::string>();
            const Json& background = value.contains("background") ? value.at("background") : Json("white");
            if (background.is_string()) {
                const std::string b = lower(background.get<std::string>());
                if (b == "black" || b == "transparent") board.background = b;
            } else if (background.is_object() && background.contains("custom")) {
                board.background = "custom";
                board.rgb = render::pcraft_annotations::rgb_of_color(render::pcraft::color_from(background.at("custom")));
            }
            read.emplace(id, board);
        } catch (const std::exception&) {
            notes.push_back("読めないアートボードは普通のグループにしました");
        }
    }
    std::vector<Board> boards;  // (the bottom of the Layers panel first)
    for (const core::Layer& layer : page.layers) {
        if (const auto found = read.find(layer.id); found != read.end()) boards.push_back(Board{layer.id, found->second});
    }
    if (boards.empty()) return;
    split_boards(doc, boards, notes, ".pcraft", false);
}

}  // namespace genko::formats
