// The print and e-book exports: Python's genko/export.py.

#include "formats/export.hpp"

#include <QDateTime>

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "formats/output.hpp"
#include "formats/pdf.hpp"
#include "formats/pillow_save.hpp"
#include "render/colour.hpp"
#include "render/covers.hpp"
#include "render/page.hpp"
#include "render/tones.hpp"
#include "render/zip.hpp"
#include "storage/fsutil.hpp"

namespace genko::formats {

using render::Image;

namespace {

// The code points of UTF-8 text (as Python's str iterates it).
std::vector<std::string> characters(std::string_view text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lead = static_cast<unsigned char>(text[at]);
        const std::size_t n = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        out.emplace_back(text.substr(at, n));
        at += n;
    }
    return out;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

render::RenderOptions print_options() {
    render::RenderOptions options;
    options.mode = "print";
    return options;
}

// xml.sax.saxutils.escape: & < >
std::string escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out += c;
    }
    return out;
}

std::array<double, 2> dpi_pair(int dpi) { return {static_cast<double>(dpi), static_cast<double>(dpi)}; }

// n // 2
int floor_half(int n) { return n >= 0 ? n / 2 : -((-n + 1) / 2); }

}  // namespace

namespace detail {

std::string padded(const core::Num& value, std::size_t width) {
    if (!value.is_int()) throw core::PyValueError("Unknown format code 'd' for object of type 'float'");
    const std::int64_t v = value.int_value();
    std::string digits = std::to_string(v < 0 ? -static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v));
    const std::size_t room = v < 0 ? width - 1 : width;
    if (width > 0 && digits.size() < room) digits.insert(0, room - digits.size(), '0');
    return (v < 0 ? "-" : "") + digits;
}

fs::path join(const fs::path& dest, std::string_view name) {
    const std::string text = core::path_to_utf8(dest);
    if (text.empty() || text == ".") return core::path_from_utf8(name);
    return dest / core::path_from_utf8(name);
}

std::string suffix(const fs::path& path) {
    const std::string name = core::path_to_utf8(path.filename());
    const std::size_t i = name.rfind('.');
    if (i != std::string::npos && i > 0 && i < name.size() - 1) return name.substr(i);
    return {};
}

fs::path with_suffix(const fs::path& path, std::string_view new_suffix) {
    const std::string name = core::path_to_utf8(path.filename());
    const std::string old = suffix(path);
    const std::string renamed = name.substr(0, name.size() - old.size()) + std::string(new_suffix);
    return path.has_parent_path() ? path.parent_path() / core::path_from_utf8(renamed) : core::path_from_utf8(renamed);
}

std::string native_text(std::string text) {
#ifdef _WIN32
    // (Path.write_text writes in text mode: "\n" becomes "\r\n" on Windows)
    for (std::size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 2)) text.insert(at, 1, '\r');
#endif
    return text;
}

}  // namespace detail

std::string safe_name(std::string_view text, std::string_view fallback) {
    static const std::string kBad = "<>:\"/\\|?*";
    std::string out;
    for (const std::string& ch : characters(text)) {
        const bool bad = ch.size() == 1 && (kBad.find(ch[0]) != std::string::npos || static_cast<unsigned char>(ch[0]) < 32);
        out += bad ? std::string("_") : ch;
    }
    out = core::py_strip(out);
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty()) out = std::string(fallback);
    std::string first = out.substr(0, out.find('.'));
    for (char& c : first) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const std::vector<std::string> kReserved = [] {
        std::vector<std::string> r{"CON", "PRN", "AUX", "NUL"};
        for (int i = 1; i <= 9; ++i) r.push_back("COM" + std::to_string(i));
        for (int i = 1; i <= 9; ++i) r.push_back("LPT" + std::to_string(i));
        return r;
    }();
    if (std::find(kReserved.begin(), kReserved.end(), first) != kReserved.end()) out = "_" + out;
    const auto chars = characters(out);
    if (chars.size() > 80) {
        std::string cut;
        for (std::size_t i = 0; i < 80; ++i) cut += chars[i];
        out = cut;
    }
    return out;
}

std::string stem(const core::Document& episode) { return safe_name(episode.title) + "_ep" + detail::padded(episode.episode, 2); }

Image crop_to(const Image& image, const core::Page& page, std::string_view area, int dpi) {
    if (area == "paper") return image;
    const core::Rect r = area == "bleed" ? page.bleed_rect_mm() : page.trim_rect_mm();
    const int x0 = render::mm_to_px(r.x.value(), dpi);
    const int y0 = render::mm_to_px(r.y.value(), dpi);
    return image.crop(render::Box{x0, y0, x0 + render::mm_to_px(r.width.value(), dpi), y0 + render::mm_to_px(r.height.value(), dpi)});
}

