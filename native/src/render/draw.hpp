#pragma once

#include <memory>
#include <array>
#include <cstdint>
#include <utility>
#include <stop_token>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <vector>

#include "render/image.hpp"

class QByteArray;  // (font_bytes, below)

// Pillow's ImageDraw (PIL/ImageDraw.py and the draw methods of _imaging.c) over libImaging's Draw.c: the same
// coordinates (each one truncated to an int as _imaging.c does), the same inks, the same Python-level joints of a
// wide line (joint="curve"), so a shape covers the same pixels as in the Python baseline. Text (M4) goes through
// FreeType as Pillow's _imagingft.c lays it out without raqm (the BASIC layout the reference is held to).

struct FT_FaceRec_;
struct FT_LibraryRec_;

namespace genko::render {

class TrueTypeFonts;

struct PointD {
    double x = 0.0;
    double y = 0.0;
};

// PC5 G-b (the OpenType typesetting, render/text/shaping): a glyph by its id in a font (HarfBuzz's choice), where its pen
// lies from the run's origin on its baseline (px; y down).
struct GlyphPlace {
    std::uint32_t id = 0;
    double x = 0.0;
    double y = 0.0;
};
// How each glyph's outline is drawn about its own origin: x' = scale_x·x + lean·y, y' = scale_y·y (y up, as the font's;
// lean of the scaled height: a faux italic's slant).
struct GlyphShape {
    double scale_x = 1.0;
    double scale_y = 1.0;
    double lean = 0.0;
    bool plain() const { return scale_x == 1.0 && scale_y == 1.0 && lean == 0.0; }
};
// A font's lines (px at its size): the underline's place below the baseline and its thickness, the strikeout's above it
// and its thickness (the font's post and OS/2 tables; a share of the em where it has none).
struct FontLines {
    double underline = 0.0;
    double underline_thickness = 1.0;
    double strikeout = 0.0;
    double strikeout_thickness = 1.0;
};

enum class Joint { None, Curve };

std::string text_font(std::string_view spec,std::string_view text,std::stop_token stop={});
double text_length(std::string_view text,std::string_view font,int size,std::stop_token stop={});

// A shared horizontal text mask and its LA-anchor bounds; immutable bundled font, no display required.
std::pair<Image,std::array<int,4>> text_mask(std::string_view text,std::string_view font,int size,
                                         PointD fraction={},std::stop_token stop={},int stroke=0);

// Pillow's FreeTypeFont (ImageFont.truetype(path, size)) under the BASIC layout: one glyph per code point, advanced
// by its horizontal advance and the font's own kerning pairs (Pillow adds them rounded to whole pixels), never
// shaped. Its methods compute what _imagingft.c's font_getlength, font_getsize and font_render compute, in the same
// integer and single-precision arithmetic, so a mask has the same pixels and offset as Pillow's. Owned by the
// TrueTypeFonts that opened it; like Pillow's font object it is not for two threads at once.
class TrueTypeFont {
public:
    TrueTypeFont(const TrueTypeFont&) = delete;
    TrueTypeFont& operator=(const TrueTypeFont&) = delete;
    ~TrueTypeFont();

