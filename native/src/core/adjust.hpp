#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// PC2 (補正・色管理, SPEC.md COLOR-04): the corrections of the C++ build beyond Python's — a correction layer's "adjust"
// and the destructive filter_raster kinds. Python's own (core/filters.hpp) are read and drawn as they are.
//
//   * A Python kind's adjust may hold the C++ build's own settings in one object, `native` ({"kind": "levels", …,
//     "native": {…}}, design 2.2): its blend (every kind), the formula of levels and of a tone curve (method: genko —
//     Python's, the default — or upstream) and their channel records (channels, space), the upstream formulas of
//     brightness_contrast and posterize, a gradient map's reverse and dither. A layer without `native` is Python's.
//   * The C++ build's own kinds (core/adjust_kinds.hpp: hue_saturation … color_lookup, and the filter kinds desaturate
//     …) are stored in their normal form (every setting, the defaults filled, in the table's order).
//   * Every correction is one AdjustFunction to the three ways that draw it (ARCHITECTURE.md §4a): the 8-bit page
//     (render/composite: Python's form through Python's tables), the precise canvas (render/color_canvas) and the
//     ink canvas (render/ink_canvas); `native.blend` is laid by each way's own blend formula.
//   * A book using any of it names adjustments@1 (schema-v4.md).

namespace genko::core {

inline constexpr std::string_view kAdjustmentsFeature = "adjustments@1";

// The 3D lookup tables a correction may use (color_lookup's `lut`), by their asset ref: a book's (Document::luts), or
// those an op made of a file's text (`data`) — the op keeps them in its book (core/lut3d's keep_luts) or, a filter,
// uses them and lets them go.
using LookupTables = std::map<std::string, Bytes>;

// A function as a table per channel: red, green and blue each through its 256 values (0..255). The 8-bit page's fast way.
struct ChannelTables {
    std::array<std::array<std::uint8_t, 256>, 3> rgb{};
};

// Records laid on a CMYK, Lab or grey page's plates directly (design 4.2: levels' and a tone curve's channel records in
// the page's own space): each plate's tone — CMYK its brightness (1 − ink), Lab its normalized value (L/100,
// (a+128)/255, (b+128)/255), grey its lightness; 0..1 — through its function (an empty one: the plate as it is).
struct InkRecords {
    InkSpace space = InkSpace::Cmyk;
    std::vector<std::function<double(double)>> plates;
};

// What a correction does to a colour.
class AdjustFunction {
public:
    virtual ~AdjustFunction() = default;
    // One pixel's colour — the page's encoded RGB values (sRGB, or its profile's; 0..1, beyond for HDR) — changed; (x, y)
    // where it is on the page in pixels (a dither's place: a part of the page is drawn as the whole).
    virtual std::array<double, 3> apply(const std::array<double, 3>& rgb, int x, int y) const = 0;
    // Does where the pixel is change it (a dither)? (Else a colour met again is not computed again.)
    virtual bool positional() const { return false; }
    // The same as tables (the 8-bit page lays them on the pixels; nothing else of it is asked there), when it is one.
    virtual std::optional<ChannelTables> tables() const { return std::nullopt; }
    // Records for a CMYK, Lab or grey page's plates, when it has them (else the colours the inks mean go through apply
    // and back by the page's rules).
    virtual std::optional<InkRecords> ink_records() const { return std::nullopt; }
};
using AdjustPtr = std::shared_ptr<const AdjustFunction>;

// A correction's `native`, as the three ways need it.
struct AdjustNative {
    std::string blend = "normal";  // how the corrected picture is laid over what is under it (Genko's 20 blend modes)
    std::string method;            // levels and a tone curve: "genko" (Python's formula: the default) or "upstream"
};

// Does the adjust use what adjustments@1 names: a `native`, or a kind of the C++ build's own? (Else it is Python's.)
bool uses_adjustments(const Json& adjust);
bool uses_adjustments(const Document& doc);
// The adjust's `native` as stored (none: the defaults). core::Error("value") for one that is not an object, or a blend
// or method that is not a name (validate_adjustments says what else is wrong).
AdjustNative native_of(const Json& adjust);
// Do the colours change as Python's form changes them (its tables, PreciseAdjustment)? A Python kind whose native, if
// any, says only how it is laid (blend) or what changes nothing (method genko, an empty channels, reverse and dither
// false).
bool python_way(const Json& adjust);
// The adjust without its `native`: Python's settings (what core::adjustment and Python's apply_filter read).
Json without_native(const Json& adjust);

// An op's adjust (add_layer kind=adjust, set_layer adjust: the C++ build's own, as exposure was): its `native` read
// (only the keys given, in the table's order; an empty one dropped: the layer is Python's) and a kind of the C++
// build's own in its normal form. Python's own settings are left as given (the caller checks them as Python does).
// OpError for what is refused (a value of the wrong type, not finite, a key it does not know, a blend Lab or a
// multichannel page does not composite); numbers out of range are clamped, as PhotoCraft's fields are.
// core::Error("not_yet_ported") naming a kind or a native setting whose formula this build does not have yet. `made`:
// where the tables it makes of a file's text go (color_lookup's `data`: OpError without it).
Json normalize_adjust(const Json& adjust, const Page* page, LookupTables* made = nullptr);
// The settings of a filter_raster kind of the C++ build's own, or of a Python kind with a `native`: its normal form,
// as normalize_adjust (`params`: the op's settings, without op, page, layer, id, kind, area and channel).
Json normalize_filter(std::string_view kind, const Json& params, const Page* page, LookupTables* made = nullptr);
// Is filter_raster's kind (with these settings) one of the C++ build's own ways (a kind of its own, or a native)?
bool native_filter(std::string_view kind, const Json& params);

// adjustments@1 as stored: every correction layer's `native` and own kind well formed — known keys, values of the right
// type in their ranges, a blend its page composites (a Lab or multichannel page: as its layers' blends), a
// color_lookup's table held by the book (Document::luts: else it could not be written). core::Error ("value") naming
// the layer: the book opens read-only, an op that would leave it so is refused. Python's form is not looked at
// (Python's rule: a setting it cannot read makes the layer do nothing).
void validate_adjustments(const Document& doc);

// The function of a correction (its "adjust") on a page: Python's form as PreciseAdjustment (its continuous formulas
// of Python's tables), the rest by its kind's hooks (core/adjust_kinds.hpp). `doc`: the book whose lookup tables a
// color_lookup uses (its own only: none, or one it does not hold, does nothing).
// Null for a correction that does nothing (no kind; a kind Python's apply_filter does not know; Python's ValueError).
// core::Error("not_yet_ported") naming a kind or setting whose formula is not in this build yet.
AdjustPtr adjust_function(const Json& adjust, const Page& page, const Document* doc = nullptr);
// The function of a filter_raster kind of the C++ build's own (or a Python kind with a native) and its settings in
// their normal form; not_yet_ported as adjust_function. A kind that works beyond each pixel (PC2c's local
// corrections, equalize): not_yet_ported — the op does it (render/ops_adjust.cpp). `luts`: the tables it may use (the
// book's and those the op made).
AdjustPtr filter_function(std::string_view kind, const Json& params, const Page& page, const LookupTables* luts = nullptr);

// The words of a refusal of a kind or setting not in this build yet (core::Error("not_yet_ported")).
[[noreturn]] void adjust_not_yet(std::string_view what, std::string_view group);

}  // namespace genko::core
