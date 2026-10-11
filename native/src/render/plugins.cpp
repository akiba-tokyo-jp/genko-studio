#include "render/plugins.hpp"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/statvfs.h>
#endif

#include <algorithm>
#include <deque>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>

#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/poses.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/page.hpp"
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
    what, path, home = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
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
        module.__file__ = home  # (the code run is the copy chosen; files beside the plugin are found where it lives)
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

std::string sha256_bytes(const std::string& bytes) {
    return QCryptographicHash::hash(QByteArray::fromRawData(bytes.data(), static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

std::string sha256_of(const std::filesystem::path& path) { return sha256_bytes(read_file(path)); }

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

// The plugin's code as it was chosen: read once, and only these bytes are run (a file changed since is refused).
struct Code {
    std::string bytes;
    std::string sha256;
};
Code chosen_code(const std::string& key) {
    const std::filesystem::path path = plugin_file(key);
    Code code{read_file(path), {}};
    code.sha256 = sha256_bytes(code.bytes);
    const Settings s = settings();
    const auto chosen = s.chosen.find(key);
    if (!s.enabled || chosen == s.chosen.end() || !chosen->is_string() || chosen->get<std::string>() != code.sha256) {
        throw core::Error("plugin_not_allowed",
                          "plugin " + key + " is not chosen to run (turn plugins on and choose it in the plugin settings)");
    }
    return code;
}

struct Reply {
    Json header;
    std::string data;
};

// The runner's own trouble (no Python, not started, stopped, too slow, too much written back): never the plugin's
// answer, so a correction layer is not quietly left as it was — refused for output, left out (and said) on screen.
[[noreturn]] void runner_trouble(const std::string& message) { throw core::Error("plugin_runner", message); }

// The variables a plugin's process gets: a short list (the environment's secrets stay behind). (Windows: APPDATA and
// LOCALAPPDATA are where `pip install --user` puts Pillow)
QProcessEnvironment plugin_environment() {
    const QProcessEnvironment outside = QProcessEnvironment::systemEnvironment();
    QProcessEnvironment env;
    for (const char* name : {"PATH", "HOME", "USERPROFILE", "APPDATA", "LOCALAPPDATA", "LANG", "LC_ALL", "LC_CTYPE", "SYSTEMROOT", "TEMP", "TMP",
                             "TMPDIR"}) {
        const QString n = QString::fromLatin1(name);
        if (outside.contains(n)) env.insert(n, outside.value(n));
    }
    return env;
}

// A plugin's process starts a process group of its own (POSIX), so that stopping it stops what it started too.
// (Windows: only the process itself is stopped — what it started itself runs on; that would need a Job Object.)
void own_process_group(QProcess& process) {
#ifdef Q_OS_UNIX
    process.setChildProcessModifier([] { (void)::setpgid(0, 0); });
#else
    (void)process;
#endif
}

// One request to a started plugin process (the runner, or a native plugin's program), bounded in time and in what it
// may write back; `input` streamed to it in pieces after `header`. Its reply: one JSON object on a line, then the
// picture's bytes. What it writes to stderr: the last 64 KB kept (their end said when it stops before answering).
Reply exchange(QProcess& process, const std::string& key, const std::string& header, const std::string& input, std::chrono::seconds limit,
               std::size_t most, const std::stop_token& stop) {
    const auto stop_now = [&process] {
#ifdef Q_OS_UNIX
        const qint64 pid = process.processId();
        if (pid > 0) (void)::kill(-static_cast<pid_t>(pid), SIGKILL);  // (its group: what it started too)
#endif
        process.kill();
        process.waitForFinished(5000);
    };
    QElapsedTimer clock;
    clock.start();
    std::string out;
    std::string err;
    const auto drain = [&] {
        const QByteArray o = process.readAllStandardOutput();
        out.append(o.constData(), static_cast<std::size_t>(o.size()));
        const QByteArray e = process.readAllStandardError();
        err.append(e.constData(), static_cast<std::size_t>(e.size()));
        if (err.size() > 65536) err.erase(0, err.size() - 65536);
    };
    const auto check = [&] {
        if (stop.stop_requested()) {
            stop_now();
            throw Cancelled();
        }
        if (out.size() > most) {
            stop_now();
            runner_trouble("plugin " + key + " wrote back more than a picture");
        }
        if (clock.elapsed() > std::chrono::duration_cast<std::chrono::milliseconds>(limit).count()) {
            stop_now();
            runner_trouble("plugin " + key + " did not finish in " + std::to_string(limit.count()) + " seconds");
        }
    };
    // the request in pieces (only a few of them waiting in the pipe at a time: the pixels are not copied again)
    constexpr std::size_t kPiece = 1 << 20;
    std::size_t sent = 0;
    if (!header.empty()) process.write(header.data(), static_cast<qint64>(header.size()));
    while (sent < input.size() && process.state() != QProcess::NotRunning) {
        if (process.bytesToWrite() < static_cast<qint64>(4 * kPiece)) {
            const std::size_t n = std::min(kPiece, input.size() - sent);
            process.write(input.data() + sent, static_cast<qint64>(n));
            sent += n;
        }
        process.waitForBytesWritten(50);
        drain();
        check();
    }
    process.closeWriteChannel();
    while (process.state() != QProcess::NotRunning) {
        if (process.bytesToWrite() > 0) process.waitForBytesWritten(50);
        process.waitForFinished(50);
        drain();
        check();
    }
    drain();
    check();
    const auto line = out.find('\n');
    Json reply;
    if (line != std::string::npos) {
        try {
            reply = core::parse_python_json(std::string_view(out).substr(0, line));
        } catch (const core::Error&) {
        }
    }
    if (!reply.is_object()) {
        std::string tail = err.size() > 300 ? err.substr(err.size() - 300) : err;
        while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r')) tail.pop_back();
        runner_trouble("plugin " + key + " stopped before it answered" + (tail.empty() ? std::string() : " (" + tail + ")"));
    }
    out.erase(0, line + 1);
    return Reply{std::move(reply), std::move(out)};
}

// One request to the runner. The plugin's code goes to the runner's own folder as the bytes chosen (never the file as
// it is by then).
Reply ask_runner(const std::string& key, const Code& code, const char* what, const std::string& header, const std::string& input,
                 std::chrono::seconds limit, std::size_t most, const std::stop_token& stop) {
    const auto py = python();
    if (!py) runner_trouble("plugins need Python on this computer (none was found: choose it in the plugin settings)");
    QTemporaryDir dir;
    if (!dir.isValid()) throw core::Error("io", "cannot make a folder for the plugin runner");
    const std::filesystem::path runner = core::path_from_utf8(dir.filePath(QStringLiteral("genko_plugin_runner.py")).toStdString());
    const std::filesystem::path plugin = core::path_from_utf8(dir.filePath(QStringLiteral("plugin")).toStdString()) / core::path_from_utf8(key + ".py");
    {
        std::error_code ec;
        std::filesystem::create_directories(plugin.parent_path(), ec);
        std::ofstream file(runner, std::ios::binary | std::ios::trunc);
        file << kRunner;
        std::ofstream copy(plugin, std::ios::binary | std::ios::trunc);
        copy.write(code.bytes.data(), static_cast<std::streamsize>(code.bytes.size()));
        if (!file || !copy) throw core::Error("io", "cannot write the plugin runner");
    }
    QProcess process;
    process.setProgram(QString::fromStdString(core::path_to_utf8(*py)));
    process.setArguments({QString::fromStdString(core::path_to_utf8(runner)), QString::fromLatin1(what),
                          QString::fromStdString(core::path_to_utf8(plugin)), QString::fromStdString(core::path_to_utf8(plugin_file(key)))});
    process.setWorkingDirectory(dir.path());
    QProcessEnvironment env = plugin_environment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    process.setProcessEnvironment(env);
    own_process_group(process);
    process.start();
    if (!process.waitForStarted(10000)) {
        runner_trouble("plugin " + key + " could not be started (" + process.errorString().toStdString() + ")");
    }
    Reply reply = exchange(process, key, header, input, limit, most, stop);
    if (!core::py_truthy(core::py_get(reply.header, "ok"))) {
        const std::string message = core::py_str(core::py_get(reply.header, "error", "the runner refused"));
        if (core::py_truthy(core::py_get(reply.header, "runner"))) runner_trouble(message);
        throw core::PyValueError(message);  // (the plugin's own errors: Python's ValueError)
    }
    return reply;
}

// --- native plugins (docs/PLUGIN_NATIVE.md) ---------------------------------------------------------------------------

constexpr std::uintmax_t kMostManifestBytes = 64 * 1024;
constexpr std::uintmax_t kMostProgramBytes = std::uintmax_t{256} << 20;
constexpr std::int64_t kProtocol = 1;

std::filesystem::path native_home(const std::string& key) { return folder() / core::path_from_utf8(key); }

bool usable_key(const std::string& key) {
    return !key.empty() && key.find('/') == std::string::npos && key.find('\\') == std::string::npos && key != "." && key != "..";
}

// A plugin of this key is a native one: no Python file of its name, its folder holding a plugin.json.
bool is_native(const std::string& key) {
    std::error_code ec;
    return usable_key(key) && !std::filesystem::is_regular_file(folder() / core::path_from_utf8(key + ".py"), ec) &&
           std::filesystem::is_regular_file(native_home(key) / "plugin.json", ec);
}

[[noreturn]] void manifest_problem(const std::string& key, const std::string& why) {
    throw core::Error("plugin_manifest", "the plugin.json of plugin " + key + " cannot be used (" + why + ")");
}

struct Native {
    std::filesystem::path home;     // its folder (as found in the plugins' folder)
    std::filesystem::path program;  // the program, the real path inside home
    std::string given;              // plugin.json's executable
    std::vector<std::string> args;
    std::string name;
    Json params = Json::object();
    Json manifest = Json::object();
    std::string sha256;  // of plugin.json's bytes and the program's
};

// plugin.json's bytes, from a file (not a link) of the plugin's own folder (not a link either), at most 64 KB
std::string manifest_text(const std::string& key) {
    std::error_code ec;
    if (!usable_key(key)) throw core::PyValueError("no plugin " + key);
    if (std::filesystem::is_regular_file(folder() / core::path_from_utf8(key + ".py"), ec)) {
        manifest_problem(key, "a Python plugin " + key + ".py has the same name");
    }
    const std::filesystem::path home = native_home(key);
    if (std::filesystem::symlink_status(home, ec).type() != std::filesystem::file_type::directory) manifest_problem(key, "its folder is a link");
    const std::filesystem::path manifest = home / "plugin.json";
    const auto status = std::filesystem::symlink_status(manifest, ec);
    if (ec || status.type() != std::filesystem::file_type::regular) manifest_problem(key, "it is not a file in the plugin's folder");
    const std::uintmax_t bytes = std::filesystem::file_size(manifest, ec);
    if (ec) manifest_problem(key, "it cannot be read");
    if (bytes > kMostManifestBytes) manifest_problem(key, "it is larger than 64 KB");
    QFile file(QString::fromStdString(core::path_to_utf8(manifest)));
    if (!file.open(QIODevice::ReadOnly)) manifest_problem(key, "it cannot be read");
    const QByteArray text = file.read(static_cast<qint64>(kMostManifestBytes) + 1);
    if (static_cast<std::uintmax_t>(text.size()) > kMostManifestBytes) manifest_problem(key, "it is larger than 64 KB");
    return text.toStdString();
}

// plugin.json (these bytes) read — it is never run — and its program found inside the plugin's folder (not a link, no
// part of its path leading out of the folder, at most 256 MB); the problems as core::Error("plugin_manifest")
Native parse_native(const std::string& key, const std::string& text) {
    std::error_code ec;
    Native out;
    out.home = native_home(key);
    try {
        out.manifest = core::parse_python_json(text);
    } catch (const core::Error&) {
        manifest_problem(key, "it is not JSON");
    }
    if (!out.manifest.is_object()) manifest_problem(key, "it is not a JSON object");
    const auto field = [&](const char* name) { return out.manifest.contains(name) ? out.manifest[name] : Json(); };
    const Json protocol = field("protocol");
    if (!protocol.is_number_integer() || protocol.get<std::int64_t>() != kProtocol) manifest_problem(key, "its protocol must be 1");
    const Json name = field("name");
    if (!name.is_string() || core::py_strip(name.get<std::string>()).empty()) manifest_problem(key, "its name must be a text");
    out.name = name.get<std::string>();
    const Json args = field("args");
    if (!args.is_null()) {
        if (!args.is_array()) manifest_problem(key, "its args must be a list of texts");
        for (const Json& arg : args) {
            if (!arg.is_string()) manifest_problem(key, "its args must be a list of texts");
            out.args.push_back(arg.get<std::string>());
        }
    }
    const Json params = field("params");
    if (!params.is_null()) {
        if (!params.is_object()) manifest_problem(key, "its params must be an object");
        out.params = params;
    }
    // the program: a relative path that stays inside the folder (no "..", no drive or root, no link out of it)
    const Json executable = field("executable");
    if (!executable.is_string() || executable.get<std::string>().empty()) manifest_problem(key, "its executable must be a path inside the plugin's folder");
    out.given = executable.get<std::string>();
    const std::filesystem::path relative = core::path_from_utf8(out.given);
    bool inside = relative.is_relative() && !relative.has_root_name() && !relative.has_root_directory() && out.given.find(':') == std::string::npos;
    for (const auto& part : relative) inside = inside && part != "..";
    if (!inside) manifest_problem(key, "its executable must be a path inside the plugin's folder");
    const std::filesystem::path program = out.home / relative;
    const auto program_status = std::filesystem::symlink_status(program, ec);
    if (ec || program_status.type() == std::filesystem::file_type::not_found) manifest_problem(key, "its program " + out.given + " is not in its folder");
    if (program_status.type() == std::filesystem::file_type::symlink) manifest_problem(key, "its program " + out.given + " is a link");
    if (program_status.type() != std::filesystem::file_type::regular) manifest_problem(key, "its program " + out.given + " is not in its folder");
    const std::filesystem::path home_real = std::filesystem::canonical(out.home, ec);
    const std::filesystem::path program_real = ec ? std::filesystem::path() : std::filesystem::canonical(program, ec);
    if (ec) manifest_problem(key, "its program " + out.given + " is not in its folder");
    const auto mismatch = std::mismatch(home_real.begin(), home_real.end(), program_real.begin(), program_real.end());
    if (mismatch.first != home_real.end()) manifest_problem(key, "its executable must be a path inside the plugin's folder");
    const std::uintmax_t program_bytes = std::filesystem::file_size(program_real, ec);
    if (ec) manifest_problem(key, "its program " + out.given + " is not in its folder");
    if (program_bytes > kMostProgramBytes) manifest_problem(key, "its program is larger than 256 MB");
    out.program = program_real;
    return out;
}

// A native plugin's plugin.json read and its program found and read through for its digest (nothing run): sha256 of
// plugin.json's bytes, a line break and the program's bytes. A program that cannot be read is refused (a program the
// person may only run cannot be checked or copied).
Native read_native(const std::string& key) {
    const std::string text = manifest_text(key);
    Native out = parse_native(key, text);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(text.data(), static_cast<qsizetype>(text.size())));
    hash.addData(QByteArrayView("\n", 1));
    QFile file(QString::fromStdString(core::path_to_utf8(out.program)));
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) manifest_problem(key, "its program " + out.given + " cannot be read");
    out.sha256 = hash.result().toHex().toStdString();
    return out;
}

// What a native plugin is now, for the choice: its digest when plugin.json and the program can be used, and the
// plugin.json's problem otherwise (a plugin.json that cannot be read: the digest of its bytes alone, never a chosen one)
struct NativeNow {
    std::optional<Native> plugin;
    std::optional<std::string> problem;
    std::string sha256;
};
NativeNow native_now(const std::string& key) {
    NativeNow now;
    try {
        now.plugin = read_native(key);
        now.sha256 = now.plugin->sha256;
    } catch (const core::Error& error) {
        now.problem = error.what();
        now.sha256 = "!" + sha256_bytes(read_file(native_home(key) / "plugin.json"));
    }
    return now;
}

NativeNow native_now_remembered(const std::string& key);

[[noreturn]] void not_chosen(const std::string& key) {
    throw core::Error("plugin_not_allowed", "plugin " + key + " is not chosen to run (turn plugins on and choose it in the plugin settings)");
}

// A chosen native plugin as last read (when neither file's size or time has changed since): plugins on, chosen as its
// plugin.json and program are. What runs is checked again from the bytes copied (prepare_native).
Native chosen_native(const std::string& key) {
    const NativeNow now = native_now_remembered(key);
    const Settings s = settings();
    const auto chosen = s.chosen.find(key);
    if (!now.plugin || !s.enabled || chosen == s.chosen.end() || !chosen->is_string() || chosen->get<std::string>() != now.sha256) not_chosen(key);
    return *now.plugin;
}

// The folder the programs are copied to before they run: inside Genko's settings folder (the system's temporary folder
// may forbid programs), each run in a folder of its own that only this user may enter, removed afterwards (what a crash
// left behind, a day old, is removed at the next run).
std::filesystem::path runs_folder() { return core::poses::config_dir() / "plugin-runs"; }

void remove_old_runs(const std::filesystem::path& runs) {
    std::error_code ec;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(runs, ec)) {
        if (!core::path_to_utf8(entry.path().filename()).starts_with("run-")) continue;
        std::error_code e;
        const auto time = entry.last_write_time(e);
        if (!e && now - time > std::chrono::hours(24)) std::filesystem::remove_all(entry.path(), e);
    }
}

