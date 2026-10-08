#pragma once

#include <memory>
#include <array>
#include <utility>
#include <stop_token>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <vector>

#include "render/image.hpp"

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
    // getlength(text): the advance in pixels (a whole number under BASIC).
    double getlength(std::u32string_view text) const;
    // getbbox(text, stroke_width=..., anchor=...): (left, top, right, bottom) from the anchor point. anchor "" is "la".
    std::array<int, 4> getbbox(std::u32string_view text, int stroke_width = 0, std::string_view anchor = {}) const;
    // getmask2(text, mode "L", stroke_width=..., anchor=..., start=(x, y), stroke_filled=...): the coverage and where
    // its top left lies from the anchor point. start is the fraction of the position the text is drawn at (Pillow
    // passes it as a C float); stroke_filled is True when ImageDraw.text asks (the stroke's outer border, filled).
    std::pair<Image, Point> getmask2(std::u32string_view text, int stroke_width = 0, std::string_view anchor = {},
                                     float start_x = 0.0F, float start_y = 0.0F, bool stroke_filled = false) const;

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

    // ImageFont.truetype(font, size) for a size of 1 or more. A file FreeType cannot read is where genko.fonts falls
    // back to Pillow's built-in default font, which this build does not have: NotYetPorted("default_font").
    const TrueTypeFont& truetype(const std::string& font, int size);
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

}  // namespace genko::render
