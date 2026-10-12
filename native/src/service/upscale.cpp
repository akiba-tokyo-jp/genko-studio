#include "service/upscale.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/error.hpp"
#include "core/poses.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/pyre.hpp"
#include "formats/pillow_save.hpp"
#include "render/png.hpp"
#include "service/sheets.hpp"
#include "service/text.hpp"
#include "storage/fsutil.hpp"
#include "studio/files.hpp"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace genko::service::upscale {

namespace fs = std::filesystem;

namespace {

fs::path file() { return core::poses::config_dir() / "upscalers.json"; }

void write(const Json& data) {
    // (Path.write_text: the whole text, written in place as Python writes it)
    std::ofstream out(file(), std::ios::binary | std::ios::trunc);
    if (!out) throw core::PyUncaught("OSError", "cannot write " + file().generic_string());
    out << studio::text_mode(core::dump_python_indent2(data) + "\n");
    if (!out) throw core::PyUncaught("OSError", "cannot write " + file().generic_string());
}

}  // namespace

Json registered() {
    Json data;
    try {
        std::ifstream in(file(), std::ios::binary);
        if (!in) return Json::object();
        std::ostringstream text;
        text << in.rdbuf();
        core::ParseOptions options;
        options.universal_newlines = true;
        data = core::parse_python_json(text.str(), nullptr, options);
    } catch (const core::Error&) {
        return Json::object();
    }
    Json out = Json::object();
    if (!data.is_object()) return out;
    for (const auto& [key, value] : data.items()) {
        if (value.is_object() && value.contains("command") && value["command"].is_array()) out[key] = value;
    }
    return out;
}

Json available() {
    Json out = Json::array();
    out.push_back(Json::object({{"name", kBuiltin}, {"label", "Genko の拡大（Lanczos と線の輪郭の整え。描き込みは増えない）"}, {"scales", "1〜4"}}));
    const Json data = registered();
    std::vector<std::string> names;
    for (const auto& [key, value] : data.items()) names.push_back(key);
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const Json& spec = data[name];
        out.push_back(Json::object({{"name", name},
                                    {"label", core::py_or(core::py_get(spec, "label"), Json(name))},
                                    {"scales", core::py_or(core::py_get(spec, "scales"), Json("any"))}}));
    }
    return out;
}

Json register_program(const std::string& name, const Json& command, const std::vector<double>& scales, const std::string& label) {
    static const core::PyPattern other("[^\\w.-]", 0);  // (each character isalnum() or one of "-_.")
    core::PyReBudget budget;
    if (name.empty() || name == kBuiltin || other.count(name, budget) > 0) {
        throw core::PyValueError(std::string("名前は英数字と - _ .（") + kBuiltin + " 以外）");
    }
    std::vector<std::string> parts;
    if (command.is_string()) {
        parts = shlex_split(command.get<std::string>());
    } else {
        for (const Json& p : core::iterate(command)) parts.push_back(core::py_str(p));
    }
    const auto any = [&parts](const char* mark) {
        return std::any_of(parts.begin(), parts.end(), [mark](const std::string& p) { return p.find(mark) != std::string::npos; });
    };
    if (parts.empty() || !any("{in}") || !any("{out}")) throw core::PyValueError("コマンドには {in}（元の画像）と {out}（拡大した画像）を入れる");
    Json data = registered();
    Json entry = Json::object();
    entry["command"] = Json::array();
    for (const std::string& p : parts) entry["command"].push_back(p);
    if (!scales.empty()) {
        entry["scales"] = Json::array();
        for (double s : scales) entry["scales"].push_back(s);
    }
    if (!label.empty()) entry["label"] = label;
    data[name] = entry;
    std::error_code ec;
    fs::create_directories(file().parent_path(), ec);
    write(data);
    return data[name];
}

bool unregister(const std::string& name) {
    Json data = registered();
    if (!data.contains(name)) return false;
    data.erase(name);
    write(data);
    return true;
}

render::Image builtin(const render::Image& image, double scale, int threads) {
    const std::string_view m = image.mode();
    const std::string mode = (m == "RGBA" || m == "LA" || m == "P") ? "RGBA" : (m == "L" || m == "1") ? "L" : "RGB";
    const render::Image source = image.convert(mode);
    const render::Size size{std::max(1, static_cast<int>(core::py_round_int(source.width() * scale))),
                            std::max(1, static_cast<int>(core::py_round_int(source.height() * scale)))};
    // (in bands of rows on several threads: the same pixels as resize and filter, render/image.hpp)
    render::Image big = source.resize_in_bands(size, render::Resample::Lanczos, std::nullopt, threads);
    if (scale > 1.05) {
        const render::Filter sharpen = render::Filter::unsharp_mask(core::py_max(1.0, scale * 0.7), 90, 2);
        if (mode == "RGBA") {
            const render::Image rgb = big.convert("RGB").filter_in_bands(sharpen, threads);
            const render::Image alpha = big.getchannel("A");
            big = rgb.convert("RGBA");
            big.putalpha(alpha);
        } else {
            big = big.filter_in_bands(sharpen, threads);
        }
    }
    return big;
}

namespace {

std::string repr_list(const std::vector<std::string>& items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) out += (i == 0 ? "" : ", ") + core::py_repr_str(items[i]);
    return out + "]";
}

