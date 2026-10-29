// genko/balloons.py, the text side: style_of, _inner, hug_power / _hug_keep, ink_rects, block_points, fit_in,
// _emphasis, _set_text, _vertical, _horizontal, text_layout, text_image, the letter effects (outlined, skewed,
// arched, gradient_letters, picture_letters, warped_letters), path_text and _paint_text.

#include "render/text/lettering.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

#include <QChar>

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/page_geometry.hpp"
#include "core/ruby.hpp"
#include "core/type_text.hpp"
#include "render/draw.hpp"
#include "render/imaging.hpp"
#include "render/not_yet_ported.hpp"
#include "render/op_limits.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/resample.hpp"
#include "render/text/fonts.hpp"
#include "render/text/memo.hpp"
#include "render/text/opentype.hpp"

namespace genko::render::text {

using core::Json;

namespace {

constexpr double kSqrt2 = 1.4142135623730951;  // SQRT2 = 2 ** 0.5
constexpr double kCapMm = 5.0;                  // CAP_MM
constexpr double kSfxCapMm = 12.0;              // SFX_CAP_MM
constexpr double kFitPad = 0.15;                // FIT_PAD
constexpr double kHandPower = 2.6;              // HAND_POWER
constexpr double kUnbroken = 1.0e6;             // (type@1: a point text's room, px — never broken there)
const Rgb kText{10, 10, 10};                    // TEXT

// OPENING, CLOSING, LINE_START, LINE_END (the rows across)
constexpr std::u32string_view kOpening = U"(‘“〈《「『【〔（［｛";
constexpr std::u32string_view kClosing =
    U")’”、。〉》」』】〕・），．：；］｝";
constexpr std::u32string_view kLineStart =
    U"!)?‥…、。〉》」』】ぁぃぅぇぉっゃゅょ"
    U"ゎァィゥェォッャュョヮー！），．？］";
constexpr std::u32string_view kLineEnd = U"(〈《「『【〔（［";

bool in(std::u32string_view set, char32_t c) { return set.find(c) != std::u32string_view::npos; }
bool one_of(std::string_view kind, std::initializer_list<std::string_view> kinds) {
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

const std::initializer_list<std::string_view> kElliptic{"speech", "cloud", "thought", "shout", "electric", "flash", "whisper"};
const std::initializer_list<std::string_view> kFrames{"dotted_box", "tone_box", "fancy_box"};
const std::initializer_list<std::string_view> kSquare{"box", "narration", "dotted_box", "tone_box", "fancy_box"};
const std::initializer_list<std::string_view> kHugged{"speech", "thought", "whisper", "flash", "shout"};
const std::initializer_list<std::string_view> kUneven{"speech", "thought", "whisper"};
const std::initializer_list<std::string_view> kSpoken{"speech", "rounded", "box",   "cloud", "thought",
                                                      "shout",  "electric", "flash", "whisper"};

// Python's a // b of ints, and of floats (float_floor_div: fmod, then the quotient snapped to a whole number)
std::int64_t floordiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
double float_floordiv(double vx, double wx) {
    if (wx == 0.0) throw core::PyUncaught("ZeroDivisionError", "float floor division by zero");
    double mod = std::fmod(vx, wx);
    double div = (vx - mod) / wx;
    if (mod != 0.0) {
        if ((wx < 0) != (mod < 0)) {
            mod += wx;
            div -= 1.0;
        }
    }
    double floordiv = 0.0;
    if (div != 0.0) {
        floordiv = std::floor(div);
        if (div - floordiv > 0.5) floordiv += 1.0;
    } else {
        floordiv = std::copysign(0.0, vx / wx);
    }
    return floordiv;
}

const Json& at(const Json& st, const char* key) {
    static const Json none;
    const auto it = st.find(key);
    return it == st.end() ? none : *it;
}
// float(value or fallback)
double float_or(const Json& value, double fallback) { return core::py_truthy(value) ? core::to_float(value) : fallback; }
std::string kind_of(const core::StoryLine& line) { return line.balloon.empty() ? std::string("speech") : line.balloon; }
double mm_or(const core::Num& value, double fallback) { return value.truthy() ? value.value() : fallback; }

Image blank(std::int64_t w, std::int64_t h) {
    return Image::create("RGBA", {static_cast<int>(w), static_cast<int>(h)}, Ink{0, 0, 0, 0});
}

// type@1 (PC5 G-a): a page length (mm) as a pixel's place at dpi, rounded (px() keeps a size at least 1)
std::int64_t place_px(double mm, int dpi) { return core::py_round_int(mm / 25.4 * dpi); }

// type@1 (PC5 G-a, render/text/lettering.hpp shape_of): what of native_type places the letters
const Json* native_key(const core::StoryLine& line, const char* key) {
    if (!line.native_type || !line.native_type->is_object()) return nullptr;
    const auto it = line.native_type->find(key);
    return it == line.native_type->end() ? nullptr : &*it;
}

// アンチエイリアス なし (PhotoCraft's text render: a cover of half or more is whole, the rest none)
void binarized(Image& image) {
    if (image.mode() != "RGBA") return;
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<unsigned char*>(image.raw()->image[y]);
        for (int x = 0; x < image.width(); ++x) row[x * 4 + 3] = row[x * 4 + 3] >= 128 ? 255 : 0;
    }
}

// The size of the max filter outlined() grows a halo with: grow · 2 + 1, at most 61 (Python's _outlined), so the halo
// reaches halo_filter(grow) / 2 px round the letters — never more than 30 (PSD-01's stroke is as wide).
std::int64_t halo_filter(std::int64_t grow) { return std::min<std::int64_t>(grow * 2 + 1, 61) | 1; }

// _emphasis(line, face)
std::vector<std::u32string> emphasis_of(Fonts& fonts, const core::StoryLine& line, const Face& face) {
    std::vector<std::u32string> out;
    for (const std::string& run : line.emphasis_runs) {
        if (!run.empty()) out.push_back(fonts.normalize(face, u32(run)));
    }
    return out;
}

// _set_text(line, st, face): a spoken line without its closing 。 (manga leaves it off; style.periods keeps it)
std::u32string set_text(Fonts& fonts, const core::StoryLine& line, const Json& st, const Face& face) {
    std::u32string text = fonts.normalize(face, u32(line.text));
    if (one_of(kind_of(line), kSpoken) && !core::py_truthy(at(st, "periods"))) text = without_periods(text);
    return text;
}

// _set_text with, for each character set, its place in the line's text (code points; npos: none). Only a line holding its
// ruby by place needs it (ruby-layout@1): its readings are set beside the characters that came from their bases.
struct SetText {
    std::u32string text;
    std::vector<std::size_t> from;
};

SetText set_text_mapped(Fonts& fonts, const core::StoryLine& line, const Json& st, const Face& face) {
    SetText out;
    out.text = fonts.normalize(face, u32(line.text));  // (one character for one)
    out.from.resize(out.text.size());
    for (std::size_t i = 0; i < out.from.size(); ++i) out.from[i] = i;
    if (one_of(kind_of(line), kSpoken) && !core::py_truthy(at(st, "periods"))) {
        auto [text, from] = without_periods_mapped(out.text);
        out.text = std::move(text);
        out.from = std::move(from);
    }
    return out;
}

// Each of the line's rubies by place as it is set (by its place in the line's spans): its own settings (core/ruby.hpp),
// the line's look where it gives none (the line's face, its size the line's, centred, beside the text, letter to letter).
// A font of its own that is not there (no bundled face of that name, no such file): the line's face, said.
std::vector<RubySetting> ruby_settings(const core::StoryLine& line, const Face& face, int dpi, std::vector<RubyNote>* notes) {
    std::vector<RubySetting> out;
    for (std::size_t i = 0; i < line.ruby_spans->size(); ++i) {
        const core::RubySpan& span = (*line.ruby_spans)[i];
        RubySetting set;
        const bool found = span.font && face_exists(*span.font);
        set.face = found ? face_of(Json(*span.font)) : face;
        if (span.font && !found && span.review.empty() && notes != nullptr) notes->push_back(RubyNote{"font_fallback", i, "font:" + *span.font});
        set.scale = span.scale;
        set.align = span.align.value_or("center");
        set.gap = core::py_round_int(span.gap_mm.value_or(0.0) / 25.4 * static_cast<double>(dpi));
        set.tracking = span.tracking.value_or(0.0);
        out.push_back(std::move(set));
    }
    return out;
}

// The line's drawn rubies by place as the lettering takes them (code points of the line's text, the reading's
// graphemes); モノルビ: a reading whose graphemes share evenly over its base's becomes one per grapheme of the base.
std::vector<PlacedRuby> placed_of(const core::StoryLine& line, bool mono) {
    std::vector<PlacedRuby> out;
    const std::u32string text = u32(line.text);
    const std::vector<std::size_t> b = core::ruby::bounds(text);
    const auto n = static_cast<std::int64_t>(b.size()) - 1;
    for (std::size_t i = 0; i < line.ruby_spans->size(); ++i) {
        const core::RubySpan& span = (*line.ruby_spans)[i];
        if (!span.review.empty() || span.start < 0 || span.end > n || span.end <= span.start) continue;
        const std::u32string reading = u32(span.ruby);
        const std::vector<std::size_t> rb = core::ruby::bounds(reading);
        std::vector<std::u32string> letters;
        for (std::size_t k = 0; k + 1 < rb.size(); ++k) letters.push_back(reading.substr(rb[k], rb[k + 1] - rb[k]));
        const auto base = static_cast<std::size_t>(span.end - span.start);
        if (mono && base > 1 && letters.size() % base == 0) {
            const std::size_t each = letters.size() / base;
            for (std::size_t g = 0; g < base; ++g) {
                const auto at_g = static_cast<std::size_t>(span.start) + g;
                out.push_back(PlacedRuby{b[at_g], b[at_g + 1],
                                         std::vector<std::u32string>(letters.begin() + static_cast<std::ptrdiff_t>(g * each),
                                                                     letters.begin() + static_cast<std::ptrdiff_t>((g + 1) * each)),
                                         i});
            }
        } else {
            out.push_back(PlacedRuby{b[static_cast<std::size_t>(span.start)], b[static_cast<std::size_t>(span.end)], std::move(letters), i});
        }
    }
    return out;
}

// the default text colour, or style.rgb
Rgb fill_of(const Json& st) { return core::py_truthy(at(st, "rgb")) ? rgb_tuple(at(st, "rgb")) : kText; }

std::size_t cell_count(std::u32string_view text, bool tcy, bool latin) { return cells(text, tcy, latin).size(); }

// _vertical(line, st, face, em, inner_h, fill)
Image vertical_text(Fonts& fonts, const core::StoryLine& line, const Json& st, const Face& face, std::int64_t em, double inner_h,
                    const Rgb& fill, int dpi, std::vector<RubyNote>* notes, std::vector<RubyDrawn>* drawn,
                    const LetterParts* parts = nullptr, std::vector<SetRow>* cells = nullptr) {
    const bool by_place = line.ruby_spans.has_value();
    const bool opentype = sets_opentype(line);  // (PC5 G-b: each cell's glyph as HarfBuzz chooses it, the grid as before)
    SetText set = by_place || opentype ? set_text_mapped(fonts, line, st, face) : SetText{set_text(fonts, line, st, face), {}};
    std::u32string text = std::move(set.text);
    std::vector<std::size_t> from = std::move(set.from);
    const double tracking = float_or(at(st, "tracking"), 0);
    const double leading = float_or(at(st, "leading"), 0);
    const bool latin = at(st, "latin") != Json("upright");
    const bool tcy = core::py_truthy(at(st, "tcy"));
    const double step = static_cast<double>(em) * (1 + tracking);
    const double column = inner_h;
    if (text.find(U'\n') == std::u32string::npos && em > 0) {
        // broken between phrases where a column is full (まんが作りの／モヤモヤを／…), not after so many letters
        const auto count = static_cast<std::int64_t>(cell_count(text, tcy, latin));
        const std::int64_t fit =
            std::max<std::int64_t>(1, core::py_trunc_int(float_floordiv(inner_h - static_cast<double>(em), step)) + 1);
        if (count > fit) {
            std::u32string joined;
            const auto cols = phrase_columns(text, fit, [&](std::u32string_view part) {
                return static_cast<std::int64_t>(cell_count(part, tcy, latin));
            });
            // (the columns are the text without its whitespace at either end, in order: where each character came from
            // follows it, a line break put between them comes from nowhere)
            std::size_t lead = 0;
            while (lead < text.size() && py_isspace(text[lead])) ++lead;
            std::vector<std::size_t> joined_from;
            std::size_t next = lead;
            for (std::size_t i = 0; i < cols.size(); ++i) {
                if (i > 0) {
                    joined.push_back(U'\n');
                    joined_from.push_back(std::u32string::npos);
                }
                joined += cols[i];
                for (std::size_t k = 0; k < cols[i].size(); ++k, ++next) {
                    joined_from.push_back((by_place || opentype) && next < from.size() && text[next] == cols[i][k] ? from[next] : std::u32string::npos);
                }
            }
            text = std::move(joined);
            from = std::move(joined_from);
        }
    }
    (void)fonts.font(face, em);  // (compose's `font`, face.font(em): not drawn with when there is a face)
    ComposeArgs args;
    args.text = std::move(text);
    args.em = em;
    args.max_height = std::max<std::int64_t>(em, core::py_trunc_int(column + static_cast<double>(em) * 0.1));
    args.fill = fill;
    args.ruby_runs = line.ruby_runs;
    args.tracking = tracking;
    args.leading = leading;
    args.tcy = tcy;
    args.align = core::py_str(core::py_or(at(st, "align"), Json("top")));
    args.latin = latin;
    args.emphasis_runs = emphasis_of(fonts, line, face);
    args.emphasis_mark = core::py_str(core::py_or(at(st, "emphasis_mark"), Json("sesame")));
    args.style_runs = line.style_runs;
    args.bold = line_weight(st);
    args.ruby_scale = float_or(at(st, "ruby_scale"), 0.5);
    args.mono_ruby = core::py_truthy(at(st, "mono_ruby"));
    // PC5 G-b: a record set by the OpenType typesetting — each letter's size, faux bold and colour as its attributes say,
    // and each one-letter cell's glyph as HarfBuzz chooses it down (vert, vrt2 and the record's features: jp78 …)
    std::vector<Json> attrs;  // (each letter of the text without its line breaks: its attributes)
    if (opentype) {
        std::u32string letters;
        std::vector<std::size_t> letters_from;
        for (std::size_t i = 0; i < args.text.size(); ++i) {
            if (args.text[i] == U'\n') continue;
            letters.push_back(args.text[i]);
            letters_from.push_back(i < from.size() ? from[i] : std::u32string::npos);
        }
        attrs = resolve(line, letters, letters_from, TypeStyles::current()).chars;
        // (a letter's size_mm against the line's own size — its style's, else the lines' 5 mm — as across: review P3-12)
        const double base_mm = float_or(at(st, "size_mm"), kCapMm);
        const std::vector<Json> words = char_styles(letters, line.style_runs, Json());
        for (std::size_t k = 0; k < attrs.size(); ++k) {
            const Json& a = attrs[k];
            const Json& size_mm = at(a, "size_mm");
            const Json& own_scale = at(a, "scale");
            const double word = k < words.size() && words[k].is_object() && words[k].contains("scale") ? core::to_float(words[k]["scale"]) : 1.0;
            const double scale = (own_scale.is_number() ? own_scale.get<double>() : 1.0) * (size_mm.is_number() && base_mm > 0 ? size_mm.get<double>() / base_mm : 1.0) * word;
            Json look = Json::object();
            if (std::fabs(scale - 1.0) > 1e-9) look["scale"] = scale;
            const Json& faux = at(a, "faux_bold");
            const int level = faux == Json("heavy") ? 2 : faux == Json("bold") ? 1 : 0;
            // (the weight: a word's own where Genko's words say it, else the record's resolved one — which holds the line's:
            // a letter set normal in a bold line drawn normal, as across; review P3-1)
            const bool word_bold = k < words.size() && words[k].is_object() && words[k].contains("bold");
            if (!word_bold && level != args.bold) look["bold"] = level;
            if (at(a, "rgb").is_array() && !(k < words.size() && core::py_truthy(at(words[k], "rgb")))) look["rgb"] = at(a, "rgb");
            args.looks.push_back(std::move(look));
        }
        const Json line_font = at(st, "font");
        args.opentype_cell = [&fonts, &face, &attrs, line_font](std::size_t letter, std::u32string_view cell, std::int64_t size, const Rgb& rgb,
                                                                 int bold) -> std::optional<Image> {
            if (letter >= attrs.size() || cell.empty()) return std::nullopt;
            const Json& a = attrs[letter];
            Face f = face;
            if (const Json& font = at(a, "font"); font.is_string() && font != line_font) f = face_of(font, kDefaultDialogue);
            const std::string path = fonts.readable(fonts.path_for(f, cell.substr(0, 1)));
            std::map<std::string, std::uint32_t> features{{"vert", 1}, {"vrt2", 1}};
            if (at(a, "proportional") == Json(true)) features["vpal"] = 1;
            if (const Json& own = at(a, "features"); own.is_object()) {
                for (const auto& [tag, value] : own.items()) {
                    if (tag.size() == 4 && value.is_number_integer()) features[tag] = static_cast<std::uint32_t>(value.get<std::int64_t>());
                }
            }
            ShapeRequest request;
            request.text = utf8(cell);
            request.font = path;
            request.size_px = static_cast<double>(std::max<std::int64_t>(1, size));
            request.direction = "ttb";
            if (const Json& language = at(a, "language"); language.is_string()) request.language = language.get<std::string>();
            for (const auto& [tag, value] : features) request.features.emplace_back(tag, value);
            if (const Json& axes = at(a, "variations"); axes.is_object()) {
                for (const auto& [tag, value] : axes.items()) {
                    if (value.is_number()) request.variations.emplace_back(tag, value.get<float>());
                }
            }
            const std::vector<ShapedGlyph> glyphs = shape(request);
            if (glyphs.size() != 1 || glyphs.front().glyph == 0) return std::nullopt;
            const TrueTypeFont& drawn_font = fonts.truetype(path, size, request.variations);
            // (the glyph the letter has anyway, no axes, no variation selector: set as before — its vertical form, its turn)
            if (request.variations.empty() && !has_vs(cell) && glyphs.front().glyph == drawn_font.glyph_index(cell.front())) return std::nullopt;
            return glyph_by_id(glyphs.front().glyph, cell, drawn_font, size, rgb, bold);
        };
    }
    if (by_place) {
        args.placed = placed_of(line, args.mono_ruby);
        args.origins = std::move(from);
        args.ruby_sets = ruby_settings(line, face, dpi, notes);
        args.notes = notes;
        args.drawn = drawn;
    }
    args.draw_text = parts == nullptr || parts->text;
    args.draw_ruby = parts == nullptr || parts->ruby;
    args.cells = cells;
    return compose(fonts, face, args);
}

// --- PC5 G-b: the OpenType typesetting's rows (SPEC.md TEXT-02「OpenType の組み方」) -----------------------------------------

// What horizontal_text hands the OpenType rows: the record, its style, face, em and room, the text as set (and each letter's
// place in the record's text), Genko's words set larger, bolder or in a colour (style_runs, letter by letter), the letters
// carrying 傍点, the lanes above the rows (傍点 and rubies), whether the letters are drawn and where each is set (PSD-01).
struct OpenTypeIn {
    Fonts& fonts;
    const core::StoryLine& line;
    const Json& st;
    const Face& face;
    std::int64_t em;
    double inner_w;
    const Rgb& fill;
    int dpi;
    const std::u32string& text;
    const std::vector<std::size_t>& from;
    const std::vector<Json>& styles;
    const std::set<std::size_t>& marked;
    std::int64_t mark_h;
    std::int64_t lane;
    std::int64_t above;
    bool draw_text;
    std::vector<SetRow>* cells;
};
// The rows' picture, and where each letter of the text is (left, right, its row's top) and its box (the rubies' lane).
struct OpenTypeRows {
    Image image;
    std::map<std::size_t, std::tuple<double, double, std::int64_t>> xs;
    std::map<std::size_t, BoxF> set_in;
};

// 禁則処理: the letters a row may not begin with, and those it may not end with — Genko's (the lines' rule), 弱い, 強い.
constexpr std::u32string_view kWeakStart = U"、。，．）」』】〕〉》］｝!?！？";
constexpr std::u32string_view kWeakEnd = U"（「『【〔〈《［｛";
constexpr std::u32string_view kStrongStart = U"ゝゞヽヾ々〻・：；゛゜ヵヶ‐゠–〜～";
constexpr std::u32string_view kStrongEnd = U"｛〘〖«";

// A letter set close to its neighbours, never broken from them by a row (CJK and full-width letters break anywhere).
bool loose_letter(char32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF) ||
           (c >= 0x20000 && c <= 0x3FFFF);
}

const Json& attr(const Json& attrs, const char* key) { return at(attrs, key); }
double attr_num(const Json& attrs, const char* key, double fallback) {
    const Json& v = at(attrs, key);
    return v.is_number() ? v.get<double>() : fallback;
}

OpenTypeRows opentype_rows(const OpenTypeIn& q) {
    OpenTypeRows result;
    const std::u32string& text = q.text;
    const Resolved resolved = resolve(q.line, text, q.from, TypeStyles::current());
    const bool native = q.line.native_type.has_value();
    const double px_mm = static_cast<double>(q.dpi) / 25.4;
    // (the line's own 長体・平体 and lean are drawn after its letters, as before: each letter's are against them)
    const double line_scale_x = float_or(at(q.st, "scale_x"), 1.0);
    const bool leant_after = float_or(at(q.st, "skew_deg"), 0) != 0.0 || core::py_truthy(at(q.st, "italic"));
    // (a letter's size_mm against the line's own size — its style's, else the lines' 5 mm —: a line fitted to its balloon
    // shrinks all its letters alike, review P3-12)
    const double base_mm = float_or(at(q.st, "size_mm"), kCapMm);
    const Json& line_font = at(q.st, "font");
    // each letter's look
    std::vector<Look> looks;
    std::map<std::string, std::size_t> look_index;  // (a look's key → its place: found in log time, review P2-5)
    std::vector<std::size_t> look_of(text.size(), 0);
    std::map<std::string, std::vector<std::string>> font_features;  // (font → its OpenType features)
    const auto has_feature = [&](const std::string& font, const std::string& tag) {
        auto it = font_features.find(font);
        if (it == font_features.end()) {
            std::vector<std::string> tags;
            try {
                tags = font_info(font).features;
            } catch (const std::exception&) {
            }
            it = font_features.emplace(font, std::move(tags)).first;
        }
        return std::binary_search(it->second.begin(), it->second.end(), tag);
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const Json& a = resolved.chars[i];
        Face f = q.face;
        if (const Json& font = attr(a, "font"); font.is_string() && font != line_font) f = face_of(font, kDefaultDialogue);
        const std::u32string one(1, text[i] == U'\n' ? U' ' : text[i]);
        Look look;
        look.font = q.fonts.readable(q.fonts.path_for(f, one));
        const Json* run_scale = q.styles[i].is_object() && q.styles[i].contains("scale") ? &q.styles[i]["scale"] : nullptr;
        const double word_scale = run_scale != nullptr ? core::to_float(*run_scale) : 1.0;
        const double size_ratio = attr(a, "size_mm").is_number() && base_mm > 0 ? attr(a, "size_mm").get<double>() / base_mm : 1.0;
        const double scale = core::py_max(0.1, core::py_min(10.0, attr_num(a, "scale", 1.0) * size_ratio)) * core::py_max(0.3, core::py_min(3.0, word_scale));
        look.size = std::max<std::int64_t>(4, core::py_round_int(static_cast<double>(q.em) * scale));
        std::map<std::string, std::uint32_t> features;
        if (attr(a, "ligatures") == Json(false)) features["liga"] = features["clig"] = 0;
        if (attr(a, "discretionary_ligatures") == Json(true)) features["dlig"] = 1;
        if (attr(a, "kerning") == Json("none")) features["kern"] = 0;
        if (attr(a, "proportional") == Json(true)) features["palt"] = 1;
        if (const Json& own = attr(a, "features"); own.is_object()) {
            for (const auto& [tag, value] : own.items()) {
                if (tag.size() == 4 && value.is_number_integer()) features[tag] = static_cast<std::uint32_t>(value.get<std::int64_t>());
            }
        }
        const Json& caps = attr(a, "caps");
        if (caps == Json("all")) {
            look.all_caps = true;
        } else if (caps == Json("small")) {  // (the font's own small capitals, else capitals at 0.7 of the size)
            if (has_feature(look.font, "smcp")) {
                features["smcp"] = 1;
            } else if (QChar::isLower(static_cast<char32_t>(text[i]))) {
                look.all_caps = true;
                look.size = std::max<std::int64_t>(4, core::py_round_int(static_cast<double>(look.size) * 0.7));
            }
        }
        for (const auto& [tag, value] : features) look.features.emplace_back(tag, value);
        if (const Json& axes = attr(a, "variations"); axes.is_object()) {
            for (const auto& [tag, value] : axes.items()) {
                if (value.is_number()) look.variations.emplace_back(tag, value.get<float>());
            }
        }
        if (const Json& language = attr(a, "language"); language.is_string()) look.language = language.get<std::string>();
        look.tracking = static_cast<double>(look.size) * attr_num(a, "tracking", 0.0);
        look.shape.scale_x = attr_num(a, "scale_x", 1.0) / (line_scale_x > 0 ? line_scale_x : 1.0);
        look.shape.scale_y = attr_num(a, "scale_y", 1.0);
        if (attr(a, "faux_italic") == Json(true) && !leant_after) look.shape.lean = std::tan(12.0 * core::kPi / 180.0);
        look.shift = attr_num(a, "baseline_shift_mm", 0.0) * px_mm;
        const Json& word_rgb = at(q.styles[i], "rgb");
        look.rgb = core::py_truthy(word_rgb) ? rgb_tuple(word_rgb) : attr(a, "rgb").is_array() ? rgb_tuple(attr(a, "rgb")) : q.fill;
        // (faux bold: the word's own where Genko's words say it — style_runs —, else the record's resolved attribute, which
        // holds the line's weight: a word set normal in a bold line stays normal, review P3-1)
        const int faux = attr(a, "faux_bold") == Json("heavy") ? 2 : attr(a, "faux_bold") == Json("bold") ? 1 : 0;
        const bool word_bold = q.styles[i].is_object() && q.styles[i].contains("bold");
        look.bold = bold_px(look.size, word_bold ? weight_level(q.styles[i]["bold"]) : faux);
        look.underline = attr(a, "underline") == Json(true);
        look.strikethrough = attr(a, "strikethrough") == Json(true);
        std::string key = look.font + '|' + std::to_string(look.size) + '|' + look.language + '|' + std::to_string(look.tracking) + '|' +
                          std::to_string(look.shape.scale_x) + ',' + std::to_string(look.shape.scale_y) + ',' + std::to_string(look.shape.lean) + '|' +
                          std::to_string(look.shift) + '|' + std::to_string(look.bold) + (look.underline ? "u" : "") + (look.strikethrough ? "s" : "") +
                          (look.all_caps ? "A" : "") + (look.small_caps ? "c" : "");
        for (const std::int64_t c : look.rgb) key += ',' + std::to_string(c);
        for (const auto& [tag, value] : look.features) key += '|' + tag + '=' + std::to_string(value);
        for (const auto& [tag, value] : look.variations) key += '|' + tag + ':' + std::to_string(value);
        const auto [it, fresh] = look_index.emplace(std::move(key), looks.size());
        look_of[i] = it->second;
        if (fresh) looks.push_back(std::move(look));
    }
    if (looks.empty()) {  // (an empty text: a row of the line's size)
        Look look;
        look.font = q.fonts.readable(q.fonts.path_for(q.face, U" "));
        look.size = q.em;
        look.rgb = q.fill;
        looks.push_back(std::move(look));
    }
    const auto ascent_of = [&](std::size_t l) {
        return q.fonts.truetype(looks[l].font, looks[l].size, looks[l].variations).getmetrics();
    };
    const double leading_add = native ? float_or(at(q.line.style, "leading"), 0) : float_or(at(q.st, "leading"), 0);
    struct Row {
        std::size_t para = 0;     // (which shaped paragraph)
        std::size_t start = 0;    // (letters of the paragraph [start, end), drawn [start, drawn))
        std::size_t end = 0;
        std::size_t drawn = 0;
        bool first = false;       // (its paragraph's first row, and its last)
        bool last = false;
        RowGlyphs glyphs;
        std::int64_t tallest = 0;
        std::size_t tallest_look = 0;
        std::int64_t height = 0;
        std::int64_t gap_before = 0;  // (段落前のアキ, or the paragraph before's 段落後のアキ)
        double lead = 0.0;            // (its indent at its left, and at its right)
        double trail = 0.0;
        std::string align;
        bool justified = false;
    };
    struct Para {
        std::size_t offset = 0;  // (its first letter's place in the text)
        ShapedParagraph shaped;
        std::u32string text;
    };
    std::vector<Para> paras;
    std::vector<Row> rows;
    // (a line's alignment as the lines' own setting reads it — left, right and justify kept, anything else, its default
    // "top" included, centred —; a point or box text's is its own or left: layout_of; review P2-1)
    std::string line_align = core::py_str(core::py_or(at(q.st, "align"), Json("center")));
    if (line_align != "left" && line_align != "right" && line_align != "start" && !line_align.starts_with("justify")) line_align = "center";
    std::int64_t pending_after = 0;
    // (a paragraph ends at a line break, and where Unicode ends one — U+2029, a lone CR, NEL, U+001C..U+001E —: SheenBidi
    // is never handed a separator inside a paragraph; review P2-4)
    const auto ends_paragraph = [&](std::size_t i) {
        const char32_t c = text[i];
        return c == U'\n' || c == U'\u2029' || c == U'\u0085' || (c >= 0x1C && c <= 0x1E) || (c == U'\r' && (i + 1 >= text.size() || text[i + 1] != U'\n'));
    };
    for (std::size_t ps = 0;;) {
        if (q.fonts.stop().stop_requested()) throw Cancelled();
        std::size_t pe = ps;
        while (pe < text.size() && !ends_paragraph(pe)) ++pe;
        if (pe >= text.size()) pe = std::u32string::npos;
        const bool last_para = pe == std::u32string::npos;
        if (last_para) pe = text.size();
        const std::size_t probe = ps < text.size() ? ps : (ps > 0 ? ps - 1 : 0);
        const Json& pa = resolved.paras[std::min(resolved.para_of.empty() ? 0 : resolved.para_of[std::min(probe, resolved.para_of.size() - 1)],
                                                  resolved.paras.size() - 1)];
        const bool world = attr(pa, "composer") == Json("world");
        const std::string kinsoku = world ? "none" : core::py_str(core::py_or(attr(pa, "kinsoku"), Json("genko")));
        const bool yakumono = !world && attr(pa, "yakumono") != Json(false);
        const std::string direction = core::py_str(core::py_or(attr(pa, "direction"), Json("auto")));
        Para para;
        para.offset = ps;
        para.text = text.substr(ps, pe - ps);
        std::vector<std::size_t> plooks(look_of.begin() + static_cast<std::ptrdiff_t>(ps), look_of.begin() + static_cast<std::ptrdiff_t>(pe));
        para.shaped = shape_paragraph(para.text, looks, plooks, direction);
        ShapedParagraph& shaped = para.shaped;
        const std::u32string& pt = para.text;
        const std::size_t n = pt.size();
        if (yakumono) {  // 約物の詰め: a closing mark before another mark, or any mark before an opening one, keeps half its width
            for (std::size_t i = 0; i + 1 < n; ++i) {
                if (!shaped.starts[i]) continue;
                const char32_t ch = pt[i], after = pt[i + 1];
                if ((in(kClosing, ch) && (in(kClosing, after) || in(kOpening, after))) || (in(kOpening, ch) && in(kOpening, after))) {
                    const double half = static_cast<double>(looks[plooks[i]].size) * 0.5;
                    shaped.tracking[i] -= half;
                    shaped.advance[i] -= half;
                }
            }
        }
        const bool rtl = shaped.bidi.rtl();
        const double first_indent = attr_num(pa, "first_indent_mm", 0.0) * px_mm;
        const double start_indent = attr_num(pa, "start_indent_mm", 0.0) * px_mm;
        const double end_indent = attr_num(pa, "end_indent_mm", 0.0) * px_mm;
        // the alignment: the paragraph's own (its style's, its native attributes'), else the line's — at a right-to-left
        // paragraph's start (its right) where the record names none (a point or box text's left)
        std::string align = attr(pa, "align").is_string() ? attr(pa, "align").get<std::string>() : line_align;
        if (align == "start") align = rtl ? "right" : "left";
        if (!attr(pa, "align").is_string() && rtl && align == "left") align = "right";
        const auto starts_forbidden = [&](char32_t c) {
            if (kinsoku == "none") return false;
            if (kinsoku == "weak") return in(kWeakStart, c);
            return in(kLineStart, c) || (kinsoku == "strong" && in(kStrongStart, c));
        };
        const auto ends_forbidden = [&](char32_t c) {
            if (kinsoku == "none") return false;
            if (kinsoku == "weak") return in(kWeakEnd, c);
            return in(kLineEnd, c) || (kinsoku == "strong" && in(kStrongEnd, c));
        };
        // where a row may break: before a cluster, after a space or beside a CJK letter, never before a space (it hangs)
        // nor where kinsoku says
        const auto breaks_before = [&](std::size_t i) {
            if (i == 0 || i >= n || !shaped.starts[i] || py_isspace(pt[i])) return false;
            const char32_t before = pt[i - 1], here = pt[i];
            if (!(py_isspace(before) || loose_letter(before) || loose_letter(here))) return false;
            return !starts_forbidden(here) && !ends_forbidden(before);
        };
        std::vector<std::pair<std::size_t, std::size_t>> spans;  // (the rows, letters of the paragraph)
        {
            std::size_t row_start = 0;
            double width = 0.0;
            std::optional<std::size_t> breakable;
            const auto room = [&](bool first) { return q.inner_w - start_indent - end_indent - (first ? first_indent : 0.0); };
            for (std::size_t i = 0; i < n; ++i) {
                if (!shaped.starts[i]) continue;
                if (i > row_start && breaks_before(i)) breakable = i;
                const double w = shaped.advance[i];
                if (i > row_start && !py_isspace(pt[i]) && width + w > room(spans.empty())) {
                    const std::size_t at_ = breakable && *breakable > row_start ? *breakable : i;
                    spans.emplace_back(row_start, at_);
                    row_start = at_;
                    width = 0.0;
                    breakable.reset();
                    for (std::size_t k = row_start; k < i; ++k) {
                        width += shaped.advance[k];
                        if (k > row_start && breaks_before(k)) breakable = k;
                    }
                    if (i > row_start && breaks_before(i)) breakable = i;
                }
                width += w;
            }
            spans.emplace_back(row_start, n);
        }
        for (std::size_t r = 0; r < spans.size(); ++r) {
            if ((r & 63) == 63 && q.fonts.stop().stop_requested()) throw Cancelled();  // (a long paragraph's rows: review P2-5)
            Row row;
            row.para = paras.size();
            row.start = spans[r].first;
            row.end = spans[r].second;
            row.drawn = row.end;
            while (row.drawn > row.start && py_isspace(pt[row.drawn - 1])) --row.drawn;
            row.first = r == 0;
            row.last = r + 1 == spans.size();
            row.glyphs = row_glyphs(shaped, row.start, row.drawn);
            row.tallest = 0;
            double leading_px = -1.0;
            for (std::size_t i = row.start; i < row.end; ++i) {
                const std::size_t l = plooks[i];
                if (looks[l].size > row.tallest) {
                    row.tallest = looks[l].size;
                    row.tallest_look = l;
                }
                const Json& lead = attr(resolved.chars[ps + i], "leading_mm");
                if (lead.is_number()) leading_px = std::max(leading_px, lead.get<double>() * px_mm);
            }
            if (row.tallest == 0) {  // (an empty row: its paragraph's look)
                row.tallest_look = n > 0 ? plooks[0] : (ps < look_of.size() ? look_of[ps] : (ps > 0 ? look_of[ps - 1] : 0));
                row.tallest = looks[row.tallest_look].size;
            }
            const double factor = (native ? attr_num(pa, "auto_leading", 1.2) : 1.15) + leading_add;
            row.height = (leading_px >= 0 ? core::py_round_int(leading_px) : core::py_round_int(static_cast<double>(row.tallest) * factor)) + q.above;
            if (r == 0) {
                row.gap_before = pending_after + (ps > 0 ? core::py_round_int(attr_num(pa, "space_before_mm", 0.0) * px_mm) : 0);
            }
            const double first = r == 0 ? first_indent : 0.0;
            row.lead = std::max(0.0, rtl ? end_indent : start_indent + first);
            row.trail = std::max(0.0, rtl ? start_indent + first : end_indent);
            row.align = align;
            if (align == "justify_all" || (align.starts_with("justify") && !row.last)) {
                row.justified = true;
            } else if (align.starts_with("justify")) {  // (a justified paragraph's last row)
                row.align = align == "justify_center" ? "center" : align == "justify_right" ? "right" : rtl ? "right" : "left";
            }
            rows.push_back(std::move(row));
        }
        pending_after = last_para ? 0 : core::py_round_int(attr_num(pa, "space_after_mm", 0.0) * px_mm);
        paras.push_back(std::move(para));
        if (last_para) break;
        ps = pe + 1;
    }
    // the picture: as wide as its widest row with its indents (a justified box text: its box), as tall as its rows
    double full = 0.0;
    bool justified = false;
    for (const Row& row : rows) {
        full = std::max(full, row.lead + row.glyphs.width + row.trail);
        justified = justified || row.justified;
    }
    // (a box text: as wide as its box, each row aligned in it — a right-to-left paragraph at its right; a justified row
    // spread over it)
    if ((justified || shape_of(q.line) == "box") && q.inner_w < kUnbroken / 2) full = std::max(full, q.inner_w);
    const std::int64_t out_w = std::max<std::int64_t>(1, static_cast<std::int64_t>(core::py_ceil(full)));
    // each row's top and baseline (characters share the row's baseline area, as a line's rows: the tallest letter's ascent
    // from its top); the picture holds every letter's ascent and descent — rows closer than their letters (leading below
    // the size) overlap, the first one's letters above its top kept by moving all down
    std::vector<std::int64_t> tops;
    std::vector<double> baselines;
    std::int64_t total = 0;
    double low = 0.0, high = 0.0;
    for (const Row& row : rows) {
        total += row.gap_before;
        tops.push_back(total);
        const double base_y = static_cast<double>(total + q.above) + static_cast<double>(row.height - q.above - row.tallest) / 2;
        const double baseline = base_y + static_cast<double>(ascent_of(row.tallest_look).first);
        baselines.push_back(baseline);
        const std::vector<std::size_t>& plooks = paras[row.para].shaped.look_of;
        for (std::size_t i = row.start; i < row.drawn; ++i) {
            const Look& look = looks[plooks[i]];
            const auto [ascent, descent] = ascent_of(plooks[i]);
            low = std::min(low, baseline - look.shift - static_cast<double>(ascent) * std::max(1.0, look.shape.scale_y));
            high = std::max(high, baseline - look.shift + static_cast<double>(descent) * std::max(1.0, look.shape.scale_y));
        }
        total += row.height;
    }
    const auto drop = static_cast<std::int64_t>(std::ceil(std::max(0.0, -low)));
    total = std::max<std::int64_t>(total, static_cast<std::int64_t>(std::ceil(high))) + drop;
    // (its size judged before it is made — at 600 dpi, whatever the resolution asked, as layout_of judges a text: what an op
    // lets in is drawn at any resolution up to it; review P1-1)
    {
        const double at600 = 600.0 / static_cast<double>(q.dpi);
        const double wide = static_cast<double>(out_w) * std::max(1.0, at600), high600 = static_cast<double>(total) * std::max(1.0, at600);
        if (!std::isfinite(wide) || !std::isfinite(high600) || wide > static_cast<double>(limits::kSide) || high600 > static_cast<double>(limits::kSide) ||
            wide * high600 > static_cast<double>(limits::kPixels))
            throw core::Error("image_too_large", "the text is too large to set at this resolution");
    }
    result.image = blank(out_w, std::max<std::int64_t>(1, total));
    Image& out = result.image;
    const std::string mark_kind = core::py_str(core::py_or(at(q.st, "emphasis_mark"), Json("sesame")));
    for (std::size_t r = 0; r < rows.size(); ++r) {
        Row& row = rows[r];
        const std::int64_t top = tops[r] + drop;
        const Para& para = paras[row.para];
        const double room = static_cast<double>(out_w) - row.lead - row.trail;
        double x0 = row.lead;
        if (row.justified) {
            std::size_t clusters = 0;
            for (std::size_t i = row.start; i < row.drawn; ++i) clusters += para.shaped.starts[i] ? 1 : 0;
            if (clusters > 1 && room > row.glyphs.width) row.glyphs = row_glyphs(para.shaped, row.start, row.drawn, (room - row.glyphs.width) / static_cast<double>(clusters - 1));
        } else if (row.align == "center") {
            x0 += (room - row.glyphs.width) / 2;
        } else if (row.align == "right") {
            x0 += room - row.glyphs.width;
        }
        const double baseline = baselines[r] + static_cast<double>(drop);
        SetRow* set_row = nullptr;
        if (q.cells != nullptr) {
            q.cells->emplace_back();
            set_row = &q.cells->back();
            set_row->axis = baseline;
            set_row->start = x0;
            set_row->end = x0 + row.glyphs.width;
        }
        if (q.draw_text) {
            Draw draw(out);
            std::size_t k = 0;
            const std::vector<RowGlyph>& glyphs = row.glyphs.glyphs;
            while (k < glyphs.size()) {
                std::size_t e = k;
                while (e < glyphs.size() && glyphs[e].look == glyphs[k].look) ++e;
                const Look& look = looks[glyphs[k].look];
                std::vector<GlyphPlace> places;
                for (std::size_t g = k; g < e; ++g) places.push_back(glyphs[g].place);
                const Ink ink = Ink::with_alpha(look.rgb, 255);
                draw.glyphs(PointD{x0, baseline - look.shift}, places, q.fonts.truetype(look.font, look.size, look.variations), ink, look.shape,
                            look.bold, look.bold != 0 ? std::optional<Ink>(ink) : std::nullopt);
                k = e;
            }
        }
        const std::vector<std::size_t>& plooks = para.shaped.look_of;
        for (std::size_t i = row.start; i < row.end; ++i) {
            const std::size_t at_text = para.offset + i;
            const bool drawn = i < row.drawn;
            const std::pair<double, double> span = drawn ? row.glyphs.span[i - row.start] : std::pair<double, double>{row.glyphs.width, row.glyphs.width};
            const double left = x0 + span.first, right = x0 + span.second;
            result.xs[at_text] = {left, right, top};
            const Look& look = looks[plooks[i]];
            const auto [ascent, descent] = q.fonts.truetype(look.font, look.size, look.variations).getmetrics();
            const double y = baseline - look.shift - static_cast<double>(ascent);
            result.set_in[at_text] = BoxF{left - look.bold, y - look.bold, right + look.bold, y + ascent + descent + look.bold};
            if (set_row != nullptr) {
                if (is_vs(para.text[i]) && !set_row->chars.empty()) {
                    set_row->chars.back().text.push_back(para.text[i]);
                } else {
                    SetChar ch;
                    ch.text = std::u32string(1, para.text[i]);
                    ch.size = look.size;
                    ch.weight = look.bold == 0 ? 0 : look.bold >= bold_px(look.size, 2) ? 2 : 1;
                    ch.rgb = look.rgb;
                    ch.font = look.font;
                    ch.box = BoxF{left, y, right, y + ascent + descent};
                    set_row->chars.push_back(std::move(ch));
                }
            }
            if (!q.draw_text || !drawn) continue;
            if (look.underline || look.strikethrough) {  // (下線・取り消し線: the font's own lines, under or through its cluster)
                const FontLines lines = q.fonts.truetype(look.font, look.size, look.variations).lines();
                Draw draw(out);
                const Ink ink = Ink::with_alpha(look.rgb, 255);
                if (look.underline) {
                    const double uy = baseline - look.shift + lines.underline - lines.underline_thickness / 2;
                    draw.rectangle(BoxF{left, uy, std::max(left, right - 1), uy + std::max(1.0, lines.underline_thickness) - 1}, ink);
                }
                if (look.strikethrough) {
                    const double sy = baseline - look.shift - lines.strikeout - lines.strikeout_thickness / 2;
                    draw.rectangle(BoxF{left, sy, std::max(left, right - 1), sy + std::max(1.0, lines.strikeout_thickness) - 1}, ink);
                }
            }
            if (q.marked.contains(at_text)) {
                draw_mark(out, (left + right) / 2,
                          static_cast<double>(top + q.lane) + static_cast<double>(q.mark_h) / 2 +
                              static_cast<double>(std::max<std::int64_t>(1, floordiv(q.em, 16))),
                          static_cast<double>(q.mark_h), mark_kind, look.rgb, false);
            }
        }
    }
    return result;
}

// _horizontal(line, st, face, em, inner_w, fill): rows left to right with kinsoku, centred (or aligned by
// style.align: left / center / right / justify). Ruby sits above its words and 傍点 just above the characters (the
// ruby above them); part of a line can be larger, smaller, bolder or in another colour (style_runs), which makes its
// row taller.
Image horizontal_text(Fonts& fonts, const core::StoryLine& line, const Json& st, const Face& face, std::int64_t em,
                      double inner_w, const Rgb& fill, int dpi, std::vector<RubyNote>* notes, std::vector<RubyDrawn>* drawn_at,
                      const LetterParts* drawing = nullptr, std::vector<SetRow>* cells = nullptr) {
    const bool draw_text = drawing == nullptr || drawing->text;  // (PSD-01: the text and/or the rubies, laid out alike)
    const bool draw_ruby = drawing == nullptr || drawing->ruby;
    std::vector<SetRow> set_rows;  // (PSD-01: where each character is set, row by row)
    const bool by_place = line.ruby_spans.has_value();
    const bool opentype = sets_opentype(line);  // (PC5 G-b: the rows set by HarfBuzz — each letter's place in the record kept)
    const SetText set = by_place || opentype ? set_text_mapped(fonts, line, st, face) : SetText{set_text(fonts, line, st, face), {}};
    const std::u32string& text = set.text;
    const double tracking = float_or(at(st, "tracking"), 0);
    std::u32string flat;
    for (const char32_t c : text) {
        if (c != U'\n') flat.push_back(c);
    }
    const int weight = line_weight(st);
    // (the OpenType rows take the line's weight from the record's resolved attributes: the words' own alone here)
    const std::vector<Json> styles_flat = char_styles(flat, line.style_runs, weight && !opentype ? Json{{"bold", weight}} : Json());
    std::vector<Json> styles;  // (styles per character of the text, "\n" included)
    std::size_t k = 0;
    for (const char32_t c : text) styles.push_back(c == U'\n' ? Json::object() : styles_flat[k++]);
    std::set<std::size_t> marked;
    std::size_t pos = 0;
    for (const std::u32string& base : emphasis_of(fonts, line, face)) {
        const std::size_t found = text.find(base, pos);
        if (found != std::u32string::npos) {
            for (std::size_t i = found; i < found + base.size(); ++i) {
                if (!py_isspace(text[i])) marked.insert(i);
            }
            pos = found + base.size();
        }
    }
    struct RubyAt {
        std::size_t first;
        std::size_t last;
        std::u32string ruby;
        std::vector<std::u32string> letters;  // (by place: the reading's graphemes, and which of the line's spans)
        std::size_t span = 0;
    };
    std::vector<RubyAt> ruby_at;
    const std::vector<RubySetting> sets = by_place ? ruby_settings(line, face, dpi, notes) : std::vector<RubySetting>{};
    if (by_place) {  // (the characters that came from each ruby's base; across, Python's lettering has no モノルビ)
        for (const PlacedRuby& placed : placed_of(line, false)) {
            std::size_t first = std::u32string::npos;
            std::size_t last = 0;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == U'\n' || set.from[i] < placed.from || set.from[i] >= placed.to) continue;
                first = std::min(first, i);
                last = i + 1;
            }
            if (first == std::u32string::npos) {
                if (notes != nullptr) notes->push_back(RubyNote{"not_drawn", placed.span, "its base is not set"});
                continue;
            }
            std::u32string reading;
            for (const auto& letter : placed.reading) reading += letter;
            ruby_at.push_back({first, last, reading, placed.reading, placed.span});
        }
    }
    pos = 0;
    for (const Json& run : line.ruby_runs) {
        if (by_place) break;
        if (!core::py_truthy(run) || core::length(run) < 2) continue;
        const Json base_value = core::subscript(run, 0);
        const Json ruby_value = core::subscript(run, 1);
        if (!core::py_truthy(base_value) || !core::py_truthy(ruby_value)) continue;
        const std::u32string base = fonts.normalize(face, u32(core::py_str(base_value)));
        const std::size_t found = text.find(base, pos);
        if (found != std::u32string::npos) {
            ruby_at.push_back({found, found + base.size(), u32(core::py_str(ruby_value)), {}, 0});
            pos = found + base.size();
        }
    }
    const std::int64_t mark_h = !marked.empty() ? core::py_round_int(static_cast<double>(em) * 0.36) : 0;
    // Python's ruby across is half the em (it never reads ruby_scale); by place each ruby its own size (its scale),
    // distance from the text and face, the lane above the rows as tall as the tallest of them needs
    const std::int64_t ruby_h = std::max<std::int64_t>(6, floordiv(em, 2));
    const RubySetting plain_set{face, std::nullopt, "center", 0, 0.0};
    const auto set_of = [&](std::size_t span) -> const RubySetting& { return span < sets.size() ? sets[span] : plain_set; };
    const auto own_h = [&](std::size_t span) {
        const std::optional<double>& scale = set_of(span).scale;
        return scale ? std::max<std::int64_t>(4, core::py_round_int(static_cast<double>(em) * core::py_max(0.25, core::py_min(0.8, *scale)))) : ruby_h;
    };
    std::int64_t lane = 0;  // (never less than nothing: a reading nearer than its own size reaches into the text)
    for (const RubyAt& run : ruby_at) lane = std::max(lane, by_place ? own_h(run.span) + set_of(run.span).gap : ruby_h);
    const auto size_of = [&](std::size_t i) -> std::int64_t {
        const Json& style = styles[i];
        const Json* scale = style.is_object() && style.contains("scale") ? &style["scale"] : nullptr;
        const double s = scale != nullptr ? core::to_float(*scale) : 1.0;
        return std::max<std::int64_t>(4, core::py_round_int(static_cast<double>(em) * core::py_max(0.3, core::py_min(3.0, s))));
    };
    // features = [str(f) for f in st.get("features") or []] or None: OpenType features need raqm; Pillow's BASIC
    // layout refuses them with a KeyError when a letter is drawn (the reference is held to BASIC)
    bool features = false;
    if (core::py_truthy(at(st, "features"))) features = !core::iterate(at(st, "features")).empty();
    const bool yakumono = st.contains("yakumono") ? core::py_truthy(st["yakumono"]) : true;
    const auto advance = [&](std::size_t i) -> double {
        const char32_t ch = text[i];
        if (is_vs(ch)) return 0.0;
        const std::u32string one(1, ch);
        double width = fonts.font(face, size_of(i), one).getlength(one) + static_cast<double>(em) * tracking;
        if (yakumono && i + 1 < text.size()) {
            const char32_t after = text[i + 1];
            // 約物の詰め: a closing mark before another mark, or any mark before an opening one, keeps half its width
            if ((in(kClosing, ch) && (in(kClosing, after) || in(kOpening, after))) || (in(kOpening, ch) && in(kOpening, after))) {
                width -= static_cast<double>(size_of(i)) * 0.5;
            }
        }
        return width;
    };
    const std::int64_t above = std::max<std::int64_t>(0, mark_h + lane);
    std::map<std::size_t, std::tuple<double, double, std::int64_t>> xs;  // place in text → (left, right, row top)
    std::map<std::size_t, BoxF> set_in;  // (asked where the rubies were set: place in text → the box its letter was set in)
    Image out;
    std::int64_t out_w = 0;
    if (opentype) {  // (PC5 G-b: the rows set by HarfBuzz — attributes, bidirectional order, indents, alignment)
        OpenTypeRows made = opentype_rows(OpenTypeIn{fonts, line, st, face, em, inner_w, fill, dpi, text, set.from, styles, marked, mark_h,
                                                     lane, above, draw_text, cells != nullptr ? &set_rows : nullptr});
        out = std::move(made.image);
        out_w = out.width();
        xs = std::move(made.xs);
        set_in = std::move(made.set_in);
    } else {
        std::vector<std::vector<std::size_t>> rows;  // the characters' places in the text, row by row
        std::size_t index = 0;
        for (std::size_t start = 0;;) {
            std::size_t end = text.find(U'\n', start);
            const bool last = end == std::u32string::npos;
            if (last) end = text.size();
            std::vector<std::size_t> row;
            double width = 0.0;
            for (std::size_t p = start; p < end; ++p) {
                const char32_t ch = text[p];
                const double w = advance(index);
                if (!row.empty() && width + w > inner_w && !in(kLineStart, ch)) {
                    if (in(kLineEnd, text[row.back()]) && row.size() > 1) {
                        const std::size_t carried = row.back();
                        row.pop_back();
                        rows.push_back(row);
                        row = {carried};
                        width = advance(carried);
                    } else {
                        rows.push_back(row);
                        row.clear();
                        width = 0.0;
                    }
                }
                row.push_back(index);
                ++index;
                width += w;
            }
            rows.push_back(row);
            ++index;  // the "\n"
            if (last) break;
            start = end + 1;
        }
        const double leading = float_or(at(st, "leading"), 0);
        const auto tallest_of = [&](const std::vector<std::size_t>& row) {
            std::int64_t tallest = 0;
            for (const std::size_t i : row) tallest = std::max(tallest, size_of(i));
            return row.empty() ? em : tallest;
        };
        std::vector<std::int64_t> heights;
        std::vector<double> widths;
        for (const auto& row : rows) {
            heights.push_back(core::py_round_int(static_cast<double>(tallest_of(row)) * (1.15 + leading)) + above);
            std::vector<double> parts;
            for (const std::size_t i : row) parts.push_back(advance(i));
            widths.push_back(core::py_float_sum(parts));
        }
        double widest = widths.front();
        for (const double w : widths) widest = core::py_max(widest, w);
        out_w = std::max<std::int64_t>(1, static_cast<std::int64_t>(core::py_ceil(widest)));
        std::int64_t total = 0;
        for (const std::int64_t h : heights) total += h;
        out = blank(out_w, std::max<std::int64_t>(1, total));
        const Json& align_value = at(st, "align");
        const std::string align = align_value == Json("left") || align_value == Json("right") ? align_value.get<std::string>() : "center";
        const bool justify = align_value == Json("justify");  // 均等揃え: each row (but the last) spread over the whole width
        const std::string mark_kind = core::py_str(core::py_or(at(st, "emphasis_mark"), Json("sesame")));
        std::int64_t top = 0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const auto& row = rows[r];
            double x = align == "left" || justify ? 0.0 : (static_cast<double>(out_w) - widths[r]) / (align == "center" ? 2 : 1);
            SetRow* set_row = nullptr;
            bool baseline_set = false;
            if (cells != nullptr) {
                set_rows.emplace_back();
                set_row = &set_rows.back();
                set_row->start = set_row->end = x;
            }
            const double spread = justify && row.size() > 1 && r < rows.size() - 1
                                      ? (static_cast<double>(out_w) - widths[r]) / static_cast<double>(row.size() - 1)
                                      : 0.0;
            const std::int64_t tallest = tallest_of(row);
            // characters share the row's baseline area
            const double base_y = static_cast<double>(top + above) + static_cast<double>(heights[r] - above - tallest) / 2;
            for (const std::size_t i : row) {
                const std::int64_t size = size_of(i);
                const Json& rgb_value = at(styles[i], "rgb");
                const Rgb rgb = core::py_truthy(rgb_value) ? rgb_tuple(rgb_value) : fill;
                const int thick = bold_px(size, at(styles[i], "bold"));
                if (is_vs(text[i])) {
                    xs[i] = {x, x, top};
                    if (set_row != nullptr && !set_row->chars.empty()) set_row->chars.back().text.push_back(text[i]);
                    continue;
                }
                std::u32string letter(1, text[i]);
                for (std::size_t j = i + 1; j < std::min(text.size(), i + 3); ++j) {
                    if (is_vs(text[j])) {
                        letter.push_back(text[j]);
                        break;
                    }
                }
                if (features) {
                    throw core::PyUncaught("KeyError", "setting text direction, language or font features is not supported without libraqm");
                }
                if (draw_text) {
                    Draw draw(out);
                    const Ink ink = Ink::with_alpha(rgb, 255);
                    draw.text(PointD{x, base_y + static_cast<double>(tallest - size)}, letter, fonts.font(face, size, std::u32string(1, text[i])),
                              ink, {}, thick, thick != 0 ? std::optional<Ink>(ink) : std::nullopt);
                }
                const double w = advance(i) - static_cast<double>(em) * tracking;
                if (set_row != nullptr) {  // (its advance, the font's ascent and descent from where it is drawn)
                    const std::u32string one(1, text[i]);
                    const auto [ascent, descent] = fonts.font(face, size, one).getmetrics();
                    const double y = base_y + static_cast<double>(tallest - size);
                    SetChar ch;
                    ch.text = one;
                    ch.size = size;
                    ch.weight = weight_level(at(styles[i], "bold"));
                    ch.rgb = rgb;
                    ch.font = fonts.path_for(face, one);
                    ch.box = BoxF{x, y, x + w, y + ascent + descent};
                    if (size == tallest && !baseline_set) {  // (the row's baseline: its first tallest letter's)
                        set_row->axis = y + ascent;
                        baseline_set = true;
                    }
                    set_row->chars.push_back(std::move(ch));
                    set_row->end = x + w;
                }
                if (drawn_at != nullptr) {  // (its advance, the font's ascent and descent from where it is drawn; a bold one's stroke)
                    const auto [ascent, descent] = fonts.font(face, size, std::u32string(1, text[i])).getmetrics();
                    const double y = base_y + static_cast<double>(tallest - size);
                    set_in[i] = BoxF{x - thick, y - thick, x + w + thick, y + ascent + descent + thick};
                }
                if (marked.contains(i) && draw_text) {
                    draw_mark(out, x + w / 2,
                              static_cast<double>(top + lane) + static_cast<double>(mark_h) / 2 +
                                  static_cast<double>(std::max<std::int64_t>(1, floordiv(em, 16))),
                              static_cast<double>(mark_h), mark_kind, rgb, false);
                }
                xs[i] = {x, x + w, top};
                x += advance(i) + spread;
            }
            if (set_row != nullptr && !baseline_set) set_row->axis = base_y + fonts.font(face, tallest).getmetrics().first;  // (an empty row)
            top += heights[r];
        }

    }
    struct Drawn {
        std::int64_t row;
        double left;
        double right;
        std::size_t span;
    };
    std::vector<Drawn> drawn;
    struct Part {  // (by place: a ruby's letters over one row, and its base's characters, left and right there)
        std::int64_t row = 0;
        std::vector<std::u32string> letters;
        double x0 = 0.0;
        double x1 = 0.0;
        std::size_t span = 0;
        std::vector<std::size_t> chars;
    };
    std::vector<Part> parts;
    for (const RubyAt& run : ruby_at) {
        // a word split over rows gets its ruby split in proportion
        std::map<std::int64_t, std::vector<std::size_t>> groups;
        for (std::size_t i = run.first; i < run.last; ++i) {
            if (const auto it = xs.find(i); it != xs.end()) groups[std::get<2>(it->second)].push_back(i);
        }
        std::int64_t count = 0;
        for (const auto& [row_top, chars] : groups) count += static_cast<std::int64_t>(chars.size());
        if (count == 0) count = 1;
        const auto len = static_cast<std::int64_t>(by_place ? run.letters.size() : run.ruby.size());
        std::int64_t taken = 0;
        std::size_t n = 0;
        for (const auto& [row_top, chars] : groups) {
            const std::int64_t share =
                n == groups.size() - 1
                    ? len - taken
                    : core::py_round_int(static_cast<double>(len * static_cast<std::int64_t>(chars.size())) / static_cast<double>(count));
            ++n;
            const std::int64_t from = std::clamp<std::int64_t>(taken, 0, len);
            const std::int64_t to = std::clamp<std::int64_t>(taken + share, 0, len);
            taken += share;
            if (to <= from) continue;
            if (by_place) {
                Part piece;
                piece.row = row_top;
                piece.letters.assign(run.letters.begin() + from, run.letters.begin() + to);
                piece.x0 = std::get<0>(xs.at(chars.front()));
                piece.x1 = std::get<1>(xs.at(chars.back()));
                piece.span = run.span;
                piece.chars = chars;
                parts.push_back(std::move(piece));
                continue;
            }
            const std::u32string part = run.ruby.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
            const TrueTypeFont& font = fonts.font(face, ruby_h, part.substr(0, 1));
            const double span = font.getlength(part);
            const double centre = (std::get<0>(xs.at(chars.front())) + std::get<1>(xs.at(chars.back()))) / 2;
            if (!draw_ruby) continue;
            Draw draw(out);
            draw.text(PointD{core::py_max(0.0, core::py_min(static_cast<double>(out_w) - span, centre - span / 2)),
                             static_cast<double>(row_top)},
                      part, font, Ink::with_alpha(fill, 255));
        }
    }
    // by place, each ruby as its own settings say: from the base's head, around its middle or to its tail, its letters
    // `tracking` apart in its face and size, `gap` above the text (its foot on the lane's), kept within the lettering
    // (as Python keeps its ruby); a reading wider than all the text is not cut: the lettering widens (said)
    const auto track_of = [&](const Part& piece) {
        return static_cast<double>(core::py_round_int(static_cast<double>(own_h(piece.span)) * set_of(piece.span).tracking));
    };
    const auto width_of = [&](const Part& piece, std::vector<double>* advances) {
        const Face& own = set_of(piece.span).face;
        const std::int64_t size = own_h(piece.span);
        const double track = track_of(piece);
        if (track == 0.0) {
            std::u32string part;
            for (const auto& letter : piece.letters) part += letter;
            return fonts.font(own, size, part.substr(0, 1)).getlength(part);
        }
        std::vector<double> each;
        for (const auto& letter : piece.letters) each.push_back(fonts.font(own, size, letter).getlength(letter));
        const double span = core::py_float_sum(each) + track * static_cast<double>(piece.letters.size() - 1);
        if (advances != nullptr) *advances = std::move(each);
        return span;
    };
    double widest_ruby = 0.0;
    for (const Part& piece : parts) widest_ruby = core::py_max(widest_ruby, width_of(piece, nullptr));
    const std::int64_t text_w = out_w;
    double shift = 0.0;
    if (widest_ruby > static_cast<double>(out_w)) {
        const auto wide = static_cast<std::int64_t>(core::py_ceil(widest_ruby));
        const std::int64_t dx = floordiv(wide - out_w, 2);
        Image wider = blank(wide, out.height());
        wider.paste(out, Point{static_cast<int>(dx), 0}, &out);
        out = std::move(wider);
        out_w = wide;
        shift = static_cast<double>(dx);
    }
    for (const Part& piece : parts) {
        const RubySetting& own = set_of(piece.span);
        const std::int64_t size = own_h(piece.span);
        const double track = track_of(piece);
        std::vector<double> advances;
        const double span = width_of(piece, &advances);
        if (span > static_cast<double>(text_w) && notes != nullptr) {
            notes->push_back(RubyNote{"overflow", piece.span, "wider than the text"});
        }
        const double x0 = piece.x0 + shift;
        const double x1 = piece.x1 + shift;
        const double want = own.align == "head" ? x0 : own.align == "tail" ? x1 - span : (x0 + x1) / 2 - span / 2;
        const double x = core::py_max(0.0, core::py_min(static_cast<double>(out_w) - span, want));
        const auto y = static_cast<double>(piece.row + lane - own.gap - size);
        RubyDrawn where;
        Draw draw(out);
        if (track != 0.0) {
            double at_x = x;
            for (std::size_t i = 0; i < piece.letters.size(); ++i) {
                const std::string used = fonts.path_for(own.face, piece.letters[i]);
                note_font(fonts, own.face, piece.letters[i], used, piece.span, notes);
                if (draw_ruby) draw.text(PointD{at_x, y}, piece.letters[i], fonts.font(own.face, size, piece.letters[i]), Ink::with_alpha(fill, 255));
                where.letters.push_back(PointD{at_x, y});
                where.fonts.push_back(used);
                at_x += advances[i] + track;
            }
        } else {
            std::u32string part;
            for (const auto& letter : piece.letters) part += letter;
            // (the part drawn in the font of its first letter, as Python draws its ruby: each letter as that font has it)
            const std::string used = fonts.path_for(own.face, part.substr(0, 1));
            for (const auto& letter : piece.letters) note_font(fonts, own.face, letter, used, piece.span, notes);
            where.fonts.assign(piece.letters.size(), used);
            const TrueTypeFont& font = fonts.font(own.face, size, part.substr(0, 1));
            if (draw_ruby) draw.text(PointD{x, y}, part, font, Ink::with_alpha(fill, 255));
            if (drawn_at != nullptr) {  // (each letter where the line of them puts it)
                std::u32string before;
                for (const auto& letter : piece.letters) {
                    where.letters.push_back(PointD{x + font.getlength(before), y});
                    before += letter;
                }
            }
        }
        drawn.push_back(Drawn{piece.row, x, x + span, piece.span});
        where.spacing = track;
        where.fill = fill;
        if (drawn_at != nullptr) {  // (the box its letters are set in: their advances, the font's ascent and descent)
            const auto [ascent, descent] = fonts.font(own.face, size, piece.letters.front().substr(0, 1)).getmetrics();
            where.span = piece.span;
            for (const auto& letter : piece.letters) where.reading += letter;
            where.ruby = BoxF{x, y, x + span, y + ascent + descent};
            bool first = true;
            for (const std::size_t i : piece.chars) {
                const auto it = set_in.find(i);
                if (it == set_in.end()) continue;
                const BoxF b{it->second.x0 + shift, it->second.y0, it->second.x1 + shift, it->second.y1};
                where.base = first ? b : BoxF{std::min(where.base.x0, b.x0), std::min(where.base.y0, b.y0), std::max(where.base.x1, b.x1),
                                              std::max(where.base.y1, b.y1)};
                first = false;
            }
            where.size = size;
            where.face = own.face;
            where.vertical = false;
            drawn_at->push_back(std::move(where));
        }
    }
    for (std::size_t a = 0; a < drawn.size() && notes != nullptr; ++a) {
        for (std::size_t b = a + 1; b < drawn.size(); ++b) {
            if (drawn[a].row == drawn[b].row && drawn[a].left < drawn[b].right && drawn[b].left < drawn[a].right) {
                notes->push_back(RubyNote{"overlap", drawn[a].span, "with ruby " + std::to_string(drawn[b].span)});
            }
        }
    }
    if (cells != nullptr) {  // (moved with the text when a reading wider than it widened the lettering)
        for (SetRow& row : set_rows) {
            row.start += shift;
            row.end += shift;
            for (SetChar& ch : row.chars) ch.box = BoxF{ch.box.x0 + shift, ch.box.y0, ch.box.x1 + shift, ch.box.y1};
            cells->push_back(std::move(row));
        }
    }
    return out;
}

}  // namespace

