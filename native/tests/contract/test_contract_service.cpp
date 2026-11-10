// The studio service (M5②: Python's genko/studio/service.py StudioService and HumanService, adopt.adopt_drafts,
// evaluate's stats, audit and tests, and `genko studio …` of genko/studio/cli.py) against Python's: the jobs of
// contract/service_cases.json (tools/migration/make_service_cases.py: the demo book of tests/fixtures/studio/demo4 written
// as an agent writes one, with a person's approvals and their refusals to an agent, chat approvals, tickets, proposals,
// Undo, character sheets, stats and the audit; the style catalog read from a folder; M0's drafts adopted; art requests,
// previews, imports, candidates, adoption with Genko's and a person's enlargers, regions, the finishing pass, lines
// drawn from a picture, the checks, exports into the book, proofs, background jobs, review.html and the blind character
// test; a one-page book taken to a person's final export; hand-drawn names analysed and accepted; the command line
// itself; palette pictures written as Pillow writes them), each step run here and by tools/migration/service_harness.py, each side on its own root and config folder,
// ids counted from 1 and the clock fixed on both. Each step's record must be the same: the reply (or the exception
// raised: its type and words), the previews' bytes (sha256), every book under the root after it (project.json as
// Python's v3 writer writes it, without its revision; the approval audit's {rev, actor, changes, via}) and every other
// file under the root and the config folder (sha256; not the journal, the audit, the tool log, the lock, stored
// snapshots or studio/jobs/, which hold this build's v4 files, times and process ids; what the exports wrote compared
// at the end of the job as export_files.hpp compares them, without the times their sRGB profiles carry). A background job's id and its
// time are written <job> and <seconds>, an audit entry's time <at>, and a Python object's address in a message is left
// out; a book an Undo restored is compared with its keys in any order and without the revisions it records
// (schema-v4.md: state assets are canonical JSON, and the revision counts on through an Undo), and a book whose several
// character sheets were asked for in one save (SPEC COMP-01a: Python saves each) without the revisions it records.
// The command line's steps run `genko studio …` in this process as the harness runs Python's (genko::api::studio_command
// in the root, the step's stdin given, its stdout read; the ids it makes counted as every other step's) and compare its
// stdout and exit code, then the books and files as every step does; the same job is run once more with each step a
// process of its own (this build's executable), its stdout and exit code compared with its random ids written <id>,
// and every step's ending checked (no crash; exit 0, 1 or 3, or 2 where Python's argparse says 2); Python's tracebacks are this build's {"ok": false,
// "error": "<Type>: …", "code": "python_error"}, and this build's failures carry "code" (ARCHITECTURE.md §8). A work
// item's lease (studio/claims/<item>.json) is compared with its "expires" written <expires>; this build's is checked to
// be the time it was made plus the lease. No file or folder this build makes under the root has a ":" in its name (the
// fixed clock is a time, formatted as each name asks: Windows takes no ":" in a name). confine=False, which only
// Python's command line passes (keyword-only), is this build's StudioService::set_confine.

#include <QtTest>

#include <cstdio>
#include <iostream>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "api/studio_cli.hpp"
#include "core/base64.hpp"
#include "core/clock.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/args.hpp"
#include "service/evaluate.hpp"
#include "service/human_service.hpp"
#include "service/studio_service.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/reader.hpp"
#include "storage/writer.hpp"
#include "export_files.hpp"
#include "test3d.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace fs = std::filesystem;

namespace {

genko::test::Run service_harness(const QStringList& args, const QString& scratch, int timeout_ms = 1800000) {
    QStringList full{genko::test::repo_root() + QStringLiteral("/tools/migration/service_harness.py")};
    full += args;
    return genko::test::run(genko::test::python_ref(), full, genko::test::python_env(scratch), timeout_ms);
}

std::string text_of(const fs::path& path) { return genko::storage::path_to_utf8(path); }

// --- placeholders (make_service_cases.py) ---------------------------------------------------------------------------------

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    return genko::test::replace_all(std::move(text), from, to);
}

