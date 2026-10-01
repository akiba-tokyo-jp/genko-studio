#include "storage/gc.hpp"

#include <chrono>
#include <system_error>
#include <vector>

#include "core/error.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/state.hpp"
#include "storage/transaction.hpp"

namespace genko::storage {

namespace fs = std::filesystem;
using core::Json;

namespace {

const Json* get(const Json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

void add_ref(const Json* value, std::set<std::string>& refs, std::set<std::string>* contents = nullptr) {
    if (value == nullptr || !value->is_string() || !AssetStore::is_ref(value->get_ref<const std::string&>())) return;
    refs.insert(value->get<std::string>());
    if (contents != nullptr) contents->insert(value->get<std::string>());
}

void add_ref(const std::optional<std::string>& value, std::set<std::string>& refs, std::set<std::string>& contents) {
    if (!value || !AssetStore::is_ref(*value)) return;
    refs.insert(*value);
    contents.insert(*value);
}

core::ParseOptions text_file() {
    core::ParseOptions options;
    options.bytes = true;
    options.universal_newlines = true;
    return options;
}

// Every ref inside the assets named (states or old snapshots), when they can be read.
void add_contents(const AssetStore& store, const std::set<std::string>& names, std::string_view suffix,
                  std::set<std::string>& refs) {
    for (const std::string& ref : names) {
        try {
            const auto data = store.get_bytes(ref, suffix);
            if (data) refs_in(core::parse_python_json(*data, nullptr, text_file()), refs);
        } catch (const core::Error&) {
            // (an unreadable snapshot refers to nothing that can be told)
        }
    }
}

bool named_like_an_asset(const std::string& name) {
    if (name.size() < 64) return false;
    for (std::size_t i = 0; i < 64; ++i) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return name.size() == 64 || name[64] == '.';
}

}  // namespace

void refs_in(const Json& value, std::set<std::string>& out) {
    switch (value.type()) {
        case Json::value_t::object:
            for (const auto& [key, item] : value.items()) refs_in(item, out);
            return;
        case Json::value_t::array:
            for (const auto& item : value) refs_in(item, out);
            return;
        case Json::value_t::string: {
            const auto& text = value.get_ref<const std::string&>();
            if (AssetStore::is_ref(text)) out.insert(text);
            return;
        }
        default: return;
    }
}

std::set<std::string> referenced_assets(const fs::path& dir) {
    std::set<std::string> refs;
    std::set<std::string> states;     // .state.json assets whose contents are kept too
    std::set<std::string> snapshots;  // .project.json (v3 snapshots)
    const AssetStore store(dir);
    std::error_code ec;

    if (fs::is_regular_file(dir / "project.json", ec)) {
        core::ParseOptions parse;
        parse.universal_newlines = true;
        refs_in(core::parse_python_json(read_file(dir / "project.json"), nullptr, parse), refs);
    }

    // the journal: the last 100 commits, the unfinished transactions, the Undo and Redo stacks
    const journal::Lines lines = journal::read_lines(journal::journal_file(dir));
    const auto txns = journal::transactions(lines);
    std::vector<const journal::Transaction*> committed;
    for (const auto& t : txns) {
        if (t.status == journal::Transaction::Status::committed) committed.push_back(&t);
        if (t.status == journal::Transaction::Status::pending) {
            add_ref(get(t.prepare, "before"), refs, &states);
            add_ref(get(t.prepare, "after"), refs, &states);
            add_ref(get(t.prepare, "ops_asset"), refs);
        }
    }
    const std::size_t first = committed.size() > kKeepCommits ? committed.size() - kKeepCommits : 0;
    for (std::size_t i = first; i < committed.size(); ++i) {
        add_ref(get(committed[i]->prepare, "before"), refs, &states);
        add_ref(get(committed[i]->prepare, "after"), refs, &states);
        add_ref(get(committed[i]->prepare, "ops_asset"), refs);
    }
    const journal::Stacks stacks = journal::stacks(dir);
    for (const auto* stack : {&stacks.undo, &stacks.redo}) {
        for (const auto& item : *stack) {
            add_ref(item.before, refs, states);
            add_ref(item.after, refs, states);
            if (!item.before_old.empty()) add_ref(std::optional<std::string>(item.before_old), refs, snapshots);
            if (!item.after_old.empty()) add_ref(std::optional<std::string>(item.after_old), refs, snapshots);
        }
    }
    // (lines of an older format in the journal: Python's rule, the last 100)
    std::vector<const Json*> old_lines;
    for (const Json& line : lines.values) {
        if (!journal::is_v4(line)) old_lines.push_back(&line);
    }
    for (std::size_t i = old_lines.size() > kKeepCommits ? old_lines.size() - kKeepCommits : 0; i < old_lines.size(); ++i) {
        add_ref(get(*old_lines[i], "before"), refs, &snapshots);
        add_ref(get(*old_lines[i], "after"), refs, &snapshots);
        add_ref(get(*old_lines[i], "ops_asset"), refs);
    }

    // the converted history, whole
    if (fs::is_regular_file(journal::map_file(dir), ec)) {
        try {
            const Json map = core::parse_python_json(read_file(journal::map_file(dir)));
            add_ref(get(map, "source_project"), refs, &snapshots);
            if (const Json* entries = get(map, "entries"); entries != nullptr && entries->is_array()) {
                for (const Json& entry : *entries) {
                    add_ref(get(entry, "before"), refs, &states);
                    add_ref(get(entry, "after"), refs, &states);
                    add_ref(get(entry, "before_old"), refs, &snapshots);
                    add_ref(get(entry, "after_old"), refs, &snapshots);
                }
            }
            refs_in(map, refs);
        } catch (const core::Error&) {
            // (doctor reports an unreadable map; every ref the old journal names is kept below all the same)
        }
    }
    for (const Json& line : journal::read_lines(dir / "legacy" / "journal.jsonl").values) {
        add_ref(get(line, "before"), refs, &snapshots);
        add_ref(get(line, "after"), refs, &snapshots);
        add_ref(get(line, "ops_asset"), refs);
    }

    // recovery points
    const fs::path autosave = dir / "studio" / "autosave";
    if (fs::is_directory(autosave, ec)) {
        for (auto it = fs::recursive_directory_iterator(autosave, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            std::error_code type_ec;
            if (!it->is_regular_file(type_ec)) continue;
            try {
                refs_in(core::parse_python_json(read_file(it->path()), nullptr, text_file()), refs);
            } catch (const core::Error&) {
                // (not JSON: a recovery point keeps its assets by naming them in JSON)
            }
        }
    }

    add_contents(store, states, kStateSuffix, refs);
    add_contents(store, snapshots, kSnapshotSuffix, refs);
    return refs;
}

Json gc(const fs::path& dir, bool dry_run, const std::string& agent, bool legacy_pages) {
    ProjectLock lock(dir, agent);
    lock.try_acquire();
    const DiskState disk = read_disk_state(dir);
    if (disk.exists && disk.version < 4) throw NeedsMigration(dir, disk.version);
    if (!dry_run) journal::repair(dir);  // (a write: the journal is brought to its consistent point first)
    const std::set<std::string> keep = referenced_assets(dir);
    const AssetStore store(dir);
    const auto now = fs::file_time_type::clock::now();
    const auto young = std::chrono::duration_cast<fs::file_time_type::duration>(std::chrono::duration<double>(kKeepNewAssetsSeconds));
    Json removed = Json::array();
    for (const fs::path& file : store.all_files()) {
        const std::string name = path_to_utf8(file.filename());
        if (!named_like_an_asset(name)) continue;
        if (keep.contains("sha256:" + name.substr(0, 64))) continue;
        std::error_code ec;
        const auto written = fs::last_write_time(file, ec);
        if (ec || now - written < young) continue;
        removed.push_back(path_to_utf8(file.lexically_relative(dir)));
        if (!dry_run) {
            fs::remove(file, ec);
            if (ec) throw core::Error("io", os_error_text(ec.value(), file), path_to_utf8(file));
        }
    }
    bool legacy_removed = false;
    std::error_code ec;
    if (legacy_pages && fs::is_directory(dir / "pages", ec) && disk.exists && disk.version >= 3) {
        legacy_removed = true;
        if (!dry_run) fs::remove_all(dir / "pages", ec);
    }
    Json out = Json::object();
    out["ok"] = true;
    out["dry_run"] = dry_run;
    out["removed"] = std::move(removed);
    out["legacy_pages_removed"] = legacy_removed;
    return out;
}

}  // namespace genko::storage
