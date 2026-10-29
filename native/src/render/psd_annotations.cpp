// PC5 J-c (render/psd_annotations.hpp): a PSD's notes (the global block Anno) as the page's — PhotoCraft v0.2.0's
// crates/io/src/annotations_map.rs (MIT, native/third_party/photocraft/NOTICE.md), its places in mm.

#include "render/psd_annotations.hpp"

#include <algorithm>
#include <exception>
#include <cmath>
#include <cstdint>
#include <set>

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/pynum.hpp"
#include "render/ops_annotations.hpp"
#include "render/psd_descriptor.hpp"

namespace genko::render::psd_annotations {

namespace {

struct Reader {
    std::string_view d;
    std::size_t p = 0;
    bool ok = true;

    std::string_view take(std::size_t n) {
        if (!ok || n > d.size() - p) {
            ok = false;
            return {};
        }
        const std::string_view out = d.substr(p, n);
        p += n;
        return out;
    }
    std::uint32_t u(std::size_t n) {
        const std::string_view b = take(n);
        std::uint32_t v = 0;
        for (const char c : b) v = (v << 8) | static_cast<unsigned char>(c);
        return v;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(u(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(u(2)); }
    std::uint32_t u32() { return u(4); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    // (a Pascal string padded to an even length, one byte a character: Latin-1)
    std::string pascal() {
        const std::size_t n = u8();
        const std::string_view s = take(n);
        if ((n + 1) % 2 == 1) take(1);
        std::string out;
        for (const char c : s) {
            const auto b = static_cast<unsigned char>(c);
            if (b < 0x80) {
                out += c;
            } else {
                out += static_cast<char>(0xC0 | (b >> 6));
                out += static_cast<char>(0x80 | (b & 0x3F));
            }
        }
        return out;
    }
};

void put16(std::string& out, std::uint32_t v) {
    out += static_cast<char>((v >> 8) & 0xff);
    out += static_cast<char>(v & 0xff);
}
void put32(std::string& out, std::uint32_t v) {
    for (int k = 3; k >= 0; --k) out += static_cast<char>((v >> (8 * k)) & 0xff);
}

void put_pascal(std::string& out, const std::string& utf8) {
    std::string bytes;
    const std::u16string units = psd::utf16(utf8);
    for (std::size_t i = 0; i < units.size() && bytes.size() < 255; ++i) {
        const char16_t c = units[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units.size()) ++i;  // (a pair: one character)
        bytes += c < 256 ? static_cast<char>(c) : '?';
    }
    out += static_cast<char>(bytes.size());
    out += bytes;
    if ((bytes.size() + 1) % 2 == 1) out += '\0';
}

int unit(double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

// A PSD colour (its space, 4 × u16) as RGB 0..255 (PhotoCraft's color_from).
std::array<int, 3> color_from(std::uint16_t space, const std::array<std::uint16_t, 4>& c) {
    const auto f = [](std::uint16_t v) { return v / 65535.0; };
    switch (space) {
    case 1: {  // HSB, the hue 0..65535 for 0..360°
        const double h = f(c[0]) * 6.0, s = f(c[1]), v = f(c[2]);
        const int i = ((static_cast<int>(std::floor(h)) % 6) + 6) % 6;
        const double fr = h - std::floor(h);
        const double p = v * (1 - s), q = v * (1 - s * fr), t = v * (1 - s * (1 - fr));
        const std::array<std::array<double, 3>, 6> rgb{{{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}}};
        const auto& o = rgb[static_cast<std::size_t>(i)];
        return {unit(o[0]), unit(o[1]), unit(o[2])};
    }
    case 2: {  // CMYK (inverted: 0 all ink), simply
        const double k = 1 - f(c[3]);
        return {unit(f(c[0]) * (1 - k)), unit(f(c[1]) * (1 - k)), unit(f(c[2]) * (1 - k))};
    }
    case 8: {  // grey 0..10000
        const int g = unit(1 - std::clamp(c[0] / 10000.0, 0.0, 1.0));
        return {g, g, g};
    }
    default:
        return {unit(f(c[0])), unit(f(c[1])), unit(f(c[2]))};
    }
}

std::string text_of(std::string_view data) {
    std::string out;
    if (data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0xFE && static_cast<unsigned char>(data[1]) == 0xFF) {
        std::u16string units;
        for (std::size_t i = 2; i + 1 < data.size(); i += 2)
            units += static_cast<char16_t>((static_cast<unsigned char>(data[i]) << 8) | static_cast<unsigned char>(data[i + 1]));
        for (std::size_t i = 0; i < units.size(); ++i) {  // (the review's P2-1: a surrogate without its pair: U+FFFD)
            const char16_t u = units[i];
            if (u >= 0xD800 && u < 0xDC00 && i + 1 < units.size() && units[i + 1] >= 0xDC00 && units[i + 1] < 0xE000) {
                ++i;
            } else if (u >= 0xD800 && u < 0xE000) {
                units[i] = 0xFFFD;
            }
        }
        out = psd::utf8_of(units);
    } else {
        for (const char c : data) {
            const auto b = static_cast<unsigned char>(c);
            if (b < 0x80) {
                out += c;
            } else {
                out += static_cast<char>(0xC0 | (b >> 6));
                out += static_cast<char>(0x80 | (b & 0x3F));
            }
        }
    }
    while (!out.empty() && out.back() == '\0') out.pop_back();
    std::string lines;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r') {
            lines += '\n';
            if (i + 1 < out.size() && out[i + 1] == '\n') ++i;
        } else {
            lines += out[i];
        }
    }
    return lines;
}

std::string text_data(const std::string& text) {
    std::string lines;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        lines += text[i] == '\n' ? '\r' : text[i];
    }
    std::string out("\xFE\xFF", 2);
    for (const char16_t u : psd::utf16(lines)) put16(out, u);
    return out;
}

// days since 1970-01-01 of a date (Howard Hinnant's days_from_civil)
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

}  // namespace

std::string utc_of_pdf_date(std::string_view pdf) {
    if (pdf.starts_with("D:")) pdf.remove_prefix(2);
    const auto digits = [&](std::size_t at, std::size_t n, int& out) {
        if (pdf.size() < at + n) return false;
        out = 0;
        for (std::size_t i = at; i < at + n; ++i) {
            if (pdf[i] < '0' || pdf[i] > '9') return false;
            out = out * 10 + (pdf[i] - '0');
        }
        return true;
    };
    int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
    if (!digits(0, 4, Y) || !digits(4, 2, M) || !digits(6, 2, D) || !digits(8, 2, h) || !digits(10, 2, m) || !digits(12, 2, s)) return {};
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || s > 60) return {};
    std::int64_t offset = 0;  // (seconds east of UTC)
    if (pdf.size() > 14) {
        const char zone = pdf[14];
        if (zone == '+' || zone == '-') {
            int oh = 0, om = 0;
            if (!digits(15, 2, oh)) return {};
            if (pdf.size() > 17) {
                const std::size_t at = pdf[17] == '\'' ? 18 : 17;
                if (!digits(at, 2, om)) om = 0;
            }
            offset = (zone == '+' ? 1 : -1) * (oh * 3600 + om * 60);
        } else if (zone != 'Z') {
            return {};
        }
    }
    std::int64_t t = days_from_civil(Y, static_cast<unsigned>(M), static_cast<unsigned>(D)) * 86400 + h * 3600 + m * 60 + std::min(s, 59) - offset;
    std::int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    std::int64_t rest = t - days * 86400;
    std::int64_t y = 0;
    unsigned mo = 0, d = 0;
    civil_from_days(days, y, mo, d);
    if (y < 0 || y > 9999) return {};
    const auto two = [](std::int64_t v) { return std::string(1, static_cast<char>('0' + v / 10)) + static_cast<char>('0' + v % 10); };
    std::string year = std::to_string(y);
    year.insert(0, 4 - year.size(), '0');
    return year + "-" + two(mo) + "-" + two(d) + "T" + two(rest / 3600) + ":" + two(rest / 60 % 60) + ":" + two(rest % 60) + "Z";
}

std::string pdf_date_of(std::string_view utc) {
    if (!core::annotation_time(utc)) return {};
    std::string out = "D:";
    for (const char c : utc.substr(0, 19))
        if (c >= '0' && c <= '9') out += c;
    return out + "Z";
}

std::optional<std::vector<FileNote>> parse(std::string_view data) {
    Reader r{data};
    r.u16();
    r.u16();
    const std::uint32_t count = r.u32();
    if (!r.ok || count > (data.size() - 8) / 4 || count > kEntriesMax) return std::nullopt;
    std::vector<FileNote> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t start = r.p;
        const std::uint32_t length = r.u32();
        if (!r.ok || length < 8 || length > data.size() - start) return std::nullopt;
        const std::size_t end = start + length;
        FileNote note;
        note.at = start;
        note.length = length;
        if (r.take(4) != "txtA") {
            note.is_text = false;
            out.push_back(std::move(note));
            r.p = end;
            continue;
        }
        note.open = r.u8() != 0;
        r.u8();
        r.u16();
        const std::int32_t top = r.i32(), left = r.i32();
        r.i32();
        r.i32();
        const std::int32_t pt = r.i32(), pl = r.i32(), pb = r.i32(), pr = r.i32();
        const std::uint16_t space = r.u16();
        std::array<std::uint16_t, 4> c{};
        for (auto& v : c) v = r.u16();
        note.author = r.pascal();
        r.pascal();  // (its name)
        note.modified = utc_of_pdf_date(r.pascal());
        r.u32();
        r.take(4);
        const std::uint32_t n = r.u32();
        const std::string_view text = r.take(n);
        if (!r.ok || r.p > end) return std::nullopt;
        note.at_px = {static_cast<double>(left), static_cast<double>(top)};
        note.popup_px = {static_cast<double>(pl), static_cast<double>(pt), static_cast<double>(pr), static_cast<double>(pb)};
        note.color = color_from(space, c);
        note.text = text_of(text);
        if (note.text.size() > core::kAnnotationTextMax) return std::nullopt;
        out.push_back(std::move(note));
        r.p = end;
    }
    return out;
}

