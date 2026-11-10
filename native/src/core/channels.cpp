#include "core/channels.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

[[noreturn]] void bad(const std::string& why) { throw Error("value", "the page's channels cannot be read: " + why); }

std::size_t code_points(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

std::optional<double> number(const Json& v) {
    if (!v.is_number()) return std::nullopt;
    const double d = v.get<double>();
    if (!std::isfinite(d)) return std::nullopt;
    return d;
}

bool is_ref(const std::string& text) {
    if (text.size() != 71 || text.compare(0, 7, "sha256:") != 0) return false;
    return std::all_of(text.begin() + 7, text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::array<int, 3> colour_of(const Json& v) {
    if (!v.is_array() || v.size() != 3) bad("a colour is [r, g, b]");
    std::array<int, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!v[i].is_number_integer()) bad("a colour is three whole numbers");
        const std::int64_t c = v[i].get<std::int64_t>();
        if (c < 0 || c > 255) bad("a colour's values are 0..255");
        out[i] = static_cast<int>(c);
    }
    return out;
}

double unit_of(const Json& v, const char* what) {
    const auto d = number(v);
    if (!d || *d < 0 || *d > 1) bad(std::string(what) + " is a number 0..1");
    return *d;
}

constexpr std::array<std::string_view, 7> kAlphaKeys{"id", "name", "kind", "raster", "color", "opacity", "indicates"};
constexpr std::array<std::string_view, 6> kSpotKeys{"id", "name", "kind", "raster", "color", "solidity"};

}  // namespace

std::string_view to_string(ChannelKind kind) { return kind == ChannelKind::Spot ? "spot" : "alpha"; }

std::optional<ChannelKind> channel_kind_from(std::string_view text) {
    if (text == "alpha") return ChannelKind::Alpha;
    if (text == "spot") return ChannelKind::Spot;
    return std::nullopt;
}

Json to_json(const Channel& channel, const std::string& raster_ref) {
    Json out = Json::object();
    out["id"] = channel.id;
    out["name"] = channel.name;
    out["kind"] = to_string(channel.kind);
    out["raster"] = raster_ref.empty() ? Json(nullptr) : Json(raster_ref);
    out["color"] = Json::array({channel.color[0], channel.color[1], channel.color[2]});
    if (channel.kind == ChannelKind::Alpha) {
        out["opacity"] = channel.opacity;
        out["indicates"] = channel.indicates;
    } else {
        out["solidity"] = channel.solidity;
    }
    if (channel.quick_mask) out["quick_mask"] = true;  // (masks@1, PC3: the page's Quick Mask)
    return out;
}

Channel channel_from_json(const Json& value, std::string& raster_ref) {
    if (!value.is_object()) bad("a channel is an object");
    Channel out;
    const Json* kind = get(value, "kind");
    if (kind == nullptr || !kind->is_string() || !channel_kind_from(kind->get<std::string>())) bad("a channel's kind is alpha or spot");
    out.kind = *channel_kind_from(kind->get<std::string>());
    // (the keys of its kind, each once — written in their order, read in any: the journal's snapshots keep their keys
    // sorted —; anything else is not this build's to keep)
    std::set<std::string> keys;
    for (const auto& [key, item] : value.items()) keys.insert(key);
    // (masks@1, PC3: an alpha channel marked the page's Quick Mask)
    if (keys.erase("quick_mask") > 0) {
        if (out.kind != ChannelKind::Alpha || value["quick_mask"] != Json(true)) bad("only an alpha channel is a Quick Mask (quick_mask: true)");
        out.quick_mask = true;
    }
    const std::set<std::string> expected = out.kind == ChannelKind::Alpha ? std::set<std::string>(kAlphaKeys.begin(), kAlphaKeys.end())
                                                                          : std::set<std::string>(kSpotKeys.begin(), kSpotKeys.end());
    if (keys != expected) bad("a channel's keys are not its kind's");
    const Json& id = value["id"];
    if (!id.is_string() || !is_channel_id(id.get<std::string>())) bad("a channel's id is ch_ and 12 hex digits");
    out.id = id.get<std::string>();
    const Json& name = value["name"];
    if (!name.is_string()) bad("a channel's name is text");
    try {
        if (channel_name(name.get<std::string>()) != name.get<std::string>()) bad("a channel's name has spaces around it");
    } catch (const Error& error) {
        if (error.code() != "value") throw;
        bad("a channel's name is 1..64 characters");
    }
    out.name = name.get<std::string>();
    const Json& raster = value["raster"];
    raster_ref.clear();
    if (!raster.is_null()) {
        if (!raster.is_string() || !is_ref(raster.get<std::string>())) bad("a channel's raster is null or an asset ref");
        raster_ref = raster.get<std::string>();
    }
    out.color = colour_of(value["color"]);
    if (out.kind == ChannelKind::Alpha) {
        out.opacity = unit_of(value["opacity"], "an alpha channel's opacity");
        const Json& indicates = value["indicates"];
        if (!indicates.is_string() || (indicates != Json("masked") && indicates != Json("selected"))) bad("indicates is masked or selected");
        out.indicates = indicates.get<std::string>();
    } else {
        out.solidity = unit_of(value["solidity"], "a spot channel's solidity");
    }
    return out;
}