// The program about to run: plugin.json and the program read once more and checked against the choice, and the very
// bytes checked written to a private folder and made executable there; that copy is what runs (a file changed after
// the check never runs). GENKO_PLUGIN_HOME stays the plugin's own folder.
struct Launch {
    std::unique_ptr<QTemporaryDir> dir;
    std::filesystem::path program;
    Native plugin;
};
Launch prepare_native(const std::string& key, const std::string& chosen_sha256) {
    Launch out;
    std::string text;
    try {
        text = manifest_text(key);
        out.plugin = parse_native(key, text);
    } catch (const core::Error&) {
        not_chosen(key);  // (it was usable when chosen: it has changed since)
    }
    const std::filesystem::path runs = runs_folder();
    std::error_code ec;
    std::filesystem::create_directories(runs, ec);
    remove_old_runs(runs);
    const auto copy_trouble = [&](const std::string& why) {
        runner_trouble("the program of plugin " + key + " could not be copied to run it (" + why + ")");
    };
    out.dir = std::make_unique<QTemporaryDir>(QString::fromStdString(core::path_to_utf8(runs / "run-XXXXXX")));
    if (!out.dir->isValid()) copy_trouble("no folder could be made in " + core::path_to_utf8(runs));
    out.program = core::path_from_utf8(out.dir->path().toStdString()) / out.plugin.program.filename();
    QFile source(QString::fromStdString(core::path_to_utf8(out.plugin.program)));
    if (!source.open(QIODevice::ReadOnly)) not_chosen(key);
    QFile copy(QString::fromStdString(core::path_to_utf8(out.program)));
    if (!copy.open(QIODevice::WriteOnly | QIODevice::Truncate)) copy_trouble(copy.errorString().toStdString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(text.data(), static_cast<qsizetype>(text.size())));
    hash.addData(QByteArrayView("\n", 1));
    std::uintmax_t copied = 0;
    for (;;) {
        const QByteArray piece = source.read(qint64{1} << 20);
        if (piece.isEmpty()) break;
        copied += static_cast<std::uintmax_t>(piece.size());
        if (copied > kMostProgramBytes) not_chosen(key);
        hash.addData(piece);
        if (copy.write(piece) != piece.size()) copy_trouble(copy.errorString().toStdString());
    }
    if (source.error() != QFileDevice::NoError) not_chosen(key);
    copy.close();
    if (copy.error() != QFileDevice::NoError) copy_trouble(copy.errorString().toStdString());
    if (hash.result().toHex().toStdString() != chosen_sha256) not_chosen(key);
    if (!copy.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        copy_trouble("it could not be made executable");
    }
#ifdef __linux__
    // (a settings folder on a file system mounted noexec: said as such)
    struct statvfs fs {};
    if (::statvfs(out.dir->path().toLocal8Bit().constData(), &fs) == 0 && (fs.f_flag & ST_NOEXEC) != 0) {
        runner_trouble("programs cannot run from Genko's settings folder on this computer (" + core::path_to_utf8(runs) +
                       ": its file system is mounted noexec), so plugin " + key + " cannot run");
    }
#endif
    return out;
}

