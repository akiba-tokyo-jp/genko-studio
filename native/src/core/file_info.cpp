// file-info@1 and image-grid@1 (core/file_info.hpp).

#include "core/file_info.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <span>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/pyconv.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace {

constexpr std::array<std::string_view, 8> kInfoKeys{"author", "author_title", "description", "keywords",
                                                    "copyright", "copyright_status", "copyright_url", "xmp"};
constexpr std::array<std::string_view, 10> kSourceKeys{"format", "name", "width_px", "height_px", "dpi",
                                                       "depth", "mode", "exif", "xmp", "camera"};
constexpr std::array<std::string_view, 6> kCameraKeys{"make", "model", "iso", "exposure", "aperture", "focal"};
constexpr std::int64_t kSideMax = 1 << 20;  // pixels of a file's side (as a smart object's file)
constexpr std::size_t kNameMax = 255;       // characters of a file's name

template <std::size_t N>
bool in(const std::array<std::string_view, N>& list, std::string_view value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

[[noreturn]] void refuse(const std::string& message) { throw Error("value", message); }

bool is_ref(std::string_view text) {
    return text.size() == 71 && text.starts_with("sha256:") &&
           std::all_of(text.begin() + 7, text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::size_t code_points(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

const Json* get(const Json& object, std::string_view key) {
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

void only_keys(const Json& object, std::span<const std::string_view> known, const std::string& what) {
    for (const auto& [key, value] : object.items()) {
        if (std::find(known.begin(), known.end(), key) == known.end()) refuse(what + " key " + py_repr_str(key) + " is not known to this build");
    }
}

std::string text_of(const Json& object, std::string_view key, const std::string& what) {
    const Json* v = get(object, key);
    if (v == nullptr || v->is_null()) return {};
    if (!v->is_string()) refuse(what + "'s " + std::string(key) + " is text");
    return v->get<std::string>();
}

double number_of(const Json& value, const std::string& what) {
    if (!value.is_number() || value.is_boolean()) refuse(what + " is a number");
    const double v = value.get<double>();
    if (!std::isfinite(v)) refuse(what + " is a finite number");
    return v;
}

// The length of the well-formed UTF-8 sequence at s[i] (Unicode table 3-7), or 0 — and in `valid` how many of its bytes
// were (the lead and the good continuation bytes: the maximal subpart one U+FFFD stands for).
std::size_t utf8_sequence(std::string_view s, std::size_t i, std::size_t& valid) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char lead = byte(i);
    valid = 1;
    if (lead < 0x80) return 1;
    std::size_t need = 0;
    unsigned char lo = 0x80, hi = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        need = 1;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        need = 2;
        if (lead == 0xE0) lo = 0xA0;
        if (lead == 0xED) hi = 0x9F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        need = 3;
        if (lead == 0xF0) lo = 0x90;
        if (lead == 0xF4) hi = 0x8F;
    } else {
        return 0;
    }
    for (std::size_t k = 1; k <= need; ++k) {
        if (i + k >= s.size()) return 0;
        const unsigned char c = byte(i + k);
        if (c < (k == 1 ? lo : 0x80) || c > (k == 1 ? hi : 0xBF)) return 0;
        valid = k + 1;
    }
    return need + 1;
}

bool is_utf8(std::string_view text) {
    std::size_t valid = 0;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t n = utf8_sequence(text, i, valid);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

void check_text(const std::string& text, const std::string& what) {
    if (!is_utf8(text)) refuse(what + " is not UTF-8 text");
    if (code_points(text) > kFileInfoTextMax) refuse(what + " is longer than 2000 characters");
}

void check_asset(const std::string& ref, const std::string& what) {
    if (!ref.empty() && !is_ref(ref)) refuse(what + " is an asset (sha256:…)");
}

}  // namespace

std::string utf8_replaced(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    std::size_t valid = 0;
    for (std::size_t i = 0; i < bytes.size();) {
        const std::size_t n = utf8_sequence(bytes, i, valid);
        if (n != 0) {
            out.append(bytes.substr(i, n));
            i += n;
        } else {
            out += "\xEF\xBF\xBD";
            i += std::max<std::size_t>(valid, 1);
        }
    }
    return out;
}

bool FileInfo::empty() const {
    return author.empty() && author_title.empty() && description.empty() && keywords.empty() && copyright.empty() &&
           copyright_status.empty() && copyright_url.empty() && xmp.empty();
}

Json to_json(const FileInfo& info) {
    Json out = Json::object();
    const auto put = [&](const char* key, const std::string& value) {
        if (!value.empty()) out[key] = value;
    };
    put("author", info.author);
    put("author_title", info.author_title);
    put("description", info.description);
    if (!info.keywords.empty()) out["keywords"] = info.keywords;
    put("copyright", info.copyright);
    put("copyright_status", info.copyright_status);
    put("copyright_url", info.copyright_url);
    put("xmp", info.xmp);
    return out;
}

FileInfo file_info_from_json(const Json& value) {
    const std::string what = "the book's file_info";
    if (!value.is_object()) refuse(what + " is an object");
    only_keys(value, kInfoKeys, what);
    FileInfo info;
    info.author = text_of(value, "author", what);
    info.author_title = text_of(value, "author_title", what);
    info.description = text_of(value, "description", what);
    if (const Json* keywords = get(value, "keywords"); keywords != nullptr && !keywords->is_null()) {
        if (!keywords->is_array()) refuse(what + "'s keywords are a list of text");
        for (const Json& keyword : *keywords) {
            if (!keyword.is_string()) refuse(what + "'s keywords are a list of text");
            info.keywords.push_back(keyword.get<std::string>());
        }
    }
    info.copyright = text_of(value, "copyright", what);
    info.copyright_status = text_of(value, "copyright_status", what);
    info.copyright_url = text_of(value, "copyright_url", what);
    info.xmp = text_of(value, "xmp", what);
    check_file_info(info);
    return info;
}

void check_file_info(const FileInfo& info) {
    const std::string what = "the book's file_info";
    check_text(info.author, what + "'s author");
    check_text(info.author_title, what + "'s author_title");
    check_text(info.description, what + "'s description");
    check_text(info.copyright, what + "'s copyright");
    check_text(info.copyright_url, what + "'s copyright_url");
    if (info.keywords.size() > kKeywordsMax) refuse(what + " has more than 200 keywords");
    for (const std::string& keyword : info.keywords) {
        if (keyword.empty()) refuse(what + "'s keywords are not empty");
        check_text(keyword, what + "'s keyword");
    }
    if (!info.copyright_status.empty() && !in(kCopyrightStatuses, info.copyright_status))
        refuse(what + "'s copyright_status is unknown, copyrighted or public_domain");
    check_asset(info.xmp, what + "'s xmp");
}

// --- XMP ---------------------------------------------------------------------------------------------------------------

namespace {

// The items File Info edits: (the property, its container: Alt, Seq, Bag, or "" for a simple one).
struct XmpField {
    std::string_view prop;
    std::string_view kind;
};
constexpr XmpField kTitle{"dc:title", "Alt"};
constexpr XmpField kCreator{"dc:creator", "Seq"};
constexpr XmpField kDescription{"dc:description", "Alt"};
constexpr XmpField kSubject{"dc:subject", "Bag"};
constexpr XmpField kRights{"dc:rights", "Alt"};
constexpr XmpField kPosition{"photoshop:AuthorsPosition", ""};
constexpr std::string_view kMarked = "xmpRights:Marked";
constexpr std::string_view kWebStatement = "xmpRights:WebStatement";
// (XMP's older prefix of the same namespace: read when the new one is not there, removed when the items are written)
constexpr std::string_view kOldMarked = "xapRights:Marked";
constexpr std::string_view kOldWebStatement = "xapRights:WebStatement";

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}


std::string xml_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// XML's five entities and its character references (&#169;, &#xA9;) as their characters, in one pass (an entity it does
// not know, or a reference to no character, left as written); the bytes that are not UTF-8 one U+FFFD each — a packet's
// text as UTF-8 text, whatever its writer put there.
std::string xml_unescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const std::size_t semi = text[i] == '&' ? text.find(';', i) : std::string_view::npos;
        if (semi == std::string_view::npos || semi - i > 12) {
            out += text[i];
            continue;
        }
        const std::string_view name = text.substr(i + 1, semi - i - 1);
        std::optional<std::uint32_t> cp;
        if (name == "lt") {
            cp = '<';
        } else if (name == "gt") {
            cp = '>';
        } else if (name == "quot") {
            cp = '"';
        } else if (name == "apos") {
            cp = '\'';
        } else if (name == "amp") {
            cp = '&';
        } else if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string_view digits = name.substr(hex ? 2 : 1);
            std::uint32_t v = 0;
            bool ok = !digits.empty();
            for (const char c : digits) {
                const int d = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0 || v > 0x10FFFF) {
                    ok = false;
                    break;
                }
                v = v * (hex ? 16 : 10) + static_cast<std::uint32_t>(d);
            }
            if (ok && v != 0 && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) cp = v;
        }
        if (!cp) {
            out += text[i];
            continue;
        }
        append_utf8(out, *cp);
        i = semi;
    }
    return utf8_replaced(out);
}

