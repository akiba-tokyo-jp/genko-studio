// psd-keep@1 (core/psd_keep.hpp).

#include "core/psd_keep.hpp"

#include <algorithm>
#include <array>
#include <set>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/pyconv.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace {

[[noreturn]] void refuse(const std::string& message) { throw Error("value", message); }

bool is_ref(std::string_view text) {
    return text.size() == 71 && text.starts_with("sha256:") &&
           std::all_of(text.begin() + 7, text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A tagged block's key or signature: four printable ASCII characters.
bool is_key(std::string_view text) {
    return text.size() == 4 && std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c < 0x7f; });
}

const Json* get(const Json& object, std::string_view key) {
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

void only_keys(const Json& object, std::initializer_list<std::string_view> known, const std::string& what) {
    for (const auto& [key, value] : object.items()) {
        if (std::find(known.begin(), known.end(), key) == known.end()) refuse(what + " key " + py_repr_str(key) + " is not known to this build");
    }
}

std::string text(const Json& value, const std::string& what) {
    if (!value.is_string()) refuse(what + " is text");
    return value.get<std::string>();
}

std::int64_t whole(const Json& value, const std::string& what) {
    if (!value.is_number_integer()) refuse(what + " is a whole number");
    return value.get<std::int64_t>();
}

const Json& list_of(const Json& object, const char* key, const std::string& what) {
    static const Json empty = Json::array();
    const Json* v = get(object, key);
    if (v == nullptr || v->is_null()) return empty;
    if (!v->is_array()) refuse(what + "'s " + key + " is a list");
    return *v;
}

}  // namespace

Json to_json(const PsdLayerKeep& keep) {
    Json out = Json::object();
    Json blocks = Json::array();
    for (const auto& [key, ref] : keep.blocks) blocks.push_back(Json::array({key, ref}));
    out["blocks"] = std::move(blocks);
    if (!keep.ranges.empty()) out["ranges"] = keep.ranges;
    if (keep.id) out["id"] = *keep.id;
    return out;
}

Json to_json(const PsdPageKeep& keep) {
    Json out = Json::object();
    Json resources = Json::array();
    for (const PsdResource& r : keep.resources) resources.push_back(Json::array({r.id, r.name, r.ref}));
    out["resources"] = std::move(resources);
    Json globals = Json::array();
    for (const PsdGlobal& g : keep.globals) globals.push_back(Json::array({g.signature, g.key, g.ref}));
    out["globals"] = std::move(globals);
    if (keep.from) out["from"] = Json{{"version", keep.from->version}, {"mode", keep.from->mode}, {"depth", keep.from->depth}};
    return out;
}

PsdLayerKeep psd_layer_keep_from_json(const Json& value) {
    const std::string what = "a layer's native_psd";
    if (!value.is_object()) refuse(what + " is an object");
    only_keys(value, {"blocks", "ranges", "id"}, what);
    PsdLayerKeep keep;
    for (const Json& block : list_of(value, "blocks", what)) {
        if (!block.is_array() || block.size() != 2) refuse(what + "'s blocks are [key, asset]");
        keep.blocks.emplace_back(text(block[0], what + "'s block key"), text(block[1], what + "'s block asset"));
    }
    if (const Json* ranges = get(value, "ranges"); ranges != nullptr && !ranges->is_null()) keep.ranges = text(*ranges, what + "'s ranges");
    if (const Json* id = get(value, "id"); id != nullptr && !id->is_null()) keep.id = whole(*id, what + "'s id");
    check_psd_keep(keep);
    return keep;
}