// The OSError subprocess.run raises for a program that cannot be started, in Python's words where they are known.
std::string start_failure(const std::string& program, const QString& qt_error) {
    const QString name = QString::fromStdString(program);
    const bool has_dir = name.contains(QLatin1Char('/'));
    const QString found = has_dir ? name : QStandardPaths::findExecutable(name);
    if (found.isEmpty() || !QFileInfo::exists(found)) return "[Errno 2] No such file or directory: " + core::py_repr_str(program);
    if (!QFileInfo(found).isExecutable() || QFileInfo(found).isDir()) return "[Errno 13] Permission denied: " + core::py_repr_str(program);
    return qt_error.toStdString();
}

}  // namespace

render::Image external(const std::string& name, const render::Image& image, double scale) {
    const Json all = registered();
    if (!all.contains(name)) {
        std::string names;
        for (const Json& a : available()) names += (names.empty() ? "" : ", ") + core::py_str(a["name"]);
        throw core::PyValueError("高解像度化の道具 " + name + " は登録されていない（" + names + "）");
    }
    const Json spec = all[name];
    QTemporaryDir tmp(QDir::tempPath() + QStringLiteral("/genko-up-XXXXXX"));
    if (!tmp.isValid()) throw core::PyUncaught("OSError", "cannot make a temporary folder");
    const std::string src = (tmp.path() + QStringLiteral("/in.png")).toStdString();
    const std::string dst = (tmp.path() + QStringLiteral("/out.png")).toStdString();
    storage::write_atomic(core::path_from_utf8(src), formats::png_bytes(image));
    const bool whole = scale == std::floor(scale) && std::isfinite(scale);
    const std::string scale_word = whole ? std::to_string(static_cast<std::int64_t>(scale)) : core::py_format_g(scale);
    std::vector<std::string> cmd;
    for (const Json& part : core::iterate(spec["command"])) {
        std::string text = core::py_str(part);
        for (const auto& [mark, value] : {std::pair<std::string, std::string>{"{in}", src}, {"{out}", dst}, {"{scale}", scale_word}}) {
            for (std::size_t at = text.find(mark); at != std::string::npos; at = text.find(mark, at + value.size())) text.replace(at, mark.size(), value);
        }
        cmd.push_back(text);
    }
    if (cmd.empty()) throw core::PyUncaught("IndexError", "list index out of range");
    QProcess process;
    QStringList arguments;
    for (std::size_t i = 1; i < cmd.size(); ++i) arguments << QString::fromStdString(cmd[i]);
    process.start(QString::fromStdString(cmd[0]), arguments);
    if (!process.waitForStarted()) throw core::PyValueError(name + " を動かせなかった（" + start_failure(cmd[0], process.errorString()) + "）");
    if (!process.waitForFinished(kTimeoutSeconds * 1000)) {
        process.kill();
        process.waitForFinished();
        throw core::PyValueError(name + " を動かせなかった（Command '" + repr_list(cmd) + "' timed out after " +
                                 std::to_string(kTimeoutSeconds) + " seconds）");
    }
    const std::string out_text = QString::fromUtf8(process.readAllStandardOutput()).toStdString();
    const std::string err_text = QString::fromUtf8(process.readAllStandardError()).toStdString();
    std::error_code ec;
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !std::filesystem::is_regular_file(core::path_from_utf8(dst), ec)) {
        throw core::PyValueError(name + " が失敗した: " + text::head(core::py_strip(err_text.empty() ? out_text : err_text), 300));
    }
    const render::Image made = open_picture(storage::read_file(core::path_from_utf8(dst)));
    const std::string_view m = image.mode();
    const render::Image result = made.convert((m == "RGB" || m == "RGBA" || m == "L") ? m : std::string_view("RGB"));
    const render::Size wanted{std::max(1, static_cast<int>(core::py_round_int(image.width() * scale))),
                              std::max(1, static_cast<int>(core::py_round_int(image.height() * scale)))};
    return result.size() == wanted ? result : result.resize(wanted, render::Resample::Lanczos);
}

render::Image enlarge(const render::Image& image, double scale, const std::string& method) {
    if (!(1.0 <= scale && scale <= kMaxScale)) throw core::PyValueError("scale は 1〜" + core::py_format_g(kMaxScale));
    return method == kBuiltin ? builtin(image, scale) : external(method, image, scale);
}

std::vector<std::string> shlex_split(std::string_view text) {
    // shlex.shlex(text, posix=True) with whitespace_split=True and no comments
    std::vector<std::string> out;
    std::string token;
    bool in_token = false;
    std::size_t i = 0;
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (i < text.size()) {
        const char c = text[i];
        if (space(c)) {
            if (in_token) {
                out.push_back(token);
                token.clear();
                in_token = false;
            }
            ++i;
        } else if (c == '\\') {
            if (i + 1 >= text.size()) throw core::PyValueError("No escaped character");
            token += text[i + 1];
            in_token = true;
            i += 2;
        } else if (c == '\'') {
            const std::size_t end = text.find('\'', i + 1);
            if (end == std::string_view::npos) throw core::PyValueError("No closing quotation");
            token += text.substr(i + 1, end - i - 1);
            in_token = true;
            i = end + 1;
        } else if (c == '"') {
            ++i;
            bool closed = false;
            while (i < text.size()) {
                if (text[i] == '"') {
                    closed = true;
                    ++i;
                    break;
                }
                if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '"' || text[i + 1] == '\\')) {
                    token += text[i + 1];
                    i += 2;
                    continue;
                }
                if (text[i] == '\\' && i + 1 >= text.size()) throw core::PyValueError("No escaped character");
                token += text[i];
                ++i;
            }
            if (!closed) throw core::PyValueError("No closing quotation");
            in_token = true;
        } else {
            token += c;
            in_token = true;
            ++i;
        }
    }
    if (in_token) out.push_back(token);
    return out;
}

}  // namespace genko::service::upscale
