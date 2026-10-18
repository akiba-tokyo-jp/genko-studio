// Photoshop's ActionDescriptor written and read (render/psd_descriptor.hpp): the one writer and reader of the PSD's
// descriptors (PSD-01's text layers and layer effects, PC2's adjustment layers).

#include "render/psd_descriptor.hpp"

#include <cstring>

namespace genko::render::psd::descriptor {

void put_u8(std::string& out, unsigned v) { out += static_cast<char>(v & 0xffU); }
void put_u16(std::string& out, std::uint32_t v) {
    put_u8(out, v >> 8);
    put_u8(out, v);
}
void put_u32(std::string& out, std::uint32_t v) {
    put_u16(out, v >> 16);
    put_u16(out, v & 0xffffU);
}
void put_i32(std::string& out, std::int32_t v) { put_u32(out, static_cast<std::uint32_t>(v)); }
void put_f64(std::string& out, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    put_u32(out, static_cast<std::uint32_t>(bits >> 32));
    put_u32(out, static_cast<std::uint32_t>(bits & 0xffffffffULL));
}

void put_id(std::string& out, std::string_view id) {
    if (id.size() == 4 && id != "warp") {
        put_u32(out, 0);
    } else {
        put_u32(out, static_cast<std::uint32_t>(id.size()));
    }
    out += id;
}

void put_unicode(std::string& out, std::u16string_view text) {
    put_u32(out, static_cast<std::uint32_t>(text.size() + 1));
    for (const char16_t c : text) put_u16(out, c);
    put_u16(out, 0);
}

// --- the writer --------------------------------------------------------------------------------------------------------

void Writer::text(std::string_view key, std::u16string_view value) {
    head(key, "TEXT");
    put_unicode(body_, value);
}
void Writer::enumerated(std::string_view key, std::string_view type, std::string_view value) {
    head(key, "enum");
    put_id(body_, type);
    put_id(body_, value);
}
void Writer::integer(std::string_view key, std::int32_t value) {
    head(key, "long");
    put_i32(body_, value);
}
void Writer::number(std::string_view key, double value) {
    head(key, "doub");
    put_f64(body_, value);
}
void Writer::unit(std::string_view key, std::string_view unit, double value) {
    head(key, "UntF");
    body_ += unit;
    put_f64(body_, value);
}
void Writer::boolean(std::string_view key, bool value) {
    head(key, "bool");
    put_u8(body_, value ? 1 : 0);
}
void Writer::object(std::string_view key, const Writer& value, std::string_view class_id) {
    head(key, "Objc");
    body_ += value.structure(class_id);
}
void Writer::raw(std::string_view key, std::string_view bytes) {
    head(key, "tdta");
    put_u32(body_, static_cast<std::uint32_t>(bytes.size()));
    body_ += bytes;
}
void Writer::numbers(std::string_view key, const std::vector<double>& values) {
    head(key, "VlLs");
    put_u32(body_, static_cast<std::uint32_t>(values.size()));
    for (const double v : values) {
        body_ += "doub";
        put_f64(body_, v);
    }
}
void Writer::object_array(std::string_view key, std::uint32_t count, std::string_view class_id,
                          const std::vector<std::pair<std::string, std::vector<double>>>& lists, std::string_view unit) {
    head(key, "ObAr");
    put_u32(body_, count);
    put_unicode(body_, u"");
    put_id(body_, class_id);
    put_u32(body_, static_cast<std::uint32_t>(lists.size()));
    for (const auto& [name, values] : lists) {
        put_id(body_, name);
        body_ += "UnFl";
        body_ += unit;
        put_u32(body_, static_cast<std::uint32_t>(values.size()));
        for (const double v : values) put_f64(body_, v);
    }
}
std::string Writer::structure(std::string_view class_id) const {
    std::string out;
    put_unicode(out, u"");
    put_id(out, class_id);
    put_u32(out, count_);
    return out + body_;
}
std::string Writer::versioned(std::string_view class_id) const {
    std::string out;
    put_u32(out, 16);
    return out + structure(class_id);
}
void Writer::head(std::string_view key, std::string_view type) {
    put_id(body_, key);
    body_ += type;
    ++count_;
}

// --- the reader --------------------------------------------------------------------------------------------------------

std::string_view Bytes::take(std::size_t n) {
    if (n > left()) throw Broken("it ends early");
    const std::string_view out = data_.substr(at_, n);
    at_ += n;
    return out;
}
std::uint32_t Bytes::u16() {
    const std::string_view b = take(2);
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(b[0])) << 8) | static_cast<unsigned char>(b[1]);
}
std::uint32_t Bytes::u32() {
    const std::uint32_t high = u16();
    return (high << 16) | u16();
}
double Bytes::f64() {
    const std::uint64_t high = u32();
    const std::uint64_t bits = (high << 32) | u32();
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

std::optional<double> Value::numeric() const {
    if (type == "long" || type == "comp") return static_cast<double>(integer);
    if (type == "doub" || type == "UntF" || type == "UnFl") return number;
    return std::nullopt;
}

const Value* Object::get(std::string_view key) const {
    for (const auto& [k, v] : items) {
        if (k == key) return &v;
    }
    return nullptr;
}

std::string read_id(Bytes& b) {
    const std::uint32_t n = b.u32();
    return std::string(b.take(n == 0 ? 4 : n));
}

std::u16string read_unicode(Bytes& b) {
    const std::uint32_t n = b.u32();
    if (n > b.left() / 2) throw Broken("a text is longer than the block");
    std::u16string out;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) out.push_back(static_cast<char16_t>(b.u16()));
    while (!out.empty() && out.back() == u'\0') out.pop_back();
    return out;
}

