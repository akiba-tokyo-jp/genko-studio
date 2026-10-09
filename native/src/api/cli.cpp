#include "api/cli.hpp"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "api/buildinfo.hpp"
#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/json.hpp"
#include "core/ops_schema.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "formats/export.hpp"
#include "render/anim.hpp"
#include "render/brushes.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/timelapse.hpp"
#include "storage/doctor.hpp"
#include "storage/fsutil.hpp"
#include "storage/gc.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/migrate.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"
#include "storage/transaction.hpp"
#include "storage/undo.hpp"

namespace genko::api {

namespace {

namespace fs = std::filesystem;
using core::Json;

void write_text(const std::string& text, std::FILE* to) {
    std::fwrite(text.data(), 1, text.size(), to);
    std::fflush(to);
}

void write_json(const nlohmann::json& value, std::FILE* to = stdout) {
    write_text(value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n", to);
}

// Python's _print_json: json.dumps(payload, ensure_ascii=…) and a newline, on stdout.
void print_json(const core::Json& value, bool ascii) { write_text(core::dump_python(value, ascii) + "\n", stdout); }

int usage(const std::string& message, int exit_code = 2) {
    write_json({{"ok", false}, {"error", message}, {"code", "usage"}}, stderr);
    return exit_code;
}

// {"ok": false, "error": …, "code": …} on stdout, as Python's __main__ prints its failures (plus the code,
// docs/cpp-migration/ARCHITECTURE.md §8).
int fail(const std::string& message, const std::string& code, bool ascii, int exit_code, const Json* report = nullptr) {
    core::Json out = core::Json::object();
    out["ok"] = false;
    out["error"] = message;
    out["code"] = code;
    if (report != nullptr) out["report"] = *report;
    print_json(out, ascii);
    return exit_code;
}

// The path as Python's pathlib writes it ("a//b/./c/" → "a/b/c", "" → "."), so error messages name the same file.
std::string pathlib_text(const std::string& text) {
    const bool absolute = !text.empty() && text.front() == '/';
    std::vector<std::string> parts;
    std::string part;
    for (const char c : text + "/") {
        if (c == '/') {
            if (!part.empty() && part != ".") parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    std::string out = absolute ? "/" : "";
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i == 0 ? "" : "/") + parts[i];
    if (out.empty()) out = ".";
    return out;
}

fs::path path_arg(const QString& arg) { return storage::path_from_utf8(pathlib_text(arg.toStdString())); }

// --- arguments (argparse's rules: options anywhere, "--name value" or "--name=value", unique prefixes) ---------

struct Option {
    const char* name;  // "--title"
    bool takes_value;
};

struct Arguments {
    std::vector<QString> positional;
    std::map<std::string, QString> values;
    std::set<std::string> flags;

    bool flag(const char* name) const { return flags.contains(name); }
    std::optional<QString> value(const char* name) const {
        const auto it = values.find(name);
        return it == values.end() ? std::nullopt : std::optional<QString>(it->second);
    }
};

bool negative_number(const QString& text) {
    bool ok = false;
    text.toDouble(&ok);
    return ok && text.startsWith(QLatin1Char('-'));
}

// The error message, or nothing when the arguments are right.
std::optional<std::string> parse_arguments(const QStringList& args, std::span<const Option> options,
                                           std::size_t positionals, const char* names, Arguments& out) {
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (!arg.startsWith(QLatin1String("--")) || arg.size() == 2) {
            if (arg.startsWith(QLatin1Char('-')) && arg != QLatin1String("-") && !negative_number(arg)) {
                return "unrecognized arguments: " + arg.toStdString();
            }
            out.positional.push_back(arg);
            continue;
        }
        const qsizetype equals = arg.indexOf(QLatin1Char('='));
        const std::string given = (equals < 0 ? arg : arg.left(equals)).toStdString();
        const Option* match = nullptr;
        std::vector<const Option*> prefixed;
        for (const Option& option : options) {
            if (given == option.name) match = &option;
            if (std::string_view(option.name).starts_with(given)) prefixed.push_back(&option);
        }
        if (match == nullptr) {
            if (prefixed.size() > 1) {
                std::string could;
                for (const Option* option : prefixed) could += (could.empty() ? "" : ", ") + std::string(option->name);
                return "ambiguous option: " + given + " could match " + could;
            }
            if (prefixed.empty()) return "unrecognized arguments: " + arg.toStdString();
            match = prefixed.front();
        }
        if (!match->takes_value) {
            if (equals >= 0) return "argument " + std::string(match->name) + ": ignored explicit argument '" + arg.mid(equals + 1).toStdString() + "'";
            out.flags.insert(match->name);
            continue;
        }
        if (equals >= 0) {
            out.values[match->name] = arg.mid(equals + 1);
            continue;
        }
        if (i + 1 >= args.size() ||
            (args[i + 1].startsWith(QLatin1Char('-')) && args[i + 1] != QLatin1String("-") && !negative_number(args[i + 1]))) {
            return "argument " + std::string(match->name) + ": expected one argument";
        }
        out.values[match->name] = args[++i];
    }
    if (out.positional.size() < positionals) return std::string("the following arguments are required: ") + names;
    if (out.positional.size() > positionals) return "unrecognized arguments: " + out.positional[positionals].toStdString();
    return std::nullopt;
}

// int(value) for an option (argparse's type=int).
std::optional<std::int64_t> int_option(const QString& text) {
    try {
        return core::py_int(Json(text.toStdString()));
    } catch (const core::Error&) {
        return std::nullopt;
    }
}

std::string read_stdin() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    std::string out;
    char buf[1 << 16];
    for (;;) {
        const std::size_t got = std::fread(buf, 1, sizeof buf, stdin);
        if (got == 0) break;
        out.append(buf, got);
    }
    return out;
}

core::ParseOptions text_file() {
    core::ParseOptions options;
    options.universal_newlines = true;
    return options;
}

// A v1–v3 book is never written: found before anything is touched (no project.lock is made in it).
void refuse_legacy(const fs::path& dir) {
    const int version = storage::project_version(core::parse_python_json(storage::read_file(dir / "project.json"), nullptr, text_file()));
    if (version < 4) throw storage::NeedsMigration(dir, version);
}

// Python's _is_studio_project: an agent's book (strict gates, studio state, or M0 sidecars) does not trust an unnamed
// caller as a person.
bool is_studio_project(const fs::path& dir) {
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

// --- commands ----------------------------------------------------------------------------------------------------

int inspect(const QStringList& args, bool ascii) {
    QString src;
    bool have_src = false;
    bool full = false;
    QString stroke;
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (arg == QLatin1String("--full")) {
            full = true;
        } else if (arg == QLatin1String("--stroke")) {
            if (i + 1 >= args.size()) return usage("argument --stroke: expected one argument");
            stroke = args[++i];
        } else if (arg.startsWith(QLatin1String("--stroke="))) {
            stroke = arg.mid(9);
        } else if (arg.startsWith(QLatin1Char('-')) && arg.size() > 1) {
            return usage("unrecognized arguments: " + arg.toStdString());
        } else if (!have_src) {
            src = arg;
            have_src = true;
        } else {
            return usage("unrecognized arguments: " + arg.toStdString());
        }
    }
    if (!have_src) return usage("genko inspect <dir> [--full] [--stroke ID]: the following arguments are required: src");
    const auto loaded = storage::load_document(path_arg(src));
    if (!stroke.isEmpty()) {
        print_json(storage::inspect_stroke(loaded.document, stroke.toStdString()), ascii);
    } else {
        print_json(storage::snapshot(loaded.document, full), ascii);
    }
    return 0;
}

// argparse's type=int: int(text), with spaces around and "_" between digits allowed.
std::optional<std::int64_t> py_int_arg(const QString& raw) {
    const std::string text = raw.trimmed().toStdString();
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
    if (i >= text.size()) return std::nullopt;
    std::int64_t value = 0;
    bool digit_before = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '_') {
            if (!digit_before || i + 1 >= text.size() || text[i + 1] < '0' || text[i + 1] > '9') return std::nullopt;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        if (value > (std::numeric_limits<std::int64_t>::max() - (c - '0')) / 10) return std::nullopt;
        value = value * 10 + (c - '0');
        digit_before = true;
    }
    return negative ? -value : value;
}

// `genko render <dir> --page N [--dpi D] [--mode print|proof|name] --out file.png` (Python's render command): the
// page drawn as `python -m genko render` draws it, written as PNG; {"ok": true, "path", "mode"}.
int render_command(const QStringList& args, bool ascii) {
    QString src;
    bool have_src = false;
    QString out;
    bool have_out = false;
    std::int64_t page_number = 1;
    std::int64_t dpi = 150;
    std::string mode = "print";
    const auto value_of = [&](qsizetype& i, const QString& arg, const QString& flag, QString& value) {
        if (arg == flag) {
            if (i + 1 >= args.size()) return 1;
            value = args[++i];
            return 2;
        }
        if (arg.startsWith(flag + QLatin1Char('='))) {
            value = arg.mid(flag.size() + 1);
            return 2;
        }
        return 0;
    };
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        QString value;
        int found = 0;
        if ((found = value_of(i, arg, QStringLiteral("--page"), value)) != 0) {
            if (found == 1) return usage("argument --page: expected one argument");
            const auto n = py_int_arg(value);
            if (!n) return usage("argument --page: invalid int value: '" + value.toStdString() + "'");
            page_number = *n;
        } else if ((found = value_of(i, arg, QStringLiteral("--dpi"), value)) != 0) {
            if (found == 1) return usage("argument --dpi: expected one argument");
            const auto n = py_int_arg(value);
            if (!n) return usage("argument --dpi: invalid int value: '" + value.toStdString() + "'");
            dpi = *n;
        } else if ((found = value_of(i, arg, QStringLiteral("--mode"), value)) != 0) {
            if (found == 1) return usage("argument --mode: expected one argument");
            mode = value.toStdString();
            if (mode != "name" && mode != "proof" && mode != "print") {
                return usage("argument --mode: invalid choice: '" + mode + "' (choose from 'name', 'proof', 'print')");
            }
        } else if ((found = value_of(i, arg, QStringLiteral("--out"), value)) != 0) {
            if (found == 1) return usage("argument --out: expected one argument");
            out = value;
            have_out = true;
        } else if (arg.startsWith(QLatin1Char('-')) && arg.size() > 1) {
            return usage("unrecognized arguments: " + arg.toStdString());
        } else if (!have_src) {
            src = arg;
            have_src = true;
        } else {
            return usage("unrecognized arguments: " + arg.toStdString());
        }
    }
    if (!have_src || !have_out) {
        std::string missing;
        if (!have_src) missing = "src";
        if (!have_out) missing += std::string(missing.empty() ? "" : ", ") + "--out";
        return usage("genko render <dir> --page N [--dpi D] [--mode print|proof|name] --out file.png: the following "
                     "arguments are required: " + missing);
    }
    // (Python takes any int: a page is at least one pixel; one too large to hold is refused while drawing)
    if (dpi < std::numeric_limits<int>::min() || dpi > std::numeric_limits<int>::max()) {
        return fail("the page is too large at this resolution", "image_too_large", ascii, 1);
    }
    const std::string out_text = pathlib_text(out.toStdString());
    const std::filesystem::path out_path = storage::path_from_utf8(out_text);
    // the format from the file's extension, as Pillow's Image.save chooses it
    std::string ext = core::path_to_utf8(out_path.extension());
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".png") {
        static const char* const kPillowSaves[] = {".jpg", ".jpeg", ".jpe", ".jfif", ".tif", ".tiff", ".bmp", ".dib", ".gif",
                                                   ".webp", ".pdf", ".ppm", ".pgm", ".pbm", ".tga", ".ico", ".im", ".pcx", ".eps",
                                                   ".j2k", ".jp2", ".jpx", ".icns", ".sgi", ".spi", ".xbm", ".qoi", ".avif"};
        if (std::find(std::begin(kPillowSaves), std::end(kPillowSaves), ext) == std::end(kPillowSaves)) {
            return fail("unknown file extension: " + ext, "value", ascii, 1);
        }
        throw render::NotYetPorted("format:" + ext.substr(1));
    }

    const auto loaded = storage::load_document(storage::path_from_utf8(pathlib_text(src.toStdString())));
    const core::Document& doc = loaded.document;
    render::brushes::clear_custom();
    render::brushes::register_book(doc.brush_custom);  // (the book's own brushes, as Python's reader registers them)
    const core::Page* page = nullptr;
    for (const auto& p : doc.pages) {
        if (p->index == core::Num(page_number)) {
            page = p.get();
            break;
        }
    }
    if (page == nullptr) return fail("no page " + std::to_string(page_number), "not_found", ascii, 1);
    render::RenderOptions options;
    options.mode = mode;
    const render::RenderResult result = render::render_page(*page, static_cast<int>(dpi), options, &doc);
    const std::string bytes = render::write_png(result.image);
    if (out_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(out_path.parent_path(), ec);
    }
    storage::write_atomic(out_path, bytes);
    core::Json json = core::Json::object();
    json["ok"] = true;
    json["path"] = out_text;
    json["mode"] = mode;
    print_json(json, ascii);
    return 0;
}

