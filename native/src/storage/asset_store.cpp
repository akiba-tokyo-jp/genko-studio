#include "storage/asset_store.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>

#include <algorithm>
#include <system_error>
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

std::optional<std::string> AssetStore::get_bytes(std::string_view ref_value, std::string_view suffix) const {
    const fs::path target = path(ref_value, suffix);
    std::error_code ec;
    if (!fs::is_regular_file(target, ec)) return std::nullopt;
    return read_file(target);
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

}  // namespace genko::storage
