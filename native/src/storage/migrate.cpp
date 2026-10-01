#include "storage/migrate.hpp"

#include <QProcess>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/state.hpp"
#include "storage/writer.hpp"

#ifndef GENKO_FAULT_INJECTION
#define GENKO_FAULT_INJECTION 0
#endif

namespace genko::storage {

namespace fs = std::filesystem;
using core::Json;

namespace {

// Every file of a book but project.lock: its path ("/" between folders) → the sha256 of its bytes.
using Manifest = std::map<std::string, std::string>;

std::string generic_utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

Manifest manifest_of(const fs::path& root) {
    Manifest out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code type_ec;
        if (!it->is_regular_file(type_ec)) continue;
        const std::string rel = generic_utf8(it->path().lexically_relative(root));
        if (rel == "project.lock") continue;  // (who holds the lock is not the book's content)
        out.emplace(rel, sha256_file(it->path()));
    }
    if (ec) throw core::Error("io", os_error_text(ec.value(), root), path_to_utf8(root));
    return out;
}

bool inside(const fs::path& path, const fs::path& folder) {
    auto f = folder.begin();
    auto p = path.begin();
    for (; f != folder.end(); ++f, ++p) {
        if (p == path.end() || *p != *f) return false;
    }
    return true;
}

bool named_like_an_asset(const std::string& name) {
    if (name.size() < 66) return false;  // 64 hex digits and a suffix
    for (std::size_t i = 0; i < 64; ++i) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return AssetStore::is_suffix(std::string_view(name).substr(64));
}

const Json* get(const Json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

Json value_or_null(const Json& object, const char* key) {
    const Json* v = get(object, key);
    return v != nullptr ? *v : Json(nullptr);
}

core::ParseOptions project_text() {
    core::ParseOptions options;
    options.universal_newlines = true;
    return options;
}

// Tests only (builds with fault injection): a command run while the old book is being copied, after the copy and
// before the files are read again: GENKO_TEST_MIGRATE_HOOK='["program", "arg", …]' (its output is discarded).
void run_collect_hook() {
#if GENKO_FAULT_INJECTION
    const char* spec = std::getenv("GENKO_TEST_MIGRATE_HOOK");
    if (spec == nullptr || *spec == '\0') return;
    const Json command = core::parse_python_json(spec);
    if (!command.is_array() || command.empty()) {
        throw core::Error("value", "GENKO_TEST_MIGRATE_HOOK must be a JSON array: [program, arguments…]");
    }
    QStringList arguments;
    for (std::size_t i = 1; i < command.size(); ++i) arguments << QString::fromStdString(core::py_str(command[i]));
    QProcess process;
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(QString::fromStdString(core::py_str(command[0])), arguments);
    process.waitForFinished(-1);
#endif
}

void sync_tree(const fs::path& root) {
    std::vector<fs::path> folders{root};
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code type_ec;
        if (it->is_directory(type_ec)) folders.push_back(it->path());
    }
    for (auto it = folders.rbegin(); it != folders.rend(); ++it) sync_dir(*it);
}

std::string bytes_with_newline(std::string bytes) {
    if (!bytes.empty() && bytes.back() != '\n') bytes += '\n';
    return bytes;
}

// What the old journal's snapshots became: a v4 state, or why none could be made.
struct Snapshot {
    std::optional<std::string> state;
    std::string reason;
};

class Converter {
public:
    Converter(fs::path frozen, fs::path book, const LoadOptions& options, bool accept_repairs, std::string book_id)
        : frozen_(std::move(frozen)), book_(std::move(book)), old_store_(frozen_), new_store_(book_),
          options_(options), accept_repairs_(accept_repairs), book_id_(std::move(book_id)) {}

    AssetStore& store() { return new_store_; }
    const AssetStore& old_store() const { return old_store_; }