Json resolved(const Json& value, const fs::path& root) {
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) out[key] = resolved(item, root);
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) out.push_back(resolved(item, root));
        return out;
    }
    if (!value.is_string()) return value;
    const std::string text = value.get<std::string>();
    const std::size_t colon = text.find(':');
    const std::string kind = text.substr(0, colon);
    if (kind == "$job") {
        const std::size_t second = text.find(':', colon + 1);
        const fs::path folder = root / text.substr(colon + 1, second - colon - 1) / "studio" / "jobs";
        std::vector<std::pair<double, std::string>> jobs;  // (in the order they started)
        for (const auto& entry : fs::directory_iterator(folder)) {
            const std::string file = text_of(entry.path().filename());
            if (!file.starts_with("job_") || !file.ends_with(".json")) continue;
            const Json record = genko::core::parse_python_json(genko::storage::read_file(entry.path()));
            jobs.emplace_back(record.at("started").get<double>(), file.substr(0, file.size() - 5));
        }
        std::sort(jobs.begin(), jobs.end());
        return jobs.at(static_cast<std::size_t>(std::stoll(text.substr(second + 1)))).second;
    }
    if (kind == "$ticket" || kind == "$frame" || kind == "$candidate" || kind == "$panelcand" || kind == "$request" || kind == "$proposal") {
        std::vector<std::string> parts;
        std::size_t start = colon + 1;
        for (;;) {
            const std::size_t next = text.find(':', start);
            parts.push_back(text.substr(start, next == std::string::npos ? std::string::npos : next - start));
            if (next == std::string::npos) break;
            start = next + 1;
        }
        const genko::core::Document doc = genko::storage::load_document(root / parts[0]).document;
        const auto index = [](const Json& list, const std::string& n) { return genko::core::subscript(list, std::stoll(n)); };
        if (kind == "$ticket") return index(doc.tickets, parts[1])["id"];
        if (kind == "$frame" || kind == "$panelcand") {
            for (const auto& page : doc.pages) {
                if (page->index == genko::core::Num(std::stoll(parts[1]))) {
                    const auto leaves = page->leaf_frames();
                    const genko::core::Frame* frame = leaves.at(static_cast<std::size_t>(std::stoll(parts[2])));
                    if (kind == "$frame") return frame->id;
                    return index(frame->panel.value().at("candidates"), parts[3])["id"];
                }
            }
            throw std::runtime_error("no page");
        }
        if (kind == "$request" || kind == "$proposal") {
            const Json& all = doc.studio.at(kind == "$request" ? "requests" : "proposals");
            std::size_t n = 0;
            for (const auto& [key, entry] : all.items()) {
                (void)entry;
                if (n++ == static_cast<std::size_t>(std::stoll(parts[1]))) return key;
            }
            throw std::runtime_error("no such entry");
        }
        return index(doc.studio.at("character_candidates").at(parts[1]), parts[2])["id"];
    }
    return replace_all(text, "$root", text_of(root));
}

// --- this build's side of the harness's records ------------------------------------------------------------------------

// type(exc).__name__ and str(exc) of what Python raises where this build throws
Json raised(const std::exception& error) {
    std::string type = "Exception";
    if (dynamic_cast<const genko::core::ApplyError*>(&error) != nullptr) {
        type = "ApplyError";
    } else if (dynamic_cast<const genko::core::PyTypeError*>(&error) != nullptr) {
        type = "TypeError";
    } else if (dynamic_cast<const genko::core::PyValueError*>(&error) != nullptr) {
        type = "ValueError";
    } else if (const auto* uncaught = dynamic_cast<const genko::core::PyUncaught*>(&error)) {
        type = uncaught->type().substr(uncaught->type().rfind('.') == std::string::npos ? 0 : uncaught->type().rfind('.') + 1);
    } else if (const auto* e = dynamic_cast<const genko::core::Error*>(&error)) {
        type = e->code() == "not_found" ? "FileNotFoundError" : e->code() == "json" ? "JSONDecodeError" : "Error(" + e->code() + ")";
    }
    return Json::object({{"raised", type}, {"message", error.what()}});
}

std::string sha256(const std::string& bytes) {
    return QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256).toHex().toStdString();
}

bool skipped(const std::string& rel) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = rel.find('/', start);
        parts.push_back(rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    const std::string& name = parts.back();
    const std::string tail = parts.size() >= 2 ? parts[parts.size() - 2] + "/" + name : name;
    const auto has = [&parts](const char* p) { return std::find(parts.begin(), parts.end(), p) != parts.end(); };
    const bool job = parts.size() >= 3 && parts[parts.size() - 3] == "studio" && parts[parts.size() - 2] == "jobs";
    return name == "project.json" || name == "project.lock" || (name.size() > 4 && name.ends_with(".tmp")) || tail == "studio/journal.jsonl" ||
           tail == "studio/journal.partial" || tail == "studio/audit.jsonl" || tail == "studio/audit.partial" || (has("studio") && has("logs")) ||
           has("legacy") || (has("assets") && !name.ends_with(".png")) || job;
}

// A file an export wrote (in a folder named exports, or under the root's out/): compared at the end of the job, without
// the times the sRGB profile some of them carry holds (export_files.hpp); listed in each step's record.
bool exported(const std::string& rel) {
    const std::size_t last = rel.rfind('/');
    if (last == std::string::npos) return false;
    if (rel.rfind("out/", 0) == 0) return true;
    const std::string folders = "/" + rel.substr(0, last) + "/";
    return folders.find("/exports/") != std::string::npos;
}

// A work item's lease (claims.py): studio/claims/<item>.json.
bool claim_file(const std::string& rel) {
    const std::size_t at = rel.rfind("studio/claims/");
    return at != std::string::npos && (at == 0 || rel[at - 1] == '/') && rel.ends_with(".json") &&
           rel.find('/', at + 14) == std::string::npos;
}

// A lease with its time ("expires": the time it was taken plus the lease) written <expires>, as the harness writes it.
std::string without_expiry(std::string data) {
    static const QRegularExpression expires(QStringLiteral(R"("expires": -?[0-9][0-9.eE+-]*)"));
    return QString::fromStdString(data).replace(expires, QStringLiteral("\"expires\": \"<expires>\"")).toStdString();
}

double now_seconds() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// The leases this build took or renewed between `started` and `ended` hold the item for claims.LEASE_SECONDS (600) from
// then: "" or what is wrong.
std::string leases_problem(const fs::path& root, double started, double ended) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string rel = text_of(it->path().lexically_relative(root));
        if (!it->is_regular_file() || !claim_file(rel)) continue;
        const Json lease = genko::core::parse_python_json(genko::storage::read_file(it->path()));
        const double expires = lease.at("expires").get<double>();
        if (expires >= started + 600 && expires > ended + 600 + 0.001) return rel + ": expires " + std::to_string(expires) + ", after the lease";
    }
    return {};
}

