// A file opened as a book of one page (formats/open_file.hpp).

#include "formats/open_file.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <unordered_map>

#include "core/actor.hpp"
#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/paths.hpp"
#include "core/error.hpp"
#include "core/file_info.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/smart.hpp"
#include "formats/page_picture.hpp"
#include "render/codecs/metadata.hpp"
#include "render/image.hpp"
#include "render/ink.hpp"
#include "render/not_yet_ported.hpp"
#include "render/pcraft_read.hpp"
#include "render/ops_registry.hpp"
#include "render/import_psd.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
#include "render/psd_image.hpp"
#include "render/smart_decode.hpp"

namespace genko::formats {

namespace {

using core::Json;
using render::codecs::CodecImage;
using render::codecs::Layout;
using render::codecs::Sample;

// The name without its folder; its stem (the name less its last extension).
std::string base_name(std::string_view name) {
    const std::size_t cut = name.find_last_of("/\\");
    return std::string(cut == std::string_view::npos ? name : name.substr(cut + 1));
}
std::string stem_of(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}
std::string suffix_of(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return {};
    std::string suffix = name.substr(dot);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return suffix;
}

// A profile the file carries, of the picture's colours (none: it carries none, or one of other colours — said in the notes).
std::optional<std::string> own_profile(const CodecImage& picture, const OpenOptions& options, std::vector<std::string>& notes) {
    if (!picture.icc || picture.icc->empty() || options.profile != "keep") return std::nullopt;
    const auto space = render::ink::profile_space(*picture.icc);
    const bool fits = space && ((*space == core::ProfileSpace::Gray && render::codecs::is_gray(picture.layout())) ||
                                (*space == core::ProfileSpace::Rgb && render::codecs::is_rgb(picture.layout())) ||
                                (*space == core::ProfileSpace::Cmyk && render::codecs::is_cmyk(picture.layout())));
    if (!fits) {
        notes.push_back("埋め込みプロファイルが画像の色と合わないため、作業用のプロファイルにしました");
        return std::nullopt;
    }
    return picture.icc;
}

std::string builtin_bytes(std::string_view id) {
    const core::BuiltinProfile* builtin = core::builtin_profile(id);
    if (builtin == nullptr || !builtin->bytes) throw core::Error("value", "the built-in profile " + std::string(id) + " is not in this build");
    return *builtin->bytes;
}

core::PageColor ink_mode(core::InkSpace space, int depth, const std::string& icc) {
    core::PageColor color;
    color.space = space;
    color.depth = depth;
    color.icc = std::make_shared<const std::string>(icc);
    color.profile = core::bytes_ref(icc);
    return color;
}

// The ops of the colour model a file's pixels are in, applied to the book made of them (never undone: they are how it
// opens).
core::Document applied(const core::Document& doc, const Json& ops) {
    return core::CommandBus(render::ops_registry()).apply(doc, ops, core::Actor("human:genko")).doc;
}

// Bytes kept with the book in memory (written as its assets with their suffix when it is saved), by their asset ref.
std::string keep_bytes(core::Document& doc, std::string bytes) {
    const std::string ref = core::bytes_ref(bytes);
    doc.native_files.emplace(ref, std::make_shared<const std::string>(std::move(bytes)));
    return ref;
}

// Is the page an 8-bit RGB page (none of the colour models that work at their pixels' grid already)?
bool eight_bit_rgb(const core::Page& page) { return !page.color && (!page.rgb || page.rgb->depth == 8); }

// The first page made an indexed page of its own colours, its pixels as they are (no flatten, no resample: the to_indexed
// op flattens a page at its grid, the finest of an 8-bit RGB page, and quantizes what that made). The table: the colours
// of its shown pixels, in the order met, and a slot for the clear ones (white, as to_indexed's) when there are some and
// room for it. False when it holds more than 256 colours (it stays RGB).
bool make_indexed(core::Document& doc) {
    std::vector<std::array<int, 3>> colours;
    std::unordered_map<std::uint32_t, std::size_t> seen;
    bool clear = false;
    for (const core::Layer& layer : doc.page(0).layers) {
        if (!layer.raster_png || layer.raster_png->empty()) continue;
        const std::string bytes = render::open_image(*layer.raster_png).convert("RGBA").tobytes();
        for (std::size_t at = 0; at + 3 < bytes.size(); at += 4) {
            const auto* q = reinterpret_cast<const unsigned char*>(bytes.data() + at);
            if (q[3] == 0) {
                clear = true;
                continue;
            }
            const std::uint32_t key = (std::uint32_t(q[0]) << 16) | (std::uint32_t(q[1]) << 8) | q[2];
            if (seen.count(key) != 0) continue;
            if (colours.size() == core::kColorTableMax) return false;
            seen.emplace(key, colours.size());
            colours.push_back({q[0], q[1], q[2]});
        }
    }
    core::ColorTable table;
    table.colors = std::move(colours);
    if ((clear || table.colors.empty()) && table.colors.size() < core::kColorTableMax) {
        table.colors.push_back({255, 255, 255});
        table.transparent = static_cast<int>(table.colors.size() - 1);
    }
    core::check_color_table(table);
    core::RgbMode mode;
    mode.table = table;
    core::Page& page = doc.edit_page(0);
    page.rgb = mode;
    page.color.reset();
    doc.rgb = std::move(mode);
    doc.color.reset();
    return true;
}

// The first page, a grey page of 8 bits, made a bitmap page of Threshold's look, its layers' pixels 0 or full at 50% over
// white and opaque (to_bitmap's threshold) where they are: the to_bitmap op flattens the page into a layer of another
// name. False when it is not such a page.
bool make_bitmap(core::Document& doc) {
    if (doc.page(0).color == std::nullopt || !doc.color) return false;
    const core::PageColor& was = *doc.page(0).color;
    if (was.space != core::InkSpace::Gray || was.depth != 8 || was.bitmap || !was.duotone.empty()) return false;
    core::Page& page = doc.edit_page(0);
    for (core::Layer& layer : page.layers) {
        if (!layer.ink_raster) continue;
        core::InkPixels pixels = core::decode_ink_raster(*layer.ink_raster);
        for (std::size_t i = 0; i < pixels.pixel_count(); ++i) {
            const double a = std::clamp(pixels.value(i, 1), 0.0, 1.0);
            const double v = std::clamp(pixels.value(i, 0), 0.0, 1.0) * a + (1.0 - a);
            pixels.set(i, 0, v >= 0.5 ? 1.0 : 0.0);
            pixels.set(i, 1, 1.0);
        }
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(pixels));
        layer.ink_memo = {};
    }
    core::BitmapLook look;
    look.method = "threshold";
    page.color->bitmap = look;
    page.color->duotone.clear();
    doc.color->depth = 8;
    doc.color->bitmap = look;
    doc.color->duotone.clear();
    return true;
}

// A book of one page of `w` × `h` pixels at the resolution a file said (72 ppi when it says none or less, as Photoshop
// opens it): its paper the pixels at that resolution, its trim the paper, no bleed, margins or panels, bound on the
// left, no layers yet (picture_book's, and since PC4 F-d a PSD's).
core::Document pixels_book(std::uint32_t w, std::uint32_t h, double said, std::string_view title, bool mono, bool grid) {
    const auto dpi = static_cast<std::int64_t>(std::clamp<double>(std::llround(said), 72, 100000));
    core::PageSpec spec;
    spec.width_mm = core::Num(static_cast<double>(w) * 25.4 / static_cast<double>(dpi));
    spec.height_mm = core::Num(static_cast<double>(h) * 25.4 / static_cast<double>(dpi));
    spec.dpi = core::Num(dpi);
    spec.bleed_mm = core::Num(std::int64_t{0});
    spec.inner_margin_mm = core::Num(std::int64_t{0});
    spec.trim_w_mm = spec.width_mm;
    spec.trim_h_mm = spec.height_mm;
    // (the margins as the reader reads them back: floats)
    spec.margins_mm = core::NumList{core::Num(0.0), core::Num(0.0), core::Num(0.0), core::Num(0.0)};
    spec.expression = mono ? "mono" : "color";
    if (grid) spec.grid = "finest";  // (image-grid@1: design 3, 8-2 (b))
    core::Document doc = core::new_episode(title.empty() ? std::string("無題") : std::string(title), core::Num(std::int64_t{1}), 1, spec,
                                           core::Binding::Left);
    core::Page& page = doc.edit_page(0);
    page.frames.clear();
    page.numero = false;
    page.layers.clear();
    return doc;
}

// (PC4 F-f) A .pcraft read (render/pcraft_read) as the book open_file gives: its page's native_source the file's name.
Opened pcraft_book(render::pcraft::Read read, const std::string& file) {
    Opened out;
    out.doc = std::move(read.doc);
    out.notes = std::move(read.notes);
    core::Page& page = out.doc.edit_page(0);
    if (page.native_source && !file.empty() && file.find('/') == std::string::npos && file.find('\\') == std::string::npos) page.native_source->name = file;
    return out;
}

}  // namespace