std::vector<Channel> channels_from_json(const Json& value, std::vector<std::string>& raster_refs) {
    if (!value.is_array()) bad("channels is a list");
    if (value.size() > kChannelsMax) bad("a page has at most " + std::to_string(kChannelsMax) + " channels");
    std::vector<Channel> out;
    raster_refs.clear();
    std::set<std::string> ids, names;
    for (const Json& item : value) {
        std::string ref;
        Channel channel = channel_from_json(item, ref);
        if (!ids.insert(channel.id).second) bad("two channels have the id " + channel.id);
        if (!names.insert(channel.name).second) bad("two channels have the name " + py_repr_str(channel.name));
        out.push_back(std::move(channel));
        raster_refs.push_back(std::move(ref));
    }
    return out;
}

std::string new_channel_id() { return "ch_" + new_id(); }

bool is_channel_id(std::string_view text) {
    if (text.size() != 15 || text.substr(0, 3) != "ch_") return false;
    return std::all_of(text.begin() + 3, text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string channel_name(std::string_view name) {
    std::string out = py_strip(name);
    const std::size_t n = code_points(out);
    if (n == 0) throw Error("value", "a channel's name cannot be empty");
    if (n > kChannelNameMax) throw Error("value", "a channel's name is " + std::to_string(kChannelNameMax) + " characters at most");
    return out;
}

int page_depth(const Page& page) {
    if (page.color) return page.color->depth;
    if (page.rgb) return page.rgb->depth;
    return 8;
}

int channel_dpi(const Page& page, ChannelKind kind) {
    if (kind == ChannelKind::Alpha) return kAlphaChannelDpi;
    const double dpi = page.spec.dpi.value();
    return std::isfinite(dpi) && dpi >= 1 && dpi <= 2400 ? static_cast<int>(std::lround(dpi)) : 600;
}

std::size_t channel_index(const Page& page, std::string_view id_or_name) {
    for (std::size_t i = 0; i < page.channels.size(); ++i) {
        if (page.channels[i].id == id_or_name) return i;
    }
    for (std::size_t i = 0; i < page.channels.size(); ++i) {
        if (page.channels[i].name == id_or_name) return i;
    }
    return page.channels.size();
}

const Channel* find_channel(const Page& page, std::string_view id_or_name) {
    const std::size_t i = channel_index(page, id_or_name);
    return i < page.channels.size() ? &page.channels[i] : nullptr;
}

std::string next_channel_name(const Page& page, ChannelKind kind) {
    const std::string base = kind == ChannelKind::Spot ? "特色" : "Alpha";
    for (std::size_t n = 1; n <= page.channels.size() + 1; ++n) {
        const std::string name = base + " " + std::to_string(n);
        if (std::none_of(page.channels.begin(), page.channels.end(), [&](const Channel& c) { return c.name == name; })) return name;
    }
    return base;
}

std::vector<std::string> plate_names(const Page& page) {
    if (page.color) {
        switch (page.color->space) {
            case InkSpace::Cmyk: return {"cyan", "magenta", "yellow", "black"};
            case InkSpace::Lab: return {"lightness", "a", "b"};
            case InkSpace::Gray: return {"gray"};
            case InkSpace::Multi: {  // (a multichannel page's plates by their names, lower case)
                std::vector<std::string> out;
                for (const Plate& plate : page.color->plates) {
                    std::string name = plate.name;
                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    out.push_back(std::move(name));
                }
                return out;
            }
            default: return {};
        }
    }
    return {"red", "green", "blue"};
}

std::vector<std::string> plate_labels(const Page& page) {
    if (page.color) {
        switch (page.color->space) {
            case InkSpace::Cmyk: return {"シアン", "マゼンタ", "イエロー", "ブラック"};
            case InkSpace::Lab: return {"明度", "a", "b"};
            case InkSpace::Gray: return {"グレー"};
            case InkSpace::Multi: {
                std::vector<std::string> out;
                for (const Plate& plate : page.color->plates) out.push_back(plate.name);
                return out;
            }
            default: return {};
        }
    }
    return {"レッド", "グリーン", "ブルー"};
}

std::optional<std::size_t> plate_index(const Page& page, std::string_view name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::vector<std::string> names = plate_names(page), labels = plate_labels(page);
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == lower || labels[i] == name) return i;
    }
    return std::nullopt;
}