// The names under the root with a ":" (none may have one: Windows takes none).
std::string colon_names(const fs::path& root) {
    std::string found;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = text_of(it->path().filename());
        if (name.find(':') != std::string::npos) found += text_of(it->path().lexically_relative(root)) + "\n";
    }
    return found;
}

// Each file's sha256, the root's and the config folder's paths in it written <root> and <config> first.
Json files_of(const QString& folder, const std::string& prefix, const std::string& root, const std::string& config) {
    Json out = Json::object();
    if (!QFileInfo(folder).isDir()) return out;
    QDirIterator it(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    std::vector<std::string> rels;
    while (it.hasNext()) rels.push_back(QDir(folder).relativeFilePath(it.next()).toStdString());
    const auto parts = [](const std::string& rel) {  // (Python sorts paths part by part)
        std::vector<std::string> pieces;
        std::size_t start = 0;
        for (;;) {
            const std::size_t slash = rel.find('/', start);
            pieces.push_back(rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return pieces;
    };
    std::sort(rels.begin(), rels.end(), [&parts](const std::string& a, const std::string& b) { return parts(a) < parts(b); });
    for (const std::string& rel : rels) {
        if (skipped(rel)) continue;
        if (prefix.empty() && exported(rel)) {
            out[rel] = "<exported>";
            continue;
        }
        std::string data = genko::test::read_bytes(folder + "/" + QString::fromStdString(rel));
        if (claim_file(rel)) data = without_expiry(std::move(data));
        data = replace_all(std::move(data), root, "<root>");
        if (!config.empty()) data = replace_all(std::move(data), config, "<config>");
        out[prefix + rel] = sha256(data);
    }
    return out;
}

Json books_of(const fs::path& root, const fs::path& scratch_store) {
    // (Python's harness reads project.json as JSON; reading a book here makes each page's default layers first, which
    // would take ids from the counted ones: the book is read with ids of its own)
    const genko::core::ScopedIdSource apart(genko::core::counting_ids(1'000'000'000));
    Json out = Json::object();
    std::vector<fs::path> found;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename() == "project.json") found.push_back(it->path().parent_path());
    }
    std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    });
    for (const fs::path& book : found) {
        Json project;
        try {
            const genko::core::Document doc = genko::storage::load_document(book).document;
            genko::storage::AssetStore store(scratch_store);
            project = genko::test::as_v3(genko::storage::project_payload_v4(doc, store));
            project.erase("revision");
            // (the page spec as written: the reader takes margins_mm as floats, as Python's from_dict does on its next
            // load, but a book not read back since keeps the numbers it was made with, as Python's does)
            const Json raw = genko::core::parse_python_json(genko::storage::read_file(book / "project.json"));
            if (raw.is_object() && raw.contains("spec") && raw["spec"].is_object()) project["spec"] = raw["spec"];
        } catch (const std::exception&) {
            continue;
        }
        Json audit = Json::array();
        for (const Json& e : genko::storage::journal::audit_entries(book)) {
            if (genko::core::py_get(e, "kind") == "migrated") continue;
            audit.push_back(Json::object({{"rev", genko::core::py_get(e, "rev")},
                                          {"actor", genko::core::py_get(e, "actor")},
                                          {"changes", genko::core::py_get(e, "changes")},
                                          {"via", genko::core::py_get(e, "via")}}));
        }
        out[text_of(book.lexically_relative(root))] = Json::object({{"project", std::move(project)}, {"audit", std::move(audit)}});
    }
    return out;
}

// The JSON with every object's keys in sorted order.
Json sorted_keys(const Json& value) {
    if (value.is_object()) {
        std::vector<std::string> keys;
        for (const auto& [key, item] : value.items()) {
            (void)item;
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        Json out = Json::object();
        for (const std::string& key : keys) out[key] = sorted_keys(value.at(key));
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) out.push_back(sorted_keys(item));
        return out;
    }
    return value;
}

// The JSON with the revisions it records ("rev", "revision", "undone_revision", "at_rev", "changed_rev",
// "resolved_rev": numbers) written <rev>.
Json without_revisions(const Json& value) {
    if (value.is_object()) {
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            const bool revision = key == "rev" || key == "revision" || key == "undone_revision" || key == "at_rev" || key == "changed_rev" ||
                                  key == "resolved_rev";
            out[key] = revision && item.is_number() ? Json("<rev>") : without_revisions(item);
        }
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) out.push_back(without_revisions(item));
        return out;
    }
    return value;
}

// The record with the folders' paths written <root> and <config> and the time a style was taken <taken_at>.
Json normal(const Json& record, const std::string& root, const std::string& config) {
    std::string text = genko::core::dump_python(record);
    const auto escaped = [](const std::string& path) {
        const std::string quoted = genko::core::dump_python(Json(path));
        return quoted.substr(1, quoted.size() - 2);
    };
    text = replace_all(text, escaped(root), "<root>");
    if (!config.empty()) text = replace_all(text, escaped(config), "<config>");
    static const QRegularExpression address(QStringLiteral(" at 0x[0-9a-f]+>"));
    static const QRegularExpression job(QStringLiteral("job_[^\"/]*_[0-9a-f]{6}"));
    text = QString::fromStdString(text).replace(address, QStringLiteral(">")).replace(job, QStringLiteral("<job>")).toStdString();
    Json data = genko::core::parse_python_json(text);
    std::function<void(Json&)> walk = [&walk](Json& v) {
        if (v.is_object()) {
            for (auto& [key, item] : v.items()) {
                if (key == "seconds" && item.is_number_float()) {
                    item = "<seconds>";
                } else if (key == "at" && item.is_number_float()) {
                    item = "<at>";
                } else if (key == "taken_at" && item.is_string()) {
                    item = "<taken_at>";
                } else {
                    walk(item);
                }
            }
        } else if (v.is_array()) {
            for (Json& item : v) walk(item);
        }
    };
    walk(data);
    return data;
}

