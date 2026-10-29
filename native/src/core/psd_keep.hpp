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
//                         "ids"?: [int, …], "from": {"version": 1 | 2, "mode": int, "depth": int}}
// The book names psd-keep@1 while a layer or a page has one.
//
// PC4 F-d (SPEC.md FILE-01「PSD・PSB」): what is kept and written back — after PhotoCraft v0.2.0's io/src/psd_import.rs
// (REGENERATED, MAPPED_RESOURCES) and psd/src/tagged.rs (PSB_LONG_KEYS), MIT, with this build's own rules: a layer keeps
// every tagged block but those this build writes or reads into the layer (its name, id and section — its locks' lspf
// kept when it locks something, written again from the layer's locks with its other bits —; the
// adjustment, fill, vector, placed and text blocks PC2 and PC3 read); a page every image resource but those this build
// writes or reads (resolution, profile, XMP, EXIF, thumbnails, version, paths, channels and their names and looks, the
// layers' state, groups and selection, the grid and guides, an indexed page's table) and every global block but the
// layer info (Lr16, Lr32, Layr), the merged transparency (Mtrn, Mt16, Mt32) and the linked files (lnk2, lnk3, lnkD,
// lnkE: written again from the smart objects). A page's `ids` are the layer ids (lyid) of the layers read with them: its
// layer comps (1065) and slices (1050), which name layers, are written back only while each of those layers is on it.

namespace genko::core {

struct Document;
struct Layer;

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

// (PC5 J-0, render/psd_production) A kept block the page's own was read from — a layer comps', slices', notes' or
// measurement scale's —, and the fingerprint of what the page held right after: the kept bytes are written again while the
// page's is the same, else the page's own (none: neither).
struct PsdCopied {
    int resource = -1;        // an image resource's id (a global block: -1)
    std::string global;       // a global block's key (an image resource: "")
    std::string fingerprint;  // (1..128 bytes)

    bool operator==(const PsdCopied&) const = default;
};

struct PsdPageKeep {
    std::vector<PsdResource> resources;
    std::vector<PsdGlobal> globals;
    std::vector<std::int64_t> ids;  // (PC4 F-d) the lyids of the layers read with them (0..4294967295)
    std::optional<PsdFrom> from;
    std::vector<PsdCopied> copied;  // (PC5 J-0: "copied": [[id | key, fingerprint], …], only while there is one)

    bool empty() const { return resources.empty() && globals.empty() && ids.empty() && !from && copied.empty(); }
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

// PC4 F-d: the rules above. Is a layer's tagged block of this key kept (not one this build writes or reads into the
// layer)? An image resource of this id? A global block of this key?
bool psd_layer_block_kept(std::string_view key);
bool psd_resource_kept(int id);
bool psd_global_kept(std::string_view key);
// The image resources written back only while the layers read with them are on the page: layer comps and slices.
bool psd_resource_names_layers(int id);
// A layer effects' block (lfx2, lfxs, lmfx, lrFX): kept, and not drawn by this build.
bool psd_effect_key(std::string_view key);
// Does the layer keep a PSD's layer effects (drawn by Photoshop, not by this build: the layer panel's mark)? A folder's
// are not written back (folders are not PSD groups): not said.
bool psd_effects_kept(const Layer& layer);
// A pattern block (Patt, Pat2, Pat3): kept with the patterns no fill of the book took, merged with the book's own.
bool psd_pattern_key(std::string_view key);
// The tagged blocks a PSB holds with 8-byte lengths (Adobe's file format specification; PhotoCraft's PSB_LONG_KEYS).
bool psb_long_key(std::string_view key);
// The largest block or resource kept (an asset: storage::kAssetMaxBytes); a larger one is not kept.
inline constexpr std::uint64_t kPsdBlockMaxBytes = std::uint64_t{256} * 1024 * 1024;

}  // namespace genko::core