Opened open_folder(const std::filesystem::path& folder, const OpenOptions&) {
    std::string name = core::utf8_replaced(core::path_to_utf8(folder.filename()));
    if (name.empty()) name = core::utf8_replaced(core::path_to_utf8(folder.parent_path().filename()));
    return pcraft_book(render::pcraft::read_folder(folder, stem_of(name)), name);
}

Opened picture_book(CodecImage picture, std::string_view title, const OpenOptions& options) {
    if (picture.empty()) throw core::Error("value", "a picture of no pixels cannot be opened");
    options.limits.check(picture.width(), picture.height(), picture.layout(), picture.sample());
    Opened out;
    const std::uint32_t w = picture.width(), h = picture.height();
    const bool alpha = render::codecs::has_alpha(picture.layout());
    const bool gray = render::codecs::is_gray(picture.layout()), cmyk = render::codecs::is_cmyk(picture.layout());
    const bool eight = picture.sample() == Sample::U8;
    // (its resolution: the file's, 72 ppi when it says none or less, as Photoshop opens it)
    const double said = picture.meta.dpi ? picture.meta.dpi->first : 0.0;
    core::Document doc = pixels_book(w, h, said, title, gray, eight && !gray && !cmyk);
    core::Page& page = doc.edit_page(0);
    core::Layer layer;
    layer.id = core::new_id();
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.title = alpha ? "レイヤー 0" : "背景";
    layer.exportable = true;
    layer.panel_clip = false;
    layer.panel_each = false;

    const std::optional<std::string> own = own_profile(picture, options, out.notes);
    if (gray || cmyk) {
        // (a page of inks: grey 8, 16 or 32 bits — a half float's values as floats —, CMYK 8, 16 or 32)
        const int depth = picture.sample() == Sample::U8 ? 8 : picture.sample() == Sample::U16 ? 16 : 32;
        const std::string icc = own ? *own
                                    : gray ? (options.working_gray.empty() ? builtin_bytes("sgray") : options.working_gray)
                                           : (options.working_cmyk.empty() ? builtin_bytes("coated-cmyk") : options.working_cmyk);
        const core::PageColor color = ink_mode(gray ? core::InkSpace::Gray : core::InkSpace::Cmyk, depth, icc);
        core::InkPixels inks = core::blank_ink_pixels(color.space, depth, w, h);
        const unsigned colours = render::codecs::colour_channels(picture.layout()), all = render::codecs::channels(picture.layout());
        for (std::size_t i = 0; i < inks.pixel_count(); ++i) {
            for (unsigned c = 0; c < colours; ++c) inks.set(i, c, picture.value(i * all + c));
            inks.set(i, colours, alpha ? picture.value(i * all + colours) : 1.0);
        }
        layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(inks));
        page.color = color;
        doc.color = color;
    } else {
        std::optional<core::RgbMode> mode;
        const int depth = picture.sample() == Sample::U8 ? 8 : picture.sample() == Sample::U16 ? 16 : 32;
        // (the file's own profile — one that shows its numbers as sRGB does is the page's without a profile, Little CMS
        // telling by its colours —; a file that carries none takes the working RGB, an sRGB one being none either)
        const std::string chosen = own ? *own : options.working_rgb;
        const std::string icc = chosen.empty() || render::proof::same_as_srgb(chosen) ? std::string() : chosen;
        if (depth != 8 || !icc.empty()) {
            mode = core::RgbMode{};
            mode->depth = depth;
            if (!icc.empty()) {
                mode->icc = std::make_shared<const std::string>(icc);
                mode->profile = core::bytes_ref(icc);
            }
        }
        if (depth == 8) {
            const CodecImage rgba = picture.convert(Layout::Rgba, Sample::U8);
            const render::Image image = render::Image::frombytes("RGBA", render::Size{static_cast<int>(w), static_cast<int>(h)}, rgba.data());
            layer.raster_png = std::make_shared<const std::string>(render::write_png(image));
        } else {
            const unsigned all = render::codecs::channels(picture.layout());
            layer.color_raster = std::make_shared<const std::string>(core::encode_color_pixels(w, h, depth == 16 ? "u16" : "f32", [&](std::size_t i) {
                return std::array<double, 4>{picture.value(i * all), picture.value(i * all + 1), picture.value(i * all + 2),
                                             alpha ? std::clamp(static_cast<double>(picture.value(i * all + 3)), 0.0, 1.0) : 1.0};
            }));
        }
        page.rgb = mode;
        doc.rgb = mode;
    }
    page.layers = {std::move(layer)};
    out.doc = std::move(doc);
    return out;
}