std::string trimmed(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::size_t a = 0, b = text.size();
    while (a < b && space(text[a])) ++a;
    while (b > a && space(text[b - 1])) --b;
    return std::string(text.substr(a, b - a));
}

struct Found {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string_view inner;
};

// The element <tag …>…</tag> (its first from `from`), as file_cmds.rs find_element.
std::optional<Found> find_element(std::string_view xmp, std::string_view tag, std::size_t from = 0) {
    const std::string open = "<" + std::string(tag);
    for (std::size_t i = xmp.find(open, from); i != std::string_view::npos; i = xmp.find(open, from)) {
        const std::size_t after = i + open.size();
        const char next = after < xmp.size() ? xmp[after] : '\0';
        if (next == '>' || next == '/' || is_space(next)) {
            const std::size_t gt = xmp.find('>', i);
            if (gt == std::string_view::npos) return std::nullopt;
            if (gt > 0 && xmp[gt - 1] == '/') return Found{i, gt + 1, {}};
            const std::string close = "</" + std::string(tag) + ">";
            const std::size_t c = xmp.find(close, gt);
            if (c == std::string_view::npos) return std::nullopt;
            return Found{i, c + close.size(), xmp.substr(gt + 1, c - gt - 1)};
        }
        from = after;
    }
    return std::nullopt;
}

