#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"

// psd-keep@1 (PC4 節 F, format:PSD-PSB; docs/cpp-migration/schema-v4.md「ファイル・形式」): what a PSD or PSB held that
// this build does not read, kept as its bytes and written back where it was — a layer's tagged blocks (its effects, its
// label colour, …), the raw bytes of its blending ranges (Blend If) and the layer id it had; a page's image resources and
// global blocks (comps, slices, notes, patterns, …) and the file it came from. Each block is an asset (.psdblock). The
// reading and writing are the PSD's (render/psd, render/import_psd, formats/psd_export: PC4 F-d); here are the types,
// their JSON and their checks (core::Error("value")).
//
// A layer's `native_psd`: {"blocks": [[key, "sha256:…"], …], "ranges"?: "sha256:…", "id"?: int}
// A page's `native_psd`: {"resources": [[id, name, "sha256:…"], …], "globals": [[signature, key, "sha256:…"], …],
//                         "from": {"version": 1 | 2, "mode": int, "depth": int}}
// The book names psd-keep@1 while a layer or a page has one.

namespace genko::core {

struct Document;

inline constexpr std::string_view kPsdKeepFeature = "psd-keep@1";
inline constexpr std::string_view kPsdBlockSuffix = ".psdblock";
inline constexpr std::size_t kPsdKeptMax = 4096;  // blocks or resources one layer or page keeps

struct PsdLayerKeep {
    std::vector<std::pair<std::string, std::string>> blocks;  // its key (4 characters) and the block's bytes
    std::string ranges;                                       // the blending ranges' bytes, "" when every one is the default
    std::optional<std::int64_t> id;                           // the lyid it had

    bool empty() const { return blocks.empty() && ranges.empty() && !id; }
    bool operator==(const PsdLayerKeep&) const = default;
};

struct PsdResource {
    int id = 0;        // 0..65535
    std::string name;  // its Pascal name (at most 255 bytes)
    std::string ref;

    bool operator==(const PsdResource&) const = default;
};

struct PsdGlobal {
    std::string signature;  // 8BIM | 8B64
    std::string key;        // 4 characters
    std::string ref;

    bool operator==(const PsdGlobal&) const = default;
};

struct PsdFrom {
    int version = 1;  // 1 PSD, 2 PSB
    int mode = 3;     // Photoshop's colour mode number (0..9)
    int depth = 8;    // 1, 8, 16, 32

    bool operator==(const PsdFrom&) const = default;
};

struct PsdPageKeep {
    std::vector<PsdResource> resources;
    std::vector<PsdGlobal> globals;
    std::optional<PsdFrom> from;

    bool empty() const { return resources.empty() && globals.empty() && !from; }
    bool operator==(const PsdPageKeep&) const = default;
};

Json to_json(const PsdLayerKeep& keep);
Json to_json(const PsdPageKeep& keep);
PsdLayerKeep psd_layer_keep_from_json(const Json& value);
PsdPageKeep psd_page_keep_from_json(const Json& value);
void check_psd_keep(const PsdLayerKeep& keep);
void check_psd_keep(const PsdPageKeep& keep);

// Does the book name psd-keep@1 (a layer's or a page's native_psd, on its pages or a contents page)?
bool uses_psd_keep(const Document& doc);
// The book's kept PSD blocks as they must be (core::Error("value")). Pages not read yet are not looked at.
void validate_psd_keep(const Document& doc);
// The assets psd-keep@1 names (each once, in the order met), with their suffix.
std::vector<std::pair<std::string, std::string_view>> psd_keep_assets(const Document& doc);

}  // namespace genko::core
