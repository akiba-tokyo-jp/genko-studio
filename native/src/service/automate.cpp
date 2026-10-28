// 自動処理の土台 (PC5 I-0, SPEC.md AUTO-01; PhotoCraft v0.2.0 engine/src/automate_cmds.rs parse_script and
// file_cmds.rs fit_image and batch, MIT, read as Genko's ops).

#include "service/automate.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <system_error>

#include "core/covers.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/smart.hpp"
#include "formats/copy.hpp"
#include "formats/export.hpp"
#include "render/brushes.hpp"
#include "render/codecs/format.hpp"
#include "render/ops_registry.hpp"
#include "render/timelapse.hpp"
#include "service/actions.hpp"
#include "service/studio_service.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"

namespace genko::service::automate {

using core::Json;

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// An extension as write_copy names it: lower case, no dot, .jpeg as jpg, .tiff as tif.
std::string extension_of(const fs::path& path) {
    std::string ext = lower(core::path_to_utf8(path.extension()));
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    if (ext == "jpeg") return "jpg";
    if (ext == "tiff") return "tif";
    return ext;
}

bool is_book_folder(const fs::path& dir) {
    std::error_code ec;
    return fs::is_directory(dir, ec) && fs::is_regular_file(dir / "project.json", ec);
}

bool hidden(const fs::path& path) {
    const std::string name = core::path_to_utf8(path.filename());
    return !name.empty() && name[0] == '.';
}

std::string op_name(const Json& op) {
    const auto it = op.is_object() ? op.find("op") : op.end();
    return op.is_object() && it != op.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool op_name_text(std::string_view name) {
    if (name.empty()) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::islower(c) || std::isdigit(c) || c == '_'; });
}

core::ParseOptions steps_json() {
    core::ParseOptions options;
    options.max_depth = kMaxStepsDepth;
    return options;
}

// One JSON value of the steps; NaN and Infinity refused as `genko apply` refuses them.
Json parse_value(std::string_view text, const std::string& where) {
    core::ParseRepairs repairs;
    Json value;
    try {
        value = core::parse_python_json(text, &repairs, steps_json());
    } catch (const core::Error& error) {
        throw StepsError(where + "not JSON (" + error.what() + ")");
    }
    if (!repairs.nonfinite.empty()) throw StepsError(where + "the steps must not hold NaN or Infinity (at " + repairs.nonfinite.front() + ")");
    return value;
}

std::size_t page_at(const core::Document& doc, const Json& page) {
    for (std::size_t i = 0; i < doc.pages.size(); ++i) {
        if (core::py_equals(doc.pages[i]->index.json(), page)) return i;
    }
    throw core::OpError("no page " + core::py_str(page));
}

std::optional<double> number_at(const Json& op, const char* key) {
    const auto it = op.find(key);
    if (it == op.end() || it->is_null()) return std::nullopt;
    if (!it->is_number() || it->is_boolean()) throw core::OpError(std::string(key) + " is a number");
    return it->get<double>();
}

// 画像を合わせる (file.automate.fitImage; PhotoCraft's file_cmds.rs fit_image): the page made to fit the box, its shape
// kept; nothing when it fits already (or would grow, with dont_enlarge).
Json fit_image(const core::Document& doc, const Json& op) {
    const auto page = op.find("page");
    if (page == op.end()) throw core::OpError("fit_image takes {page, width_px, height_px, dont_enlarge?, resample?}");
    const core::Page& p = doc.page(page_at(doc, *page));
    const double w = std::round(p.spec.width_mm.value() / 25.4 * p.spec.dpi.value());
    const double h = std::round(p.spec.height_mm.value() / 25.4 * p.spec.dpi.value());
    const double bw = number_at(op, "width_px").value_or(w), bh = number_at(op, "height_px").value_or(h);
    if (!(bw >= 1 && bw <= 100000) || !(bh >= 1 && bh <= 100000)) throw core::OpError("width_px and height_px are 1 to 100000");
    const auto dont = op.find("dont_enlarge");
    if (dont != op.end() && !dont->is_boolean()) throw core::OpError("dont_enlarge is true or false");
    const auto resample = op.find("resample");
    if (resample != op.end() && !resample->is_string()) throw core::OpError("resample is bicubic, bilinear, nearest, lanczos or preserve_details");
    const double k = std::min(bw / w, bh / h);
    const bool enlarge = k > 1.0;
    if ((enlarge && dont != op.end() && dont->get<bool>()) || (std::round(w * k) == w && std::round(h * k) == h)) return Json::array();
    Json resize = Json::object();
    resize["op"] = "resize_pages";
    resize["page"] = *page;
    resize["width_px"] = static_cast<std::int64_t>(std::max(1.0, std::round(w * k)));
    resize["height_px"] = static_cast<std::int64_t>(std::max(1.0, std::round(h * k)));
    resize["resample"] = resample != op.end() ? resample->get<std::string>() : std::string("bicubic");
    return Json::array({resize});
}

