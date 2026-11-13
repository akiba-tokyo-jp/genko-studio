// PC5 G-e: Photoshop's text layer and Genko's type layer each other's (render/psd_type.hpp).

#include "render/psd_type.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <tuple>

#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/pynum.hpp"
#include "core/ruby.hpp"
#include "core/type_styles.hpp"
#include "core/type_text.hpp"
#include "core/vector.hpp"
#include "render/ops_type.hpp"
#include "render/text/fonts.hpp"
#include "render/text/shaping.hpp"

namespace genko::render::psd_type {

using core::Json;

namespace {

double r4(double v) { return core::py_round(v, 4); }
double clamp4(double v, double lo, double hi) { return r4(std::clamp(v, lo, hi)); }

const Json* key(const Json& object, std::string_view name) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(name);
    return it == object.end() ? nullptr : &*it;
}
std::optional<double> number_at(const Json& object, std::string_view name) {
    const Json* v = key(object, name);
    if (v == nullptr || !v->is_number()) return std::nullopt;
    const double x = v->get<double>();
    return std::isfinite(x) ? std::optional<double>(x) : std::nullopt;
}
std::optional<bool> flag_at(const Json& object, std::string_view name) {
    const Json* v = key(object, name);
    return v != nullptr && v->is_boolean() ? std::optional<bool>(v->get<bool>()) : std::nullopt;
}

// A CMYK fill's RGB (render/psd_text's rule: K alone its grey, else each ink taking its share of the light).
std::array<int, 3> rgb_of(const std::array<double, 4>& c) {
    if (c[0] == 0 && c[1] == 0 && c[2] == 0) {
        const int v = static_cast<int>(std::lround(std::clamp(1.0 - c[3], 0.0, 1.0) * 255.0));
        return {v, v, v};
    }
    std::array<int, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) out[i] = static_cast<int>(std::lround(std::clamp((1.0 - c[i]) * (1.0 - c[3]), 0.0, 1.0) * 255.0));
    return out;
}

// A PostScript name as a record keeps it: the text engine's tokens, 1..256 of them.
std::string postscript_token(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '+' || c == '-') out += c;
        if (out.size() >= 256) break;
    }
    return out;
}

constexpr std::array<std::string_view, 7> kAligns{"left", "right", "center", "justify", "justify_right", "justify_center", "justify_all"};

int justification_of(std::string_view align) {
    if (align == "top" || align == "start") return 0;
    if (align == "bottom") return 1;
    for (std::size_t i = 0; i < kAligns.size(); ++i) {
        if (kAligns[i] == align) return static_cast<int>(i);
    }
    return 0;
}

std::string align_of(int justification, bool vertical) {
    const int j = std::clamp(justification, 0, 6);
    if (vertical && j == 0) return "top";
    if (vertical && j == 1) return "bottom";
    return std::string(kAligns[static_cast<std::size_t>(j)]);
}

// The UTF-16 units of a text as code points (a pair one, a lone half U+FFFD) — each unit's code point's place.
struct Units {
    std::u32string points;
    std::vector<std::size_t> point_of;  // (each unit's; then the end)
};
Units units_of(std::u16string_view text) {
    Units out;
    out.point_of.reserve(text.size() + 1);
    for (std::size_t i = 0; i < text.size(); ++i) {
        out.point_of.push_back(out.points.size());
        char32_t c = text[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (text[i + 1] - 0xdc00);
            out.point_of.push_back(out.points.size());
            ++i;
        } else if (c >= 0xd800 && c < 0xe000) {
            c = 0xfffd;
        }
        // (a paragraph's end and Photoshop's forced line break: a line break; another control character a space)
        if (c == U'\r' || c == 0x03) c = U'\n';
        else if ((c < 0x20 && c != U'\n') || c == 0x7f) c = U' ';
        out.points.push_back(c);
    }
    out.point_of.push_back(out.points.size());
    return out;
}

// The grapheme a code point is in.
std::int64_t grapheme_of(const std::vector<std::size_t>& bounds, std::size_t point) {
    const auto it = std::upper_bound(bounds.begin(), bounds.end(), point);
    return static_cast<std::int64_t>(it - bounds.begin()) - 1;
}

// `attrs` without what `given` says the same (a style's attributes: what the text has of them is not its own).
Json without_same(const Json& attrs, const Json* given) {
    if (given == nullptr || !given->is_object()) return attrs;
    Json out = Json::object();
    for (const auto& [k, v] : attrs.items()) {
        const auto it = given->find(k);
        if (it == given->end() || *it != v) out[k] = v;
    }
    return out;
}

// The attributes whose values are their defaults (core::type_text's: one of null — the line's own — never left out).
Json without_defaults(const Json& attrs, const Json& defaults) {
    Json out = Json::object();
    for (const auto& [k, v] : attrs.items()) {
        const auto it = defaults.find(k);
        if (it == defaults.end() || it->is_null() || *it != v) out[k] = v;
    }
    return out;
}

// A style's name as a book takes it (core::type_styles::checked_name's rules: trimmed, NFC, printable, 1..64 characters,
// not 「[なし]」 or 「[基本段落]」), else the kind's own; then made unique among the kind's.
std::string style_name(const Json* styles, core::type_styles::Kind kind, std::u16string_view given) {
    std::u32string kept;
    for (const char32_t c : units_of(given).points) {
        if (c < 0x20 || (c >= 0x7f && c < 0xa0) || c == 0x2028 || c == 0x2029 || c == 0xfffd) continue;
        kept.push_back(c);
    }
    std::string name = core::ruby::to_utf8(kept);
    const std::string base(kind == core::type_styles::Kind::Character ? core::type_styles::kCharacterBase : core::type_styles::kParagraphBase);
    try {
        name = core::type_styles::checked_name(nullptr, kind, Json(name.empty() ? base : name));
    } catch (const std::exception&) {
        name = core::ruby::slice(name, 0, 64);
        try {
            name = core::type_styles::checked_name(nullptr, kind, Json(name));
        } catch (const std::exception&) {
            name = base;
        }
    }
    return core::type_styles::unique_name(styles, kind, name, name == base);
}

// A named sheet taken into the book (its id), or none past the kind's 512.
std::optional<std::string> take_sheet(core::Document& doc, Sheets& sheets, core::type_styles::Kind kind, const psd::NamedSheet& sheet, double mm,
                                      bool vertical, std::vector<std::string>& missing) {
    const bool character = kind == core::type_styles::Kind::Character;
    const std::string seen = std::string(character ? "c" : "p") + "\n" + psd::utf8_of(sheet.name) + "\n" + sheet.data.dump() + "\n" +
                             std::to_string(sheet.default_sheet);
    if (const auto it = sheets.taken.find(seen); it != sheets.taken.end()) return it->second;
    if (character ? sheets.full_char : sheets.full_para) return std::nullopt;
    const Json* styles = doc.type_styles ? &*doc.type_styles : nullptr;
    Json item = Json::object();
    if (character) {
        item["char"] = core::type_text::check_char(char_attrs(sheet.data, mm, &missing));
    } else {
        item["para"] = core::type_text::check_para(para_attrs(sheet.data, mm, vertical));
    }
    // (an earlier import's style of the same name and attributes: that one)
    const std::string wanted = style_name(nullptr, kind, sheet.name);
    if (const Json* same = core::type_styles::same_style(styles, kind, wanted, item)) {
        const std::string id = same->value("id", std::string());
        sheets.taken.emplace(seen, id);
        return id;
    }
    const Json* list = styles != nullptr ? key(*styles, character ? "character" : "paragraph") : nullptr;
    if (list != nullptr && list->is_array() && list->size() >= 512) {
        sheets.full = true;
        (character ? sheets.full_char : sheets.full_para) = true;
        return std::nullopt;
    }
    item["id"] = core::type_styles::next_id(styles, kind);
    item["name"] = style_name(styles, kind, sheet.name);
    Json all = styles != nullptr && styles->is_object() ? *styles : Json::object();
    Json& into = all[character ? "character" : "paragraph"];
    if (!into.is_array()) into = Json::array();
    Json ordered = Json::object({{"id", item["id"]}, {"name", item["name"]}});
    for (const char* k : {"char", "para"}) {
        if (item.contains(k) && !item[k].empty()) ordered[k] = item[k];
    }
    into.push_back(std::move(ordered));
    doc.type_styles = core::type_text::check_type_styles(all);
    const std::string id = item["id"].get<std::string>();
    sheets.taken.emplace(seen, id);
    sheets.added.push_back(id);
    return id;
}

// The bundled fonts by their PostScript names (read once).
const std::map<std::string, std::string>& bundled_names() {
    static std::once_flag once;
    static std::map<std::string, std::string> names;
    std::call_once(once, [] {
        for (const char* font : {"gothic", "mincho", "maru", "hand", "sfx", "sfx_pop"}) {
            try {
                const text::FontInfo info = text::font_info(font, 0, false);
                if (!info.postscript.empty()) names.emplace(info.postscript, font);
            } catch (const std::exception&) {
            }
        }
    });
    return names;
}

}  // namespace

std::optional<std::string> bundled_font(std::string_view postscript) {
    const auto& names = bundled_names();
    const auto it = names.find(std::string(postscript));
    if (it == names.end()) return std::nullopt;
    return it->second;
}