Json style_of(const core::StoryLine& line) {
    static const Json defaults = Json::parse(R"({"font": null, "size_mm": null, "tracking": 0.0, "leading": 0.4,
        "align": "top", "outline_mm": null, "rgb": null, "tcy": true, "border_mm": 0.35, "fill": "white", "group": null,
        "rotate_deg": 0.0, "skew_deg": 0.0, "arc": 0.0, "latin": "rotate", "emphasis_mark": "sesame", "bold": false,
        "weight": null, "italic": false, "outline_rgb": null, "wobble": 0.0, "double": false, "spikes": null,
        "spike_depth": 0.2, "scale_x": 1.0, "gradient": null, "text_path": null, "features": null, "yakumono": true,
        "spike_jitter": 0.0, "bumps": null, "picture": null, "warp": null, "fill_png": null, "line_rgb": null,
        "fill_rgb": null, "fill_opacity": null, "text_dx_mm": 0.0, "text_dy_mm": 0.0, "path_curve": false, "cuts": null,
        "ruby_scale": 0.5, "mono_ruby": false, "below_layer": null, "hand": true, "periods": false})");
    Json st = defaults;
    if (line.style.is_object()) {
        for (const auto& [key, value] : line.style.items()) st[key] = value;
    }
    return st;
}

int line_weight(const Json& st) {
    const Json& weight = at(st, "weight");
    if (core::py_truthy(weight)) return weight_level(weight);
    return weight_level(Json(core::py_truthy(at(st, "bold"))));
}

