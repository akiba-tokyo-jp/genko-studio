// PC5 J-e2 (video@1, SPEC.md PROD-01「ビデオレイヤー」): Photoshop's ビデオレイヤー — レイヤー → ビデオレイヤー ▸ (ファイルから新規
// ビデオレイヤー…・新規空白ビデオレイヤー・空白フレームを挿入・フレームを複製・フレームを削除・フッテージを置き換え…・フッテージを
// 解釈…・変更されたビデオを表示・フレームを復元・すべてのフレームを復元・フレームを再読み込み・ラスタライズ), ラスタライズ → ビデオ
// and ファイル → 読み込み → ビデオフレームからレイヤー… — each change of the book an op through apply_ops (render/ops_video), as
// the person; the footage's frames read into the book first (render/footage: the book's asset store). A stroke begun on a
// video layer alters the frame shown (alter_video_frame, from paint_layer). The user's decision 4: a video file is not read.

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <QSpinBox>

#include <filesystem>
#include <optional>

#include "app/ask.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/session.hpp"
#include "app/timeline.hpp"
#include "core/anim.hpp"
#include "core/paths.hpp"
#include "core/video.hpp"
#include "render/footage.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

namespace {

// (12 commands of ビデオレイヤー ▸ in Photoshop's order, and their words)
struct Command {
    const char* attribute;
    const char* title;
    const char* tip;
};
constexpr Command kVideo[] = {
    {"act_video_from_file", "ファイルから新規ビデオレイヤー…", "連番の画像・動く画像（GIF・APNG・WebP）をビデオレイヤーにします"},
    {"act_video_blank", "新規空白ビデオレイヤー", "空のビデオレイヤーを足します"},
    {"act_video_insert_blank", "空白フレームを挿入", "今のフレームに空白のフレームを差し込みます"},
    {"act_video_duplicate", "フレームを複製", "今のフレームを複製します"},
    {"act_video_delete", "フレームを削除", "今のフレームを削除して後ろを詰めます"},
    {"act_video_replace", "フッテージを置き換え…", "ビデオレイヤーの素材を別のものに替えます"},
    {"act_video_interpret", "フッテージを解釈…", "素材のフレームレート・アルファチャンネル・ピクセル縦横比を決めます"},
    {"act_video_show_altered", "変更されたビデオを表示", "描き替えたフレームを見せるか、素材のまま見せるかを切り替えます"},
    {"act_video_restore", "フレームを復元", "今のフレームの描き替えを外して素材に戻します"},
    {"act_video_restore_all", "すべてのフレームを復元", "ビデオレイヤーの描き替えをすべて外します"},
    {"act_video_reload", "フレームを再読み込み", "素材のファイルを読み直します"},
    {"act_video_rasterize", "ラスタライズ", "今のフレームの見えをふつうのペイントレイヤーにします"},
};

const QString kPictures = QStringLiteral(
    "画像・動く画像 (*.png *.apng *.jpg *.jpeg *.tif *.tiff *.webp *.bmp *.gif *.tga *.ico *.pnm *.ppm *.pgm *.pbm *.qoi *.exr *.hdr *.avif)");

}  // namespace

