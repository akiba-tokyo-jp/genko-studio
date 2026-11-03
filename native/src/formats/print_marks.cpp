// プリント…'s sheet on the paper (PC5 I-f; PhotoCraft v0.2.0's engine/src/print_cmds.rs: PAPERS, marks_ops, layout,
// print_image's colour handling — MIT).

#include "formats/print_marks.hpp"

#include <lcms2.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <tuple>

#include "core/color_model.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "formats/export.hpp"
#include "render/page.hpp"
#include "render/proof.hpp"
#include "storage/fsutil.hpp"

namespace genko::formats::print {

using core::Json;

namespace {

constexpr double kPt = 25.4 / 72.0;      // a point in mm
constexpr double kGap = 6.0 * kPt;       // the marks begin this far from the picture
constexpr double kLength = 18.0 * kPt;   // a crop mark's length
constexpr double kRing = 5.0 * kPt;      // a registration mark's radius
constexpr double kMaxProfileBytes = 64.0 * 1024 * 1024;

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

Json rect_json(const RectMm& r) { return Json::array({round3(r.x), round3(r.y), round3(r.w), round3(r.h)}); }

std::optional<double> number(std::string_view text) {
    const std::string s(text);
    if (s.empty() || s.size() > 32) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

cmsUInt32Number intent_of(std::string_view intent) {
    if (intent == "perceptual") return INTENT_PERCEPTUAL;
    if (intent == "saturation") return INTENT_SATURATION;
    if (intent == "absolute") return INTENT_ABSOLUTE_COLORIMETRIC;
    return INTENT_RELATIVE_COLORIMETRIC;
}

struct Profile {
    cmsHPROFILE handle = nullptr;
    explicit Profile(cmsHPROFILE h) : handle(h) {}
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;
    ~Profile() {
        if (handle != nullptr) cmsCloseProfile(handle);
    }
};

struct Transform {
    cmsHTRANSFORM handle = nullptr;
    explicit Transform(cmsHTRANSFORM h) : handle(h) {}
    Transform(const Transform&) = delete;
    Transform& operator=(const Transform&) = delete;
    ~Transform() {
        if (handle != nullptr) cmsDeleteTransform(handle);
    }
};

// The area a page is cut to: the whole paper when the area asked is all of it (a picture's page: no bleed, no margin) — the
// exports' crop_to counts an edge at 0 mm as one pixel in (render::mm_to_px is at least 1), which would cut the picture's
// first row and column away.
std::string_view cut_area(const core::Page& page, std::string_view area) {
    if (area == "paper") return area;
    const core::Rect r = area == "bleed" ? page.bleed_rect_mm() : page.trim_rect_mm();
    const bool whole = r.x.value() <= 0.0 && r.y.value() <= 0.0 && r.width.value() >= page.spec.width_mm.value() &&
                       r.height.value() >= page.spec.height_mm.value();
    return whole ? std::string_view("paper") : area;
}

cmsHPROFILE open(const std::optional<std::string>& bytes) {
    return bytes ? cmsOpenProfileFromMem(bytes->data(), static_cast<cmsUInt32Number>(bytes->size())) : cmsCreate_sRGBProfile();
}

}  // namespace

const std::vector<Paper>& papers() {
    static const std::vector<Paper> all{{"a3", 297.0, 420.0},      {"a4", 210.0, 297.0},      {"a5", 148.0, 210.0},
                                        {"b4", 257.0, 364.0},      {"b5", 182.0, 257.0},      {"letter", 215.9, 279.4},
                                        {"legal", 215.9, 355.6},   {"tabloid", 279.4, 431.8}, {"4x6", 101.6, 152.4},
                                        {"5x7", 127.0, 177.8}};
    return all;
}

Paper paper_of(std::string_view text) {
    const std::string name = lower(text);
    for (const Paper& paper : papers()) {
        if (paper.name == name) return paper;
    }
    if (const auto x = name.find('x'); x != std::string::npos) {
        std::string_view h = std::string_view(name).substr(x + 1);
        if (h.ends_with("mm")) h.remove_suffix(2);
        const auto w = number(std::string_view(name).substr(0, x)), hh = number(h);
        if (w && hh && *w >= 25.0 && *w <= 2000.0 && *hh >= 25.0 && *hh <= 2000.0) return Paper{name, *w, *hh};
    }
    throw core::Error("value", "the paper is a4, a3, a5, b4, b5, letter, legal, tabloid, 4x6, 5x7 or <width>x<height> in mm (25..2000)");
}

Marks marks_of(std::string_view list) {
    Marks marks;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= list.size(); ++i) {
        if (i < list.size() && list[i] != ',') continue;
        const std::string word = lower(list.substr(start, i - start));
        start = i + 1;
        if (word.empty()) continue;
        if (word == "corner") marks.corner = true;
        else if (word == "center" || word == "centre") marks.center = true;
        else if (word == "registration") marks.registration = true;
        else if (word == "description") marks.description = true;
        else if (word == "labels" || word == "label") marks.labels = true;
        else throw core::Error("value", "the marks are corner, center, registration, description and labels");
    }
    return marks;
}

std::string marks_text(const Marks& marks) {
    std::string out;
    const auto put = [&](bool on, const char* word) {
        if (!on) return;
        if (!out.empty()) out += ",";
        out += word;
    };
    put(marks.corner, "corner");
    put(marks.center, "center");
    put(marks.registration, "registration");
    put(marks.description, "description");
    put(marks.labels, "labels");
    return out;
}

std::optional<std::string> printer_profile(const Color& color) {
    if (color.intent != "perceptual" && color.intent != "relative" && color.intent != "saturation" && color.intent != "absolute") {
        throw core::Error("value", "the rendering intent is perceptual, relative, saturation or absolute");
    }
    if (color.profile.empty()) throw core::Error("value", "Genko's colour management needs the printer's profile");
    std::optional<std::string> bytes;
    if (const core::BuiltinProfile* builtin = core::builtin_profile(color.profile); builtin != nullptr) {
        if (builtin->space == core::ProfileSpace::Lab) throw core::Error("value", "the printer's profile is not an RGB profile");
        if (builtin->bytes) bytes = *builtin->bytes;
    } else {
        const std::filesystem::path file = core::path_from_utf8(color.profile);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) throw core::Error("value", "the printer's profile is not found: " + color.profile);
        bytes = storage::read_file_bounded(file, static_cast<std::size_t>(kMaxProfileBytes));
    }
    const Profile profile(open(bytes));
    if (profile.handle == nullptr) throw core::Error("value", "the printer's profile cannot be read");
    if (cmsGetColorSpace(profile.handle) != cmsSigRgbData) {
        throw core::Error("value", "the printer's profile is not an RGB profile (Genko gives the printer an RGB picture: a CMYK or grey profile cannot be chosen)");
    }
    return bytes;
}

