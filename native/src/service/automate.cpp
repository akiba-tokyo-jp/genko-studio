// 自動処理の土台 (PC5 I-0, SPEC.md AUTO-01; PhotoCraft v0.2.0 engine/src/automate_cmds.rs parse_script and
// file_cmds.rs fit_image and batch, MIT, read as Genko's ops).

#include "service/automate.hpp"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <set>
#include <system_error>

#include "core/covers.hpp"
#include "core/ink.hpp"
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

// 条件付きモード変更 (file.automate.conditionalModeChange; PhotoCraft's file_cmds.rs conditional_mode_change, PC5 I-e): the
// page into `to` (rgb, gray, cmyk, lab; grayscale is gray) when its mode is one of `from` (any: every mode), its depth
// kept; nothing when it is not, or is in `to` already.
Json conditional_mode_change(const core::Document& doc, const Json& op) {
    const auto page = op.find("page");
    const auto to_it = op.find("to");
    if (page == op.end() || to_it == op.end()) throw core::OpError("conditional_mode_change takes {page, from?, to}");
    const auto mode = [](const Json& value) {
        std::string m = value.is_string() ? lower(value.get<std::string>()) : std::string();
        return m == "grayscale" ? std::string("gray") : m;
    };
    const std::string to = mode(*to_it);
    const auto& into = modes_into();
    if (std::find(into.begin(), into.end(), to) == into.end()) throw core::OpError("conditional_mode_change: to is rgb, gray, cmyk or lab");
    std::vector<std::string> from;
    const auto from_it = op.find("from");
    if (from_it == op.end() || *from_it == Json("any")) {
        from = mode_names();
    } else if (from_it->is_array()) {
        for (const Json& each : *from_it) {
            const std::string m = mode(each);
            const auto& names = mode_names();
            if (std::find(names.begin(), names.end(), m) == names.end()) {
                throw core::OpError("conditional_mode_change: from is a list of bitmap, gray, duotone, indexed, rgb, cmyk, lab and multichannel, or any");
            }
            from.push_back(m);
        }
    } else {
        throw core::OpError("conditional_mode_change: from is a list of bitmap, gray, duotone, indexed, rgb, cmyk, lab and multichannel, or any");
    }
    const core::Page& p = doc.page(page_at(doc, *page));
    const std::string now = page_mode(p);
    if (now == to || std::find(from.begin(), from.end(), now) == from.end()) return Json::array();
    Json set = Json::object();
    set["op"] = "set_color_mode";
    set["page"] = *page;
    set["mode"] = to;
    // (its depth kept: a page of no colour model keeps the one it holds its pixels at, as set_color_mode does unasked)
    const int depth = p.color && !p.color->bitmap ? p.color->depth : p.rgb && !p.rgb->table ? p.rgb->depth : 0;
    if (depth == 8 || depth == 16 || depth == 32) set["depth"] = depth;
    return Json::array({set});
}

