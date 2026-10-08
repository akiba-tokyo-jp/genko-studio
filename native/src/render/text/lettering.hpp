#pragma once

#include <array>
#include <cstdint>
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
// speaker's name painted on the page. The balloons themselves (shapes, tails, joined outlines) are a later step.

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
// text_image(line, dpi, font_path): the lettering and its em.
std::pair<Image, std::int64_t> text_image(const core::StoryLine& line, int dpi,
                                          const std::optional<std::string>& font_path = std::nullopt, std::stop_token stop = {});

// _paint_text(image, line, dpi, show_speaker, font_path): the line's letters on the page (from its box's middle, or
// its corner for "none"; along its path when it has one) and the speaker's name above the box.
void paint_text(Image& image, const core::StoryLine& line, int dpi, bool show_speaker,
                const std::optional<std::string>& font_path = std::nullopt, std::stop_token stop = {});
// path_text(image, line, dpi, font_path): 文字をパスに沿わせる — each letter stood on the path, turned with it.
void path_text(Image& image, const core::StoryLine& line, int dpi, const std::optional<std::string>& font_path = std::nullopt,
               std::stop_token stop = {});

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
