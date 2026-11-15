#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "core/model.hpp"

// fukidashi@1 (SPEC.md BALLOON-01, docs/cpp-migration/schema-v4.md「フキダシ」): the balloons drawn clean. Since 2026-11-13
// every balloon is drawn by one renderer (render/text/balloon_clean) — true ellipses, even lines, tails that run out
// of the outline — and the hand-drawn look (the egg-shaped oval, the pen's swelling line, the wobble) is gone, in
// books of before too. What this header holds:
//
//   - the feature: the book names fukidashi@1 while a line (or a type layer's record) uses what a build of before
//     cannot draw at all — the kinds capsule and polygon, the style keys corner_mm, squircle, sides, wave_mm,
//     spike_alt and dash_mm, a tail of kind inward or with fillet_mm;
//   - the letters' box: the letters of an oval are set in the box they always were — the balloon's box made
//     1 / 1.035 as big about its middle — in the oval they always were (render/text/lettering). The true ellipse of
//     the balloon's box holds that oval;
//   - the geometry's generation (project.json's "balloon_geometry", a plain key a build of before keeps as it is):
//     a book without it was lettered for the hand-drawn oval, whose box was the letters' own. Read, each such oval's
//     box is made 1.035 times as big about its middle — its letters' box is then the box it had, to the last bit, so
//     its letters are set exactly as they were — and what it holds from its box's corner (the balloon eraser's cuts,
//     the path its letters follow) is moved to stay where it was on the page; a line with border_mm 0, which drew a
//     hairline before and draws none now, is given the hairline (kHairlineMm). Once, in memory; the book says 2 from
//     its next save.

//
// fukidashi@2 (SPEC.md BALLOON-02, schema-v4.md「フキダシの追加」): four additions to the clean balloons, each of them
// keys a line may or may not carry — a line without them is drawn, measured and saved exactly as before:
//
//   - the pen's line (style pen_swell, pen_angle, pen_taper, pen_strokes): an outline whose width swells on one side
//     and thins toward the ends of its strokes (強弱・入り抜き);
//   - drips (style drip_mm, drips, drip_width_mm, drip_seed): the balloon's body takes the box less its foot
//     (body_box), and drips run down from it to the box's foot — arranged by the seed the line itself holds, never by
//     chance;
//   - the symbol kinds heart, star (style points: 4 is the four-point sparkle), flower and diamond;
//   - a tone, a tone that grows darker, or a blend of two colours inside any outline (style fill_tone, fill_gradient).
//
// A build that knows fukidashi@1 alone would open such a book for writing and draw an oval or an even line: the book
// names fukidashi@2 while a line (or a type layer's record) uses one of these kinds or keys, and that build opens it
// read-only. fukidashi@1 is named for exactly what it was.

namespace genko::core {

inline constexpr std::string_view kFukidashiFeature = "fukidashi@1";
inline constexpr std::string_view kFukidashi2Feature = "fukidashi@2";

// The generation of the balloons' geometry this build draws (1: the hand-drawn ovals of before, never written).
inline constexpr std::int64_t kBalloonGeometry = 2;
// How much bigger than the letters' box an oval's own box is: the true ellipse of that box holds the oval the letters
// are kept in (the superellipse of power 2.6 at 0.955 of the letters' box reaches 2^(1/2 − 1/2.6) × 0.955 = 1.0345 of
// the ellipse's diagonal).
inline constexpr double kOvalGrowth = 1.035;
// The share of an oval's own box the letters' oval takes (0.955 of the letters' box): what a balloon sized for its
// words divides the letters' oval by (render::text::hug_keep).
inline constexpr double kOvalRoom = 0.955 / kOvalGrowth;
// The border a line of a book of before is given for its border_mm 0: the thinnest line, one pixel at any resolution
// (up to 3800 dpi) — what 0 drew before fukidashi@1, where it now means no line.
inline constexpr double kHairlineMm = 0.01;

// Whether a line's balloon is one of the ovals (speech, thought, whisper).
bool is_oval(const StoryLine& line);
// Whether a line's letters are kept in the letters' oval — an oval kind, not hand: false, no wobble, set in its
// balloon (not a point or box text) —, in a box of their own inside the balloon's (letter_box) unless the balloon is
// drawn by hand (a path: its box is its outline's bounds, the letters' box with it).
bool letters_in_oval(const StoryLine& line);
// Whether a line of a book of before was drawn as the hand-drawn oval, whose box grows when it is read:
// letters_in_oval, and no hand-drawn outline.
bool legacy_oval(const StoryLine& line);
// The box a line's letters are set in and the checks measure (x, y, w, h mm): the body's (body_box: the line's own
// unless it drips), but for an oval of legacy_oval's kind 1 / kOvalGrowth of it about its middle (to nine decimals of a mm: for an oval grown from a book
// of before, the box it had there, exactly).
std::array<double, 4> letter_box(const StoryLine& line);
// A line of a book of before read (migrate_legacy_line): a hand-drawn oval's box made kOvalGrowth times as big about
// its middle (to nine decimals of a mm), its cuts and its letters' path moved by as much as its corner moved the
// other way; a border_mm of 0 made kHairlineMm. True when the line changed.
bool migrate_legacy_line(StoryLine& line);

// fukidashi@2 — whether a kind is one of the symbols (heart, star, flower, diamond).
bool is_symbol(std::string_view kind);
// A star's points or a flower's petals: style.points (4..12), or the kind's own — a star 5, a flower 6.
int symbol_points(const StoryLine& line);
// How far in a star's valleys lie, as a share of its points' reach: 1 − style.spike_depth (0.05..0.6; a star's own 0.38).
double star_inner(const StoryLine& line);
// How far a line's drips run below its balloon's body (mm): style.drip_mm (0..30), at most 0.6 of the box's height; 0
// for a line without it, for a balloon drawn by hand (its box is its outline's bounds) and for the kinds that have no
// body to drip from (dotted_box, flash, picture, sfx, none).
double drip_reach(const StoryLine& line);
// The box a line's balloon body is drawn in (x, y, w, h mm): the line's own, its foot raised by drip_reach (to nine
// decimals of a mm). A line without drips: its own box, the very numbers.
std::array<double, 4> body_box(const StoryLine& line);

// Whether a line uses what names the feature.
bool uses_fukidashi(const StoryLine& line);
// Whether a line or a type layer's record of a page the book writes does.
bool uses_fukidashi(const Document& doc);
// The same for fukidashi@2: a symbol kind, or one of the style keys pen_swell, pen_angle, pen_taper, pen_strokes,
// drip_mm, drips, drip_width_mm, drip_seed, points, fill_tone, fill_gradient (by the key's presence, as fukidashi@1).
bool uses_fukidashi2(const StoryLine& line);
bool uses_fukidashi2(const Document& doc);
// Whether the book says its balloons' generation: it has an oval, or a line whose border_mm is 0 (a line's or a type
// layer's record's) — what a book of before is read otherwise for.
bool says_balloon_geometry(const Document& doc);

}  // namespace genko::core