// (the pseudo ops' table)
struct Pseudo {
    std::string_view name;
    Json (*expand)(const core::Document&, const Json&);  // none: not in this build yet
};
constexpr Pseudo kPseudo[] = {
    {"fit_image", &fit_image},
    {"conditional_mode_change", &conditional_mode_change},
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
        if (!at.empty() && pseudo_op(name)) {  // (only the steps' own level is turned into ops: for_pages repeats ops)
            throw StepsError("step " + where + ": " + name + " is a pseudo op and cannot be inside for_pages");
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
            const std::string where = "steps, line " + std::to_string(n) + ": ";
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

std::string read_steps_bytes(const fs::path& file) {
    std::error_code ec;
    const fs::file_status status = fs::status(file, ec);
    // (a FIFO, a device: never read — its reading may not end; the review of PC5 I-e, P3-10)
    if (fs::exists(status) && !fs::is_regular_file(status)) throw core::Error("value", "the file cannot be read (it is not a regular file)");
    const auto size = fs::file_size(file, ec);
    if (!ec && size > kMaxStepsBytes) throw StepsError("the steps are larger than 1 MiB");
    return storage::read_file_bounded(file, kMaxStepsBytes);
}

Json read_steps_file(const fs::path& file) { return read_steps(read_steps_bytes(file)); }

bool pseudo_op(std::string_view name) { return find_pseudo(name) != nullptr; }

std::string page_mode(const core::Page& page) {
    if (page.color) return std::string(core::mode_name(*page.color));
    if (page.rgb && page.rgb->table) return "indexed";
    return "rgb";
}

const std::vector<std::string>& mode_names() {
    static const std::vector<std::string> names{"bitmap", "gray", "duotone", "indexed", "rgb", "cmyk", "lab", "multichannel"};
    return names;
}

const std::vector<std::string>& modes_into() {
    static const std::vector<std::string> names{"rgb", "cmyk", "lab", "gray"};
    return names;
}

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

void check_script_name(const std::string& name) {
    const QString text = QString::fromStdString(name);
    if (text.size() > static_cast<qsizetype>(kMaxScriptName)) {
        throw core::Error("value", "the script's name is longer than " + std::to_string(kMaxScriptName) + " characters");
    }
}

// --- the books --------------------------------------------------------------------------------------------------------

namespace {

core::ParseOptions text_file() {
    core::ParseOptions options;
    options.universal_newlines = true;
    return options;
}

}  // namespace

bool studio_book(const fs::path& dir) {
    std::error_code ec;
    if (fs::is_directory(dir / "studio" / "drafts", ec)) return true;
    try {
        const Json payload = core::parse_python_json(storage::read_file(dir / "project.json"), nullptr, text_file());
        const auto truthy = [&payload](const char* key) {
            const auto it = payload.find(key);
            return it != payload.end() && core::py_truthy(*it);
        };
        return payload.is_object() && (truthy("strict_gates") || truthy("studio"));
    } catch (const core::Error&) {
        return false;
    }
}

void refuse_legacy(const fs::path& dir) {
    const int version = storage::project_version(core::parse_python_json(storage::read_file(dir / "project.json"), nullptr, text_file()));
    if (version < 4) throw storage::NeedsMigration(dir, version);
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
    std::size_t looked = 0;  // (the files and folders looked at: a folder of many folders ends too)
    const auto look = [&looked, &options] {
        if (++looked > options.scan_limit) {
            throw core::Error("value", "more than " + std::to_string(options.scan_limit) + " files and folders to look through: choose fewer");
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
                    look();
                    entries.push_back(it->path());
                }
                if (ec) out.skipped.push_back(Json{{"path", core::path_to_utf8(folder)}, {"reason", "the folder cannot be read"}});
                std::sort(entries.begin(), entries.end());
                std::vector<fs::path> inner;
                for (const fs::path& entry : entries) {
                    if (hidden(entry)) continue;
                    std::error_code e2;
                    // (a link to a folder is never entered nor taken: no folder twice, a loop of links ends)
                    if (fs::is_symlink(fs::symlink_status(entry, e2)) && fs::is_directory(entry, e2)) continue;
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

// Nothing is written inside a book's folder — the input's or any other —: refused before anything is made.
void refuse_inside_book(const fs::path& place) {
    std::error_code ec;
    // (links and .. resolved first: a link to a folder in a book is inside it, /a/book/../x is not — the review's P3-1)
    fs::path resolved = fs::absolute(place, ec);
    if (ec) resolved = place;
    std::error_code e3;
    const fs::path canonical = fs::weakly_canonical(resolved, e3);
    resolved = e3 ? resolved.lexically_normal() : canonical;
    for (fs::path p = resolved;; p = p.parent_path()) {
        std::error_code e2;
        if (fs::is_regular_file(p / "project.json", e2)) {
            throw core::Error("refused_target", "cannot write inside the book's folder: " + core::path_to_utf8(p));
        }
        if (p == p.parent_path() || p.empty()) break;
    }
}

// A folder the runner writes into: refuse_inside_book, then made.
void make_output_folder(const fs::path& folder) {
    if (folder.empty()) throw core::Error("value", "the folder to write into is not given");
    refuse_inside_book(folder);
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec || !fs::is_directory(folder, ec)) throw core::Error("io", "the folder cannot be made: " + core::path_to_utf8(folder));
}

void check_format(const Output& output) {
    if (output.quality && (*output.quality < 0 || *output.quality > 12)) throw core::Error("value", "the quality is 0 to 12");
    if (output.format == "same") return;
    const std::string ext = output_extension(output, Input{});
    const auto exts = formats::copy_extensions();
    if (std::find(exts.begin(), exts.end(), ext) == exts.end()) throw core::Error("value", "not a format this build writes: ." + ext);
}

// The outputs each page is written to: the output itself, then each of its Also.
std::vector<Output> targets_of(const Output& output) {
    std::vector<Output> out{output};
    for (const Output::Also& also : output.also) {
        Output more = output;
        more.also.clear();
        more.folder = also.folder;
        more.format = also.format;
        more.quality = also.quality;
        out.push_back(std::move(more));
    }
    return out;
}

std::string serial_text(int number) {
    std::string text = std::to_string(number);
    while (text.size() < 3) text.insert(text.begin(), '0');
    return text;
}

}  // namespace

Result run(const std::vector<Input>& inputs, const Json& steps, const Output& output, const Options& options) {
    check_steps(steps, options.actor);
    if (output.quality && (*output.quality < 0 || *output.quality > 12)) throw core::Error("value", "the quality is 0 to 12");
    if (output.serial < 0) throw core::Error("value", "the serial number starts at 0 or more");
    if (options.open_book != nullptr && (inputs.size() != 1 || output.to == Output::To::Book)) {
        throw core::Error("value", "the open book is run alone, and written into a folder or not at all");
    }
    const std::vector<Output> targets = targets_of(output);
    if (output.to == Output::To::Folder) {
        for (const Output& target : targets) check_format(target);
        for (const Output& target : targets) make_output_folder(target.folder);
    }
    Result result;
    std::vector<fs::path> written;
    Json outputs = Json::array();  // (the files of the input in hand written so far: said with its error too — the review's P3-7)
    // Each page of `doc` written to every target (never over the picture's own file, the book, its links: the guard).
    const auto write_pages = [&](const core::Document& doc, const Input& input, const formats::WriteGuard& guard, std::size_t number,
                                 Json& done) {
        std::vector<std::string> notes;
        for (Output target : targets) {
            if (target.serial > 0) target.suffix += "_" + serial_text(target.serial + static_cast<int>(number));
            const std::string ext = output_extension(target, input);
            formats::CopyOptions copy;
            copy.quality = target.quality;
            for (std::size_t p = 0; p < doc.pages.size(); ++p) {
                const fs::path dest = output_path(target, input, input.book ? core::file_stem(doc.page(p)) : std::string(), ext, written);
                formats::check_target(guard, dest);
                const formats::CopyResult wrote = formats::write_copy(doc, p, dest, copy);
                written.push_back(dest);
                outputs.push_back(core::path_to_utf8(wrote.file));
                notes.insert(notes.end(), wrote.notes.begin(), wrote.notes.end());
            }
        }
        // (a picture: "output", its first file, as `genko batch` has said since PC5 I-0 — and "outputs" too when it made more:
        // more formats, pages a step added; a book's pages: "outputs")
        if (!input.book && !outputs.empty()) done["output"] = outputs[0];
        if (input.book || outputs.size() > 1) done["outputs"] = outputs;
        return notes;
    };
    // The steps on `doc` — Options::first's, then the steps aimed at its pages — as one change.
    const auto ran_on = [&](const core::Document& doc, bool picture, const core::Actor& actor) {
        Json all = options.first ? options.first(doc, picture) : Json::array();
        append(all, aim_steps(doc, steps, picture, options.recorded));
        return run_steps(doc, all, actor);
    };
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const Input& input = inputs[i];
        if (options.cancelled && options.cancelled()) {
            result.cancelled = true;
            break;
        }
        if (options.progress) options.progress(i, inputs.size(), input.path);
        const std::string where = core::path_to_utf8(input.path);
        outputs = Json::array();
        try {
            Json done = Json::object();
            done["input"] = where;
            if (options.open_book != nullptr) {
                // (the window's book as it is: the steps run on a copy, the window's own left as it was)
                const core::ApplyResult ran = ran_on(*options.open_book, !input.book, options.actor);
                if (output.to == Output::To::Folder) {
                    const formats::WriteGuard guard = input.book ? formats::write_guard(ran.doc, input.path)
                                                                 : formats::write_guard(ran.doc, std::nullopt, {input.path});
                    const std::vector<std::string> notes = write_pages(ran.doc, input, guard, i, done);
                    if (!notes.empty()) done["notes"] = notes_json(notes);
                }
            } else if (!input.book) {
                if (output.to == Output::To::Book) {
                    throw core::Error("refused_target", "a picture's own file is never written: write the batch into a folder");
                }
                formats::OpenOptions open = options.open;
                open.folder = input.path.parent_path();  // (PC5 I-d: a .pcraft's data sets' pictures beside it)
                const formats::Opened opened = formats::open_file(storage::read_file(input.path), core::path_to_utf8(input.path.filename()), open);
                const core::ApplyResult ran = ran_on(opened.doc, true, options.actor);
                std::vector<std::string> notes = opened.notes;
                if (output.to == Output::To::Folder) {
                    const std::vector<std::string> more = write_pages(ran.doc, input, formats::write_guard(ran.doc, std::nullopt, {input.path}), i, done);
                    notes.insert(notes.end(), more.begin(), more.end());
                }
                if (!notes.empty()) done["notes"] = notes_json(notes);
            } else if (output.to == Output::To::Book) {
                refuse_legacy(input.path);  // (a v1–v3 book: before its lock or its journal is touched)
                const core::Actor actor = options.unnamed && studio_book(input.path) ? core::Actor("legacy:unknown") : options.actor;
                storage::ProjectLock lock(input.path, actor.name());
                lock.try_acquire();
                storage::journal::repair(input.path);
                const storage::LoadResult loaded = storage::load_document(input.path);
                if (loaded.report.source_version < 4) throw storage::NeedsMigration(input.path, loaded.report.source_version);
                if (options.book_brushes) render::brushes::register_book(loaded.document);
                const core::ApplyResult ran = ran_on(loaded.document, false, actor);
                storage::SaveRequest request;
                request.actor = actor.name();
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
                const core::Actor actor = options.unnamed && studio_book(input.path) ? core::Actor("legacy:unknown") : options.actor;
                const storage::LoadResult loaded = storage::load_document(input.path);
                if (options.book_brushes) render::brushes::register_book(loaded.document);
                const core::ApplyResult ran = ran_on(loaded.document, false, actor);
                if (output.to == Output::To::Folder) {
                    const std::vector<std::string> notes = write_pages(ran.doc, input, formats::write_guard(ran.doc, input.path), i, done);
                    if (!notes.empty()) done["notes"] = notes_json(notes);
                }
            }
            result.files.push_back(std::move(done));
        } catch (const core::Error& error) {
            result.errors.push_back(Json{{"input", where}, {"error", error.what()}, {"code", error.code()}});
            if (!outputs.empty()) result.errors.back()["outputs"] = outputs;  // (written before it failed: kept)
            if (options.stop_on_error) break;
        } catch (const std::exception& error) {
            result.errors.push_back(Json{{"input", where}, {"error", error.what()}, {"code", "internal"}});
            if (!outputs.empty()) result.errors.back()["outputs"] = outputs;
            if (options.stop_on_error) break;
        }
    }
    if (options.progress) options.progress(result.files.size() + result.errors.size(), inputs.size(), {});
    return result;
}

// --- PC5 I-e: 画像プロセッサー・レンズ補正 -------------------------------------------------------------------------------

namespace {

// The formats 画像プロセッサー writes, each its folder (Photoshop's), in the window's order.
constexpr std::pair<std::string_view, std::string_view> kProcessorFormats[] = {{"jpg", "JPEG"}, {"psd", "PSD"}, {"tif", "TIFF"}, {"png", "PNG"}};

std::string processor_format(std::string text) {
    text = lower(std::move(text));
    if (!text.empty() && text[0] == '.') text.erase(0, 1);
    if (text == "jpeg") return "jpg";
    if (text == "tiff") return "tif";
    return text;
}

// The pages the runner's own steps go to: a picture's page, a book's but its covers.
std::vector<const core::Page*> worked_pages(const core::Document& doc) {
    std::vector<const core::Page*> out;
    for (const auto& page : doc.pages) {
        if (doc.pages.size() == 1 || !core::is_cover(*page)) out.push_back(page.get());
    }
    return out;
}

bool shown_with_parents(const core::Page& page, const core::Layer& layer) {
    if (!layer.visible) return false;
    Json parent = layer.parent_id;
    for (std::size_t guard = 0; parent.is_string() && guard <= page.layers.size(); ++guard) {
        const auto it = std::find_if(page.layers.begin(), page.layers.end(), [&parent](const core::Layer& l) { return l.id == parent.get<std::string>(); });
        if (it == page.layers.end()) break;
        if (!it->visible) return false;
        parent = it->parent_id;
    }
    return true;
}

}  // namespace

std::string processor_folder(std::string_view format) {
    const std::string f = processor_format(std::string(format));
    for (const auto& [ext, folder] : kProcessorFormats) {
        if (ext == f) return std::string(folder);
    }
    throw core::Error("value", "the image processor writes jpg, psd, tif and png");
}

Json processor_steps(const ImageProcessor& processor, const core::Document& doc) {
    Json out = Json::array();
    for (const core::Page* page : worked_pages(doc)) {
        const Json at = page->index.json();
        if (processor.to_srgb) {
            // (a mode convert_profile does not take made RGB first; then sRGB at the page's own depth)
            out.push_back(Json{{"op", "conditional_mode_change"}, {"page", at}, {"from", Json::array({"bitmap", "duotone", "indexed", "multichannel"})}, {"to", "rgb"}});
            out.push_back(Json{{"op", "convert_profile"}, {"page", at}, {"profile", "srgb"}});
        }
        if (processor.width_px || processor.height_px) {
            Json fit{{"op", "fit_image"}, {"page", at}, {"dont_enlarge", true}};
            if (processor.width_px) fit["width_px"] = *processor.width_px;
            if (processor.height_px) fit["height_px"] = *processor.height_px;
            out.push_back(std::move(fit));
        }
    }
    return out;
}

Output processor_output(const ImageProcessor& processor, const fs::path& folder) {
    if (processor.formats.empty()) throw core::Error("value", "the image processor writes at least one of jpg, psd, tif and png");
    if (processor.quality < 0 || processor.quality > 12) throw core::Error("value", "the quality is 0 to 12");
    if ((processor.width_px && (*processor.width_px < 1 || *processor.width_px > 100000)) ||
        (processor.height_px && (*processor.height_px < 1 || *processor.height_px > 100000))) {
        throw core::Error("value", "the image processor's width and height are 1 to 100000 px");
    }
    Output out;
    out.to = Output::To::Folder;
    std::vector<std::string> seen;
    for (const std::string& given : processor.formats) {
        const std::string f = processor_format(given);
        const std::string sub = processor_folder(f);
        if (std::find(seen.begin(), seen.end(), f) != seen.end()) throw core::Error("value", "the image processor writes each format once");
        seen.push_back(f);
        const std::optional<int> quality = f == "jpg" ? std::optional<int>(processor.quality) : std::nullopt;
        if (seen.size() == 1) {
            out.folder = folder / sub;
            out.format = f;
            out.quality = quality;
        } else {
            out.also.push_back(Output::Also{folder / sub, f, quality});
        }
    }
    return out;
}

Json lens_steps(const Json& settings, const core::Document& doc) {
    if (!settings.is_object()) throw core::Error("value", "the lens correction's settings are an object");
    Json out = Json::array();
    for (const core::Page* page : worked_pages(doc)) {
        std::vector<const core::Layer*> shown;
        for (const core::Layer& layer : page->layers) {
            if (layer.kind != core::LayerKind::Folder && layer.role != core::LayerRole::Name && layer.role != core::LayerRole::Draft &&
                shown_with_parents(*page, layer)) {
                shown.push_back(&layer);
            }
        }
        if (shown.empty()) continue;  // (nothing shows: nothing to correct)
        const Json at = page->index.json();
        // (more than one layer showing: flattened first, as Photoshop's — the layer it keeps is the lowest one shown)
        if (shown.size() > 1) out.push_back(Json{{"op", "merge_visible"}, {"page", at}, {"copy", false}, {"flatten", true}});
        Json lens{{"op", "filter_raster"}, {"page", at}, {"id", shown.front()->id}, {"kind", "lens_correction"}, {"profile", "auto"}};
        for (const auto& [key, value] : settings.items()) {
            if (key == "op" || key == "page" || key == "id" || key == "layer" || key == "kind" || key == "area" || key == "channel") {
                throw core::Error("value", "the lens correction's settings are its filter's (" + key + " is not one)");
            }
            lens[key] = value;
        }
        out.push_back(std::move(lens));
    }
    return out;
}

// --- PC5 I-e: ドロップレット ----------------------------------------------------------------------------------------------

namespace {

constexpr std::string_view kLauncherMark = "Genko droplet";

std::optional<std::string> text_at(const Json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) return std::nullopt;
    if (!it->is_string()) throw core::Error("value", std::string("a droplet's ") + key + " is text");
    return it->get<std::string>();
}

// A path in a launcher, as the shell reads it between single quotes.
std::string shell_quoted(const std::string& text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    return out + "'";
}

// An argument of a .desktop file's Exec (its quoting, then its string's escapes: the Desktop Entry Specification).
std::string desktop_quoted(const std::string& text) {
    std::string quoted = "\"";
    for (const char c : text) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    std::string out;
    for (const char c : quoted) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '%') {
            out += "%%";
        } else {
            out += c;
        }
    }
    return out;
}