Json Setup::json() const {
    return Json{{"area", area},
                {"scale", scale},
                {"percent", percent},
                {"center", center},
                {"left_mm", left_mm},
                {"top_mm", top_mm},
                {"spreads", spreads},
                {"marks", marks_text(marks)},
                {"color", Json{{"genko", color.genko}, {"profile", color.profile}, {"intent", color.intent}, {"bpc", color.bpc}}},
                {"copies", copies}};
}

Setup Setup::from_json(const Json& value) {
    Setup out;
    if (!value.is_object()) return out;
    const auto text = [&](const Json& v, const char* key, std::string& into) {
        if (v.contains(key) && v[key].is_string()) into = v[key].get<std::string>();
    };
    const auto flag = [&](const Json& v, const char* key, bool& into) {
        if (v.contains(key) && v[key].is_boolean()) into = v[key].get<bool>();
    };
    const auto real = [&](const char* key, double& into) {
        if (value.contains(key) && value[key].is_number()) into = value[key].get<double>();
    };
    text(value, "area", out.area);
    text(value, "scale", out.scale);
    real("percent", out.percent);
    flag(value, "center", out.center);
    real("left_mm", out.left_mm);
    real("top_mm", out.top_mm);
    flag(value, "spreads", out.spreads);
    if (value.contains("marks") && value["marks"].is_string()) {
        try {
            out.marks = marks_of(value["marks"].get<std::string>());
        } catch (const core::Error&) {
        }
    }
    if (value.contains("color") && value["color"].is_object()) {
        const Json& c = value["color"];
        flag(c, "genko", out.color.genko);
        text(c, "profile", out.color.profile);
        text(c, "intent", out.color.intent);
        flag(c, "bpc", out.color.bpc);
    }
    if (value.contains("copies") && value["copies"].is_number_integer()) out.copies = value["copies"].get<int>();
    // (what this build does not take keeps its default)
    const Setup plain;
    if (out.area != "trim" && out.area != "bleed" && out.area != "paper") out.area = plain.area;
    if (out.scale != "fit" && out.scale != "actual" && out.scale != "percent") out.scale = plain.scale;
    if (!(out.percent >= 1.0 && out.percent <= 1000.0)) out.percent = plain.percent;
    if (out.copies < 1 || out.copies > 999) out.copies = plain.copies;
    if (!(std::abs(out.left_mm) <= 1000.0) || !(std::abs(out.top_mm) <= 1000.0)) {
        out.left_mm = out.top_mm = 0.0;
        out.center = true;
    }
    if (out.color.intent != "perceptual" && out.color.intent != "relative" && out.color.intent != "saturation" && out.color.intent != "absolute") {
        out.color.intent = plain.color.intent;
    }
    return out;
}

