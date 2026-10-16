#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// channels@1 (docs/cpp-migration/schema-v4.md「チャンネル」, SPEC.md COLOR-03): a page's alpha and spot channels
// (core::Channel in core/model.hpp) — their JSON, their checks and the names the channel ops give. A channel's pixels are
// a GKIR Mask raster (core/ink.hpp: one plane of density, no alpha) at the page's depth, stretched over the page; an
// alpha channel is made at the selection's 200 dpi, a spot channel at the book's dpi. Which channel is targeted and
// which are shown is the window's (never saved).
//
// The channel ops are the drawing build's (render/ops_channels.cpp): here are the model's rules only.

namespace genko::core {

inline constexpr std::string_view kChannelsFeature = "channels@1";
inline constexpr std::size_t kChannelsMax = 50;
inline constexpr std::size_t kChannelNameMax = 64;  // characters
inline constexpr int kAlphaChannelDpi = 200;       // (selops.SEL_DPI)

std::string_view to_string(ChannelKind kind);
std::optional<ChannelKind> channel_kind_from(std::string_view text);

// {"id", "name", "kind", "raster", "color", "opacity", "indicates"} (alpha) or {"id", "name", "kind", "raster",
// "color", "solidity"} (spot), in that order; `raster` the raster's asset ref ("" for none: null).
Json to_json(const Channel& channel, const std::string& raster_ref);
// A channel as the writer writes it, checked (core::Error("value") says what is wrong); its raster is not read: the ref
// is put in `raster_ref` ("" for null).
Channel channel_from_json(const Json& value, std::string& raster_ref);
// The channels of a page as the writer writes them (a list of at most kChannelsMax), checked; their refs in order.
std::vector<Channel> channels_from_json(const Json& value, std::vector<std::string>& raster_refs);

// A new channel id: "ch_" and 12 hex digits (core::new_id()).
std::string new_channel_id();
// Is the text a channel id ("ch_" and 12 lowercase hex digits)?
bool is_channel_id(std::string_view text);
// The name trimmed of the spaces around it; core::Error("value") for one empty or longer than kChannelNameMax.
std::string channel_name(std::string_view name);

// The depth of a page's pixels, which its channels share: its colour mode's, its RGB mode's, else 8.
int page_depth(const Page& page);
// The resolution a new channel of this kind is made at on the page: 200 dpi for an alpha channel, the page's own for a
// spot channel (its spec's dpi; 600 when it has none).
int channel_dpi(const Page& page, ChannelKind kind);

// The channel a value names on the page: its id, else its name (null for none).
const Channel* find_channel(const Page& page, std::string_view id_or_name);
std::size_t channel_index(const Page& page, std::string_view id_or_name);  // (page.channels.size() for none)

// "Alpha n" / "特色 n": the first n from 1 whose name the page has no channel of (PhotoCraft's next_alpha_name).
std::string next_channel_name(const Page& page, ChannelKind kind);

// The page's colour planes as the channel ops name them (lower case, PhotoCraft's): red, green, blue (RGB); cyan,
// magenta, yellow, black (CMYK); lightness, a, b (Lab); gray (grey) — and their names in the panel (レッド, …).
std::vector<std::string> plate_names(const Page& page);
std::vector<std::string> plate_labels(const Page& page);
// The plate a value names (its name in any case, or its label), or none.
std::optional<std::size_t> plate_index(const Page& page, std::string_view name);

// The pixel ops that take this build's own "channel" (the channel they work on instead of the layer's colour planes:
// render/ops_channels.cpp): the pen and the eraser, the fills and the gradient, the filters and corrections, an area
// deleted or moved, smudge, liquify, shapes, closing gaps.
bool channel_target_op(std::string_view op);
// filter_raster's kinds that read a "channel" of their own (Python's filters.py: levels, curve and brightness_contrast —
// r, g or b, anything else all three).
bool python_channel_kind(const Json& op);

// What a pixel op's "channel" names on its page as this build reads it: an alpha or spot channel (its index in the
// page's list), a colour plate of the page (its index), or the composite (nothing to do). None when the key is not this
// build's there — the op keeps Python's way with it —: no key, or null; a value that names nothing on the page (not a
// string, no channel of that id or name, no plate of that name), which Python ignores as a key it does not know (and
// says so); and on filter_raster's levels, curve and brightness_contrast, whose "channel" is Python's (r, g, b, else all
// three: "red", a channel's name or "composite" too), anything but an alpha or spot channel's id, or a plate of a CMYK or
// Lab page (pages Python does not have). None too when the op names no page the book has (the op says so itself).
struct ChannelTarget {
    enum class Kind { Channel, Plate, Composite };
    Kind kind = Kind::Composite;
    std::size_t index = 0;
};
std::optional<ChannelTarget> channel_target(const Document& doc, const Json& op);

// Does the book need channels@1 (a page with a channel)?
bool uses_channels(const Document& doc);
// The book's channels as they must be (core::Error("value") says what is not): at most kChannelsMax on a page, ids and
// names well formed and one each on the page, values in range, rasters GKIR Mask at the page's depth. Pages not read
// yet are not looked at.
void validate_channels(const Document& doc);

}  // namespace genko::core
