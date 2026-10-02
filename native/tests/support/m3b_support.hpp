#pragma once

// Helpers for the tests of tones, effect lines and rulers (M3-B; header only, not part of the product): books saved
// and edited as `genko apply` does (with the drawing ops), a book's state for comparisons, and project.json as
// Python's v3 writer writes it with each patch picture as its pixels.

#include <QCryptographicHash>
#include <QString>

#include <filesystem>
#include <string>
#include <string_view>

#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"

namespace genko::test::m3b {

namespace fs = std::filesystem;

inline fs::path to_path(const QString& path) { return storage::path_from_utf8(path.toStdString()); }

// The command bus with every op this build has, the drawing ops included.
inline const core::CommandBus& bus() {
    static const core::CommandBus b(render::ops_registry());
    return b;
}

// A new v4 book in `dir` holding `doc` (its first revision).
inline void save_new(const fs::path& dir, const core::Document& doc) {
    storage::ProjectLock lock(dir);
    lock.try_acquire();
    storage::SaveRequest request;
    request.ops = core::Json::array();
    storage::Saver(lock).save(doc, request);
}

inline core::Document read(const fs::path& dir) { return storage::load_document(dir).document; }

struct Edited {
    core::ApplyResult result;
    storage::SaveResult saved;
};

// `genko apply` in this process: under the lock, the ops applied as `actor` and saved as the next revision (with
// their journal record, so they can be undone).
inline Edited edit(const fs::path& dir, const core::Json& ops, const std::string& actor = "genko") {
    storage::ProjectLock lock(dir, actor);
    lock.try_acquire();
    storage::journal::repair(dir);
    const auto loaded = storage::load_document(dir);
    Edited out{bus().apply(loaded.document, ops, core::Actor(actor)), {}};
    storage::SaveRequest request;
    request.actor = actor;
    request.base_revision = loaded.document.revision;
    request.ops = out.result.journal_ops;
    out.saved = storage::Saver(lock).save(out.result.doc, request);
    return out;
}

// "px:" + sha256(mode|WxH|raw pixels) of a PNG (tone_harness.picture_digest), "bytes:" + sha256 when it is not one.
inline std::string picture_digest(std::string_view bytes) {
    const auto sha = [](std::string_view data) {
        return QCryptographicHash::hash(QByteArrayView(data.data(), static_cast<qsizetype>(data.size())), QCryptographicHash::Sha256)
            .toHex()
            .toStdString();
    };
    try {
        const render::Image image = render::read_png(bytes);
        const std::string head = std::string(image.mode()) + "|" + std::to_string(image.width()) + "x" + std::to_string(image.height()) + "|";
        return "px:" + sha(head + image.tobytes());
    } catch (const std::exception&) {
        return "bytes:" + sha(bytes);
    }
}

// project.json as Python's v3 writer writes `doc` (version 3, no revision, writer, book_id, features or min_reader),
// each patch picture as its pixels' digest (the PNG bytes are libpng's here, Pillow's there). `scratch`: a folder for
// the assets written on the way.
inline core::Json normalized_payload(const core::Document& doc, const fs::path& scratch) {
    storage::AssetStore store(scratch);
    core::Json payload = storage::project_payload_v4(doc, store);
    for (const char* key : {"min_reader", "writer", "book_id", "features", "revision"}) payload.erase(key);
    payload["version"] = 3;
    for (core::Json& page : payload["pages"]) {
        for (core::Json& layer : page["layers"]) {
            if (!layer.contains("patches")) continue;
            for (core::Json& patch : layer["patches"]) {
                if (!patch.contains("asset") || !patch["asset"].is_string()) continue;
                const auto bytes = store.get_bytes(patch["asset"].get<std::string>(), ".png");
                patch["asset"] = bytes ? picture_digest(*bytes) : std::string("missing");
            }
        }
    }
    return payload;
}

// A book's state for comparing two of them: normalized_payload without the revision.
inline core::Json state_of(const core::Document& doc, const fs::path& scratch) { return normalized_payload(doc, scratch); }

}  // namespace genko::test::m3b
