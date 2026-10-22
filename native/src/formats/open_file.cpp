// A file opened as a book of one page (formats/open_file.hpp).

#include "formats/open_file.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

#include "core/actor.hpp"
#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
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
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "render/proof.hpp"
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

}  // namespace

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
    spec.expression = gray ? "mono" : "color";
    if (eight && !gray && !cmyk) spec.grid = "finest";  // (image-grid@1: design 3, 8-2 (b))

    core::Document doc = core::new_episode(title.empty() ? std::string("無題") : std::string(title), core::Num(std::int64_t{1}), 1, spec,
                                           core::Binding::Left);
    core::Page& page = doc.edit_page(0);
    page.frames.clear();
    page.numero = false;
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

Opened open_file(std::string_view bytes, std::string_view name, const OpenOptions& options) {
    const std::string file = core::utf8_replaced(base_name(name));  // (a name not UTF-8: its bytes as U+FFFD — the book's title and source)
    const std::string suffix = suffix_of(file);
    // (F-d: a PSD or PSB as a book of its layers; F-e: a camera RAW file through Camera Raw — a TIFF-shaped RAW is not read
    // as its TIFF —; F-f: a .pcraft)
    if (bytes.substr(0, 4) == "8BPS") throw render::NotYetPorted("psd_file");
    if (const auto later = core::smart::later_format_of_suffix(suffix); later && *later == "raw") throw render::NotYetPorted("raw_file");
    if (suffix == ".pcraft") throw render::NotYetPorted("pcraft_file");
    const auto format = options.as ? options.as : render::codecs::detect(bytes);
    if (!format) throw core::Error("unidentified_image", "the file is not a picture of a format this build knows");
    CodecImage picture = render::codecs::decode_as(*format, bytes, options.limits);
    const render::codecs::FileMetadata meta = render::codecs::read_metadata(*format, bytes);
    std::vector<std::string> notes;
    std::optional<std::string> exif = meta.exif, xmp = meta.xmp;
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