void MainWindow::build_video_actions() {
    for (const Command& command : kVideo) {
        const std::string attribute = command.attribute;
        const QString title = QString::fromUtf8(command.title);
        std::function<void()> slot;
        if (attribute == "act_video_from_file") slot = [this] { new_video_layer(); };
        else if (attribute == "act_video_blank") slot = [this] { video_op(Json{{"op", "add_video_layer"}}); };
        else if (attribute == "act_video_insert_blank") slot = [this] { video_op(Json{{"op", "video_insert_blank"}}); };
        else if (attribute == "act_video_duplicate") slot = [this] { video_op(Json{{"op", "video_duplicate"}}); };
        else if (attribute == "act_video_delete") slot = [this] { video_op(Json{{"op", "video_delete"}}); };
        else if (attribute == "act_video_replace") slot = [this] { replace_video_footage(); };
        else if (attribute == "act_video_interpret") slot = [this] { interpret_video_footage(); };
        else if (attribute == "act_video_restore") slot = [this] { video_op(Json{{"op", "video_restore"}}); };
        else if (attribute == "act_video_restore_all") slot = [this] { video_op(Json{{"op", "video_restore_all"}}); };
        else if (attribute == "act_video_reload") slot = [this] { reload_video_footage(); };
        else if (attribute == "act_video_rasterize") slot = [this] { video_op(Json{{"op", "rasterize_video"}}); };
        if (attribute == "act_video_show_altered") {
            QAction* shown = make(QString::fromLatin1(command.attribute), title, [] {}, {}, QString::fromUtf8(command.tip), true);
            shown->setChecked(true);
            connect(shown, &QAction::triggered, this, [this](bool on) {
                if (!video_op(Json{{"op", "set_video_layer"}, {"show_altered", on}})) {
                    const QSignalBlocker quiet(action(QStringLiteral("act_video_show_altered")));
                    action(QStringLiteral("act_video_show_altered"))->setChecked(!on);
                }
            });
            continue;
        }
        make(QString::fromLatin1(command.attribute), title, slot, {}, QString::fromUtf8(command.tip));
    }
    // ラスタライズ → ビデオ (layer.rasterize.video)
    make(QStringLiteral("act_rasterize_video"), QStringLiteral("ビデオ"), [this] { video_op(Json{{"op", "rasterize_video"}}); }, {},
         QStringLiteral("ビデオレイヤーの今のフレームの見えをふつうのペイントレイヤーにします"));
    // ファイル → 読み込み → ビデオフレームからレイヤー… (file.import.videoFramesToLayers)
    make(QStringLiteral("act_video_frames_to_layers"), QStringLiteral("ビデオフレームからレイヤー…"), [this] { video_frames_to_layers(); }, {},
         QStringLiteral("連番の画像・動く画像のフレームを、1 枚ずつレイヤーにします"));
}

void MainWindow::build_video_menus(QMenu* file, QMenu* layers) {
    add_to_import_menu(file, action(QStringLiteral("act_video_frames_to_layers")));
    // ラスタライズ ▸ ビデオ: after スマートオブジェクト, as Photoshop's
    if (QMenu* rasterize = layers->findChild<QMenu*>(QStringLiteral("menu_rasterize"))) {
        const QList<QAction*> items = rasterize->actions();
        const qsizetype smart = items.indexOf(action(QStringLiteral("act_rasterize_smart")));
        rasterize->insertAction(smart >= 0 && smart + 1 < items.size() ? items.at(smart + 1) : nullptr, action(QStringLiteral("act_rasterize_video")));
    }
    // 変更されたビデオを表示: checked as the video layer chosen shows
    if (QMenu* video = layers->findChild<QMenu*>(QStringLiteral("menu_video_layers"))) {
        connect(video, &QMenu::aboutToShow, this, [this] {
            const core::Page* page = current_page();
            const auto folder = video_folder();
            bool on = true;
            if (page != nullptr && folder) {
                if (const Json* video = core::video_row(*page, Json(*folder))) on = video->at("show_altered").get<bool>();
            }
            const QSignalBlocker quiet(action(QStringLiteral("act_video_show_altered")));
            action(QStringLiteral("act_video_show_altered"))->setChecked(on);
        });
    }
}

std::optional<std::string> MainWindow::video_folder() const {
    const core::Page* page = current_page();
    if (page == nullptr || !core::anim::is_animation(*page)) return std::nullopt;
    const auto is_video = [&](const std::string& id) { return core::video_row(*page, Json(id)) != nullptr; };
    if (const core::Layer* layer = target_layer()) {
        if (is_video(layer->id)) return layer->id;
        if (layer->parent_id.is_string() && is_video(layer->parent_id.get<std::string>())) return layer->parent_id.get<std::string>();
    }
    if (timeline_ != nullptr) {
        if (const auto folder = timeline_->current_folder(); folder && is_video(*folder)) return folder;
    }
    return std::nullopt;
}

std::int64_t MainWindow::video_frame() const {
    const core::Page* page = current_page();
    if (page == nullptr) return 1;
    if (const auto found = anim_frames.find(page->id); found != anim_frames.end()) return found->second;
    return timeline_ != nullptr ? timeline_->frame : 1;
}