// HumanService's methods with Python's parameters.
Json human_call(genko::service::HumanService& human, const std::string& name, const Json& args, const Json& kwargs) {
    using genko::service::Param;
    const Json none;
    const auto bound = [&](std::initializer_list<Param> params) {
        const std::vector<Param> list(params);
        return genko::service::bind_arguments("HumanService." + name, list, args, kwargs);
    };
    if (name == "approve_name") return human.approve_name(bound({{"pages", std::nullopt}})["pages"]);
    if (name == "approve_art") return human.approve_art(bound({{"pages", std::nullopt}})["pages"]);
    if (name == "approve_sheet") {
        const Json a = bound({{"character_id", std::nullopt}, {"candidate_id", std::nullopt}, {"face_box01", none}});
        return human.approve_sheet(a["character_id"], a["candidate_id"], a["face_box01"]);
    }
    if (name == "chat_approval") return human.chat_approval(bound({{"on", std::nullopt}})["on"]);
    if (name == "revoke") {
        const Json a = bound({{"gate", std::nullopt}, {"pages", std::nullopt}, {"character_id", none}, {"reason", Json("")}});
        return human.revoke(a["gate"], a["pages"], a["character_id"], a["reason"]);
    }
    if (name == "reject_proposal") {
        const Json a = bound({{"proposal_id", std::nullopt}, {"note", Json("")}});
        return human.reject_proposal(a["proposal_id"], a["note"]);
    }
    if (name == "close_ticket") {
        const Json a = bound({{"ticket_id", std::nullopt}, {"reply", Json("")}});
        return human.close_ticket(a["ticket_id"], a["reply"]);
    }
    if (name == "reopen_ticket") {
        const Json a = bound({{"ticket_id", std::nullopt}, {"note", Json("")}});
        return human.reopen_ticket(a["ticket_id"], a["note"]);
    }
    if (name == "comment") {
        const Json a = bound({{"page", std::nullopt}, {"text", std::nullopt}, {"frame_id", none}});
        return human.comment(a["page"], a["text"], a["frame_id"]);
    }
    if (name == "accept_proposal") {
        const Json a = bound({{"proposal_id", std::nullopt}, {"force", Json(false)}});
        return human.accept_proposal(a["proposal_id"], genko::core::py_truthy(a["force"]));
    }
    if (name == "export") {
        const Json a = bound({{"fmt", std::nullopt}, {"out", std::nullopt}, {"dpi", none}, {"allow_fixture", Json(false)}, {"force", Json(false)},
                              {"width_px", Json(800), true}, {"max_height", Json(1280), true}, {"long_edge", Json(2048), true}, {"jpeg", none, true},
                              {"spreads", Json(false), true}, {"area", Json("bleed"), true}, {"color", Json("auto"), true}, {"icc", none, true},
                              {"screen", none, true}});
        genko::service::OfficialExport o;
        o.dpi = a["dpi"];
        o.allow_fixture = genko::core::py_truthy(a["allow_fixture"]);
        o.force = genko::core::py_truthy(a["force"]);
        o.width_px = static_cast<int>(genko::core::to_int(a["width_px"]));
        o.max_height = static_cast<int>(genko::core::to_int(a["max_height"]));
        o.long_edge = static_cast<int>(genko::core::to_int(a["long_edge"]));
        if (!a["jpeg"].is_null()) o.jpeg = genko::core::py_truthy(a["jpeg"]);
        o.spreads = genko::core::py_truthy(a["spreads"]);
        o.area = a["area"].get<std::string>();
        o.color = a["color"].get<std::string>();
        if (a["icc"].is_string()) o.icc = a["icc"].get<std::string>();
        if (!a["screen"].is_null()) o.screen = a["screen"];
        return human.export_book(a["fmt"].get<std::string>(), genko::storage::path_from_utf8(a["out"].get<std::string>()), o);
    }
    throw std::runtime_error("no such person's call " + name);
}