bool channel_target_op(std::string_view op) {
    static constexpr std::array<std::string_view, 24> kOps{"add_stroke", "erase", "erase_raster", "fill", "fill_area", "fill_enclosed",
                                                         "flood_fill", "gradient_fill", "filter_raster", "delete_area", "transform_area",
                                                         "smudge", "liquify", "add_shape", "fill_gaps",
                                                         // (PC4 E5: the brushes that change pixels paint a channel or a mask too)
                                                         "dodge", "burn", "sponge", "blur_brush", "sharpen_brush", "smudge_brush", "mixer_brush",
                                                         "background_eraser", "color_replacement"};
    return std::find(kOps.begin(), kOps.end(), op) != kOps.end();
}

bool python_channel_kind(const Json& op) {
    const Json* name = get(op, "op");
    const Json* kind = get(op, "kind");
    return name != nullptr && *name == Json("filter_raster") && kind != nullptr &&
           (*kind == Json("levels") || *kind == Json("curve") || *kind == Json("brightness_contrast"));
}

std::optional<ChannelTarget> channel_target(const Document& doc, const Json& op) {
    if (!op.is_object()) return std::nullopt;
    const Json* name = get(op, "op");
    const Json* channel = get(op, "channel");
    if (name == nullptr || !name->is_string() || channel == nullptr || !channel_target_op(name->get<std::string>())) return std::nullopt;
    if (!channel->is_string()) return std::nullopt;  // (null, a number…: ignored, as Python ignores the key)
    const std::string& value = channel->get_ref<const std::string&>();
    const Page* page = nullptr;
    try {
        page = &doc.page(require_page(doc, op));
    } catch (const std::exception&) {  // (the op's own refusal says it)
        return std::nullopt;
    }
    const bool python = python_channel_kind(op);
    if (value == "composite") {
        if (python) return std::nullopt;  // (Python's: all three)
        return ChannelTarget{ChannelTarget::Kind::Composite, 0};
    }
    const std::size_t index = channel_index(*page, value);
    if (index < page->channels.size() && (!python || page->channels[index].id == value)) return ChannelTarget{ChannelTarget::Kind::Channel, index};
    if (const auto plate = plate_index(*page, value)) {
        const bool ink_page = page->color && (page->color->space == InkSpace::Cmyk || page->color->space == InkSpace::Lab ||
                                              page->color->space == InkSpace::Multi);
        if (python && !ink_page) return std::nullopt;  // (an RGB or grey page's plates there: Python's r, g, b, else all three)
        if (plate_names(*page).size() == 1) return ChannelTarget{ChannelTarget::Kind::Composite, 0};  // (a grey page's one plate)
        return ChannelTarget{ChannelTarget::Kind::Plate, *plate};
    }
    return std::nullopt;
}

bool uses_channels(const Document& doc) {
    return std::any_of(doc.pages.begin(), doc.pages.end(), [](const PagePtr& page) { return !page->channels.empty(); });
}

void validate_channels(const Document& doc) {
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        if (doc.is_deferred(p)) continue;
        const Page& page = *doc.pages[p];
        if (page.channels.empty()) continue;
        const std::string at = "page " + page.index.repr() + ": ";
        if (page.channels.size() > kChannelsMax) throw Error("value", at + "a page has at most " + std::to_string(kChannelsMax) + " channels");
        std::set<std::string> ids, names;
        const int depth = page_depth(page);
        for (const Channel& channel : page.channels) {
            if (!is_channel_id(channel.id)) throw Error("value", at + "a channel's id is ch_ and 12 hex digits");
            if (!ids.insert(channel.id).second) throw Error("value", at + "two channels have the id " + channel.id);
            try {
                if (channel_name(channel.name) != channel.name) throw Error("value", "spaces");
            } catch (const Error&) {
                throw Error("value", at + "a channel's name is 1..64 characters without spaces around it");
            }
            if (!names.insert(channel.name).second) throw Error("value", at + "two channels have the name " + py_repr_str(channel.name));
            for (const int c : channel.color) {
                if (c < 0 || c > 255) throw Error("value", at + "a channel's colour values are 0..255");
            }
            if (!(channel.opacity >= 0 && channel.opacity <= 1) || !(channel.solidity >= 0 && channel.solidity <= 1)) {
                throw Error("value", at + "a channel's opacity and solidity are 0..1");
            }
            if (channel.indicates != "masked" && channel.indicates != "selected") throw Error("value", at + "indicates is masked or selected");
            if (!channel.raster) continue;
            const InkRasterHeader header = ink_raster_header(*channel.raster);
            if (header.space != InkSpace::Mask) throw Error("value", at + "the channel " + channel.id + "'s raster is not a mask");
            if (header.depth != depth) throw Error("value", at + "the channel " + channel.id + "'s raster is not at the page's depth");
        }
    }
}

}  // namespace genko::core