std::string antialias_of(std::string_view anta, int engine) {
    if (anta == "Anno" || anta == "antiAliasNone") return "none";
    if (anta == "antiAliasSharp" || anta == "AnSh") return "sharp";
    if (anta == "AnCr" || anta == "antiAliasCrisp") return "crisp";
    if (anta == "AnSt" || anta == "antiAliasStrong") return "strong";
    if (anta == "AnSm" || anta == "antiAliasSmooth") return "smooth";
    if (anta == "antiAliasPlatformGray") return "windows";
    if (anta == "antiAliasPlatformLCD") return "windowsLcd";
    static constexpr std::array<std::string_view, 5> kEngine{"none", "sharp", "crisp", "strong", "smooth"};
    return std::string(kEngine[static_cast<std::size_t>(std::clamp(engine, 0, 4))]);
}

std::pair<std::string, int> anta_of(std::string_view antialias) {
    if (antialias == "none") return {"Anno", 0};
    if (antialias == "sharp") return {"antiAliasSharp", 1};
    if (antialias == "crisp") return {"AnCr", 2};
    if (antialias == "strong") return {"AnSt", 3};
    if (antialias == "windows") return {"antiAliasPlatformGray", 1};
    if (antialias == "windowsLcd") return {"antiAliasPlatformLCD", 1};
    return {"AnSm", 4};
}

Json engine_keys(const psd::TextStyle& s) {
    if (!s.attrs.empty()) return Json{{"_attrs", s.attrs}};  // (Genko's own, as they are: a .pcraft's)
    Json out = Json::object();
    if (!s.font.empty()) out["Font"] = s.font;
    out["FontSize"] = s.size;
    if (s.fill_cmyk) out["FillColor"] = Json{{"cmyk", Json::array({(*s.fill_cmyk)[0], (*s.fill_cmyk)[1], (*s.fill_cmyk)[2], (*s.fill_cmyk)[3]})}};
    else out["FillColor"] = Json::array({s.fill[0], s.fill[1], s.fill[2]});
    out["Tracking"] = s.tracking;
    out["AutoLeading"] = s.auto_leading;
    out["Leading"] = s.leading;
    out["BaselineShift"] = s.baseline_shift;
    out["HorizontalScale"] = s.horizontal_scale;
    out["VerticalScale"] = s.vertical_scale;
    out["FauxBold"] = s.faux_bold;
    out["FauxItalic"] = s.faux_italic;
    out["Underline"] = s.underline;
    out["Strikethrough"] = s.strikethrough;
    out["FontCaps"] = s.caps;
    out["AutoKerning"] = s.auto_kerning;
    out["Ligatures"] = s.ligatures;
    out["DLigatures"] = s.dligatures;
    for (std::size_t i = 0; i < psd::kOpenTypeKeys.size(); ++i) {
        if ((s.opentype >> i) & 1u) out[std::string(psd::kOpenTypeKeys[i].key)] = true;
    }
    return out;
}

Json engine_keys(const psd::ParagraphRun& p) {
    if (!p.attrs.empty()) return Json{{"_attrs", p.attrs}};
    return Json{{"Justification", p.justification}, {"FirstLineIndent", p.first_indent}, {"StartIndent", p.start_indent},
                {"EndIndent", p.end_indent},       {"SpaceBefore", p.space_before},     {"SpaceAfter", p.space_after},
                {"AutoLeading", p.auto_leading},   {"AutoHyphenate", p.hyphenate}};
}

Json char_attrs(const Json& keys, double mm, std::vector<std::string>* missing) {
    if (const Json* own = key(keys, "_attrs"); own != nullptr && own->is_object()) {  // (Genko's own: a .pcraft's)
        if (missing != nullptr && own->contains("postscript") && !own->contains("font") && (*own)["postscript"].is_string()) {
            const std::string name = (*own)["postscript"].get<std::string>();
            if (std::find(missing->begin(), missing->end(), name) == missing->end()) missing->push_back(name);
        }
        return *own;
    }
    Json out = Json::object();
    if (const Json* font = key(keys, "Font"); font != nullptr && font->is_string() && font->get<std::string>() != "AdobeInvisFont") {
        const std::string name = font->get<std::string>();
        if (const auto found = bundled_font(name)) {
            out["font"] = *found;
        } else if (const std::string token = postscript_token(name); !token.empty()) {
            out["postscript"] = token;
            if (missing != nullptr && std::find(missing->begin(), missing->end(), token) == missing->end()) missing->push_back(token);
        }
    }
    if (const auto v = number_at(keys, "FontSize"); v && *v > 0) out["size_mm"] = clamp4(*v * mm, 0.1, 1000.0);
    if (const Json* fill = key(keys, "FillColor")) {
        if (fill->is_array() && fill->size() == 3) {
            Json rgb = Json::array();
            for (const Json& c : *fill) rgb.push_back(std::clamp(c.is_number() ? static_cast<int>(std::lround(c.get<double>())) : 0, 0, 255));
            out["rgb"] = std::move(rgb);
        } else if (const Json* cmyk = key(*fill, "cmyk"); cmyk != nullptr && cmyk->is_array() && cmyk->size() == 4) {
            std::array<double, 4> c{};
            for (std::size_t i = 0; i < 4; ++i) c[i] = (*cmyk)[i].is_number() ? (*cmyk)[i].get<double>() : 0.0;
            const auto rgb = rgb_of(c);
            out["rgb"] = Json::array({rgb[0], rgb[1], rgb[2]});
        }
    }
    if (const auto v = number_at(keys, "Tracking")) out["tracking"] = clamp4(*v / 1000.0, -1.0, 10.0);
    const auto autoleading = flag_at(keys, "AutoLeading");
    const auto leading = number_at(keys, "Leading");
    if (autoleading == true) out["leading_mm"] = "auto";
    else if (leading && *leading > 0) out["leading_mm"] = clamp4(*leading * mm, 0.0, 1000.0);
    if (const auto v = number_at(keys, "BaselineShift")) out["baseline_shift_mm"] = clamp4(*v * mm, -1000.0, 1000.0);
    if (const auto v = number_at(keys, "HorizontalScale")) out["scale_x"] = clamp4(*v, 0.01, 10.0);
    if (const auto v = number_at(keys, "VerticalScale")) out["scale_y"] = clamp4(*v, 0.01, 10.0);
    if (const auto v = flag_at(keys, "FauxBold")) out["faux_bold"] = *v ? "bold" : "normal";
    if (const auto v = flag_at(keys, "FauxItalic")) out["faux_italic"] = *v;
    if (const auto v = flag_at(keys, "Underline")) out["underline"] = *v;
    if (const auto v = flag_at(keys, "Strikethrough")) out["strikethrough"] = *v;
    if (const auto v = number_at(keys, "FontCaps")) out["caps"] = *v >= 1.5 ? "all" : *v >= 0.5 ? "small" : "normal";
    if (const auto v = flag_at(keys, "AutoKerning")) out["kerning"] = *v ? "metrics" : "none";
    if (const auto v = flag_at(keys, "Ligatures")) out["ligatures"] = *v;
    if (const auto v = flag_at(keys, "DLigatures")) out["discretionary_ligatures"] = *v;
    Json features = Json::object();
    for (const psd::OpenTypeKey& k : psd::kOpenTypeKeys) {
        if (flag_at(keys, k.key) == true) features[std::string(k.tag)] = 1;
    }
    if (!features.empty()) out["features"] = std::move(features);
    return out;
}

Json para_attrs(const Json& keys, double mm, bool vertical) {
    if (const Json* own = key(keys, "_attrs"); own != nullptr && own->is_object()) return *own;
    Json out = Json::object();
    if (const auto v = number_at(keys, "Justification")) out["align"] = align_of(static_cast<int>(std::lround(*v)), vertical);
    if (const auto v = number_at(keys, "FirstLineIndent")) out["first_indent_mm"] = clamp4(*v * mm, -1000.0, 1000.0);
    if (const auto v = number_at(keys, "StartIndent")) out["start_indent_mm"] = clamp4(*v * mm, -1000.0, 1000.0);
    if (const auto v = number_at(keys, "EndIndent")) out["end_indent_mm"] = clamp4(*v * mm, -1000.0, 1000.0);
    if (const auto v = number_at(keys, "SpaceBefore")) out["space_before_mm"] = clamp4(*v * mm, 0.0, 1000.0);
    if (const auto v = number_at(keys, "SpaceAfter")) out["space_after_mm"] = clamp4(*v * mm, 0.0, 1000.0);
    if (const auto v = number_at(keys, "AutoLeading")) out["auto_leading"] = clamp4(*v, 0.0, 5.0);
    if (const auto v = flag_at(keys, "AutoHyphenate")) out["hyphenate"] = *v;
    return out;
}