// `genko export <book> <out> [--format F] [options]` (Python's export command, genko/__main__.py): the book written
// out in one format; the paths of the files one a line, or with --json {"ok": true, "count", "files"}.
int export_command(const QStringList& args, bool ascii) {
    static constexpr Option kOptions[] = {{"--json", false},      {"--dpi", true},       {"--format", true},     {"--width", true},
                                          {"--max-height", true}, {"--long-edge", true}, {"--jpeg", false},      {"--spreads", false},
                                          {"--color", true},      {"--icc", true},       {"--screen-lpi", true}, {"--screen-shape", true},
                                          {"--area", true},       {"--fps", true},       {"--seconds", true},    {"--page", true}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 2, "src, out", a)) return usage("genko export: " + *error);
    // argparse's choices
    const auto choice = [&](const char* name, const char* fallback, std::initializer_list<const char*> choices) -> std::optional<std::string> {
        const std::string value = a.value(name).value_or(QString::fromLatin1(fallback)).toStdString();
        for (const char* c : choices) {
            if (value == c) return value;
        }
        return std::nullopt;
    };
    const auto invalid_choice = [&](const char* name, std::initializer_list<const char*> choices) {
        std::string listed;
        for (const char* c : choices) listed += std::string(listed.empty() ? "" : ", ") + "'" + c + "'";
        return usage("genko export: argument " + std::string(name) + ": invalid choice: " + core::py_repr_str(a.value(name)->toStdString()) +
                     " (choose from " + listed + ")");
    };
    const std::initializer_list<const char*> kFormats{"png", "tiff",   "pdf",    "strip",  "psd",       "epub",     "pack",
                                                      "webtoon", "sns", "cmyk", "layers", "kindle", "timelapse", "animation"};
    const std::initializer_list<const char*> kColours{"rgb", "cmyk", "gray"};
    const std::initializer_list<const char*> kShapes{"round", "square", "diamond", "ellipse"};
    const std::initializer_list<const char*> kAreas{"paper", "bleed", "trim"};
    const auto fmt = choice("--format", "png", kFormats);
    if (!fmt) return invalid_choice("--format", kFormats);
    const auto color = choice("--color", "rgb", kColours);
    if (!color) return invalid_choice("--color", kColours);
    const auto shape = choice("--screen-shape", "round", kShapes);
    if (!shape) return invalid_choice("--screen-shape", kShapes);
    const auto area = choice("--area", "paper", kAreas);
    if (!area) return invalid_choice("--area", kAreas);
    std::map<std::string, std::optional<std::int64_t>> ints;
    for (const char* name : {"--dpi", "--width", "--max-height", "--long-edge", "--page"}) {
        if (const auto value = a.value(name)) {
            const auto n = int_option(*value);
            if (!n) return usage("genko export: argument " + std::string(name) + ": invalid int value: " + core::py_repr_str(value->toStdString()));
            ints[name] = *n;
        }
    }
    std::map<std::string, std::optional<double>> floats;
    for (const char* name : {"--screen-lpi", "--fps", "--seconds"}) {
        if (const auto value = a.value(name)) {
            try {
                floats[name] = core::py_float(Json(value->toStdString()));
            } catch (const core::Error&) {
                return usage("genko export: argument " + std::string(name) + ": invalid float value: " + core::py_repr_str(value->toStdString()));
            }
        }
    }
    const auto int_or = [&](const char* name, std::int64_t fallback) { return ints[name].value_or(fallback); };
    const auto narrow = [](std::int64_t v) {  // (pixels or dots past what a picture can hold: refused as drawing refuses them)
        if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
            throw core::Error("image_too_large", "the picture is too large at this resolution");
        }
        return static_cast<int>(v);
    };
    const std::optional<std::int64_t> dpi = ints["--dpi"] && *ints["--dpi"] != 0 ? ints["--dpi"] : std::nullopt;  // (args.dpi or …)

    const fs::path src = path_arg(a.positional[0]);
    const fs::path out = path_arg(a.positional[1]);
    const bool file_given = !formats::detail::suffix(out).empty();  // (args.out.suffix)
    const auto loaded = storage::load_document(src);
    const core::Document& doc = loaded.document;
    render::brushes::clear_custom();
    render::brushes::register_book(doc.brush_custom);  // (the book's own brushes, as Python's reader registers them)
    const std::int64_t spec_dpi = doc.spec.dpi.truthy() ? core::py_int(doc.spec.dpi) : 0;
    std::vector<fs::path> paths;
    const std::string& f = *fmt;
    // (the resolution of a format that takes one, 1 to 100000 dpi: refused before anything is drawn or made)
    if (f != "webtoon" && f != "sns" && f != "kindle" && f != "timelapse") {
        const std::int64_t fallback = f == "strip" || f == "epub" ? 150 : f == "animation" ? 100 : f == "layers" ? spec_dpi : spec_dpi != 0 ? spec_dpi : 600;
        formats::check_dpi(dpi.value_or(fallback));
    }
    if (f == "strip") {
        paths = {formats::export_strip(doc, file_given ? out : formats::detail::join(out, "strip.png"), narrow(dpi.value_or(150)))};
    } else if (f == "psd") {
        // a folder gets one layered PSD per page; a .psd path gets the first page
        if (file_given) {
            paths = {formats::export_psd(doc, out, dpi.value_or(spec_dpi))};
        } else {
            paths = formats::export_psd_pages(doc, out, dpi);
        }
    } else if (f == "epub") {
        paths = {formats::export_epub(doc, file_given ? out : formats::detail::join(out, "out.epub"), narrow(dpi.value_or(150)))};
    } else if (f == "webtoon") {
        paths = formats::export_webtoon(doc, out, narrow(int_or("--width", 800)), narrow(int_or("--max-height", 1280)), 0,
                                        a.flag("--jpeg") ? "jpeg" : "png");
    } else if (f == "sns") {
        paths = formats::export_sns(doc, out, narrow(int_or("--long-edge", 2048)), a.flag("--jpeg") ? "jpeg" : "png", 92, a.flag("--spreads"));
    } else if (f == "pack") {
        paths = formats::export_pack(doc, out, "shueisha", dpi);
    } else if (f == "layers") {
        paths = formats::export_layers(doc, out, narrow(dpi.value_or(spec_dpi)), *area);
    } else if (f == "kindle") {
        const std::int64_t given = int_or("--long-edge", 2048);
        const std::int64_t long_edge = given != 2048 ? given : formats::kKindleLongEdge;
        paths = {formats::export_kindle(doc, file_given ? out : formats::detail::join(out, "kindle.epub"), narrow(long_edge))};
    } else if (f == "animation") {
        const std::int64_t wanted = ints["--page"] && *ints["--page"] != 0 ? *ints["--page"] : 1;  // (args.page or 1)
        const core::Page* page = nullptr;
        for (const auto& p : doc.pages) {
            if (p->index == core::Num(wanted)) {
                page = p.get();
                break;
            }
        }
        if (page == nullptr) {  // (Python: SystemExit with these words, exit 1)
            return usage("no page " + (ints["--page"] ? std::to_string(*ints["--page"]) : std::string("None")), 1);
        }
        const fs::path dest = file_given ? out : formats::detail::join(out, "p" + formats::detail::padded(page->index, 3) + ".gif");
        paths = render::anim::export_animation(*page, dest, &doc, narrow(dpi.value_or(100)));
    } else if (f == "timelapse") {
        std::optional<Json> page;
        if (ints["--page"]) page = Json(*ints["--page"]);
        paths = {render::timelapse::export_timelapse(src, file_given ? out : formats::detail::join(out, "timelapse.webp"), page,
                                                     floats["--fps"].value_or(12.0), floats["--seconds"])};
    } else {
        std::optional<Json> screen;
        if (floats["--screen-lpi"] && *floats["--screen-lpi"] != 0.0) {
            screen = Json::object();
            (*screen)["lpi"] = *floats["--screen-lpi"];
            (*screen)["shape"] = *shape;
        }
        paths = formats::export_print(doc, out, f, dpi, 180, true, *area, f == "cmyk" ? std::string("cmyk") : *color,
                                      a.value("--icc").value_or(QString()).toStdString(), screen);
    }
    if (a.flag("--json")) {
        Json files = Json::array();
        for (const auto& p : paths) files.push_back(core::path_to_utf8(p));
        Json json = Json::object();
        json["ok"] = true;
        json["count"] = static_cast<std::int64_t>(paths.size());
        json["files"] = files;
        print_json(json, ascii);
    } else {
        std::string lines;
        for (const auto& p : paths) lines += core::path_to_utf8(p) + "\n";
        write_text(lines, stdout);
    }
    return 0;
}