// A .desktop file's string value: its escapes (no line ends).
std::string desktop_value(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += c;
        }
    }
    return out;
}

// A path a launcher names: never one holding a control character (a line end would start a line of its own in a
// .desktop file — the review of PC5 I-e, P2-1 —; refused for every kind alike).
void refuse_control(const std::string& text) {
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) throw core::Error("refused_target", "the launcher's path holds a control character: " + text);
    }
}

std::u16string utf16(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())).toStdU16String(); }

void put_u16(std::string& out, std::uint16_t v) {
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>((v >> 8) & 0xFF);
}

void put_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

void put_utf16(std::string& out, const std::u16string& text) {
    for (const char16_t c : text) put_u16(out, static_cast<std::uint16_t>(c));
}

// A Windows shortcut (MS-SHLLINK: its header, a LinkInfo of a local path — ANSI and Unicode —, its description and its
// arguments as StringData, the terminal block): `target` run with `arguments`. Explorer puts the files dropped on a
// shortcut after its arguments, each one argument of the program: no cmd.exe reads them (the review of PC5 I-e, P2-2).
std::string shell_link(const std::string& target, const std::string& arguments, const std::string& description) {
    const std::u16string target16 = utf16(target), arguments16 = utf16(arguments), description16 = utf16(description);
    if (arguments16.size() > 0xFFFF || description16.size() > 0xFFFF) throw core::Error("refused_target", "the launcher's path is too long");
    std::string out;
    // ShellLinkHeader
    put_u32(out, 0x4C);
    for (const unsigned char b : {0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}) out += static_cast<char>(b);
    put_u32(out, 0x02 | 0x04 | 0x20 | 0x80);  // (HasLinkInfo, HasName, HasArguments, IsUnicode)
    put_u32(out, 0);                          // (FileAttributes)
    out.append(24, '\0');                     // (CreationTime, AccessTime, WriteTime)
    put_u32(out, 0);                          // (FileSize)
    put_u32(out, 0);                          // (IconIndex)
    put_u32(out, 1);                          // (ShowCommand: SW_SHOWNORMAL)
    put_u16(out, 0);                          // (HotKey)
    out.append(10, '\0');                     // (Reserved1..3)
    // LinkInfo: a VolumeID of a fixed drive, the local path in ANSI ('?' for what is not ASCII) and in Unicode
    std::string ansi;
    for (const char16_t c : target16) ansi += c < 0x80 ? static_cast<char>(c) : '?';
    const std::uint32_t header = 0x24, volume = 17;
    const std::uint32_t base = header + volume;
    const std::uint32_t suffix = base + static_cast<std::uint32_t>(ansi.size()) + 1;
    const std::uint32_t base16 = suffix + 1;
    const std::uint32_t suffix16 = base16 + static_cast<std::uint32_t>(target16.size() + 1) * 2;
    const std::uint32_t size = suffix16 + 2;
    put_u32(out, size);
    put_u32(out, header);
    put_u32(out, 1);  // (VolumeIDAndLocalBasePath)
    put_u32(out, header);
    put_u32(out, base);
    put_u32(out, 0);  // (no network share)
    put_u32(out, suffix);
    put_u32(out, base16);
    put_u32(out, suffix16);
    put_u32(out, volume);
    put_u32(out, 3);     // (DRIVE_FIXED)
    put_u32(out, 0);     // (its serial number: unknown)
    put_u32(out, 0x10);  // (its label, empty, right after)
    out += '\0';
    out += ansi;
    out += '\0';
    out += '\0';  // (CommonPathSuffix: empty)
    put_utf16(out, target16);
    put_u16(out, 0);
    put_u16(out, 0);  // (CommonPathSuffixUnicode: empty)
    // StringData: NAME_STRING, COMMAND_LINE_ARGUMENTS
    put_u16(out, static_cast<std::uint16_t>(description16.size()));
    put_utf16(out, description16);
    put_u16(out, static_cast<std::uint16_t>(arguments16.size()));
    put_utf16(out, arguments16);
    put_u32(out, 0);  // (TerminalBlock)
    return out;
}