    // The v4 state of a document (written as an asset); its assets go to the new store.
    std::string state_of(core::Document doc) {
        doc.read_only_reason.clear();
        doc.book_id = book_id_;
        doc.features.clear();
        doc.revision = 1;
        const Json payload = project_payload_v4(doc, new_store_);
        return new_store_.put_bytes(state_text(payload), kStateSuffix);
    }

    void known(const std::string& old_ref, const std::string& state) { snapshots_[old_ref] = Snapshot{state, {}}; }

    Snapshot snapshot(const std::string& old_ref) {
        if (const auto it = snapshots_.find(old_ref); it != snapshots_.end()) return it->second;
        Snapshot out = convert_snapshot(old_ref);
        snapshots_[old_ref] = out;
        return out;
    }

    Json unavailable() const {
        Json out = Json::array();
        for (const auto& [ref, snapshot] : snapshots_) {
            if (!snapshot.state) out.push_back(Json::object({{"ref", ref}, {"reason", snapshot.reason}}));
        }
        return out;
    }

    std::size_t converted() const {
        return static_cast<std::size_t>(std::count_if(snapshots_.begin(), snapshots_.end(),
                                                      [](const auto& item) { return item.second.state.has_value(); }));
    }

private:
    Snapshot convert_snapshot(const std::string& old_ref) {
        if (!AssetStore::is_ref(old_ref)) return {std::nullopt, "not an asset ref"};
        const auto data = old_store_.get_bytes(old_ref, kSnapshotSuffix);
        if (!data) return {std::nullopt, "the snapshot is not in the old book's assets (Python's gc keeps the last 100)"};
        if (AssetStore::ref(*data) != old_ref) return {std::nullopt, "the snapshot does not hold the bytes its name says"};
        try {
            LoadResult loaded = load_document_text(*data, frozen_, options_);
            if (!loaded.report.clean() && !accept_repairs_) {
                return {std::nullopt, "needs repairs (convert with --accept-repairs to keep it): " + loaded.document.read_only_reason};
            }
            return {state_of(std::move(loaded.document)), {}};
        } catch (const core::Error& error) {
            return {std::nullopt, std::string("cannot be read: ") + error.what()};
        }
    }

    fs::path frozen_;
    fs::path book_;
    AssetStore old_store_;
    AssetStore new_store_;
    LoadOptions options_;
    bool accept_repairs_;
    std::string book_id_;
    std::map<std::string, Snapshot> snapshots_;
};

// The staging folder goes when the conversion ends, whether it worked or not (the new book has left it by then).
class Cleanup {
public:
    explicit Cleanup(fs::path path) : path_(std::move(path)) {}
    Cleanup(const Cleanup&) = delete;
    Cleanup& operator=(const Cleanup&) = delete;
    ~Cleanup() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

private:
    fs::path path_;
};

// An absolute, normal path without a trailing separator.
fs::path absolute_folder(const fs::path& path) {
    fs::path out = fs::weakly_canonical(fs::absolute(path)).lexically_normal();
    if (out.filename().empty() && out.has_parent_path()) out = out.parent_path();
    return out;
}

}  // namespace

ConvertError::ConvertError(std::string code, const std::string& message, Json report)
    : core::Error(std::move(code), message), report_(std::move(report)) {}

