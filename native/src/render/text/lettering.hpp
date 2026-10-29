#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"
#include "render/text/tategaki.hpp"

// The lettering of a line (Python's genko/balloons.py, the text side): its style, the room inside its balloon,
// the words set across (rows with kinsoku, 約物の詰め, ruby above, 傍点) or down (tategaki.compose, broken between
// phrases), shrunk until they fit the balloon by their own outline, then the letter effects in Python's order
// (gradient, picture, outline, arc, lean, warp) and where the letters sit; text on a path; the letters and the
// speaker's name painted on the page. The balloons themselves (shapes, tails, joined outlines): balloons.hpp.

namespace genko::render::text {

// style_of(line): DEFAULTS with the line's own style over them.
core::Json style_of(const core::StoryLine& line);
// line_weight(st): style.weight (normal / bold / heavy) over the older bold switch.
int line_weight(const core::Json& st);
// px(mm, dpi): max(1, round(mm / 25.4 * dpi))
std::int64_t px(double mm, int dpi);

// _inner(kind, w, h, pad, depth): the room for the words inside the shape.
std::pair<double, double> inner(std::string_view kind, double w, double h, double pad, const core::Json& depth);
// hug_power(kind, st) and _hug_keep(kind, st): the hand-drawn oval's fullness, the share of the box the letters use.
double hug_power(std::string_view kind, const core::Json& st);
double hug_keep(std::string_view kind, const core::Json& st);

// ink_rects(image, em): the letters' outline as upright strips (x0, y0, x1, y1) with ink above 40.
std::vector<std::array<std::int64_t, 4>> ink_rects(const Image& image, std::int64_t em);
// block_points(rects, pad): the strips' corners, pad out, around the block's middle.
std::vector<std::array<double, 2>> block_points(const std::vector<std::array<std::int64_t, 4>>& rects, double pad);
// fit_in(points, a, b, power): (worst, dx, dy) of the points in the oval of half-sizes a, b, moved to fit best.
struct Fit {
    double worst = 0.0;
    double dx = 0.0;
    double dy = 0.0;
};
Fit fit_in(const std::vector<std::array<double, 2>>& points, double a, double b, double power);

// text_layout(line, dpi, font_path): the lettering (RGBA), its em in px, and where its top left sits from the box's
// middle (px).
struct Layout {
    Image image;
    std::int64_t em = 0;
    double corner_x = 0.0;
    double corner_y = 0.0;
};
Layout text_layout(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path = std::nullopt,
                   std::stop_token stop = {});
// What setting the line's rubies by place (ruby-layout@1) could not do as asked (render::text::RubyNote: overflow,
// overlap, font_fallback, glyph_missing, not_drawn), each kind once a ruby, as text_layout sets them; nothing for a
// line of Python's.
std::vector<RubyNote> ruby_notes(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path = std::nullopt);
// PSD-01 (internal, not an op): where each drawn ruby lies on the page and where its base does, so the PSD writer can
// give each reading a text layer of its own. A place for each ruby drawn (one needing review is not), in the order the
// lettering sets them; a base broken over columns (rows) gives a place for each part. `span`: which of the line's
// ruby_spans (a line of Python's: of its runs as core::ruby::from_runs finds them); `reading`: the letters drawn there;
// `ruby`: the box they are set in (down: their em squares; across: their advances by the font's ascent and descent
// from where they are drawn); `base`: the box its base's characters are set in there (down: their cells in the column;
// across, the same as the reading's); `letters`: where each letter is set (down: its em square's top left; across: the
// point the font draws it from, the ascender's left); `size`: the letters' size (px); `face`: their face. Page pixels
// at `dpi`, as paint_text puts the lettering. `exact`: false when the letters are not where the boxes say — an effect
// after they were set (scale_x: the boxes follow it, the letters drawn narrower or wider than `size`; arc, skew_deg or
// italic, warp: the boxes are where they were set), a line turned by rotate_deg (its group drawn turned: the boxes
// unturned), a line of Python's that the explicit rule finds otherwise than its lettering does. A line on a path
// (text_path) draws no ruby: no places.
struct RubyPlace {
    std::size_t span = 0;
    std::string reading;
    Box ruby;
    Box base;
    std::vector<PointD> letters;
    std::int64_t size = 0;
    Face face;
    bool vertical = true;
};
struct RubyPlaces {
    std::vector<RubyPlace> places;
    bool exact = true;
};
RubyPlaces ruby_places(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path = std::nullopt);

// PSD-01 (internal, not an op): which parts of a placed line are drawn — the editable PSD puts its balloon, its text
// and its rubies on layers of their own. `halo`: the letters' outline (白フチ; the PSD gives it as the text layer's
// stroke instead). Whatever is drawn is laid out as when everything is: a part drawn alone is where it is then.
struct LetterParts {
    bool balloon = true;
    bool text = true;
    bool ruby = true;
    bool halo = true;
    bool all() const { return balloon && text && ruby && halo; }
};

// PSD-01 (internal): one character of a line's text as the lettering set it, before its letter effects: what it is (a
// cell: a character with its variation selector; down, also the letters of a 縦中横 cell or of half-width letters laid
// on their side), its size (px), weight (0 normal, 1 bold, 2 heavy) and colour, the font file it is drawn in (a
// bundled font's name or a file's path, as Fonts::path_for gives it), and where: down, its cell; across, its advance
// by the font's ascent and descent from where it is drawn.
struct SetChar {
    std::u32string text;
    std::int64_t size = 0;
    int weight = 0;
    Rgb rgb;
    std::string font;
    bool tcy = false;       // (down) letters set across in one cell (縦中横)
    bool sideways = false;  // (down) half-width letters laid on their side
    BoxF box;
};
// A column (down, right to left) or a row (across, top to bottom) of the text, and its line: down, the column's
// middle (`axis`, x) and the top of its first cell (`start`); across, its baseline (the tallest letter's, y) and its
// left. `end`: down, the foot of its last cell; across, its right.
struct SetRow {
    std::vector<SetChar> chars;
    double axis = 0.0;
    double start = 0.0;
    double end = 0.0;
};
// A line's lettering as set (its own pixels, before the letter effects): the text, its em, face and alignment (down:
// top | center | bottom | justify; across: left | center | right | justify), the box the text is set in (`area`),
// its rubies (each part as render::text::RubyDrawn has it), and what the lettering does to that picture afterwards:
// `to_image` maps a set pixel to the lettering's picture (x' = a·x + c·y + e, y' = b·x + d·y + f: 長体・平体 scale_x,
// the halo's room, the lean of skew_deg or italic). `italic`: the lean is style.italic's (a slanted face), not
// skew_deg's. `halo`: the outline's width (px) and colour. `effects`: what has no such map — "gradient",
// "picture_fill", "arc", "warp", "text_path" (letters on a path: nothing else is set) — and "emphasis" (傍点).
struct SetLetters {
    bool vertical = true;
    std::int64_t em = 0;
    Face face;
    std::string align;
    std::vector<SetRow> rows;
    std::vector<RubyDrawn> rubies;
    BoxF area;
    Size size;
    std::array<double, 6> to_image{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    bool italic = false;
    std::optional<std::pair<std::int64_t, Rgb>> halo;
    std::vector<std::string> effects;
};
// The lettering of a line as set, and its picture drawn with only `parts` (the letters where they are when everything
// is drawn), its em and corner as text_layout's. A line on a path: only its `effects`.
struct SetLayout {
    SetLetters set;
    Layout layout;
};
SetLayout set_layout(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path, const LetterParts& parts);
// The face a line's letters are set in (its style's font; the effects face for "sfx"; else the book's font or the
// default) and whether a font its style names was not found (and the default used instead).
std::pair<Face, bool> line_face(const core::StoryLine& line, const std::optional<std::string>& font_path = std::nullopt);
// Where paint_text puts the top left of a line's lettering on the page (px), for a lettering with this corner.
Point lettering_at(const core::StoryLine& line, int dpi, double corner_x, double corner_y);
// type@1 (PC5 G-a, SPEC.md TEXT-02): a record's shape (native_type.shape): "point" — ポイントテキスト, set at its size
// unbroken but where its text breaks, its first row's baseline (down: its first column's middle, at its top) on its
// anchor_mm, left, centred or right of it by its alignment (down: top, middle or foot) — or "box" — 段落テキスト, set at
// its size broken at its box (x_mm…h_mm), from the box's top left (down: from its top right; across centred or right
// by its alignment, down by its alignment), what runs past the box hidden —, or none: a line's own setting (in its
// box, fitted to its balloon). Neither point nor box text shrinks or fits a balloon; their size is style.size_mm (the
// lines' 5 mm without one). native_type.antialias "none" draws each pixel whole or not at all (a cover of half or
// more); the others are drawn smooth (PSD keeps their name).
std::string shape_of(const core::StoryLine& line);
// Where the lettering of a line lies on the page (px): its picture's box as paint_text puts it, the box its letters
// are drawn in (none: nothing drawn), the box a box text is cut to (none for others), whether its letters run past
// that box (Photoshop's overflow mark), and its em (px).
struct Placement {
    Box picture;
    std::optional<Box> letters;
    std::optional<Box> clip;
    bool overflow = false;
    std::int64_t em = 0;
};
Placement placement(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path = std::nullopt, std::stop_token stop = {});

// text_image(line, dpi, font_path): the lettering and its em.
std::pair<Image, std::int64_t> text_image(const core::StoryLine& line, int dpi,
                                          const std::optional<std::string>& font_path = std::nullopt, std::stop_token stop = {});
// text_layout's result remembered for the same line (what it reads of it: its text, kind, wrap, size, style and
// runs, a picture in its style by its digest), dpi and font: a page drawn again, or in parts, sets each line once.
// clear_layout_cache() forgets them (and the pictures remembered).
std::shared_ptr<const Layout> remembered_layout(const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path,
                                                std::stop_token stop = {});
void clear_layout_cache();

class Fonts;

// The speakers' names' font for the lines drawn on one part of a page: opened once for all of them (when a name is
// drawn there), not once a line.
class SpeakerFonts {
public:
    explicit SpeakerFonts(std::stop_token stop = {});
    ~SpeakerFonts();
    SpeakerFonts(const SpeakerFonts&) = delete;
    SpeakerFonts& operator=(const SpeakerFonts&) = delete;
    Fonts& fonts();

private:
    std::stop_token stop_;
    std::unique_ptr<Fonts> fonts_;
};

// カラー設定 → 文字の色をガンマで混ぜる (color-model@1's book text_gamma, SPEC COLOR-03 PC1b; PhotoCraft's
// blendTextGamma): while one is alive on this thread (render_page makes one from the book), the letters of the lines
// paint_text and path_text put on a page are mixed with what lies under them in that gamma. None: as Python's (Pillow's
// paste with the letters' alpha).
class TextGamma {
public:
    explicit TextGamma(std::optional<double> gamma);
    ~TextGamma();
    TextGamma(const TextGamma&) = delete;
    TextGamma& operator=(const TextGamma&) = delete;
    static std::optional<double> current();

private:
    std::optional<double> was_;
};

// The letters ("RGBA") over the picture ("RGB" or "RGBA") at `at`, mixed in `gamma` (PhotoCraft's composite_gamma in
// the normal mode): each colour ((1 − a)·under^g + a·letter^g)^(1/g) over an opaque picture — weighted by the alphas
// over a transparent one, whose alpha becomes a + under·(1 − a). Other pictures: Pillow's paste.
void paste_in_gamma(Image& image, const Image& letters, Point at, double gamma);

// _paint_text(image, line, dpi, show_speaker, font_path): the line's letters on the page (from its box's middle, or
// its corner for "none"; along its path when it has one) and the speaker's name above the box. `origin`: where the
// image's top left lies on the page, when it holds a part of it (the same pixels as the whole page cut there). A part
// the letters do not reach — known from the line's layout made before — is left without looking for the layout.
void paint_text(Image& image, const core::StoryLine& line, int dpi, bool show_speaker,
                const std::optional<std::string>& font_path = std::nullopt, std::stop_token stop = {}, Point origin = {},
                SpeakerFonts* speakers = nullptr, const LetterParts* parts = nullptr);
// path_text(image, line, dpi, font_path): 文字をパスに沿わせる — each letter stood on the path, turned with it.
void path_text(Image& image, const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path = std::nullopt,
               std::stop_token stop = {}, Point origin = {});

// picture_of(data): a picture balloon's (or picture letters') image, base64 PNG, JPEG, BMP or GIF decoded to RGBA;
// nothing when the data is not a picture (Python's `except Exception`). Unhashable data (a list, a dict) is Python's
// TypeError; other formats Pillow opens: NotYetPorted("image_format").
std::optional<Image> picture_of(const core::Json& data);
// picture_of(data) remembered by the data's digest (Python's balloons._PICTURES keeps the 64 last by hash(data)), for
// the threads drawing a page's parts: each picture decoded once. Python's errors as picture_of's (not remembered).
std::shared_ptr<const std::optional<Image>> remembered_picture(const core::Json& data);
// Whether picture_of(data) stops with NotYetPorted("image_format") (a picture of a format Pillow opens and this build
// does not yet), known from the decoded bytes' first ones without opening the picture; remembered by the data's
// digest. False for data picture_of finds no picture in. (Python's hash(data) first, as picture_of.)
bool picture_unported(const core::Json& data);

// The letter effects.
// outlined(text_img, grow, colour): a halo (白フチ) grow px wide.
Image outlined(const Image& text, std::int64_t grow, const Rgb& colour);
// skewed(image, degrees, vertical): across text leans like italics, vertical text's columns slide down to the left.
Image skewed(const Image& image, double degrees, bool vertical);
// arched(image, amount, vertical): bent into a bow (amount 1 lifts the middle by a third of the length).
Image arched(const Image& image, double amount, bool vertical);
// gradient_letters(image, spec): coloured from rgb_from to rgb_to (top to bottom, or at `angle` degrees).
Image gradient_letters(const Image& image, const core::Json& spec);
// picture_letters(image, data): painted with a picture (base64 PNG, JPEG, BMP or GIF) stretched over them; the
// letters unchanged when the data is not a picture (other formats Pillow opens: NotYetPorted("image_format")).
Image picture_letters(const Image& image, const core::Json& data);
// warped_letters(image, corners): the box pulled so its corners go to these places (shares of the box). Python finds
// the map with numpy's SVD, here by Gaussian elimination: the same map to about 1e-13, so a few pixels can differ by a
// level or two (compared within ARCHITECTURE.md §9). Fewer than four corners, or four that enclose nothing, have no
// map to reproduce: NotYetPorted("text_warp").
Image warped_letters(const Image& image, const core::Json& corners);

}  // namespace genko::render::text