std::int64_t px(double mm, int dpi) { return std::max<std::int64_t>(1, core::py_round_int(mm / 25.4 * dpi)); }

std::pair<double, double> inner(std::string_view kind, double w, double h, double pad, const Json& depth) {
    if (kind == "electric") return {(w - 2 * pad) * 0.76, (h - 2 * pad) * 0.76};  // (a squarish outline, less the teeth)
    if (kind == "shout") {
        // (the valleys, and their chords)
        const double keep = 1 - core::py_max(0.05, core::py_min(0.6, depth.is_null() ? 0.2 : core::to_float(depth))) - 0.06;
        return {(w - 2 * pad) / kSqrt2 * keep, (h - 2 * pad) / kSqrt2 * keep};
    }
    if (one_of(kind, kElliptic)) return {(w - 2 * pad) / kSqrt2, (h - 2 * pad) / kSqrt2};
    if (kind == "rounded") {
        const double r = core::py_min(w, h) * 0.3;
        return {w - 2 * pad - r * 0.3, h - 2 * pad - r * 0.3};
    }
    if (one_of(kind, kFrames)) return {w - 6 * pad, h - 6 * pad};  // (a frame's lines and marks keep the words further in)
    if (one_of(kind, kSquare)) return {w - 2 * pad, h - 2 * pad};
    if (kind == "picture") return {(w - 2 * pad) * 0.7, (h - 2 * pad) * 0.7};  // (the words keep to its middle)
    return {w, h};
}

