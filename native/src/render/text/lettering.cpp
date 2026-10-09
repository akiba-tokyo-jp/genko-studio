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

#include "core/base64.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "render/draw.hpp"
#include "render/not_yet_ported.hpp"
#include "render/png.hpp"
#include "render/text/fonts.hpp"
#include "render/text/memo.hpp"

namespace genko::render::text {

using core::Json;

namespace {

constexpr double kSqrt2 = 1.4142135623730951;  // SQRT2 = 2 ** 0.5
constexpr double kCapMm = 5.0;                  // CAP_MM
constexpr double kSfxCapMm = 12.0;              // SFX_CAP_MM
constexpr double kFitPad = 0.15;                // FIT_PAD
constexpr double kHandPower = 2.6;              // HAND_POWER
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

// the default text colour, or style.rgb
Rgb fill_of(const Json& st) { return core::py_truthy(at(st, "rgb")) ? rgb_tuple(at(st, "rgb")) : kText; }

std::size_t cell_count(std::u32string_view text, bool tcy, bool latin) { return cells(text, tcy, latin).size(); }

// _vertical(line, st, face, em, inner_h, fill)
Image vertical_text(Fonts& fonts, const core::StoryLine& line, const Json& st, const Face& face, std::int64_t em, double inner_h,
                    const Rgb& fill) {
    std::u32string text = set_text(fonts, line, st, face);
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
            for (std::size_t i = 0; i < cols.size(); ++i) {
                if (i > 0) joined.push_back(U'\n');
                joined += cols[i];
            }
            text = std::move(joined);
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
    return compose(fonts, face, args);
}

// _horizontal(line, st, face, em, inner_w, fill): rows left to right with kinsoku, centred (or aligned by
// style.align: left / center / right / justify). Ruby sits above its words and 傍点 just above the characters (the
// ruby above them); part of a line can be larger, smaller, bolder or in another colour (style_runs), which makes its
// row taller.
Image horizontal_text(Fonts& fonts, const core::StoryLine& line, const Json& st, const Face& face, std::int64_t em,
                      double inner_w, const Rgb& fill) {
    const std::u32string text = set_text(fonts, line, st, face);
    const double tracking = float_or(at(st, "tracking"), 0);
    std::u32string flat;
    for (const char32_t c : text) {
        if (c != U'\n') flat.push_back(c);
    }
    const int weight = line_weight(st);
    const std::vector<Json> styles_flat = char_styles(flat, line.style_runs, weight ? Json{{"bold", weight}} : Json());
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
    };
    std::vector<RubyAt> ruby_at;
    pos = 0;
    for (const Json& run : line.ruby_runs) {
        if (!core::py_truthy(run) || core::length(run) < 2) continue;
        const Json base_value = core::subscript(run, 0);
        const Json ruby_value = core::subscript(run, 1);
        if (!core::py_truthy(base_value) || !core::py_truthy(ruby_value)) continue;
        const std::u32string base = fonts.normalize(face, u32(core::py_str(base_value)));
        const std::size_t found = text.find(base, pos);
        if (found != std::u32string::npos) {
            ruby_at.push_back({found, found + base.size(), u32(core::py_str(ruby_value))});
            pos = found + base.size();
        }
    }
    const std::int64_t mark_h = !marked.empty() ? core::py_round_int(static_cast<double>(em) * 0.36) : 0;
    const std::int64_t ruby_h = !ruby_at.empty() ? std::max<std::int64_t>(6, floordiv(em, 2)) : 0;
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
    const std::int64_t above = mark_h + ruby_h;
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
    const auto out_w = std::max<std::int64_t>(1, static_cast<std::int64_t>(core::py_ceil(widest)));
    std::int64_t total = 0;
    for (const std::int64_t h : heights) total += h;
    Image out = blank(out_w, std::max<std::int64_t>(1, total));
    const Json& align_value = at(st, "align");
    const std::string align = align_value == Json("left") || align_value == Json("right") ? align_value.get<std::string>() : "center";
    const bool justify = align_value == Json("justify");  // 均等揃え: each row (but the last) spread over the whole width
    const std::string mark_kind = core::py_str(core::py_or(at(st, "emphasis_mark"), Json("sesame")));
    std::map<std::size_t, std::tuple<double, double, std::int64_t>> xs;  // place in text → (left, right, row top)
    std::int64_t top = 0;
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const auto& row = rows[r];
        double x = align == "left" || justify ? 0.0 : (static_cast<double>(out_w) - widths[r]) / (align == "center" ? 2 : 1);
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
            {
                Draw draw(out);
                const Ink ink = Ink::with_alpha(rgb, 255);
                draw.text(PointD{x, base_y + static_cast<double>(tallest - size)}, letter, fonts.font(face, size, std::u32string(1, text[i])),
                          ink, {}, thick, thick != 0 ? std::optional<Ink>(ink) : std::nullopt);
            }
            const double w = advance(i) - static_cast<double>(em) * tracking;
            if (marked.contains(i)) {
                draw_mark(out, x + w / 2,
                          static_cast<double>(top + ruby_h) + static_cast<double>(mark_h) / 2 +
                              static_cast<double>(std::max<std::int64_t>(1, floordiv(em, 16))),
                          static_cast<double>(mark_h), mark_kind, rgb, false);
            }
            xs[i] = {x, x + w, top};
            x += advance(i) + spread;
        }
        top += heights[r];
    }
    for (const RubyAt& run : ruby_at) {
        // a word split over rows gets its ruby split in proportion
        std::map<std::int64_t, std::vector<std::size_t>> groups;
        for (std::size_t i = run.first; i < run.last; ++i) {
            if (const auto it = xs.find(i); it != xs.end()) groups[std::get<2>(it->second)].push_back(i);
        }
        std::int64_t count = 0;
        for (const auto& [row_top, chars] : groups) count += static_cast<std::int64_t>(chars.size());
        if (count == 0) count = 1;
        const auto len = static_cast<std::int64_t>(run.ruby.size());
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
            const std::u32string part = run.ruby.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
            const TrueTypeFont& font = fonts.font(face, ruby_h, part.substr(0, 1));
            const double span = font.getlength(part);
            const double centre = (std::get<0>(xs.at(chars.front())) + std::get<1>(xs.at(chars.back()))) / 2;
            Draw draw(out);
            draw.text(PointD{core::py_max(0.0, core::py_min(static_cast<double>(out_w) - span, centre - span / 2)),
                             static_cast<double>(row_top)},
                      part, font, Ink::with_alpha(fill, 255));
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

Layout text_layout(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path, std::stop_token stop) {
    // The lettering, its em, and where its top left sits from the box's middle (px). In an ellipse the letters are
    // fitted to the outline by their own shape, and sit where they fit best.
    const Json st = style_of(line);
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
    if (core::py_truthy(at(st, "size_mm"))) {
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
    const bool hug = one_of(kind, kHugged);
    const double keep = hug ? hug_keep(kind, st) : 1.0;
    const double power = hug_power(kind, st);
    const auto room = [&](std::int64_t size) -> std::pair<double, double> {
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
    for (int attempt = 0; attempt < 12; ++attempt) {
        image = vertical ? vertical_text(fonts, line, st, face, em, inner_h, fill) : horizontal_text(fonts, line, st, face, em, inner_w, fill);
        if (std::fabs(scale_x - 1) > 1e-3) {  // 長体 (< 1) or 平体 (> 1): the letters narrower or wider than tall
            image = image.resize({static_cast<int>(std::max<std::int64_t>(1, core::py_round_int(image.width() * scale_x))), image.height()},
                                 Resample::Lanczos);
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
    if (grow != 0) image = outlined(image, grow, core::py_truthy(at(st, "outline_rgb")) ? rgb_tuple(at(st, "outline_rgb")) : Rgb{255, 255, 255});
    if (core::py_truthy(at(st, "arc"))) image = arched(image, core::to_float(at(st, "arc")), vertical);
    double skew = float_or(at(st, "skew_deg"), 0);
    if (skew == 0.0) skew = core::py_truthy(at(st, "italic")) ? 12.0 : 0.0;  // (italic: a light lean)
    if (skew != 0.0) image = skewed(image, skew, vertical);
    if (core::py_truthy(at(st, "warp"))) image = warped_letters(image, at(st, "warp"));
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
    out.image = std::move(image);
    out.em = em;
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
    return detail::memo_key(Json::array({line.text, line.balloon, line.wrap, line.w_mm.json(), line.h_mm.json(),
                                         detail::keyed_style(line.style), Json(line.ruby_runs), Json(line.emphasis_runs), style_runs,
                                         dpi, font_path ? Json(*font_path) : Json()}));
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
    const Image halo = padded.filter(Filter::max_filter(static_cast<int>(std::min<std::int64_t>(grow * 2 + 1, 61) | 1)));
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
        image.paste(turned,
                    Point{static_cast<int>(core::py_round_int(x - static_cast<double>(turned.width()) / 2) - origin.x),
                          static_cast<int>(core::py_round_int(y - static_cast<double>(turned.height()) / 2) - origin.y)},
                    &turned);
    }
}

void paint_text(Image& image, const core::StoryLine& line, int dpi, bool show_speaker, const std::optional<std::string>& font_path,
                std::stop_token stop, Point origin, SpeakerFonts* speakers) {
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
    // (a part of the page: every place below is a whole pixel, so moving it by the part's corner is exact)
    const Point at_page = kind == "none" ? Point{static_cast<int>(x), static_cast<int>(y)}  // text only: from the box's corner
                                         : Point{static_cast<int>(core::py_round_int(cx + shape->corner_x)),
                                                 static_cast<int>(core::py_round_int(cy + shape->corner_y))};
    const Box here{origin.x, origin.y, origin.x + image.width(), origin.y + image.height()};
    const auto reaches = [&here](const Box& b) { return b.x0 < here.x1 && here.x0 < b.x1 && b.y0 < here.y1 && here.y0 < b.y1; };
    if (reaches(Box{at_page.x, at_page.y, at_page.x + shape->width, at_page.y + shape->height})) {
        if (!layout) layout = layout_by_key(key, line, dpi, font_path, stop);
        image.paste(layout->image, Point{at_page.x - origin.x, at_page.y - origin.y}, &layout->image);
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