// One request to a native plugin's program: its checked copy started directly with plugin.json's args (no shell), in
// the copy's own folder, with the short list of variables, the protocol and the plugin's folder.
Reply ask_native(const std::string& key, const Launch& launch, const std::string& header, const std::string& input, std::chrono::seconds limit,
                 std::size_t most, const std::stop_token& stop) {
    QProcess process;
    process.setProgram(QString::fromStdString(core::path_to_utf8(launch.program)));
    QStringList args;
    for (const std::string& arg : launch.plugin.args) args.push_back(QString::fromStdString(arg));
    process.setArguments(args);
    process.setWorkingDirectory(launch.dir->path());
    QProcessEnvironment env = plugin_environment();
    env.insert(QStringLiteral("GENKO_PLUGIN_PROTOCOL"), QString::number(kProtocol));
    env.insert(QStringLiteral("GENKO_PLUGIN_HOME"), QString::fromStdString(core::path_to_utf8(launch.plugin.home)));
    process.setProcessEnvironment(env);
    own_process_group(process);
    process.start();
    if (!process.waitForStarted(10000)) {
        runner_trouble("the program of plugin " + key + " could not be started (" + process.errorString().toStdString() + ")");
    }
    Reply reply = exchange(process, key, header, input, limit, most, stop);
    if (!core::py_truthy(core::py_get(reply.header, "ok"))) {
        const Json error = core::py_get(reply.header, "error", "");
        throw core::PyValueError("plugin " + key + " failed (" + (error.is_string() ? error.get<std::string>() : core::dump(error, core::DumpOptions{})) + ")");
    }
    return reply;
}