// (the pseudo ops' table: PC5 I-e fills in what is still refused)
struct Pseudo {
    std::string_view name;
    Json (*expand)(const core::Document&, const Json&);  // none: not in this build yet
};
constexpr Pseudo kPseudo[] = {
    {"fit_image", &fit_image},
    {"conditional_mode_change", nullptr},
};

const Pseudo* find_pseudo(std::string_view name) {
    for (const Pseudo& p : kPseudo) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

void check_list(const Json& steps, const core::Actor& actor, const std::string& at) {
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const std::string where = at + std::to_string(i + 1);
        const Json& step = steps[i];
        const std::string name = op_name(step);
        if (name.empty()) throw StepsError("step " + where + ": not an op ({\"op\": its name, …its arguments})");
        if (name == "undo") throw StepsError("step " + where + ": undo is not a step (the steps are undone as one change)");
        if (actions::gate_op(name)) {
            throw StepsError("step " + where + ": " + name + " cannot be a step (approvals and page locks are made by a person, not by a script)");
        }
        if (!actor.can_approve() && !pseudo_op(name) && !agent_op(name)) {
            throw StepsError("step " + where + ": " + name + " is not an op an agent may run (" + actor.name() + ": the ops apply_ops takes)");
        }
        if (name == "for_pages") {
            const auto inner = step.find("ops");
            if (inner != step.end() && inner->is_array()) check_list(*inner, actor, where + ".");
        }
    }
}

void append(Json& to, const Json& more) {
    for (const Json& item : more) to.push_back(item);
}

}  // namespace

// --- the steps --------------------------------------------------------------------------------------------------------

Json read_steps(std::string_view text) {
    if (text.size() > kMaxStepsBytes) throw StepsError("the steps are larger than 1 MiB");
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    Json steps = Json::array();
    if (first < text.size() && (text[first] == '[' || text[first] == '{')) {
        Json value = parse_value(text, "");
        if (value.is_object()) {
            const auto it = value.find("steps");
            if (it == value.end() || !it->is_array()) throw StepsError("a steps object holds \"steps\": [ …ops… ]");
            value = *it;
        }
        if (!value.is_array()) throw StepsError("the steps are a JSON array of ops, {\"steps\": […]} or one op a line");
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (op_name(value[i]).empty()) throw StepsError("step " + std::to_string(i + 1) + ": not an op ({\"op\": its name, …its arguments})");
        }
        steps = std::move(value);
    } else {
        std::size_t n = 0;
        std::size_t start = 0;
        while (start <= text.size()) {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            std::string_view line = text.substr(start, end - start);
            ++n;
            start = end + 1;
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.remove_suffix(1);
            if (line.empty() || line[0] == '#' || line.substr(0, 2) == "//") {
                if (end == text.size()) break;
                continue;
            }
            const std::string where = "line " + std::to_string(n) + ": ";
            std::size_t space = 0;
            while (space < line.size() && !std::isspace(static_cast<unsigned char>(line[space]))) ++space;
            const std::string name(line.substr(0, space));
            std::string_view rest = line.substr(space);
            while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.remove_prefix(1);
            if (!op_name_text(name)) throw StepsError(where + "an op's name first (a-z, 0-9 and _), then its arguments as {…}");
            Json op = Json::object();
            op["op"] = name;
            if (!rest.empty()) {
                const Json args = parse_value(rest, where);
                if (!args.is_object()) throw StepsError(where + "the arguments are one JSON object {…}");
                for (const auto& [key, value] : args.items()) {
                    if (key == "op") {
                        if (value != Json(name)) throw StepsError(where + "the op is named twice");
                        continue;
                    }
                    op[key] = value;
                }
            }
            steps.push_back(std::move(op));
            if (steps.size() > kMaxSteps) throw StepsError("more than " + std::to_string(kMaxSteps) + " steps");
            if (end == text.size()) break;
        }
    }
    if (steps.size() > kMaxSteps) throw StepsError("more than " + std::to_string(kMaxSteps) + " steps");
    return steps;
}

