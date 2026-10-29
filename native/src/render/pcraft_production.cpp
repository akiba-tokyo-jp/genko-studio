// PC5 J-0: the .pcraft document's keys the production copies (render/pcraft_production.hpp) — the table, its rows not
// read or written until their groups.

#include "render/pcraft_production.hpp"

#include <algorithm>
#include <array>

#include "render/pcraft_annotations.hpp"
#include "render/pcraft_comps.hpp"
#include "render/pcraft_manifest.hpp"
#include "render/pcraft_measure.hpp"
#include "render/pcraft_variables.hpp"

namespace genko::render::pcraft_production {

namespace {

bool holds_something(const core::Json& v) {
    if (v.is_null()) return false;
    if (v.is_array()) return !v.empty();
    if (v.is_object()) return std::any_of(v.begin(), v.end(), [](const core::Json& item) { return holds_something(item); });
    return true;
}

// (its scale alone, as every document has one: nothing to say)
bool measuring(const core::Json& v) {
    const core::Json* counts = pcraft::optional_field(v, "count_groups");
    return (counts != nullptr && holds_something(*counts)) || pcraft::optional_field(v, "ruler") != nullptr;
}

// (each group puts its reader and writer here: J-a layer_comps, J-b slices, J-c notes, J-d1 measurement, J-e1 timeline;
// 節 I's variables — I-d — among them, where PC4 F-f's notes said it: the notes in the order they had)
constexpr std::array<Row, 6> kRows{{
    {"layer_comps", "レイヤーカンプ", holds_something, pcraft_comps::read, pcraft_comps::write},
    {"notes", "注釈", holds_something, pcraft_annotations::read, pcraft_annotations::write},  // (PC5 J-c)
    {"slices", "スライス", holds_something, nullptr, nullptr},
    {"timeline", "タイムライン", holds_something, nullptr, nullptr},
    {"variables", "変数とデータセット", holds_something, pcraft_variables::read, pcraft_variables::write},  // (PC5 I-d)
    {"measurement", "計測（カウント・ものさし）", measuring, pcraft_measure::read, pcraft_measure::write},  // (PC5 J-d1)
}};

}  // namespace

std::span<const Row> rows() { return kRows; }

void read(const core::Json& document, ReadContext& context) {
    context.document = &document;
    for (const Row& row : rows()) {
        const core::Json* value = pcraft::optional_field(document, row.key);
        if (value == nullptr) continue;
        if (row.read != nullptr) {
            row.read(*value, context);
        } else if (row.worth_saying != nullptr && row.worth_saying(*value) && context.say) {
            context.say(std::string(row.what) + "は Genko に無いため外しました");
        }
    }
}

void write(const core::Page& page, core::Json& document, WriteContext& context) {
    context.document = &document;
    for (const Row& row : rows()) {
        if (row.write == nullptr) continue;
        if (std::optional<core::Json> value = row.write(page, context)) document[std::string(row.key)] = std::move(*value);
    }
}

}  // namespace genko::render::pcraft_production
