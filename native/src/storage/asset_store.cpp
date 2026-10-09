#include "storage/asset_store.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>

#include <algorithm>
#include <system_error>
#include <vector>
#include <utility>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "storage/fsutil.hpp"

namespace genko::storage {

namespace fs = std::filesystem;

AssetStore::AssetStore(fs::path project) : project_(std::move(project)) {}

std::string AssetStore::ref(std::string_view bytes) {
    const QByteArray digest = QCryptographicHash::hash(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
                                                       QCryptographicHash::Sha256)
                                  .toHex();
    return "sha256:" + digest.toStdString();
}

bool AssetStore::is_ref(std::string_view ref) {
    constexpr std::string_view prefix = "sha256:";
    if (ref.size() != prefix.size() + 64 || ref.substr(0, prefix.size()) != prefix) return false;
    return std::all_of(ref.begin() + static_cast<std::ptrdiff_t>(prefix.size()), ref.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool AssetStore::is_suffix(std::string_view suffix) {
    return !suffix.empty() && suffix.front() == '.' && suffix.find('/') == std::string_view::npos &&
           suffix.find('\\') == std::string_view::npos && suffix.find("..") == std::string_view::npos &&
           suffix.find('\0') == std::string_view::npos;
}

std::string AssetStore::relpath(std::string_view ref, std::string_view suffix) {
    if (!is_ref(ref)) throw core::Error("format", "not an asset ref: " + core::py_repr_str(ref));
    if (!is_suffix(suffix)) throw core::Error("format", "not an asset suffix: " + core::py_repr_str(suffix));
    const std::string_view digest = ref.substr(7);
    std::string out = "assets/";
    out.append(digest.substr(0, 2));
    out += '/';
    out.append(digest);
    out.append(suffix);
    return out;
}

fs::path AssetStore::path(std::string_view ref, std::string_view suffix) const {
    return project_ / path_from_utf8(relpath(ref, suffix));
}

bool AssetStore::has(std::string_view ref, std::string_view suffix) const {
    std::error_code ec;
    return fs::is_regular_file(path(ref, suffix), ec);
}

std::string AssetStore::put_bytes(std::string_view bytes, std::string_view suffix) {
    std::string r = ref(bytes);
    put_known(r, bytes, suffix);
    return r;
}

void AssetStore::put_known(std::string_view ref_value, std::string_view bytes, std::string_view suffix) {
    const fs::path target = path(ref_value, suffix);
    std::error_code ec;
    if (fs::exists(target, ec)) return;
    write_atomic(target, bytes);
}

std::optional<std::string> AssetStore::get_bytes(std::string_view ref_value, std::string_view suffix, std::optional<std::size_t> maximum) const {
    const fs::path target = path(ref_value, suffix);
    std::error_code ec;
    if (!fs::is_regular_file(target, ec)) return std::nullopt;
    return maximum ? read_file_bounded(target, *maximum) : read_file(target);
}

std::vector<fs::path> AssetStore::all_files() const {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(root(), ec)) return out;
    for (auto it = fs::recursive_directory_iterator(root(), ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code type_ec;
        if (!it->is_regular_file(type_ec)) continue;
        const std::string name = path_to_utf8(it->path().filename());
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) continue;
        out.push_back(it->path());
    }
    if (ec) throw core::Error("io", os_error_text(ec.value(), root()), path_to_utf8(root()));
    std::sort(out.begin(), out.end());
    return out;
}

std::size_t copy_assets(const fs::path& src, const fs::path& dest) {
    const fs::path root = src / "assets";
    std::error_code ec;
    if (fs::symlink_status(root, ec).type() != fs::file_type::directory) return 0;  // (none, or a link: not followed)
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code type_ec;  // (the iterator does not go into a linked folder; a linked file is passed over here)
        if (it->symlink_status(type_ec).type() == fs::file_type::regular && !type_ec) files.push_back(it->path());
    }
    if (ec) throw core::Error("io", os_error_text(ec.value(), root), path_to_utf8(root));
    std::sort(files.begin(), files.end());
    AssetStore target(dest);
    std::size_t copied = 0;
    for (const fs::path& file : files) {
        // <ab>/<64 lowercase hex><suffix>, the folder the digest's first two digits
        const fs::path rel = file.lexically_relative(root);
        std::vector<std::string> parts;
        for (const auto& part : rel) parts.push_back(path_to_utf8(part));
        if (parts.size() != 2) continue;
        const std::string& name = parts[1];
        if (name.size() < 64 || (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0)) continue;
        const std::string ref = "sha256:" + name.substr(0, 64);
        const std::string suffix = name.substr(64);
        if (!AssetStore::is_ref(ref) || !AssetStore::is_suffix(suffix) || parts[0] != name.substr(0, 2)) continue;
        if (target.has(ref, suffix)) continue;
        const std::string bytes = read_file(file);
        if (AssetStore::ref(bytes) != ref) continue;  // (it does not hold what its name says)
        target.put_known(ref, bytes, suffix);
        ++copied;
    }
    return copied;
}

}  // namespace genko::storage
