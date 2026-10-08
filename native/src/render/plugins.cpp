#include "render/plugins.hpp"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/poses.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "storage/fsutil.hpp"

namespace genko::render::plugins {

namespace {

using core::Json;

// Genko's plugin runner: one plugin, one request (describe | run), in its own process. What the plugin prints goes to
// stderr; the reply is one JSON line on stdout, then the picture's bytes.
constexpr const char* kRunner = R"PY(
import importlib.util
import json
import sys
from pathlib import Path


def main():
    out = sys.stdout.buffer
    sys.stdout = sys.stderr  # (a plugin's prints never reach the reply)
    what, path = sys.argv[1], Path(sys.argv[2])
    stem = path.stem

    def reply(header, data=b""):
        out.write(json.dumps(header, ensure_ascii=True).encode("ascii") + b"\n")
        out.write(data)
        out.flush()

    try:
        from PIL import Image
    except ImportError:
        reply({"ok": False, "runner": True, "error": "plugins need Pillow in the Python that runs them (pip install Pillow)"})
        return
    try:
        spec = importlib.util.spec_from_file_location(f"genko_plugin_{stem}", path)
        if spec is None or spec.loader is None:
            raise ValueError(f"plugin {stem} cannot be loaded")
        module = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(module)
        except Exception as exc:
            raise ValueError(f"plugin {stem} cannot be loaded ({exc})") from exc
        if not callable(getattr(module, "run", None)):
            raise ValueError(f"plugin {stem} has no run(image)")
        if what == "describe":
            params = getattr(module, "PARAMS", {}) or {}
            params = params if isinstance(params, dict) else {}
            reply({"ok": True, "name": str(getattr(module, "NAME", stem)), "params": json.loads(json.dumps(params, default=str))})
            return
        header = json.loads(sys.stdin.buffer.readline())
        size = (int(header["width"]), int(header["height"]))
        rgba = Image.frombytes("RGBA", size, sys.stdin.buffer.read(size[0] * size[1] * 4))
        try:
            result = module.run(rgba.copy(), **(header.get("params") or {}))
        except Exception as exc:
            raise ValueError(f"plugin {stem} failed ({exc})") from exc
        if not isinstance(result, Image.Image):
            raise ValueError(f"plugin {stem} did not return a picture")
        if result.size != rgba.size:
            result = result.resize(rgba.size)
        if result.mode != "RGBA":
            alpha = rgba.getchannel("A")
            result = result.convert("RGBA")
            result.putalpha(alpha)
        reply({"ok": True, "width": size[0], "height": size[1]}, result.tobytes())
    except ValueError as exc:
        reply({"ok": False, "error": str(exc)})


main()
)PY";

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string sha256_of(const std::filesystem::path& path) {
    const std::string bytes = read_file(path);
    return QCryptographicHash::hash(QByteArray::fromRawData(bytes.data(), static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

std::filesystem::path settings_file() { return core::poses::config_dir() / "plugin_settings.json"; }

// plugins.run's checks on the name: a file of that name in the folder.
std::filesystem::path plugin_file(const std::string& key) {
    std::error_code ec;
    const std::filesystem::path path = folder() / core::path_from_utf8(key + ".py");
    if (key.empty() || key.find('/') != std::string::npos || key.find('\\') != std::string::npos ||
        !std::filesystem::is_regular_file(path, ec)) {
        throw core::PyValueError("no plugin " + key);
    }
    return path;
}

void require_allowed(const std::string& key) {
    if (!allowed(key)) {
        throw core::Error("plugin_not_allowed",
                          "plugin " + key + " is not chosen to run (turn plugins on and choose it in the plugin settings)");
    }
}

struct Reply {
    Json header;
    std::string data;
};

// One request to the runner, bounded in time and in what it may write back.
Reply ask_runner(const std::string& key, const std::filesystem::path& plugin, const char* what, const std::string& input,
                 std::chrono::seconds limit, std::size_t most) {
    const auto py = python();
    if (!py) throw core::Error("plugin_runner", "plugins need Python on this computer (none was found: choose it in the plugin settings)");
    QTemporaryDir dir;
    if (!dir.isValid()) throw core::Error("io", "cannot make a folder for the plugin runner");
    const std::filesystem::path runner = core::path_from_utf8(dir.filePath(QStringLiteral("genko_plugin_runner.py")).toStdString());
    {
        std::ofstream file(runner, std::ios::binary | std::ios::trunc);
        file << kRunner;
        if (!file) throw core::Error("io", "cannot write the plugin runner");
    }
    QProcess process;
    process.setProgram(QString::fromStdString(core::path_to_utf8(*py)));
    process.setArguments({QString::fromStdString(core::path_to_utf8(runner)), QString::fromLatin1(what),
                          QString::fromStdString(core::path_to_utf8(plugin))});
    process.setWorkingDirectory(dir.path());
    const QProcessEnvironment outside = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment env;
    for (const char* name : {"PATH", "HOME", "USERPROFILE", "LANG", "LC_ALL", "LC_CTYPE", "SYSTEMROOT", "TEMP", "TMP", "TMPDIR"}) {
        const QString n = QString::fromLatin1(name);
        if (outside.contains(n)) env.insert(n, outside.value(n));
    }
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    process.setProcessEnvironment(env);
    process.start();
    if (!process.waitForStarted(10000)) {
        throw core::PyValueError("plugin " + key + " failed (the runner could not be started: " + process.errorString().toStdString() + ")");
    }
    if (!input.empty()) process.write(input.data(), static_cast<qint64>(input.size()));
    process.closeWriteChannel();
    QElapsedTimer clock;
    clock.start();
    std::string out;
    std::string err;
    const auto drain = [&] {
        const QByteArray o = process.readAllStandardOutput();
        out.append(o.constData(), static_cast<std::size_t>(o.size()));
        const QByteArray e = process.readAllStandardError();
        if (err.size() < 65536) err.append(e.constData(), static_cast<std::size_t>(std::min<qsizetype>(e.size(), 65536)));
    };
    while (process.state() != QProcess::NotRunning) {
        if (process.bytesToWrite() > 0) process.waitForBytesWritten(50);
        process.waitForFinished(50);
        drain();
        if (out.size() > most) {
            process.kill();
            process.waitForFinished(5000);
            throw core::PyValueError("plugin " + key + " failed (it wrote back more than a picture)");
        }
        if (clock.elapsed() > std::chrono::duration_cast<std::chrono::milliseconds>(limit).count()) {
            process.kill();
            process.waitForFinished(5000);
            throw core::PyValueError("plugin " + key + " failed (it did not finish in " + std::to_string(limit.count()) + " seconds)");
        }
    }
    drain();
    const auto line = out.find('\n');
    Json header;
    if (line != std::string::npos) {
        try {
            header = core::parse_python_json(out.substr(0, line));
        } catch (const core::Error&) {
        }
    }
    if (!header.is_object()) {
        std::string tail = err.size() > 300 ? err.substr(err.size() - 300) : err;
        while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) tail.pop_back();
        throw core::PyValueError("plugin " + key + " failed (the runner stopped" + (tail.empty() ? std::string() : ": " + tail) + ")");
    }
    if (!core::py_truthy(core::py_get(header, "ok"))) {
        const std::string message = core::py_str(core::py_get(header, "error", "the runner refused"));
        if (core::py_truthy(core::py_get(header, "runner"))) throw core::Error("plugin_runner", message);
        throw core::PyValueError(message);
    }
    return Reply{std::move(header), out.substr(line + 1)};
}

// The last pictures made (a page drawn in tiles asks a correction layer's plugin for the same picture again and again).
struct Made {
    std::string request;  // sha256 of the key, the file, the settings and the pixels
    Image picture;
};
std::mutex g_made_mutex;
std::vector<Made> g_made;
constexpr std::size_t kMadeKept = 4;

std::mutex g_described_mutex;
std::map<std::string, std::pair<std::string, Described>> g_described;  // key → (the file's sha256, what it said)

}  // namespace

std::filesystem::path folder() { return core::poses::config_dir() / "plugins"; }

std::vector<Listed> listed() {
    std::vector<Listed> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(folder(), ec)) return out;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder(), ec)) {
        const std::string name = core::path_to_utf8(entry.path().filename());
        if (entry.path().extension() != ".py" || name.starts_with("_") || !entry.is_regular_file(ec)) continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    const Settings now = settings();
    for (const auto& path : files) {
        Listed item;
        item.key = core::path_to_utf8(path.stem());
        item.sha256 = sha256_of(path);
        const std::filesystem::path manifest = path.parent_path() / core::path_from_utf8(item.key + ".json");
        if (std::filesystem::is_regular_file(manifest, ec)) {
            try {
                Json data = core::parse_python_json(read_file(manifest));
                if (data.is_object()) item.manifest = std::move(data);
            } catch (const core::Error&) {
            }
        }
        const auto chosen = now.chosen.find(item.key);
        item.chosen = chosen != now.chosen.end() && *chosen == Json(item.sha256);
        out.push_back(std::move(item));
    }
    return out;
}