int schema(const QStringList& args, bool ascii) {
    if (!args.isEmpty()) return usage("unrecognized arguments: " + args.join(QLatin1Char(' ')).toStdString());
    core::Json out = core::Json::object();
    out["ok"] = true;
    out["ops"] = core::ops_schema();
    print_json(out, ascii);
    return 0;
}

int new_book(const QStringList& args, bool ascii) {
    static constexpr Option kOptions[] = {{"--title", true},  {"--episode", true}, {"--pages", true}, {"--webtoon", false},
                                          {"--b4", false},    {"--preset", true},  {"--paper", true}, {"--json", false},
                                          {"--plain", false}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 1, "dest", a)) return usage("genko new: " + *error);
    const auto episode = int_option(a.value("--episode").value_or(QStringLiteral("1")));
    if (!episode) return usage("genko new: argument --episode: invalid int value: " + core::py_repr_str(a.value("--episode")->toStdString()));
    const auto pages = int_option(a.value("--pages").value_or(QStringLiteral("8")));
    if (!pages) return usage("genko new: argument --pages: invalid int value: " + core::py_repr_str(a.value("--pages")->toStdString()));
    core::PageSpec spec;
    const std::string paper = a.value("--paper").value_or(QString()).toStdString();
    if (!paper.empty()) {
        const core::PaperPreset* preset = core::find_paper_preset(paper);
        if (preset == nullptr) {
            std::string names;
            for (const auto& item : core::paper_presets()) names += (names.empty() ? "" : ", ") + std::string(item.key);
            return usage("--paper must be one of " + names, 1);  // (Python: SystemExit with this text, exit 1)
        }
        spec = preset->make();
    } else if (a.flag("--webtoon")) {
        spec = core::PageSpec::webtoon();
    } else if (const auto preset = a.value("--preset"); preset && !preset->isEmpty()) {
        spec = core::PageSpec::publisher(preset->toStdString());
    } else if (a.flag("--b4")) {
        spec = core::PageSpec::b4_comic();
    } else {
        spec = core::PageSpec::a4_mono();
    }
    const std::string dest_text = pathlib_text(a.positional[0].toStdString());
    const fs::path dest = storage::path_from_utf8(dest_text);
    std::error_code ec;
    if (fs::exists(dest / "project.json", ec)) {
        return fail("a book is there already: " + storage::path_to_utf8(dest / "project.json") +
                        " (genko new never writes over a book)",
                    "exists", ascii, 1);
    }
    const auto page_count = static_cast<int>(std::clamp<std::int64_t>(*pages, -1, 100000));
    core::Document doc = core::new_episode(a.value("--title").value_or(QStringLiteral("無題")).toStdString(),
                                           core::Num(*episode), page_count, spec);
    storage::ProjectLock lock(dest, std::string(core::kLegacyActor));
    lock.try_acquire();
    if (fs::exists(dest / "project.json", ec)) {
        return fail("a book is there already: " + storage::path_to_utf8(dest / "project.json"), "exists", ascii, 1);
    }
    storage::SaveRequest request;
    request.actor = std::string(core::kLegacyActor);
    request.base_revision = 0;
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
    lock.release();
    if (a.flag("--plain")) {
        write_text(dest_text + "\n", stdout);
    } else {
        Json out = Json::object();
        out["ok"] = true;
        out["path"] = dest_text;
        out["snapshot"] = storage::snapshot(doc);
        print_json(out, ascii);
    }
    return 0;
}