// Bytes written whole to `place` with the mode set before they take its name (a temporary file beside it, made runnable,
// renamed over it: no moment where a link put there gets the mode — the review's P3-2).
void write_runnable(const fs::path& place, const std::string& bytes, bool runnable) {
    const fs::path temporary = place.parent_path() / core::path_from_utf8("." + core::path_to_utf8(place.filename()) + ".genko-" +
                                                                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    storage::write_atomic(temporary, bytes);
    std::error_code ec;
#ifndef _WIN32
    if (runnable) {
        fs::permissions(temporary, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::replace, ec);
        if (ec) {
            fs::remove(temporary, ec);
            throw core::Error("io", "the launcher cannot be made runnable: " + core::path_to_utf8(place));
        }
    }
#else
    (void)runnable;
#endif
    fs::rename(temporary, place, ec);
    if (ec) {
        std::error_code e2;
        fs::remove(temporary, e2);
        throw core::Error("io", "the launcher cannot be written: " + core::path_to_utf8(place));
    }
}

}  // namespace

Json droplet_json(const Droplet& droplet) {
    Json out = Json::object();
    out["genkoDroplet"] = 1;
    out["name"] = droplet.name;
    out["steps"] = droplet.steps;
    if (droplet.recorded) out["recorded"] = true;
    Json options = Json::object();
    if (droplet.output) options["output"] = core::path_to_utf8(*droplet.output);
    if (droplet.format != "same") options["format"] = droplet.format;
    if (droplet.quality) options["quality"] = *droplet.quality;
    if (!droplet.prefix.empty()) options["prefix"] = droplet.prefix;
    if (!droplet.suffix.empty()) options["suffix"] = droplet.suffix;
    if (!options.empty()) out["options"] = options;
    return out;
}

