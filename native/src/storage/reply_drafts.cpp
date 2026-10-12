#include "storage/reply_drafts.hpp"

#include <algorithm>
#include <system_error>

#include "core/error.hpp"
#include "core/json.hpp"
#include "core/pyconv.hpp"
#include "core/paths.hpp"
#include "storage/fsutil.hpp"

namespace genko::storage::reply_drafts {

namespace fs = std::filesystem;
using core::Json;

namespace {

const Json* get(const Json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

bool open_status(const Json& item) {
    const Json* status = get(item, "status");
    return status != nullptr && *status == Json("open");
}

// The id as the app keys its drafts: str() of it (a number too; None when there is none), as review_model does.
bool same_id(const Json& item, std::string_view id) {
    const Json* value = get(item, "id");
    return core::py_str(value != nullptr ? *value : Json()) == id;
}

bool has_frame(const Json& frames, std::string_view id) {
    if (!frames.is_array()) return false;
    for (const Json& frame : frames) {
        if (same_id(frame, id)) return true;
        if (const Json* children = get(frame, "children"); children != nullptr && has_frame(*children, id)) return true;
    }
    return false;
}

}  // namespace

fs::path folder(const fs::path& book) { return book / "studio" / "reply_drafts"; }

bool is_draft_name(std::string_view name) {
    if (name.size() != 64 + 5 || !name.ends_with(".json")) return false;
    return std::all_of(name.begin(), name.begin() + 64, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string file_name(std::string_view item) { return sha256_hex(item) + ".json"; }

std::optional<std::string> item_of(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(file, ec)) || ec) return std::nullopt;
    try {
        const std::string bytes = read_file_bounded(file, kMaxFile);
        if (core::utf8_error(bytes)) return std::nullopt;
        const Json data = core::parse_python_json(bytes);
        const Json* format = get(data, "format");
        const Json* item = get(data, "item");
        const Json* text = get(data, "text");
        const Json* at = get(data, "at");
        if (format == nullptr || *format != Json(kFormat) || item == nullptr || !item->is_string() || text == nullptr ||
            !text->is_string() || (at != nullptr && !at->is_string())) {
            return std::nullopt;
        }
        const std::string& key = item->get_ref<const std::string&>();
        if (key.empty() || text->get_ref<const std::string&>().size() > kMaxText || file_name(key) != core::path_to_utf8(file.filename())) {
            return std::nullopt;
        }
        return key;
    } catch (const core::Error&) {
        return std::nullopt;
    }
}

bool waiting(const Json& payload, std::string_view item) {
    if (item.starts_with("panel:")) {
        const std::string_view frame = item.substr(6);
        if (const Json* pages = get(payload, "pages"); pages != nullptr && pages->is_array()) {
            for (const Json& page : *pages) {
                if (const Json* frames = get(page, "frames"); frames != nullptr && has_frame(*frames, frame)) return true;
            }
        }
        return false;
    }
    if (const Json* tickets = get(payload, "tickets"); tickets != nullptr && tickets->is_array()) {
        for (const Json& ticket : *tickets) {
            if (same_id(ticket, item) && open_status(ticket)) return true;
        }
    }
    const Json* studio = get(payload, "studio");
    if (const Json* proposals = studio != nullptr ? get(*studio, "proposals") : nullptr; proposals != nullptr && proposals->is_object()) {
        for (const auto& [key, proposal] : proposals->items()) {
            (void)key;
            if (same_id(proposal, item) && open_status(proposal)) return true;
        }
    }
    return false;
}

bool is_leftover_name(std::string_view name) {
    return name.size() > 64 + 6 && is_draft_name(name.substr(0, 64 + 5)) && name.substr(64 + 5, 1) == "." && name.ends_with(".tmp");
}

std::vector<fs::path> leftovers(const fs::path& book) {
    std::vector<fs::path> out;
    const fs::path dir = folder(book);
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(dir, ec))) return out;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code type;
        if (is_leftover_name(core::path_to_utf8(it->path().filename())) && fs::is_regular_file(it->symlink_status(type))) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<fs::path> stale(const fs::path& book, const Json& payload) {
    std::vector<fs::path> out;
    const fs::path dir = folder(book);
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(dir, ec))) return out;  // (a folder that is a link is not the book's)
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!is_draft_name(core::path_to_utf8(it->path().filename()))) continue;
        const auto item = item_of(it->path());
        if (item && !waiting(payload, *item)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace genko::storage::reply_drafts