std::string page_color(const core::Page& page, std::string_view color) {
    if (color != "auto") return std::string(color);
    return page.spec.expression == "color" || core::is_cover(page) ? "rgb" : "gray";
}

std::vector<fs::path> export_png_sequence(const core::Document& episode, const fs::path& dest, int working_dpi, std::string_view mode) {
    detail::make_dirs(dest);
    detail::Output output;
    std::vector<fs::path> written;
    render::RenderOptions options;
    options.mode = std::string(mode);
    for (const auto& page : episode.pages) {
        const Image image = render::render_page(*page, working_dpi, options, &episode).image;
        const fs::path path = detail::join(dest, stem(episode) + "_" + core::file_stem(*page) + ".png");
        output.put(path, png_bytes(image));
        written.push_back(path);
    }
    output.commit();
    return written;
}

std::vector<fs::path> export_print(const core::Document& episode, const fs::path& dest, std::string fmt, std::optional<std::int64_t> dpi_given,
                                   int threshold, bool crop_marks, std::string_view area, std::string_view color, const std::string& icc,
                                   const std::optional<core::Json>& screen_given) {
    if (std::find(kAreas.begin(), kAreas.end(), area) == kAreas.end()) throw core::PyValueError("area must be one of paper, bleed, trim");
    if (std::find(kColors.begin(), kColors.end(), color) == kColors.end()) {
        throw core::PyValueError("color must be auto, rgb, cmyk, gray or bitonal");
    }
    fmt = lower(fmt);
    if (color == "cmyk" && fmt != "tiff" && fmt != "pdf" && fmt != "cmyk") throw core::PyValueError("CMYK is written as TIFF or PDF");
    const std::optional<fs::path> icc_path = icc.empty() ? std::nullopt : std::optional<fs::path>(core::path_from_utf8(icc));
    if (icc_path && color == "cmyk" && !render::colour::is_cmyk_profile(*icc_path)) {
        throw core::PyValueError("the profile is not a CMYK printing profile");
    }
    detail::make_dirs(dest);
    // print resolution comes from the page spec (B4 comic: 600)
    const std::int64_t dpi64 = dpi_given && *dpi_given != 0 ? *dpi_given
                               : episode.spec.dpi.truthy()  ? core::py_int(episode.spec.dpi)
                                                            : 600;
    if (dpi64 < 1 || dpi64 > 100000) throw core::Error("image_too_large", "the page is too large at this resolution");
    const int dpi = static_cast<int>(dpi64);
    // (Python checks the screen once every page is drawn: here first, before any is)
    std::optional<core::Json> screen;
    if (screen_given && core::py_truthy(*screen_given)) {
        const core::Json& s = *screen_given;
        const double lpi = s.contains("lpi") ? core::py_float(s["lpi"]) : 60.0;
        if (!(10 <= lpi && lpi <= 150)) throw core::PyValueError("screen lpi must be between 10 and 150");
        const std::string shape = s.contains("shape") && core::py_truthy(s["shape"]) ? core::py_str(s["shape"]) : "round";
        const auto& shapes = render::tones::dot_shapes();
        if (std::find(shapes.begin(), shapes.end(), shape) == shapes.end()) {
            std::string names;
            for (const auto& n : shapes) names += (names.empty() ? "" : ", ") + n;
            throw core::PyValueError("screen shape must be one of " + names);
        }
        screen = s;
        (*screen)["dpi"] = dpi;
    }
    const core::Json* screen_ptr = screen ? &*screen : nullptr;
    render::RenderOptions options = print_options();
    options.crop_marks = crop_marks && area == "paper";
    const auto image_of = [&](const core::Page& page) { return crop_to(render::render_page(page, dpi, options, &episode).image, page, area, dpi); };
    const auto coloured = [&](const Image& image, const std::string& how) {
        if (how == "cmyk") return render::colour::to_cmyk(image, icc_path);
        if (how == "bitonal") return render::to_bitonal(image, threshold, screen_ptr);
        return image.convert(how == "gray" ? "L" : "RGB");
    };
    // (Python reads the printer's profile again for each page: here once, the same bytes)
    std::optional<std::string> icc_bytes;
    const auto profile_for = [&](const std::string& how) -> std::string_view {
        if (icc_path && how == "cmyk") {
            if (!icc_bytes) icc_bytes = storage::read_file(*icc_path);
            return *icc_bytes;
        }
        if (how == "rgb") return srgb_icc_once();
        return {};
    };

    if (fmt == "pdf") {
        const fs::path path = detail::join(dest, stem(episode) + ".pdf");
        std::vector<PdfPage> pages;
        std::vector<std::string> hows;
        for (const auto& page : episode.pages) {
            const std::string how = page_color(*page, color);
            hows.push_back(how);
            const std::string mode = how == "cmyk" ? "CMYK" : how == "bitonal" ? "1" : how == "gray" ? "L" : "RGB";
            pages.push_back(PdfPage{mode, profile_for(how), pdf_boxes(*page, area)});
        }
        write_pdf(path, pages, [&](std::size_t n) { return coloured(image_of(*episode.pages[n]), hows[n]); });
        return {path};
    }
    detail::Output output;
    std::vector<fs::path> written;
    for (const auto& page_ptr : episode.pages) {
        const core::Page& page = *page_ptr;
        const Image image = image_of(page);
        const std::string name = stem(episode) + "_" + core::file_stem(page);
        const std::string how = page_color(page, color);
        const std::string profile(profile_for(how));
        fs::path path;
        std::string bytes;
        if (fmt == "cmyk" || (fmt == "tiff" && how != "rgb" && how != "bitonal" && color != "auto")) {  // (colour and grey TIFF)
            path = detail::join(dest, name + ".tiff");
            bytes = tiff_bytes(coloured(image, how), TiffSave{"tiff_lzw", dpi_pair(dpi), profile});
        } else if (fmt == "tiff" && how == "rgb" && color == "auto") {  // (a colour page, as a colour TIFF)
            path = detail::join(dest, name + ".tiff");
            bytes = tiff_bytes(coloured(image, how), TiffSave{"tiff_lzw", dpi_pair(dpi), profile});
        } else if (fmt == "tiff") {
            path = detail::join(dest, name + ".tiff");
            bytes = tiff_bytes(render::to_bitonal(image, threshold, screen_ptr), TiffSave{"group4", dpi_pair(dpi), {}});
        } else if (fmt == "png1") {
            path = detail::join(dest, name + ".png");
            bytes = png_bytes(render::to_bitonal(image, threshold, screen_ptr), PngSave{dpi_pair(dpi), {}, false});
        } else {
            path = detail::join(dest, name + ".png");
            bytes = png_bytes(coloured(image, how), PngSave{dpi_pair(dpi), profile, false});
        }
        output.put(path, bytes);
        written.push_back(path);
    }
    output.commit();
    return written;
}