Droplet droplet_from_json(const Json& json) {
    if (!json.is_object() || json.value("genkoDroplet", Json()) != Json(1)) throw core::Error("value", "not a Genko droplet (a .genkodroplet of version 1)");
    for (const auto& [key, value] : json.items()) {
        if (key != "genkoDroplet" && key != "name" && key != "steps" && key != "recorded" && key != "options") {
            throw core::Error("value", "a droplet holds no " + key + " (its name, its steps and its options only)");
        }
    }
    Droplet out;
    out.name = text_at(json, "name").value_or(std::string());
    check_script_name(out.name);
    const auto steps = json.find("steps");
    if (steps == json.end() || !steps->is_array()) throw core::Error("value", "a droplet holds its steps: \"steps\": [ …ops… ]");
    out.steps = read_steps(steps->dump());  // (each an op, at most kMaxSteps)
    if (const auto recorded = json.find("recorded"); recorded != json.end()) {
        if (!recorded->is_boolean()) throw core::Error("value", "a droplet's recorded is true or false");
        out.recorded = recorded->get<bool>();
    }
    if (const auto options = json.find("options"); options != json.end()) {
        if (!options->is_object()) throw core::Error("value", "a droplet's options are an object");
        for (const auto& [key, value] : options->items()) {
            if (key != "output" && key != "format" && key != "quality" && key != "prefix" && key != "suffix") {
                throw core::Error("value", "a droplet has no option " + key + " (output, format, quality, prefix, suffix)");
            }
        }
        if (const auto folder = text_at(*options, "output"); folder && !folder->empty()) out.output = core::path_from_utf8(*folder);
        out.format = text_at(*options, "format").value_or(std::string("same"));
        if (const auto q = options->find("quality"); q != options->end() && !q->is_null()) {
            if (!q->is_number_integer() || q->get<std::int64_t>() < 0 || q->get<std::int64_t>() > 12) throw core::Error("value", "the quality is 0 to 12");
            out.quality = static_cast<int>(q->get<std::int64_t>());
        }
        out.prefix = text_at(*options, "prefix").value_or(std::string());
        out.suffix = text_at(*options, "suffix").value_or(std::string());
    }
    Output check;
    check.format = out.format;
    check.quality = out.quality;
    check_format(check);
    return out;
}