Json char_keys(const Json& attrs, double k, const std::function<std::string(const std::string&)>& font_name) {
    Json out = Json::object();
    if (const Json* font = key(attrs, "font"); font != nullptr && font->is_string()) {
        const text::Face face = text::face_of(*font);
        out["Font"] = font_name(face.other);
    } else if (const Json* ps = key(attrs, "postscript"); ps != nullptr && ps->is_string()) {
        out["Font"] = ps->get<std::string>();
    }
    if (const auto v = number_at(attrs, "size_mm")) out["FontSize"] = *v * k;
    if (const Json* rgb = key(attrs, "rgb"); rgb != nullptr && rgb->is_array() && rgb->size() == 3) out["FillColor"] = *rgb;
    if (const auto v = number_at(attrs, "tracking")) out["Tracking"] = *v * 1000.0;
    if (const Json* leading = key(attrs, "leading_mm")) {
        if (leading->is_string()) {
            out["AutoLeading"] = true;
        } else if (leading->is_number()) {
            out["AutoLeading"] = false;
            out["Leading"] = leading->get<double>() * k;
        }
    }
    if (const auto v = number_at(attrs, "baseline_shift_mm")) out["BaselineShift"] = *v * k;
    if (const auto v = number_at(attrs, "scale_x")) out["HorizontalScale"] = *v;
    if (const auto v = number_at(attrs, "scale_y")) out["VerticalScale"] = *v;
    if (const Json* bold = key(attrs, "faux_bold"); bold != nullptr && bold->is_string()) out["FauxBold"] = bold->get<std::string>() != "normal";
    if (const auto v = flag_at(attrs, "faux_italic")) out["FauxItalic"] = *v;
    if (const auto v = flag_at(attrs, "underline")) out["Underline"] = *v;
    if (const auto v = flag_at(attrs, "strikethrough")) out["Strikethrough"] = *v;
    if (const Json* caps = key(attrs, "caps"); caps != nullptr && caps->is_string())
        out["FontCaps"] = caps->get<std::string>() == "all" ? 2 : caps->get<std::string>() == "small" ? 1 : 0;
    if (const Json* kern = key(attrs, "kerning"); kern != nullptr && kern->is_string()) out["AutoKerning"] = kern->get<std::string>() != "none";
    if (const auto v = flag_at(attrs, "ligatures")) out["Ligatures"] = *v;
    if (const auto v = flag_at(attrs, "discretionary_ligatures")) out["DLigatures"] = *v;
    if (const Json* features = key(attrs, "features"); features != nullptr && features->is_object()) {
        for (const psd::OpenTypeKey& ot : psd::kOpenTypeKeys) {
            if (const auto it = features->find(ot.tag); it != features->end() && it->is_number_integer()) out[std::string(ot.key)] = it->get<std::int64_t>() > 0;
        }
    }
    return out;
}

Json para_keys(const Json& attrs, double k, bool vertical) {
    (void)vertical;
    Json out = Json::object();
    if (const Json* align = key(attrs, "align"); align != nullptr && align->is_string()) out["Justification"] = justification_of(align->get<std::string>());
    if (const auto v = number_at(attrs, "first_indent_mm")) out["FirstLineIndent"] = *v * k;
    if (const auto v = number_at(attrs, "start_indent_mm")) out["StartIndent"] = *v * k;
    if (const auto v = number_at(attrs, "end_indent_mm")) out["EndIndent"] = *v * k;
    if (const auto v = number_at(attrs, "space_before_mm")) out["SpaceBefore"] = *v * k;
    if (const auto v = number_at(attrs, "space_after_mm")) out["SpaceAfter"] = *v * k;
    if (const auto v = number_at(attrs, "auto_leading")) out["AutoLeading"] = *v;
    if (const auto v = flag_at(attrs, "hyphenate")) out["AutoHyphenate"] = *v;
    return out;
}

void take_sheets(const psd::TextLayer& t, const Frame& frame, core::Document& doc, Sheets& sheets, std::vector<std::optional<std::string>>* char_ids,
                 std::vector<std::optional<std::string>>* para_ids, std::vector<std::string>* missing) {
    std::vector<std::string> unused;
    std::vector<std::string>& fonts = missing != nullptr ? *missing : unused;
    for (const psd::NamedSheet& s : t.char_sheets) {
        auto id = take_sheet(doc, sheets, core::type_styles::Kind::Character, s, frame.mm_y, t.vertical, fonts);
        if (char_ids != nullptr) char_ids->push_back(std::move(id));
    }
    for (const psd::NamedSheet& s : t.para_sheets) {
        auto id = take_sheet(doc, sheets, core::type_styles::Kind::Paragraph, s, frame.mm_y, t.vertical, fonts);
        if (para_ids != nullptr) para_ids->push_back(std::move(id));
    }
}