// The attribute `prop="value"` or `prop='value'` after a space, a tab or a new line (on an rdf:Description; its start
// that space), its first from `from`.
std::optional<Found> find_attr(std::string_view xmp, std::string_view prop, std::size_t from = 0) {
    const std::string key = std::string(prop) + "=";
    for (std::size_t i = xmp.find(key, from); i != std::string_view::npos; i = xmp.find(key, i + 1)) {
        const std::size_t q = i + key.size();
        if (i == 0 || !is_space(xmp[i - 1]) || q >= xmp.size() || (xmp[q] != '"' && xmp[q] != '\'')) continue;
        const std::size_t v1 = xmp.find(xmp[q], q + 1);
        if (v1 == std::string_view::npos) return std::nullopt;
        return Found{i - 1, v1 + 1, xmp.substr(q + 1, v1 - q - 1)};
    }
    return std::nullopt;
}

// The packet without any element or attribute of the property, in one pass over it.
std::string without(std::string_view xmp, std::string_view prop) {
    std::string kept;
    kept.reserve(xmp.size());
    std::size_t from = 0;
    while (const auto element = find_element(xmp, prop, from)) {
        kept.append(xmp.substr(from, element->start - from));
        from = element->end;
    }
    kept.append(xmp.substr(from));
    std::string out;
    out.reserve(kept.size());
    from = 0;
    while (const auto attr = find_attr(kept, prop, from)) {
        out.append(std::string_view(kept).substr(from, attr->start - from));
        from = attr->end;
    }
    out.append(std::string_view(kept).substr(from));
    return out;
}