std::string block(const std::vector<FileNote>& notes, std::string_view source) {
    std::string out;
    put16(out, 2);
    put16(out, 1);
    std::uint32_t count = 0;
    std::string items;
    for (const FileNote& n : notes) {
        if (!n.is_text || n.as_read) {
            if (n.at > source.size() || n.length > source.size() - n.at) continue;  // (not of this source)
            items += source.substr(n.at, n.length);
            ++count;
            continue;
        }
        std::string b = "txtA";
        b += static_cast<char>(n.open ? 1 : 0);
        b += static_cast<char>(0x1C);
        put16(b, 1);
        const auto x = static_cast<std::int32_t>(std::lround(n.at_px[0])), y = static_cast<std::int32_t>(std::lround(n.at_px[1]));
        for (const std::int32_t v : {y, x, y + 20, x + 16}) put32(b, static_cast<std::uint32_t>(v));
        const std::array<double, 4>& p = n.popup_px;
        for (const double v : {p[1], p[0], p[3], p[2]}) put32(b, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(v))));
        put16(b, 0);  // (RGB)
        for (const int v : n.color) put16(b, static_cast<std::uint32_t>(std::lround(std::clamp(v, 0, 255) / 255.0 * 65535.0)));
        put16(b, 0);
        put_pascal(b, n.author);
        put_pascal(b, "");
        put_pascal(b, pdf_date_of(n.modified));
        const std::string text = text_data(n.text);
        put32(b, static_cast<std::uint32_t>(text.size() + 12));
        b += "txtC";
        put32(b, static_cast<std::uint32_t>(text.size()));
        b += text;
        put32(items, static_cast<std::uint32_t>(b.size() + 4));
        items += b;
        ++count;
    }
    put32(out, count);
    return out + items;
}

