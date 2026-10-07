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
// wide line (joint="curve"), so a shape covers the same pixels as in the Python baseline. Text is drawn in M4.

namespace genko::render {

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
