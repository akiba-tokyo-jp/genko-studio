// PC5 J-0: the PSD's blocks the production copies (render/psd_production.hpp) — the table (J-a's row: render/psd_comps), the
// layers' metadata blocks (PC5 J-a).

#include "render/psd_production.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <set>

#include "core/studio.hpp"
#include "render/psd_comps.hpp"

namespace genko::render::psd_production {

namespace {

// (each group adds its row here — J-a: 1065 and its layers' cmls; J-b: {1050, …, true, …}; J-c: {-1, "Anno", "J-c", false,
// …} (its writer puts back the sound notes of WriteContext::kept_global("Anno")); J-d1: {1074, …} and {1080, "", "J-d1",
// false, nullptr, nullptr, nullptr, supersedes})
constexpr std::array<Row, 1> kRows{{
    {psd_comps::kResource, "", "J-a", true, psd_comps::read, psd_comps::write, psd_comps::fingerprint, nullptr, "cmls", psd_comps::write_layers},
}};

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
        c.fingerprint = row.fingerprint != nullptr ? row.fingerprint(page, context.doc) : std::string();
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

Written write(const core::Page& page, const WriteContext& given) {
    Written out;
    // (the rows name layers through it: their lyids written in their records — PC5 J-a, the review's P2-4)
    WriteContext context = given;
    context.name_layer = [&out, &given](const std::string& layer) -> std::optional<std::int64_t> {
        const auto found = given.lyids.find(layer);
        if (found == given.lyids.end()) return std::nullopt;
        out.layers.lyids[layer] = found->second;
        return found->second;
    };
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
        // (what the page holds of it: asked once — it may draw the layers, the comps' places)
        std::optional<std::string> held;
        const auto fingerprint = [&]() -> const std::string& {
            if (!held) held = row.fingerprint != nullptr ? row.fingerprint(page, context.doc) : std::string();
            return *held;
        };
        if (row.names_layers && !context.layers_written) {  // (by layers the file has not)
            if (mark != nullptr || !fingerprint().empty()) handled();
            continue;
        }
        if (mark != nullptr && row.fingerprint != nullptr && fingerprint() == mark->fingerprint) {
            std::optional<std::string> kept;
            if (resource && context.kept_resource) kept = context.kept_resource(row.resource);
            if (!resource && context.kept_global) kept = context.kept_global(row.global);
            if (kept) {
                put(std::move(*kept));  // (as it was read: the file's own bytes — its layers' kept shmd as they were too)
                continue;
            }
        }
        std::optional<std::string> own = row.write != nullptr ? row.write(page, context) : std::nullopt;
        if (own) {
            put(std::move(*own));
            if (!row.layer_item.empty()) {  // (its layers' items the page's own, in place of the kept ones)
                out.layers.keys.insert(std::string(row.layer_item));
                if (row.write_layers != nullptr) row.write_layers(page, context, out.layers);
            }
        } else if (mark != nullptr) {
            handled();  // (the page holds none of it any more: the kept one is not written)
            if (!row.layer_item.empty()) out.layers.keys.insert(std::string(row.layer_item));  // (nor its layers' items)
        }
    }
    return out;
}

// --- the layers' metadata block (shmd) --------------------------------------------------------------------------------

std::optional<std::vector<MetaItem>> shmd_items(std::string_view bytes) {
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k) v = (v << 8) | static_cast<unsigned char>(bytes[at + k]);
        return v;
    };
    if (bytes.size() < 4) return std::nullopt;
    const std::uint32_t count = u32(0);
    if (count > (bytes.size() - 4) / 16) return std::nullopt;
    std::vector<MetaItem> out;
    std::size_t at = 4;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (bytes.size() - at < 16) return std::nullopt;
        MetaItem item;
        item.signature = std::string(bytes.substr(at, 4));
        item.key = std::string(bytes.substr(at + 4, 4));
        item.copy = bytes[at + 8] != 0;
        item.reserved = std::string(bytes.substr(at + 9, 3));
        const std::uint32_t length = u32(at + 12);
        at += 16;
        if (length > bytes.size() - at) return std::nullopt;
        item.data = std::string(bytes.substr(at, length));
        at += length;
        out.push_back(std::move(item));
    }
    if (at != bytes.size()) return std::nullopt;  // (bytes after its items: not one)
    return out;
}

std::string shmd_bytes(const std::vector<MetaItem>& items) {
    std::string out;
    const auto put32 = [&](std::uint32_t v) {
        for (int k = 3; k >= 0; --k) out += static_cast<char>((v >> (8 * k)) & 0xff);
    };
    put32(static_cast<std::uint32_t>(items.size()));
    for (const MetaItem& item : items) {
        out += item.signature;
        out += item.key;
        out += static_cast<char>(item.copy ? 1 : 0);
        out += item.reserved;
        put32(static_cast<std::uint32_t>(item.data.size()));
        out += item.data;
    }
    return out;
}

std::optional<std::string> layer_shmd(const Written& own, const std::string& layer, const std::optional<std::string>& kept) {
    if (own.layers.keys.empty()) return kept;
    std::vector<MetaItem> items;
    if (kept) {
        auto read = shmd_items(*kept);
        if (!read) return kept;  // (not a shmd this build reads: as it was)
        items = std::move(*read);
    }
    const auto mine = own.layers.items.find(layer);
    const auto own_items = [&](const std::string& key) {
        std::vector<MetaItem> made;
        if (mine == own.layers.items.end()) return made;
        for (const auto& [k, data] : mine->second) {
            if (k != key) continue;
            MetaItem item;
            item.key = k;
            item.data = data;
            if (item.data.size() % 2 != 0) item.data += '\0';  // (Photoshop's even length)
            made.push_back(std::move(item));
        }
        return made;
    };
    std::vector<MetaItem> out;
    std::set<std::string> placed;
    for (MetaItem& item : items) {
        if (!own.layers.keys.contains(item.key)) {
            out.push_back(std::move(item));
            continue;
        }
        if (!placed.insert(item.key).second) continue;
        for (MetaItem& m : own_items(item.key)) out.push_back(std::move(m));
    }
    std::vector<MetaItem> first;  // (a key it had not: first, as PhotoCraft's set_cmls)
    for (const std::string& key : own.layers.keys) {
        if (placed.contains(key)) continue;
        for (MetaItem& m : own_items(key)) first.push_back(std::move(m));
    }
    out.insert(out.begin(), std::make_move_iterator(first.begin()), std::make_move_iterator(first.end()));
    if (out.empty()) return std::nullopt;
    return shmd_bytes(out);
}

}  // namespace genko::render::psd_production