namespace {

// --- a PSD or PSB as a book (PC4 F-d) -----------------------------------------------------------------------------

// What a PSD's header and image resources say (the layers are import_psd's to read): its version, size, channels, depth
// and mode; its resolution, profile, XMP and EXIF.
struct PsdHead {
    int version = 1;
    std::uint32_t width = 0, height = 0;
    unsigned channels = 0;
    int depth = 8;
    int mode = 3;
    double dpi = 0;
    std::string icc, xmp, exif;
};

std::uint32_t be32_at(std::string_view d, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | static_cast<unsigned char>(d[at + k]);
    return v;
}
unsigned be16_at(std::string_view d, std::size_t at) { return (static_cast<unsigned>(static_cast<unsigned char>(d[at])) << 8) | static_cast<unsigned char>(d[at + 1]); }

PsdHead psd_head(std::string_view d) {
    if (d.size() < 30) throw core::Error("format", "the PSD cannot be read (the PSD file is cut short)");
    PsdHead head;
    head.version = static_cast<int>(be16_at(d, 4));
    if (head.version != 1 && head.version != 2) throw core::Error("format", "the PSD cannot be read (unknown PSD version)");
    head.channels = be16_at(d, 12);
    head.height = be32_at(d, 14);
    head.width = be32_at(d, 18);
    head.depth = static_cast<int>(be16_at(d, 22));
    head.mode = static_cast<int>(be16_at(d, 24));
    std::size_t at = 26;
    const std::uint32_t colour_data = be32_at(d, at);
    if (colour_data > d.size() - at - 4) throw core::Error("format", "the PSD cannot be read (the PSD file is cut short)");
    at += 4 + colour_data;
    if (d.size() - at < 4) throw core::Error("format", "the PSD cannot be read (the PSD file is cut short)");
    const std::uint32_t length = be32_at(d, at);
    at += 4;
    const std::size_t end = length > d.size() - at ? d.size() : at + length;
    while (end - at >= 12 && d.substr(at, 4) == "8BIM") {
        const unsigned id = be16_at(d, at + 4);
        const std::size_t name = static_cast<unsigned char>(d[at + 6]);
        std::size_t p = at + 6 + name + 1 + ((name + 1) % 2);  // (a Pascal name, padded to even)
        if (p + 4 > end) break;
        const std::uint32_t size = be32_at(d, p);
        p += 4;
        if (size > end - p) break;
        const std::string_view data = d.substr(p, size);
        if (id == 1005 && size >= 4) {
            const std::uint32_t fixed = be32_at(data, 0);
            head.dpi = fixed / 65536.0;
        }
        if (id == 1039) head.icc = std::string(data);
        if (id == 1060) head.xmp = std::string(data);
        if (id == 1058) head.exif = std::string(data);
        at = p + size + size % 2;
    }
    return head;
}

// A PSD or PSB as a book of one page (formats/open_file.hpp): the page of its pixels at its resolution in its mode and
// depth — RGB 8 bits an 8-bit page (its grid the finest), 16 and 32 bits a page in precise colour; grey (and duotone, as
// grey) and CMYK and Lab a page of inks of its depth (CMYK and Lab at most 16); bitmap a bitmap page; indexed and
// multichannel its merged picture as an opened picture is (an indexed page of its colours) —, in its own profile (else the
// working one's), and its layers as import_psd reads them onto it, exactly (the file's pixels; at their depth; masks,
// folders, adjustment, fill, shape, text and placed layers as import_psd makes them; what this build does not read kept
// to be written back, psd-keep@1).
Opened open_psd(std::string_view bytes, const std::string& file, const OpenOptions& options) {
    const PsdHead head = psd_head(bytes);
    const std::string title = stem_of(file);
    std::vector<std::string> notes;
    if (head.width == 0 || head.height == 0) throw core::Error("format", "the PSD cannot be read (it has no pixels)");
    options.limits.check(head.width, head.height, Layout::Rgba, head.depth == 32 ? Sample::F32 : head.depth > 8 ? Sample::U16 : Sample::U8);
    Opened out;
    if (head.mode == 2 || head.mode == 7) {
        // (indexed and multichannel: the merged picture, as Pillow reads it — an indexed page of its colours)
        if (head.version != 1) throw core::Error("format", "the PSD cannot be read (an indexed or multichannel PSB)");
        const render::Image image = render::read_psd_image(bytes);
        const bool grey = image.mode() == "L" || image.mode() == "1";
        const render::Image shown = image.convert(grey ? "L" : "RGBA");
        CodecImage picture = CodecImage::blank(head.width, head.height, grey ? Layout::Gray : Layout::Rgba, Sample::U8);
        picture.data() = shown.tobytes();
        if (head.dpi > 0) picture.meta.dpi = std::make_pair(head.dpi, head.dpi);
        out = picture_book(std::move(picture), title, options);
        if (head.mode == 2 && eight_bit_rgb(out.doc.page(0)) && !make_indexed(out.doc)) out.notes.push_back("カラーテーブルの色のままでは開けなかったため、RGB で開きました");
        if (head.mode == 7) out.notes.push_back("マルチチャンネルの PSD は、統合された画像をグレースケールで開きました");
    } else {
        const bool rgb = head.mode == 3;
        const bool grey = head.mode == 0 || head.mode == 1 || head.mode == 8;
        const bool cmyk = head.mode == 4, lab = head.mode == 9;
        if (!rgb && !grey && !cmyk && !lab) throw core::Error("format", "the PSD cannot be read (a colour mode this build does not open: " + std::to_string(head.mode) + ")");
        int depth = head.depth == 1 ? 8 : head.depth;
        if ((cmyk || lab) && depth == 32) depth = 16;  // (Photoshop has no 32-bit CMYK or Lab)
        if (depth != 8 && depth != 16 && depth != 32) throw core::Error("format", "the PSD cannot be read (PSD depth " + std::to_string(head.depth) + " is not supported)");
        core::Document doc = pixels_book(head.width, head.height, head.dpi, title, grey, rgb && depth == 8);
        // (its colours: its own profile where it is one of its colours, else the working one's — as an opened picture's)
        const auto space = head.icc.empty() ? std::nullopt : render::ink::profile_space(head.icc);
        const bool own = options.profile == "keep" && space &&
                         ((rgb && *space == core::ProfileSpace::Rgb) || (grey && *space == core::ProfileSpace::Gray) || (cmyk && *space == core::ProfileSpace::Cmyk));
        if (!head.icc.empty() && options.profile == "keep" && !own && !lab) notes.push_back("埋め込みプロファイルが画像の色と合わないため、作業用のプロファイルにしました");
        out.profile_missing = !lab && !own;
        if (rgb) {
            const std::string chosen = own ? head.icc : options.working_rgb;
            std::string icc = chosen.empty() || render::proof::same_as_srgb(chosen) ? std::string() : chosen;
            // (the PC4 F-d review: a 32-bit file's values are linear light, as Photoshop's 32 bits are — in sRGB's colours
            // (its profile sRGB's, or none) the page is the built-in linear-srgb's and they come in as they are, as an
            // OpenEXR's do; in another profile's the page is that profile's and they are encoded by 1/2.2, as this build
            // reads 32 bits elsewhere)
            if (depth == 32 && icc.empty()) icc = builtin_bytes("linear-srgb");
            if (depth != 8 || !icc.empty()) {
                core::RgbMode mode;
                mode.depth = depth;
                if (!icc.empty()) {
                    mode.icc = std::make_shared<const std::string>(icc);
                    mode.profile = core::bytes_ref(icc);
                }
                doc.edit_page(0).rgb = mode;
                doc.rgb = mode;
            }
        } else if (lab) {
            try {
                doc = applied(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "lab"}, {"depth", depth}, {"default", true}}}));
            } catch (const core::ApplyError& error) {
                throw core::Error("format", error.what());
            }
        } else {
            const std::string icc = own ? head.icc
                                        : grey ? (options.working_gray.empty() ? builtin_bytes("sgray") : options.working_gray)
                                               : (options.working_cmyk.empty() ? builtin_bytes("coated-cmyk") : options.working_cmyk);
            const core::PageColor color = ink_mode(grey ? core::InkSpace::Gray : core::InkSpace::Cmyk, depth, icc);
            doc.edit_page(0).color = color;
            doc.color = color;
        }
        // its layers, as import_psd reads them onto a page of its pixels (a flat file's merged picture its 背景, or with a
        // transparency its レイヤー 0)
        const unsigned colours = cmyk ? 4 : rgb || lab ? 3 : 1;
        const Json op{{"op", "import_psd"}, {"page", 1}, {"fit", "paper"}, {"name", head.channels > colours ? "レイヤー 0" : "背景"}};
        Json report;
        try {
            report = render::import_psd_data(doc, op, std::string(bytes), true);
            core::validate_color_document(doc);  // (the book as an op leaves it: its precise colour within its budget)
        } catch (const render::NotYetPorted&) {
            throw;
        } catch (const std::runtime_error& error) {  // (core::Error, and import_psd's OpError and OpKeyError: the PC4 F-d review)
            const std::string why = error.what();
            if (why.find("too large") != std::string::npos || why.find("too many layers") != std::string::npos ||
                why.find("budget exceeded") != std::string::npos) {
                throw core::Error("image_too_large", why);
            }
            throw core::Error("format", why);
        }
        if (head.mode == 0 && !make_bitmap(doc)) notes.push_back("2 階調のままでは開けなかったため、グレースケールで開きました");
        if (head.mode == 8) notes.push_back("ダブルトーンの PSD はグレースケールで開きました");
        if (head.depth == 32 && (cmyk || lab)) notes.push_back("32 bit の CMYK・Lab は 16 bit で開きました");
        if (report.contains("effects")) notes.push_back("レイヤー効果は Genko では描きません（PSD に書き戻すと Photoshop が描きます）");
        if (report.contains("skipped") && !report["skipped"].empty()) {
            std::string names;
            for (const Json& n : report["skipped"]) names += (names.empty() ? "" : "、") + (n.is_string() ? n.get<std::string>() : std::string());
            notes.push_back("この版で読めないレイヤーは開きませんでした（" + names + "）");
        }
        if (report.contains("psd_blocks_cut")) notes.push_back("PSD の塊の一部は、多すぎるか大きすぎるため保てませんでした");
        if (report.contains("in_8_bits")) {
            const std::string why = report["in_8_bits"].is_string() ? report["in_8_bits"].get<std::string>() : std::string();
            notes.push_back(why == "budget" ? "高精度の画素が本の上限（1 GiB）を越えるため、レイヤーを 8 bit で読みました"
                            : why == "mode" ? "ファイルのプロファイルがページと違うため、レイヤーを 8 bit で読みました"
                                            : "レイヤーを 8 bit で読みました");
        }
        out.doc = std::move(doc);
        for (std::string& note : notes) out.notes.push_back(std::move(note));
    }
    // what it was opened from, and its XMP's File Info
    core::Document& doc = out.doc;
    static const std::map<int, std::string> kModes{{0, "bitmap"}, {1, "gray"}, {2, "indexed"}, {3, "rgb"}, {4, "cmyk"}, {7, "multichannel"}, {8, "duotone"}, {9, "lab"}};
    core::NativeSource source;
    source.format = head.version == 2 ? "psb" : "psd";
    source.name = file.empty() ? std::string("image") : file;
    if (source.name.find('/') != std::string::npos || source.name.find('\\') != std::string::npos) source.name = "image";
    source.width_px = head.width;
    source.height_px = head.height;
    if (head.dpi > 0 && head.dpi <= 1000000) source.dpi = std::make_pair(head.dpi, head.dpi);
    source.depth = head.depth == 1 || head.depth == 8 || head.depth == 16 || head.depth == 32 ? head.depth : 8;
    source.mode = kModes.contains(head.mode) ? kModes.at(head.mode) : std::string("rgb");
    if (!head.exif.empty()) source.exif = keep_bytes(doc, head.exif);
    if (!head.xmp.empty()) {
        source.xmp = keep_bytes(doc, head.xmp);
        core::FileInfo info = core::file_info_from_xmp(head.xmp);
        info.xmp = source.xmp;
        try {
            core::check_file_info(info);
        } catch (const core::Error&) {
            info = core::FileInfo{};
            info.xmp = source.xmp;
        }
        doc.file_info = info;
    }
    doc.edit_page(0).native_source = std::move(source);
    return out;
}

}  // namespace

