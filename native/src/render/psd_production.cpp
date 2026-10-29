// PC5 J-0: the PSD's blocks the production copies (render/psd_production.hpp) — the table, empty until its groups.

#include "render/psd_production.hpp"

#include <algorithm>
#include <array>

namespace genko::render::psd_production {

namespace {

// (each group adds its row here — J-a: {1065, "", "J-a", read, write}; J-b: {1050, …}; J-c: {-1, "Anno", …}; J-d1: {1074, …})
constexpr std::array<Row, 0> kRows{};

}  // namespace

std::span<const Row> rows() { return kRows; }

bool copies_resource(int id) {
    const auto all = rows();
    return std::any_of(all.begin(), all.end(), [id](const Row& r) { return r.resource == id && r.read != nullptr; });
}

bool copies_global(std::string_view key) {
    const auto all = rows();
    return std::any_of(all.begin(), all.end(), [key](const Row& r) { return r.resource < 0 && r.global == key && r.read != nullptr; });
}

Taken read(core::Page& page, const std::vector<std::pair<int, std::string_view>>& resources,
           const std::vector<std::pair<std::string, std::string_view>>& globals, const ReadContext& context) {
    Taken taken;
    for (const Row& row : rows()) {
        if (row.read == nullptr) continue;
        if (row.resource >= 0) {
            for (const auto& [id, data] : resources) {
                if (id == row.resource && !taken.resources.contains(id) && row.read(page, data, context)) taken.resources.insert(id);
            }
        } else {
            for (const auto& [key, data] : globals) {
                if (key == row.global && !taken.globals.contains(key) && row.read(page, data, context)) taken.globals.insert(key);
            }
        }
    }
    return taken;
}

Written write(const core::Page& page, const WriteContext& context) {
    Written out;
    for (const Row& row : rows()) {
        if (row.write == nullptr) continue;
        std::optional<std::string> data = row.write(page, context);
        if (!data) continue;
        if (row.resource >= 0) {
            out.resources.emplace_back(row.resource, std::move(*data));
        } else {
            out.globals.emplace_back(std::string(row.global), std::move(*data));
        }
    }
    return out;
}

}  // namespace genko::render::psd_production