int apply(const QStringList& args, bool ascii) {
    static constexpr Option kOptions[] = {{"--dry-run", false}, {"--expect-revision", true}, {"--agent", true}, {"--txn", true}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 2, "src, ops", a)) return usage("genko apply: " + *error);
    std::optional<std::int64_t> expect;
    if (const auto value = a.value("--expect-revision")) {
        expect = int_option(*value);
        if (!expect) return usage("genko apply: argument --expect-revision: invalid int value: " + core::py_repr_str(value->toStdString()));
    }
    const bool dry_run = a.flag("--dry-run");
    const std::string txn = a.value("--txn").value_or(QString()).toStdString();
    if (!txn.empty() && !core::is_txn_id(txn)) return usage("genko apply: --txn takes 32 lowercase hex digits");

    // the ops first, as Python reads them
    const QString source = a.positional[1];
    const std::string raw = source == QLatin1String("-") ? read_stdin() : storage::read_file(path_arg(source));
    core::ParseRepairs repairs;
    const Json ops = core::parse_python_json(raw, &repairs, text_file());
    if (!repairs.nonfinite.empty()) {
        throw core::ApplyError("ops must not hold NaN or Infinity (at " + repairs.nonfinite.front() + ")");
    }
    if (!ops.is_array()) throw core::ApplyError("ops file must be a JSON array");

    const fs::path dir = path_arg(a.positional[0]);
    const std::string agent =
        a.value("--agent").value_or(QString()).isEmpty()
            ? (is_studio_project(dir) ? std::string("legacy:unknown") : std::string(core::kLegacyActor))
            : a.value("--agent")->toStdString();
    refuse_legacy(dir);
    storage::ProjectLock lock(dir, agent);
    lock.try_acquire();
    storage::journal::repair(dir);  // (the book is read at its last consistent point)
    // Read inside the lock, so a concurrent writer's changes are never overwritten.
    const storage::LoadResult loaded = storage::load_document(dir);
    if (loaded.report.source_version < 4) throw storage::NeedsMigration(dir, loaded.report.source_version);

    // A retry of a transaction saved before gets its revision back, whatever revision the book has reached since
    // (schema-v4 §4.3); then the base revision is checked.
    if (!txn.empty() && !dry_run) {
        const auto txns = storage::journal::transactions(storage::journal::read_lines(storage::journal::journal_file(dir)));
        if (const auto* done = storage::journal::find_committed(txns, txn)) {
            Json out = Json::object();  // (this transaction was saved before: it is not applied again)
            out["ok"] = true;
            Json applied = Json::array();
            for (const Json& op : ops) {
                const auto name = op.is_object() ? op.find("op") : op.end();
                applied.push_back(op.is_object() && name != op.end() ? core::py_str(*name) : std::string("None"));
            }
            out["applied"] = applied;
            out["snapshot"] = storage::snapshot(loaded.document);
            out["job_id"] = core::new_id();
            out["warnings"] = Json::array();
            out["revision"] = done->rev;
            out["txn"] = txn;
            out["already_committed"] = true;
            print_json(out, ascii);
            return 0;
        }
    }
    if (expect && loaded.document.revision != *expect) throw storage::RevisionConflict(*expect, loaded.document.revision);

    // [{"op": "undo"}] on its own undoes the latest saved change, as `genko undo --as <agent>` does (the same checks:
    // another actor's change, approvals that need a person, an outside edit). Python's apply_ops undoes its session's
    // changes here; a book's saved changes are in its journal. (A dry run only shows the book, as Python's does.)
    const bool undo_op = ops.size() == 1 && ops[0].is_object() && ops[0].contains("op") && ops[0]["op"] == Json("undo");
    if (undo_op && !dry_run) {
        const storage::RestoreResult restored = storage::restore(lock, agent, false, false, txn);
        const storage::LoadResult after = storage::load_document(dir);
        Json out = Json::object();
        out["ok"] = true;
        out["applied"] = Json::array({"undo"});
        out["snapshot"] = storage::snapshot(after.document);
        out["job_id"] = core::new_id();
        out["kind"] = restored.kind;
        out["rev"] = restored.rev;
        out["revision"] = restored.revision;
        out["txn"] = restored.txn;
        lock.release();
        print_json(out, ascii);
        return 0;
    }

    const core::CommandBus bus(render::ops_registry());
    const core::ApplyResult result = bus.apply(loaded.document, ops, core::Actor(agent), dry_run);
    Json out = Json::object();
    out["ok"] = true;
    out["applied"] = result.applied;
    out["snapshot"] = storage::snapshot(result.doc);
    out["job_id"] = core::new_id();
    if (result.has_warnings) out["warnings"] = result.warnings;
    if (!result.results.empty()) out["results"] = result.results;
    if (dry_run) {
        out["revision"] = loaded.document.revision;
    } else {
        storage::SaveRequest request;
        request.actor = agent;
        request.base_revision = loaded.document.revision;
        request.ops = result.journal_ops;
        request.txn = txn;
        const bool lapse = render::timelapse::is_on(result.doc);  // (タイムラプス: the pages this save changes)
        Json before;
        if (lapse) {
            try {
                before = storage::read_disk_state(dir).payload;
            } catch (const std::exception&) {  // (the timelapse never stops a save: every page is recorded then)
            }
        }
        const storage::SaveResult saved = storage::Saver(lock).save(result.doc, request);
        if (lapse && !saved.already_committed) render::timelapse::after_save(dir, result.doc, before);
        out["revision"] = saved.revision;
        out["txn"] = saved.txn;
        if (saved.repaired) out["repaired"] = true;
    }
    lock.release();
    print_json(out, ascii);
    return 0;
}