bool MainWindow::video_op(Json op) {
    const core::Page* page = current_page();
    if (page == nullptr) return false;
    op["page"] = page->index.json();
    const std::string name = op.value("op", std::string());
    if (name != "add_video_layer" && name != "footage_to_layers") {
        const auto folder = video_folder();
        if (!folder) {
            flash(QStringLiteral("ビデオレイヤーを選んでください（レイヤー → ビデオレイヤー → 新規空白ビデオレイヤー・ファイルから新規ビデオレイヤー…）"), 5000, true);
            return false;
        }
        op["folder"] = *folder;
        if (name != "video_restore_all" && name != "set_video_layer" && name != "replace_footage" && !op.contains("at")) op["at"] = video_frame();
    }
    const bool ok = apply_ops(Json::array({op}));
    if (ok) {
        for (const Json& result : last_results_) {
            if (!result.is_object()) continue;
            if (result.contains("folder") && result["folder"].is_string()) set_target_layer(result["folder"].get<std::string>());
            if (name == "rasterize_video" && result.contains("id") && result["id"].is_string()) set_target_layer(result["id"].get<std::string>());
        }
    }
    if (timeline_ != nullptr) timeline_->refresh();
    if (layer_panel() != nullptr) layer_panel()->refresh();
    return ok;
}

const core::Layer* MainWindow::alter_video_frame(const core::Layer& folder) {
    const std::string id = folder.id;
    const core::Page* page = current_page();
    if (page == nullptr) return nullptr;
    const Json op{{"op", "video_alter"}, {"page", page->index.json()}, {"folder", id}, {"at", video_frame()}};
    if (!video_op(op)) return nullptr;
    for (const Json& result : last_results_) {
        if (result.is_object() && result.contains("cel") && result["cel"].is_string()) {
            // (a cel made now: what is drawn next is one step with it — apply_ops)
            if (result.value("made", false)) video_alter_ = VideoAlter{op, result["cel"].get<std::string>(), book().revision};
            set_target_layer(result["cel"].get<std::string>());
            if (layer_panel() != nullptr) layer_panel()->refresh();
            return target_layer();
        }
    }
    return nullptr;
}

std::optional<Json> MainWindow::read_video_footage(const QString& path, std::optional<double> fps, std::int64_t every) {
    if (!session_->path()) {
        flash(QStringLiteral("ビデオの素材を読み込む前に、原稿を保存します（ファイル → 別名で保存…）"), 6000, true);
        return std::nullopt;
    }
    render::footage::ReadOptions options;
    options.fps = fps;
    options.every = every;
    const std::uint64_t held = core::book_footage_bytes(book());
    options.most_bytes = held >= core::kFootageMaxBytes ? 0 : core::kFootageMaxBytes - held;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        render::footage::Footage read =
            render::footage::read_footage(core::path_from_utf8(path.toStdString()), options, render::footage::book_store(*session_->path()));
        QApplication::restoreOverrideCursor();
        if (!read.notes.empty()) flash(QString::fromStdString(read.notes.front()), 5000);
        return read.json;
    } catch (const std::exception& error) {
        QApplication::restoreOverrideCursor();
        flash(QStringLiteral("素材を読み込めません:\n%1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return std::nullopt;
    }
}

void MainWindow::new_video_layer(const QString& path) {
    if (current_page() == nullptr) return;
    const QString file = path.isEmpty() ? ask::open_path(this, QStringLiteral("ファイルから新規ビデオレイヤー"), kPictures) : path;
    if (file.isEmpty()) return;
    const auto footage = read_video_footage(file, std::nullopt);
    if (!footage) return;
    if (video_op(Json{{"op", "add_video_layer"}, {"footage", *footage}}) && timeline_ != nullptr) timeline_->set_frame(1, false);
}

void MainWindow::replace_video_footage(const QString& path) {
    if (!video_folder()) {
        video_op(Json{{"op", "replace_footage"}});  // (says to choose one)
        return;
    }
    const QString file = path.isEmpty() ? ask::open_path(this, QStringLiteral("フッテージを置き換え"), kPictures) : path;
    if (file.isEmpty()) return;
    const auto footage = read_video_footage(file, std::nullopt);
    if (footage) video_op(Json{{"op", "replace_footage"}, {"footage", *footage}});
}

void MainWindow::reload_video_footage() {
    const core::Page* page = current_page();
    const auto folder = video_folder();
    const Json* video = page != nullptr && folder ? core::video_row(*page, Json(*folder)) : nullptr;
    if (video == nullptr) {
        video_op(Json{{"op", "replace_footage"}});  // (says to choose one)
        return;
    }
    const Json& footage = video->at("footage");
    if (!footage.is_object() || !footage.contains("source")) {
        flash(QStringLiteral("このビデオレイヤーには読み直す元のファイルがありません"), 5000, true);
        return;
    }
    const QString source = QString::fromStdString(footage.at("source").at("path").get<std::string>());
    std::error_code ec;
    if (!std::filesystem::exists(core::path_from_utf8(source.toStdString()), ec)) {
        flash(QStringLiteral("元のファイルが見つかりません:\n%1").arg(source), 6000, true);
        return;
    }
    const auto read = read_video_footage(source, footage.at("fps").get<double>());
    if (!read) return;
    Json again = *read;
    again["alpha"] = footage.at("alpha");
    again["par"] = footage.at("par");
    video_op(Json{{"op", "replace_footage"}, {"footage", again}});
}

void MainWindow::interpret_video_footage() {
    const core::Page* page = current_page();
    const auto folder = video_folder();
    const Json* video = page != nullptr && folder ? core::video_row(*page, Json(*folder)) : nullptr;
    if (video == nullptr || !video->at("footage").is_object()) {
        if (video == nullptr) video_op(Json{{"op", "set_video_layer"}});  // (says to choose one)
        else flash(QStringLiteral("空白のビデオレイヤーには解釈する素材がありません"), 5000, true);
        return;
    }
    const Json footage = video->at("footage");
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("フッテージを解釈"));
    auto* fps = new QDoubleSpinBox(&dialog);
    fps->setObjectName(QStringLiteral("interpret_fps"));
    fps->setRange(core::kTimelineFpsMin, core::kTimelineFpsMax);
    fps->setDecimals(3);
    fps->setValue(footage.at("fps").get<double>());
    auto* alpha = new QComboBox(&dialog);
    alpha->setObjectName(QStringLiteral("interpret_alpha"));
    alpha->addItem(QStringLiteral("ストレート - マットなし"), QStringLiteral("straight"));
    alpha->addItem(QStringLiteral("プリマルチプライ - マット"), QStringLiteral("premultiplied"));
    alpha->addItem(QStringLiteral("無視"), QStringLiteral("ignore"));
    alpha->setCurrentIndex(alpha->findData(QString::fromStdString(footage.at("alpha").get<std::string>())));
    auto* par = new QDoubleSpinBox(&dialog);
    par->setObjectName(QStringLiteral("interpret_par"));
    par->setRange(0.01, 100);
    par->setDecimals(4);
    par->setValue(footage.at("par").get<double>());
    auto* form = new QFormLayout(&dialog);
    form->addRow(QStringLiteral("フレームレート:"), fps);
    form->addRow(QStringLiteral("アルファチャンネル:"), alpha);
    form->addRow(QStringLiteral("ピクセル縦横比:"), par);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    Json op{{"op", "set_video_layer"}};
    if (fps->value() != footage.at("fps").get<double>()) op["fps"] = fps->value();
    if (alpha->currentData().toString().toStdString() != footage.at("alpha").get<std::string>()) op["alpha"] = alpha->currentData().toString().toStdString();
    if (par->value() != footage.at("par").get<double>()) op["par"] = par->value();
    if (op.size() > 1) video_op(op);
}