Droplet read_droplet(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    if (!ec && size > kMaxStepsBytes) throw core::Error("value", "the droplet is larger than 1 MiB");
    std::string text;
    try {
        text = read_steps_bytes(file);  // (a regular file only)
    } catch (const StepsError&) {
        throw core::Error("value", "the droplet is larger than 1 MiB");
    }
    Json json;
    try {
        json = core::parse_python_json(text, nullptr, steps_json());
    } catch (const core::Error& error) {
        throw core::Error("value", std::string("not a Genko droplet (") + error.what() + ")");
    }
    Droplet out = droplet_from_json(json);
    if (out.name.empty()) out.name = core::path_to_utf8(file.stem());
    // (a folder written relative: from the droplet's own folder, not from wherever its launcher runs — the review's P3-4)
    if (out.output && out.output->is_relative()) out.output = (fs::absolute(file, ec).parent_path() / *out.output).lexically_normal();
    return out;
}

fs::path write_droplet(const fs::path& file, const Droplet& droplet) {
    fs::path place = file;
    if (lower(core::path_to_utf8(place.extension())) != kDropletSuffix) place += core::path_from_utf8(std::string(kDropletSuffix));
    refuse_inside_book(place);
    droplet_from_json(droplet_json(droplet));  // (written only as it will be read)
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(place, ec))) throw core::Error("refused_target", "cannot write over a link: " + core::path_to_utf8(place));
    storage::write_atomic(place, droplet_json(droplet).dump(1) + "\n");
    return place;
}

