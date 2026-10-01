#include "api/cli.hpp"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "api/buildinfo.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/ops_schema.hpp"
#include "core/paths.hpp"
#include "render/brushes.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"

namespace genko::api {

namespace {

void write_text(const std::string& text, std::FILE* to) {
    std::fwrite(text.data(), 1, text.size(), to);
    std::fflush(to);
}

void write_json(const nlohmann::json& value, std::FILE* to = stdout) {
    write_text(value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n", to);
}

// Python's _print_json: json.dumps(payload, ensure_ascii=…) and a newline, on stdout.
void print_json(const core::Json& value, bool ascii) { write_text(core::dump_python(value, ascii) + "\n", stdout); }

int usage(const std::string& message) {
    write_json({{"ok", false}, {"error", message}, {"code", "usage"}}, stderr);
    return 2;
}

// {"ok": false, "error": …, "code": …} on stdout, as Python's __main__ prints its failures (plus the code,
// docs/cpp-migration/ARCHITECTURE.md §8).
int fail(const std::string& message, const std::string& code, bool ascii, int exit_code) {
    core::Json out = core::Json::object();
    out["ok"] = false;
    out["error"] = message;
    out["code"] = code;
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
    const auto loaded = storage::load_document(storage::path_from_utf8(pathlib_text(src.toStdString())));
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
    render::brushes::register_brushes(doc.brush_custom);  // (the book's own brushes, as Python's reader registers them)
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

int schema(const QStringList& args, bool ascii) {
    if (!args.isEmpty()) return usage("unrecognized arguments: " + args.join(QLatin1Char(' ')).toStdString());
    core::Json out = core::Json::object();
    out["ok"] = true;
    out["ops"] = core::ops_schema();
    print_json(out, ascii);
    return 0;
}

}  // namespace

int run_cli(int argc, char** argv) {
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
        if (command == QLatin1String("render")) return render_command(args, ascii);
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
        const std::string message = error.code() == "not_found" ? "not found: " + std::string(error.what()) : error.what();
        return fail(message, error.code(), ascii, 1);
    } catch (const std::exception& error) {
        return fail(error.what(), "internal", ascii, 1);
    }
}

}  // namespace genko::api