// A container's rdf:li values (or a simple property's text).
std::vector<std::string> li_values(std::string_view inner) {
    std::vector<std::string> out;
    std::string_view rest = inner;
    while (const auto li = find_element(rest, "rdf:li")) {
        out.push_back(xml_unescape(trimmed(li->inner)));
        rest = rest.substr(li->end);
    }
    if (out.empty() && inner.find('<') == std::string_view::npos && !trimmed(inner).empty()) out.push_back(xml_unescape(trimmed(inner)));
    return out;
}

std::vector<std::string> values_of(std::string_view xmp, std::string_view prop) {
    if (const auto element = find_element(xmp, prop)) return li_values(element->inner);
    if (const auto attr = find_attr(xmp, prop)) return {xml_unescape(attr->inner)};
    return {};
}

std::string first_of(std::string_view xmp, std::string_view prop) {
    const std::vector<std::string> values = values_of(xmp, prop);
    return values.empty() ? std::string() : values.front();
}

// "a; b" as its items (trimmed, the empty ones left out): the names an author's text joins ("Smith, John" is one).
std::vector<std::string> split_items(std::string_view text) {
    std::vector<std::string> out;
    std::size_t from = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ';') {
            std::string item = trimmed(text.substr(from, i - from));
            if (!item.empty()) out.push_back(std::move(item));
            from = i + 1;
        }
    }
    return out;
}

}  // namespace

FileInfo file_info_from_xmp(std::string_view packet) {
    FileInfo info;
    const std::vector<std::string> creators = values_of(packet, kCreator.prop);
    for (std::size_t i = 0; i < creators.size(); ++i) info.author += (i > 0 ? "; " : "") + creators[i];
    info.author_title = first_of(packet, kPosition.prop);
    info.description = first_of(packet, kDescription.prop);
    for (std::string& keyword : values_of(packet, kSubject.prop)) {
        if (!keyword.empty()) info.keywords.push_back(std::move(keyword));
    }
    info.copyright = first_of(packet, kRights.prop);
    std::string marked = first_of(packet, kMarked);
    if (marked.empty()) marked = first_of(packet, kOldMarked);
    info.copyright_status = marked == "True" ? "copyrighted" : marked == "False" ? "public_domain" : "";
    info.copyright_url = first_of(packet, kWebStatement);
    if (info.copyright_url.empty()) info.copyright_url = first_of(packet, kOldWebStatement);
    return info;
}

std::string xmp_title(std::string_view packet) { return first_of(packet, kTitle.prop); }

