#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// image-assets@1 (PC5 I-f, SPEC.md AUTO-01「画像アセット」, docs/cpp-migration/schema-v4.md「画像アセット（image-assets@1）」):
// Photoshop's ファイル → 生成 → 画像アセット after PhotoCraft v0.2.0's engine/src/web_cmds.rs (969-1191: the naming rule,
// parse_asset_name and parse_defaults; MIT, native/third_party/photocraft/NOTICE.md). The book's "generator":
// {"image_assets": true} while it is on (none while it is off: the book's bytes as before); the files are written by the
// window on each save the person makes (never the automatic save, never an op: formats/image_assets.hpp).

namespace genko::core {

class OpRegistry;

inline constexpr std::string_view kImageAssetsFeature = "image-assets@1";

// The book's "generator" as the writer writes it ({"image_assets": true}), and back (core::Error("value") for anything
// else: another key, a value that is not true).
Json image_assets_json();
bool image_assets_from_json(const Json& value);

// set_image_assets {"on": bool} → {"image_assets": bool}: 生成 → 画像アセット on or off for the book (a change of the book,
// undone as any; nothing is written by it). A book op (never per page, for_pages refuses it).
void register_image_asset_ops(OpRegistry& registry);

namespace image_assets {

// One file a layer's name asks for: "foo.png", "200% foo@2x.png", "48x48 icons/bar.png8", "photo.jpg80%", "?x100 a.jpg5",
// "1in x 2cm label.gif" — names joined by "," or "+".
struct Spec {
    std::string file;    // its path under the assets' folder, the extension as written (png, jpg, jpeg, gif, webp)
    std::string format;  // png32 | png24 | png8 | jpg | gif | webp
    std::optional<int> quality;  // JPEG's 1..100 (a number 1..10 is tenths: jpg8 is 80)
    std::optional<double> scale;   // 1.0 = 100 %
    std::optional<double> width;   // px (in, cm and mm at the page's resolution); none with the other given: in proportion
    std::optional<double> height;
    bool operator==(const Spec&) const = default;
};

// A "default …" layer's variants: each file is written once for each (its size scaled again, then into its folder with
// its suffix): "default 50% low/ + 200% @2x".
struct Default {
    std::optional<double> scale;
    std::optional<double> width;
    std::optional<double> height;
    std::string folder;  // "low/" (ends with "/"), or ""
    std::string suffix;  // "@2x", or ""
    bool operator==(const Default&) const = default;
};

// The files a layer's name asks for (none: it names no asset). `dpi`: what in, cm and mm are counted at.
std::vector<Spec> parse_name(std::string_view name, double dpi);
// A "default …" name's variants (none: not such a name; an empty list: "default" alone).
std::optional<std::vector<Default>> parse_defaults(std::string_view name, double dpi);
// A path under the assets' folder its rule takes: relative, "/" between its parts, no part empty, "." or "..", no "\", ":"
// or control character. (Others are said as the asset's error and nothing is written for it.)
bool safe_relative(std::string_view path);

}  // namespace image_assets

}  // namespace genko::core
