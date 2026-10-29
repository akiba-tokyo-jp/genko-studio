// PC5 J-0: the PSD's blocks the production copies (render/psd_production.hpp) — the table, empty until its groups.

#include "render/psd_production.hpp"

#include <algorithm>
#include <array>

#include "core/studio.hpp"

namespace genko::render::psd_production {

namespace {

// (each group adds its row here — J-a: {1065, "", "J-a", true, read, write, fingerprint}; J-b: {1050, …, true, …}; J-c:
// {-1, "Anno", "J-c", false, …} (its writer puts back the sound notes of WriteContext::kept_global("Anno")); J-d1: {1074,
// …} and {1080, "", "J-d1", false, nullptr, nullptr, nullptr, supersedes})
constexpr std::array<Row, 0> kRows{};

std::span<const Row>& table() {
    static std::span<const Row> current = kRows;
    return current;
}

const core::PsdCopied* mark_of(const core::Page& page, const Row& row) {
    if (!page.native_psd) return nullptr;
    for (const core::PsdCopied& c : page.native_psd->copied) {
        if (row.resource >= 0 ? c.resource == row.resource : (c.resource < 0 && c.global == row.global)) return &c;
    }
    return nullptr;
}

}  // namespace

std::span<const Row> rows() { return table(); }

TableForTest::TableForTest(std::span<const Row> rows) { table() = rows; }
TableForTest::~TableForTest() { table() = kRows; }

bool copies_resource(int id) {
    const auto all = rows();
    return std::any_of(all.begin(), all.end(), [id](const Row& r) { return r.resource == id && r.read != nullptr; });
}

bool copies_global(std::string_view key) {
    const auto all = rows();
    return std::any_of(all.begin(), all.end(), [key](const Row& r) { return r.resource < 0 && r.global == key && r.read != nullptr; });
}

std::string fingerprint_of(const core::Json& value) { return core::content_hash(value); }

Taken read(core::Page& page, const std::vector<std::pair<int, std::string_view>>& resources,
           const std::vector<std::pair<std::string, std::string_view>>& globals, const ReadContext& context) {
    Taken taken;
    const auto mark = [&](const Row& row, int resource, const std::string& global) {
        core::PsdCopied c;
        c.resource = resource;
        c.global = global;
        c.fingerprint = row.fingerprint != nullptr ? row.fingerprint(page) : std::string();
        if (!c.fingerprint.empty()) taken.copied.push_back(std::move(c));
    };
    for (const Row& row : rows()) {
        if (row.read == nullptr) continue;
        if (row.resource >= 0) {
            // (the first of a file's resources of the id: one a page holds)
            for (const auto& [id, data] : resources) {
                if (id != row.resource || taken.resources.contains(id)) continue;
                if (row.read(page, data, context)) {
                    taken.resources.insert(id);
                    mark(row, id, {});
                }
                break;
            }
        } else {
            for (const auto& [key, data] : globals) {
                if (key != row.global || taken.globals.contains(key)) continue;
                if (row.read(page, data, context)) {
                    taken.globals.insert(key);
                    mark(row, -1, key);
                }
                break;
            }
        }
    }
    return taken;
}

Written write(const core::Page& page, const WriteContext& context) {
    Written out;
    for (const Row& row : rows()) {
        const bool resource = row.resource >= 0;
        const auto handled = [&] {
            if (resource) {
                out.handled_resources.insert(row.resource);
            } else {
                out.handled_globals.insert(std::string(row.global));
            }
        };
        const auto put = [&](std::string data) {
            if (resource) {
                out.resources.emplace_back(row.resource, std::move(data));
            } else {
                out.globals.emplace_back(std::string(row.global), std::move(data));
            }
            handled();
        };
        if (row.supersedes != nullptr && row.supersedes(page)) handled();
        const core::PsdCopied* mark = mark_of(page, row);
        if (row.names_layers && !context.layers_written) {  // (by layers the file has not)
            if (mark != nullptr || (row.fingerprint != nullptr && !row.fingerprint(page).empty())) handled();
            continue;
        }
        if (mark != nullptr && row.fingerprint != nullptr && row.fingerprint(page) == mark->fingerprint) {
            std::optional<std::string> kept;
            if (resource && context.kept_resource) kept = context.kept_resource(row.resource);
            if (!resource && context.kept_global) kept = context.kept_global(row.global);
            if (kept) {
                put(std::move(*kept));  // (as it was read: the file's own bytes)
                continue;
            }
        }
        std::optional<std::string> own = row.write != nullptr ? row.write(page, context) : std::nullopt;
        if (own) {
            put(std::move(*own));
        } else if (mark != nullptr) {
            handled();  // (the page holds none of it any more: the kept one is not written)
        }
    }
    return out;
}

}  // namespace genko::render::psd_production