// A standard stream (0, 1 or 2) read from or written to a file while it lives.
class Redirected {
public:
    Redirected(int fd, const QString& file, bool output) : fd_(fd) {
        std::fflush(stdout);
        std::fflush(stderr);
        const QByteArray name = QFile::encodeName(file);
#ifdef _WIN32
        const int opened = output ? ::_open(name.constData(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0600)
                                  : ::_open(name.constData(), _O_RDONLY | _O_BINARY);
        saved_ = ::_dup(fd);
        ::_dup2(opened, fd);
        ::_close(opened);
#else
        const int opened = output ? ::open(name.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) : ::open(name.constData(), O_RDONLY | O_CLOEXEC);
        saved_ = ::dup(fd);
        ::dup2(opened, fd);
        ::close(opened);
#endif
        settle();
    }
    ~Redirected() {
        std::fflush(stdout);
        std::fflush(stderr);
#ifdef _WIN32
        ::_dup2(saved_, fd_);
        ::_close(saved_);
#else
        ::dup2(saved_, fd_);
        ::close(saved_);
#endif
        settle();
    }
    Redirected(const Redirected&) = delete;
    Redirected& operator=(const Redirected&) = delete;

private:
    static void settle() {  // (what stdin said before is forgotten: an end of file, a read ahead)
        std::clearerr(stdin);
        std::cin.clear();
    }

    int fd_;
    int saved_ = -1;
};

// `genko studio <args>` as service_harness.py runs genko.studio.cli.main(args): in this process (its ids counted with
// every other step's), in the root (or its "cwd" under it), with the step's stdin; what it printed on stdout (JSON, or
// nothing) and its exit code.
Json cli_step(const Json& step, const fs::path& root) {
    QStringList args;
    bool ascii = false;
    for (const Json& a : step["cli"]) {
        const QString arg = QString::fromStdString(a.get<std::string>());
        if (arg == QLatin1String("--ascii")) {  // (genko's own flag, taken before the command: api/cli.cpp)
            ascii = true;
            continue;
        }
        args << arg;
    }
    QTemporaryDir io;
    const QString in = io.filePath(QStringLiteral("stdin")), out = io.filePath(QStringLiteral("stdout")), err = io.filePath(QStringLiteral("stderr"));
    genko::test::write_bytes(in, step.value("stdin", std::string()));
    const QString was = QDir::currentPath();
    QDir::setCurrent(QString::fromStdString(text_of(root / genko::storage::path_from_utf8(step.value("cwd", std::string())))));
    int code = 0;
    {
        const Redirected input(0, in, false);
        const Redirected output(1, out, true);
        const Redirected errors(2, err, true);
        code = genko::api::studio_command(args, ascii);
    }
    QDir::setCurrent(was);
    const std::string text = genko::test::read_bytes(out);
    Json parsed;
    if (!QString::fromStdString(text).trimmed().isEmpty()) parsed = genko::core::parse_python_json(text);
    return Json::object({{"out", parsed}, {"code", code}});
}

// One step as service_harness.py's _step makes it, with this build.
Json cpp_step(const Json& raw, const fs::path& root, const QProcessEnvironment& env) {
    Json step;
    try {
        step = resolved(raw, root);
    } catch (const std::exception&) {
        return Json::object({{"unresolved", true}});
    }
    if (step.contains("write")) {
        const QString target = QString::fromStdString(text_of(root / genko::storage::path_from_utf8(step["write"].get<std::string>())));
        QDir().mkpath(QFileInfo(target).path());
        genko::test::write_bytes(target, step.contains("base64") ? genko::core::a2b_base64(step["base64"].get<std::string>()) : step["text"].get<std::string>());
        return Json::object();
    }
    if (step.contains("asset")) {
        genko::storage::AssetStore store(root / genko::storage::path_from_utf8(step["asset"].get<std::string>()));
        return Json::object({{"reply", store.put_bytes(genko::core::a2b_base64(step["base64"].get<std::string>()), ".png")}});
    }
    if (step.contains("tool")) {
        try {
            genko::service::StudioService service(root, step.value("actor", std::string("ai:agent")));
            Json kwargs = step.value("kwargs", Json::object());
            if (kwargs.contains("confine")) {  // (Python's keyword-only confine=, which only its command line passes)
                service.set_confine(genko::core::py_truthy(kwargs["confine"]));
                kwargs.erase("confine");
            }
            const genko::service::ToolResult result = service.call(step["tool"].get<std::string>(), step.value("args", Json::array()), kwargs);
            Json images = Json::array();
            for (const std::string& png : result.images) images.push_back(sha256(png));
            return Json::object({{"reply", result.to_dict()}, {"images", std::move(images)}});
        } catch (const std::exception& error) {
            return raised(error);
        }
    }
    if (step.contains("human")) {
        try {
            genko::service::HumanService human(root / genko::storage::path_from_utf8(step["project"].get<std::string>()), step["actor"].get<std::string>());
            return Json::object({{"reply", human_call(human, step["human"].get<std::string>(), step.value("args", Json::array()),
                                                      step.value("kwargs", Json::object()))}});
        } catch (const std::exception& error) {
            return raised(error);
        }
    }
    if (step.contains("fn")) {
        const fs::path book = root / genko::storage::path_from_utf8(step["project"].get<std::string>());
        const std::string fn = step["fn"].get<std::string>();
        const Json args = step.value("args", Json::array());
        const auto path_arg = [&args](std::size_t n) { return genko::storage::path_from_utf8(args.at(n).get<std::string>()); };
        try {
            if (fn == "adopt_drafts") return Json::object({{"reply", genko::service::adopt::adopt_drafts(book)}});
            if (fn == "stats") return Json::object({{"reply", genko::service::evaluate::stats(book)}});
            if (fn == "eval_sample") {
                return Json::object({{"reply", genko::service::evaluate::eval_sample(book, path_arg(0), args.size() > 1 ? args[1].get<std::int64_t>() : 10,
                                                                                     args.size() > 2 ? args[2].get<std::int64_t>() : 0)}});
            }
            if (fn == "eval_score") return Json::object({{"reply", genko::service::evaluate::eval_score(book, path_arg(0))}});
            if (fn == "eval_atari") {
                return Json::object({{"reply", genko::service::evaluate::eval_atari(book, args.empty() ? 5.0 : args[0].get<double>())}});
            }
            return Json::object({{"reply", genko::service::evaluate::audit(book)}});
        } catch (const std::exception& error) {
            return raised(error);
        }
    }
    (void)env;
    return cli_step(step, root);
}

// The command line's record as compared: Python's traceback as this build reports it.
Json cli_record(Json record) {
    if (record.contains("traceback")) {
        record["out"] = Json::object({{"ok", false}, {"error", record["traceback"]}, {"code", "python_error"}});
        record.erase("traceback");
    }
    return record;
}

// A record of a process of its own, whose ids are random: each id it made written <id>, a proposal's (a hash of its
// page's id) pr_<id>.
Json without_ids(const Json& record) {
    static const QRegularExpression ids(QStringLiteral("(?<![0-9a-f])[0-9a-f]{12}(?![0-9a-f])"));
    static const QRegularExpression proposals(QStringLiteral("pr_[0-9a-f]{10}(?![0-9a-f])"));
    const QString text = QString::fromStdString(genko::core::dump_python(record));
    return genko::core::parse_python_json(QString(text).replace(ids, QStringLiteral("<id>")).replace(proposals, QStringLiteral("pr_<id>")).toStdString());
}

// Python's records of a job (service_harness.py run), made in `folder`.
Json python_records(const Json& job, const QString& folder, const QString& scratch) {
    const QString root = folder + "/" + QString::fromStdString(job["name"].get<std::string>());
    Json env = job["env"];
    env["GENKO_CONFIG_DIR"] = (root + "-config").toStdString();
    const QString out = root + ".json";
    QDir().mkpath(folder);
    const Json jobs = Json::array({Json::object({{"root", root.toStdString()}, {"env", env}, {"clock", job["clock"]}, {"steps", job["steps"]},
                                                  {"out", out.toStdString()}})});
    genko::test::write_bytes(root + "-jobs.json", genko::core::dump_python(jobs));
    const auto ran = service_harness({"run", root + "-jobs.json"}, scratch);
    if (!ran.finished || ran.exit_code != 0) throw std::runtime_error("the harness failed: " + ran.err.right(4000).toStdString());
    return genko::test::read_json(out);
}

}  // namespace

