#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "core/json.hpp"
#include "render/draw.hpp"

// Typefaces for lettering (Python's genko/fonts.py): a face gives the font for each character, so a composite face
// mixes two fonts (アンチック: kana and punctuation in the Mincho, kanji, digits and Latin in the Gothic), and falls
// back to the other half, then the bundled Gothic and the effects face for a character its font lacks. The fonts
// are Pillow's (render::TrueTypeFont) under the BASIC layout the reference is held to.

namespace genko::render::text {

// Python's str as a sequence of code points, and back.
std::u32string u32(std::string_view utf8);
std::string utf8(std::u32string_view text);
// str.isspace() of one character (Unicode whitespace as CPython's _PyUnicode_IsWhitespace), and of a text (not
// empty, every character whitespace).
bool py_isspace(char32_t c);
bool py_isspace(std::u32string_view text);

// A face: its key ("antique", …, or a font file's path) and the fonts of its kana and of everything else, each a
// name render::TrueTypeFonts opens (a bundled font's name, "gothic", "mincho", …, or a font file's path).
struct Face {
    std::string key;
    std::string kana;
    std::string other;
    bool composite() const { return kana != other; }
};

inline constexpr std::string_view kDefaultDialogue = "antique";
inline constexpr std::string_view kDefaultSfx = "sfx";

// fonts.face(spec, default): a bundled key, a font file's path (a file that exists), or null (the default). A spec
// that is neither gives the default face. Python's TypeError for a spec that is not a str.
Face face_of(const core::Json& spec, std::string_view default_key = kDefaultDialogue);

// fonts.is_kana_like(char): the first character is kana, the long vowel mark or Japanese punctuation.
bool is_kana_like(std::u32string_view ch);

// The fonts of one piece of lettering: Python's lru_cache'd fonts.truetype and fonts.has_glyph.
class Fonts {
public:
    explicit Fonts(std::stop_token stop = {});

    // fonts.truetype(path, size): the font at max(6, size) px.
    const TrueTypeFont& truetype(const std::string& path, std::int64_t size);
    // fonts.has_glyph(path, char): the font draws the character with something other than its "no glyph" box (a
    // character drawn as nothing counts when it is whitespace).
    bool has_glyph(const std::string& path, std::u32string_view ch);
    // Face.font(size, char)
    const TrueTypeFont& font(const Face& face, std::int64_t size, std::u32string_view ch = U"漢");
    // Face.normalize(text): characters the face lacks swapped for look-alikes it has (― → —, ～ → 〜 …).
    std::u32string normalize(const Face& face, std::u32string_view text);

    std::stop_token stop() const { return fonts_.stop(); }

private:
    TrueTypeFonts fonts_;
    std::map<std::pair<std::string, std::u32string>, bool> glyphs_;
};

}  // namespace genko::render::text