Settings settings() {
    Settings out;
    try {
        const std::string text = read_file(settings_file());
        if (text.empty()) return out;
        const Json data = core::parse_python_json(text);
        if (!data.is_object()) return out;
        out.enabled = data.value("enabled", false) == true;
        if (data.contains("python") && data["python"].is_string()) out.python = data["python"].get<std::string>();
        if (data.contains("chosen") && data["chosen"].is_object()) out.chosen = data["chosen"];
    } catch (const core::Error&) {
    }
    return out;
}

void save_settings(const Settings& s) {
    Json data = Json::object();
    data["enabled"] = s.enabled;
    data["python"] = s.python;
    data["chosen"] = s.chosen.is_object() ? s.chosen : Json::object();
    core::DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    storage::write_atomic(settings_file(), core::dump(data, options));
}

void choose(const std::string& key, bool on) {
    Settings s = settings();
    if (on) {
        s.chosen[key] = sha256_of(plugin_file(key));
    } else {
        s.chosen.erase(key);
    }
    save_settings(s);
}

bool allowed(const std::string& key) {
    const Settings s = settings();
    if (!s.enabled) return false;
    const auto chosen = s.chosen.find(key);
    if (chosen == s.chosen.end() || !chosen->is_string()) return false;
    try {
        return chosen->get<std::string>() == sha256_of(plugin_file(key));
    } catch (const core::Error&) {
        return false;
    }
}

