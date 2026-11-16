#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// The figure's pose library (SPEC FIGURE-02, this build's own): a few hundred original poses for the 3D figure
// (`add_figure`), each authored as joint angles from a written description of the action — nothing taken from any
// other gallery. The file is data/figure_poses.json, embedded byte for byte (genko.poses@1):
//
//   {"format": "genko.poses@1",
//    "categories": [{"id", "ja", "en"}, …],
//    "poses": [{"id": "<category>/<name>", "category", "ja", "en", "tags": [Japanese and English words],
//               "joints": {joint: {x, y, z}} (as add_figure's), "hands": {l, r: pose | {pose, curls, spread, thumb}},
//               "hold": {l, r: prop}? (what the hand holds: drawn from G5 on), "support": floor | chair | none,
//               "seat_h": u? (chair: the seat's height above the floor, in the figure's unit u = height / heads),
//               "contact": [l_foot, r_foot, l_knee, r_knee, l_hand, r_hand, seat, pelvis, back, front, side],
//               "symmetric": bool}, …]}
//
// A mirrored pose is a toggle, not an entry of its own: its id is the pose's with "~m" (left and right swapped, each
// joint (x, −y, −z): the mirror image of the body, exactly).
//
// A pair pose (G5: couple, two_people) holds two people in one entry, in place of joints … contact:
//
//   {"id", "category", "ja", "en", "tags",
//    "figures": [{"joints", "hands", "hold"?, "support", "seat_h"?, "contact"},          (a: the one the op is given)
//                {"joints", "hands", "hold"?, "support", "seat_h"?, "contact",           (b: placed from a)
//                 "offset": [x, y, z] (b's pelvis from a's, in a's own frame, in a's unit u), "turn": y (b's turn from a's)}],
//    "touch": [{"a": part, "b": part}, …] (where they touch each other: l_hand … r_foot, torso, head),
//    "symmetric": false}
//
// A contact "partner" says the person leans on or is held up by the other. "<id>#a" and "<id>#b" name one person of it
// as a pose of its own (what a figure of the pair keeps as its pose_id); "<id>~m#a" the mirror's. The mirror of a pair is
// each person mirrored and b's offset x and turn the other way (the roles stay).

namespace genko::core {
struct Document;
}

namespace genko::core::pose_library {

inline constexpr std::string_view kFormat = "genko.poses@1";
inline constexpr std::string_view kMirror = "~m";  // (the suffix of a mirrored pose's id)

// The file's bytes as embedded, and the file read (once).
std::string_view text();
const Json& library();

struct Category {
    std::string id;
    std::string ja;
    std::string en;
    std::size_t count = 0;  // (its poses)
};
std::vector<Category> categories();

// A pose of the file by its id (without "~m"); null when there is none.
const Json* find(std::string_view id);

// The pose an id names — "<id>" or "<id>~m" —, mirrored when the id says so or `mirror` is set (both: not mirrored),
// its "id" the id it is kept under on a figure ("…~m" when mirrored). Nothing for an id the file does not have. A pair's
// id gives the pair; "<id>[~m]#a" or "#b" one person of it (a pose like any other, its id kept so, its category the
// pair's).
std::optional<Json> resolve(std::string_view id, bool mirror = false);

// Whether a pose (as the file or resolve has it) is a pair; one person of a pair (role "a" or "b") as a pose.
bool is_pair(const Json& pose);
Json member(const Json& pair, std::string_view role);
// The pair kinds' contacts and touching parts.
inline constexpr std::string_view kPartner = "partner";
inline constexpr std::string_view kTouchParts[] = {"l_hand", "r_hand", "l_fore", "r_fore", "l_upper", "r_upper", "l_thigh", "r_thigh",
                                                   "l_shin", "r_shin", "l_foot", "r_foot", "torso", "head"};
// An id with its mirror turned over: "<id>" ↔ "<id>~m", before a pair role ("#a", "#b") when it has one.
std::string mirror_id(std::string_view id);

// The mirror image of a pose (or of a figure's joints, hands and hold, the other keys kept): l_ and r_ swapped, each
// joint (x, −y, −z), the hands and what they hold swapped, the contacts' sides swapped. Its own inverse.
Json mirrored(const Json& pose);
// {joint: {x, y, z}} mirrored as above.
Json mirrored_joints(const Json& joints);

// The poses that match (all the conditions given; empty: any), in the file's order. `query` is matched, without
// regard to ASCII case, against the id, the Japanese and English names and the tags (words split by spaces, ASCII or
// 全角: each must match); `tags` must each be a tag of the pose; `contact`, each a contact of it; `props` true: only the
// poses whose hands hold something, false: only those that hold nothing.
struct Filter {
    std::string category;
    std::vector<std::string> tags;
    std::string query;
    std::string support;
    std::vector<std::string> contact;
    std::optional<bool> props;
};
std::vector<const Json*> list(const Filter& filter = {});

// What the gallery and the figure_poses tool say of a pose: {id, ja, en, category, tags, hands {l, r: the hand's shape's
// name}, contact, support, props [what the hands hold, l then r], symmetric}; a pair's: the same of its first person
// (contact and props: both people's), and "pair": true with "figures": [{role, hands, contact, support, props}, …].
// (A pair matches a filter's support and contact when one of its people does, props when one holds something.)
Json summary(const Json& pose);

// A random pick for the gallery and the tool (never inside an op): `n` poses of `from` (all of them when fewer), each
// once, in the order drawn, by splitmix64 from `seed` — the same seed, the same pick. A seed is written "fp-" and 16 hex
// digits (seed_of reads one: nothing for any other text; seed_text writes one).
std::vector<const Json*> sample(const std::vector<const Json*>& from, std::size_t n, std::uint64_t seed);
std::optional<std::uint64_t> seed_of(std::string_view text);
std::string seed_text(std::uint64_t seed);

// What a hand may hold (the props G5 draws in the hand: until then the key is kept and nothing is drawn).
inline constexpr std::string_view kProps[] = {"cup", "phone", "paper", "bag", "umbrella", "book", "pen", "sword", "bat", "ball"};
bool is_prop(std::string_view name);

// figures@1: the book's prims say something a build of before would not keep or draw — a figure's `pose_id`, `hold`,
// `props` or `pair`, a hand's `spread` or `thumb` (a figure's hands, or a hand on its own). Named while one does (books without them are
// saved as before, byte for byte).
inline constexpr std::string_view kFiguresFeature = "figures@1";
bool uses_figures_feature(const Json& prim);
bool uses_figures_feature(const Document& doc);

}  // namespace genko::core::pose_library