std::vector<Launcher> launchers_here() {
#ifdef _WIN32
    return {Launcher::Shortcut};
#else
    return {Launcher::Shell, Launcher::Desktop};
#endif
}

fs::path launcher_path(const fs::path& droplet, Launcher kind) {
    fs::path base = droplet;
    if (lower(core::path_to_utf8(base.extension())) == kDropletSuffix) base.replace_extension();
    const char* suffix = kind == Launcher::Shell ? ".sh" : kind == Launcher::Desktop ? ".desktop" : ".lnk";
    base += core::path_from_utf8(suffix);
    return base;
}

fs::path launcher_program(const fs::path& running) {
    // (an AppImage runs from a mount that is gone once it ends: the AppImage itself — the review's P3-3)
    const QByteArray image = qgetenv("APPIMAGE");
    if (!image.isEmpty()) {
        const fs::path file = core::path_from_utf8(image.toStdString());
        std::error_code ec;
        if (file.is_absolute() && fs::is_regular_file(file, ec)) return file;
    }
    return running;
}

std::string launcher_text(const fs::path& droplet, const fs::path& genko, Launcher kind) {
    const std::string target = core::path_to_utf8(droplet);
    const std::string program = core::path_to_utf8(genko);
    refuse_control(target);
    refuse_control(program);
    switch (kind) {
        case Launcher::Shell:
            return "#!/bin/sh\n# " + std::string(kLauncherMark) + ": runs the droplet on the files given (or dropped on it)\nexec " +
                   shell_quoted(program) + " droplet " + shell_quoted(target) + " \"$@\"\n";
        case Launcher::Desktop: {
            std::string name = core::path_to_utf8(droplet.stem());
            return "[Desktop Entry]\nType=Application\nName=" + desktop_value(name) + "\nComment=" + std::string(kLauncherMark) +
                   ": drop files on it\nExec=" + desktop_quoted(program) + " droplet " + desktop_quoted(target) + " %F\nTerminal=false\n";
        }
        case Launcher::Shortcut:
            // (a path on Windows never holds a ": the droplet's one argument quoted as it is)
            if (target.find('"') != std::string::npos) throw core::Error("refused_target", "the launcher's path holds a quotation mark: " + target);
            return shell_link(program, "droplet \"" + target + "\"", std::string(kLauncherMark) + ": drop files on it");
    }
    return {};
}