Opened open_file(std::string_view bytes, std::string_view name, const OpenOptions& options) {
    const std::string file = core::utf8_replaced(base_name(name));  // (a name not UTF-8: its bytes as U+FFFD — the book's title and source)
    const std::string suffix = suffix_of(file);
    // (F-d: a PSD or PSB as a book of its layers; F-e: a camera RAW file through Camera Raw — a TIFF-shaped RAW is not read
    // as its TIFF —; F-f: a .pcraft)
    if (bytes.substr(0, 4) == "8BPS") return open_psd(bytes, file, options);  // (PC4 F-d)
    if (const auto later = core::smart::later_format_of_suffix(suffix); later && *later == "raw") throw render::NotYetPorted("raw_file");
    // (a .pcraft by what it holds — a zip whose first file is manifest.json —, or a zip named so; a picture named .pcraft is
    // opened as the picture it is)
    if (render::pcraft::is_pcraft(bytes) || (suffix == ".pcraft" && bytes.starts_with(std::string_view("PK", 2)))) {
        return pcraft_book(render::pcraft::read(bytes, stem_of(file)), file);
    }
    const auto format = options.as ? options.as : render::codecs::detect(bytes);
    if (!format) throw core::Error("unidentified_image", "the file is not a picture of a format this build knows");
    CodecImage picture = render::codecs::decode_as(*format, bytes, options.limits);
    if (render::codecs::linear_light(*format) && render::codecs::is_gray(picture.layout())) {
        // (PC4 F-a: an OpenEXR's or Radiance HDR's grey linear light — no built-in grey is linear —: with sRGB's curve, in
        // the built-in sgray, which has it)
        picture = render::codecs::srgb_encoded(picture);
        picture.icc = builtin_bytes("sgray");
    }
    const render::codecs::FileMetadata meta = render::codecs::read_metadata(*format, bytes);
    std::vector<std::string> notes;
    // (PC4 F-a: what the codecs read with the pixels where the containers' reader reads none — an AVIF's, by libavif)
    std::optional<std::string> exif = meta.exif ? meta.exif : picture.meta.exif, xmp = meta.xmp ? meta.xmp : picture.meta.xmp;
    // (the EXIF's orientation applied to the pixels, and the EXIF and XMP kept upright: as Photoshop opens it. A TIFF's
    // reader turns its pixels itself.)
    if (exif && *format != render::codecs::Format::Tiff) {
        if (const int orientation = render::codecs::exif_orientation(*exif); orientation != 1) {
            picture = render::codecs::oriented(picture, orientation);
            exif = render::codecs::exif_upright(*exif);
            if (xmp) xmp = render::codecs::xmp_upright(*xmp);
            notes.push_back("EXIF の向きのとおりに回転して開きました");
        }
    }
    // (a TIFF's own orientation, tag 274: its 8-bit reader turns the pixels; a 16- or 32-bit TIFF's reader does not —
    // turned here, as Photoshop opens it)
    if (*format == render::codecs::Format::Tiff && render::smart::stored_depth(bytes) > 8) {
        if (const int orientation = render::codecs::exif_orientation(bytes); orientation != 1) {
            picture = render::codecs::oriented(picture, orientation);
            if (xmp) xmp = render::codecs::xmp_upright(*xmp);
            notes.push_back("TIFF の向きのとおりに回転して開きました");
        }
    }
    // (no profile and an EXIF that says sRGB: sRGB, not the working RGB — unless the EXIF's profile tag is ignored)
    OpenOptions as_said = options;
    const bool profiled = picture.icc && !picture.icc->empty();
    bool exif_srgb = false;
    if (!profiled && exif && !options.ignore_exif_profile && render::codecs::is_rgb(picture.layout()) && render::codecs::exif_colour_space(*exif) == 1) {
        as_said.working_rgb.clear();
        exif_srgb = true;
    }
    if (meta.frames > 1) {
        notes.push_back(*format == render::codecs::Format::Tiff ? "複数ページの TIFF の最初のページだけを開きました（" + std::to_string(meta.frames) + " ページ）"
                                                                : "アニメーションの最初のコマだけを開きました（" + std::to_string(meta.frames) + " コマ）");
    }
    const std::string mode(render::codecs::to_string(picture.layout()));
    const int depth = render::codecs::depth_of(picture.sample());
    core::NativeSource source;
    source.format = std::string(render::codecs::id_of(*format));
    source.name = file.empty() ? std::string("image") : file;
    if (source.name.find('/') != std::string::npos || source.name.find('\\') != std::string::npos) source.name = "image";
    source.width_px = picture.width();
    source.height_px = picture.height();
    source.dpi = picture.meta.dpi;
    source.depth = meta.one_bit ? 1 : depth;
    source.mode = meta.palette ? "indexed" : meta.one_bit ? "bitmap" : mode;
    Opened out = picture_book(std::move(picture), stem_of(file), as_said);
    for (std::string& note : notes) out.notes.push_back(std::move(note));
    out.profile_missing = !profiled && !exif_srgb && !meta.palette;  // (a colour table's page is sRGB's: nothing to ask)
    core::Document& doc = out.doc;
    // (the colour model of its pixels: a colour table's exact colours, one bit's black and white — its pixels as they are)
    if (meta.palette && eight_bit_rgb(doc.page(0))) {
        if (!make_indexed(doc)) out.notes.push_back("カラーテーブルの色のままでは開けなかったため、RGB で開きました");
    } else if (meta.one_bit && !make_bitmap(doc)) {
        out.notes.push_back("2 階調のままでは開けなかったため、グレースケールで開きました");
    }
    if (exif) source.exif = keep_bytes(doc, *exif);
    if (xmp) {
        source.xmp = keep_bytes(doc, *xmp);
        // (its File Info: the book's, the packet kept with it for the items File Info does not edit)
        core::FileInfo info = core::file_info_from_xmp(*xmp);
        info.xmp = source.xmp;
        try {
            core::check_file_info(info);
        } catch (const core::Error&) {
            info = core::FileInfo{};  // (items too long or too many: only the packet)
            info.xmp = source.xmp;
        }
        doc.file_info = info;
    }
    doc.edit_page(0).native_source = std::move(source);
    return out;
}