Json read_steps_file(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (!ec && size > kMaxStepsBytes) throw StepsError("the steps are larger than 1 MiB");
    return read_steps(storage::read_file_bounded(file, kMaxStepsBytes));
}

bool pseudo_op(std::string_view name) { return find_pseudo(name) != nullptr; }

std::vector<std::string> pseudo_op_names() {
    std::vector<std::string> out;
    for (const Pseudo& p : kPseudo) out.emplace_back(p.name);
    return out;
}

Json expand_pseudo(const core::Document& doc, const Json& op) {
    const std::string name = op_name(op);
    const Pseudo* p = find_pseudo(name);
    if (p == nullptr) throw core::OpError(name + " is not a pseudo op");
    if (p->expand == nullptr) throw core::ApplyError(name + " is not in the C++ build yet", "not_yet_ported");
    return p->expand(doc, op);
}

void check_steps(const Json& steps, const core::Actor& actor) {
    if (!steps.is_array()) throw StepsError("the steps are a list of ops");
    if (steps.size() > kMaxSteps) throw StepsError("more than " + std::to_string(kMaxSteps) + " steps");
    check_list(steps, actor, "");
}

core::ApplyResult run_steps(const core::Document& doc, const Json& steps, const core::Actor& actor, bool dry_run) {
    check_steps(steps, actor);
    const core::CommandBus bus(render::ops_registry());
    const bool any_pseudo = std::any_of(steps.begin(), steps.end(), [](const Json& op) { return pseudo_op(op_name(op)); });
    if (!any_pseudo) return bus.apply(doc, steps, actor, dry_run);  // (as `genko apply` applies them)

    core::ApplyResult total{doc};
    std::set<std::string> seen;
    std::size_t done = 0;  // the ops run before the ones in hand (the results' "index" counts on)
    Json pending = Json::array();
    const auto flush = [&] {
        if (pending.empty()) return;
        core::ApplyResult part = bus.apply(total.doc, pending, actor, dry_run);
        append(total.applied, part.applied);
        for (const Json& warning : part.warnings) {
            const std::string key = warning.dump();
            if (seen.insert(key).second) total.warnings.push_back(warning);
        }
        for (Json result : part.results) {
            if (result.is_object() && result.contains("index") && result["index"].is_number_integer()) {
                result["index"] = result["index"].get<std::int64_t>() + static_cast<std::int64_t>(done);
            }
            total.results.push_back(std::move(result));
        }
        append(total.journal_ops, part.journal_ops);
        total.doc = std::move(part.doc);
        done += pending.size();
        pending = Json::array();
    };
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const Json& step = steps[i];
        const std::string name = op_name(step);
        if (!pseudo_op(name)) {
            pending.push_back(step);
            continue;
        }
        flush();
        Json made;
        try {
            made = expand_pseudo(total.doc, step);
        } catch (const core::ApplyError& error) {
            throw core::ApplyError("ops[" + std::to_string(i) + "] " + name + ": " + error.what(), error.code());
        } catch (const core::OpError& error) {
            throw core::ApplyError("ops[" + std::to_string(i) + "] " + name + ": " + error.what());
        }
        try {
            check_steps(made, actor);
        } catch (const StepsError& error) {
            throw StepsError("step " + std::to_string(i + 1) + " (" + name + "): " + error.what());
        }
        pending = std::move(made);
        flush();
    }
    flush();
    return total;
}