Read read_text(const psd::TextLayer& t, const Frame& frame, core::Document& doc, Sheets& sheets) {
    Read out;
    // (a text past what a type layer holds by its UTF-16 alone — at most 16 units a grapheme — said before it is spread
    // into code points and graphemes outside the reading's budget: the review's P2-3)
    const auto too_long = [&](std::size_t units) {
        if (units <= type_ops::kMaxTypeText * 16) return false;
        out.why = "文字が多すぎます（" + std::to_string(type_ops::kMaxTypeText) + " 字まで）";
        return true;
    };
    if (too_long(t.text.size()) || (t.text.empty() && too_long(t.engine_text.size()))) return out;
    std::u16string text16 = t.text;
    if (text16.empty() && !t.engine_text.empty()) {
        text16 = t.engine_text;
        if (!text16.empty() && text16.back() == u'\r') text16.pop_back();
    }
    const Units units = units_of(text16);
    if (units.points.empty()) {
        out.why = "文字がありません";
        return out;
    }
    const std::vector<std::size_t> bounds = core::ruby::bounds(units.points);
    const auto graphemes = static_cast<std::int64_t>(bounds.size()) - 1;
    if (graphemes > static_cast<std::int64_t>(type_ops::kMaxTypeText)) {
        out.why = "文字が多すぎます（" + std::to_string(type_ops::kMaxTypeText) + " 字まで）";
        return out;
    }
    const double mm = frame.mm_y;  // (the text's own space: a pixel of the file this many mm before its matrix)
    if (!(mm > 0) || !(frame.mm_x > 0)) {
        out.why = "置き場所がありません";
        return out;
    }
    const bool vertical = t.vertical;
    // (a place in the text's units → the grapheme it begins in; the end of the text → its graphemes)
    const auto grapheme_at = [&](std::size_t unit) -> std::int64_t {
        if (unit >= text16.size()) return graphemes;
        return grapheme_of(bounds, units.point_of[unit]);
    };
    // the file's sheets as the book's styles
    std::vector<std::optional<std::string>> char_ids, para_ids;
    take_sheets(t, frame, doc, sheets, &char_ids, &para_ids, &out.missing_fonts);
    const Json* styles = doc.type_styles ? &*doc.type_styles : nullptr;
    const auto style_attrs = [&](core::type_styles::Kind kind, const std::optional<std::string>& id, const char* part) -> const Json* {
        if (!id) return nullptr;
        const Json* s = core::type_styles::find(styles, kind, *id);
        return s != nullptr ? key(*s, part) : nullptr;
    };
    // the characters: each run's attributes, its own past its style's and the defaults
    struct Piece {
        std::int64_t start, end;
        std::optional<std::string> style;
        Json own;
    };
    std::vector<Piece> pieces;
    std::size_t unit = 0;
    for (const psd::StyleRun& run : t.runs) {
        const std::size_t from = unit;
        unit = std::min<std::size_t>(text16.size(), unit + run.length);
        const std::int64_t g0 = grapheme_at(from), g1 = grapheme_at(unit);
        if (g1 <= g0) continue;
        std::optional<std::string> style;
        if (run.style.sheet && static_cast<std::size_t>(*run.style.sheet) < char_ids.size()) style = char_ids[static_cast<std::size_t>(*run.style.sheet)];
        Json own = char_attrs(engine_keys(run.style), mm, &out.missing_fonts);
        own = without_defaults(without_same(own, style_attrs(core::type_styles::Kind::Character, style, "char")), core::type_text::char_defaults());
        pieces.push_back(Piece{g0, g1, style, std::move(own)});
    }
    if (!pieces.empty() && pieces.back().end < graphemes) pieces.back().end = graphemes;  // (a short last run: to the end)
    Json nt = Json::object();
    // (the whole text's style when every run takes the same; its own: what every run says alike)
    const bool one_style = !pieces.empty() && std::all_of(pieces.begin(), pieces.end(), [&](const Piece& p) { return p.style == pieces.front().style; });
    if (one_style && pieces.front().style) nt["char_style"] = *pieces.front().style;
    // (a value every run has — or, a run taking a style of its own as a range's, one its style gives over it —: the whole's)
    const auto given_by_style = [&](const Piece& p, const std::string& name) {
        if (one_style || !p.style) return false;
        const Json* attrs = style_attrs(core::type_styles::Kind::Character, p.style, "char");
        return attrs != nullptr && attrs->contains(name);
    };
    Json whole = Json::object();
    std::set<std::string> asked;  // (each attribute's question once: a run's own value is the only one that can be everywhere)
    for (const Piece& p : pieces) {
        for (const auto& [name, value] : p.own.items()) {
            if (!asked.insert(name).second) continue;
            const bool everywhere = std::all_of(pieces.begin(), pieces.end(), [&](const Piece& q) {
                const auto there = q.own.find(name);
                return there != q.own.end() ? *there == value : given_by_style(q, name);
            });
            if (everywhere) whole[name] = value;
        }
    }
    if (!whole.empty()) nt["char"] = whole;
    Json runs = Json::array();
    for (const Piece& p : pieces) {
        Json own = Json::object();
        for (const auto& [k, v] : p.own.items()) {
            if (!whole.contains(k)) own[k] = v;
        }
        Json run{{"start", p.start}, {"end", p.end}};
        if (!one_style && p.style) run["style"] = *p.style;
        if (!own.empty()) run["char"] = std::move(own);
        if (run.size() == 2) continue;
        // (one run after another of the same: one)
        if (!runs.empty() && runs.back()["end"] == Json(p.start)) {
            Json last = runs.back();
            last.erase("start");
            last.erase("end");
            Json now = run;
            now.erase("start");
            now.erase("end");
            if (last == now) {
                runs.back()["end"] = p.end;
                continue;
            }
        }
        runs.push_back(std::move(run));
    }
    if (!runs.empty()) nt["runs"] = std::move(runs);
    // the paragraphs: each the settings of the run it begins in
    const std::vector<std::int64_t> starts = core::type_text::paragraph_starts_of(core::ruby::to_utf8(units.points));
    std::vector<std::pair<std::optional<std::string>, Json>> paras;
    std::vector<std::int64_t> run_ends;  // (the grapheme each paragraph run ends before)
    {
        std::size_t at = 0;
        for (const psd::ParagraphRun& p : t.paragraphs) {
            at = std::min<std::size_t>(text16.size(), at + p.length);
            run_ends.push_back(grapheme_at(at));
        }
    }
    std::size_t next_run = 0;
    for (const std::int64_t start : starts) {
        while (next_run + 1 < run_ends.size() && run_ends[next_run] <= start) ++next_run;
        const psd::ParagraphRun* found = t.paragraphs.empty() ? nullptr : &t.paragraphs[std::min(next_run, t.paragraphs.size() - 1)];
        std::optional<std::string> style;
        Json own = Json::object();
        if (found != nullptr) {
            if (found->sheet && static_cast<std::size_t>(*found->sheet) < para_ids.size()) style = para_ids[static_cast<std::size_t>(*found->sheet)];
            own = para_attrs(engine_keys(*found), mm, vertical);
            own = without_same(own, style_attrs(core::type_styles::Kind::Paragraph, style, "para"));
            const std::string align = own.value("align", std::string());
            own = without_defaults(own, core::type_text::para_defaults());
            if (!align.empty()) own["align"] = align;  // (always: a shaped record's own default is left, top)
        }
        paras.emplace_back(style, std::move(own));
    }
    const bool one_para_style = !paras.empty() && std::all_of(paras.begin(), paras.end(), [&](const auto& p) { return p.first == paras.front().first; });
    if (one_para_style && paras.front().first) nt["para_style"] = *paras.front().first;
    const bool one_para = !paras.empty() && std::all_of(paras.begin(), paras.end(), [&](const auto& p) { return p.second == paras.front().second; });
    if (!paras.empty() && !paras.front().second.empty()) nt["para"] = paras.front().second;  // (the first's: the others say what differs)
    Json paragraphs = Json::array();
    for (std::size_t i = 0; i < paras.size(); ++i) {
        Json item{{"index", static_cast<std::int64_t>(i)}};
        if (!one_para_style && paras[i].first) item["style"] = *paras[i].first;
        if (!one_para) {
            const Json* first = nt.contains("para") ? &nt["para"] : nullptr;
            Json own = without_same(paras[i].second, first);
            // (what the first paragraph says that this one does not: its default again)
            if (first != nullptr) {
                for (const auto& [k, v] : first->items()) {
                    if (!paras[i].second.contains(k)) {
                        const Json& d = core::type_text::para_defaults()[k];
                        own[k] = d.is_null() ? Json(vertical ? "top" : "left") : d;
                    }
                }
            }
            if (!own.empty()) item["para"] = std::move(own);
        }
        if (item.size() > 1) paragraphs.push_back(std::move(item));
    }
    if (!paragraphs.empty()) nt["paragraphs"] = std::move(paragraphs);
    // where it is: its origin on the page, its matrix past the file's own scale
    const auto& m = t.transform;  // (xx, xy, yx, yy, tx, ty)
    const double ox = frame.x0_mm + m[4] * frame.mm_x, oy = frame.y0_mm + m[5] * frame.mm_y;
    const double a = frame.mm_x * m[0] / mm, b = frame.mm_y * m[1] / mm, c = frame.mm_x * m[2] / mm, d = frame.mm_y * m[3] / mm;
    if (!std::isfinite(a + b + c + d + ox + oy) || std::abs(a * d - b * c) < 1e-9) {
        out.why = "文字の行列が潰れています";
        return out;
    }
    core::StoryLine r;
    r.text = core::ruby::to_utf8(units.points);
    r.balloon = "none";
    r.wrap = vertical ? "vertical" : "horizontal";
    r.style = Json::object();
    if (t.box) {
        const auto& bx = *t.box;
        nt["shape"] = "box";
        r.x_mm = core::Num(r4(ox + bx[0] * mm));
        r.y_mm = core::Num(r4(oy + bx[1] * mm));
        r.w_mm = core::Num(r4(std::max(0.1, (bx[2] - bx[0]) * mm)));
        r.h_mm = core::Num(r4(std::max(0.1, (bx[3] - bx[1]) * mm)));
    } else {
        nt["shape"] = "point";
        nt["anchor_mm"] = Json::array({r4(ox), r4(oy)});
        r.x_mm = core::Num(r4(ox));
        r.y_mm = core::Num(r4(oy));
        r.w_mm = core::Num(1);
        r.h_mm = core::Num(1);
    }
    for (const double v : {r.x_mm.value(), r.y_mm.value(), r.w_mm.value(), r.h_mm.value(), ox, oy}) {
        if (!std::isfinite(v) || std::abs(v) > 1000.0) {
            out.why = "文字がページから 1 m より遠くにあります";
            return out;
        }
    }
    if (r.w_mm.value() > 1000.0 || r.h_mm.value() > 1000.0) {
        out.why = "段落テキストの枠が 1 m より大きいです";
        return out;
    }
    if (std::abs(a - 1) > 1e-6 || std::abs(d - 1) > 1e-6 || std::abs(b) > 1e-6 || std::abs(c) > 1e-6) {
        nt["transform"] = Json::array({r4(a), r4(b), r4(c), r4(d), r4(ox - (a * ox + c * oy)), r4(oy - (b * ox + d * oy))});
    }
    const std::string aa = antialias_of(t.antialias, t.engine_antialias);
    if (aa != "smooth") nt["antialias"] = aa;
    nt["shaping"] = "opentype";
    // its warp (ワープテキスト: the style's native_warp)
    if (!t.warp.empty() && t.warp != "warpNone" && t.warp.starts_with("warp") && t.warp.size() > 4) {
        std::string style = t.warp.substr(4);
        style[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(style[0])));
        Json warp{{"style", style}, {"bend", clamp4(t.warp_value, -100, 100)}, {"h_distort", clamp4(t.warp_perspective, -100, 100)},
                  {"v_distort", clamp4(t.warp_perspective_other, -100, 100)}};
        if (t.warp_vertical) warp["vertical"] = true;
        try {
            core::vector::check_native_warp(warp);
            r.style["native_warp"] = std::move(warp);
        } catch (const std::exception&) {
            out.notes.push_back("ワープテキストの「" + t.warp + "」は Genko に無いため外しました");
        }
    }
    try {
        r.native_type = core::type_text::check_native_type(nt, r.text);
    } catch (const std::exception& error) {
        out.why = std::string("文字の設定を Genko の文字で表せません（") + error.what() + "）";
        return out;
    }
    out.record = std::move(r);
    return out;
}