void check(const Setup& setup) {
    if (setup.area != "trim" && setup.area != "bleed" && setup.area != "paper") throw core::Error("value", "the area is trim, bleed or paper");
    if (setup.scale != "fit" && setup.scale != "actual" && setup.scale != "percent") throw core::Error("value", "the scale is fit, actual or a percent");
    if (!(setup.percent >= 1.0 && setup.percent <= 1000.0)) throw core::Error("value", "the scale is 1% to 1000%");
    if (setup.copies < 1 || setup.copies > 999) throw core::Error("value", "the copies are 1 to 999");
    if (!(std::abs(setup.left_mm) <= 1000.0) || !(std::abs(setup.top_mm) <= 1000.0)) throw core::Error("value", "the position is within 1000 mm of the paper's corner");
    if (setup.dpi && (*setup.dpi < 72 || *setup.dpi > 600)) throw core::Error("value", "the resolution is 72 to 600 dpi");
    if (setup.color.genko) (void)printer_profile(setup.color);
}

Json Sheet::json() const {
    Json pages_json = Json::array();
    for (const std::int64_t n : pages) pages_json.push_back(n);
    Json lines_json = Json::array(), circles_json = Json::array(), texts_json = Json::array();
    for (const Line& l : lines) lines_json.push_back(Json::array({round3(l.x0), round3(l.y0), round3(l.x1), round3(l.y1)}));
    for (const Circle& c : circles) circles_json.push_back(Json::array({round3(c.x), round3(c.y), round3(c.r)}));
    for (const Text& t : texts) texts_json.push_back(Json{{"kind", t.kind}, {"text", t.text}, {"at_mm", Json::array({round3(t.x), round3(t.y)})}});
    return Json{{"pages", std::move(pages_json)},
                {"pixels", Json::array({width_px, height_px})},
                {"dpi", dpi},
                {"image_mm", rect_json(image)},
                {"trim_mm", rect_json(trim)},
                {"scale", round3(scale * 100.0)},
                {"outside", outside},
                {"marks", Json{{"lines", std::move(lines_json)}, {"circles", std::move(circles_json)}}},
                {"texts", std::move(texts_json)}};
}

