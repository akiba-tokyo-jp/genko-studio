#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "studio/schemas.hpp"

// komawari@1 (コマ割り K1, docs/cpp-migration/SPEC.md LAYOUT-01): the shape of a page's panels — where each is in its
// basic frame, in reading order — from a name plan (its tiers, insets and panels' layout keys) or from a page's own
// frames, and the measures taken of it. One place for them: the batch of layout ideas sorts its ideas out with them
// (layout_ideas), and submit_name warns of a dull layout with the same (monotony).

namespace genko::service::komawari {

using core::Json;

// The thresholds the measures are judged by (one place: the ideas' sieve and the monotony warnings share them).
inline constexpr double kPlainSizeRatio = 1.2;   // the largest panel under this times the smallest: all the same size
inline constexpr double kFlatSizeRatio = 1.5;    // under this (3 panels or more): コマの大きさの差が小さい
inline constexpr double kMinGapMm = 2.0;         // a narrower gap between panels is too narrow
inline constexpr double kBigShare = 0.3;         // a panel of this share of the frame or more is a 大ゴマ
inline constexpr double kRevealSmallShare = 0.3; // a 見せ場 panel under this share (and not the largest) is small
inline constexpr int kSimilarBelow = 3;          // two ideas whose difference is under this are alike (one batch)
inline constexpr int kRepeatAtMost = 1;          // two pages whose difference is at most this are the same layout
inline constexpr int kAccentPages = 4;           // the book-wide check waits for this many pages with panels

// One panel: its box in mm from the basic frame's top left, before the slant of its borders; how much a slanted border
// takes from it at its narrow end (squeeze); and its layout keys.
struct PanelBox {
    std::string slot;
    double x = 0, y = 0, w = 0, h = 0;
    double squeeze_w = 0, squeeze_h = 0;
    int tier = 0;  // the tier it is in (an inset: its host's)
    int col = 0;   // its column in the tier, from the right (an inset: its host's)
    bool slanted = false, bleed = false, over = false, frameless = false, overflow = false;
    std::string host;  // an inset: the panel it goes into

    double area() const { return w * h; }
    double min_side() const;  // the shorter side at the narrow end
};

struct Shape {
    double frame_w = 0, frame_h = 0;  // the basic frame (mm)
    std::vector<PanelBox> panels;     // in reading order
    std::string pattern;              // the tiers' panels, top to bottom: "2-1-3", a stacked column marked "s" ("2s-1"); "free" when drawn
    int tilts = 0;                    // slanted borders
    std::vector<double> gaps;         // the gaps between neighbouring panels (mm)
    bool drawn = false;               // drawn panels (not tiers): measured as they are

    std::size_t count() const { return panels.size(); }
};

// The basic frame (mm) of a book's pages.
std::pair<double, double> frame_size(const core::PageSpec& spec);

// The shape a name plan gives in a basic frame of w × h mm (its tiers or template, gap keys, slants, bleeds, insets
// and border keys). Throws layout::LayoutError when the plan has no tiers to read.
Shape shape_of_plan(const Json& plan, double frame_w_mm, double frame_h_mm);
// The shape of a page's own panels (nothing for a blank page: one panel, no plan).
std::optional<Shape> shape_of_page(const core::Document& doc, const core::Page& page);
// A page's own panels as a skeleton (骨組み: {"tiers", "insets"?, "panels"} with the slots p1… in its reading order,
// each border's gap_mm and tilt_mm where they are not the plan's), when its panels are tiers of cuts (nothing for drawn
// panels).
std::optional<Json> skeleton_of_page(const core::Document& doc, const core::Page& page);

// The plan's slots in the order the page will read them (its tiers', each inset where it is read beside its host).
std::vector<std::string> reading_slots(const Json& plan, double frame_w_mm = 180, double frame_h_mm = 270);

// --- the measures ---------------------------------------------------------------------------------------------------

// The largest panel's area over the smallest's (1 for one panel).
double size_ratio(const Shape& shape);
// All the same size, straight, no bleed, no inset, every border drawn: a plain grid (無難な格子).
bool plain_grid(const Shape& shape);
// 3 panels or more, the largest under kFlatSizeRatio times the smallest: コマの大きさの差が小さい (layout_flat; the
// batch sieves these out too, so an idea never gets the warning).
bool flat(const Shape& shape);
// Each inset meets its host: a quarter of its box or more on the host's (inside it, or across its border).
bool insets_on_hosts(const Shape& shape);
// Where the reading order jumps back (up, or right inside a row): the 1-based index of the panel read after the jump, else 0.
std::size_t reading_break(const Shape& shape);
// The shortest side of any panel (mm) and the narrowest gap (mm; none: a large number).
double smallest_side(const Shape& shape);
double narrowest_gap(const Shape& shape);
// The 1-based index of the largest panel, and its share of the basic frame.
std::size_t largest(const Shape& shape);
double share(const Shape& shape, std::size_t index);
// What two layouts differ in, as a score: the tiers' pattern 3, where the 大ゴマ is 2, slants 2 (1 for how many),
// bleeds 1, insets 2, borders and overflow 1.
int difference(const Shape& a, const Shape& b);
// A short account of a shape: "4コマ・段 2-1-1・大ゴマ p3・斜め 1・断ち切り 1".
std::string describe(const Shape& shape);

// --- 単調さの警告（C） -----------------------------------------------------------------------------------------------

// The warnings a name plan for `page_index` gets for a dull layout (never errors): the same or a like layout three pages
// running (layout_repeats), panels of much the same size (layout_flat), a reveal beat's panel small
// (reveal_beat_small), and no slant nor bleed anywhere in the book's names (book_without_accent).
std::vector<studio::Issue> monotony(const core::Document& doc, const Json& page_index, const Json& plan, const Json& script);
// The same for the book as it is (each page's own panels), each with its page's index as the book has it (null: the
// book's): for review.html. Neither ever throws: what cannot be measured gets no warning.
std::vector<std::pair<Json, studio::Issue>> book_monotony(const core::Document& doc, const Json& script);

}  // namespace genko::service::komawari