namespace {

bool is_launcher(const std::string& bytes) {
    if (bytes.find(kLauncherMark) != std::string::npos) return true;
    std::string wide;  // (a shortcut's description: UTF-16)
    put_utf16(wide, utf16(std::string(kLauncherMark)));
    return bytes.find(wide) != std::string::npos;
}

}  // namespace

std::vector<fs::path> write_launchers(const fs::path& droplet, const fs::path& genko) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(droplet, ec);
    const fs::path target = ec ? droplet : absolute.lexically_normal();
    refuse_inside_book(target);
    std::vector<std::pair<fs::path, std::string>> made;
    for (const Launcher kind : launchers_here()) {
        const fs::path place = launcher_path(target, kind);
        // (only a Genko launcher is written over: never a link, nor another file of that name)
        const fs::file_status status = fs::symlink_status(place, ec);
        if (fs::exists(status) || fs::is_symlink(status)) {
            bool ours = fs::is_regular_file(status) && !fs::is_symlink(status);
            if (ours) {
                try {
                    ours = is_launcher(storage::read_file_bounded(place, 64 * 1024));
                } catch (const std::exception&) {
                    ours = false;
                }
            }
            if (!ours) throw core::Error("refused_target", "cannot write over a file that is not a droplet's launcher: " + core::path_to_utf8(place));
        }
        made.emplace_back(place, launcher_text(target, genko, kind));
    }
    std::vector<fs::path> out;
    for (const auto& [place, text] : made) {
        write_runnable(place, text, true);
        out.push_back(place);
    }
    return out;
}

Output droplet_output(const Droplet& droplet, const std::vector<Input>& inputs, const std::optional<fs::path>& out) {
    Output output;
    output.to = Output::To::Folder;
    output.format = droplet.format;
    output.quality = droplet.quality;
    output.prefix = droplet.prefix;
    output.suffix = droplet.suffix;
    if (out) {
        output.folder = *out;
    } else if (droplet.output) {
        output.folder = *droplet.output;
    } else {
        if (inputs.empty()) throw core::Error("value", "no file to run the droplet on");
        fs::path first = inputs.front().path;
        if (first.filename().empty()) first = first.parent_path();
        output.folder = first.parent_path() / "droplet-output";
    }
    return output;
}

}  // namespace genko::service::automate
