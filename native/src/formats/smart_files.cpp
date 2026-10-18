#include "formats/smart_files.hpp"

#include <algorithm>
#include <set>
#include <system_error>

#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "formats/export.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/smart.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"

namespace genko::formats::smart_files {

namespace {

using core::Json;

bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// The book with only these contents' page as its page (what drawing and writing the page alone take: the book's
// contents, files and settings, no lines).
core::Document page_alone(const core::Document& doc, const core::PagePtr& page) {
    core::Document out = doc;
    out.pages = {page};
    out.deferred.clear();
    out.story.clear();
    return out;
}

const core::SmartContents& held(const core::Document& doc, const std::string& id) {
    const auto found = doc.smart_contents.find(id);
    if (found == doc.smart_contents.end() || !found->second) throw core::Error("not_found", "no smart object contents " + id);
    return *found->second;
}

}  // namespace

std::optional<fs::path> linked_file(const Json& link, const std::optional<fs::path>& book_dir) {
    if (!link.is_object()) return std::nullopt;
    const Json* path = core::get(link, "path");
    if (path != nullptr && path->is_string() && !path->get_ref<const std::string&>().empty()) {
        const fs::path at = core::path_from_utf8(path->get<std::string>());
        if (at.is_absolute() && is_file(at)) return at;
    }
    const Json* relative = core::get(link, "relative");
    if (book_dir && relative != nullptr && relative->is_string() && !relative->get_ref<const std::string&>().empty()) {
        const fs::path at = (*book_dir / core::path_from_utf8(relative->get<std::string>())).lexically_normal();
        if (is_file(at)) return at;
    }
    return std::nullopt;
}

std::vector<LinkState> link_states(const core::Document& doc) {
    std::vector<LinkState> out;
    for (const std::string& id : core::smart::used_contents(doc)) {
        const auto found = doc.smart_contents.find(id);
        if (found == doc.smart_contents.end() || !found->second || !found->second->link) continue;
        const core::SmartContents& contents = *found->second;
        const Json& link = *contents.link;
        LinkState state;
        state.contents = id;
        state.name = contents.name;
        state.path = core::py_str(core::py_get(link, "path", Json("")));
        state.relative = core::py_str(core::py_get(link, "relative", Json("")));
        state.found = linked_file(link, doc.asset_dir);
        state.shown = core::smart::showing(doc, id);
        if (!state.found) {
            state.status = "missing";
        } else {
            std::error_code ec;
            const auto size = fs::file_size(*state.found, ec);
            const Json* bytes = core::get(link, "bytes");
            const bool same_size = !ec && bytes != nullptr && bytes->is_number_integer() && static_cast<std::uintmax_t>(bytes->get<std::int64_t>()) == size;
            std::string sha;
            if (same_size) {
                try {
                    sha = storage::sha256_file(*state.found);
                } catch (const core::Error&) {
                }
            }
            state.status = same_size && sha == core::py_str(core::py_get(link, "sha256", Json(""))) ? "ok" : "modified";
        }
        out.push_back(std::move(state));
    }
    return out;
}

Json to_json(const LinkState& state) {
    Json out{{"contents", state.contents}, {"name", state.name}, {"path", state.path}, {"status", state.status}};
    if (!state.relative.empty()) out["relative"] = state.relative;
    if (state.found) out["found"] = core::path_to_utf8(*state.found);
    if (state.shown) {
        Json shown{{"page", state.shown->page.json()}, {"id", state.shown->layer}};
        if (state.shown->inside) shown["contents"] = *state.shown->inside;
        out["shown"] = shown;
    }
    return out;
}

std::string export_suffix(const core::SmartContents& contents, const std::string& format) {
    if (contents.kind == "file") {
        if (!format.empty() && format != contents.format && format != contents.suffix.substr(1))
            throw core::Error("value", "a file's contents are written as they are (" + contents.format + ")");
        return contents.suffix;
    }
    if (format.empty() || format == "psd") return ".psd";
    if (format == "png") return ".png";
    throw core::Error("value", "a page's contents are written as psd or png");
}

Json export_contents(const core::Document& doc, const std::string& contents_id, const fs::path& dest, std::string format) {
    const core::SmartContents& contents = held(doc, contents_id);
    const std::string suffix = export_suffix(contents, format);
    if (contents.kind == "file") {
        const core::Bytes bytes = render::smart::file_bytes(doc, contents);
        if (!bytes) throw core::Error("not_found", "the smart object's file is not in the book");
        storage::write_atomic(dest, *bytes);
        return Json{{"path", core::path_to_utf8(dest)}, {"kind", "file"}, {"format", contents.format}, {"bytes", bytes->size()}};
    }
    if (!contents.page) throw core::Error("not_found", "the smart object contents " + contents_id + " have no page");
    const core::Document alone = page_alone(doc, contents.page);
    const auto dpi = std::max<std::int64_t>(1, core::py_int(contents.page->spec.dpi.json()));
    if (suffix == ".psd") {
        export_contents_psd(alone, *contents.page, dest, dpi);
    } else {
        render::RenderOptions options;
        options.mode = "print";
        const render::Image drawn = render::render_contents(*contents.page, static_cast<int>(dpi), options, &alone).image;
        storage::write_atomic(dest, render::write_png(drawn.convert("RGBA")));
    }
    std::error_code ec;
    const auto size = fs::file_size(dest, ec);
    return Json{{"path", core::path_to_utf8(dest)}, {"kind", "page"}, {"format", suffix.substr(1)}, {"bytes", ec ? 0 : size}};
}

Json package(const fs::path& book, const fs::path& out, const std::string& actor) {
    storage::LoadResult loaded = storage::load_document(book);
    core::Document doc = std::move(loaded.document);
    if (!doc.read_only_reason.empty()) throw core::Error("read_only", "the book is not whole, so it is not packed: " + doc.read_only_reason);
    const fs::path name = book.filename().empty() ? book.parent_path().filename() : book.filename();
    const fs::path target = out / name;
    std::error_code ec;
    if (fs::exists(target / "project.json", ec)) {
        throw core::Error("exists", "the package's place holds a book already: " + core::path_to_utf8(target / "project.json"));
    }
    storage::make_dirs_durable(target);
    storage::ProjectLock lock(target, actor);
    lock.try_acquire();
    storage::copy_assets(book, target);  // (every file of the book's: its pictures, contents and their history)
    doc.asset_dir = target;
    doc.book_id = core::new_book_id();
    doc.revision = 0;
    // each linked file's copy — the bytes the book draws — in Links/, the copy's contents linked to it there
    Json links = Json::array();
    std::set<std::string> names;
    int relinked = 0;
    for (const std::string& id : core::smart::used_contents(doc)) {
        const auto found = doc.smart_contents.find(id);
        if (found == doc.smart_contents.end() || !found->second || !found->second->link) continue;
        const core::SmartContents& contents = *found->second;
        const core::Bytes bytes = render::smart::file_bytes(doc, contents);
        if (!bytes) throw core::Error("not_found", "the smart object's file is not in the book: " + contents.name);
        std::string file = core::path_to_utf8(core::path_from_utf8(core::py_str(core::py_get(*contents.link, "path", Json("")))).filename());
        if (file.empty()) file = contents.name.empty() ? id + contents.suffix : contents.name;
        if (names.contains(file)) file = std::to_string(names.size()) + "-" + file;  // (PhotoCraft's: two of one name)
        names.insert(file);
        const fs::path written = target / "Links" / core::path_from_utf8(file);
        storage::write_atomic(written, *bytes);
        doc.edit_contents(id).link =
            core::smart::link_json(core::path_to_utf8(fs::absolute(written).lexically_normal()), "Links/" + file, contents.asset.substr(7),
                                   static_cast<std::int64_t>(bytes->size()));
        links.push_back(core::path_to_utf8(written));
        ++relinked;
    }
    storage::SaveRequest request;
    request.actor = actor;
    request.base_revision = 0;
    request.ops = Json::array();
    const storage::SaveResult saved = storage::Saver(lock).save(doc, request);
    lock.release();
    return Json{{"ok", true}, {"book", core::path_to_utf8(target)}, {"links", links}, {"relinked", relinked}, {"revision", saved.revision}};
}

}  // namespace genko::formats::smart_files