namespace {

// (`st.get("hand", True) and not float(st.get("wobble") or 0)`: drawn by hand, and not wobbled)
bool hand_drawn(const Json& st) {
    const Json& hand = st.contains("hand") ? st["hand"] : Json(true);
    return core::py_truthy(hand) && float_or(at(st, "wobble"), 0) == 0.0;
}

}  // namespace

double hug_power(std::string_view kind, const Json& st) { return one_of(kind, kUneven) && hand_drawn(st) ? kHandPower : 2.0; }

double hug_keep(std::string_view kind, const Json& st) {
    if (kind == "shout") {
        const Json& depth = at(st, "spike_depth");
        return 1 - core::py_max(0.05, core::py_min(0.6, depth.is_null() ? 0.2 : core::to_float(depth))) - 0.02;
    }
    if (one_of(kind, kUneven) && hand_drawn(st)) return 0.955;
    return 1.0;
}

std::vector<std::array<std::int64_t, 4>> ink_rects(const Image& image, std::int64_t em) {
    const Image alpha_band = image.mode() == "RGBA" ? image.getchannel("A") : image.convert("L");
    const std::string pixels = alpha_band.tobytes();
    const std::int64_t w = alpha_band.width();
    const std::int64_t h = alpha_band.height();
    const auto inked = [&](std::int64_t x, std::int64_t y) {
        return static_cast<unsigned char>(pixels[static_cast<std::size_t>(y * w + x)]) > 40;
    };
    std::vector<std::array<std::int64_t, 4>> rects;
    const std::int64_t step = std::max<std::int64_t>(1, floordiv(em, 3));
    for (std::int64_t x = 0; x < w; x += step) {
        const std::int64_t x_end = std::min(w, x + step);
        std::int64_t top = -1, bottom = -1, left = -1, right = -1;
        for (std::int64_t y = 0; y < h; ++y) {
            for (std::int64_t xx = x; xx < x_end; ++xx) {
                if (!inked(xx, y)) continue;
                if (top < 0) top = y;
                bottom = y;
                if (left < 0 || xx < left) left = xx;
                if (xx > right) right = xx;
            }
        }
        if (top >= 0) rects.push_back({left, top, right + 1, bottom + 1});
    }
    return rects;
}