std::optional<psd::TextLayer> text_of(const core::StoryLine& record, const core::Document& book, double k,
                                      const std::function<std::string(const std::string&)>& font_name) {
    if (!record.native_type || !record.native_type->is_object()) return std::nullopt;
    const Json& nt = *record.native_type;
    const std::string shape = nt.value("shape", std::string());
    if (shape != "point" && shape != "box") return std::nullopt;
    const Json* styles = book.type_styles ? &*book.type_styles : nullptr;
    psd::TextLayer out;
    out.vertical = record.wrap == "vertical";
    const std::u32string points = core::ruby::code_points(record.text);
    const std::vector<std::size_t> bounds = core::ruby::bounds(points);
    // the book's styles as the named sheets (each run naming the one it takes by its place)
    std::map<std::string, int> char_place, para_place;
    if (styles != nullptr) {
        if (const Json* list = key(*styles, "character"); list != nullptr && list->is_array()) {
            for (const Json& s : *list) {
                char_place.emplace(s.value("id", std::string()), static_cast<int>(out.char_sheets.size()));
                out.char_sheets.push_back(psd::NamedSheet{psd::utf16(s.value("name", std::string())),
                                                          key(s, "char") != nullptr ? char_keys(s["char"], k, font_name) : Json::object(), 0});
            }
        }
        if (const Json* list = key(*styles, "paragraph"); list != nullptr && list->is_array()) {
            for (const Json& s : *list) {
                para_place.emplace(s.value("id", std::string()), static_cast<int>(out.para_sheets.size()));
                out.para_sheets.push_back(psd::NamedSheet{psd::utf16(s.value("name", std::string())),
                                                          key(s, "para") != nullptr ? para_keys(s["para"], k, out.vertical) : Json::object(), 0});
            }
        }
    }
    // each grapheme's character style (a range's, else the whole text's)
    const auto graphemes = static_cast<std::int64_t>(bounds.size()) - 1;
    std::vector<std::optional<int>> sheet_of(static_cast<std::size_t>(std::max<std::int64_t>(0, graphemes)));
    if (const Json* whole = key(nt, "char_style"); whole != nullptr && whole->is_string()) {
        if (const auto it = char_place.find(whole->get<std::string>()); it != char_place.end()) std::fill(sheet_of.begin(), sheet_of.end(), it->second);
    }
    if (const Json* runs = key(nt, "runs"); runs != nullptr && runs->is_array()) {
        for (const Json& run : *runs) {
            const Json* style = key(run, "style");
            if (style == nullptr || !style->is_string()) continue;
            const auto it = char_place.find(style->get<std::string>());
            for (std::int64_t g = run.value("start", std::int64_t{0}); g < run.value("end", std::int64_t{0}) && g < graphemes; ++g)
                sheet_of[static_cast<std::size_t>(g)] = it != char_place.end() ? std::optional<int>(it->second) : std::nullopt;
        }
    }
    // the letters: each in its resolved look (its font letter by letter: a composite face's kana its own)
    text::Fonts fonts;
    std::vector<psd::TextStyle> each;  // (a UTF-16 unit's)
    const std::vector<core::type_text::CharRun> resolved = core::type_text::resolve_chars(record, styles);
    for (const core::type_text::CharRun& run : resolved) {
        const Json& at = run.attrs;
        psd::TextStyle s;
        for (const auto& [name, value] : at.items()) {  // (Genko's own, for a .pcraft: what is resolved)
            if (!value.is_null()) s.attrs[name] = value;
        }
        const Json* font = key(at, "font");
        const Json* postscript = key(at, "postscript");
        const text::Face face = text::face_of(font != nullptr && font->is_string() ? *font : Json());
        const double size_mm = number_at(at, "size_mm").value_or(5.0) * number_at(at, "scale").value_or(1.0);
        s.size = size_mm * k;
        if (const Json* rgb = key(at, "rgb"); rgb != nullptr && rgb->is_array() && rgb->size() == 3) {
            for (std::size_t i = 0; i < 3; ++i) s.fill[i] = std::clamp((*rgb)[i].is_number() ? (*rgb)[i].get<int>() : 0, 0, 255);
        }
        s.tracking = number_at(at, "tracking").value_or(0.0) * 1000.0;
        if (const Json* leading = key(at, "leading_mm"); leading != nullptr && leading->is_number()) {
            s.leading = leading->get<double>() * k;
        } else {
            s.auto_leading = true;
        }
        s.baseline_shift = number_at(at, "baseline_shift_mm").value_or(0.0) * k;
        s.horizontal_scale = number_at(at, "scale_x").value_or(1.0);
        s.vertical_scale = number_at(at, "scale_y").value_or(1.0);
        s.faux_bold = at.value("faux_bold", std::string("normal")) != "normal";
        s.faux_italic = flag_at(at, "faux_italic").value_or(false);
        s.underline = flag_at(at, "underline").value_or(false);
        s.strikethrough = flag_at(at, "strikethrough").value_or(false);
        const std::string caps = at.value("caps", std::string("normal"));
        s.caps = caps == "all" ? 2 : caps == "small" ? 1 : 0;
        s.auto_kerning = at.value("kerning", std::string("metrics")) != "none";
        s.ligatures = flag_at(at, "ligatures").value_or(true);
        s.dligatures = flag_at(at, "discretionary_ligatures").value_or(false);
        if (const Json* features = key(at, "features"); features != nullptr && features->is_object()) {
            for (std::size_t i = 0; i < psd::kOpenTypeKeys.size(); ++i) {
                const auto it = features->find(psd::kOpenTypeKeys[i].tag);
                if (it != features->end() && it->is_number_integer() && it->get<std::int64_t>() > 0) s.opentype |= 1u << i;
            }
        }
        for (std::int64_t g = run.start; g < run.end && g < graphemes; ++g) {
            s.sheet = sheet_of[static_cast<std::size_t>(g)];
            for (std::size_t p = bounds[static_cast<std::size_t>(g)]; p < bounds[static_cast<std::size_t>(g) + 1]; ++p) {
                char32_t c = points[p];
                std::string name;
                if ((font == nullptr || !font->is_string()) && postscript != nullptr && postscript->is_string()) name = postscript->get<std::string>();
                else name = font_name(fonts.path_for(face, std::u32string(1, c == U'\n' || c < 0x20 ? U' ' : c)));
                s.font = name;
                if (c == U'\n') c = U'\r';
                else if (c < 0x20 || c == 0x7f) c = U' ';
                const std::u16string unit = psd::utf16(core::ruby::to_utf8(std::u32string(1, c)));
                out.text += unit;
                each.insert(each.end(), unit.size(), s);
            }
        }
    }
    if (out.text.empty()) return std::nullopt;
    each.push_back(each.back());  // (the text engine's own "\r" at the end)
    for (const psd::TextStyle& s : each) {
        if (!out.runs.empty() && out.runs.back().style == s) ++out.runs.back().length;
        else out.runs.push_back(psd::StyleRun{1, s});
    }
    // the paragraphs: each its resolved settings, its characters and its break (the last, the text engine's own)
    const std::vector<std::int64_t> starts = core::type_text::paragraph_starts_of(record.text);
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const std::int64_t g0 = starts[i];
        const std::int64_t g1 = i + 1 < starts.size() ? starts[i + 1] : graphemes;
        std::size_t units = 0;
        for (std::int64_t g = g0; g < g1 && g < graphemes; ++g) {
            for (std::size_t p = bounds[static_cast<std::size_t>(g)]; p < bounds[static_cast<std::size_t>(g) + 1]; ++p) units += points[p] >= 0x10000 ? 2 : 1;
        }
        if (i + 1 == starts.size()) units += 1;
        const Json at = core::type_text::resolve_para(record, styles, static_cast<std::int64_t>(i));
        psd::ParagraphRun p;
        p.length = units;
        for (const auto& [name, value] : at.items()) {
            if (!value.is_null()) p.attrs[name] = value;
        }
        const Json* align = key(at, "align");
        p.justification = align != nullptr && align->is_string() ? justification_of(align->get<std::string>()) : 0;
        p.first_indent = number_at(at, "first_indent_mm").value_or(0.0) * k;
        p.start_indent = number_at(at, "start_indent_mm").value_or(0.0) * k;
        p.end_indent = number_at(at, "end_indent_mm").value_or(0.0) * k;
        p.space_before = number_at(at, "space_before_mm").value_or(0.0) * k;
        p.space_after = number_at(at, "space_after_mm").value_or(0.0) * k;
        p.auto_leading = number_at(at, "auto_leading").value_or(1.2);
        p.hyphenate = flag_at(at, "hyphenate").value_or(false);
        // (its style: its own, else the whole text's)
        std::optional<std::string> style;
        if (const Json* whole = key(nt, "para_style"); whole != nullptr && whole->is_string()) style = whole->get<std::string>();
        if (const Json* list = key(nt, "paragraphs"); list != nullptr && list->is_array()) {
            for (const Json& item : *list) {
                if (item.value("index", std::int64_t{-1}) == static_cast<std::int64_t>(i) && key(item, "style") != nullptr && item["style"].is_string())
                    style = item["style"].get<std::string>();
            }
        }
        if (style) {
            if (const auto it = para_place.find(*style); it != para_place.end()) p.sheet = it->second;
        }
        out.paragraphs.push_back(p);
    }
    // where it is: its origin (a point text's anchor, a box's corner) through its matrix, at the page's pixels
    const auto t = core::type_text::transform_of(record).value_or(std::array<double, 6>{1, 0, 0, 1, 0, 0});
    double ox = record.x_mm.value(), oy = record.y_mm.value();
    if (shape == "point") {
        if (const Json* anchor = key(nt, "anchor_mm"); anchor != nullptr && anchor->is_array() && anchor->size() == 2) {
            ox = (*anchor)[0].get<double>();
            oy = (*anchor)[1].get<double>();
        }
        out.bounds = {(record.x_mm.value() - ox) * k, (record.y_mm.value() - oy) * k, (record.x_mm.value() + record.w_mm.value() - ox) * k,
                      (record.y_mm.value() + record.h_mm.value() - oy) * k};
    } else {
        out.box = std::array<double, 4>{0.0, 0.0, record.w_mm.value() * k, record.h_mm.value() * k};
        out.bounds = *out.box;
    }
    out.transform = {t[0], t[1], t[2], t[3], (t[0] * ox + t[2] * oy + t[4]) * k, (t[1] * ox + t[3] * oy + t[5]) * k};
    const auto [anta, engine] = anta_of(nt.value("antialias", std::string("smooth")));
    out.antialias = anta;
    out.engine_antialias = engine;
    if (const Json* warp = key(record.style, "native_warp"); warp != nullptr && warp->is_object() && warp->contains("style") && (*warp)["style"].is_string()) {
        std::string style = (*warp)["style"].get<std::string>();
        if (!style.empty()) {
            style[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(style[0])));
            out.warp = "warp" + style;
            out.warp_value = number_at(*warp, "bend").value_or(50.0);
            out.warp_perspective = number_at(*warp, "h_distort").value_or(0.0);
            out.warp_perspective_other = number_at(*warp, "v_distort").value_or(0.0);
            out.warp_vertical = flag_at(*warp, "vertical").value_or(false);
        }
    }
    return out;
}

// --- .pcraft (PhotoCraft's TextLayer and TextStyles, doc/src/text.rs and text_styles.rs: their serde names) ------------