Json convert(const fs::path& src, const fs::path& dst, const std::string& actor, bool accept_repairs) {
    const auto started = std::chrono::steady_clock::now();
    std::error_code ec;

    // --- the old book and the new folder ------------------------------------------------------------------------
    if (!fs::is_directory(src, ec)) {
        if (!fs::exists(src, ec)) throw core::Error("not_found", os_error_text(ENOENT, src), path_to_utf8(src));
        throw core::Error("value", "not a book folder: " + core::py_repr_str(path_to_utf8(src)));
    }
    const int version = project_version(core::parse_python_json(read_file(src / "project.json"), nullptr, project_text()));
    if (version >= 4) {
        throw core::Error("not_legacy", path_to_utf8(src) + " is a version 4 book already: open it as it is");
    }
    const fs::path src_abs = absolute_folder(src);
    const fs::path dst_abs = absolute_folder(dst);
    if (inside(dst_abs, src_abs) || inside(src_abs, dst_abs)) {
        throw core::Error("value", "the new book must be a folder outside the old book (and not around it)");
    }
    bool replace_empty = false;
    if (fs::exists(dst_abs, ec)) {
        if (!fs::is_directory(dst_abs, ec) || !fs::is_empty(dst_abs, ec)) {
            throw core::Error("exists", path_to_utf8(dst) + " exists already: choose a new folder for the converted book");
        }
        replace_empty = true;
    }
    const fs::path parent = dst_abs.parent_path();
    make_dirs_durable(parent);
    const fs::path staging =
        parent / path_from_utf8("." + path_to_utf8(dst_abs.filename()) + ".migrating-" + core::new_id());
    fs::create_directory(staging, ec);
    if (ec) throw core::Error("io", os_error_text(ec.value(), staging), path_to_utf8(staging));
    const Cleanup cleanup(staging);

    Json report = Json::object();
    report["ok"] = false;
    report["source"] = Json::object({{"path", path_to_utf8(src)}, {"version", version}});
    report["destination"] = Json::object({{"path", path_to_utf8(dst)}});
    report["actor"] = actor;
    report["accept_repairs"] = accept_repairs;

    // --- 1, 2: the fixed copy -------------------------------------------------------------------------------------
    std::optional<ProjectLock> source_lock;
    if (fs::exists(src / "project.lock", ec)) {  // (no lock file: none is made)
        ProjectLock::Options options;
        options.create = false;
        options.write_content = false;
        source_lock.emplace(src, actor, options);
        try {
            source_lock->try_acquire();
        } catch (const LockedError& error) {
            throw ConvertError("source_locked",
                               "the old book is in use (" + std::string(error.what()) +
                                   "); nothing was converted: try again when no other program is writing it",
                               report);
        }
    }
    const fs::path frozen = staging / "source";
    Manifest first;
    Manifest second;
    std::vector<std::string> changed;
    try {
        first = manifest_of(src);
        for (const auto& [rel, sha] : first) {
            if (copy_file_hashed(src / path_from_utf8(rel), frozen / path_from_utf8(rel)) != sha) changed.push_back(rel);
        }
        run_collect_hook();
        second = manifest_of(src);
    } catch (const core::Error& error) {
        if (error.code() != "not_found") throw;
        changed.push_back(error.path().empty() ? std::string(error.what()) : error.path());  // (a file went away)
    }
    source_lock.reset();
    if (changed.empty()) {
        for (const auto& [rel, sha] : second) {
            const auto it = first.find(rel);
            if (it == first.end() || it->second != sha) changed.push_back(rel);
        }
        for (const auto& [rel, sha] : first) {
            if (!second.contains(rel)) changed.push_back(rel);
        }
    }
    if (!changed.empty()) {
        Json files = Json::array();
        for (std::size_t i = 0; i < changed.size() && i < 20; ++i) files.push_back(changed[i]);
        report["changed"] = files;
        throw ConvertError("source_changed",
                           "the old book changed while it was being read (" + changed.front() +
                               (changed.size() > 1 ? " and " + std::to_string(changed.size() - 1) + " more" : "") +
                               "); nothing was converted: try again when no other program is writing it",
                           report);
    }
    std::uintmax_t source_bytes = 0;
    for (const auto& [rel, sha] : first) {
        const auto size = fs::file_size(frozen / path_from_utf8(rel), ec);
        if (!ec) source_bytes += size;
    }
    report["source"]["files"] = static_cast<std::int64_t>(first.size());
    report["source"]["bytes"] = static_cast<std::int64_t>(source_bytes);

    // --- 3: the book --------------------------------------------------------------------------------------------
    auto cache = std::make_shared<LoadCache>();
    LoadOptions options;
    options.cache = cache;
    const std::string current_text = read_file(frozen / "project.json");
    LoadResult current = load_document_text(current_text, frozen, options);
    report["source"]["revision"] = current.document.revision;
    Json issues = Json::array();
    bool unknown_feature = false;
    for (const auto& issue : current.report.issues) {
        issues.push_back(issue.to_json());
        unknown_feature = unknown_feature || issue.kind == "unknown_feature";
    }
    report["issues"] = issues;
    if (unknown_feature) {
        throw ConvertError("unsupported", "the old book uses features this build does not know", report);
    }
    if (!current.report.clean() && !accept_repairs) {
        throw ConvertError("needs_repairs",
                           "the old book cannot be kept exactly as it is (" + std::to_string(issues.size()) +
                               " problem(s), listed in the report): nothing was converted; convert with --accept-repairs to "
                               "accept the changes",
                           report);
    }
    report["repairs_accepted"] = static_cast<std::int64_t>(accept_repairs ? issues.size() : 0);

    const fs::path book = staging / "book";
    const std::string book_id = core::new_book_id();
    Converter converter(frozen, book, options, accept_repairs, book_id);

    // 4: every asset of the old book, byte for byte
    std::int64_t imported = 0;
    std::uintmax_t imported_bytes = 0;
    Json skipped = Json::array();
    for (const fs::path& file : converter.old_store().all_files()) {
        const std::string name = path_to_utf8(file.filename());
        const std::string rel = generic_utf8(file.lexically_relative(frozen));
        if (!named_like_an_asset(name)) {
            skipped.push_back(Json::object({{"path", rel}, {"reason", "not named like an asset"}}));
            continue;
        }
        const fs::path target = book / file.lexically_relative(frozen);
        const std::string sha = copy_file_hashed(file, target);
        if (sha != name.substr(0, 64)) {
            fs::remove(target, ec);
            skipped.push_back(Json::object({{"path", rel}, {"reason", "its bytes do not hash to its name"}}));
            continue;
        }
        ++imported;
        const auto size = fs::file_size(target, ec);
        if (!ec) imported_bytes += size;
    }
    Json pages = Json::array();  // (v2: pictures under pages/NNN/)
    if (fs::is_directory(frozen / "pages", ec)) {
        for (auto it = fs::recursive_directory_iterator(frozen / "pages", ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            std::error_code type_ec;
            if (!it->is_regular_file(type_ec)) continue;
            const std::string rel = generic_utf8(it->path().lexically_relative(frozen));
            if (it->path().extension() != ".png") {
                skipped.push_back(Json::object({{"path", rel}, {"reason", "not a .png picture"}}));
                continue;
            }
            const std::string ref = converter.store().put_bytes(read_file(it->path()), ".png");
            pages.push_back(Json::object({{"path", rel}, {"ref", ref}}));
        }
    }

    // 3: the book itself
    core::Document doc = std::move(current.document);
    doc.read_only_reason.clear();
    doc.book_id = book_id;
    doc.features.clear();
    doc.revision = 1;
    const Json payload = project_payload_v4(doc, converter.store());
    const std::string text = core::dump_python_indent2(payload);
    const std::string state = converter.store().put_bytes(state_text(payload), kStateSuffix);
    const std::string source_ref = AssetStore::ref(current_text);
    std::string lf_text = current_text;
    for (std::size_t at = lf_text.find("\r\n"); at != std::string::npos; at = lf_text.find("\r\n", at)) lf_text.erase(at, 1);
    const std::string source_ref_lf = AssetStore::ref(lf_text);
    converter.store().put_known(source_ref, current_text, kSnapshotSuffix);
    converter.known(source_ref, state);
    converter.known(source_ref_lf, state);

    // 4, 5: the old history
    const fs::path legacy = book / "legacy";
    Json legacy_files = Json::array();
    const auto keep_legacy = [&](const fs::path& from, const char* name) {
        if (!fs::is_regular_file(from, ec)) return;
        copy_file_hashed(from, legacy / name);
        legacy_files.push_back(std::string("legacy/") + name);
    };
    keep_legacy(frozen / "studio" / "journal.jsonl", "journal.jsonl");
    keep_legacy(frozen / "studio" / "audit.jsonl", "audit.jsonl");
    keep_legacy(frozen / "project.v2.bak.json", "project.v2.bak.json");

    const journal::Lines old_journal = journal::read_lines(frozen / "studio" / "journal.jsonl");
    Json entries = Json::array();
    bool consistent = true;
    std::string note;
    if (!old_journal.values.empty()) {
        const Json* last_after = get(old_journal.values.back(), "after");
        consistent = last_after != nullptr && last_after->is_string() &&
                     (last_after->get_ref<const std::string&>() == source_ref ||
                      last_after->get_ref<const std::string&>() == source_ref_lf);
        if (!consistent) {
            note = "the old journal's last state is not the old project.json (it was changed outside the journal): the "
                   "old history is kept in legacy/ but cannot be undone into";
        }
    }
    if (consistent) {
        for (const Json& line : old_journal.values) {
            const Json* kind = get(line, "kind");
            Json entry = Json::object();
            entry["old_rev"] = value_or_null(line, "rev");
            entry["actor"] = value_or_null(line, "actor");
            entry["at"] = value_or_null(line, "at");
            entry["kind"] = kind != nullptr ? *kind : Json("commit");
            entry["before_old"] = value_or_null(line, "before");
            entry["after_old"] = value_or_null(line, "after");
            for (const char* side : {"before", "after"}) {
                const Json* old = get(line, side);
                if (old == nullptr || !old->is_string()) {
                    entry[side] = nullptr;
                    continue;
                }
                const Snapshot snapshot = converter.snapshot(old->get<std::string>());
                entry[side] = snapshot.state ? Json(*snapshot.state) : Json(nullptr);
            }
            entries.push_back(std::move(entry));
        }
    }
    Json map = Json::object();
    map["v"] = 4;
    map["source_version"] = version;
    map["boundary_rev"] = 1;
    map["source_project"] = source_ref;
    map["entries"] = entries;
    write_new_file(legacy / "map.json", core::dump_python_indent2(map) + "\n");
    legacy_files.push_back("legacy/map.json");
    Json manifest_files = Json::object();
    for (const auto& [rel, sha] : first) manifest_files[rel] = sha;
    write_new_file(legacy / "source-manifest.json",
                   core::dump_python_indent2(Json::object({{"source", path_to_utf8(src_abs)}, {"files", manifest_files}})) + "\n");
    legacy_files.push_back("legacy/source-manifest.json");

    // 7, 6: the journal's first transaction, project.json, the audit
    const std::string txn = core::new_txn_id();
    const double at = journal::now_seconds();
    Json prepare = Json::object();
    prepare["v"] = 4;
    prepare["kind"] = "prepare";
    prepare["txn"] = txn;
    prepare["action"] = "migrate";
    prepare["rev"] = 1;
    prepare["base_rev"] = 0;
    prepare["actor"] = actor;
    prepare["at"] = at;
    prepare["before"] = nullptr;
    prepare["after"] = state;
    prepare["project_sha256"] = sha256_hex(text);
    journal::append(journal::journal_file(book), prepare);
    write_atomic(book / "project.json", text);
    std::string audit;
    std::int64_t audit_lines = 0;
    if (fs::is_regular_file(frozen / "studio" / "audit.jsonl", ec)) {
        audit = bytes_with_newline(read_file(frozen / "studio" / "audit.jsonl"));
        audit_lines = static_cast<std::int64_t>(std::count(audit.begin(), audit.end(), '\n'));
    }
    Json migrated = Json::object();
    migrated["v"] = 4;
    migrated["kind"] = "migrated";
    migrated["txn"] = txn;
    migrated["from_version"] = version;
    migrated["at"] = at;
    migrated["actor"] = actor;
    write_new_file(journal::audit_file(book), audit + journal::line_text(migrated));
    journal::append(journal::journal_file(book), journal::commit_line(txn, 1));

    // the other files of the old book, as they are
    Json carried = Json::array();
    const std::set<std::string> handled{"project.json", "project.v2.bak.json", "studio/journal.jsonl", "studio/audit.jsonl"};
    for (const auto& [rel, sha] : first) {
        if (handled.contains(rel) || rel.starts_with("assets/") || rel.starts_with("pages/")) continue;
        if (rel.starts_with("legacy/") || rel.starts_with("studio/journal.partial") || rel.starts_with("studio/audit.partial")) {
            skipped.push_back(Json::object({{"path", rel}, {"reason", "the converted book keeps its own " + rel}}));
            continue;
        }
        copy_file_hashed(frozen / path_from_utf8(rel), book / path_from_utf8(rel));
        carried.push_back(rel);
    }
    for (auto it = fs::recursive_directory_iterator(frozen, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {  // (empty folders too: studio/drafts/ tells an agent's book)
        std::error_code type_ec;
        if (!it->is_directory(type_ec)) continue;
        const std::string rel = generic_utf8(it->path().lexically_relative(frozen));
        if (rel == "assets" || rel.starts_with("assets/") || rel == "pages" || rel.starts_with("pages/") ||
            rel == "legacy" || rel.starts_with("legacy/")) {
            continue;
        }
        fs::create_directories(book / path_from_utf8(rel), type_ec);
    }

    // 8: the report
    Json fonts = Json::object();
    fonts["font_path"] = doc.font_path;
    fonts["font_path_found"] = doc.font_path.empty() ? Json(nullptr) : Json(fs::is_regular_file(path_from_utf8(doc.font_path), ec));
    std::set<std::string> named;
    for (const auto& line : doc.story) {
        if (const Json* font = get(line.style, "font"); font != nullptr && font->is_string()) named.insert(font->get<std::string>());
    }
    Json named_list = Json::array();
    for (const auto& font : named) named_list.push_back(font);
    fonts["line_fonts"] = named_list;
    fonts["line_fonts_checked"] = false;  // (the text module, M4, finds fonts by name)

    report["ok"] = true;
    report["destination"]["book_id"] = book_id;
    report["destination"]["revision"] = 1;
    report["assets"] = Json::object({{"imported", imported},
                                     {"bytes", static_cast<std::int64_t>(imported_bytes)},
                                     {"pages", pages},
                                     {"skipped", skipped}});
    Json history = Json::object();
    history["journal_lines"] = static_cast<std::int64_t>(old_journal.values.size());
    Json unreadable = Json::array();
    for (const std::size_t number : old_journal.unreadable) unreadable.push_back(static_cast<std::int64_t>(number));
    history["unreadable_lines"] = unreadable;
    history["cut_tail_bytes"] = static_cast<std::int64_t>(old_journal.tail.size());
    history["consistent"] = consistent;
    history["note"] = note.empty() ? Json(nullptr) : Json(note);
    history["snapshots"] = Json::object({{"converted", static_cast<std::int64_t>(converter.converted())},
                                         {"unavailable", converter.unavailable()}});
    report["history"] = history;
    report["map_entries"] = static_cast<std::int64_t>(entries.size());
    report["audit"] = Json::object({{"old_lines", audit_lines}, {"migrated_line", true}});
    report["legacy"] = legacy_files;
    report["carried"] = carried;
    report["fonts"] = fonts;
    report["elapsed_s"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    write_new_file(legacy / "report.json", core::dump_python_indent2(report) + "\n");

    // publish: the new book appears whole, in one rename
    sync_tree(book);
    if (replace_empty) fs::remove(dst_abs, ec);
    rename_new(book, dst_abs);
    return report;
}

}  // namespace genko::storage