std::vector<fs::path> export_layers(const core::Document& episode, const fs::path& dest, int dpi, std::string_view area) {
    detail::Output output;
    std::vector<fs::path> written;
    for (const auto& page_ptr : episode.pages) {
        const core::Page& page = *page_ptr;
        const fs::path folder = detail::join(dest, stem(episode) + "_" + core::file_stem(page));
        detail::make_dirs(folder);
        int n = 0;
        render::page_layers(page, episode, dpi, [&](render::PageLayer&& layer) {
            ++n;
            Image image = std::move(layer.image);
            if (layer.mask) {  // (the layer's mask applies: a PNG has no mask of its own)
                image = image.convert("RGBA");
                image.putalpha(render::chops::multiply(image.getchannel("A"), layer.mask->convert("L").resize(image.size())));
            }
            const fs::path path = detail::join(folder, detail::padded(core::Num(n), 2) + "_" + safe_name(layer.name, "layer") + ".png");
            output.put(path, png_bytes(crop_to(image, page, area, dpi), PngSave{dpi_pair(dpi), {}, false}));
            written.push_back(path);
        });
    }
    output.commit();
    return written;
}

fs::path export_strip(const core::Document& episode, const fs::path& dest, int dpi) {
    if (dest.has_parent_path()) detail::make_dirs(dest.parent_path());
    std::vector<const core::Page*> pages;
    for (const auto& page : episode.pages) {
        if (!core::is_cover(*page)) pages.push_back(page.get());
    }
    if (pages.empty()) throw core::PyValueError("max() arg is an empty sequence");
    // the strip's size from the pages' (each page its paper at this resolution, as render_page draws it), then each
    // page drawn in turn into it: the strip is never held whole
    int width = 0;
    std::int64_t height = 0;
    for (const core::Page* page : pages) {
        width = std::max(width, render::mm_to_px(page->spec.width_mm.value(), dpi));
        height += render::mm_to_px(page->spec.height_mm.value(), dpi);
    }
    if (height > 0x7fffffff / 4) throw core::Error("image_too_large", "the strip is too large at this resolution");
    PngWriter writer(render::Size{width, static_cast<int>(height)}, "RGB");
    for (const core::Page* page : pages) {
        Image image = render::render_page(*page, dpi, print_options(), &episode).image;
        if (image.width() != width) {
            Image band = Image::create("RGB", render::Size{width, image.height()}, render::Ink{255, 255, 255});
            band.paste(image, render::Point{0, 0});
            image = std::move(band);
        }
        writer.add(image);
    }
    detail::Output output;
    output.put(dest, writer.finish());
    output.commit();
    return dest;
}