Json aim_steps(const core::Document& doc, const Json& steps, bool picture, bool recorded) {
    Json out = Json::array();
    if (steps.empty()) return out;
    if (recorded) {
        for (const auto& page : doc.pages) {
            if (!picture && core::is_cover(*page)) continue;
            std::optional<std::string> layer;
            if (picture) {
                for (auto it = page->layers.rbegin(); it != page->layers.rend(); ++it) {
                    if (it->kind != core::LayerKind::Folder) {
                        layer = it->id;
                        break;
                    }
                }
            }
            append(out, actions::replay(steps, page->index.json(), layer, std::nullopt));
        }
        return out;
    }
    Json repeat = Json::object();
    repeat["op"] = "for_pages";
    repeat["pages"] = picture ? "all" : "body";
    repeat["ops"] = steps;
    try {
        return core::expand_for_pages(doc, repeat);
    } catch (const core::OpError& error) {
        throw StepsError(std::string("the steps are run page by page: ") + error.what());
    }
}

// --- the inputs -------------------------------------------------------------------------------------------------------

const std::vector<std::string>& input_extensions() {
    static const std::vector<std::string> out = [] {
        std::vector<std::string> list;
        const auto add = [&list](std::string_view ext) {
            std::string e = lower(std::string(ext));
            if (!e.empty() && e[0] == '.') e.erase(0, 1);
            if (std::find(list.begin(), list.end(), e) == list.end()) list.push_back(e);
        };
        for (const render::codecs::Format format : render::codecs::kFormats) {
            if (!render::codecs::caps(format).read) continue;
            for (const std::string_view ext : render::codecs::extensions(format)) add(ext);
        }
        for (const char* ext : {"psd", "psb", "pcraft"}) add(ext);
        for (const core::smart::FileFormat& f : core::smart::kLaterFileFormats) {
            if (f.format == "raw") add(f.suffix);
        }
        return list;
    }();
    return out;
}

Inputs list_inputs(const std::vector<fs::path>& given, const InputOptions& options) {
    Inputs out;
    const std::vector<std::string>& wanted = options.extensions.empty() ? input_extensions() : options.extensions;
    const auto taken = [&wanted](const fs::path& file) {
        std::string ext = lower(core::path_to_utf8(file.extension()));
        if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
        return std::find(wanted.begin(), wanted.end(), ext) != wanted.end();
    };
    const auto add = [&out, &options](Input input) {
        out.items.push_back(std::move(input));
        if (out.items.size() > options.limit) {
            throw core::Error("value", "more than " + std::to_string(options.limit) + " files to take: choose fewer");
        }
    };
    for (const fs::path& path : given) {
        std::error_code ec;
        if (is_book_folder(path)) {
            add(Input{path, true});
        } else if (fs::is_directory(path, ec)) {
            std::vector<fs::path> folders{path};
            while (!folders.empty()) {
                const fs::path folder = folders.front();
                folders.erase(folders.begin());
                std::vector<fs::path> entries;
                for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                    entries.push_back(it->path());
                }
                if (ec) out.skipped.push_back(Json{{"path", core::path_to_utf8(folder)}, {"reason", "the folder cannot be read"}});
                std::sort(entries.begin(), entries.end());
                std::vector<fs::path> inner;
                for (const fs::path& entry : entries) {
                    if (hidden(entry)) continue;
                    std::error_code e2;
                    if (is_book_folder(entry)) {
                        add(Input{entry, true});  // (a book is a document of the folder, as a picture is)
                    } else if (fs::is_directory(entry, e2)) {
                        if (options.recurse) inner.push_back(entry);
                    } else if (fs::is_regular_file(entry, e2) && taken(entry)) {
                        add(Input{entry, false});
                    }
                }
                folders.insert(folders.begin(), inner.begin(), inner.end());  // (depth first, in name order)
            }
        } else if (fs::is_regular_file(path, ec)) {
            add(Input{path, false});
        } else {
            out.skipped.push_back(Json{{"path", core::path_to_utf8(path)}, {"reason", "not found"}});
        }
    }
    return out;
}