// The digests of native plugins as last read, by their files' sizes and times (a page drawn in tiles asks whether a
// plugin is chosen for each tile: its program is read again only when it has changed)
struct Stamped {
    std::string stamp;
    NativeNow now;
};
std::mutex g_native_mutex;
std::map<std::string, Stamped> g_native;

std::string stamp_of(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return "-";
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return "-";
    return std::to_string(size) + "@" + std::to_string(time.time_since_epoch().count());
}

NativeNow native_now_remembered(const std::string& key) {
    const std::filesystem::path manifest = native_home(key) / "plugin.json";
    std::string stamp = stamp_of(manifest);
    {
        std::lock_guard lock(g_native_mutex);
        if (const auto found = g_native.find(key); found != g_native.end() && found->second.now.plugin) {
            if (found->second.stamp == stamp + "|" + stamp_of(found->second.now.plugin->program)) return found->second.now;
        }
    }
    NativeNow now = native_now(key);
    if (now.plugin) stamp += "|" + stamp_of(now.plugin->program);
    std::lock_guard lock(g_native_mutex);
    g_native[key] = Stamped{stamp, now};
    return now;
}

// The last pictures made (a page drawn in tiles asks a correction layer's plugin for the same picture again and again),
// and the ones being made (asked again meanwhile: the same run is waited for, not started again).
struct Made {
    std::string request;  // sha256 of the key, the file, the settings and the pixels
    std::shared_ptr<const Image> picture;
    std::size_t bytes = 0;
};
std::mutex g_made_mutex;
std::deque<Made> g_made;
std::size_t g_made_bytes = 0;
constexpr std::size_t kMadeKept = 4;
constexpr std::size_t kMadeMostBytes = std::size_t{256} << 20;
std::map<std::string, std::shared_future<std::shared_ptr<const Image>>> g_running;

