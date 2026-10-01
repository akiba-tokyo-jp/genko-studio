#include "storage/doctor.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <system_error>

#include "core/pyconv.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
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

std::string text_of(const Json* value) { return value != nullptr ? core::py_str(*value) : std::string("None"); }

std::size_t code_points(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

// Python's AssetStore.relpath for any ref (the text only: a ref that is not one is never looked up).
std::string python_relpath(const std::string& ref, std::string_view suffix) {
    const auto colon = ref.find(':');
    const std::string digest = colon == std::string::npos ? ref : ref.substr(colon + 1);
    return "assets/" + digest.substr(0, 2) + "/" + digest + std::string(suffix);
}

void check_asset(const AssetStore& store, const Json* ref, std::string_view suffix, const std::string& where,
                 Json& problems) {
    if (ref == nullptr || !core::py_truthy(*ref)) return;
    const std::string text = core::py_str(*ref);
    if (AssetStore::is_ref(text) && store.has(text, suffix)) return;
    problems.push_back(where + ": missing " + python_relpath(text, suffix));
}

}  // namespace

Json doctor(const fs::path& dir) {
    std::error_code ec;
    fs::path project = fs::weakly_canonical(fs::absolute(dir, ec), ec);
    if (project.empty()) project = dir;
    core::ParseOptions parse;
    parse.universal_newlines = true;
    const Json payload = core::parse_python_json(read_file(project / "project.json"), nullptr, parse);
    if (!payload.is_object()) throw core::Error("format", "project.json must hold an object, not " + core::py_repr(payload));
    const int version = project_version(payload);
    const AssetStore store(project);
    Json problems = Json::array();

    // Python's checks
    if (const Json* pages = get(payload, "pages"); pages != nullptr && pages->is_array()) {
        for (const Json& page : *pages) {
            const Json* layers = get(page, "layers");
            if (layers == nullptr || !layers->is_array()) continue;
            for (const Json& layer : *layers) {
                const std::string where = "page " + text_of(get(page, "index")) + " layer " + text_of(get(layer, "id"));
                check_asset(store, get(layer, "asset"), ".png", where, problems);
                check_asset(store, get(layer, "strokes_blob"), ".strokes.json", where, problems);
                if (version < 4) continue;
                if (const Json* mask = get(layer, "mask")) check_asset(store, get(*mask, "asset"), ".png", where + " mask", problems);
                if (const Json* patches = get(layer, "patches"); patches != nullptr && patches->is_array()) {
                    for (const Json& patch : *patches) {
                        check_asset(store, get(patch, "asset"), ".png", where + " patch " + text_of(get(patch, "id")), problems);
                    }
                }
            }
        }
    }
    const std::size_t length = code_points(path_to_utf8(project));
    if (length > 150) {
        problems.push_back("project path is " + std::to_string(length) + " characters; Windows may refuse deep asset paths");
    }

    Json out = Json::object();
    out["ok"] = false;
    out["version"] = get(payload, "version") != nullptr ? *get(payload, "version") : Json(nullptr);
    out["revision"] = get(payload, "revision") != nullptr ? *get(payload, "revision") : Json(nullptr);
    out["problems"] = Json::array();
    out["checks_skipped"] = Json::array({"bundled font (the text module brings the fonts: M4)"});
    if (version < 4) {
        out["needs_migration"] = true;
        out["problems"] = std::move(problems);
        out["ok"] = out["problems"].empty();
        return out;
    }

    // what the reader reports (features this build does not know, values it had to change, …)
    const LoadResult loaded = load_document(project);
    Json issues = Json::array();
    for (const auto& issue : loaded.report.issues) {
        issues.push_back(issue.to_json());
        if (issue.kind == "unknown_feature") problems.push_back(issue.message + " (the book opens read-only)");
        else if (issue.kind != "missing_asset") problems.push_back(issue.kind + " at " + (issue.pointer.empty() ? "/" : issue.pointer) + ": " + issue.message);
    }
    Json features = Json::array();
    for (const auto& feature : loaded.document.features) features.push_back(feature);

    // the journal
    const journal::Lines lines = journal::read_lines(journal::journal_file(project));
    if (!lines.tail.empty()) {
        problems.push_back("studio/journal.jsonl ends with a cut line (" + std::to_string(lines.tail.size()) +
                           " bytes): the next write keeps it in studio/journal.partial and cuts it off");
    }
    for (const std::size_t number : lines.unreadable) {
        problems.push_back("studio/journal.jsonl line " + std::to_string(number) + " is not a JSON object");
    }
    const journal::Repair plan = journal::plan_repair(project);
    for (const auto& txn : plan.committed) {
        problems.push_back("transaction " + txn + " is unfinished; project.json holds its new state, so the next write commits it");
    }
    for (const auto& txn : plan.aborted) {
        problems.push_back("transaction " + txn + " is unfinished; project.json does not hold its state, so the next write aborts it");
    }
    for (const auto& txn : plan.audited) {
        if (std::find(plan.committed.begin(), plan.committed.end(), txn) != plan.committed.end()) continue;
        problems.push_back("the approval changes of transaction " + txn + " are missing from studio/audit.jsonl (the next write adds them)");
    }
    const auto txns = journal::transactions(lines);
    const journal::Transaction* last_committed = nullptr;
    for (const auto& t : txns) {
        if (t.status == journal::Transaction::Status::committed) last_committed = &t;
    }
    if (last_committed != nullptr && plan.committed.empty()) {
        const auto after = get(last_committed->prepare, "after");
        const std::string disk_state = state_ref(payload);
        if (after == nullptr || !after->is_string() || after->get<std::string>() != disk_state) {
            problems.push_back("project.json is not the state of the last committed transaction (" + last_committed->txn +
                               "): it changed outside the journal");
        }
    }
    const journal::Stacks stacks = journal::stacks(project);
    for (const auto& problem : stacks.problems) problems.push_back("history: " + problem);
    std::size_t missing_states = 0;
    for (const auto* stack : {&stacks.undo, &stacks.redo}) {
        for (const auto& item : *stack) {
            for (const auto* ref : {&item.before, &item.after}) {
                if (*ref && !store.has(**ref, kStateSuffix)) ++missing_states;
            }
        }
    }
    if (missing_states > 0) {
        problems.push_back(std::to_string(missing_states) + " state(s) of the Undo/Redo history are missing from assets/");
    }

    // the audit
    const journal::Lines audit = journal::read_lines(journal::audit_file(project));
    if (!audit.tail.empty()) {
        problems.push_back("studio/audit.jsonl ends with a cut line (" + std::to_string(audit.tail.size()) +
                           " bytes): the next write keeps it in studio/audit.partial");
    }
    std::map<std::string, int> per_txn;
    for (const Json& line : audit.values) {
        if (!journal::is_v4(line)) continue;
        const Json* txn = get(line, "txn");
        if (txn != nullptr && txn->is_string()) ++per_txn[txn->get<std::string>()];
    }
    for (const auto& [txn, count] : per_txn) {
        if (count > 1) problems.push_back("studio/audit.jsonl records transaction " + txn + " " + std::to_string(count) + " times");
    }

    Json journal_report = Json::object();
    journal_report["lines"] = static_cast<std::int64_t>(lines.values.size());
    journal_report["unreadable"] = static_cast<std::int64_t>(lines.unreadable.size());
    journal_report["tail_bytes"] = static_cast<std::int64_t>(lines.tail.size());
    journal_report["transactions"] = static_cast<std::int64_t>(txns.size());
    journal_report["undo"] = static_cast<std::int64_t>(stacks.undo.size());
    journal_report["redo"] = static_cast<std::int64_t>(stacks.redo.size());
    journal_report["repair"] = plan.to_json();

    out["problems"] = std::move(problems);
    out["ok"] = out["problems"].empty();
    out["features"] = std::move(features);
    out["read_only"] = loaded.document.read_only_reason.empty() ? Json(nullptr) : Json(loaded.document.read_only_reason);
    out["issues"] = std::move(issues);
    out["journal"] = std::move(journal_report);
    return out;
}

}  // namespace genko::storage
