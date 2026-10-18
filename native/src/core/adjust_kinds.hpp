#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "core/adjust.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// PC2 (補正・色管理, SPEC.md COLOR-04): the table of every correction kind — Python's (filters.ADJUSTMENTS and exposure)
// with the keys its `native` may hold (design 2.2), and the C++ build's own (design 2.3, 3.2) with their settings —:
// each kind's settings in their stored order, their defaults and ranges, and the group of PC2 that writes its formula.
// One table (core/adjust_kinds.cpp) for the ops, the stored form (adjustments@1), the three ways that draw it, the
// windows (app/adjust_forms) and the PSD's adjustment layers (formats/psd_adjust, render/psd_adjust).
//
// A kind's formula is its group's: each group's own file fills the hooks of its kinds (core/adjust_tone.cpp: PC2a,
// core/adjust_color.cpp: PC2b; PC2c's local corrections work a layer's pixels: render/tone_ops.cpp). A kind whose hooks
// are not filled yet is in the table (its name, settings and group) and refused where it would be used: not_yet_ported
// naming it.

namespace genko::core {

// One setting of a correction kind: its key, how it is read, its default and range. Read as PhotoCraft reads its fields
// (engine/adjust_params.rs: Reader): a number out of its range is clamped, a value of the wrong type or not finite is
// refused (an op's OpError; stored: the book opens read-only).
struct AdjustParam {
    enum class Type {
        Number,   // a finite number, clamped to lo..hi
        Whole,    // a whole number (rounded), clamped to lo..hi
        Bool,     // true or false (fallback 0 / 1)
        Choice,   // one of `choices` (the default: `text`, else the first)
        Rgb,      // [r, g, b], whole numbers 0..255 (the default: `numbers`)
        Numbers,  // `numbers.size()` numbers, each clamped to lo..hi (the default: `numbers`)
        Group,    // an object of the settings `group`
        Text,     // a string of 1..255 characters
        Custom,   // read by the kind's own normalize (kept as given by the table's reading)
    };
    std::string_view key;
    Type type = Type::Number;
    double fallback = 0;
    double lo = 0, hi = 0;
    std::vector<std::string_view> choices;
    std::string_view text;
    std::vector<double> numbers;
    std::vector<AdjustParam> group;
    // Not filled in when not given (a key whose absence means something: every `native` key, photo_filter's preset,
    // color_lookup's look or table).
    bool optional = false;
};

// How a correction's function is made, and how its settings are read beyond the table's own reading.
struct AdjustHooks {
    // The settings as the table read them (an op's: clamped, defaults filled) → their normal form (what depends on
    // several of them: PhotoCraft's canonical_bounds, a preset's colour, a channel record's white past its black…).
    // OpError for what it refuses. None: the table's reading is the normal form. `made`: where a lookup table it makes
    // of a file's text goes (none: it cannot make one).
    std::function<Json(Json read, const Page* page, LookupTables* made)> normalize;
    // The correction on a page: `adjust` its normal form with its "kind" (a Python kind: the whole adjust, its native
    // included). `luts`: the lookup tables it may use (none: none).
    std::function<AdjustPtr(const Json& adjust, const Page& page, const LookupTables* luts)> function;
};

struct AdjustKind {
    std::string_view name;
    std::string_view group;  // "Python" (Python's own kinds), "PC2a", "PC2b" or "PC2c": who writes its formula
    std::string_view native_group;  // a Python kind's native that changes its formula: who writes that (PC2a)
    bool layer = false;      // a correction layer's kind (add_layer kind=adjust, set_layer adjust)
    bool filter = false;     // a kind of filter_raster
    bool local = false;      // it looks beyond each pixel (no AdjustFunction: the op does it, render/ops_adjust)
    // A filter kind a smart object can hold as a smart filter (smart-objects@1, PC3 A2: core::smart::filter_kind): what it
    // makes depends on its settings and the layer's own pixels alone (not another layer's, nor a selection's statistics).
    bool smart = false;
    std::vector<AdjustParam> params;  // a kind of the C++ build's own: its settings, in their stored order
    std::vector<AdjustParam> native;  // the keys its `native` may hold, in their stored order (blend: every kind's)
    // A Python kind: its native settings that change its formula (design 2.2), or a kind of the C++ build's own: the
    // kind itself. Empty until the group fills it (not_yet_ported).
    AdjustHooks hooks;
};

// Every kind, Python's first (in filters.ADJUSTMENTS's order), then the C++ build's own by group.
const std::vector<AdjustKind>& adjust_kinds();
const AdjustKind* adjust_kind(std::string_view name);
// Is this one of Python's correction kinds (filters.ADJUSTMENTS and exposure)?
bool python_adjust_kind(std::string_view kind);

// Each group's file fills the hooks of its kinds (by name; a kind it does not know is left as it is).
using HookTable = std::vector<std::pair<std::string_view, AdjustHooks>>;
HookTable tone_hooks();   // core/adjust_tone.cpp (PC2a): levels, curve, brightness_contrast, posterize, gradient_map, color_lookup, equalize
HookTable color_hooks();  // core/adjust_color.cpp (PC2b): hue_saturation … selective_color, desaturate

// How a setting list is read. Op: an op's settings — clamped to their ranges, the defaults of the non-optional ones
// filled in (an op's normal form). Stored: a book's — out of range refused, nothing filled in.
enum class ReadMode { Op, Stored };
// The settings of `params` read from `given` (an object: OpError otherwise) in their stored order; OpError for a key
// it does not know (`where`: what the error names), a value of the wrong type, one not finite or (Stored) out of range.
// Custom ones are kept as given.
Json read_params(const std::vector<AdjustParam>& params, const Json& given, ReadMode mode, std::string_view where);

// levels' and a tone curve's channel records (their native.channels, design 2.2): the channels of a space ("rgb": red,
// green, blue; "cmyk": cyan, magenta, yellow, black; "lab": lightness, a, b; "gray": gray), the space a page's records
// are in by default (CMYK cmyk, Lab lab, grey gray, else rgb), and the records read — levels' each {black 0..253,
// white 2..255 (at least black + 2), gamma 0.01..9.99, out_black, out_white 0..255} (Python's levels_table's
// settings), a curve's each 2..19 points [in, out] in 0..255 (sorted by in; two within half a level are one: the
// first), as PhotoCraft reads them (engine/adjust_params.rs: levels, parse_curve). OpError as read_params.
std::vector<std::string_view> tone_channels(std::string_view space);
std::string_view default_tone_space(const Page* page);
Json read_tone_channels(std::string_view kind, const Json& channels, std::string_view space, ReadMode mode, std::string_view where);

}  // namespace genko::core