namespace {

constexpr double kPt = 25.4 / 72.0;  // (mm a point)

bool tag_of(const std::string& tag, bool space) {
    return tag.size() == 4 && std::all_of(tag.begin(), tag.end(), [&](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || (space && ch == ' '); });
}

std::array<int, 3> rgb_of_colour(const Json& colour) {
    std::array<int, 3> out{0, 0, 0};
    const Json* c = key(colour, "c");
    if (c == nullptr || !c->is_array() || c->size() < 4) return out;
    const auto at = [&](std::size_t i) { return (*c)[i].is_number() ? std::clamp((*c)[i].get<double>(), 0.0, 1.0) : 0.0; };
    const std::string mode = colour.value("mode", std::string("Rgb"));
    if (mode == "Cmyk") return rgb_of({at(0), at(1), at(2), at(3)});
    if (mode == "Grayscale") {
        const int v = static_cast<int>(std::lround(at(0) * 255.0));
        return {v, v, v};
    }
    for (std::size_t i = 0; i < 3; ++i) out[i] = static_cast<int>(std::lround(at(i) * 255.0));
    return out;
}

Json colour_json(const std::array<int, 3>& rgb) {
    return Json{{"mode", "Rgb"}, {"c", Json::array({rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0, 0.0})}, {"alpha", 1.0}};
}

// Upstream's character attributes (a CharStyle, or a style's partial set) as Genko's (only those given).
Json char_from_upstream(const Json& s) {
    Json out = Json::object();
    if (!s.is_object()) return out;
    if (const Json* ps = key(s, "postscript_name"); ps != nullptr && ps->is_string() && !ps->get<std::string>().empty()) {
        if (const auto found = bundled_font(ps->get<std::string>())) out["font"] = *found;
        else if (const std::string token = postscript_token(ps->get<std::string>()); !token.empty()) out["postscript"] = token;
    }
    for (const auto& [from, to] : {std::pair<const char*, const char*>{"font_family", "family"}, {"font_style", "font_style"}}) {
        if (const Json* v = key(s, from); v != nullptr && v->is_string() && !v->get<std::string>().empty()) out[to] = core::ruby::slice(v->get<std::string>(), 0, 256);
    }
    if (const auto v = number_at(s, "size_pt"); v && *v > 0) out["size_mm"] = clamp4(*v * kPt, 0.1, 1000.0);
    if (const Json* c = key(s, "color"); c != nullptr && c->is_object()) {
        const auto rgb = rgb_of_colour(*c);
        out["rgb"] = Json::array({rgb[0], rgb[1], rgb[2]});
    }
    if (const auto v = number_at(s, "tracking")) out["tracking"] = clamp4(*v / 1000.0, -1.0, 10.0);
    if (const Json* v = key(s, "leading_pt")) {
        if (v->is_null()) out["leading_mm"] = "auto";
        else if (v->is_number()) out["leading_mm"] = clamp4(v->get<double>() * kPt, 0.0, 1000.0);
    }
    if (const auto v = number_at(s, "baseline_shift_pt")) out["baseline_shift_mm"] = clamp4(*v * kPt, -1000.0, 1000.0);
    if (const auto v = number_at(s, "horizontal_scale")) out["scale_x"] = clamp4(*v, 0.01, 10.0);
    if (const auto v = number_at(s, "vertical_scale")) out["scale_y"] = clamp4(*v, 0.01, 10.0);
    if (const auto v = flag_at(s, "faux_bold")) out["faux_bold"] = *v ? "bold" : "normal";
    for (const char* name : {"faux_italic", "underline", "strikethrough", "ligatures", "discretionary_ligatures"}) {
        if (const auto v = flag_at(s, name)) out[name] = *v;
    }
    if (const Json* v = key(s, "kerning"); v != nullptr && v->is_string())
        out["kerning"] = *v == "Off" ? "none" : *v == "Optical" ? "optical" : "metrics";
    if (const Json* v = key(s, "caps"); v != nullptr && v->is_string()) out["caps"] = *v == "AllCaps" ? "all" : *v == "SmallCaps" ? "small" : "normal";
    if (const Json* list = key(s, "features"); list != nullptr && list->is_array()) {
        Json features = Json::object();
        for (const Json& f : *list) {
            const std::string tag = f.value("tag", std::string());
            if (tag_of(tag, true) && f.contains("value") && f["value"].is_number_integer() && features.size() < 64)
                features[tag] = std::clamp<std::int64_t>(f["value"].get<std::int64_t>(), 0, 65535);
        }
        if (!features.empty()) out["features"] = std::move(features);
    }
    if (const Json* list = key(s, "variations"); list != nullptr && list->is_array()) {
        Json axes = Json::object();
        for (const Json& f : *list) {
            const std::string axis = f.value("axis", std::string());
            if (tag_of(axis, false) && f.contains("value") && f["value"].is_number() && axes.size() < 64)
                axes[axis] = clamp4(f["value"].get<double>(), -32768.0, 32767.0);
        }
        if (!axes.empty()) out["variations"] = std::move(axes);
    }
    if (const Json* v = key(s, "language"); v != nullptr && v->is_string()) {
        const std::string tag = v->get<std::string>();
        if (!tag.empty() && tag.size() <= 35 && std::all_of(tag.begin(), tag.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '-'; }))
            out["language"] = tag;
    }
    return out;
}

constexpr std::array<std::string_view, 7> kUpstreamAligns{"Left", "Right", "Center", "JustifyLeft", "JustifyRight", "JustifyCenter", "JustifyAll"};

Json para_from_upstream(const Json& s, bool vertical) {
    Json out = Json::object();
    if (!s.is_object()) return out;
    if (const Json* v = key(s, "align"); v != nullptr && v->is_string()) {
        for (std::size_t i = 0; i < kUpstreamAligns.size(); ++i) {
            if (v->get_ref<const std::string&>() == kUpstreamAligns[i]) out["align"] = align_of(static_cast<int>(i), vertical);
        }
    }
    for (const auto& [from, to] : {std::pair<const char*, const char*>{"first_line_indent_pt", "first_indent_mm"}, {"start_indent_pt", "start_indent_mm"},
                                   {"end_indent_pt", "end_indent_mm"}}) {
        if (const auto v = number_at(s, from)) out[to] = clamp4(*v * kPt, -1000.0, 1000.0);
    }
    for (const auto& [from, to] : {std::pair<const char*, const char*>{"space_before_pt", "space_before_mm"}, {"space_after_pt", "space_after_mm"}}) {
        if (const auto v = number_at(s, from)) out[to] = clamp4(*v * kPt, 0.0, 1000.0);
    }
    if (const auto v = number_at(s, "auto_leading")) out["auto_leading"] = clamp4(*v, 0.0, 5.0);
    if (const Json* v = key(s, "direction"); v != nullptr && v->is_string()) out["direction"] = *v == "Rtl" ? "rtl" : *v == "Ltr" ? "ltr" : "auto";
    if (const auto v = flag_at(s, "hyphenate")) out["hyphenate"] = *v;
    return out;
}

// Genko's attributes as upstream's partial sets (a style's).
Json char_to_upstream(const Json& a, const std::function<std::pair<std::string, std::string>(const std::string&)>& family) {
    Json out = Json::object();
    if (const Json* font = key(a, "font"); font != nullptr && font->is_string()) {
        const text::Face face = text::face_of(*font);
        const auto [fam, style] = family(face.other);
        if (!fam.empty()) out["font_family"] = fam;
        if (!style.empty()) out["font_style"] = style;
    }
    if (const Json* v = key(a, "family"); v != nullptr && v->is_string()) out["font_family"] = *v;
    if (const Json* v = key(a, "font_style"); v != nullptr && v->is_string()) out["font_style"] = *v;
    if (const Json* v = key(a, "postscript"); v != nullptr && v->is_string()) out["postscript_name"] = *v;
    if (const auto v = number_at(a, "size_mm")) out["size_pt"] = *v / kPt;
    if (const Json* rgb = key(a, "rgb"); rgb != nullptr && rgb->is_array() && rgb->size() == 3)
        out["color"] = colour_json({(*rgb)[0].get<int>(), (*rgb)[1].get<int>(), (*rgb)[2].get<int>()});
    if (const auto v = number_at(a, "tracking")) out["tracking"] = *v * 1000.0;
    if (const Json* v = key(a, "leading_mm")) out["leading_pt"] = v->is_number() ? Json(v->get<double>() / kPt) : Json();
    if (const auto v = number_at(a, "baseline_shift_mm")) out["baseline_shift_pt"] = *v / kPt;
    if (const auto v = number_at(a, "scale_x")) out["horizontal_scale"] = *v;
    if (const auto v = number_at(a, "scale_y")) out["vertical_scale"] = *v;
    if (const Json* v = key(a, "faux_bold"); v != nullptr && v->is_string()) out["faux_bold"] = *v != "normal";
    for (const char* name : {"faux_italic", "underline", "strikethrough", "ligatures", "discretionary_ligatures"}) {
        if (const auto v = flag_at(a, name)) out[name] = *v;
    }
    if (const Json* v = key(a, "kerning"); v != nullptr && v->is_string()) out["kerning"] = *v == "none" ? "Off" : *v == "optical" ? "Optical" : "Metrics";
    if (const Json* v = key(a, "caps"); v != nullptr && v->is_string()) out["caps"] = *v == "all" ? "AllCaps" : *v == "small" ? "SmallCaps" : "Normal";
    if (const Json* f = key(a, "features"); f != nullptr && f->is_object()) {
        Json list = Json::array();
        for (const auto& [tag, value] : f->items()) list.push_back(Json{{"tag", tag}, {"value", value}});
        out["features"] = std::move(list);
    }
    if (const Json* f = key(a, "variations"); f != nullptr && f->is_object()) {
        Json list = Json::array();
        for (const auto& [axis, value] : f->items()) list.push_back(Json{{"axis", axis}, {"value", value}});
        out["variations"] = std::move(list);
    }
    if (const Json* v = key(a, "language"); v != nullptr && v->is_string()) out["language"] = *v;
    return out;
}

