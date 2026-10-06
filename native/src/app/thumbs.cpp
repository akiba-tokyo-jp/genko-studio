#include "app/thumbs.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <system_error>
#include <vector>

#include "app/config.hpp"
#include "app/frame_tools.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "core/strokes.hpp"
#include "render/page.hpp"
#include "storage/asset_store.hpp"
#include "storage/writer.hpp"

namespace genko::app {

namespace fs = std::filesystem;
using core::Json;

namespace {

class Hasher {
public:
    void add(std::string_view text) {
        hash_.addData(QByteArrayView(text.data(), static_cast<qsizetype>(text.size())));
        hash_.addData(QByteArrayView("\x1f", 1));
    }
    void add(const std::string& text) { add(std::string_view(text)); }
    void add(const Json& value) { add(core::dump_canonical(value)); }
    void add_bytes(const core::Bytes& bytes, const core::BlobMemo* memo) {
        if (!bytes) {
            add(std::string_view("-"));
            return;
        }
        if (memo != nullptr) {
            if (const std::string* ref = memo->ref_for(bytes)) {
                add(*ref);
                return;
            }
        }
        add(storage::AssetStore::ref(*bytes));
    }
    std::string hex() const { return hash_.result().toHex().toStdString(); }

private:
    QCryptographicHash hash_{QCryptographicHash::Sha256};
};

Json opt(const std::optional<Json>& value) { return value ? *value : Json(nullptr); }

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

}  // namespace

std::string page_fingerprint(const core::Document& doc, const core::Page& page) {
    Hasher h;
    h.add(storage::spec_to_json(page.spec));
    h.add(core::to_string(page.binding));
    for (const core::Frame& frame : page.frames) h.add(frame_tree(frame));
    for (const auto& [role, rgb] : page.fills) {
        h.add(core::to_string(role));
        Json values = Json::array();
        for (const auto& v : rgb) values.push_back(v.json());
        h.add(values);
    }
    h.add(page.extra);
    h.add(page.effects);
    h.add(opt(page.ruler));
    h.add(page.rulers);
    h.add(page.prims);
    h.add(Json(page.numero));
    h.add(page.onion_from ? page.onion_from->json() : Json(nullptr));
    for (const core::Layer& layer : page.layers) {
        Json look = Json::object();
        look["id"] = layer.id;
        look["role"] = core::to_string(layer.role);
        look["kind"] = core::to_string(layer.kind);
        look["visible"] = layer.visible;
        look["exportable"] = layer.exportable;
        look["opacity"] = layer.opacity;
        look["blend"] = layer.blend;
        look["clip"] = layer.clip;
        look["lock_alpha"] = layer.lock_alpha;
        look["panel_clip"] = layer.panel_clip;
        look["panel_each"] = layer.panel_each;
        look["reference"] = layer.reference;
        look["color_prints"] = layer.color_prints;
        look["angle"] = layer.angle;
        look["fit"] = layer.fit;
        look["clip_to"] = layer.clip_to;
        look["asset"] = layer.asset ? Json(*layer.asset) : Json(nullptr);
        look["tone"] = opt(layer.tone);
        look["fill"] = opt(layer.fill);
        look["adjust"] = opt(layer.adjust);
        look["effect"] = opt(layer.effect);
        look["screen"] = opt(layer.screen);
        look["finish"] = opt(layer.finish);
        look["color"] = layer.color ? Json(*layer.color) : Json(nullptr);
        if (layer.fill_rgb) {
            Json rgb = Json::array();
            for (const auto& v : *layer.fill_rgb) rgb.push_back(v.json());
            look["fill_rgb"] = rgb;
        }
        if (layer.placement_mm) look["placement"] = core::rect_to_json(*layer.placement_mm);
        h.add(look);
        h.add_bytes(layer.raster_png, &layer.raster_memo);
        if (layer.color_raster) {
            h.add(std::string_view("native-color-raster/1"));
            h.add_bytes(layer.color_raster, nullptr);
        }
        if (layer.mask) {
            h.add(Json(layer.mask->enabled));
            h.add_bytes(layer.mask->png, &layer.mask->memo);
        }
        for (const core::Patch& patch : layer.patches) {
            h.add(patch.attrs);
            h.add_bytes(patch.png, &patch.memo);
        }
        const core::StrokeList& strokes = *layer.strokes;
        if (strokes.items.empty()) {
            h.add(std::string_view("no lines"));
        } else if (!strokes.blob_ref.empty()) {
            h.add(strokes.blob_ref);  // (the hash of the very bytes of these lines)
        } else {
            h.add(storage::AssetStore::ref(core::strokes_blob(strokes)));
        }
    }
    h.add(doc.brush_custom);
    return h.hex();
}

ThumbCache::ThumbCache(fs::path root, std::size_t max_files, std::uint64_t max_bytes)
    : root_(std::move(root)), max_files_(max_files), max_bytes_(max_bytes) {}

std::string ThumbCache::key(const core::Document& doc, const core::Page& page, int height, const std::string& mode) {
    Hasher h;
    h.add(std::string_view(kThumbVersion));
    h.add(mode);
    h.add(std::to_string(height));
    h.add(page_fingerprint(doc, page));
    return h.hex();
}

fs::path ThumbCache::file(const std::string& key) const { return root_ / key.substr(0, 2) / (key + ".png"); }

std::optional<QImage> ThumbCache::get(const std::string& key) const {
    const QString path = qpath(file(key));
    if (!QFileInfo::exists(path)) return std::nullopt;
    QImage image(path);
    if (image.isNull()) return std::nullopt;
    QFile touched(path);
    if (touched.open(QIODevice::ReadWrite)) touched.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    return image;
}

void ThumbCache::put(const std::string& key, const QImage& image) const {
    const fs::path target = file(key);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    const QString temp = qpath(target) + QStringLiteral(".tmp");
    if (!image.save(temp, "PNG")) {
        QFile::remove(temp);
        return;
    }
    fs::rename(core::path_from_utf8(temp.toStdString()), target, ec);
    if (ec) QFile::remove(temp);
    static int puts = 0;
    if (++puts % 64 == 0) trim();
}

std::size_t ThumbCache::trim() const {
    struct Item {
        fs::path path;
        std::uintmax_t size;
        fs::file_time_type when;
    };
    std::vector<Item> items;
    std::uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root_, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".png") continue;
        const auto size = it->file_size(ec);
        const auto when = it->last_write_time(ec);
        items.push_back(Item{it->path(), size, when});
        total += size;
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.when < b.when; });
    std::size_t removed = 0;
    std::size_t count = items.size();
    for (const Item& item : items) {
        if (count <= max_files_ && total <= max_bytes_) break;
        if (fs::remove(item.path, ec)) {
            ++removed;
            --count;
            total -= item.size;
        }
    }
    return removed;
}