std::vector<std::array<double, 2>> block_points(const std::vector<std::array<std::int64_t, 4>>& rects, double pad) {
    double min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const auto& r = rects[i];
        if (i == 0 || static_cast<double>(r[0]) < min_x) min_x = static_cast<double>(r[0]);
        if (i == 0 || static_cast<double>(r[2]) > max_x) max_x = static_cast<double>(r[2]);
        if (i == 0 || static_cast<double>(r[1]) < min_y) min_y = static_cast<double>(r[1]);
        if (i == 0 || static_cast<double>(r[3]) > max_y) max_y = static_cast<double>(r[3]);
    }
    const double cx = (min_x + max_x) / 2;
    const double cy = (min_y + max_y) / 2;
    std::vector<std::array<double, 2>> out;
    const auto corner = [&](int xi, int yi, double xs, double ys) {
        for (const auto& r : rects) out.push_back({static_cast<double>(r[xi]) + xs - cx, static_cast<double>(r[yi]) + ys - cy});
    };
    corner(0, 1, -pad, -pad);
    corner(2, 1, pad, -pad);
    corner(0, 3, -pad, pad);
    corner(2, 3, pad, pad);
    return out;
}

namespace {

// numpy.linspace(-span, span, 9)
std::array<double, 9> linspace9(double span) {
    const double start = -span;
    const double stop = span;
    const double delta = stop - start;
    const double step = delta / 8;
    std::array<double, 9> y{};
    for (int k = 0; k < 9; ++k) {
        y[static_cast<std::size_t>(k)] = (step == 0 ? (static_cast<double>(k) / 8) * delta : static_cast<double>(k) * step) + start;
    }
    y[8] = stop;
    return y;
}

}  // namespace

Fit fit_in(const std::vector<std::array<double, 2>>& points, double a, double b, double power) {
    // (|x / a|^n + |y / b|^n for each point and each trial middle: numpy squares for n = 2 and calls pow otherwise)
    const auto pw = [power](double v) { return power == 2.0 ? v * v : core::py_pow(v, power); };
    const double sa = core::py_max(a, 1e-6);
    const double sb = core::py_max(b, 1e-6);
    bool have = false;
    double best_worst = 0.0;
    std::array<double, 2> best_off{0.0, 0.0};
    std::array<double, 2> centre{0.0, 0.0};
    for (const double step : {0.25, 0.06}) {
        const auto lx = linspace9(a * step);
        const auto ly = linspace9(b * step);
        double k_worst = 0.0;
        std::array<double, 2> k_off{};
        bool k_nan = false;
        for (int k = 0; k < 81; ++k) {
            const std::array<double, 2> off{centre[0] + lx[static_cast<std::size_t>(k % 9)], centre[1] + ly[static_cast<std::size_t>(k / 9)]};
            // d.max(axis=1): NaN when any is
            double worst = -std::numeric_limits<double>::infinity();
            for (std::size_t p = 0; p < points.size() && !std::isnan(worst); ++p) {
                const double d = pw(std::fabs((points[p][0] + off[0]) / sa)) + pw(std::fabs((points[p][1] + off[1]) / sb));
                if (std::isnan(d) || d > worst) worst = d;
            }
            // argmin: the first NaN, else the first of the smallest
            if (!k_nan && (k == 0 || std::isnan(worst) || worst < k_worst)) {
                k_worst = worst;
                k_off = off;
                k_nan = std::isnan(worst);
            }
        }
        if (!have || k_worst < best_worst) {
            best_worst = k_worst;
            best_off = k_off;
            have = true;
        }
        centre = best_off;
    }
    return {best_worst, best_off[0], best_off[1]};
}

namespace {

// (asked where the rubies by place were set: those places through the letter effects; `exact` false when an effect
// moved the letters some other way than the boxes follow)
struct SetRubies {
    std::vector<RubyDrawn> drawn;
    bool exact = true;
};

Layout layout_of(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path, std::stop_token stop,
                 std::vector<RubyNote>* notes, SetRubies* rubies = nullptr, const LetterParts* parts = nullptr, SetLetters* set_out = nullptr);

}  // namespace

Layout text_layout(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path, std::stop_token stop) {
    return layout_of(line, dpi, font_path, std::move(stop), nullptr);
}

std::vector<RubyNote> ruby_notes(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path) {
    std::vector<RubyNote> notes;
    if (!line.ruby_spans) return notes;
    (void)layout_of(line, dpi, font_path, {}, &notes);
    std::vector<RubyNote> once;
    for (const RubyNote& note : notes) {
        const bool seen = std::any_of(once.begin(), once.end(), [&](const RubyNote& n) { return n.kind == note.kind && n.span == note.span; });
        if (!seen) once.push_back(note);
    }
    return once;
}

RubyPlaces ruby_places(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path) {
    RubyPlaces out;
    const Json st = style_of(line);
    if (core::py_truthy(at(st, "text_path"))) return out;  // (letters on a path carry no ruby, as Python's)
    core::StoryLine twin = line;  // (a line of Python's: its runs where the explicit rule finds them)
    if (!line.ruby_spans) {
        twin.ruby_spans = core::ruby::from_runs(line.text, line.ruby_runs, false);
        if (twin.ruby_spans->empty()) return out;
    }
    SetRubies set;
    const Layout layout = layout_of(twin, dpi, font_path, {}, nullptr, &set);
    out.exact = set.exact;
    if (!line.ruby_spans) out.exact = out.exact && layout.image.tobytes() == text_layout(line, dpi, font_path).image.tobytes();
    const Json& rotate = at(st, "rotate_deg");
    if (core::py_truthy(rotate) && std::fabs(core::to_float(rotate)) > 0.01) out.exact = false;  // (its group drawn turned)
    // where the lettering's top left lies on the page (as paint_text puts it)
    const std::int64_t x = px(line.x_mm.value() + float_or(at(st, "text_dx_mm"), 0), dpi);
    const std::int64_t y = px(line.y_mm.value() + float_or(at(st, "text_dy_mm"), 0), dpi);
    const double cx = static_cast<double>(x) + static_cast<double>(px(mm_or(line.w_mm, 40), dpi)) / 2;
    const double cy = static_cast<double>(y) + static_cast<double>(px(mm_or(line.h_mm, 20), dpi)) / 2;
    const bool corner = kind_of(line) == "none";
    const auto left = static_cast<double>(corner ? x : core::py_round_int(cx + layout.corner_x));
    const auto top = static_cast<double>(corner ? y : core::py_round_int(cy + layout.corner_y));
    const auto on_page = [&](const BoxF& b) {
        return Box{static_cast<int>(std::floor(b.x0 + left)), static_cast<int>(std::floor(b.y0 + top)), static_cast<int>(std::ceil(b.x1 + left)),
                   static_cast<int>(std::ceil(b.y1 + top))};
    };
    for (const RubyDrawn& where : set.drawn) {
        RubyPlace place;
        place.span = where.span;
        place.reading = utf8(where.reading);
        place.ruby = on_page(where.ruby);
        place.base = on_page(where.base);
        for (const PointD& p : where.letters) place.letters.push_back(PointD{p.x + left, p.y + top});
        place.size = where.size;
        place.face = where.face;
        place.vertical = where.vertical;
        out.places.push_back(std::move(place));
    }
    return out;
}

