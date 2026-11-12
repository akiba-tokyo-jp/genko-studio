#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"
#include "service/book.hpp"
#include "service/komawari_shape.hpp"
#include "service/layout.hpp"

// komawari@1 (コマ割り K1, docs/cpp-migration/SPEC.md LAYOUT-01): コマ割りの案の束. A batch of layout ideas for one page
// — an exact number of panels (or a range), a range of tiers, a preset (会話・標準・アクション・見せゴマ／扉) and knobs
// (斜め・断ち切り・コマ内分割・大ゴマ率・重ね・はみ出し・枠なし) — drawn from a seed, so the same request and seed give the same
// batch on every machine (its own random numbers: xoshiro256** from splitmix64, no std distribution). Or variations of
// one skeleton (an idea, or the page as it is) by a degree. Each idea is a skeleton a name plan takes as it is
// ({"tiers", "insets"?, "panels"}); the ideas too plain, read out of order, too small or too narrow, or alike, are sieved
// out by the measures of komawari_shape. Applying one goes through layout::apply_layout.
//
// The GUI (群 K2) and the MCP tool (群 K3) use this API; it changes no book (apply_idea is the one that does, on `work`).

namespace genko::service::layout_ideas {

using core::Json;

inline constexpr int kDefaultCount = 12;  // 束の数（利用者の決定 2026-11-12）
inline constexpr int kMaxCount = 48;
inline constexpr int kMaxTiers = 6;

enum class Preset { Talk, Standard, Action, Showcase };

// "talk", "standard", "action", "showcase"; 会話, 標準, アクション, 見せゴマ／扉.
std::string_view preset_key(Preset preset);
std::string_view preset_label(Preset preset);
// A preset from its key or its label (会話・標準・アクション・見せゴマ・扉 too).
std::optional<Preset> preset_from(std::string_view text);

// The knobs, each 0..1 (none: the preset's).
struct Knobs {
    std::optional<double> slant;      // 斜め: how often and how far a border slants
    std::optional<double> bleed;      // 断ち切り
    std::optional<double> split;      // コマ内分割: a column stacked into rows
    std::optional<double> big;        // 大ゴマ率: how much larger the 大ゴマ is
    std::optional<double> overlap;    // 重ね: a panel set over another (差し込み・重ね)
    std::optional<double> overflow;   // はみ出し: the art runs past the border
    std::optional<double> frameless;  // 枠なし
};

// What a request that leaves them out gets from a preset: its numbers of panels and of tiers, and every knob (群 K2:
// the window starts its knobs there).
struct PresetDefaults {
    int min_panels = 0, max_panels = 0, min_tiers = 0, max_tiers = 0;
    Knobs knobs;  // every one set
};
PresetDefaults preset_defaults(Preset preset);

struct Request {
    double frame_w_mm = 180;  // the page's basic frame (komawari::frame_size)
    double frame_h_mm = 270;
    int panels = 0;           // exactly this many (1..kMaxPanels); 0: between min_panels and max_panels
    int min_panels = 0;       // 0: the preset's
    int max_panels = 0;
    int min_tiers = 0;        // 0: the preset's
    int max_tiers = 0;
    Preset preset = Preset::Standard;
    Knobs knobs;
    std::string seed;         // empty: a new one (the batch says which)
    int count = kDefaultCount;
    std::optional<Json> base;  // 変異の元: a skeleton (an idea's, or skeleton_of_page's)
    double variation = 0.3;   // 変異度 0..1 (with base)
    int focus = 0;            // the panel (1-based, in reading order) to make the 見せ場; 0: none
};

struct Idea {
    Json skeleton;            // {"tiers", "insets"?, "panels"}: the slots p1… in reading order
    komawari::Shape shape;
    std::string summary;      // komawari::describe
};

struct Batch {
    std::vector<Idea> ideas;
    std::string seed;         // the same request with this seed gives this batch again
    int tried = 0;            // ideas drawn
    std::vector<std::pair<std::string, int>> dropped;  // sieved out, by why: plain, order, small, gap, focus, similar, same_as_base, unfit
};

// The request an agent or the window sends ({"panels", "min_panels", "max_panels", "min_tiers", "max_tiers", "preset",
// "knobs": {…}, "seed", "count", "base", "variation", "focus"}; every key may be left out or null), for a page of `spec`.
// layout::LayoutError(path, message) for what is not allowed.
Request request_from_json(const Json& args, const core::PageSpec& spec);
Json request_to_json(const Request& request);

// The batch (never more than request.count ideas; fewer when the sieve leaves fewer — Batch::dropped says why: plain,
// flat, order, small, gap, focus, similar, same_as_base, unfit). A request made by hand is kept in its bounds (knobs and
// the degree in 0..1, NaN: the preset's / 0.3). layout::LayoutError for a request or base it cannot draw from (a base
// that cannot be read: "/base…", in Japanese).
Batch generate(const Request& request);
// {"seed", "ideas": [{"skeleton", "summary", "panels", "pattern", "largest", "reading_order"}], "tried", "dropped"}
Json batch_to_json(const Batch& batch);

// A seed's 64 bits: "km-" and 16 hex digits (either case) as they are, any other text by its FNV-1a hash.
std::uint64_t seed_value(std::string_view text);

// A name plan with the skeleton's layout put in: its tiers, insets and gaps (the template dropped), and each panel's
// layout keys (bleed, border_mm, overflow; slant cleared — the tiers' tilt_mm say it) set on the plan's panel of that
// slot (added for a slot it has no panel for). The plan's panels of slots the skeleton does not have are left out, their
// slots put in `dropped` when it is given (an element that is not an object is left out too, its JSON text in `dropped`).
Json merge_into_plan(const Json& skeleton, const Json& plan, std::vector<std::string>* dropped = nullptr);

// Apply a skeleton to a page of `work` through layout::apply_layout: the page must be blank, or `replace` clears it
// first (layout::clear_page). Only who may approve (a person: core::can_approve) changes a page whose name is approved.
// The skeleton is checked (layout::check_tiers) before the page is touched and laid on a copy: on a LayoutError (path
// "/page" or "/skeleton…", in Japanese) `work` is as it was.
layout::CompiledLayout apply_idea(Work& work, const Json& page_index, const Json& skeleton, const std::string& agent, bool replace);

}  // namespace genko::service::layout_ideas