Sheet place(int width_px, int height_px, int dpi, const RectMm& trim_px, const RectMm& paper, const RectMm& printable, const Setup& setup,
            const std::string& label, const std::string& description) {
    Sheet sheet;
    sheet.width_px = width_px;
    sheet.height_px = height_px;
    sheet.dpi = dpi;
    sheet.paper = paper;
    sheet.printable = printable;
    const double mm = 25.4 / dpi;
    const double iw = width_px * mm, ih = height_px * mm;
    const bool texts = (setup.marks.labels && !label.empty()) || (setup.marks.description && !description.empty());
    // (PhotoCraft's room for the marks: half an inch with lines, a quarter with texts alone; none without marks — the
    // sheet as before)
    const double margin = setup.marks.lines() ? 36.0 * kPt : texts ? 18.0 * kPt : 0.0;
    double scale = 1.0;
    if (setup.scale == "fit") {
        const double rw = std::max(1.0, printable.w - 2.0 * margin), rh = std::max(1.0, printable.h - 2.0 * margin);
        scale = std::min(rw / iw, rh / ih);
    } else if (setup.scale == "percent") {
        scale = setup.percent / 100.0;
    }
    sheet.scale = scale;
    const double w = iw * scale, h = ih * scale;
    double x = 0.0, y = 0.0;
    if (setup.center) {
        x = printable.x + (printable.w - w) / 2.0;
        y = printable.y + (printable.h - h) / 2.0;
    } else {
        x = setup.left_mm;
        y = setup.top_mm;
    }
    sheet.image = RectMm{x, y, w, h};
    const double k = mm * scale;
    sheet.trim = RectMm{x + trim_px.x * k, y + trim_px.y * k, trim_px.w * k, trim_px.h * k};
    constexpr double eps = 1e-6;
    sheet.outside = x < printable.x - eps || y < printable.y - eps || x + w > printable.x + printable.w + eps || y + h > printable.y + printable.h + eps;
    // the marks: on the finished size's edges, beginning outside the picture (PhotoCraft's marks_ops)
    const RectMm& t = sheet.trim;
    const double left = x, right = x + w, top = y, bottom = y + h;
    const double tl = t.x, tr = t.x + t.w, tt = t.y, tb = t.y + t.h, cx = t.x + t.w / 2.0, cy = t.y + t.h / 2.0;
    const auto line = [&](double x0, double y0, double x1, double y1) { sheet.lines.push_back(Line{x0, y0, x1, y1}); };
    if (setup.marks.corner && setup.area != "paper") {  // (the whole paper carries Genko's own crop marks already)
        for (const auto& [ex, ey, dx, dy] : {std::tuple{tl, tt, -1.0, -1.0}, std::tuple{tr, tt, 1.0, -1.0}, std::tuple{tl, tb, -1.0, 1.0},
                                             std::tuple{tr, tb, 1.0, 1.0}}) {
            const double from_x = dx < 0 ? left - kGap : right + kGap, from_y = dy < 0 ? top - kGap : bottom + kGap;
            line(from_x, ey, from_x + dx * kLength, ey);
            line(ex, from_y, ex, from_y + dy * kLength);
        }
    }
    if (setup.marks.center) {
        line(cx, top - kGap, cx, top - kGap - kLength);
        line(cx, bottom + kGap, cx, bottom + kGap + kLength);
        line(left - kGap, cy, left - kGap - kLength, cy);
        line(right + kGap, cy, right + kGap + kLength, cy);
    }
    if (setup.marks.registration) {
        const double off = kGap + kLength / 2.0;
        for (const auto& [rx, ry] : {std::pair{cx, top - off}, std::pair{cx, bottom + off}, std::pair{left - off, cy}, std::pair{right + off, cy}}) {
            sheet.circles.push_back(Circle{rx, ry, kRing});
            line(rx - kRing * 1.6, ry, rx + kRing * 1.6, ry);
            line(rx, ry - kRing * 1.6, rx, ry + kRing * 1.6);
        }
    }
    const double pad = setup.marks.lines() ? 30.0 * kPt : 6.0 * kPt;
    if (setup.marks.labels && !label.empty()) sheet.texts.push_back(Text{"label", label, x, top - pad});
    if (setup.marks.description && !description.empty()) sheet.texts.push_back(Text{"description", description, x, bottom + pad + kTextPt * kPt});
    return sheet;
}

RectMm trim_in(const core::Page& page, std::string_view asked, int dpi) {
    const std::string_view area = cut_area(page, asked);
    const core::Rect trim = page.trim_rect_mm();
    // (as crop_to cuts: render::mm_to_px; an edge at the paper's own, 0 mm, at 0)
    const auto px = [dpi](double v) { return v <= 0.0 ? 0.0 : static_cast<double>(render::mm_to_px(v, dpi)); };
    RectMm out{px(trim.x.value()), px(trim.y.value()), px(trim.width.value()), px(trim.height.value())};
    if (area == "trim") {
        out.x = out.y = 0.0;
    } else if (area == "bleed") {
        const core::Rect bleed = page.bleed_rect_mm();
        out.x -= px(bleed.x.value());
        out.y -= px(bleed.y.value());
    }
    return out;
}

std::string label_of(const core::Document& doc, const std::vector<const core::Page*>& pages) {
    std::string numbers;
    for (const core::Page* page : pages) {
        if (!numbers.empty()) numbers += "・";
        numbers += page->index.repr();
    }
    return (doc.title.empty() ? std::string("無題") : doc.title) + "　" + numbers + " ページ";
}

std::string description_of(const core::Document& doc) { return doc.file_info ? doc.file_info->description : std::string(); }