Json para_to_upstream(const Json& a) {
    Json out = Json::object();
    if (const Json* align = key(a, "align"); align != nullptr && align->is_string())
        out["align"] = std::string(kUpstreamAligns[static_cast<std::size_t>(justification_of(align->get<std::string>()))]);
    for (const auto& [from, to] : {std::pair<const char*, const char*>{"first_indent_mm", "first_line_indent_pt"}, {"start_indent_mm", "start_indent_pt"},
                                   {"end_indent_mm", "end_indent_pt"}, {"space_before_mm", "space_before_pt"}, {"space_after_mm", "space_after_pt"}}) {
        if (const auto v = number_at(a, from)) out[to] = *v / kPt;
    }
    if (const auto v = number_at(a, "auto_leading")) out["auto_leading"] = *v;
    if (const Json* v = key(a, "direction"); v != nullptr && v->is_string()) out["direction"] = *v == "rtl" ? "Rtl" : *v == "ltr" ? "Ltr" : "Auto";
    if (const auto v = flag_at(a, "hyphenate")) out["hyphenate"] = *v;
    return out;
}

// UTF-8 bytes [from, from + length) of a text — the end at a character's start — as UTF-16 units, and where it ends.
std::size_t units_in(const std::string& text, std::size_t& at, std::size_t length) {
    std::size_t end = std::min(text.size(), at + length);
    while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) ++end;
    std::size_t units = 0;
    for (std::size_t i = at; i < end; ++i) {
        const auto b = static_cast<unsigned char>(text[i]);
        if ((b & 0xc0) != 0x80) units += b >= 0xf0 ? 2 : 1;
    }
    at = end;
    return units;
}

}  // namespace

std::optional<psd::TextLayer> from_pcraft(const Json& c, const Json* text_styles) {
    const Json* text = key(c, "text");
    if (text == nullptr || !text->is_string()) return std::nullopt;
    const std::string utf8 = text->get<std::string>();
    psd::TextLayer out;
    out.text = psd::utf16(utf8);
    out.vertical = c.value("orientation", std::string()) == "Vertical";
    std::map<std::int64_t, int> char_place, para_place;
    if (text_styles != nullptr && text_styles->is_object()) {
        if (const Json* list = key(*text_styles, "character"); list != nullptr && list->is_array()) {
            for (const Json& s : *list) {
                if (!s.is_object() || !s.contains("id") || !s["id"].is_number_integer()) continue;
                char_place.emplace(s["id"].get<std::int64_t>(), static_cast<int>(out.char_sheets.size()));
                out.char_sheets.push_back(psd::NamedSheet{psd::utf16(s.value("name", std::string())),
                                                          Json{{"_attrs", char_from_upstream(s.value("attrs", Json::object()))}}, 0});
            }
        }
        if (const Json* list = key(*text_styles, "paragraph"); list != nullptr && list->is_array()) {
            for (const Json& s : *list) {
                if (!s.is_object() || !s.contains("id") || !s["id"].is_number_integer()) continue;
                para_place.emplace(s["id"].get<std::int64_t>(), static_cast<int>(out.para_sheets.size()));
                out.para_sheets.push_back(psd::NamedSheet{psd::utf16(s.value("name", std::string())),
                                                          Json{{"_attrs", para_from_upstream(s.value("para_attrs", Json::object()), out.vertical)}}, 0});
            }
        }
    }
    const auto sheet_of = [](const Json& style, const std::map<std::int64_t, int>& places) -> std::optional<int> {
        const Json* id = key(style, "style_sheet");
        if (id == nullptr || !id->is_number_integer()) return std::nullopt;
        const auto it = places.find(id->get<std::int64_t>());
        return it == places.end() ? std::nullopt : std::optional<int>(it->second);
    };
    // the runs (none: one of the layer's summary — its family, size and colour)
    Json runs = c.value("runs", Json::array());
    if (!runs.is_array() || runs.empty()) {
        Json summary = Json::object();
        for (const char* k : {"font_family", "size_pt", "color"}) {
            if (c.contains(k)) summary[k] = c[k];
        }
        runs = Json::array({Json{{"len", utf8.size()}, {"style", summary}}});
    }
    std::size_t at = 0;
    for (std::size_t i = 0; i < runs.size() && at < utf8.size(); ++i) {
        const Json& run = runs[i];
        const std::size_t length = i + 1 == runs.size() ? utf8.size() - at : static_cast<std::size_t>(std::max<std::int64_t>(0, run.value("len", std::int64_t{0})));
        const std::size_t units = units_in(utf8, at, length);
        if (units == 0) continue;
        psd::TextStyle s;
        const Json style = run.value("style", Json::object());
        s.attrs = char_from_upstream(style);
        if (s.attrs.empty()) s.attrs["tracking"] = 0;  // (Genko's own all the same: the keys above not taken)
        s.sheet = sheet_of(style, char_place);
        out.runs.push_back(psd::StyleRun{units, std::move(s)});
    }
    Json paras = c.value("paragraphs", Json::array());
    if (!paras.is_array()) paras = Json::array();
    at = 0;
    for (std::size_t i = 0; i < paras.size() && at < utf8.size(); ++i) {
        const Json& run = paras[i];
        const std::size_t length = i + 1 == paras.size() ? utf8.size() - at : static_cast<std::size_t>(std::max<std::int64_t>(0, run.value("len", std::int64_t{0})));
        const std::size_t units = units_in(utf8, at, length);
        if (units == 0) continue;
        psd::ParagraphRun p;
        p.length = units;
        const Json style = run.value("style", Json::object());
        p.attrs = para_from_upstream(style, out.vertical);
        if (p.attrs.empty()) p.attrs["hyphenate"] = false;
        p.sheet = sheet_of(style, para_place);
        out.paragraphs.push_back(std::move(p));
    }
    if (const Json* shape = key(c, "shape"); shape != nullptr && shape->is_object()) {
        if (const Json* box = key(*shape, "Box"); box != nullptr && box->is_object()) {
            const double x = number_at(*box, "x").value_or(0), y = number_at(*box, "y").value_or(0);
            out.box = std::array<double, 4>{x, y, x + number_at(*box, "width").value_or(0), y + number_at(*box, "height").value_or(0)};
        }
    }
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kAntialias{
        {{"None", "none"}, {"Sharp", "sharp"}, {"Crisp", "crisp"}, {"Strong", "strong"}, {"Smooth", "smooth"}, {"Windows", "windows"}, {"WindowsLcd", "windowsLcd"}}};
    const std::string aa = c.value("antialias", std::string("Smooth"));
    for (const auto& [up, own] : kAntialias) {
        if (aa == up) std::tie(out.antialias, out.engine_antialias) = anta_of(own);
    }
    if (const Json* warp = key(c, "warp"); warp != nullptr && warp->is_object()) {
        out.warp = warp->value("style", std::string("warpNone"));
        out.warp_value = number_at(*warp, "value").value_or(0);
        out.warp_perspective = number_at(*warp, "horizontal_distortion").value_or(0);
        out.warp_perspective_other = number_at(*warp, "vertical_distortion").value_or(0);
        out.warp_vertical = flag_at(*warp, "horizontal") == false;
    }
    if (const Json* t = key(c, "transform"); t != nullptr && t->is_object()) {
        if (const Json* m = key(*t, "m"); m != nullptr && m->is_array() && m->size() == 6) {
            for (std::size_t i = 0; i < 6; ++i) out.transform[i] = (*m)[i].is_number() ? (*m)[i].get<double>() : (i == 0 || i == 3 ? 1.0 : 0.0);
        }
    }
    return out;
}