std::string write_xmp(std::optional<std::string_view> packet, const FileInfo& info, std::optional<std::string_view> title) {
    std::string x = packet && packet->find("</rdf:RDF>") != std::string_view::npos
                        ? std::string(*packet)
                        : std::string("<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
                                      " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n </rdf:RDF>\n</x:xmpmeta>\n"
                                      "<?xpacket end=\"w\"?>");
    for (const std::string_view prop : {kCreator.prop, kDescription.prop, kSubject.prop, kRights.prop, kPosition.prop, kMarked, kWebStatement, kOldMarked,
                                        kOldWebStatement}) {
        x = without(x, prop);
    }
    if (title) x = without(x, kTitle.prop);  // (no title given: the packet's kept as it was)
    std::string body;
    const auto put = [&](const XmpField& field, const std::vector<std::string>& values) {
        if (values.empty()) return;
        if (field.kind.empty()) {
            body += "   <" + std::string(field.prop) + ">" + xml_escape(values.front()) + "</" + std::string(field.prop) + ">\n";
            return;
        }
        std::string items;
        for (const std::string& v : values) {
            items += field.kind == "Alt" ? "<rdf:li xml:lang=\"x-default\">" + xml_escape(v) + "</rdf:li>" : "<rdf:li>" + xml_escape(v) + "</rdf:li>";
        }
        body += "   <" + std::string(field.prop) + "><rdf:" + std::string(field.kind) + ">" + items + "</rdf:" + std::string(field.kind) + "></" +
                std::string(field.prop) + ">\n";
    };
    const auto one = [](std::string_view text) {
        std::string t = trimmed(text);
        return t.empty() ? std::vector<std::string>{} : std::vector<std::string>{std::move(t)};
    };
    if (title) put(kTitle, one(*title));
    put(kCreator, split_items(info.author));
    put(kDescription, one(info.description));
    std::vector<std::string> keywords;  // (each as it is: set_file_info splits what it is given)
    for (const std::string& keyword : info.keywords) {
        if (std::string item = trimmed(keyword); !item.empty()) keywords.push_back(std::move(item));
    }
    put(kSubject, keywords);
    put(kRights, one(info.copyright));
    put(kPosition, one(info.author_title));
    if (info.copyright_status == "copyrighted") body += "   <xmpRights:Marked>True</xmpRights:Marked>\n";
    if (info.copyright_status == "public_domain") body += "   <xmpRights:Marked>False</xmpRights:Marked>\n";
    if (const std::string url = trimmed(info.copyright_url); !url.empty()) body += "   <xmpRights:WebStatement>" + xml_escape(url) + "</xmpRights:WebStatement>\n";
    if (!body.empty()) {
        const std::string description =
            "  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:photoshop=\"http://ns.adobe.com/photoshop/1.0/\""
            " xmlns:xmpRights=\"http://ns.adobe.com/xap/1.0/rights/\">\n" +
            body + "  </rdf:Description>\n ";
        const std::size_t at = x.rfind("</rdf:RDF>");
        x.insert(at == std::string::npos ? x.size() : at, description);
    }
    return x;
}

Json to_json(const NativeSource& source) {
    Json out = Json::object();
    out["format"] = source.format;
    out["name"] = source.name;
    out["width_px"] = source.width_px;
    out["height_px"] = source.height_px;
    if (source.dpi) out["dpi"] = Json::array({source.dpi->first, source.dpi->second});
    out["depth"] = source.depth;
    out["mode"] = source.mode;
    if (!source.exif.empty()) out["exif"] = source.exif;
    if (!source.xmp.empty()) out["xmp"] = source.xmp;
    if (source.camera) {
        Json camera = Json::object();
        camera["make"] = source.camera->make;
        camera["model"] = source.camera->model;
        if (source.camera->iso) camera["iso"] = *source.camera->iso;
        if (source.camera->exposure) camera["exposure"] = *source.camera->exposure;
        if (source.camera->aperture) camera["aperture"] = *source.camera->aperture;
        if (source.camera->focal) camera["focal"] = *source.camera->focal;
        out["camera"] = std::move(camera);
    }
    return out;
}

NativeSource native_source_from_json(const Json& value) {
    const std::string what = "a page's native_source";
    if (!value.is_object()) refuse(what + " is an object");
    only_keys(value, kSourceKeys, what);
    NativeSource source;
    source.format = text_of(value, "format", what);
    source.name = text_of(value, "name", what);
    const auto whole = [&](const char* key) {
        const Json* v = get(value, key);
        if (v == nullptr || !v->is_number_integer()) refuse(what + "'s " + key + " is a whole number");
        return v->get<std::int64_t>();
    };
    source.width_px = whole("width_px");
    source.height_px = whole("height_px");
    if (const Json* dpi = get(value, "dpi"); dpi != nullptr && !dpi->is_null()) {
        if (!dpi->is_array() || dpi->size() != 2) refuse(what + "'s dpi is [x, y]");
        source.dpi = std::make_pair(number_of((*dpi)[0], what + "'s dpi"), number_of((*dpi)[1], what + "'s dpi"));
    }
    source.depth = static_cast<int>(whole("depth"));
    source.mode = text_of(value, "mode", what);
    source.exif = text_of(value, "exif", what);
    source.xmp = text_of(value, "xmp", what);
    if (const Json* camera = get(value, "camera"); camera != nullptr && !camera->is_null()) {
        const std::string who = what + "'s camera";
        if (!camera->is_object()) refuse(who + " is an object");
        only_keys(*camera, kCameraKeys, who);
        CameraInfo info;
        info.make = text_of(*camera, "make", who);
        info.model = text_of(*camera, "model", who);
        const auto number = [&](const char* key) -> std::optional<double> {
            const Json* v = get(*camera, key);
            if (v == nullptr || v->is_null()) return std::nullopt;
            return number_of(*v, who + "'s " + key);
        };
        info.iso = number("iso");
        info.exposure = number("exposure");
        info.aperture = number("aperture");
        info.focal = number("focal");
        source.camera = std::move(info);
    }
    check_native_source(source);
    return source;
}