    // FreeTypeFont.size: the size it was opened at.
    int size() const { return size_; }
    // getmetrics(): (ascent, descent), the descent positive below the baseline.
    std::pair<int, int> getmetrics() const;
    // getname(): (family, style) as the font names them (nothing for a name it does not have).
    std::pair<std::optional<std::string>, std::optional<std::string>> getname() const;
    // getlength(text): the advance in pixels (a whole number under BASIC).
    double getlength(std::u32string_view text) const;
    // getbbox(text, stroke_width=..., anchor=...): (left, top, right, bottom) from the anchor point. anchor "" is "la".
    std::array<int, 4> getbbox(std::u32string_view text, int stroke_width = 0, std::string_view anchor = {}) const;
    // getmask2(text, mode "L", stroke_width=..., anchor=..., start=(x, y), stroke_filled=...): the coverage and where
    // its top left lies from the anchor point. start is the fraction of the position the text is drawn at (Pillow
    // passes it as a C float); stroke_filled is True when ImageDraw.text asks (the stroke's outer border, filled).
    std::pair<Image, Point> getmask2(std::u32string_view text, int stroke_width = 0, std::string_view anchor = {},
                                     float start_x = 0.0F, float start_y = 0.0F, bool stroke_filled = false) const;
    // PC5 G-b: the coverage of glyphs by id, each at its place from the origin (its fraction `start` of a pixel on), shaped
    // so, the stroke's outer border filled under them as getmask2's; and where its top left lies from the origin's pixel.
    std::pair<Image, Point> glyph_mask(std::span<const GlyphPlace> glyphs, int stroke_width = 0, const GlyphShape& shape = {},
                                       float start_x = 0.0F, float start_y = 0.0F) const;
    // PC5 G-b: the glyph id the font's cmap gives a character (0: none), its lines, and its units per em.
    std::uint32_t glyph_index(char32_t ch) const;
    FontLines lines() const;

private:
    friend class TrueTypeFonts;
    TrueTypeFont(FT_FaceRec_* face, FT_LibraryRec_* library, int size, std::stop_token stop);

    FT_FaceRec_* face_ = nullptr;
    FT_LibraryRec_* library_ = nullptr;
    int size_ = 0;
    std::stop_token stop_;
};

// The fonts of one piece of lettering, each opened once at each size (Python's lru_cache'd genko.fonts.truetype): a
// bundled font by its name ("gothic", "mincho", "maru", "hand", "sfx", "sfx_pop"; their bytes are read once per
// process) or a font file by its path. FreeType's memory and the fonts' bytes count in the image budget, as
// text_mask's do. Not for two threads at once.
class TrueTypeFonts {
public:
    explicit TrueTypeFonts(std::stop_token stop = {});
    ~TrueTypeFonts();
    TrueTypeFonts(const TrueTypeFonts&) = delete;
    TrueTypeFonts& operator=(const TrueTypeFonts&) = delete;

    // ImageFont.truetype(font, size) for a size of 1 or more. A font file that cannot be opened or that FreeType cannot
    // read is ImageFont.truetype's OSError: NotYetPorted("default_font") (genko.fonts.truetype, text::Fonts::truetype,
    // draws with load_default() then; render._font tries its next font). A font file past 256 MB is not read:
    // NotYetPorted("large_font").
    const TrueTypeFont& truetype(const std::string& font, int size);
    // PC5 G-b: the font at `size` with its variable axes set ({tag, value}: an axis the font has, clamped to its range; others
    // left at their defaults). No axes: truetype(font, size).
    const TrueTypeFont& truetype(const std::string& font, int size, const std::vector<std::pair<std::string, float>>& axes);
    // ImageFont.load_default(size) under FreeType: Pillow 12.3.0's own Aileron Regular (the subset its ImageFont.py
    // carries, native/third_party/pillow/fonts/load_default.ttf) at `size` (Pillow's default: 10).
    const TrueTypeFont& load_default(int size = 10);
    std::stop_token stop() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

class Draw {
public:
    // ImageDraw.Draw(image[, mode]): mode "RGBA" on an RGB image blends the ink into the picture.
    explicit Draw(Image& image, std::string_view mode = {});
    ~Draw();
    Draw(const Draw&) = delete;
    Draw& operator=(const Draw&) = delete;