// --- the outputs ------------------------------------------------------------------------------------------------------

std::string output_extension(const Output& output, const Input& input) {
    const auto exts = formats::copy_extensions();
    if (output.format != "same") {
        std::string ext = lower(output.format);
        if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
        if (ext == "jpeg") ext = "jpg";
        if (ext == "tiff") ext = "tif";
        return ext;
    }
    if (!input.book) {
        const std::string own = extension_of(input.path);
        if (std::find(exts.begin(), exts.end(), own) != exts.end()) return own;
    }
    return "png";
}

fs::path output_path(const Output& output, const Input& input, std::string_view page_stem, std::string_view extension,
                     const std::vector<fs::path>& taken) {
    fs::path source = input.path;
    if (source.filename().empty()) source = source.parent_path();  // (a folder given with its separator)
    std::string name = output.prefix + core::path_to_utf8(source.stem());
    if (!page_stem.empty()) name += "_" + std::string(page_stem);
    name += output.suffix;
    const std::string base = formats::safe_name(name, "genko");
    const std::string ext(extension);
    fs::path place = output.folder / core::path_from_utf8(base + "." + ext);
    const auto used = [&taken](const fs::path& p) {
        std::error_code ec;
        return fs::exists(p, ec) || fs::is_symlink(p, ec) || std::find(taken.begin(), taken.end(), p) != taken.end();
    };
    for (int n = 2; used(place); ++n) {
        if (n > 9999) throw core::Error("refused_target", "too many files of that name: " + core::path_to_utf8(place));
        place = output.folder / core::path_from_utf8(base + "-" + std::to_string(n) + "." + ext);
    }
    return place;
}

// --- the runner -------------------------------------------------------------------------------------------------------

Json Result::json() const {
    Json out = Json::object();
    out["ok"] = errors.empty();
    out["files"] = files;
    out["errors"] = errors;
    out["skipped"] = skipped;
    if (cancelled) out["cancelled"] = true;
    return out;
}

namespace {

Json notes_json(const std::vector<std::string>& notes) {
    Json out = Json::array();
    for (const std::string& n : notes) out.push_back(n);
    return out;
}

}  // namespace

