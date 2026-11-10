// PC3 B1 (SPEC.md COLOR-06「クイックマスク」, masks@1): the op quick_mask — Select › Edit in Quick Mask Mode. Entering it
// makes the page's Quick Mask: an alpha channel marked quick_mask (one at most on a page), white where the selection was
// (none: all white, nothing masked), which the window then targets so every pixel tool paints it (the pixel ops'
// "channel", PC1c). Leaving it gives the selection the channel says back and takes the channel away. Both are steps of the
// book's history (the selection itself is the window's: the user's decision 4). PhotoCraft v0.2.0's
// engine/src/channel_cmds.rs (quick_mask_from, selection_from, quick_mask: lines 704-760), MIT
// (native/third_party/photocraft/NOTICE.md); its options (red, 50 %, masked areas) are its QuickMaskOptions' defaults.

#include <algorithm>
#include <cmath>
#include <optional>

#include "core/channels.hpp"
#include "core/command_bus.hpp"
#include "core/masks.hpp"
#include "core/pyconv.hpp"
#include "render/channels.hpp"
#include "render/ops_pc3.hpp"
#include "render/page.hpp"
#include "render/selection.hpp"

namespace genko::render {

namespace {

using core::Json;
using core::OpError;

const Json* field(const Json& op, const char* key) {
    const auto it = op.find(key);
    return it == op.end() || it->is_null() ? nullptr : &*it;
}

// The name a new Quick Mask takes: クイックマスク, else クイックマスク 2, ….
std::string free_name(const core::Page& page) {
    const std::string base = "クイックマスク";
    const auto taken = [&](const std::string& name) {
        return std::any_of(page.channels.begin(), page.channels.end(), [&](const core::Channel& c) { return c.name == name; });
    };
    if (!taken(base)) return base;
    for (std::size_t n = 2;; ++n) {
        const std::string name = base + " " + std::to_string(n);
        if (!taken(name)) return name;
    }
}

void enter(core::OpContext& c, std::size_t at) {
    core::Document& doc = c.doc;
    const Json& op = c.op;
    const core::Page& page = doc.page(at);
    if (core::quick_mask_of(page) != nullptr) throw OpError("page " + page.index.repr() + " is in Quick Mask already");
    if (page.channels.size() >= core::kChannelsMax) throw OpError("a page has at most " + std::to_string(core::kChannelsMax) + " channels");
    core::Channel channel;
    channel.id = core::new_channel_id();
    channel.name = free_name(page);
    channel.kind = core::ChannelKind::Alpha;
    channel.quick_mask = true;
    if (const Json* colour = field(op, "color")) {
        if (!colour->is_array() || colour->size() != 3) throw OpError("color is [r, g, b] (0..255 each)");
        for (std::size_t i = 0; i < 3; ++i) {
            const Json& v = (*colour)[i];
            if (!v.is_number_integer() || v.get<std::int64_t>() < 0 || v.get<std::int64_t>() > 255) throw OpError("color is [r, g, b] (0..255 each)");
            channel.color[i] = static_cast<int>(v.get<std::int64_t>());
        }
    }
    if (const Json* opacity = field(op, "opacity")) {
        if (!opacity->is_number() || opacity->is_boolean() || !std::isfinite(opacity->get<double>()) || opacity->get<double>() < 0 ||
            opacity->get<double>() > 1) {
            throw OpError("opacity is a number 0..1");
        }
        channel.opacity = opacity->get<double>();
    }
    if (const Json* indicates = field(op, "indicates")) {
        if (!indicates->is_string() || (*indicates != Json("masked") && *indicates != Json("selected"))) throw OpError("indicates is masked or selected");
        channel.indicates = indicates->get<std::string>();
    }
    // (white where selected; no selection: all white — nothing masked, as Photoshop)
    const int dpi = core::channel_dpi(page, core::ChannelKind::Alpha);
    channels::Plane plane = channels::blank(page, dpi);
    if (const Json* area = field(op, "area")) {
        const Image mask = selection::to_mask(*area, page, &doc, dpi);
        plane = channels::resized(channels::from_mask(mask), plane.width, plane.height);
    } else {
        std::fill(plane.values.begin(), plane.values.end(), 1.0f);
    }
    channel.raster = channels::encode(plane, core::page_depth(page));
    const std::string id = channel.id;
    doc.edit_page(at).channels.push_back(std::move(channel));
    c.report = Json{{"channel", id}};
}

void leave(core::OpContext& c, std::size_t at) {
    core::Document& doc = c.doc;
    const core::Page& page = doc.page(at);
    const core::Channel* quick = core::quick_mask_of(page);
    if (quick == nullptr) throw OpError("page " + page.index.repr() + " is not in Quick Mask");
    const std::optional<Json> area = quick_mask_selection(page);
    const std::string id = quick->id;
    auto& list = doc.edit_page(at).channels;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const core::Channel& ch) { return ch.id == id; }), list.end());
    c.report = Json{{"area", area ? *area : Json(nullptr)}};
}

}  // namespace

std::optional<Json> quick_mask_selection(const core::Page& page) {
    const core::Channel* quick = core::quick_mask_of(page);
    if (quick == nullptr) return std::nullopt;
    // (none when it is all white — untouched or painted white —, or nothing is selected)
    const channels::Plane plane = channels::plane_of(page, *quick);
    if (std::all_of(plane.values.begin(), plane.values.end(), [](float v) { return v >= 1.0f - 0.5f / 255.0f; })) return std::nullopt;
    const Size size{mm_to_px(page.spec.width_mm.value(), selection::kSelDpi), mm_to_px(page.spec.height_mm.value(), selection::kSelDpi)};
    return selection::from_mask(channels::mask_of(*quick, size), selection::kSelDpi);
}

void quick_mask(core::OpContext& c) {
    const std::size_t at = core::require_page(c.doc, c.op);
    const Json* on = field(c.op, "on");
    if (on == nullptr || !on->is_boolean()) throw OpError("on is required (true: enter the Quick Mask, false: leave it)");
    if (on->get<bool>()) {
        enter(c, at);
    } else {
        for (const char* key : {"area", "color", "opacity", "indicates"}) {
            if (field(c.op, key) != nullptr) throw OpError(std::string(key) + " is for entering the Quick Mask (on: true)");
        }
        leave(c, at);
    }
}

}  // namespace genko::render