std::optional<std::filesystem::path> python() {
    const Settings s = settings();
    if (!s.python.empty()) {
        std::error_code ec;
        const std::filesystem::path given = core::path_from_utf8(s.python);
        if (std::filesystem::is_regular_file(given, ec)) return given;
        const QString found = QStandardPaths::findExecutable(QString::fromStdString(s.python));
        if (!found.isEmpty()) return core::path_from_utf8(found.toStdString());
        return std::nullopt;
    }
    for (const char* name : {"python3", "python"}) {
        const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!found.isEmpty()) return core::path_from_utf8(found.toStdString());
    }
    return std::nullopt;
}

Described describe(const std::string& key) {
    const std::filesystem::path path = plugin_file(key);
    require_allowed(key);
    const std::string sha = sha256_of(path);
    {
        std::lock_guard lock(g_described_mutex);
        if (const auto found = g_described.find(key); found != g_described.end() && found->second.first == sha) return found->second.second;
    }
    const Reply reply = ask_runner(key, path, "describe", {}, kDescribeTime, 1 << 20);
    Described out;
    out.key = key;
    out.name = core::py_str(core::py_get(reply.header, "name", key));
    const Json params = core::py_get(reply.header, "params", Json::object());
    out.params = params.is_object() ? params : Json::object();
    std::lock_guard lock(g_described_mutex);
    g_described[key] = {sha, out};
    return out;
}

std::vector<Described> available() {
    std::vector<Described> out;
    if (!settings().enabled) return out;
    for (const Listed& item : listed()) {
        if (!item.chosen) continue;
        try {
            out.push_back(describe(item.key));
        } catch (const std::exception&) {
            // (a plugin that cannot be read is left out, as Python leaves out a broken one)
        }
    }
    return out;
}

std::vector<std::tuple<std::string, std::string, double, double, double>> fields(std::string_view kind) {
    std::vector<std::tuple<std::string, std::string, double, double, double>> out;
    const std::string key(kind.starts_with(kPrefix) ? kind.substr(kPrefix.size()) : kind);
    std::optional<Described> found;
    for (const Described& d : available()) {
        if (d.key == key) found = d;
    }
    if (!found) return out;
    try {
        for (const auto& [name, raw] : found->params.items()) {
            const Json spec = raw.is_object() ? raw : Json::object();
            const double lo = core::to_float(core::py_get(spec, "min", 0));
            out.emplace_back(name, core::py_str(core::py_get(spec, "label", name)), lo, core::to_float(core::py_get(spec, "max", 100)),
                             core::to_float(core::py_get(spec, "default", core::py_get(spec, "min", 0))));
        }
    } catch (const core::Error&) {
        return {};
    }
    return out;
}

Image run(std::string_view kind, const Image& image, const Json& params) {
    const std::string key(kind.starts_with(kPrefix) ? kind.substr(kPrefix.size()) : kind);
    const std::filesystem::path path = plugin_file(key);
    require_allowed(key);
    const Image rgba = image.mode() == "RGBA" ? image : image.convert("RGBA");
    Json header = Json::object();
    header["width"] = rgba.width();
    header["height"] = rgba.height();
    header["params"] = params.is_object() ? params : Json::object();
    std::string input = core::dump(header, core::DumpOptions{}) + "\n";
    input += rgba.tobytes();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::fromStdString(key + "\n" + sha256_of(path) + "\n"));
    hash.addData(QByteArray::fromRawData(input.data(), static_cast<qsizetype>(input.size())));
    const std::string request = hash.result().toHex().toStdString();
    {
        std::lock_guard lock(g_made_mutex);
        for (const Made& made : g_made) {
            if (made.request == request) return made.picture;
        }
    }
    const std::size_t pixels = static_cast<std::size_t>(rgba.width()) * static_cast<std::size_t>(rgba.height()) * 4;
    const Reply reply = ask_runner(key, path, "run", input, kRunTime, pixels + 65536);
    if (core::py_get(reply.header, "width") != Json(rgba.width()) || core::py_get(reply.header, "height") != Json(rgba.height()) ||
        reply.data.size() != pixels) {
        throw core::PyValueError("plugin " + key + " did not return a picture");
    }
    Image picture = Image::frombytes("RGBA", rgba.size(), reply.data);
    std::lock_guard lock(g_made_mutex);
    g_made.push_back(Made{request, picture});
    if (g_made.size() > kMadeKept) g_made.erase(g_made.begin());
    return picture;
}

}  // namespace genko::render::plugins