class TestContractService : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
    }

    void serviceLikePython_data() {
        QTest::addColumn<int>("job");
        const Json cases = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/service_cases.json");
        for (std::size_t n = 0; n < cases["jobs"].size(); ++n) {
            QTest::newRow(cases["jobs"][n]["name"].get<std::string>().c_str()) << static_cast<int>(n);
        }
    }

    void serviceLikePython() {
        QFETCH(int, job);
        const Json cases = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/service_cases.json");
        const Json& c = cases["jobs"][static_cast<std::size_t>(job)];
        const QString name = QString::fromStdString(c["name"].get<std::string>());

        // Python's records
        const QString py_root = path("py/" + name);
        const QString py_config = path("py/" + name + "-config");
        Json env = c["env"];
        env["GENKO_CONFIG_DIR"] = py_config.toStdString();
        const QString out = path("py/" + name + ".json");
        QDir().mkpath(path("py"));
        const Json jobs = Json::array({Json::object({{"root", py_root.toStdString()}, {"env", env}, {"clock", c["clock"]}, {"steps", c["steps"]},
                                                      {"out", out.toStdString()}})});
        genko::test::write_bytes(path("py/" + name + "-jobs.json"), genko::core::dump_python(jobs));
        const auto ran = service_harness({"run", path("py/" + name + "-jobs.json")}, path("pyenv"));
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json python = genko::test::read_json(out);
        QCOMPARE(python.size(), c["steps"].size());

        // this build's, on its own root
        const QString cpp_root_text = path("cpp/" + name);
        const QString cpp_config = path("cpp/" + name + "-config");
        QDir().mkpath(cpp_root_text);
        const fs::path cpp_root = genko::storage::path_from_utf8(cpp_root_text.toStdString());
        QProcessEnvironment process_env;
        std::vector<std::pair<QByteArray, std::optional<QByteArray>>> saved;
        const auto set_env = [&](const std::string& key, const std::string& value) {
            const QByteArray k = QByteArray::fromStdString(key);
            saved.emplace_back(k, qEnvironmentVariableIsSet(k.constData()) ? std::optional<QByteArray>(qgetenv(k.constData())) : std::nullopt);
            qputenv(k.constData(), QByteArray::fromStdString(value));
            process_env.insert(QString::fromStdString(key), QString::fromStdString(value));
        };
        const auto restore_env = [&saved] {
            for (const auto& [key, value] : saved) {
                if (value) qputenv(key.constData(), *value); else qunsetenv(key.constData());
            }
            saved.clear();
        };
        set_env("GENKO_CONFIG_DIR", cpp_config.toStdString());
        for (const auto& [key, value] : c["env"].items()) set_env(key, replace_all(value.get<std::string>(), "$root", text_of(cpp_root)));
        const genko::core::ScopedIdSource ids(genko::core::counting_ids());
        const genko::core::ScopedClock clock(c["clock"].get<std::string>());
        const fs::path store = genko::storage::path_from_utf8(path("cpp/" + name + "-store").toStdString());
        int compared = 0;
        // (the books an Undo restored: their state assets are canonical JSON, so the free-form dicts come back with their
        // keys in sorted order, and the revision counts on where Python's goes back — schema-v4.md — so from then on
        // those books are compared with their keys in any order and the revisions they and their replies record unread)
        std::set<std::string> restored;
        std::set<std::string> renumbered;
        for (std::size_t n = 0; n < c["steps"].size(); ++n) {
            const Json& step = c["steps"][n];
            const double started = now_seconds();
            Json mine = cpp_step(step, cpp_root, process_env);
            const double ended = now_seconds();
            if (const std::string lease = leases_problem(cpp_root, started, ended); !lease.empty()) {
                restore_env();
                QFAIL(("step " + std::to_string(n) + ": " + lease).c_str());
            }
            Json theirs = python[n];
            mine["books"] = books_of(cpp_root, store);
            mine["files"] = files_of(cpp_root_text, "", text_of(cpp_root), cpp_config.toStdString());
            // (held: items() of a temporary would dangle)
            const Json config_files = files_of(cpp_config, "<config>/", text_of(cpp_root), cpp_config.toStdString());
            for (const auto& [key, value] : config_files.items()) mine["files"][key] = value;
            mine = normal(mine, text_of(cpp_root), cpp_config.toStdString());
            if (theirs.contains("unresolved")) theirs["unresolved"] = true;
            if (step.contains("cli")) {
                mine = cli_record(std::move(mine));
                theirs = cli_record(std::move(theirs));
                // (this build's failures carry their code too: ARCHITECTURE.md §8)
                if (mine["out"].is_object() && theirs["out"].is_object() && mine["out"].contains("code") && !theirs["out"].contains("code") &&
                    mine["out"].value("ok", true) == false) {
                    mine["out"].erase("code");
                }
            }
            if (step.contains("tool") && step["tool"] == "undo" && mine.contains("reply") && mine["reply"].value("ok", false)) {
                restored.insert(step["args"].at(0).get<std::string>());
            }
            // (request_approval for several character sheets: one save here where Python saves each in turn — SPEC
            // COMP-01a —, so from then on the book's revision is behind Python's by the requests after the first: what it
            // records of its revision is compared without it, the rest as before)
            const Json kwargs = step.value("kwargs", Json::object());
            if (step.contains("tool") && step["tool"] == "request_approval" && kwargs.contains("character_ids") && kwargs["character_ids"].is_array() &&
                kwargs["character_ids"].size() > 1 && mine.contains("reply") && mine["reply"].value("ok", false)) {
                renumbered.insert(step["args"].at(0).get<std::string>());
            }
            for (const std::string& book : restored) {
                for (Json* side : {&mine, &theirs}) {
                    if (side->contains("books") && (*side)["books"].contains(book)) {
                        (*side)["books"][book]["project"] = without_revisions(sorted_keys((*side)["books"][book]["project"]));
                    }
                    const bool on_book = step.contains("args") && !step["args"].empty() && step["args"][0] == book;
                    if (on_book && side->contains("reply")) (*side)["reply"] = without_revisions((*side)["reply"]);
                }
            }
            for (const std::string& book : renumbered) {
                for (Json* side : {&mine, &theirs}) {
                    if (side->contains("books") && (*side)["books"].contains(book)) {
                        (*side)["books"][book]["project"] = without_revisions((*side)["books"][book]["project"]);
                        (*side)["books"][book]["audit"] = without_revisions((*side)["books"][book]["audit"]);
                    }
                    const bool on_book = (step.contains("args") && !step["args"].empty() && step["args"][0] == book) ||
                                         (step.contains("project") && step["project"] == book);
                    if (on_book && side->contains("reply")) (*side)["reply"] = without_revisions((*side)["reply"]);
                }
            }
            std::string where;
            if (!genko::test::strict_equal(mine, theirs, &where)) {
                // (GENKO_TEST_KEEP=<folder>: both records of the step written there whole, to read what differs)
                if (const QByteArray keep = qgetenv("GENKO_TEST_KEEP"); !keep.isEmpty()) {
                    const QString base = QString::fromLocal8Bit(keep) + "/" + name + "-" + QString::number(n);
                    genko::test::write_bytes(base + "-cpp.json", genko::core::dump_python_indent2(mine));
                    genko::test::write_bytes(base + "-py.json", genko::core::dump_python_indent2(theirs));
                    std::error_code ec;  // (and both roots as they are then)
                    fs::copy(cpp_root, genko::storage::path_from_utf8((base + "-cpp").toStdString()), fs::copy_options::recursive, ec);
                    fs::copy(genko::storage::path_from_utf8(py_root.toStdString()), genko::storage::path_from_utf8((base + "-py").toStdString()),
                             fs::copy_options::recursive, ec);
                }
                const std::string message = "step " + std::to_string(n) + " " + genko::core::dump_python(step).substr(0, 600) + "\n  at " + where +
                                            "\n  C++:    " + genko::core::dump_python(mine).substr(0, 3000) +
                                            "\n  Python: " + genko::core::dump_python(theirs).substr(0, 3000);
                restore_env();
                QFAIL(message.c_str());
            }
            ++compared;
        }
        restore_env();
        // what the exports wrote: the same bytes but the times Python's files carry (both roots list the same files)
        int exported_files = 0;
        QDirIterator files(py_root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString file = files.next();
            const QString rel = QDir(py_root).relativeFilePath(file);
            if (!exported(rel.toStdString())) continue;
            const QString mine_file = cpp_root_text + "/" + rel;
            QVERIFY2(QFileInfo::exists(mine_file), qPrintable("missing " + rel));
            QVERIFY2(genko::test::exports::normalized(rel, genko::test::read_bytes(mine_file)) ==
                         genko::test::exports::normalized(rel, genko::test::read_bytes(file)),
                     qPrintable(rel + ": " + genko::test::exports::tree_difference(QFileInfo(mine_file).path(), QFileInfo(file).path())));
            ++exported_files;
        }
        const std::string colons = colon_names(cpp_root);
        QVERIFY2(colons.empty(), ("names with a colon:\n" + colons).c_str());
        qInfo("%s: %d steps the same as Python's, %d exported files", qPrintable(name), compared, exported_files);
        QVERIFY(compared >= 15);
    }

    // The command line's job again, each `genko studio …` a process of its own (this build's executable, as people run
    // it, in the root, the step's stdin given): its stdout (JSON) and exit code against Python's, the ids it makes
    // written <id> (random in a process of its own; the books are compared by serviceLikePython's run in this
    // process). Every step's process started, ended in its time without crashing, and exits 0, 1 or 3 — 2 only where
    // Python's argparse refuses the arguments too. What a step's process did is said in full when it differs (no step
    // is run again).
    void commandLineAsProcesses() {
        const Json cases = genko::test::read_json(genko::test::repo_root() + "/native/tests/contract/service_cases.json");
        Json job;
        for (const Json& c : cases["jobs"]) {
            if (c.value("cli", false)) job = c;
        }
        QVERIFY(job.is_object());
        Json python;
        try {
            python = python_records(job, path("py-processes"), path("pyenv"));
        } catch (const std::exception& error) {
            QFAIL(error.what());
        }
        QCOMPARE(python.size(), job["steps"].size());
        const QString root_text = path("processes/cli");
        const QString config = path("processes/cli-config");
        QDir().mkpath(root_text);
        const fs::path root = genko::storage::path_from_utf8(root_text.toStdString());
        QProcessEnvironment env;
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), config);
        for (const auto& [key, value] : job["env"].items()) {
            env.insert(QString::fromStdString(key), QString::fromStdString(replace_all(value.get<std::string>(), "$root", text_of(root))));
        }
        int compared = 0;
        int usage = 0;
        for (std::size_t n = 0; n < job["steps"].size(); ++n) {
            const Json& raw = job["steps"][n];
            if (!raw.contains("cli")) {
                (void)cpp_step(raw, root, env);  // (the files the steps read)
                continue;
            }
            const Json step = resolved(raw, root);
            QStringList args{QStringLiteral("studio")};
            for (const Json& a : step["cli"]) args << QString::fromStdString(a.get<std::string>());
            const QString cwd = QString::fromStdString(text_of(root / genko::storage::path_from_utf8(step.value("cwd", std::string()))));
            const genko::test::Run ran = genko::test::run_genko(args, env, QByteArray::fromStdString(step.value("stdin", std::string())), cwd);
            const std::string what = "step " + std::to_string(n) + " " + genko::core::dump_python(raw).substr(0, 300) + "\n  the process: " +
                                     ran.described().toStdString();
            const Json& theirs_raw = python[n];
            // how it ended
            const int expected = theirs_raw.value("code", -1);
            const bool ended_well = ran.started && ran.finished && !ran.crashed &&
                                    (ran.exit_code == 0 || ran.exit_code == 1 || ran.exit_code == 3 || (ran.exit_code == 2 && expected == 2));
            QVERIFY2(ended_well, what.c_str());
            usage += ran.exit_code == 2 ? 1 : 0;
            // what it printed: one JSON object, or nothing
            Json out;
            const QByteArray printed = ran.out.trimmed();
            if (!printed.isEmpty()) {
                try {
                    out = genko::core::parse_python_json(ran.out.toStdString());
                } catch (const std::exception& error) {
                    QFAIL((what + "\n  stdout is not JSON: " + error.what()).c_str());
                }
            }
            Json mine = without_ids(cli_record(normal(Json::object({{"out", out}, {"code", ran.exit_code}}), text_of(root), config.toStdString())));
            Json kept = Json::object({{"out", theirs_raw["out"]}, {"code", theirs_raw["code"]}});
            if (theirs_raw.contains("traceback")) kept["traceback"] = theirs_raw["traceback"];
            Json theirs = without_ids(cli_record(std::move(kept)));
            // (this build's failures carry their code too: ARCHITECTURE.md §8)
            if (mine["out"].is_object() && theirs["out"].is_object() && mine["out"].contains("code") && !theirs["out"].contains("code") &&
                mine["out"].value("ok", true) == false) {
                mine["out"].erase("code");
            }
            std::string where;
            QVERIFY2(genko::test::strict_equal(mine, theirs, &where),
                     (what + "\n  at " + where + "\n  C++:    " + genko::core::dump_python(mine).substr(0, 3000) +
                      "\n  Python: " + genko::core::dump_python(theirs).substr(0, 3000))
                         .c_str());
            ++compared;
        }
        qInfo("cli as processes: %d steps the same as Python's (%d of them usage errors, exit 2 as Python's)", compared, usage);
        QVERIFY(compared >= 90);
    }
};

QTEST_GUILESS_MAIN(TestContractService)
#include "test_contract_service.moc"