    // line(xy, fill=None, width=1, joint=None)
    void line(std::span<const PointD> xy, const std::optional<Ink>& fill = std::nullopt, int width = 1,
              Joint joint = Joint::None);
    // polygon(xy, fill=None, outline=None, width=1)
    void polygon(std::span<const PointD> xy, const std::optional<Ink>& fill = std::nullopt,
                 const std::optional<Ink>& outline = std::nullopt, int width = 1);
    // ellipse(xy, fill=None, outline=None, width=1)
    void ellipse(const BoxF& box, const std::optional<Ink>& fill = std::nullopt,
                 const std::optional<Ink>& outline = std::nullopt, int width = 1);
    // rectangle(xy, fill=None, outline=None, width=1)
    void rectangle(const BoxF& box, const std::optional<Ink>& fill = std::nullopt,
                   const std::optional<Ink>& outline = std::nullopt, int width = 1);
    // rounded_rectangle(xy, radius=0, fill=None) (all four corners rounded; a fill only): its corners as pie slices,
    // the rest as rectangles; an ellipse when the corners meet both ways, a rectangle when they have no curve.
    void rounded_rectangle(const BoxF& box, double radius, const Ink& fill);
    // point(xy, fill=None)
    void point(std::span<const PointD> xy, const std::optional<Ink>& fill = std::nullopt);
    // arc(xy, start, end, fill=None, width=1)
    void arc(const BoxF& box, double start, double end, const std::optional<Ink>& fill = std::nullopt, int width = 1);
    // pieslice(xy, start, end, fill=None, outline=None, width=1)
    void pieslice(const BoxF& box, double start, double end, const std::optional<Ink>& fill = std::nullopt,
                  const std::optional<Ink>& outline = std::nullopt, int width = 1);
    // chord(xy, start, end, fill=None, outline=None, width=1)
    void chord(const BoxF& box, double start, double end, const std::optional<Ink>& fill = std::nullopt,
               const std::optional<Ink>& outline = std::nullopt, int width = 1);
    // text(xy, text, fill=None, font, anchor=None, spacing=4, align="left", stroke_width=0, stroke_fill=None):
    // ImageText's lines ("\n" between them, `spacing` px apart past the font's "A"), each drawn at int(x), int(y) with
    // the fraction of its position handed to the font, the stroke (its outer border, filled) under the letters in
    // stroke_fill. Only for images Pillow draws text on with an "L" mask (L, LA, RGB, RGBA, …: not 1, P, I, F).
    void text(PointD xy, std::u32string_view text, const TrueTypeFont& font, const std::optional<Ink>& fill = std::nullopt,
              std::string_view anchor = {}, int stroke_width = 0, const std::optional<Ink>& stroke_fill = std::nullopt);
    // PC5 G-b: glyphs by id (render/text/shaping) from `origin` on their baseline, as text() draws letters: at int(x),
    // int(y) with the fraction handed to the font, the stroke under them.
    void glyphs(PointD origin, std::span<const GlyphPlace> glyphs, const TrueTypeFont& font, const std::optional<Ink>& fill = std::nullopt,
                const GlyphShape& shape = {}, int stroke_width = 0, const std::optional<Ink>& stroke_fill = std::nullopt);

private:
    friend class PageCanvas;
    struct Target;
    explicit Draw(std::unique_ptr<Target> target);

    void draw_lines(std::span<const PointD> xy, int ink, int width);

    std::unique_ptr<Target> target_;
};

// Drawing in page coordinates onto `part`, an image holding the box `area` of a page of size `page` (a render
// region). The shapes come out exactly as on the whole page (the coordinates are not moved): the drawing is done on
// a page-wide band whose rows outside the area are thrown away. commit() puts the result into `part`.
class PageCanvas {
public:
    PageCanvas(Image& part, const Box& area, Size page, std::string_view mode = {});
    ~PageCanvas();
    PageCanvas(const PageCanvas&) = delete;
    PageCanvas& operator=(const PageCanvas&) = delete;

    Draw& draw() { return *draw_; }
    void commit();

private:
    Image& part_;
    Box area_;
    Size page_;
    Image band_;  // page.width × area.height, when the area is narrower than the page
    std::unique_ptr<Draw> draw_;
    bool committed_ = false;
};

// maintenance.doctor's check of the bundled font (render._DELA): the fonts are in the program's resources here, so whether
// the one named ("sfx": Dela Gothic One) reads.
bool bundled_font_present(const std::string& name);

// PC5 G-0 (type@1, render/text/shaping): the bytes of a font as TrueTypeFonts opens it — a bundled font's by its name (read
// once per process and shared), a font file's by its path (read now: NotYetPorted "default_font" or "large_font" as
// TrueTypeFonts::truetype says).
std::shared_ptr<const QByteArray> font_bytes(const std::string& font);
// PC5 G-b: the name TrueTypeFonts::load_default opens Pillow's own default font by (font_bytes and truetype take it).
const std::string& pillow_default_font();

}  // namespace genko::render