void check_native_source(const NativeSource& source) {
    const std::string what = "a page's native_source";
    if (!in(kSourceFormats, source.format)) refuse(what + "'s format is not one this build knows (" + py_repr_str(source.format) + ")");
    if (source.name.empty() || !is_utf8(source.name) || code_points(source.name) > kNameMax || source.name.find('/') != std::string::npos ||
        source.name.find('\\') != std::string::npos)
        refuse(what + "'s name is a file's name (1..255 characters of UTF-8 text, no folder)");
    if (source.width_px < 1 || source.height_px < 1 || source.width_px > kSideMax || source.height_px > kSideMax)
        refuse(what + "'s size in pixels");
    if (source.dpi && !(source.dpi->first > 0 && source.dpi->first <= 1e6 && source.dpi->second > 0 && source.dpi->second <= 1e6))
        refuse(what + "'s dpi is more than 0 and at most 1000000");
    if (source.depth != 1 && source.depth != 8 && source.depth != 16 && source.depth != 32) refuse(what + "'s depth is 1, 8, 16 or 32 bits");
    if (!in(kSourceModes, source.mode)) refuse(what + "'s mode is not one this build knows (" + py_repr_str(source.mode) + ")");
    check_asset(source.exif, what + "'s exif");
    check_asset(source.xmp, what + "'s xmp");
    if (source.camera) {
        const std::string who = what + "'s camera";
        if (!is_utf8(source.camera->make) || !is_utf8(source.camera->model) || code_points(source.camera->make) > kNameMax ||
            code_points(source.camera->model) > kNameMax)
            refuse(who + "'s make and model are at most 255 characters of UTF-8 text");
        for (const auto& v : {source.camera->iso, source.camera->exposure, source.camera->aperture, source.camera->focal}) {
            if (v && !(*v > 0)) refuse(who + "'s iso, exposure, aperture and focal are more than 0");
        }
    }
}

std::string grid_from_json(const Json& value) {
    if (!value.is_string() || !in(kGrids, value.get_ref<const std::string&>())) refuse("a paper's grid is \"finest\"");
    return value.get<std::string>();
}

bool uses_file_info(const Document& doc) {
    if (doc.file_info && !doc.file_info->empty()) return true;
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (written.page->native_source) return true;
    }
    return false;
}

bool uses_image_grid(const Document& doc) {
    if (doc.spec.grid) return true;
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (written.page->spec.grid) return true;
    }
    return false;
}

void validate_file_info(const Document& doc) {
    if (doc.file_info) check_file_info(*doc.file_info);
    const auto grid = [](const PageSpec& spec) {
        if (spec.grid && !in(kGrids, *spec.grid)) refuse("a paper's grid is \"finest\"");
    };
    grid(doc.spec);
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        grid(written.page->spec);
        if (written.page->native_source) check_native_source(*written.page->native_source);
    }
}

std::vector<std::pair<std::string, std::string_view>> file_info_assets(const Document& doc) {
    std::vector<std::pair<std::string, std::string_view>> out;
    std::set<std::string> seen;
    const auto add = [&](const std::string& ref, std::string_view suffix) {
        if (!ref.empty() && seen.insert(ref + std::string(suffix)).second) out.emplace_back(ref, suffix);
    };
    if (doc.file_info) add(doc.file_info->xmp, kXmpSuffix);
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (const auto& source = written.page->native_source) {
            add(source->exif, kExifSuffix);
            add(source->xmp, kXmpSuffix);
        }
    }
    return out;
}

}  // namespace genko::core
