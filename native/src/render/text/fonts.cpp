// genko/fonts.py: Face (bundled keys, composite kana/other fonts, the fallback for a character a font lacks,
// LOOKALIKE), has_glyph and truetype, over render::TrueTypeFonts.

#include "render/text/fonts.hpp"

#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <array>

#include "core/error.hpp"
#include "core/pyconv.hpp"

namespace genko::render::text {

std::u32string u32(std::string_view utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    const auto bad = []() { return core::Error("value", "invalid UTF-8 text"); };
    while (i < utf8.size()) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        std::size_t n = 0;
        char32_t cp = 0;
        if (c < 0x80) {
            n = 1;
            cp = c;
        } else if ((c >> 5) == 6) {
            n = 2;
            cp = c & 0x1f;
        } else if ((c >> 4) == 14) {
            n = 3;
            cp = c & 0x0f;
        } else if ((c >> 3) == 30) {
            n = 4;
            cp = c & 0x07;
        } else {
            throw bad();
        }
        if (i + n > utf8.size()) throw bad();
        for (std::size_t k = 1; k < n; ++k) {
            const auto d = static_cast<unsigned char>(utf8[i + k]);
            if ((d >> 6) != 2) throw bad();
            cp = (cp << 6) | (d & 0x3f);
        }
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::string utf8(std::u32string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char32_t cp : text) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    return out;
}

bool py_isspace(char32_t c) {
    return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

bool py_isspace(std::u32string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char32_t c) { return py_isspace(c); });
}

namespace {

// BUNDLED: key → (kana/punctuation font, everything else)
struct Bundled {
    std::string_view key;
    std::string_view kana;
    std::string_view other;
};
constexpr std::array<Bundled, 7> kBundled{{{"antique", "mincho", "gothic"},
                                           {"gothic", "gothic", "gothic"},
                                           {"mincho", "mincho", "mincho"},
                                           {"maru", "maru", "maru"},
                                           {"hand", "hand", "hand"},
                                           {"sfx", "sfx", "sfx"},
                                           {"sfx_pop", "sfx_pop", "sfx_pop"}}};

const Bundled* bundled(std::string_view key) {
    for (const Bundled& b : kBundled) {
        if (b.key == key) return &b;
    }
    return nullptr;
}

constexpr std::string_view kGothic = "gothic";  // fonts.GOTHIC
constexpr std::string_view kSfx = "sfx";        // fonts.SFX

// LOOKALIKE
constexpr std::array<std::pair<char32_t, char32_t>, 6> kLookalike{{{U'―', U'—'},
                                                                   {U'～', U'〜'},
                                                                   {U'〜', U'～'},
                                                                   {U'−', U'－'},
                                                                   {U'‐', U'-'},
                                                                   {U'∼', U'〜'}}};

}  // namespace

Face face_of(const core::Json& spec_in, std::string_view default_key) {
    // spec = spec or default
    const core::Json spec = core::py_truthy(spec_in) ? spec_in : core::Json(std::string(default_key));
    if (spec.is_array() || spec.is_object()) throw core::PyTypeError("unhashable type: '" + core::py_type_name(spec) + "'");
    if (!spec.is_string()) {
        throw core::PyTypeError("argument should be a str or an os.PathLike object where __fspath__ returns a str, not '" +
                                core::py_type_name(spec) + "'");
    }
    const std::string& name = spec.get_ref<const std::string&>();
    if (const Bundled* b = bundled(name)) return Face{name, std::string(b->kana), std::string(b->other)};
    if (name.find('\0') == std::string::npos && QFileInfo(QString::fromStdString(name)).isFile()) {
        return Face{name, name, name};
    }
    const Bundled* fallback = bundled(default_key);
    if (fallback == nullptr) fallback = bundled(kDefaultDialogue);
    return Face{std::string(default_key), std::string(fallback->kana), std::string(fallback->other)};
}

bool is_kana_like(std::u32string_view ch) {
    const char32_t code = ch.empty() ? U' ' : ch[0];
    // (`char in "ー〜～…‥"` is a substring test: a whole text of these, or no text at all, counts too)
    return (code >= 0x3040 && code <= 0x30ff) || (code >= 0x31f0 && code <= 0x31ff) || (code >= 0x3000 && code <= 0x303f) ||
           (code >= 0xff61 && code <= 0xff9f) || std::u32string_view(U"ー〜～…‥").find(ch) != std::u32string_view::npos;
}

Fonts::Fonts(std::stop_token stop) : fonts_(std::move(stop)) {}

const TrueTypeFont& Fonts::truetype(const std::string& path, std::int64_t size) {
    return fonts_.truetype(path, static_cast<int>(std::clamp<std::int64_t>(size, 6, 0x10000)));
}

bool Fonts::has_glyph(const std::string& path, std::u32string_view ch) {
    const auto key = std::make_pair(path, std::u32string(ch));
    if (const auto it = glyphs_.find(key); it != glyphs_.end()) return it->second;
    const TrueTypeFont& font = truetype(path, 32);
    const Image mask = font.getmask2(ch).first;
    const auto box = mask.getbbox();
    bool has = false;
    if (!box) {
        has = py_isspace(ch);
    } else {
        // `box != missing or font.getmask(char).tobytes() != font.getmask("\uffff").tobytes()`: the font's "no glyph"
        // box, by its bounds. (The pixels are never compared: Pillow 12's mask, an ImagingCore, has no tobytes(), so
        // when the bounds are the same the AttributeError is caught and the answer is False, even for a real glyph
        // whose ink happens to have the same bounds, such as Dela Gothic's ろ at 32 px.)
        has = box != font.getmask2(std::u32string(1, char32_t{0xFFFF})).first.getbbox();
    }
    glyphs_.emplace(key, has);
    return has;
}

const TrueTypeFont& Fonts::font(const Face& face, std::int64_t size, std::u32string_view ch) {
    const std::string& first = is_kana_like(ch) ? face.kana : face.other;
    if (ch.size() != 1 || has_glyph(first, ch)) return truetype(first, size);
    // a character the face lacks (e.g. ― in some Minchos): the other half, then the bundled Gothic
    const std::string other = first == face.kana ? face.other : face.kana;
    for (const std::string& path : {other, std::string(kGothic), std::string(kSfx)}) {
        if (has_glyph(path, ch)) return truetype(path, size);
    }
    return truetype(first, size);
}

std::u32string Fonts::normalize(const Face& face, std::u32string_view text) {
    std::u32string out;
    out.reserve(text.size());
    for (char32_t ch : text) {
        const auto it = std::find_if(kLookalike.begin(), kLookalike.end(), [&](const auto& p) { return p.first == ch; });
        if (it != kLookalike.end()) {
            const std::u32string one(1, ch);
            const std::u32string alt(1, it->second);
            if (!has_glyph(face.kana, one) && !has_glyph(face.other, one) && has_glyph(face.other, alt)) ch = it->second;
        }
        out.push_back(ch);
    }
    return out;
}

}  // namespace genko::render::text
