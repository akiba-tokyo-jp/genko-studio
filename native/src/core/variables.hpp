#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/json.hpp"

// variables@1 (PC5 I-d, SPEC.md AUTO-01「変数とデータセット」, docs/cpp-migration/schema-v4.md「変数とデータセット（variables@1）」):
// Photoshop's イメージ → 変数 ▸ (data-driven graphics) after PhotoCraft v0.2.0's doc/src/variables.rs and
// engine/src/variables_cmds.rs (MIT, native/third_party/photocraft/NOTICE.md). The book's `variables`:
//   {"defs": [{"name": str (1..255 characters, one of its name in the book), "type": "visibility" | "text" | "pixels",
//              "page": page id, "layer": layer id            (a layer: visibility any layer, text a type layer, pixels a paint
//                                                              layer or a smart object), or
//              "line": line id                                (text only: a 台詞 of the book's story),
//              "method": "fit"|"fill"|"as_is"|"conform", "align": "top_left"|…|"center"|…|"bottom_right", "clip": bool,
//              "box_mm": [x0, y0, x1, y1]                     (pixels only, always written: the box a picture is put in — the
//                                                              layer's content when it was defined, or the paper)}],
//    "data_sets": [{"name": str (1..255 characters, one of its name), "values": [{"variable": a def's name (one value each),
//                   "value": bool (visibility) | str (text) | {"asset": "sha256:…", "suffix": ".png"|…, "name"?: str}
//                   (pixels: a picture the book keeps — never a path)}]}],
//    "active"?: the index of the data set applied last}
// at most kVariableDefsMax defs and kDataSetsMax data sets. A def whose layer or line is gone is kept (applying skips it,
// and says so); a value whose variable is gone or of another type likewise. A book with defs or data sets names
// variables@1 (a book without them has no `variables`); a build that does not know it opens the book read-only. The
// pictures of the pixels values are the book's assets (assets/<ab>/<64 hex><suffix>; while only the book in memory holds
// them, Document::smart_files) — ops never read a path (the window and the command line read the files: formats/datasets).

namespace genko::core {

struct Document;

inline constexpr std::string_view kVariablesFeature = "variables@1";
inline constexpr std::size_t kVariableDefsMax = 1000;
inline constexpr std::size_t kDataSetsMax = 10000;
inline constexpr std::size_t kVariableNameMax = 255;      // characters (a def's, a data set's)
inline constexpr std::size_t kVariableTextMax = 65536;    // bytes of a text value
inline constexpr std::uint64_t kVariablePicturesMax = 4ull << 30;  // bytes of every picture the values name, together
inline constexpr double kVariableBoxMaxMm = 100000.0;             // a box's edges within 100 m of the page's corner

// The nine places a picture is aligned to in its box (PhotoCraft's PixelAlign, Photoshop's 配置).
inline constexpr std::string_view kVariableAligns[] = {"top_left",    "top_center",    "top_right",
                                                       "center_left", "center",        "center_right",
                                                       "bottom_left", "bottom_center", "bottom_right"};
// How a picture is sized into its box (PhotoCraft's PixelMethod, Photoshop's 方法: 合わせる・塗りつぶす・そのまま・サイズに合わせる).
inline constexpr std::string_view kVariableMethods[] = {"fit", "fill", "as_is", "conform"};

struct VariableDef {
    std::string name;
    std::string type;                 // visibility | text | pixels
    std::optional<std::string> page;  // (a layer's) its page's id
    std::optional<std::string> layer;
    std::optional<std::string> line;  // (text) a line's id
    std::string method = "fit";       // (pixels)
    std::string align = "center";
    bool clip = false;
    std::array<double, 4> box_mm{0, 0, 0, 0};  // (pixels) x0, y0, x1, y1 on the page, mm (4 decimals)
};

struct PictureValue {
    std::string asset;   // "sha256:…"
    std::string suffix;  // a smart object's file suffix (core::smart::format_of_suffix knows it)
    std::string name;    // the file's name (empty: none)
    friend bool operator==(const PictureValue&, const PictureValue&) = default;
};

struct DataValue {
    std::string variable;
    std::variant<bool, std::string, PictureValue> value;
};

struct DataSet {
    std::string name;
    std::vector<DataValue> values;
};

struct Variables {
    std::vector<VariableDef> defs;
    std::vector<DataSet> data_sets;
    std::optional<std::int64_t> active;
    bool empty() const { return defs.empty() && data_sets.empty(); }
};

// The book's variables read from their JSON (core::Error("value") says what is not as they must be), and written in their
// normal form (pixels' method, align and clip always; a value's name only when it has one; active only when set).
Variables variables_from_json(const Json& data);
Json variables_to_json(const Variables& variables);
// The book's variables (none: empty), and set (empty: the book's `variables` gone).
Variables variables_of(const Document& doc);
void set_variables(Document& doc, Variables variables);

// The type of the value a def of that type takes ("visibility": bool, "text": str, "pixels": a picture).
bool value_fits(const std::string& type, const DataValue& value);
// The def of that name (none: none).
const VariableDef* find_def(const Variables& variables, std::string_view name);
// The data set of that name (none: none).
std::optional<std::size_t> find_data_set(const Variables& variables, std::string_view name);

// Does the book need variables@1?
bool uses_variables(const Document& doc);
// The book's variables as they must be (core::Error("value")): in their normal form.
void validate_variables(const Document& doc);
// The pictures the book's values name, each once (its asset ref and suffix), in their order.
std::vector<std::pair<std::string, std::string>> variable_pictures(const Document& doc);

}  // namespace genko::core