fs::path export_kindle(const core::Document& episode, const fs::path& dest, int long_edge, std::optional<bool> gray, bool dots) {
    const bool grey = gray.value_or(episode.spec.expression == "mono");
    return export_epub(episode, dest, 150, true, long_edge, grey, true, dots);
}

fs::path export_epub(const core::Document& episode, const fs::path& dest, int dpi, bool kindle, std::optional<int> long_edge, bool gray,
                     bool jpeg, bool dots) {
    if (dest.has_parent_path()) detail::make_dirs(dest.parent_path());
    const std::string title = escape(episode.title);
    const bool rtl = episode.binding == core::Binding::Right;
    const std::string ident = "urn:genko:" + storage::sha256_hex(episode.title + "/" + episode.episode.repr()).substr(0, 24);
    const std::string modified = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")).toStdString();
    const std::string ext = jpeg ? "jpg" : "png";
    const std::string media = jpeg ? "image/jpeg" : "image/png";
    const bool scaled = long_edge && *long_edge != 0;
    if (scaled) {  // (render at about the resolution the long edge needs, so small type stays sharp)
        if (episode.pages.empty()) throw core::PyValueError("max() arg is an empty sequence");
        double longest = 0;
        for (std::size_t i = 0; i < episode.pages.size(); ++i) {
            const core::Rect t = episode.pages[i]->trim_rect_mm();
            const double side = std::max(t.width.value(), t.height.value());
            if (i == 0 || side > longest) longest = side;
        }
        dpi = std::max(dpi, static_cast<int>(core::py_trunc_int(*long_edge / (longest / 25.4))) + 1);
    }
    struct Picture {
        std::string stem;
        render::Size size;
        std::string bytes;
        const core::Page* page;
    };
    std::vector<Picture> pages;
    std::optional<render::Size> size;
    render::RenderOptions options = print_options();
    options.dots = dots;
    const std::string binding(core::to_string(episode.binding));
    for (const auto& [page, part] : core::reading_order(episode)) {
        Image image = render::render_page(*page, dpi, options, &episode).image;
        const core::Json* cover = core::cover_of(*page);
        if (cover != nullptr && core::is_wrap_kind(cover->value("kind", std::string()))) {
            image = render::front_of(*page, image, dpi, binding, part == "back" ? "裏表紙" : "表紙");
        } else {  // (a reader shows the finished page: no bleed, no marks, no paper around it)
            image = crop_to(image, *page, "trim", dpi);
        }
        if (scaled) {
            if (!size) {
                const double scale = static_cast<double>(*long_edge) / std::max(image.width(), image.height());
                size = render::Size{static_cast<int>(core::py_round_int(image.width() * scale)), static_cast<int>(core::py_round_int(image.height() * scale))};
            }
            // (the same size for all; never stretched)
            const double scale = std::min(static_cast<double>(size->width) / image.width(), static_cast<double>(size->height) / image.height());
            const Image fitted = image.convert("RGB").resize(
                render::Size{static_cast<int>(core::py_round_int(image.width() * scale)), static_cast<int>(core::py_round_int(image.height() * scale))},
                render::Resample::Lanczos);
            image = Image::create("RGB", *size, render::Ink{255, 255, 255});
            image.paste(fitted, render::Point{floor_half(size->width - fitted.width()), floor_half(size->height - fitted.height())});
        }
        image = image.convert(gray ? "L" : "RGB");
        std::string bytes = jpeg ? jpeg_bytes(image, JpegSave{90, false, {}}) : png_bytes(image);
        pages.push_back(Picture{part != "page" ? "cover_" + part : core::file_stem(*page), image.size(), std::move(bytes), page});
    }
    std::string manifest = "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>";
    std::string spine;
    std::vector<std::pair<std::string, std::string>> xhtml;
    const std::string start(episode.start_side.value_or(""));
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const Picture& p = pages[i];
        const std::string cover = i == 0 ? " properties=\"cover-image\"" : "";
        manifest += "<item id=\"img_" + p.stem + "\" href=\"images/" + p.stem + "." + ext + "\" media-type=\"" + media + "\"" + cover + "/>";
        manifest += "<item id=\"page_" + p.stem + "\" href=\"" + p.stem + ".xhtml\" media-type=\"application/xhtml+xml\"/>";
        if (core::is_cover(*p.page)) {  // (a cover stands alone, in the middle of the screen)
            spine += "<itemref idref=\"page_" + p.stem + "\" properties=\"rendition:page-spread-center\"/>";
        } else {
            spine += "<itemref idref=\"page_" + p.stem + "\" properties=\"page-spread-" + p.page->side(start) + "\"/>";
        }
        xhtml.emplace_back(p.stem + ".xhtml",
                           "<?xml version=\"1.0\" encoding=\"utf-8\"?><!DOCTYPE html>"
                           "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"ja\"><head>"
                           "<title>" + title + "</title><meta name=\"viewport\" content=\"width=" + std::to_string(p.size.width) +
                               ", height=" + std::to_string(p.size.height) + "\"/>"
                               "<style>html,body{margin:0;padding:0}img{display:block;width:100%;height:100%}</style></head>"
                               "<body><img src=\"images/" + p.stem + "." + ext + "\" alt=\"" + p.page->index.repr() + "\"/></body></html>");
    }
    const std::string nav = pages.empty()
                                ? std::string()
                                : "<?xml version=\"1.0\" encoding=\"utf-8\"?><!DOCTYPE html>"
                                  "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"ja\">"
                                  "<head><title>" + title + "</title></head><body><nav epub:type=\"toc\"><ol>"
                                  "<li><a href=\"" + pages[0].stem + ".xhtml\">" + title + "</a></li></ol></nav></body></html>";
    std::string kindle_meta;
    if (kindle && !pages.empty()) {
        kindle_meta = "<meta name=\"cover\" content=\"img_" + pages[0].stem + "\"/>"
                      "<meta name=\"fixed-layout\" content=\"true\"/>"
                      "<meta name=\"original-resolution\" content=\"" + std::to_string(pages[0].size.width) + "x" + std::to_string(pages[0].size.height) + "\"/>"
                      "<meta name=\"book-type\" content=\"comic\"/>"
                      "<meta name=\"primary-writing-mode\" content=\"" + std::string(rtl ? "horizontal-rl" : "horizontal-lr") + "\"/>"
                      "<meta name=\"zero-gutter\" content=\"true\"/><meta name=\"zero-margin\" content=\"true\"/>"
                      "<meta name=\"orientation-lock\" content=\"none\"/><meta name=\"region-mag\" content=\"false\"/>";
    }
    const std::string opf =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\" version=\"3.0\" xml:lang=\"ja\""
        " prefix=\"rendition: http://www.idpf.org/vocab/rendition/#\">"
        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>" + title + "</dc:title><dc:language>ja</dc:language><dc:identifier id=\"bookid\">" + ident + "</dc:identifier>"
        "<meta property=\"dcterms:modified\">" + modified + "</meta>"
        "<meta property=\"rendition:layout\">pre-paginated</meta>"
        "<meta property=\"rendition:spread\">landscape</meta>"
        "<meta property=\"rendition:orientation\">auto</meta>" +
        kindle_meta +
        "</metadata>"
        "<manifest>" + manifest + "</manifest>"
        "<spine page-progression-direction=\"" + std::string(rtl ? "rtl" : "ltr") + "\">" + spine + "</spine></package>";
    const std::string container =
        "<?xml version=\"1.0\"?>"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    std::vector<render::zip::Entry> entries;
    entries.push_back({"mimetype", "application/epub+zip", false});
    entries.push_back({"META-INF/container.xml", container, true});
    entries.push_back({"OEBPS/content.opf", opf, true});
    entries.push_back({"OEBPS/nav.xhtml", nav, true});
    for (const auto& [name, body] : xhtml) entries.push_back({"OEBPS/" + name, body, true});
    for (const Picture& p : pages) entries.push_back({"OEBPS/images/" + p.stem + "." + ext, p.bytes, false});
    detail::Output output;
    output.put(dest, render::zip::archive(entries));
    output.commit();
    return dest;
}

}  // namespace genko::formats