Result run(const std::vector<Input>& inputs, const Json& steps, const Output& output, const Options& options) {
    check_steps(steps, options.actor);
    if (output.quality && (*output.quality < 0 || *output.quality > 12)) throw core::Error("value", "the quality is 0 to 12");
    if (output.to == Output::To::Folder) {
        if (output.folder.empty()) throw core::Error("value", "the folder to write into is not given");
        if (output.format != "same") {
            const std::string ext = output_extension(output, Input{});
            const auto exts = formats::copy_extensions();
            if (std::find(exts.begin(), exts.end(), ext) == exts.end()) throw core::Error("value", "not a format this build writes: ." + ext);
        }
        std::error_code ec;
        fs::create_directories(output.folder, ec);
        if (ec || !fs::is_directory(output.folder, ec)) {
            throw core::Error("io", "the folder cannot be made: " + core::path_to_utf8(output.folder));
        }
    }
    Result result;
    std::vector<fs::path> written;
    formats::CopyOptions copy;
    copy.quality = output.quality;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const Input& input = inputs[i];
        if (options.cancelled && options.cancelled()) {
            result.cancelled = true;
            break;
        }
        if (options.progress) options.progress(i, inputs.size(), input.path);
        const std::string where = core::path_to_utf8(input.path);
        try {
            Json done = Json::object();
            done["input"] = where;
            if (!input.book) {
                if (output.to == Output::To::Book) {
                    throw core::Error("refused_target", "a picture's own file is never written: write the batch into a folder");
                }
                const formats::Opened opened =
                    formats::open_file(storage::read_file(input.path), core::path_to_utf8(input.path.filename()), options.open);
                const core::ApplyResult ran = run_steps(opened.doc, aim_steps(opened.doc, steps, true, options.recorded), options.actor);
                std::vector<std::string> notes = opened.notes;
                if (output.to == Output::To::Folder) {
                    const fs::path dest = output_path(output, input, {}, output_extension(output, input), written);
                    formats::check_target(formats::write_guard(ran.doc, std::nullopt, {input.path}), dest);
                    const formats::CopyResult wrote = formats::write_copy(ran.doc, 0, dest, copy);
                    written.push_back(dest);
                    done["output"] = core::path_to_utf8(wrote.file);
                    notes.insert(notes.end(), wrote.notes.begin(), wrote.notes.end());
                }
                if (!notes.empty()) done["notes"] = notes_json(notes);
            } else if (output.to == Output::To::Book) {
                storage::ProjectLock lock(input.path, options.actor.name());
                lock.try_acquire();
                storage::journal::repair(input.path);
                const storage::LoadResult loaded = storage::load_document(input.path);
                if (loaded.report.source_version < 4) throw storage::NeedsMigration(input.path, loaded.report.source_version);
                if (options.book_brushes) render::brushes::register_book(loaded.document);
                const core::ApplyResult ran = run_steps(loaded.document, aim_steps(loaded.document, steps, false, options.recorded), options.actor);
                storage::SaveRequest request;
                request.actor = options.actor.name();
                request.base_revision = loaded.document.revision;
                request.ops = ran.journal_ops;
                request.script = options.script;
                const bool lapse = render::timelapse::is_on(ran.doc);  // (タイムラプス: the pages this save changes)
                Json before;
                if (lapse) {
                    try {
                        before = storage::read_disk_state(input.path).payload;
                    } catch (const std::exception&) {  // (the timelapse never stops a save: every page is recorded then)
                    }
                }
                const storage::SaveResult saved = storage::Saver(lock).save(ran.doc, request);
                if (lapse && !saved.already_committed) render::timelapse::after_save(input.path, ran.doc, before);
                lock.release();
                done["revision"] = saved.revision;
            } else {
                const storage::LoadResult loaded = storage::load_document(input.path);
                if (options.book_brushes) render::brushes::register_book(loaded.document);
                const core::ApplyResult ran = run_steps(loaded.document, aim_steps(loaded.document, steps, false, options.recorded), options.actor);
                if (output.to == Output::To::Folder) {
                    const std::string ext = output_extension(output, input);
                    const formats::WriteGuard guard = formats::write_guard(ran.doc, input.path);
                    Json outputs = Json::array();
                    std::vector<std::string> notes;
                    for (std::size_t p = 0; p < ran.doc.pages.size(); ++p) {
                        const fs::path dest = output_path(output, input, core::file_stem(ran.doc.page(p)), ext, written);
                        formats::check_target(guard, dest);
                        const formats::CopyResult wrote = formats::write_copy(ran.doc, p, dest, copy);
                        written.push_back(dest);
                        outputs.push_back(core::path_to_utf8(wrote.file));
                        notes.insert(notes.end(), wrote.notes.begin(), wrote.notes.end());
                    }
                    done["outputs"] = outputs;
                    if (!notes.empty()) done["notes"] = notes_json(notes);
                }
            }
            result.files.push_back(std::move(done));
        } catch (const core::Error& error) {
            result.errors.push_back(Json{{"input", where}, {"error", error.what()}, {"code", error.code()}});
            if (options.stop_on_error) break;
        } catch (const std::exception& error) {
            result.errors.push_back(Json{{"input", where}, {"error", error.what()}, {"code", "internal"}});
            if (options.stop_on_error) break;
        }
    }
    if (options.progress) options.progress(result.files.size() + result.errors.size(), inputs.size(), {});
    return result;
}

}  // namespace genko::service::automate