int restore(const QStringList& args, bool ascii, bool redo) {
    static constexpr Option kOptions[] = {{"--as", true}, {"--force", false}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 1, "src", a)) return usage(std::string(redo ? "genko redo: " : "genko undo: ") + *error);
    const fs::path dir = path_arg(a.positional[0]);
    const std::string actor = a.value("--as").value_or(QStringLiteral("genko")).toStdString();
    refuse_legacy(dir);
    print_json(storage::restore(dir, actor, redo, a.flag("--force")).to_json(), ascii);
    return 0;
}

int gc(const QStringList& args, bool ascii) {
    static constexpr Option kOptions[] = {{"--dry-run", false}, {"--legacy", false}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 1, "src", a)) return usage("genko gc: " + *error);
    const fs::path dir = path_arg(a.positional[0]);
    refuse_legacy(dir);
    print_json(storage::gc(dir, a.flag("--dry-run"), std::string(core::kLegacyActor), a.flag("--legacy")), ascii);
    return 0;
}

int doctor(const QStringList& args, bool ascii) {
    Arguments a;
    if (auto error = parse_arguments(args, {}, 1, "src", a)) return usage("genko doctor: " + *error);
    const Json report = storage::doctor(path_arg(a.positional[0]));
    print_json(report, ascii);
    return report["ok"].get<bool>() ? 0 : 1;
}

