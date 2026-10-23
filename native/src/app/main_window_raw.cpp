// PC4 F-e (SPEC.md FILE-01「カメラ RAW」, design 5.3 and 8-3 (a)): a camera RAW file opened through Camera Raw's window
// (app/camera_raw_open) — its development chosen before it opens, as Photoshop —, and a RAW smart object's コンテンツを編集
// (the same window: set_camera_raw, one step to undo). The RAW file is never written.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent>

#include <memory>

#include "app/ask.hpp"
#include "app/camera_raw_open.hpp"
#include "app/main_window.hpp"
#include "app/open_dialog.hpp"
#include "core/paths.hpp"
#include "core/pyops.hpp"
#include "core/raw_develop.hpp"
#include "core/smart.hpp"
#include "formats/open_file.hpp"
#include "render/raw.hpp"
#include "render/smart.hpp"
#include "service/wording.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

namespace fs = std::filesystem;
using core::Json;

namespace {

QString qpath_of(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

// The development a RAW file opens with when nothing was chosen for it: the ワークフロー's (環境設定 → Camera Raw).
Json workflow_raw(const camera_raw_open::Workflow& w) {
    return Json{{"develop", Json{{"space", w.space}, {"depth", w.depth}, {"sharpen", w.sharpen}}}};
}

}  // namespace

bool MainWindow::is_raw_path(const fs::path& path) {
    const std::string name = core::path_to_utf8(path.filename());
    const auto dot = name.rfind('.');
    if (dot != std::string::npos && render::raw::raw_suffix(name.substr(dot))) return true;
    // (a RAW of another name: its marks, in its first bytes)
    QFile file(qpath_of(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = file.read(64 * 1024);
    return render::raw::looks_raw(std::string_view(head.constData(), static_cast<std::size_t>(head.size())));
}

void MainWindow::open_raw_file(const fs::path& path, bool window) {
    remember_file(path);
    flash(QStringLiteral("開いています: %1").arg(QDir::toNativeSeparators(qpath_of(path))), 30000);
    // (read on a worker — at most 256 MiB —, its asset ref for what 完了 remembers)
    struct Read {
        std::shared_ptr<const std::string> bytes;
        std::string ref;
        QString error;
    };
    auto* watcher = new QFutureWatcher<Read>(this);
    connect(watcher, &QFutureWatcher<Read>::finished, this, [this, watcher, path, window] {
        Read read = watcher->result();
        watcher->deleteLater();
        flash_timer_.stop();
        refresh_status();
        if (!read.bytes) {
            ask::warning(this, QStringLiteral("Genko"), QStringLiteral("ファイルを開けませんでした:\n%1\n%2").arg(QDir::toNativeSeparators(qpath_of(path)), wording::error(read.error)));
            return;
        }
        const camera_raw_open::Workflow flow = camera_raw_open::workflow();
        const std::optional<Json> kept = camera_raw_open::remembered(read.ref);
        Json start = kept ? *kept : workflow_raw(flow);
        double dpi = flow.dpi;
        if (const Json* resolution = core::get(start, "resolution"); resolution != nullptr && resolution->is_number()) dpi = resolution->get<double>();
        start.erase("resolution");
        camera_raw_open::Chosen chosen;
        if (window) {
            chosen = camera_raw_open::run(this, read.bytes, qpath_of(path.filename()), start, dpi, false);
        } else {  // (Shift held: as it is set, no window)
            chosen.ending = flow.smart ? camera_raw_open::Ending::OpenObject : camera_raw_open::Ending::Open;
            chosen.raw = start;
            chosen.dpi = dpi;
        }
        if (chosen.ending == camera_raw_open::Ending::Cancel) return;
        if (chosen.ending == camera_raw_open::Ending::Done) {
            Json remembered = chosen.raw;
            remembered["resolution"] = chosen.dpi;
            camera_raw_open::remember(read.ref, remembered);
            flash(QStringLiteral("「%1」の現像の設定を覚えました").arg(qpath_of(path.filename())), 4000);
            return;
        }
        formats::OpenOptions options = file_open_options();
        options.raw = chosen.raw;
        options.raw_dpi = chosen.dpi;
        options.raw_smart = chosen.ending == camera_raw_open::Ending::OpenObject;
        const bool auto_tone = !window && !kept && flow.auto_tone;
        flash(QStringLiteral("現像しています: %1").arg(QDir::toNativeSeparators(qpath_of(path))), 60000);
        using Opened = std::pair<std::optional<formats::Opened>, QString>;
        auto* developing = new QFutureWatcher<Opened>(this);
        connect(developing, &QFutureWatcher<Opened>::finished, this, [this, developing, path] {
            Opened made = developing->result();
            developing->deleteLater();
            flash_timer_.stop();
            if (!made.first) {
                ask::warning(this, QStringLiteral("Genko"), QStringLiteral("ファイルを開けませんでした:\n%1\n%2").arg(QDir::toNativeSeparators(qpath_of(path)), wording::error(made.second)));
                refresh_status();
                return;
            }
            add_picture_book(std::move(made.first->doc), made.first->notes, path);
        });
        const std::shared_ptr<const std::string> bytes = read.bytes;
        const std::string name = core::path_to_utf8(path.filename());
        developing->setFuture(QtConcurrent::run([bytes, name, options, auto_tone]() mutable -> Opened {
            try {
                if (auto_tone) {  // (環境設定's 自動補正, with no window: 自動's exposure from a preview)
                    Json& develop = (*options.raw)["develop"];
                    const core::raw::Develop d = core::raw::develop_from(&develop);
                    const render::raw::Preview look = render::raw::preview(*bytes, d, 900);
                    develop["exposure_ev"] = std::clamp(d.exposure_ev + render::raw::auto_exposure(look), -core::raw::kExposureMax, core::raw::kExposureMax);
                }
                return {formats::open_file(*bytes, name, options), QString()};
            } catch (const std::exception& error) {
                return {std::nullopt, QString::fromUtf8(error.what())};
            }
        }));
    });
    watcher->setFuture(QtConcurrent::run([path]() -> Read {
        try {
            auto bytes = std::make_shared<const std::string>(storage::read_file_bounded(path, render::raw::kFileMaxBytes));
            const std::string ref = core::bytes_ref(*bytes);
            return Read{std::move(bytes), ref, QString()};
        } catch (const std::exception& error) {
            return Read{nullptr, {}, QString::fromUtf8(error.what())};
        }
    }));
}

bool MainWindow::edit_raw_contents(const std::string& contents_id, const QString& title) {
    const auto found = book().smart_contents.find(contents_id);
    if (found == book().smart_contents.end() || !found->second || found->second->format != "raw") return false;
    const core::SmartContents contents = *found->second;
    const core::Bytes bytes = render::smart::file_bytes(book(), contents);
    if (!bytes) {
        flash(QStringLiteral("スマートオブジェクトの RAW ファイルが原稿にありません"), 6000, true);
        return false;
    }
    const Asked asked = asking();
    const Json start = contents.raw ? *contents.raw : Json::object();
    const camera_raw_open::Chosen chosen = camera_raw_open::run(this, bytes, title.isEmpty() ? QString::fromStdString(contents.name) : title, start, contents.dpi, true);
    if (chosen.ending != camera_raw_open::Ending::Open || !still(asked)) return false;
    Json op{{"op", "set_camera_raw"}, {"contents", contents_id}, {"develop", chosen.raw["develop"]}};
    op["adjust"] = chosen.raw.contains("adjust") ? chosen.raw["adjust"] : Json(nullptr);
    if (!apply_ops(Json::array({op}))) return false;
    flash(QStringLiteral("「%1」の現像の設定を変えました").arg(title.isEmpty() ? QString::fromStdString(contents.name) : title), 4000);
    return true;
}

}  // namespace genko::app