Opened new_image(const NewImage& image, const OpenOptions& options) {
    static constexpr std::string_view kModes[] = {"rgb", "gray", "cmyk", "lab", "bitmap"};
    if (std::find(std::begin(kModes), std::end(kModes), image.mode) == std::end(kModes)) throw core::Error("value", "the mode is rgb, gray, cmyk, lab or bitmap");
    const bool depth_ok = image.mode == "bitmap"                             ? image.depth == 8
                          : image.mode == "cmyk" || image.mode == "lab"      ? image.depth == 8 || image.depth == 16
                                                                             : image.depth == 8 || image.depth == 16 || image.depth == 32;
    if (!depth_ok) throw core::Error("value", "that depth is not one of the mode's (rgb and gray 8, 16 or 32; cmyk and lab 8 or 16; bitmap 8)");
    if (image.width < 1 || image.height < 1 || image.width > options.limits.max_width || image.height > options.limits.max_height)
        throw core::Error("value", "the width and the height are 1 to " + std::to_string(options.limits.max_width) + " pixels");
    if (!std::isfinite(image.dpi) || image.dpi < 72 || image.dpi > 100000) throw core::Error("value", "the resolution is 72 to 100000 ppi");
    const auto w = static_cast<std::uint32_t>(image.width), h = static_cast<std::uint32_t>(image.height);
    options.limits.check(w, h, Layout::Rgba, Sample::U8);
    // the background: an 8-bit sRGB picture of it (a page of another mode is made of it by set_color_mode, its colour kept)
    const bool clear = image.background == "transparent";
    std::array<unsigned char, 3> rgb{255, 255, 255};
    if (image.background == "black") {
        rgb = {0, 0, 0};
    } else if (image.background.size() == 7 && image.background[0] == '#') {
        for (int c = 0; c < 3; ++c) {
            const std::string hex = image.background.substr(1 + c * 2, 2);
            if (!std::all_of(hex.begin(), hex.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
                throw core::Error("value", "the background is white, black, transparent or #rrggbb");
            rgb[static_cast<std::size_t>(c)] = static_cast<unsigned char>(std::stoi(hex, nullptr, 16));
        }
    } else if (image.background != "white" && !clear) {
        throw core::Error("value", "the background is white, black, transparent or #rrggbb");
    }
    CodecImage picture = CodecImage::blank(w, h, clear ? Layout::Rgba : Layout::Rgb, Sample::U8);
    if (!clear) {
        char* p = picture.data().data();
        for (std::size_t i = 0; i < picture.pixel_count(); ++i) {
            for (int c = 0; c < 3; ++c) p[i * 3 + static_cast<std::size_t>(c)] = static_cast<char>(rgb[static_cast<std::size_t>(c)]);
        }
    }
    picture.meta.dpi = std::make_pair(image.dpi, image.dpi);
    OpenOptions srgb = options;
    srgb.working_rgb.clear();  // (the background's colours are sRGB's: the mode and profile come after)
    Opened out = picture_book(std::move(picture), image.title, srgb);
    core::Document& doc = out.doc;
    if (clear) doc.edit_page(0).layers[0].title = "レイヤー 1";  // (Photoshop's: a new transparent image's one layer)
    // (its mode, depth and profile: the working space's unless one is given)
    const std::string space = image.mode == "bitmap" ? "gray" : image.mode;
    const std::string& working = space == "rgb" ? options.working_rgb : space == "gray" ? options.working_gray : space == "cmyk" ? options.working_cmyk : std::string();
    const bool srgb_page = space == "rgb" && image.depth == 8 && image.profile.empty() && (working.empty() || render::proof::same_as_srgb(working));
    if (!srgb_page) {
        Json op{{"op", "set_color_mode"}, {"page", 1}, {"mode", space}, {"depth", image.mode == "bitmap" ? 8 : image.depth}, {"default", true}};
        if (!image.profile.empty()) {
            op["profile"] = image.profile;
        } else if (!working.empty() && space != "lab") {
            op["icc"] = core::b64encode(working);
        } else if (space == "cmyk") {
            op["profile"] = "coated-cmyk";
        } else if (space == "gray") {
            op["profile"] = "sgray";
        }
        try {
            doc = applied(doc, Json::array({op}));
            if (image.mode == "bitmap" && !make_bitmap(doc)) throw core::Error("value", "the new image could not be made a bitmap image");
        } catch (const core::ApplyError& error) {
            throw core::Error("value", error.what());
        }
    }
    if (!eight_bit_rgb(doc.page(0))) {  // (a page of another model works at its finest grid already: no mark)
        doc.spec.grid.reset();
        doc.edit_page(0).spec.grid.reset();
    }
    return out;
}

Opened duplicate_page(const core::Document& doc, std::size_t at, std::string_view title, bool merged_only, const OpenOptions& options) {
    if (at >= doc.pages.size()) throw core::Error("value", "no such page");
    if (doc.is_deferred(at)) throw core::Error("page_not_loaded", "the page is not read yet");
    const core::Page& page = doc.page(at);
    const std::string name = title.empty() ? doc.title + " のコピー" : std::string(title);
    if (merged_only) {
        const int dpi = static_cast<int>(std::lround(page.spec.dpi.value()));
        PagePicture merged = page_picture(page, doc, dpi);
        OpenOptions as_is = options;
        as_is.working_rgb.clear();  // (the picture's own profile, or sRGB: never the working one assigned to it)
        Opened out = picture_book(std::move(merged.image), name, as_is);
        for (std::string& note : merged.notes) out.notes.push_back(std::move(note));
        if (!eight_bit_rgb(out.doc.page(0))) {
            out.doc.spec.grid.reset();
            out.doc.edit_page(0).spec.grid.reset();
        }
        return out;
    }
    Opened out;
    core::Document copy = doc;
    copy.title = name;
    copy.book_id = core::new_book_id();
    copy.revision = 0;
    copy.read_only_reason.clear();
    copy.deferred.clear();
    // (its features stay: the writer names again those its content uses, and keeps the others it does not derive)
    copy.tickets = Json::array();
    copy.studio = Json::object();
    copy.strict_gates = false;
    copy.page_locks = Json::object();
    copy.start_side.reset();
    auto one = std::make_shared<core::Page>(page);
    const core::Num was = page.index;
    one->index = core::Num(std::int64_t{1});
    one->spread_with.reset();
    copy.pages = {one};
    std::vector<core::StoryLine> story;
    for (const core::StoryLine& line : doc.story) {
        if (line.page_index == was) {
            story.push_back(line);
            story.back().page_index = core::Num(std::int64_t{1});
        }
    }
    copy.story = std::move(story);
    out.doc = std::move(copy);
    return out;
}

}  // namespace genko::formats