// What each chosen plugin said about itself (its failures too, so a broken or slow one is not asked again and again);
// forgotten when the plugin settings are saved.
struct Said {
    std::string sha256;
    std::string python;
    std::optional<Described> described;
    std::exception_ptr failed;
};
std::mutex g_described_mutex;
std::map<std::string, Said> g_described;

}  // namespace

std::filesystem::path folder() { return core::poses::config_dir() / "plugins"; }

std::vector<Listed> listed() {
    std::vector<Listed> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(folder(), ec)) return out;
    std::vector<std::filesystem::path> files;
    std::vector<std::string> homes;
    for (const auto& entry : std::filesystem::directory_iterator(folder(), ec)) {
        const std::string name = core::path_to_utf8(entry.path().filename());
        if (name.starts_with("_")) continue;
        if (entry.path().extension() == ".py" && entry.is_regular_file(ec)) files.push_back(entry.path());
        if (!name.starts_with(".") && entry.is_directory(ec) && std::filesystem::is_regular_file(entry.path() / "plugin.json", ec)) {
            homes.push_back(name);
        }
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
    // the native plugins: their plugin.json and program read, nothing run
    for (const std::string& key : homes) {
        Listed item;
        item.key = key;
        item.native = true;
        const NativeNow state = native_now_remembered(key);
        item.sha256 = state.sha256;
        item.problem = state.problem;
        if (state.plugin) item.manifest = state.plugin->manifest;
        const auto chosen = now.chosen.find(item.key);
        item.chosen = state.plugin && chosen != now.chosen.end() && *chosen == Json(item.sha256);
        out.push_back(std::move(item));
    }
    std::stable_sort(out.begin(), out.end(), [](const Listed& a, const Listed& b) { return a.key < b.key; });
    return out;
}