int migrate(const QStringList& args, bool ascii) {
    static constexpr Option kOptions[] = {{"--as", true}, {"--accept-repairs", false}};
    Arguments a;
    if (auto error = parse_arguments(args, kOptions, 2, "src, dst", a)) return usage("genko migrate: " + *error);
    const std::string actor = a.value("--as").value_or(QStringLiteral("genko")).toStdString();
    try {
        print_json(storage::convert(path_arg(a.positional[0]), path_arg(a.positional[1]), actor, a.flag("--accept-repairs")),
                   ascii);
    } catch (const storage::ConvertError& error) {
        return fail(error.what(), error.code(), ascii, 1, &error.report());
    }
    return 0;
}

}  // namespace

int run_cli(int argc, char** argv) {
#ifdef _WIN32
    // UTF-8 with "\n" line ends on every OS (ARCHITECTURE.md §8): no CRLF translation on Windows.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);  // (arguments in UTF-8 on Windows too)
    QStringList args = QCoreApplication::arguments().mid(1);
    const bool ascii = args.removeAll(QStringLiteral("--ascii")) > 0;
    if (args.isEmpty()) return usage("genko <command> — try `genko --version`");
    const QString command = args.takeFirst();
    try {
        if (command == QLatin1String("--version") || command == QLatin1String("version")) {
            write_json({{"ok", true}, {"build", build_info()}});
            return 0;
        }
        if (command == QLatin1String("inspect")) return inspect(args, ascii);
        if (command == QLatin1String("schema")) return schema(args, ascii);
        if (command == QLatin1String("new")) return new_book(args, ascii);
        if (command == QLatin1String("apply")) return apply(args, ascii);
        if (command == QLatin1String("undo")) return restore(args, ascii, false);
        if (command == QLatin1String("redo")) return restore(args, ascii, true);
        if (command == QLatin1String("gc")) return gc(args, ascii);
        if (command == QLatin1String("doctor")) return doctor(args, ascii);
        if (command == QLatin1String("migrate")) return migrate(args, ascii);
        if (command == QLatin1String("render")) return render_command(args, ascii);
        if (command == QLatin1String("export")) return export_command(args, ascii);
        return usage("unknown command: " + command.toStdString());
    } catch (const storage::UnsupportedProjectVersion& error) {
        return fail(error.what(), error.code(), ascii, 2);
    } catch (const render::NotYetPorted& error) {
        // (what the page carries that this build does not draw yet: nothing is written)
        core::Json out = core::Json::object();
        out["ok"] = false;
        out["error"] = error.what();
        out["code"] = error.code();
        out["element"] = error.element();
        print_json(out, ascii);
        return 1;
    } catch (const core::Error& error) {
        if (error.code() == "needs_migration") return fail(error.what(), error.code(), ascii, 3);
        const std::string message = error.code() == "not_found" ? "not found: " + std::string(error.what()) : error.what();
        return fail(message, error.code(), ascii, 1);
    } catch (const std::exception& error) {
        return fail(error.what(), "internal", ascii, 1);
    }
}

}  // namespace genko::api