PsdPageKeep psd_page_keep_from_json(const Json& value) {
    const std::string what = "a page's native_psd";
    if (!value.is_object()) refuse(what + " is an object");
    only_keys(value, {"resources", "globals", "from"}, what);
    PsdPageKeep keep;
    for (const Json& item : list_of(value, "resources", what)) {
        if (!item.is_array() || item.size() != 3) refuse(what + "'s resources are [id, name, asset]");
        keep.resources.push_back(PsdResource{static_cast<int>(std::clamp<std::int64_t>(whole(item[0], what + "'s resource id"), -1, 65536)),
                                             text(item[1], what + "'s resource name"), text(item[2], what + "'s resource asset")});
    }
    for (const Json& item : list_of(value, "globals", what)) {
        if (!item.is_array() || item.size() != 3) refuse(what + "'s globals are [signature, key, asset]");
        keep.globals.push_back(PsdGlobal{text(item[0], what + "'s global signature"), text(item[1], what + "'s global key"),
                                         text(item[2], what + "'s global asset")});
    }
    if (const Json* from = get(value, "from"); from != nullptr && !from->is_null()) {
        if (!from->is_object()) refuse(what + "'s from is {version, mode, depth}");
        only_keys(*from, {"version", "mode", "depth"}, what + "'s from");
        const auto number = [&](const char* key) {
            const Json* v = get(*from, key);
            if (v == nullptr) refuse(what + "'s from is {version, mode, depth}");
            return static_cast<int>(std::clamp<std::int64_t>(whole(*v, what + "'s from " + key), -1, 1000));
        };
        keep.from = PsdFrom{number("version"), number("mode"), number("depth")};
    }
    check_psd_keep(keep);
    return keep;
}

void check_psd_keep(const PsdLayerKeep& keep) {
    const std::string what = "a layer's native_psd";
    if (keep.blocks.size() > kPsdKeptMax) refuse(what + " keeps more than 4096 blocks");
    for (const auto& [key, ref] : keep.blocks) {
        if (!is_key(key)) refuse(what + "'s block key is four characters");
        if (!is_ref(ref)) refuse(what + "'s block is an asset (sha256:…)");
    }
    if (!keep.ranges.empty() && !is_ref(keep.ranges)) refuse(what + "'s ranges are an asset (sha256:…)");
    if (keep.id && (*keep.id < 0 || *keep.id > 0xffffffffLL)) refuse(what + "'s id is a layer id (0..4294967295)");
}

void check_psd_keep(const PsdPageKeep& keep) {
    const std::string what = "a page's native_psd";
    if (keep.resources.size() > kPsdKeptMax || keep.globals.size() > kPsdKeptMax) refuse(what + " keeps more than 4096 resources or blocks");
    for (const PsdResource& r : keep.resources) {
        if (r.id < 0 || r.id > 65535) refuse(what + "'s resource id is 0..65535");
        if (r.name.size() > 255) refuse(what + "'s resource name is at most 255 bytes");
        if (!is_ref(r.ref)) refuse(what + "'s resource is an asset (sha256:…)");
    }
    for (const PsdGlobal& g : keep.globals) {
        if (g.signature != "8BIM" && g.signature != "8B64") refuse(what + "'s global signature is 8BIM or 8B64");
        if (!is_key(g.key)) refuse(what + "'s global key is four characters");
        if (!is_ref(g.ref)) refuse(what + "'s global block is an asset (sha256:…)");
    }
    if (keep.from) {
        if (keep.from->version != 1 && keep.from->version != 2) refuse(what + "'s from version is 1 (PSD) or 2 (PSB)");
        if (keep.from->mode < 0 || keep.from->mode > 9) refuse(what + "'s from mode is Photoshop's colour mode (0..9)");
        if (keep.from->depth != 1 && keep.from->depth != 8 && keep.from->depth != 16 && keep.from->depth != 32)
            refuse(what + "'s from depth is 1, 8, 16 or 32");
    }
}

bool uses_psd_keep(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (written.page->native_psd) return true;
        for (const Layer& layer : written.page->layers) {
            if (layer.native_psd) return true;
        }
    }
    return false;
}

void validate_psd_keep(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (written.page->native_psd) check_psd_keep(*written.page->native_psd);
        for (const Layer& layer : written.page->layers) {
            if (layer.native_psd) check_psd_keep(*layer.native_psd);
        }
    }
}

std::vector<std::pair<std::string, std::string_view>> psd_keep_assets(const Document& doc) {
    std::vector<std::pair<std::string, std::string_view>> out;
    std::set<std::string> seen;
    const auto add = [&](const std::string& ref) {
        if (!ref.empty() && seen.insert(ref).second) out.emplace_back(ref, kPsdBlockSuffix);
    };
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (const auto& keep = written.page->native_psd) {
            for (const PsdResource& r : keep->resources) add(r.ref);
            for (const PsdGlobal& g : keep->globals) add(g.ref);
        }
        for (const Layer& layer : written.page->layers) {
            if (!layer.native_psd) continue;
            for (const auto& [key, ref] : layer.native_psd->blocks) add(ref);
            add(layer.native_psd->ranges);
        }
    }
    return out;
}

}  // namespace genko::core