render::Image page_picture(const core::Document& doc, const core::Page& page, std::string_view asked, int dpi, const Color& color) {
    const std::string_view area = cut_area(page, asked);
    render::RenderOptions options;
    options.mode = "print";
    options.crop_marks = asked == "paper";  // (Genko's own crop marks: the whole paper asked)
    if (!color.genko) return crop_to(render::render_page(page, dpi, options, &doc).image, page, area, dpi);
    const std::optional<std::string> printer = printer_profile(color);
    // the page's own colours where render_page gives them: an RGB page of a colour model its values in its profile, a grey or
    // CMYK page (not a duotone) its inks in its profile — what is drawn over them in 8 bits (the lines, balloons, marks) and
    // any other page from the screen's sRGB
    std::optional<std::string> own;
    bool values = false, inks = false, grey = false;
    if (page.rgb && !page.color && !page.rgb->table && !page.rgb->is_default()) {
        values = true;
        own = std::string(detail::rgb_icc(*page.rgb));
    } else if (page.color && page.color->icc && !page.color->icc->empty() && page.color->duotone.empty() &&
               (page.color->space == core::InkSpace::Gray || page.color->space == core::InkSpace::Cmyk)) {
        inks = true;
        grey = page.color->space == core::InkSpace::Gray;
        own = *page.color->icc;
    }
    options.precise = values;
    options.inks = inks;
    options.drawn_over = values || inks;
    const render::RenderResult made = render::render_page(page, dpi, options, &doc);
    const render::Image rgb = made.image.convert("RGB");
    const std::size_t count = std::size_t(rgb.width()) * rgb.height();
    std::string out = rgb.tobytes();
    const Profile srgb(cmsCreate_sRGBProfile());
    const Profile target(open(printer));
    if (target.handle == nullptr) throw core::Error("value", "the printer's profile cannot be read");
    const cmsUInt32Number intent = intent_of(color.intent);
    const cmsUInt32Number flags = (color.bpc ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0) | cmsFLAGS_NOCACHE;
    {
        const Transform t(cmsCreateTransform(srgb.handle, TYPE_RGB_8, target.handle, TYPE_RGB_8, intent, flags));
        if (t.handle == nullptr) throw core::Error("value", "the printer's profile cannot be used");
        cmsDoTransform(t.handle, out.data(), out.data(), static_cast<cmsUInt32Number>(count));
    }
    std::shared_ptr<const std::vector<float>> own_values;
    if (values && made.precise) own_values = render::proof::values_of(*made.precise);
    if (inks && made.inks) own_values = render::proof::values_of(*made.inks);
    if (own_values && own) {
        const Profile source(open(own));
        if (source.handle == nullptr) throw core::Error("value", "the page's profile cannot be read");
        const unsigned channels = inks ? (grey ? 1u : 4u) : 3u;
        if (own_values->size() != count * channels) throw core::Error("value", "the page's colours were not drawn at its size");
        const cmsUInt32Number in = inks ? (grey ? TYPE_GRAY_FLT : TYPE_CMYK_FLT) : TYPE_RGB_FLT;
        const Transform t(cmsCreateTransform(source.handle, in, target.handle, TYPE_RGB_8, intent, flags));
        if (t.handle == nullptr) throw core::Error("value", "the printer's profile cannot be used with the page's");
        const float most = inks && !grey ? 100.0f : 1.0f;  // (Little CMS's float CMYK is in %)
        const std::vector<unsigned char>* over = made.drawn_over ? &*made.drawn_over : nullptr;
        std::vector<float> row;
        std::string device;
        const std::size_t w = static_cast<std::size_t>(rgb.width());
        for (int y = 0; y < rgb.height(); ++y) {
            row.assign(own_values->begin() + static_cast<std::ptrdiff_t>(std::size_t(y) * w * channels),
                       own_values->begin() + static_cast<std::ptrdiff_t>(std::size_t(y + 1) * w * channels));
            for (float& v : row) v *= most;
            device.resize(w * 3);
            cmsDoTransform(t.handle, row.data(), device.data(), static_cast<cmsUInt32Number>(w));
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t i = std::size_t(y) * w + x;
                if (over != nullptr && i < over->size() && (*over)[i] != 0) continue;  // (drawn over in 8 bits: its sRGB)
                std::copy_n(device.data() + x * 3, 3, out.data() + i * 3);
            }
        }
    }
    const render::Image converted = render::Image::frombytes("RGB", rgb.size(), out);
    return crop_to(converted, page, area, dpi);
}

}  // namespace genko::formats::print