namespace {

// Warp Text (PC3 C-c, the style's native_warp): the letters' picture bent by one of PhotoCraft's fifteen warp styles over
// its box (core/page_geometry's StyleWarp: its bend and distortions in %, along or across), drawn again through the map
// (render/resample's mesh, bicubic, premultiplied); the picture grows to hold what the bend reaches, its middle where it
// was.
Image style_warped(const Image& image, const Json& spec) {
    namespace pg = core::page_geometry;
    const auto style = spec.contains("style") && spec["style"].is_string() ? pg::warp_style_from(spec["style"].get<std::string>()) : std::nullopt;
    if (!style || !pg::is_preset(*style) || image.width() <= 0 || image.height() <= 0) return image;
    const auto number = [&](const char* key, double fallback) { return spec.contains(key) && spec[key].is_number() ? spec[key].get<double>() : fallback; };
    const bool vertical = spec.contains("vertical") && spec["vertical"].is_boolean() && spec["vertical"].get<bool>();
    const double w = image.width(), h = image.height();
    const auto warp = pg::StyleWarp::make(*style, number("bend", 50.0), number("h_distort", 0.0), number("v_distort", 0.0), vertical, {0.0, 0.0, w, h});
    if (!warp) return image;
    double x0 = 0, y0 = 0, x1 = w, y1 = h;
    constexpr int kSamples = 32;
    for (int j = 0; j <= kSamples; ++j) {
        for (int i = 0; i <= kSamples; ++i) {
            const core::vector::XY p = warp->apply(w * i / kSamples, h * j / kSamples);
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
    }
    // (as much room on each side as the bend reaches past the box on either: the middle stays the middle)
    const int pad_x = static_cast<int>(std::ceil(std::max(-x0, x1 - w))) + 2, pad_y = static_cast<int>(std::ceil(std::max(-y0, y1 - h))) + 2;
    const resample::Plane plane = resample::from_rgba8(image.mode() == "RGBA" ? image : image.convert("RGBA"));
    const resample::Plane out = resample::mesh(
        plane,
        [&](double x, double y) {
            const core::vector::XY p = warp->apply(x, y);
            return std::pair<double, double>{p.x, p.y};
        },
        image.width() + 2 * pad_x, image.height() + 2 * pad_y, resample::Interp::Bicubic, -pad_x, -pad_y);
    return resample::to_rgba8(out);
}

// (PSD-01) a × b of maps given as {a, b, c, d, e, f}: x' = a·x + c·y + e, y' = b·x + d·y + f; `then` applies q after p
std::array<double, 6> then(const std::array<double, 6>& p, const std::array<double, 6>& q) {
    return {q[0] * p[0] + q[2] * p[1], q[1] * p[0] + q[3] * p[1], q[0] * p[2] + q[2] * p[3], q[1] * p[2] + q[3] * p[3],
            q[0] * p[4] + q[2] * p[5] + q[4], q[1] * p[4] + q[3] * p[5] + q[5]};
}

Layout layout_of(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path, std::stop_token stop,
                 std::vector<RubyNote>* notes, SetRubies* rubies, const LetterParts* parts, SetLetters* set_out) {
    // The lettering, its em, and where its top left sits from the box's middle (px). In an ellipse the letters are
    // fitted to the outline by their own shape, and sit where they fit best. type@1 (PC5 G-a): a point text is set
    // unbroken at its size from its anchor, a box text broken at its box at its size — neither shrunk nor fitted to a
    // balloon — and their corner is from the anchor, or the box's top left.
    const std::string shape = shape_of(line);
    Json st = style_of(line);
    if (shape == "point") {  // (a point text is not justified: Photoshop's 均等配置 is a box text's)
        const Json& align = at(st, "align");
        if (align.is_string() && align.get_ref<const std::string&>().starts_with("justify")) st["align"] = line.wrap == "vertical" ? "top" : "left";
    }
    // (a point or box text with no alignment: Photoshop's left — top, down —, not a line's centre: review P2-5)
    if (!shape.empty() && !(line.style.is_object() && core::py_truthy(at(line.style, "align")))) st["align"] = line.wrap == "vertical" ? "top" : "left";
    const std::string kind = kind_of(line);
    Fonts fonts(std::move(stop));
    const Json& font_value = at(st, "font");
    const Json spec = core::py_truthy(font_value) ? font_value : (kind == "sfx" || !font_path ? Json() : Json(*font_path));
    const Face face = face_of(spec, kind == "sfx" ? kDefaultSfx : kDefaultDialogue);
    const Rgb fill = fill_of(st);
    const std::int64_t w = px(mm_or(line.w_mm, 40), dpi);
    const std::int64_t h = px(mm_or(line.h_mm, 20), dpi);
    const bool vertical = line.wrap == "vertical";
    const bool tcy = core::py_truthy(at(st, "tcy"));
    std::int64_t em = 0;
    bool fixed = false;
    if (!shape.empty()) {  // (type@1: its size, or the lines' default)
        em = px(float_or(at(st, "size_mm"), kCapMm), dpi);
        fixed = true;
    } else if (core::py_truthy(at(st, "size_mm"))) {
        em = px(core::to_float(at(st, "size_mm")), dpi);
        fixed = true;
    } else if (kind == "sfx") {
        const std::u32string text = u32(line.text.empty() ? std::string(" ") : line.text);
        const auto first = std::max<std::int64_t>(
            1, static_cast<std::int64_t>(cell_count(text.substr(0, text.find(U'\n')), tcy, at(st, "latin") != Json("upright"))));
        em = std::max<std::int64_t>(8, std::min({px(kSfxCapMm, dpi), vertical ? w : h, floordiv(vertical ? h : w, first)}));
    } else if (kind == "none") {
        // text only: the box sets the size (the longest column fills its height)
        const std::u32string text = u32(line.text.empty() ? std::string(" ") : line.text);
        std::int64_t longest = 0;
        for (std::size_t start = 0;;) {
            const std::size_t end = text.find(U'\n', start);
            const std::u32string_view part = std::u32string_view(text).substr(start, end == std::u32string::npos ? std::u32string::npos : end - start);
            longest = std::max<std::int64_t>(longest, static_cast<std::int64_t>(cell_count(part, tcy, false)));
            if (end == std::u32string::npos) break;
            start = end + 1;
        }
        if (longest == 0) longest = 1;
        em = std::max<std::int64_t>(8, vertical ? std::min(w, floordiv(h, longest)) : std::min(h, floordiv(w, longest)));
    } else {
        em = std::max<std::int64_t>(8, px(kCapMm, dpi));
    }
    if (!shape.empty()) {  // (type@1, review P2-4: a point text is never broken, a box text may run far past its box —
                           // what it would come to, roughly, past op_limits' most pixels is refused before it is drawn)
        const std::u32string text = u32(line.text);
        std::int64_t paragraphs = 1, longest = 0, run = 0;
        for (const char32_t c : text) {
            if (c == U'\n') {
                ++paragraphs;
                run = 0;
            } else {
                longest = std::max(longest, ++run);
            }
        }
        double step = static_cast<double>(em) * 1.2 * (1 + std::max(0.0, float_or(at(st, "tracking"), 0)));
        double rows = static_cast<double>(em) * 1.6 * (1 + std::max(0.0, float_or(at(st, "leading"), 0)));
        double extra_along = 0, extra_across = 0;  // (PC5 G-b review P1-1: indents, the space between paragraphs, shifts)
        if (sets_opentype(line)) {  // (the record's attributes at their largest: its letters' size, tracking and leading)
            const double px_mm = static_cast<double>(dpi) / 25.4;
            const double base_mm = float_or(at(st, "size_mm"), kCapMm);
            double scale = 1.0, tracking = 0.0, leading = 0.0, shift = 0.0;
            for (const core::type_text::CharRun& char_run : core::type_text::resolve_chars(line, TypeStyles::current())) {
                const Json& a = char_run.attrs;
                const double size_ratio = at(a, "size_mm").is_number() && base_mm > 0 ? at(a, "size_mm").get<double>() / base_mm : 1.0;
                const double s = (at(a, "scale").is_number() ? at(a, "scale").get<double>() : 1.0) * size_ratio *
                                 std::max(at(a, "scale_x").is_number() ? at(a, "scale_x").get<double>() : 1.0, at(a, "scale_y").is_number() ? at(a, "scale_y").get<double>() : 1.0);
                scale = std::max(scale, s);
                if (at(a, "tracking").is_number()) tracking = std::max(tracking, at(a, "tracking").get<double>());
                if (at(a, "leading_mm").is_number()) leading = std::max(leading, at(a, "leading_mm").get<double>() * px_mm);
                if (at(a, "baseline_shift_mm").is_number()) shift = std::max(shift, std::abs(at(a, "baseline_shift_mm").get<double>()) * px_mm);
            }
            const std::int64_t count = core::type_text::paragraph_count(line.text);
            for (std::int64_t p = 0; p < count; ++p) {
                const Json pa = core::type_text::resolve_para(line, TypeStyles::current(), p);
                const auto mm = [&](const char* key) { return at(pa, key).is_number() ? std::abs(at(pa, key).get<double>()) * px_mm : 0.0; };
                extra_along = std::max(extra_along, mm("first_indent_mm") + mm("start_indent_mm") + mm("end_indent_mm"));
                extra_across += mm("space_before_mm") + mm("space_after_mm");
            }
            scale = std::min(scale, 30.0);  // (the attributes' own bounds: scale 10 and a word's 3)
            step = static_cast<double>(em) * scale * 1.2 * (1 + std::max(0.0, tracking));
            rows = std::max(rows * scale, leading);
            extra_across += 2 * shift;
        }
        double along = 0, across = 0;  // (along the rows or columns, and across them)
        if (shape == "point") {
            along = step * static_cast<double>(longest) + static_cast<double>(em);
            across = rows * static_cast<double>(paragraphs) + static_cast<double>(em);
        } else {
            along = static_cast<double>(vertical ? h : w);
            across = rows * (static_cast<double>(paragraphs) + step * static_cast<double>(text.size()) / std::max(1.0, along)) + static_cast<double>(em);
        }
        along += extra_along;
        across += extra_across;
        // (judged as at 600 dpi, whatever the resolution asked: what an op lets in is drawn at any resolution up to it)
        const double at600 = 600.0 / static_cast<double>(dpi);
        along *= at600;
        across *= at600;
        if (along * across > static_cast<double>(limits::kPixels) || along > static_cast<double>(limits::kSide) || across > static_cast<double>(limits::kSide))
            throw core::Error("image_too_large", "the text is too large to set at this resolution");
    }
    const bool hug = shape.empty() && one_of(kind, kHugged);
    const double keep = hug ? hug_keep(kind, st) : 1.0;
    const double power = hug_power(kind, st);
    const auto room = [&](std::int64_t size) -> std::pair<double, double> {
        if (shape == "point") return {kUnbroken, kUnbroken};  // (type@1: broken only where its text breaks)
        if (shape == "box") return {static_cast<double>(w), static_cast<double>(h)};
        const double pad = kind == "none" || kind == "sfx" ? 0.0 : static_cast<double>(std::max<std::int64_t>(2, floordiv(size, 4)));
        if (hug) {  // (the columns may run the ellipse's height; whether they fit is judged by their shape below)
            const double margin = static_cast<double>(2 * size) * kFitPad;
            return {(static_cast<double>(w) * keep - margin) * 0.92, (static_cast<double>(h) * keep - margin) * 0.92};
        }
        return inner(kind, static_cast<double>(w), static_cast<double>(h), pad, at(st, "spike_depth"));
    };
    auto [inner_w, inner_h] = room(em);
    const double scale_x = core::py_max(0.3, core::py_min(3.0, float_or(at(st, "scale_x"), 1.0)));
    Image image;
    std::vector<SetRow> first;  // (type@1, a point text: its rows as set, for where its first one sits)
    int set_width = 1;          // (and the width they were set in, before 長体・平体)
    for (int attempt = 0; attempt < 12; ++attempt) {
        if (notes != nullptr) notes->clear();  // (what the lettering kept says)
        if (rubies != nullptr) rubies->drawn.clear();
        first.clear();
        std::vector<RubyDrawn>* drawn = rubies != nullptr ? &rubies->drawn : nullptr;
        std::vector<SetRow>* rows_of = shape == "point" ? &first : nullptr;
        image = vertical ? vertical_text(fonts, line, st, face, em, inner_h, fill, dpi, notes, drawn, nullptr, rows_of)
                         : horizontal_text(fonts, line, st, face, em, inner_w, fill, dpi, notes, drawn, nullptr, rows_of);
        set_width = image.width();
        if (std::fabs(scale_x - 1) > 1e-3) {  // 長体 (< 1) or 平体 (> 1): the letters narrower or wider than tall
            const int set_w = image.width();
            image = image.resize({static_cast<int>(std::max<std::int64_t>(1, core::py_round_int(image.width() * scale_x))), image.height()},
                                 Resample::Lanczos);
            if (rubies != nullptr) {  // (the boxes narrowed or widened with them)
                const double f = static_cast<double>(image.width()) / static_cast<double>(std::max(1, set_w));
                for (RubyDrawn& where : rubies->drawn) {
                    for (BoxF* b : {&where.ruby, &where.base}) *b = BoxF{b->x0 * f, b->y0, b->x1 * f, b->y1};
                    for (PointD& p : where.letters) p.x *= f;
                }
            }
        }
        bool fits = false;
        if (hug) {
            const auto rects = ink_rects(image, em);
            fits = rects.empty() || fit_in(block_points(rects, static_cast<double>(em) * kFitPad), static_cast<double>(w) / 2 * keep,
                                           static_cast<double>(h) / 2 * keep, power)
                                            .worst <= 1.03;
        } else {
            fits = image.width() <= inner_w * 1.02 && image.height() <= inner_h * 1.02;
        }
        if (fixed || fits || em <= 8) break;
        em = std::max<std::int64_t>(8, core::py_trunc_int(static_cast<double>(em) * 0.92));
        std::tie(inner_w, inner_h) = room(em);
    }
    if (core::py_truthy(at(st, "gradient"))) image = gradient_letters(image, at(st, "gradient"));
    if (core::py_truthy(at(st, "fill_png"))) image = picture_letters(image, at(st, "fill_png"));
    const Json& outline = at(st, "outline_mm");
    std::int64_t grow = core::py_truthy(outline) ? px(core::to_float(outline), dpi) : (kind == "sfx" ? std::max<std::int64_t>(2, floordiv(em, 8)) : 0);
    if (kind == "tone_box" && !core::py_truthy(outline)) grow = std::max<std::int64_t>(2, floordiv(em, 7));  // (a white edge over the tone)
    if (grow != 0) {
        image = outlined(image, grow, core::py_truthy(at(st, "outline_rgb")) ? rgb_tuple(at(st, "outline_rgb")) : Rgb{255, 255, 255});
        if (rubies != nullptr) {  // (the letters moved in by the halo's room: grow + 1 each side)
            const auto pad = static_cast<double>(grow + 1);
            for (RubyDrawn& where : rubies->drawn) {
                for (BoxF* b : {&where.ruby, &where.base}) *b = BoxF{b->x0 + pad, b->y0 + pad, b->x1 + pad, b->y1 + pad};
                for (PointD& p : where.letters) p = PointD{p.x + pad, p.y + pad};
            }
        }
    }
    if (core::py_truthy(at(st, "arc"))) image = arched(image, core::to_float(at(st, "arc")), vertical);
    double skew = float_or(at(st, "skew_deg"), 0);
    if (skew == 0.0) skew = core::py_truthy(at(st, "italic")) ? 12.0 : 0.0;  // (italic: a light lean)
    if (skew != 0.0) image = skewed(image, skew, vertical);
    if (core::py_truthy(at(st, "warp"))) image = warped_letters(image, at(st, "warp"));
    const bool bent = at(st, "native_warp").is_object();  // (PC3 C-c: Warp Text)
    if (bent) image = style_warped(image, at(st, "native_warp"));
    if (rubies != nullptr && (std::fabs(scale_x - 1) > 1e-3 || core::py_truthy(at(st, "arc")) || skew != 0.0 || core::py_truthy(at(st, "warp")) || bent)) {
        rubies->exact = false;  // (bent, leant, pulled or drawn narrower or wider: the boxes are where the letters were set)
    }
    // PSD-01: the letters set again at the em found (the same layout), drawing only the parts asked for and saying where
    // each character and ruby was set; the same effects after, a halo left out as a halo's room
    std::optional<Image> part;
    if (set_out != nullptr || (parts != nullptr && !parts->all())) {
        std::vector<SetRow> rows;
        std::vector<RubyDrawn> set_rubies;
        Image picture = vertical ? vertical_text(fonts, line, st, face, em, inner_h, fill, dpi, nullptr, &set_rubies, parts, &rows)
                                 : horizontal_text(fonts, line, st, face, em, inner_w, fill, dpi, nullptr, &set_rubies, parts, &rows);
        const Size set_size = picture.size();
        std::array<double, 6> to_image{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
        if (std::fabs(scale_x - 1) > 1e-3) {
            picture = picture.resize({static_cast<int>(std::max<std::int64_t>(1, core::py_round_int(picture.width() * scale_x))), picture.height()},
                                     Resample::Lanczos);
            to_image[0] = static_cast<double>(picture.width()) / static_cast<double>(std::max(1, set_size.width));
        }
        if (core::py_truthy(at(st, "gradient"))) picture = gradient_letters(picture, at(st, "gradient"));
        if (core::py_truthy(at(st, "fill_png"))) picture = picture_letters(picture, at(st, "fill_png"));
        if (grow != 0) {
            const Rgb halo = core::py_truthy(at(st, "outline_rgb")) ? rgb_tuple(at(st, "outline_rgb")) : Rgb{255, 255, 255};
            if (parts == nullptr || parts->halo) {
                picture = outlined(picture, grow, halo);
            } else {  // (the halo's room, nothing in it)
                Image padded = blank(picture.width() + 2 * (grow + 1), picture.height() + 2 * (grow + 1));
                padded.paste(picture, Point{static_cast<int>(grow + 1), static_cast<int>(grow + 1)});
                picture = std::move(padded);
            }
            to_image = then(to_image, {1.0, 0.0, 0.0, 1.0, static_cast<double>(grow + 1), static_cast<double>(grow + 1)});
            if (set_out != nullptr) set_out->halo = std::pair<std::int64_t, Rgb>{halo_filter(grow) / 2, halo};  // (as wide as drawn)
        }
        if (core::py_truthy(at(st, "arc"))) picture = arched(picture, core::to_float(at(st, "arc")), vertical);
        if (skew != 0.0) {
            const double k = core::py_tan(core::py_max(-60.0, core::py_min(60.0, skew)) * (core::kPi / 180.0));  // (as skewed leans them)
            const double wide = picture.width();
            const double high = picture.height();
            to_image = then(to_image, vertical ? std::array<double, 6>{1.0, -k, 0.0, 1.0, 0.0, k > 0 ? k * wide : 0.0}
                                               : std::array<double, 6>{1.0, 0.0, -k, 1.0, k > 0 ? k * high : 0.0, 0.0});
            picture = skewed(picture, skew, vertical);
        }
        if (core::py_truthy(at(st, "warp"))) picture = warped_letters(picture, at(st, "warp"));
        if (bent) picture = style_warped(picture, at(st, "native_warp"));
        if (set_out != nullptr) {
            SetLetters& set = *set_out;
            set.vertical = vertical;
            set.em = em;
            set.face = face;
            const Json& align = at(st, "align");
            set.align = vertical ? core::py_str(core::py_or(align, Json("top")))
                                 : align == Json("left") || align == Json("right") || align == Json("justify") ? align.get<std::string>() : "center";
            set.rows = std::move(rows);
            set.rubies = std::move(set_rubies);
            set.area = BoxF{0.0, 0.0, static_cast<double>(set_size.width), static_cast<double>(set_size.height)};
            set.size = set_size;
            set.to_image = to_image;
            set.italic = float_or(at(st, "skew_deg"), 0) == 0.0 && skew != 0.0;
            if (core::py_truthy(at(st, "gradient"))) set.effects.emplace_back("gradient");
            if (core::py_truthy(at(st, "fill_png"))) set.effects.emplace_back("picture_fill");
            if (core::py_truthy(at(st, "arc"))) set.effects.emplace_back("arc");
            if (core::py_truthy(at(st, "warp")) || bent) set.effects.emplace_back("warp");
            for (const std::string& run : line.emphasis_runs) {
                if (!run.empty() && line.text.find(run) != std::string::npos) {
                    set.effects.emplace_back("emphasis");
                    break;
                }
            }
        }
        if (parts != nullptr && !parts->all()) part = std::move(picture);
    }
    Layout out;
    out.corner_x = -static_cast<double>(image.width()) / 2;
    out.corner_y = -static_cast<double>(image.height()) / 2;
    if (hug) {
        const auto rects = ink_rects(image, em);
        if (!rects.empty()) {
            const Fit fit = fit_in(block_points(rects, static_cast<double>(em) * kFitPad), static_cast<double>(w) / 2 * keep,
                                   static_cast<double>(h) / 2 * keep, power);
            std::int64_t x0 = rects[0][0], y0 = rects[0][1], x1 = rects[0][2], y1 = rects[0][3];
            for (const auto& r : rects) {
                x0 = std::min(x0, r[0]);
                y0 = std::min(y0, r[1]);
                x1 = std::max(x1, r[2]);
                y1 = std::max(y1, r[3]);
            }
            // (the letters' middle moved by the best fit)
            out.corner_x = fit.dx - static_cast<double>(x0 + x1) / 2;
            out.corner_y = fit.dy - static_cast<double>(y0 + y1) / 2;
        }
    }
    if (!shape.empty()) {  // (type@1: from the point text's anchor, or the box text's top left)
        const double wide = image.width(), high = image.height();
        const double halo = grow != 0 ? static_cast<double>(grow + 1) : 0.0;  // (the halo's)
        const std::string align = core::py_str(core::py_or(at(st, "align"), Json(vertical ? "top" : "left")));
        if (shape == "box") {
            out.corner_x = vertical ? static_cast<double>(w) - wide  // (columns from its right)
                           : align == "center" ? (static_cast<double>(w) - wide) / 2
                           : align == "right"  ? static_cast<double>(w) - wide
                                               : 0.0;
            out.corner_y = !vertical ? 0.0 : align == "center" ? (static_cast<double>(h) - high) / 2 : align == "bottom" ? static_cast<double>(h) - high : 0.0;
        } else {
            const double set_wide = wide - 2 * halo;  // (the letters as set, 長体・平体 after)
            const double fx = set_wide / static_cast<double>(std::max(1, set_width));
            if (vertical) {  // (the first column's middle on the anchor, its top there — or the text's middle or foot)
                out.corner_x = first.empty() ? -wide / 2 : -(first.front().axis * fx + halo);
                out.corner_y = align == "center" ? -high / 2 : align == "bottom" ? -high : -(first.empty() ? 0.0 : first.front().start) - halo;
            } else {  // (the first row's baseline on the anchor; its left, middle or right there)
                out.corner_x = align == "center" ? -wide / 2 : align == "right" ? -wide : -halo;
                out.corner_y = -((first.empty() ? static_cast<double>(em) : first.front().axis) + halo);
            }
        }
    }
    out.image = part ? std::move(*part) : std::move(image);  // (the parts asked for, where the whole puts them)
    if (const Json* aa = native_key(line, "antialias"); aa != nullptr && *aa == Json("none")) binarized(out.image);
    out.em = em;
    return out;
}

}  // namespace

SetLayout set_layout(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path, const LetterParts& parts) {
    SetLayout out;
    if (core::py_truthy(at(style_of(line), "text_path"))) {  // (letters on a path: set along it, nothing else)
        out.set.effects.emplace_back("text_path");
        return out;
    }
    out.layout = layout_of(line, dpi, font_path, {}, nullptr, nullptr, &parts, &out.set);
    return out;
}

std::pair<Face, bool> line_face(const core::StoryLine& line, const std::optional<std::string>& font_path) {
    // (as layout_of chooses it)
    const Json st = style_of(line);
    const std::string kind = kind_of(line);
    const Json& font_value = at(st, "font");
    const Json spec = core::py_truthy(font_value) ? font_value : (kind == "sfx" || !font_path ? Json() : Json(*font_path));
    const bool missing = core::py_truthy(font_value) && font_value.is_string() && !face_exists(font_value.get_ref<const std::string&>());
    return {face_of(spec, kind == "sfx" ? kDefaultSfx : kDefaultDialogue), missing};
}

std::string shape_of(const core::StoryLine& line) {
    const Json* shape = native_key(line, "shape");
    return shape != nullptr && shape->is_string() ? shape->get<std::string>() : std::string();
}

namespace {

// type@1 (PC5 G-a): where a point text's anchor, or a box text's top left, lies on the page (px; the words moved by
// text_dx_mm and text_dy_mm as a line's)
PointD shape_origin(const core::StoryLine& line, const Json& st, int dpi, std::string_view shape) {
    double x = line.x_mm.value(), y = line.y_mm.value();
    if (shape == "point") {
        const Json* anchor = native_key(line, "anchor_mm");
        if (anchor != nullptr && anchor->is_array() && anchor->size() == 2 && (*anchor)[0].is_number() && (*anchor)[1].is_number()) {
            x = (*anchor)[0].get<double>();
            y = (*anchor)[1].get<double>();
        }
    }
    x += float_or(at(st, "text_dx_mm"), 0);
    y += float_or(at(st, "text_dy_mm"), 0);
    return PointD{static_cast<double>(place_px(x, dpi)), static_cast<double>(place_px(y, dpi))};
}

// type@1 (PC5 G-a): the box a box text is cut to (px), none for any other
std::optional<Box> clip_of(const core::StoryLine& line, int dpi) {
    if (shape_of(line) != "box") return std::nullopt;
    const double x = line.x_mm.value(), y = line.y_mm.value();
    return Box{static_cast<int>(place_px(x, dpi)), static_cast<int>(place_px(y, dpi)), static_cast<int>(place_px(x + line.w_mm.value(), dpi)),
               static_cast<int>(place_px(y + line.h_mm.value(), dpi))};
}

}  // namespace

Point lettering_at(const core::StoryLine& line, int dpi, double corner_x, double corner_y) {
    // (as paint_text puts it)
    const Json st = style_of(line);
    if (const std::string shape = shape_of(line); !shape.empty()) {
        const PointD o = shape_origin(line, st, dpi, shape);
        return Point{static_cast<int>(core::py_round_int(o.x + corner_x)), static_cast<int>(core::py_round_int(o.y + corner_y))};
    }
    const std::int64_t x = px(line.x_mm.value() + float_or(at(st, "text_dx_mm"), 0), dpi);
    const std::int64_t y = px(line.y_mm.value() + float_or(at(st, "text_dy_mm"), 0), dpi);
    if (kind_of(line) == "none") return Point{static_cast<int>(x), static_cast<int>(y)};
    const double cx = static_cast<double>(x) + static_cast<double>(px(mm_or(line.w_mm, 40), dpi)) / 2;
    const double cy = static_cast<double>(y) + static_cast<double>(px(mm_or(line.h_mm, 20), dpi)) / 2;
    return Point{static_cast<int>(core::py_round_int(cx + corner_x)), static_cast<int>(core::py_round_int(cy + corner_y))};
}

Placement placement(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path, std::stop_token stop) {
    const std::shared_ptr<const Layout> layout = remembered_layout(line, dpi, font_path, std::move(stop));
    Placement out;
    const Point at = lettering_at(line, dpi, layout->corner_x, layout->corner_y);
    out.picture = Box{at.x, at.y, at.x + layout->image.width(), at.y + layout->image.height()};
    out.em = layout->em;
    if (const std::optional<Box> inked = layout->image.getbbox()) out.letters = Box{at.x + inked->x0, at.y + inked->y0, at.x + inked->x1, at.y + inked->y1};
    out.clip = clip_of(line, dpi);
    if (out.clip && out.letters) {  // (what runs past the box: letters drawn there, not only the picture's clear margin)
        const Box& drawn = *out.letters;
        out.overflow = drawn.x0 < out.clip->x0 || drawn.y0 < out.clip->y0 || drawn.x1 > out.clip->x1 || drawn.y1 > out.clip->y1;
    }
    return out;
}

std::pair<Image, std::int64_t> text_image(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path,
                                          std::stop_token stop) {
    Layout layout = text_layout(line, dpi, font_path, std::move(stop));
    return {std::move(layout.image), layout.em};
}

namespace {

detail::Memo<Layout>& layouts() {
    static detail::Memo<Layout> memo(512, 96LL * 1024 * 1024);
    return memo;
}

// 画像のフキダシ's and picture letters' pictures, decoded once (balloons._PICTURES: by the data's hash, the 64 last)
detail::Memo<std::optional<Image>>& pictures() {
    static detail::Memo<std::optional<Image>> memo(64, 256LL * 1024 * 1024);
    return memo;
}

// Where a layout's letters lie (its size, em and corner): kept longer than the layouts' pictures, so a part of the page
// the letters do not reach is left before the layout is looked for or made again.
struct LayoutShape {
    int width = 0;
    int height = 0;
    std::int64_t em = 0;
    double corner_x = 0.0;
    double corner_y = 0.0;
};

detail::Memo<LayoutShape>& layout_shapes() {
    static detail::Memo<LayoutShape> memo(8192, 16LL * 1024 * 1024);
    return memo;
}

// The box a speaker's name in the default face at a size covers from where it is drawn (its glyphs' box, a pixel
// more around): a part of the page it does not reach opens no font.
detail::Memo<Box>& speaker_boxes() {
    static detail::Memo<Box> memo(4096, 4LL * 1024 * 1024);
    return memo;
}

// Everything text_layout reads of the line, and the font files it opens by name (a picture in its style by its digest).
std::optional<std::string> layout_key(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path) {
    Json style_runs = Json::array();
    for (const auto& [words, style] : line.style_runs) style_runs.push_back(Json::array({words, style}));
    Json key = Json::array({line.text, line.balloon, line.wrap, line.w_mm.json(), line.h_mm.json(), detail::keyed_style(line.style),
                            Json(line.ruby_runs), Json(line.emphasis_runs), style_runs, dpi, font_path ? Json(*font_path) : Json()});
    if (line.ruby_spans) key.push_back(core::ruby::to_json(*line.ruby_spans));  // (ruby-layout@1: the rubies by place)
    if (line.native_type) key.push_back(Json::array({"native_type", *line.native_type}));  // (type@1: its shape, anti-alias, …)
    // (PC5 G-b: a record set by the OpenType typesetting reads the book's type styles — Basic Paragraph, the styles it takes —,
    // a line across with features alone too; their digest, made once where they are set: review P2-3, P3-10)
    if (TypeStyles::current() != nullptr && !TypeStyles::current()->is_null() && sets_opentype(line))
        key.push_back(Json::array({"type_styles", TypeStyles::digest()}));
    return detail::memo_key(key);
}

std::shared_ptr<const Layout> layout_by_key(const std::optional<std::string>& key, const core::StoryLine& line, int dpi,
                                            const std::optional<std::string>& font_path, std::stop_token stop) {
    if (key) {
        if (auto found = layouts().get(*key)) return found;
    }
    auto made = std::make_shared<const Layout>(text_layout(line, dpi, font_path, std::move(stop)));
    if (key) {
        const std::int64_t bytes = static_cast<std::int64_t>(made->image.width()) * made->image.height() * 4;
        layouts().put(*key, made, bytes);
        layout_shapes().put(*key, std::make_shared<const LayoutShape>(LayoutShape{made->image.width(), made->image.height(), made->em,
                                                                                  made->corner_x, made->corner_y}),
                            static_cast<std::int64_t>(sizeof(LayoutShape)));
    }
    return made;
}

}  // namespace

std::shared_ptr<const Layout> remembered_layout(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path,
                                                std::stop_token stop) {
    return layout_by_key(layout_key(line, dpi, font_path), line, dpi, font_path, std::move(stop));
}

void clear_layout_cache() {
    layouts().clear();
    layout_shapes().clear();
    speaker_boxes().clear();
    pictures().clear();
}

SpeakerFonts::SpeakerFonts(std::stop_token stop) : stop_(std::move(stop)) {}
SpeakerFonts::~SpeakerFonts() = default;

Fonts& SpeakerFonts::fonts() {
    if (!fonts_) fonts_ = std::make_unique<Fonts>(stop_);
    return *fonts_;
}

Image outlined(const Image& text, std::int64_t grow, const Rgb& colour) {
    const Image alpha = text.split()[3];
    const std::int64_t pad = grow + 1;
    Image padded = Image::create("L", {static_cast<int>(alpha.width() + 2 * pad), static_cast<int>(alpha.height() + 2 * pad)}, 0);
    padded.paste(alpha, Point{static_cast<int>(pad), static_cast<int>(pad)});
    const Image halo = padded.filter(Filter::max_filter(static_cast<int>(halo_filter(grow))));
    Image out = Image::create("RGBA", padded.size(), Ink::with_alpha(colour, 0));
    out.putalpha(halo);
    Image body = Image::create("RGBA", padded.size(), Ink{0, 0, 0, 0});
    body.paste(text, Point{static_cast<int>(pad), static_cast<int>(pad)});
    out.alpha_composite(body);
    return out;
}

Image skewed(const Image& image, double degrees, bool vertical) {
    const double k = core::py_tan(core::py_max(-60.0, core::py_min(60.0, degrees)) * (core::kPi / 180.0));
    const double w = image.width();
    const double h = image.height();
    // (PIL's affine maps each output pixel back to the input: x' = a x + b y + c, y' = d x + e y + f)
    if (vertical) {
        const Size size{image.width(), static_cast<int>(core::py_trunc_int(h + std::fabs(k) * w) + 1)};
        const std::array<double, 6> data{1, 0, 0, k, 1, k > 0 ? -k * w : 0};
        return image.transform(size, TransformMethod::Affine, data, Resample::Bicubic);
    }
    const Size size{static_cast<int>(core::py_trunc_int(w + std::fabs(k) * h) + 1), image.height()};
    const std::array<double, 6> data{1, k, k > 0 ? -k * h : 0, 0, 1, 0};
    return image.transform(size, TransformMethod::Affine, data, Resample::Bicubic);
}

Image arched(const Image& image, double amount, bool vertical) {
    const std::int64_t w = image.width();
    const std::int64_t h = image.height();
    const std::int64_t length = vertical ? h : w;
    const double rise = std::fabs(amount) * static_cast<double>(length) / 3;
    const std::int64_t pad = core::py_trunc_int(rise) + 1;
    Image out = vertical ? blank(w + pad, h) : blank(w, h + pad);
    const std::int64_t strip = std::max<std::int64_t>(1, floordiv(length, 120));
    for (std::int64_t start = 0; start < length; start += strip) {
        const double t = (static_cast<double>(start) + static_cast<double>(strip) / 2) / static_cast<double>(length) * 2 - 1;  // -1 … 1
        const double lift = rise * (1 - t * t);  // the middle moves most
        const std::int64_t offset = core::py_round_int(amount < 0 ? lift : rise - lift);
        if (vertical) {
            const Image piece = image.crop(Box{0, static_cast<int>(start), static_cast<int>(w), static_cast<int>(std::min(h, start + strip))});
            composite_at(out, piece, amount > 0 ? pad - offset : offset, start);
        } else {
            const Image piece = image.crop(Box{static_cast<int>(start), 0, static_cast<int>(std::min(w, start + strip)), static_cast<int>(h)});
            composite_at(out, piece, start, offset);
        }
    }
    return out;
}

Image gradient_letters(const Image& image, const Json& spec) {
    const auto colour = [&](const char* key, std::initializer_list<std::int64_t> fallback) {
        const Json* value = core::dict_get(spec, key);
        std::vector<std::int64_t> values;
        if (value != nullptr && core::py_truthy(*value)) {
            for (const Json& v : core::iterate(*value)) values.push_back(core::to_int(v));
        } else {
            values = fallback;
        }
        if (values.size() < 3) throw core::PyValueError("a gradient's colour has three values");
        return std::array<double, 3>{static_cast<double>(values[0]), static_cast<double>(values[1]), static_cast<double>(values[2])};
    };
    const auto a = colour("rgb_from", {20, 20, 20});
    const auto b = colour("rgb_to", {230, 40, 40});
    const Json* angle_value = core::dict_get(spec, "angle");
    const double angle = (angle_value != nullptr ? core::to_float(*angle_value) : 90.0) * (core::kPi / 180.0);
    const std::int64_t w = image.width();
    const std::int64_t h = image.height();
    const Image alpha = image.getchannel(3);
    const Box box = alpha.getbbox().value_or(Box{0, 0, static_cast<int>(w), static_cast<int>(h)});  // (from the letters' own edges)
    const double cos_a = core::py_cos(angle);
    const double sin_a = core::py_sin(angle);
    const double top = std::fabs(cos_a) + std::fabs(sin_a);
    const double scale = core::py_max(1e-6, top);
    const auto across = static_cast<double>(std::max<std::int64_t>(1, box.x1 - box.x0 - 1));
    const auto down = static_cast<double>(std::max<std::int64_t>(1, box.y1 - box.y0 - 1));
    std::string rgb(static_cast<std::size_t>(w * h * 3), '\0');
    for (std::int64_t y = 0; y < h; ++y) {
        const double gy = static_cast<double>(y - box.y0) / down;
        for (std::int64_t x = 0; x < w; ++x) {
            const double gx = static_cast<double>(x - box.x0) / across;
            double t = gx * cos_a + gy * sin_a;
            if (!(std::isnan(t)) && !(t > 0)) t = 0.0;  // np.clip(…, 0, None)
            t = t / scale;
            if (!std::isnan(t)) t = t > 0 ? (t < 1 ? t : 1.0) : 0.0;  // np.clip(…, 0, 1)
            for (int c = 0; c < 3; ++c) {
                const double v = a[static_cast<std::size_t>(c)] * (1 - t) + b[static_cast<std::size_t>(c)] * t;
                // .round().astype("uint8"): ties to even, then the low byte of the whole number
                rgb[static_cast<std::size_t>((y * w + x) * 3 + c)] =
                    static_cast<char>(static_cast<std::uint8_t>(static_cast<std::int64_t>(std::nearbyint(v))));
            }
        }
    }
    Image out = Image::frombytes("RGB", image.size(), rgb).convert("RGBA");
    out.putalpha(image.split()[3]);
    return out;
}

std::optional<Image> picture_of(const Json& data) {
    // `key = hash(data)` first: a list or a dict is a TypeError; base64.b64decode of anything but a str (a number,
    // None) raises inside the try: no picture
    core::require_hashable(data);
    if (!data.is_string()) return std::nullopt;
    try {
        const std::string bytes = core::a2b_base64(data.get_ref<const std::string&>());
        return open_image(bytes, kPillowOpenLimits).convert("RGBA");
    } catch (const NotYetPorted&) {
        throw;
    } catch (const core::Error& e) {
        if (e.code() != "format" && e.code() != "unidentified_image" && e.code() != "image_too_large") throw;
    }
    return std::nullopt;
}

std::shared_ptr<const std::optional<Image>> remembered_picture(const Json& data) {
    core::require_hashable(data);  // (Python's hash(data) first)
    if (!data.is_string()) return std::make_shared<const std::optional<Image>>();
    const std::string key = detail::digest_of(data.get_ref<const std::string&>());
    if (auto found = pictures().get(key)) return found;
    auto made = std::make_shared<const std::optional<Image>>(picture_of(data));
    pictures().put(key, made, *made ? static_cast<std::int64_t>((*made)->width()) * (*made)->height() * 4 : 0);
    return made;
}

bool picture_unported(const Json& data) {
    core::require_hashable(data);
    if (!data.is_string()) return false;
    static detail::Memo<bool> sniffed(256, 1024LL * 1024);
    const std::string key = detail::digest_of(data.get_ref<const std::string&>());
    if (auto found = sniffed.get(key)) return *found;
    bool unported = false;
    try {
        unported = unported_image_format(core::a2b_base64(data.get_ref<const std::string&>()));
    } catch (const core::Error& e) {
        if (e.code() != "format") throw;  // (not base64: picture_of finds no picture)
    }
    sniffed.put(key, std::make_shared<const bool>(unported), 1);
    return unported;
}

Image picture_letters(const Image& image, const Json& data) {
    const std::shared_ptr<const std::optional<Image>> remembered = remembered_picture(data);
    const std::optional<Image>& picture = *remembered;
    if (!picture) return image;
    const Image alpha = image.getchannel(3);
    const Box box = alpha.getbbox().value_or(Box{0, 0, image.width(), image.height()});
    Image out = Image::create("RGBA", image.size(), Ink{0, 0, 0, 0});
    out.paste(picture->convert("RGB").resize({std::max(1, box.x1 - box.x0), std::max(1, box.y1 - box.y0)}, Resample::Lanczos),
              Point{box.x0, box.y0});
    out.putalpha(alpha);
    return out;
}

Image warped_letters(const Image& image, const Json& corners) {
    // 文字の変形: the letters' box pulled so its top-left, top-right, bottom-right and bottom-left corners go to these
    // places (each [x, y] as shares of the box). Python takes the map back from each output place (warp._homography:
    // the null vector of eight equations, from numpy.linalg.svd, over h[2][2]); here the same eight equations with
    // h[2][2] = 1 are solved by Gaussian elimination with partial pivoting. The coefficients agree to about 1e-13, not
    // bit for bit (OpenBLAS's LAPACK cannot be reproduced), so a few of the bicubic samples can come out a level or two
    // apart: test_contract_text compares these pictures within ARCHITECTURE.md §9.
    const double w = image.width();
    const double h = image.height();
    std::vector<std::array<double, 2>> dst;
    for (const Json& c : core::iterate(corners)) {
        dst.push_back({core::to_float(core::subscript(c, 0)) * w, core::to_float(core::subscript(c, 1)) * h});
    }
    // (with fewer than four corners Python's SVD picks one map of many: nothing to reproduce)
    if (dst.size() < 4) throw NotYetPorted("text_warp");
    double ox = dst[0][0], oy = dst[0][1], mx = dst[0][0], my = dst[0][1];
    for (const auto& p : dst) {
        ox = core::py_min(ox, p[0]);
        oy = core::py_min(oy, p[1]);
        mx = core::py_max(mx, p[0]);
        my = core::py_max(my, p[1]);
    }
    const Size size{static_cast<int>(std::max<std::int64_t>(1, core::py_trunc_int(mx - ox) + 1)),
                    static_cast<int>(std::max<std::int64_t>(1, core::py_trunc_int(my - oy) + 1))};
    // _homography(output places, the box's corners): zip takes the first four corners
    const std::array<std::array<double, 2>, 4> src{{{0, 0}, {w, 0}, {w, h}, {0, h}}};
    std::array<std::array<double, 9>, 8> m{};
    for (std::size_t i = 0; i < 4; ++i) {
        const double x = dst[i][0] - ox;
        const double y = dst[i][1] - oy;
        const double u = src[i][0];
        const double v = src[i][1];
        m[2 * i] = {x, y, 1, 0, 0, 0, -u * x, -u * y, u};
        m[2 * i + 1] = {0, 0, 0, x, y, 1, -v * x, -v * y, v};
    }
    for (std::size_t col = 0; col < 8; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < 8; ++r) {
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
        }
        // (corners that enclose nothing: Python's SVD still returns some map, which cannot be reproduced)
        if (m[pivot][col] == 0.0) throw NotYetPorted("text_warp");
        std::swap(m[pivot], m[col]);
        for (std::size_t r = col + 1; r < 8; ++r) {
            const double f = m[r][col] / m[col][col];
            for (std::size_t k = col; k < 9; ++k) m[r][k] -= f * m[col][k];
        }
    }
    std::array<double, 8> coeffs{};
    for (std::size_t r = 8; r-- > 0;) {
        double sum = m[r][8];
        for (std::size_t k = r + 1; k < 8; ++k) sum -= m[r][k] * coeffs[k];
        coeffs[r] = sum / m[r][r];
    }
    for (const double c : coeffs) {
        if (!std::isfinite(c)) throw NotYetPorted("text_warp");
    }
    return image.transform(size, TransformMethod::Perspective, coeffs, Resample::Bicubic);
}

void path_text(Image& image, const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path, std::stop_token stop,
               Point origin) {
    const Json st = style_of(line);
    Fonts fonts(std::move(stop));
    const Json& font_value = at(st, "font");
    const Json spec = core::py_truthy(font_value) ? font_value : (font_path ? Json(*font_path) : Json());
    const Face face = face_of(spec, line.balloon == "sfx" ? kDefaultSfx : kDefaultDialogue);
    const Rgb fill = fill_of(st);
    const std::int64_t em = px(float_or(at(st, "size_mm"), kCapMm), dpi);
    std::vector<std::array<double, 2>> pts;
    for (const Json& p : core::iterate(at(st, "text_path"))) {
        pts.push_back({static_cast<double>(px(line.x_mm.value() + core::to_float(core::subscript(p, 0)), dpi)),
                       static_cast<double>(px(line.y_mm.value() + core::to_float(core::subscript(p, 1)), dpi))});
    }
    if (pts.size() < 2) return;
    std::vector<double> lengths{0.0};
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        lengths.push_back(lengths.back() + core::py_dist(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1]));
    }
    const double total = lengths.back();
    std::u32string text;
    for (const char32_t c : u32(line.text)) {
        if (c != U'\n') text.push_back(c);
    }
    text = fonts.normalize(face, text);
    std::u32string letters;
    for (const char32_t c : text) {
        if (c != U'\n' && !is_vs(c)) letters.push_back(c);
    }
    const double tracking = float_or(at(st, "tracking"), 0);
    const double scale_x = float_or(at(st, "scale_x"), 1);
    std::vector<double> widths;
    for (const char32_t c : letters) {
        const std::u32string one(1, c);
        widths.push_back(fonts.font(face, em, one).getlength(one) * scale_x + static_cast<double>(em) * tracking);
    }
    const double run = core::py_float_sum(widths);
    const Json& align = at(st, "align");
    double along = align == Json("left") ? 0.0 : align == Json("right") ? core::py_max(0.0, total - run) : core::py_max(0.0, (total - run) / 2);
    const int weight = line_weight(st);
    const int bold = weight ? bold_px(em, weight) : 0;
    const auto point_at = [&](double d) -> std::array<double, 3> {
        d = core::py_max(0.0, core::py_min(total, d));
        std::size_t k = pts.size() - 2;
        for (std::size_t i = 0; i + 1 < lengths.size(); ++i) {
            if (lengths[i + 1] >= d) {
                k = i;
                break;
            }
        }
        k = std::min(k, pts.size() - 2);
        double seg = lengths[k + 1] - lengths[k];
        if (seg == 0.0) seg = 1.0;
        const double t = (d - lengths[k]) / seg;
        const auto& p0 = pts[k];
        const auto& p1 = pts[k + 1];
        return {p0[0] + (p1[0] - p0[0]) * t, p0[1] + (p1[1] - p0[1]) * t,
                core::py_atan2(p1[1] - p0[1], p1[0] - p0[0]) * (180.0 / core::kPi)};
    };
    for (std::size_t i = 0; i < letters.size(); ++i) {
        const auto [x, y, angle] = point_at(along + widths[i] / 2);
        along += widths[i];
        if (py_isspace(letters[i])) continue;
        const std::u32string one(1, letters[i]);
        const TrueTypeFont& font = fonts.font(face, em, one);
        const std::int64_t size = em * 3;
        Image cell = blank(size, size);
        {
            Draw draw(cell);
            const Ink ink = Ink::with_alpha(fill, 255);
            draw.text(PointD{static_cast<double>(size) / 2, static_cast<double>(size) / 2}, one, font, ink, "ms", bold,
                      bold != 0 ? std::optional<Ink>(ink) : std::nullopt);
        }
        if (std::fabs(scale_x - 1) > 1e-3) {
            cell = cell.resize({static_cast<int>(std::max<std::int64_t>(1, core::py_round_int(static_cast<double>(size) * core::to_float(at(st, "scale_x"))))),
                                static_cast<int>(size)},
                               Resample::Lanczos);
        }
        if (core::py_truthy(at(st, "gradient"))) cell = gradient_letters(cell, at(st, "gradient"));
        if (core::py_truthy(at(st, "outline_mm"))) {
            cell = outlined(cell, px(core::to_float(at(st, "outline_mm")), dpi),
                            core::py_truthy(at(st, "outline_rgb")) ? rgb_tuple(at(st, "outline_rgb")) : Rgb{255, 255, 255});
        }
        const Image turned = cell.rotate(-angle, Resample::Bicubic, false,
                                         std::pair<double, double>{static_cast<double>(cell.width()) / 2, static_cast<double>(cell.height()) / 2});
        const Point to{static_cast<int>(core::py_round_int(x - static_cast<double>(turned.width()) / 2) - origin.x),
                       static_cast<int>(core::py_round_int(y - static_cast<double>(turned.height()) / 2) - origin.y)};
        if (const auto gamma = TextGamma::current()) paste_in_gamma(image, turned, to, *gamma);
        else image.paste(turned, to, &turned);
    }
}