std::vector<core::Annotation> notes_of(const std::vector<FileNote>& notes, const std::array<double, 6>& px_to_mm, const std::vector<core::Annotation>& taken) {
    std::set<std::string> ids;
    for (const core::Annotation& a : taken) ids.insert(a.id);
    const auto mm = [&](double x, double y) {
        return std::array<double, 2>{core::py_round(px_to_mm[0] * x + px_to_mm[2] * y + px_to_mm[4], 4),
                                     core::py_round(px_to_mm[1] * x + px_to_mm[3] * y + px_to_mm[5], 4)};
    };
    std::vector<core::Annotation> out;
    for (const FileNote& n : notes) {
        if (!n.is_text) continue;
        core::Annotation a;
        do {
            a.id = "an_" + core::new_id();
        } while (!ids.insert(a.id).second);
        a.at_mm = mm(n.at_px[0], n.at_px[1]);
        if (n.popup_px != std::array<double, 4>{}) {
            const auto p0 = mm(n.popup_px[0], n.popup_px[1]), p1 = mm(n.popup_px[2], n.popup_px[3]);
            a.popup_mm = std::array<double, 4>{std::min(p0[0], p1[0]), std::min(p0[1], p1[1]), core::py_round(std::fabs(p1[0] - p0[0]), 4),
                                               core::py_round(std::fabs(p1[1] - p0[1]), 4)};
        }
        a.text = n.text;
        a.author = annotations::cut_utf8(n.author, 1024);
        a.color = n.color;
        a.open = n.open;
        a.modified = n.modified;
        a.actor = annotations::ReadingActor::now();  // (the reader: the op's actor, else a person's — the file's)
        out.push_back(std::move(a));
    }
    return out;
}