namespace {

Value read_value(Bytes& b, const std::string& type, int depth) {
    if (depth > 32) throw Broken("descriptors nested too deep");  // (lists in lists as well as objects)
    Value v;
    v.type = type;
    if (type == "TEXT") {
        v.text = read_unicode(b);
    } else if (type == "enum") {
        v.enum_type = read_id(b);
        v.enum_value = read_id(b);
    } else if (type == "long") {
        v.integer = b.i32();
    } else if (type == "comp") {
        const std::uint64_t high = b.u32();
        v.integer = static_cast<std::int64_t>((high << 32) | b.u32());
    } else if (type == "doub") {
        v.number = b.f64();
    } else if (type == "UntF") {
        v.unit = std::string(b.take(4));
        v.number = b.f64();
    } else if (type == "UnFl") {  // (a unit and a list of doubles, as Photoshop writes them in an ObAr)
        v.unit = std::string(b.take(4));
        const std::uint32_t n = b.u32();
        if (n > b.left() / 8) throw Broken("a list is longer than the block");
        for (std::uint32_t i = 0; i < n; ++i) v.numbers.push_back(b.f64());
        if (!v.numbers.empty()) v.number = v.numbers.front();
    } else if (type == "ObAr") {  // (an object array: the count of each list's items, then a structure of the lists)
        v.integer = b.u32();
        v.object = std::make_shared<Object>(read_structure(b, depth + 1));
    } else if (type == "bool") {
        v.boolean = b.u8() != 0;
    } else if (type == "Objc" || type == "GlbO") {
        v.object = std::make_shared<Object>(read_structure(b, depth + 1));
    } else if (type == "VlLs") {
        const std::uint32_t n = b.u32();
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::string item_type(b.take(4));
            v.list.push_back(read_value(b, item_type, depth + 1));
        }
    } else if (type == "tdta" || type == "alis" || type == "Pth ") {
        v.raw = std::string(b.take(b.u32()));
    } else if (type == "type" || type == "GlbC") {
        (void)read_unicode(b);
        v.enum_value = read_id(b);
    } else {
        throw Broken("a descriptor value of type '" + type + "'");
    }
    return v;
}

}  // namespace

Object read_structure(Bytes& b, int depth) {
    if (depth > 32) throw Broken("descriptors nested too deep");
    Object out;
    (void)read_unicode(b);
    out.class_id = read_id(b);
    const std::uint32_t n = b.u32();
    for (std::uint32_t i = 0; i < n; ++i) {
        std::string key = read_id(b);
        const std::string type(b.take(4));
        out.items.emplace_back(std::move(key), read_value(b, type, depth));
    }
    return out;
}

Object read_versioned(std::string_view payload, std::size_t at) {
    if (at > payload.size()) throw Broken("it ends early");
    Bytes b(payload.substr(at));
    if (b.u32() != 16) throw Broken("its descriptor is not of version 16");
    return read_structure(b, 0);
}

}  // namespace genko::render::psd::descriptor

namespace genko::render::psd {

std::u16string utf16(std::string_view s) {
    std::u16string out;
    for (std::size_t at = 0; at < s.size();) {
        const auto lead = static_cast<unsigned char>(s[at]);
        std::uint32_t cp = lead;
        std::size_t n = 1;
        if (lead >= 0xf0) {
            cp = lead & 0x07U;
            n = 4;
        } else if (lead >= 0xe0) {
            cp = lead & 0x0fU;
            n = 3;
        } else if (lead >= 0xc0) {
            cp = lead & 0x1fU;
            n = 2;
        }
        for (std::size_t k = 1; k < n && at + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[at + k]) & 0x3fU);
        at += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00 + (cp & 0x3ff)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string utf8_of(std::u16string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t cp = text[i];
        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] < 0xe000) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (text[i + 1] - 0xdc00);
            ++i;
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    return out;
}

}  // namespace genko::render::psd