void MainWindow::video_frames_to_layers(const QString& path, std::int64_t every, std::optional<bool> animation) {
    if (current_page() == nullptr) return;
    const QString file = path.isEmpty() ? ask::open_path(this, QStringLiteral("ビデオフレームからレイヤー"), kPictures) : path;
    if (file.isEmpty()) return;
    if (every <= 0 || !animation) {  // (Photoshop's window: every n-th frame, a frame animation made of them)
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("ビデオフレームからレイヤー"));
        auto* step = new QSpinBox(&dialog);
        step->setObjectName(QStringLiteral("frames_every"));
        step->setRange(1, 3000);
        step->setValue(every > 0 ? static_cast<int>(every) : 1);
        step->setPrefix(QStringLiteral("フレームを "));
        step->setSuffix(QStringLiteral(" 枚おきに制限"));
        auto* cels = new QCheckBox(QStringLiteral("フレームアニメーションを作成（アニメーションフォルダーのセルにする）"), &dialog);
        cels->setObjectName(QStringLiteral("frames_animation"));
        cels->setChecked(animation.value_or(false));
        auto* form = new QFormLayout(&dialog);
        form->addRow(new QLabel(QStringLiteral("読み込むのは 500 フレームまでです"), &dialog));
        form->addRow(step);
        form->addRow(cels);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(buttons);
        if (ask::exec(&dialog) != QDialog::Accepted) return;
        every = step->value();
        animation = cels->isChecked();
    }
    const auto footage = read_video_footage(file, std::nullopt, every);
    if (!footage) return;
    video_op(Json{{"op", "footage_to_layers"}, {"footage", *footage}, {"into", *animation ? "cels" : "layers"}});
}

}  // namespace genko::app