bool read(core::Page& page, std::string_view data, const psd_production::ReadContext& context) {
    // (the page's own notes, or an earlier PSD's blocks there: kept as it was)
    if (context.page_had || !page.annotations.empty() || context.onto == nullptr || context.width_px <= 0 || context.height_px <= 0) return false;
    const auto notes = parse(data);
    if (!notes) return false;
    const core::vector::Affine& onto = *context.onto;
    // (the file's pixels → its canvas's fractions → the page, mm)
    const std::array<double, 6> px_to_mm{onto[0] / context.width_px, onto[1] / context.width_px, onto[2] / context.height_px,
                                         onto[3] / context.height_px, onto[4], onto[5]};
    std::vector<core::Annotation> made = notes_of(*notes, px_to_mm, {});
    if (made.empty() || made.size() > core::kAnnotationsMax) return false;
    // (the review's P1-2: as the book's reader takes them — pulled inside the paper; P2-1: one that cannot be written is
    // not read, the block kept as it is)
    for (core::Annotation& a : made) annotations::pull_inside(a, page.spec.width_mm.value(), page.spec.height_mm.value());
    try {
        core::check_annotations(made);
        (void)psd_production::fingerprint_of(core::to_json(made));
    } catch (const std::exception&) {
        return false;
    }
    if (context.report != nullptr) (*context.report)["annotations"] = made.size();
    page.annotations = std::move(made);
    return true;
}

std::string fingerprint(const core::Page& page, const core::Document* doc) {
    (void)doc;
    if (page.annotations.empty()) return {};
    return psd_production::fingerprint_of(core::to_json(page.annotations));
}

std::optional<std::string> write(const core::Page& page, const psd_production::WriteContext& context) {
    std::vector<FileNote> notes;
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    const double sx = w > 0 && context.width_px > 0 ? context.width_px / w : 1.0, sy = h > 0 && context.height_px > 0 ? context.height_px / h : 1.0;
    for (const core::Annotation& a : page.annotations) {
        FileNote n;
        n.open = a.open;
        n.at_px = {a.at_mm[0] * sx, a.at_mm[1] * sy};
        const std::array<double, 4> box = annotations::popup_of(a);
        n.popup_px = {box[0] * sx, box[1] * sy, (box[0] + box[2]) * sx, (box[1] + box[3]) * sy};
        n.color = a.color;
        n.author = a.author;
        n.modified = a.modified;
        n.text = a.text;
        notes.push_back(std::move(n));
    }
    // (the kept block after the page's notes: of a block read onto the page — marked —, its sound notes (PhotoCraft's);
    // of one not read (the review's P1-1), every note as it was — and none of the page's: the kept block as it is)
    const bool read_onto = page.native_psd && std::any_of(page.native_psd->copied.begin(), page.native_psd->copied.end(),
                                                          [](const core::PsdCopied& c) { return c.resource < 0 && c.global == kKey; });
    if (!read_onto && notes.empty()) return std::nullopt;
    std::optional<std::string> kept;
    if (context.kept_global) kept = context.kept_global(kKey);
    if (kept) {
        if (auto entries = parse(*kept)) {
            for (FileNote& e : *entries) {
                if (e.is_text && read_onto) continue;
                e.as_read = true;
                notes.push_back(std::move(e));
            }
        }
    }
    if (notes.empty()) return std::nullopt;
    return block(notes, kept ? std::string_view(*kept) : std::string_view());
}

}  // namespace genko::render::psd_annotations