namespace {
thread_local std::optional<double> text_gamma_now;
}  // namespace

TextGamma::TextGamma(std::optional<double> gamma) : was_(text_gamma_now) { text_gamma_now = gamma; }
TextGamma::~TextGamma() { text_gamma_now = was_; }
std::optional<double> TextGamma::current() { return text_gamma_now; }

void paste_in_gamma(Image& image, const Image& letters, Point at, double gamma) {
    const bool rgba = image.mode() == "RGBA";
    if ((!rgba && image.mode() != "RGB") || letters.mode() != "RGBA" || !(gamma > 1.0)) {
        image.paste(letters, at, &letters);
        return;
    }
    const float g = static_cast<float>(gamma);
    std::array<float, 256> power{};  // (v / 255)^g
    for (int v = 0; v < 256; ++v) power[static_cast<std::size_t>(v)] = std::pow(static_cast<float>(v) / 255.0f, g);
    const int x0 = std::max(0, at.x), y0 = std::max(0, at.y);
    const int x1 = std::min(image.width(), at.x + letters.width()), y1 = std::min(image.height(), at.y + letters.height());
    const auto byte = [](float v) { return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (int y = y0; y < y1; ++y) {
        auto* row = reinterpret_cast<unsigned char*>(image.raw()->image[y]);
        const auto* from = reinterpret_cast<const unsigned char*>(letters.raw()->image[y - at.y]);
        for (int x = x0; x < x1; ++x) {
            const unsigned char* s = from + static_cast<std::size_t>(x - at.x) * 4;
            unsigned char* d = row + static_cast<std::size_t>(x) * 4;
            const unsigned a8 = s[3];
            if (a8 == 0) continue;
            const float as = static_cast<float>(a8) / 255.0f;
            const float ab = rgba ? static_cast<float>(d[3]) / 255.0f : 1.0f;
            const float ao = as + ab * (1.0f - as);
            if (ao <= 0.0f) continue;
            const float wb = (1.0f - as) * ab / ao, ws = as / ao;
            for (int c = 0; c < 3; ++c) {
                const float mixed = wb * power[d[c]] + ws * power[s[c]];
                d[c] = byte(std::pow(mixed, 1.0f / g));
            }
            if (rgba) d[3] = byte(ao);
        }
    }
}

void paint_text(Image& image, const core::StoryLine& line, int dpi, bool show_speaker, const std::optional<std::string>& font_path,
                std::stop_token stop, Point origin, SpeakerFonts* speakers, const LetterParts* parts) {
    if (parts != nullptr && !parts->all()) {  // (PSD-01: the parts asked for, where the whole lettering puts them; no name)
        if (core::py_truthy(at(style_of(line), "text_path"))) {
            if (parts->text) path_text(image, line, dpi, font_path, std::move(stop), origin);
            return;
        }
        const SetLayout set = set_layout(line, dpi, font_path, *parts);
        const Point at_page = lettering_at(line, dpi, set.layout.corner_x, set.layout.corner_y);
        // (laid over what is there, so on the blank layer they are the letters' own pixels — pasting them with their
        // alpha as the mask, as the whole page is drawn, would square it: a·a, the faintest edge pixels lost)
        composite_at(image, set.layout.image, at_page.x - origin.x, at_page.y - origin.y);
        return;
    }
    const Json st = style_of(line);
    if (core::py_truthy(at(st, "text_path"))) {
        path_text(image, line, dpi, font_path, std::move(stop), origin);
        return;
    }
    // (where the letters of a layout made before lie is known without it: the layout is looked for, or made, only for a
    // part of the page they reach)
    const std::optional<std::string> key = layout_key(line, dpi, font_path);
    std::shared_ptr<const Layout> layout;
    std::shared_ptr<const LayoutShape> shape = key ? layout_shapes().get(*key) : nullptr;
    if (!shape) {
        layout = layout_by_key(key, line, dpi, font_path, stop);
        shape = std::make_shared<const LayoutShape>(LayoutShape{layout->image.width(), layout->image.height(), layout->em, layout->corner_x,
                                                                layout->corner_y});
    }
    const std::int64_t x = px(line.x_mm.value() + float_or(at(st, "text_dx_mm"), 0), dpi);  // (the words moved inside the balloon)
    const std::int64_t y = px(line.y_mm.value() + float_or(at(st, "text_dy_mm"), 0), dpi);
    const std::int64_t w = px(mm_or(line.w_mm, 40), dpi);
    const std::int64_t h = px(mm_or(line.h_mm, 20), dpi);
    const double cx = static_cast<double>(x) + static_cast<double>(w) / 2;
    const double cy = static_cast<double>(y) + static_cast<double>(h) / 2;
    const std::string kind = kind_of(line);
    const bool shaped = !shape_of(line).empty();  // (type@1, PC5 G-a: a point or box text, placed by lettering_at)
    // (a part of the page: every place below is a whole pixel, so moving it by the part's corner is exact)
    const Point at_page = shaped           ? lettering_at(line, dpi, shape->corner_x, shape->corner_y)
                          : kind == "none" ? Point{static_cast<int>(x), static_cast<int>(y)}  // text only: from the box's corner
                                           : Point{static_cast<int>(core::py_round_int(cx + shape->corner_x)),
                                                   static_cast<int>(core::py_round_int(cy + shape->corner_y))};
    const Box here{origin.x, origin.y, origin.x + image.width(), origin.y + image.height()};
    const auto reaches = [&here](const Box& b) { return b.x0 < here.x1 && here.x0 < b.x1 && b.y0 < here.y1 && here.y0 < b.y1; };
    Box shown_box{at_page.x, at_page.y, at_page.x + shape->width, at_page.y + shape->height};
    const std::optional<Box> clip = clip_of(line, dpi);  // (a box text: what runs past its box is hidden)
    if (clip) shown_box = Box{std::max(shown_box.x0, clip->x0), std::max(shown_box.y0, clip->y0), std::min(shown_box.x1, clip->x1), std::min(shown_box.y1, clip->y1)};
    if (shown_box.width() > 0 && shown_box.height() > 0 && reaches(shown_box)) {
        if (!layout) layout = layout_by_key(key, line, dpi, font_path, stop);
        std::optional<Image> cut;
        Point put = at_page;
        if (clip && shown_box != Box{at_page.x, at_page.y, at_page.x + layout->image.width(), at_page.y + layout->image.height()}) {
            cut = layout->image.crop(Box{shown_box.x0 - at_page.x, shown_box.y0 - at_page.y, shown_box.x1 - at_page.x, shown_box.y1 - at_page.y});
            put = Point{shown_box.x0, shown_box.y0};
        }
        const Image& letters = cut ? *cut : layout->image;
        if (const auto gamma = TextGamma::current()) paste_in_gamma(image, letters, Point{put.x - origin.x, put.y - origin.y}, *gamma);
        else image.paste(letters, Point{put.x - origin.x, put.y - origin.y}, &letters);
    }
    if (show_speaker && !line.speaker.empty() && kind != "none") {
        const std::int64_t size = std::max<std::int64_t>(8, floordiv(shape->em * 2, 3));
        const PointD at_name{static_cast<double>(x), static_cast<double>(std::max<std::int64_t>(0, y - shape->em))};
        // (a name of one row whose box is known and does not reach this part: no font opened for it)
        const bool one_row = line.speaker.find('\n') == std::string::npos;
        const std::optional<std::string> name_key = one_row ? detail::memo_key(Json::array({line.speaker, size})) : std::nullopt;
        if (name_key) {
            if (const auto box = speaker_boxes().get(*name_key)) {
                const Box covered{static_cast<int>(at_name.x) + box->x0, static_cast<int>(at_name.y) + box->y0,
                                  static_cast<int>(at_name.x) + box->x1, static_cast<int>(at_name.y) + box->y1};
                if (!reaches(covered)) return;
            }
        }
        std::optional<SpeakerFonts> own;
        if (speakers == nullptr) speakers = &own.emplace(stop);
        const TrueTypeFont& font = speakers->fonts().font(face_of(Json()), size);
        const std::u32string name = u32(line.speaker);
        if (name_key) {
            const std::array<int, 4> b = font.getbbox(name);
            speaker_boxes().put(*name_key, std::make_shared<const Box>(Box{b[0] - 2, b[1] - 2, b[2] + 2, b[3] + 2}),
                                static_cast<std::int64_t>(sizeof(Box)));
        }
        Draw draw(image);
        draw.text(PointD{at_name.x - origin.x, at_name.y - origin.y}, name, font, Ink{90, 90, 90});
    }
}

}  // namespace genko::render::text
