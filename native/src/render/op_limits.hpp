#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// What the ops that draw may ask for. Python goes on with any size (a lasso a kilometre wide, a blur as wide as a
// house, a line across a continent) until it has no memory or time left; these ops refuse such a request instead,
// with OpError, and the book is left as it was. Every limit is far beyond what a page of a book needs (a B4 page at
// 600 dpi is about 6000 × 8600 pixels).

namespace genko::render::limits {

// A picture an op makes or reads (ops.MAX_IMAGE_PIXELS: the size put_raster refuses too).
inline constexpr std::int64_t kPixels = 120'000'000;
// Its width or height.
inline constexpr std::int64_t kSide = 100'000;
// Steps a drawing walks (dabs along an eraser's path, the points of a line drawn).
inline constexpr std::int64_t kSteps = 10'000'000;
// A blur's or a filter's reach, in pixels (a Gaussian radius, a rank filter's size, a grow or shrink).
inline constexpr double kReach = 2000.0;
// The points of an area, a lasso or a mask stroke.
inline constexpr std::int64_t kPoints = 1'000'000;
// A corner of a picture on the page, in pixels (Pillow's paste and crop take C ints: Python fails past 2**31 too).
inline constexpr double kCoordinate = 2'000'000'000.0;
// The window of a rank (min / max) filter: its work grows with the square of it, in Python as here.
inline constexpr std::int64_t kRankSize = 101;
// The pixels an eraser's dabs cover in all (its path's length × its dab's area).
inline constexpr double kDabWork = 2'000'000'000.0;
// The resolution a layer's pixels are taken to have (its width over the page's), in dots per inch.
inline constexpr double kResolution = 1'000'000.0;
// The length of a drop of rain (filters.rain), in pixels.
inline constexpr double kStreak = 100'000.0;

// OpError "<what> is too far off the page" for a corner past kCoordinate.
void check_coordinate(double x, std::string_view what);

// OpError "<what> is too large (<w>×<h> pixels; at most …)" past kPixels or kSide.
void check_picture(std::int64_t width, std::int64_t height, std::string_view what);
// OpError "<what> is too large (<n>; at most <limit>)".
void check_count(double n, double limit, std::string_view what);

// check_picture of sides worked out as doubles: NaN, an infinity or a side past kSide is refused as too large before
// either is made a whole number.
void check_sides(double width, double height, std::string_view what);

// An area made of areas (selops: unions, intersections, subtractions, saved areas): the areas visited for it in all,
// the pixels of the masks made for it, and of those held at once (each a page-sized mask).
inline constexpr std::int64_t kAreaParts = 1000;
inline constexpr double kAreaWork = 10.0 * static_cast<double>(kPixels);
inline constexpr double kAreaHeld = 4.0 * static_cast<double>(kPixels);

}  // namespace genko::render::limits