bool installed(const std::string& key) {
    std::error_code ec;
    return usable_key(key) && (std::filesystem::is_regular_file(folder() / core::path_from_utf8(key + ".py"), ec) ||
                               std::filesystem::is_regular_file(native_home(key) / "plugin.json", ec));
}

Settings settings() {
    Settings out;
    try {
        const std::string text = read_file(settings_file());
        if (text.empty()) return out;
        const Json data = core::parse_python_json(text);
        if (!data.is_object()) return out;
        const auto enabled = data.find("enabled");
        out.enabled = enabled != data.end() && enabled->is_boolean() && enabled->get<bool>();
        if (data.contains("python") && data["python"].is_string()) out.python = data["python"].get<std::string>();
        if (data.contains("chosen") && data["chosen"].is_object()) out.chosen = data["chosen"];
    } catch (const std::exception&) {  // (a settings file that cannot be read: plugins off)
        return Settings{};
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
    std::lock_guard lock(g_described_mutex);
    g_described.clear();  // (saved again: each plugin is asked again, e.g. with another Python or Pillow installed)
}

void choose(const std::string& key, bool on, const std::optional<std::string>& listed_sha256) {
    Settings s = settings();
    if (on) {
        const std::string now = is_native(key) ? read_native(key).sha256 : sha256_of(plugin_file(key));
        if (listed_sha256 && *listed_sha256 != now) {
            throw core::Error("plugin_changed", "plugin " + key + " changed since the list was shown (look at it again and choose it again)");
        }
        s.chosen[key] = now;
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
    if (is_native(key)) {
        const NativeNow now = native_now_remembered(key);
        return now.plugin && chosen->get<std::string>() == now.sha256;
    }
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
    if (is_native(key)) {  // (plugin.json says it: nothing runs)
        const Native plugin = chosen_native(key);
        return Described{key, plugin.name, plugin.params};
    }
    const Code code = chosen_code(key);
    const auto py = python();
    const std::string which = py ? core::path_to_utf8(*py) : std::string();
    {
        std::lock_guard lock(g_described_mutex);
        if (const auto found = g_described.find(key); found != g_described.end() && found->second.sha256 == code.sha256 && found->second.python == which) {
            if (found->second.failed) std::rethrow_exception(found->second.failed);
            return *found->second.described;
        }
    }
    Said said{code.sha256, which, std::nullopt, nullptr};
    try {
        const Reply reply = ask_runner(key, code, "describe", {}, {}, kDescribeTime, 1 << 20, {});
        Described out;
        out.key = key;
        out.name = core::py_str(core::py_get(reply.header, "name", key));
        const Json params = core::py_get(reply.header, "params", Json::object());
        out.params = params.is_object() ? params : Json::object();
        said.described = out;
    } catch (const std::exception&) {
        said.failed = std::current_exception();
    }
    {
        std::lock_guard lock(g_described_mutex);
        g_described[key] = said;
    }
    if (said.failed) std::rethrow_exception(said.failed);
    return *said.described;
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

Image run(std::string_view kind, const Image& image, const Json& params, const std::stop_token& stop) {
    const std::string key(kind.starts_with(kPrefix) ? kind.substr(kPrefix.size()) : kind);
    const bool native = is_native(key);
    // (a native plugin: as last read here — its program read again only when no picture kept answers, as it is copied)
    const Code code = native ? Code{{}, chosen_native(key).sha256} : chosen_code(key);
    const Image rgba = image.mode() == "RGBA" ? image : image.convert("RGBA");
    Json header = Json::object();
    if (native) header["protocol"] = kProtocol;
    header["width"] = rgba.width();
    header["height"] = rgba.height();
    header["params"] = params.is_object() ? params : Json::object();
    const std::string head = core::dump(header, core::DumpOptions{}) + "\n";
    const std::string pixels = rgba.tobytes();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::fromStdString(key + "\n" + code.sha256 + "\n" + head));
    hash.addData(QByteArray::fromRawData(pixels.data(), static_cast<qsizetype>(pixels.size())));
    const std::string request = hash.result().toHex().toStdString();
    for (;;) {
        std::promise<std::shared_ptr<const Image>> making;
        std::shared_future<std::shared_ptr<const Image>> waiting;
        {
            std::lock_guard lock(g_made_mutex);
            for (const Made& made : g_made) {
                if (made.request == request) return *made.picture;
            }
            if (const auto running = g_running.find(request); running != g_running.end()) {
                waiting = running->second;
            } else {
                g_running[request] = making.get_future().share();
            }
        }
        if (waiting.valid()) {  // (the same picture asked for elsewhere: its run is waited for)
            while (waiting.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
                if (stop.stop_requested()) throw Cancelled();
            }
            try {
                return *waiting.get();
            } catch (const Cancelled&) {
                continue;  // (that one was no longer wanted: this one asks again)
            }
        }
        std::shared_ptr<const Image> picture;
        try {
            // (a large picture is given longer: the time limit is for a page-sized one at screen resolution)
            const std::size_t count = static_cast<std::size_t>(rgba.width()) * static_cast<std::size_t>(rgba.height());
            const auto limit = kRunTime * static_cast<std::int64_t>(std::max<std::size_t>(1, (count + kRunPixels - 1) / kRunPixels));
            Reply reply = native ? ask_native(key, prepare_native(key, code.sha256), head, pixels, limit, count * 4 + 65536, stop)
                                 : ask_runner(key, code, "run", head, pixels, limit, count * 4 + 65536, stop);
            if (core::py_get(reply.header, "width") != Json(rgba.width()) || core::py_get(reply.header, "height") != Json(rgba.height()) ||
                reply.data.size() != count * 4) {
                throw core::PyValueError("plugin " + key + " did not return a picture");
            }
            picture = std::make_shared<const Image>(Image::frombytes("RGBA", rgba.size(), reply.data));
        } catch (...) {
            making.set_exception(std::current_exception());
            std::lock_guard lock(g_made_mutex);
            g_running.erase(request);
            throw;
        }
        making.set_value(picture);
        std::lock_guard lock(g_made_mutex);
        g_running.erase(request);
        const std::size_t bytes = static_cast<std::size_t>(picture->width()) * static_cast<std::size_t>(picture->height()) * 4;
        if (bytes <= kMadeMostBytes) {
            g_made.push_back(Made{request, picture, bytes});
            g_made_bytes += bytes;
            while (g_made.size() > kMadeKept || g_made_bytes > kMadeMostBytes) {
                g_made_bytes -= g_made.front().bytes;
                g_made.pop_front();
            }
        }
        return *picture;
    }
}

void forget_made() {
    std::lock_guard lock(g_made_mutex);
    g_made.clear();
    g_made_bytes = 0;
}

}  // namespace genko::render::plugins
