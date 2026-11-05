#include "core/groups.hpp"

#include <algorithm>
#include <map>
#include <string>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace {

[[noreturn]] void bad(const std::string& why) { throw Error("value", why); }

// The folder each layer is in (its position; none: none, or what its parent_id names is not a folder of the page — the
// last layer of that id, as the layer ops look it up).
std::vector<std::optional<std::size_t>> parents_of(const std::vector<Layer>& layers) {
    std::map<std::string, std::size_t, std::less<>> by_id;
    for (std::size_t i = 0; i < layers.size(); ++i) by_id[layers[i].id] = i;
    std::vector<std::optional<std::size_t>> out(layers.size());
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Json& parent = layers[i].parent_id;
        if (!parent.is_string()) continue;
        const auto found = by_id.find(parent.get_ref<const std::string&>());
        if (found != by_id.end() && layers[found->second].kind == LayerKind::Folder) out[i] = found->second;
    }
    return out;
}

}  // namespace

bool is_group(const Layer& layer) { return layer.group && layer.kind == LayerKind::Folder; }

bool has_groups(const std::vector<Layer>& layers) { return std::any_of(layers.begin(), layers.end(), [](const Layer& l) { return is_group(l); }); }

bool uses_groups(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        for (const Layer& layer : written.page->layers)
            if (layer.group) return true;
    }
    return false;
}

void validate_groups(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        const Page& page = *written.page;
        const std::string where =
            written.contents.empty() ? "page " + py_str(page.index.json()) + ": " : "the smart object contents " + written.contents + ": ";
        for (const Layer& layer : page.layers) {
            if (layer.group && layer.kind != LayerKind::Folder) bad(where + "the layer " + layer.id + " is marked a group but is not a folder");
            if (layer.blend == kPassThrough && !is_group(layer)) bad(where + "the blend pass_through is a group's only (layer " + layer.id + ")");
        }
    }
}

bool group_changes_look(const Layer& group) {
    return is_group(group) && (group.blend != kPassThrough || group.opacity < 1.0 ||
                               (group.mask && group.mask->enabled && group.mask->png && !group.mask->png->empty()) ||
                               (group.vector_mask && group.vector_mask->enabled));
}

std::vector<std::optional<std::size_t>> group_owners(const std::vector<Layer>& layers) {
    const std::vector<std::optional<std::size_t>> parents = parents_of(layers);
    std::vector<std::optional<std::size_t>> out(layers.size());
    std::vector<std::size_t> seen(layers.size(), layers.size());  // (the layer whose chain last walked it)
    for (std::size_t i = 0; i < layers.size(); ++i) {
        // (the whole chain walked first: one that comes back on itself is no group's)
        std::optional<std::size_t> nearest;
        bool loops = false;
        seen[i] = i;
        for (auto at = parents[i]; at; at = parents[*at]) {
            if (seen[*at] == i) {
                loops = true;
                break;
            }
            seen[*at] = i;
            if (!nearest && is_group(layers[*at])) nearest = at;
        }
        if (!loops) out[i] = nearest;
    }
    return out;
}

bool shown_in_groups(const std::vector<Layer>& layers, std::size_t index) {
    if (index >= layers.size() || !layers[index].visible) return false;
    if (!has_groups(layers)) return true;
    return shown_in_groups(layers, index, group_owners(layers));
}

bool shown_in_groups(const std::vector<Layer>& layers, std::size_t index, const std::vector<std::optional<std::size_t>>& owners) {
    if (index >= layers.size() || !layers[index].visible) return false;
    if (owners.size() != layers.size()) return true;
    for (auto at = owners[index]; at; at = owners[*at])
        if (!layers[*at].visible) return false;
    return true;
}

std::size_t group_picture_depth(const std::vector<Layer>& layers) {
    if (!has_groups(layers)) return 0;
    const std::vector<std::optional<std::size_t>> owners = group_owners(layers);
    const auto holds = [&](std::size_t i) { return group_changes_look(layers[i]) || (is_group(layers[i]) && layers[i].clip); };
    std::size_t most = 0;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        std::size_t d = is_group(layers[i]) && holds(i) ? 1 : 0;
        for (auto at = owners[i]; at; at = owners[*at])
            if (holds(*at)) ++d;
        most = std::max(most, d);
    }
    return most;
}

std::vector<std::size_t> group_contents(const std::vector<Layer>& layers, std::size_t group) {
    std::vector<std::size_t> out;
    if (group >= layers.size() || !is_group(layers[group])) return out;
    const std::vector<std::optional<std::size_t>> owners = group_owners(layers);
    for (std::size_t j = 0; j < layers.size(); ++j) {
        for (auto at = owners[j]; at; at = owners[*at]) {
            if (*at == group) {
                out.push_back(j);
                break;
            }
        }
    }
    return out;
}

std::vector<GroupStep> group_steps(const std::vector<Layer>& layers) {
    std::vector<GroupStep> steps;
    steps.reserve(layers.size());
    if (!has_groups(layers)) {
        for (std::size_t i = 0; i < layers.size(); ++i) steps.push_back(GroupStep{i, GroupStep::Kind::Layer, 0});
        return steps;
    }
    const std::vector<std::optional<std::size_t>> owners = group_owners(layers);
    std::vector<std::size_t> top;
    std::vector<std::vector<std::size_t>> held(layers.size());
    for (std::size_t i = 0; i < layers.size(); ++i) (owners[i] ? held[*owners[i]] : top).push_back(i);
    // (walked without recursion: a page may nest its groups deeply)
    struct Frame {
        const std::vector<std::size_t>* list;
        std::size_t next;
        std::optional<std::size_t> group;
        std::size_t begin;
    };
    std::vector<Frame> frames{Frame{&top, 0, std::nullopt, 0}};
    while (!frames.empty()) {
        Frame& frame = frames.back();
        if (frame.next == frame.list->size()) {
            if (frame.group) {
                const std::size_t end = steps.size();
                steps.push_back(GroupStep{*frame.group, GroupStep::Kind::End, frame.begin});
                steps[frame.begin].end = end;
            }
            frames.pop_back();
            continue;
        }
        const std::size_t i = (*frame.list)[frame.next++];
        if (is_group(layers[i])) {
            const std::size_t begin = steps.size();
            steps.push_back(GroupStep{i, GroupStep::Kind::Begin, 0});
            frames.push_back(Frame{&held[i], 0, i, begin});  // (frame is not used after this)
        } else {
            steps.push_back(GroupStep{i, GroupStep::Kind::Layer, 0});
        }
    }
    return steps;
}

}  // namespace genko::core