Json to_pcraft(const psd::TextLayer& t, double dpi, const std::function<std::pair<std::string, std::string>(const std::string&)>& family) {
    const double pt = 72.0 / (dpi > 0 ? dpi : 72.0);  // (points a pixel)
    std::u16string text16 = t.text;
    for (char16_t& c : text16) {
        if (c == u'\r') c = u'\n';
    }
    const std::string utf8 = psd::utf8_of(text16);
    // (a stretch of UTF-16 units as UTF-8 bytes)
    std::size_t unit = 0;
    const auto bytes_of = [&](std::size_t units) {
        const std::size_t end = std::min(text16.size(), unit + units);
        const std::size_t n = psd::utf8_of(std::u16string_view(text16).substr(unit, end - unit)).size();
        unit = end;
        return n;
    };
    Json runs = Json::array();
    for (const psd::StyleRun& run : t.runs) {
        const std::size_t len = bytes_of(run.length);
        if (len == 0) continue;
        const psd::TextStyle& s = run.style;
        const Json& a = s.attrs;
        const auto [fam, style] = family(s.font);
        Json c{{"font_family", a.contains("family") ? a["family"] : Json(fam)},
               {"font_style", a.contains("font_style") ? a["font_style"] : Json(style)},
               {"postscript_name", s.font.empty() ? Json() : Json(s.font)},
               {"weight", 400},
               {"italic", false},
               {"size_pt", s.size * pt},
               {"color", colour_json(s.fill)},
               {"tracking", s.tracking},
               {"leading_pt", s.auto_leading ? Json() : Json(s.leading * pt)},
               {"baseline_shift_pt", s.baseline_shift * pt},
               {"horizontal_scale", s.horizontal_scale},
               {"vertical_scale", s.vertical_scale},
               {"underline", s.underline},
               {"strikethrough", s.strikethrough},
               {"faux_bold", s.faux_bold},
               {"faux_italic", s.faux_italic},
               {"kerning", a.value("kerning", std::string()) == "optical" ? "Optical" : s.auto_kerning ? "Metrics" : "Off"},
               {"caps", s.caps == 2 ? "AllCaps" : s.caps == 1 ? "SmallCaps" : "Normal"},
               {"ligatures", s.ligatures},
               {"discretionary_ligatures", s.dligatures},
               {"features", Json::array()},
               {"variations", Json::array()},
               {"language", a.contains("language") ? a["language"] : Json()}};
        const Json up = char_to_upstream(a, family);
        if (up.contains("features")) c["features"] = up["features"];
        if (up.contains("variations")) c["variations"] = up["variations"];
        if (s.sheet) c["style_sheet"] = *s.sheet + 1;
        runs.push_back(Json{{"len", len}, {"style", std::move(c)}});
    }
    unit = 0;
    Json paragraphs = Json::array();
    for (const psd::ParagraphRun& p : t.paragraphs) {
        const std::size_t len = bytes_of(p.length);
        if (len == 0) continue;
        Json s{{"align", std::string(kUpstreamAligns[static_cast<std::size_t>(std::clamp(p.justification, 0, 6))])},
               {"first_line_indent_pt", p.first_indent * pt},
               {"start_indent_pt", p.start_indent * pt},
               {"end_indent_pt", p.end_indent * pt},
               {"space_before_pt", p.space_before * pt},
               {"space_after_pt", p.space_after * pt},
               {"auto_leading", p.auto_leading},
               {"direction", p.attrs.value("direction", std::string()) == "rtl" ? "Rtl" : p.attrs.value("direction", std::string()) == "ltr" ? "Ltr" : "Auto"},
               {"hyphenate", p.hyphenate}};
        if (p.sheet) s["style_sheet"] = *p.sheet + 1;
        paragraphs.push_back(Json{{"len", len}, {"style", std::move(s)}});
    }
    Json shape = "Point";
    if (t.box) shape = Json{{"Box", Json{{"x", (*t.box)[0]}, {"y", (*t.box)[1]}, {"width", (*t.box)[2] - (*t.box)[0]}, {"height", (*t.box)[3] - (*t.box)[1]}}}};
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kAntialias{
        {{"none", "None"}, {"sharp", "Sharp"}, {"crisp", "Crisp"}, {"strong", "Strong"}, {"smooth", "Smooth"}, {"windows", "Windows"}, {"windowsLcd", "WindowsLcd"}}};
    std::string antialias = "Smooth";
    const std::string own = antialias_of(t.antialias, t.engine_antialias);
    for (const auto& [mine, up] : kAntialias) {
        if (own == mine) antialias = up;
    }
    const psd::TextStyle first = t.runs.empty() ? psd::TextStyle{} : t.runs.front().style;
    Json out{{"kind", "text"},
             {"text", utf8},
             {"font_family", runs.empty() ? Json("") : runs.front()["style"]["font_family"]},
             {"size_pt", first.size * pt},
             {"color", colour_json(first.fill)},
             {"transform", Json{{"m", Json::array({t.transform[0], t.transform[1], t.transform[2], t.transform[3], t.transform[4], t.transform[5]})}}},
             {"cache", nullptr},
             {"psd_raw", nullptr},
             {"runs", std::move(runs)},
             {"paragraphs", std::move(paragraphs)},
             {"shape", std::move(shape)},
             {"orientation", t.vertical ? "Vertical" : "Horizontal"},
             {"antialias", antialias},
             {"warp", nullptr}};
    if (t.warp != "warpNone" && !t.warp.empty()) {
        out["warp"] = Json{{"style", t.warp},
                           {"value", t.warp_value},
                           {"horizontal_distortion", t.warp_perspective},
                           {"vertical_distortion", t.warp_perspective_other},
                           {"horizontal", !t.warp_vertical}};
    }
    return out;
}

Json pcraft_text_styles(const core::Document& book) {
    const auto family = [](const std::string& font) -> std::pair<std::string, std::string> {
        try {
            const text::FontInfo info = text::font_info(font, 0, false);
            return {info.family, info.style};
        } catch (const std::exception&) {
            return {};
        }
    };
    Json character = Json::array(), paragraph = Json::array();
    Json basic{{"id", 0}, {"name", "Basic Paragraph"}, {"para_attrs", Json::object()}, {"char_attrs", Json::object()}};
    if (book.type_styles && book.type_styles->is_object()) {
        const Json& styles = *book.type_styles;
        if (const Json* list = key(styles, "character"); list != nullptr && list->is_array()) {
            for (const Json& s : *list)
                character.push_back(Json{{"id", character.size() + 1}, {"name", s.value("name", std::string())},
                                         {"attrs", char_to_upstream(s.value("char", Json::object()), family)}});
        }
        if (const Json* list = key(styles, "paragraph"); list != nullptr && list->is_array()) {
            for (const Json& s : *list)
                paragraph.push_back(Json{{"id", paragraph.size() + 1}, {"name", s.value("name", std::string())},
                                         {"para_attrs", para_to_upstream(s.value("para", Json::object()))},
                                         {"char_attrs", char_to_upstream(s.value("char", Json::object()), family)}});
        }
        if (const Json* b = key(styles, "basic"); b != nullptr && b->is_object()) {
            basic["para_attrs"] = para_to_upstream(b->value("para", Json::object()));
            basic["char_attrs"] = char_to_upstream(b->value("char", Json::object()), family);
        }
    }
    return Json{{"character", std::move(character)}, {"paragraph", std::move(paragraph)}, {"basic", std::move(basic)}, {"psd_raw", nullptr}};
}

namespace {
std::atomic<int> pixels_for_test{0};
}  // namespace

PixelsForTest::PixelsForTest() { ++pixels_for_test; }
PixelsForTest::~PixelsForTest() { --pixels_for_test; }
bool read_as_pixels() { return pixels_for_test.load() > 0; }

std::string fingerprint(const core::StoryLine& record, const core::Document& book, const core::Page& page, std::int64_t dpi, std::int64_t width_px,
                        std::int64_t height_px) {
    // (the styles it takes — Basic Paragraph and those it names — as they are: one redefined, its look not the block's)
    const Json* all = book.type_styles ? &*book.type_styles : nullptr;
    Json styles = Json::array({all != nullptr && all->is_object() && all->contains("basic") ? (*all)["basic"] : Json()});
    if (record.native_type && record.native_type->is_object()) {
        const Json& nt = *record.native_type;
        const auto add = [&](core::type_styles::Kind kind, const Json* id) {
            if (id == nullptr || !id->is_string()) return;
            const Json* s = core::type_styles::find(all, kind, id->get<std::string>());
            styles.push_back(s != nullptr ? *s : Json(id->get<std::string>()));
        };
        add(core::type_styles::Kind::Character, key(nt, "char_style"));
        add(core::type_styles::Kind::Paragraph, key(nt, "para_style"));
        for (const char* list : {"runs", "paragraphs"}) {
            if (const Json* items = key(nt, list); items != nullptr && items->is_array()) {
                for (const Json& item : *items)
                    add(std::string_view(list) == "runs" ? core::type_styles::Kind::Character : core::type_styles::Kind::Paragraph, key(item, "style"));
            }
        }
    }
    // (the page's colour mode: inks of a space and depth, an RGB page of a depth, or 8-bit RGB)
    const std::string mode = page.color ? "ink " + std::to_string(static_cast<int>(page.color->space)) + " " + std::to_string(page.color->depth)
                             : page.rgb ? "rgb " + std::to_string(page.rgb->depth)
                                        : std::string("rgb 8");
    return core::bytes_ref(fingerprint(record, dpi, width_px, height_px) + "\n" + styles.dump() + "\n" + mode);
}

std::string fingerprint(const core::StoryLine& record, std::int64_t dpi, std::int64_t width_px, std::int64_t height_px) {
    Json spans = Json::array();
    if (record.ruby_spans) spans = core::ruby::to_json(*record.ruby_spans);
    Json path;
    if (record.path) {
        path = Json::array();
        for (const core::Point& p : *record.path) path.push_back(Json::array({p.x.json(), p.y.json()}));
    }
    Json style_runs = Json::array();
    for (const auto& [words, style] : record.style_runs) style_runs.push_back(Json::array({words, style}));
    const Json all = Json::array({record.text, record.wrap, record.x_mm.json(), record.y_mm.json(), record.w_mm.json(), record.h_mm.json(),
                                  record.balloon, record.style, record.native_type ? *record.native_type : Json(), Json(record.ruby_runs), spans,
                                  Json(record.emphasis_runs), style_runs, Json(record.tails), path, dpi,
                                  width_px, height_px});
    return core::bytes_ref(all.dump());
}

}  // namespace genko::render::psd_type
