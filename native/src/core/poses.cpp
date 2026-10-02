#include "core/poses.hpp"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QString>

#include <string>

#include "core/error.hpp"
#include "core/limits.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core::poses {

namespace {

std::filesystem::path path_of(const QString& text) { return path_from_utf8(text.toStdString()); }

std::filesystem::path poses_file() { return config_dir() / "poses.json"; }

// str.strip(): Python's whitespace at both ends
std::string strip(std::string_view text) {
    const auto space_at = [&](std::size_t at, std::size_t& length) {
        const auto c = static_cast<unsigned char>(text[at]);
        std::uint32_t cp = c;
        length = 1;
        if (c >= 0xE0 && at + 2 < text.size()) {
            cp = (c & 0x0Fu) << 12 | (static_cast<unsigned char>(text[at + 1]) & 0x3Fu) << 6 | (static_cast<unsigned char>(text[at + 2]) & 0x3Fu);
            length = 3;
        } else if (c >= 0xC0 && at + 1 < text.size()) {
            cp = (c & 0x1Fu) << 6 | (static_cast<unsigned char>(text[at + 1]) & 0x3Fu);
            length = 2;
        }
        return (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x20) || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
               (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
    };
    std::size_t start = 0;
    std::size_t length = 0;
    while (start < text.size() && space_at(start, length)) start += length;
    std::size_t end = start;
    std::size_t last = start;  // (the end of the last character that is not whitespace)
    while (end < text.size()) {
        const bool blank = space_at(end, length);
        end += length;
        if (!blank) last = end;
    }
    return std::string(text.substr(start, last - start));
}

void write(const Json& poses) {
    const std::filesystem::path path = poses_file();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    DumpOptions options;
    options.indent = 1;
    options.item_separator = ",";
    std::string text = dump(poses, options);
#ifdef _WIN32
    // (Path.write_text writes in text mode: "\n" becomes "\r\n" on Windows; JSON strings hold no raw newline)
    for (std::size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 2)) text.insert(at, 1, '\r');
#endif
    QSaveFile file(QString::fromStdString(path_to_utf8(path)));
    if (!file.open(QIODevice::WriteOnly) || file.write(text.data(), static_cast<qint64>(text.size())) != static_cast<qint64>(text.size()) ||
        !file.commit()) {
        throw Error("io", "cannot write " + path_to_utf8(path));
    }
}

}  // namespace

std::filesystem::path config_dir() {
    const QString own = qEnvironmentVariable("GENKO_CONFIG_DIR");
    if (!own.isEmpty()) return path_of(own);
#ifdef _WIN32
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty()) return path_of(appdata) / "genko";
#endif
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.config");
    return path_of(base) / "genko";
}

Json user_poses() {
    QFile file(QString::fromStdString(path_to_utf8(poses_file())));
    if (!file.open(QIODevice::ReadOnly)) return Json::array();
    const QByteArray bytes = file.readAll();
    Json data;
    try {
        ParseOptions options;
        options.universal_newlines = true;
        data = parse_python_json(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())), nullptr, options);
    } catch (const Error&) {
        return Json::array();
    }
    Json out = Json::array();
    if (!data.is_array()) return out;
    for (const Json& p : data) {
        if (!p.is_object()) continue;
        const Json* name = get(p, "name");
        if (name != nullptr && py_truthy(*name)) out.push_back(p);
    }
    return out;
}

Json save_pose(std::string_view name_in, const Json& prim) {
    const std::string name = strip(name_in);
    if (name.empty()) throw PyValueError("a pose needs a name");
    static const Json kNone = Json::object();
    const Json& joints = get_else(prim, "joints", kNone);
    if (!joints.is_object()) raise_attribute_error(joints, "items");
    Json copied = Json::object();
    for (const auto& [k, v] : joints.items()) copied[k] = py_dict(v);
    Json pose = Json::object();
    pose["name"] = name;
    pose["joints"] = std::move(copied);
    pose["hands"] = py_dict(get_else(prim, "hands", kNone));
    Json kept = Json::array();
    for (const Json& p : user_poses()) {
        const Json* own = get(p, "name");
        if (!(own != nullptr && py_equals(*own, Json(name)))) kept.push_back(p);
    }
    // (Python keeps any number; the file is read and written whole at each save)
    if (kept.size() >= limits::kUserPoses) {
        throw PyValueError("the pose library is full (at most " + std::to_string(limits::kUserPoses) + " poses): delete one first");
    }
    kept.push_back(pose);
    write(kept);
    return pose;
}

void delete_pose(std::string_view name) {
    Json kept = Json::array();
    for (const Json& p : user_poses()) {
        const Json* own = get(p, "name");
        if (!(own != nullptr && py_equals(*own, Json(std::string(name))))) kept.push_back(p);
    }
    write(kept);
}

std::optional<Json> find(std::string_view name) {
    for (const Json& p : user_poses()) {
        const Json* own = get(p, "name");
        if (own != nullptr && py_equals(*own, Json(std::string(name)))) return std::make_optional<Json>(p);
    }
    return std::nullopt;
}

}  // namespace genko::core::poses