ThumbMaker::ThumbMaker(QObject* parent)
    : QObject(parent), cache_(cache_root() / "thumbs"), self_(std::make_shared<ThumbMaker*>(this)) {
    pool_.setMaxThreadCount(1);
}

ThumbMaker::~ThumbMaker() {
    self_.reset();
    pool_.clear();
    pool_.waitForDone();
}

void ThumbMaker::cancel_waiting() { pool_.clear(); }

void ThumbMaker::request(DocPtr doc, std::size_t index, int height, const std::string& mode) {
    if (!doc || index >= doc->pages.size()) return;
    std::weak_ptr<ThumbMaker*> self = self_;
    const ThumbCache* cache = &cache_;
    pool_.start([self, cache, doc, index, height, mode]() {
        const core::Page& page = doc->page(index);
        const std::string key = ThumbCache::key(*doc, page, height, mode);
        QImage image;
        bool cached = false;
        if (!cache->root().empty()) {
            if (auto found = cache->get(key)) {
                image = std::move(*found);
                cached = true;
            }
        }
        if (image.isNull()) {
            try {
                const int dpi = std::max(6, static_cast<int>(std::nearbyint(height / (page.spec.height_mm.value() / 25.4))));
                render::RenderOptions options;
                options.mode = mode;
                options.skip_unported = true;
                const render::RenderResult drawn = render::render_page(page, dpi, options, doc.get());
                const render::Image rgb = drawn.image.mode() == "RGB" ? drawn.image : drawn.image.convert("RGB");
                const std::string bytes = rgb.tobytes();
                image = QImage(reinterpret_cast<const uchar*>(bytes.data()), rgb.width(), rgb.height(), rgb.width() * 3,
                               QImage::Format_RGB888)
                            .copy();
                if (!cache->root().empty()) cache->put(key, image);
            } catch (const std::exception&) {
                return;  // (a page that cannot be drawn keeps its blank picture)
            }
        }
        const QString id = QString::fromStdString(page.id);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, id, height, image, cached]() {
                if (const auto alive = self.lock()) {
                    if (!cached) ++(*alive)->drawn_;
                    emit (*alive)->done(id, height, image, cached);
                }
            },
            Qt::QueuedConnection);
    });
}

}  // namespace genko::app
