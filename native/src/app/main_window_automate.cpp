// ファイル → 自動処理 ▸・スクリプト ▸ and the steps run in the window (PC5 I-0, SPEC.md AUTO-01: the frame the later groups
// fill — I-e バッチ・ドロップレット・画像プロセッサー・スクリプトイベント・参照…, I-a/I-f/I-g/I-h their commands; the rules of
// service/automate.hpp and service/script_events.hpp).

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QProgressDialog>
#include <QSpinBox>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QtConcurrent>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <cmath>
#include <filesystem>
#include <utility>

#include "app/actions.hpp"
#include "app/ask.hpp"
#include "app/automate_dialogs.hpp"
#include "app/hdr_dialog.hpp"
#include "app/main_window.hpp"
#include "app/open_dialog.hpp"
#include "app/session.hpp"
#include "app/stoppable.hpp"
#include "core/paths.hpp"
#include "core/actor.hpp"
#include "service/automate.hpp"
#include "service/automate_photos.hpp"
#include "service/script_events.hpp"
#include "service/wording.hpp"
#include "render/page.hpp"

namespace genko::app {

using core::Json;
namespace fs = std::filesystem;

void MainWindow::build_automate_actions() {
    // (the later groups make their commands here too)
    // PC5 I-e: 自動処理 ▸ (Photoshop's バッチ…・ドロップレットを作成… / 条件付きモード変更…・画像を合わせる…・レンズ補正…) and
    // スクリプト ▸ (画像プロセッサー… / スクリプトイベントマネージャー… / 参照…)
    make(QStringLiteral("act_batch"), QStringLiteral("バッチ…"), [this] { batch_dialog(); }, {},
         QStringLiteral("フォルダー・ファイルの画像や原稿に、アクションか手順のファイルを 1 つずつ流します"));
    make(QStringLiteral("act_create_droplet"), QStringLiteral("ドロップレットを作成…"), [this] { create_droplet_dialog(); }, {},
         QStringLiteral("ファイルをドロップすると手順を流す、ドロップレット（と起動ファイル）を作ります"));
    make(QStringLiteral("act_conditional_mode_change"), QStringLiteral("条件付きモード変更…"), [this] { mode_change_dialog(); }, {},
         QStringLiteral("このページのカラーモードが選んだモードのときだけ、別のモードに変換します（取り消せます）"));
    make(QStringLiteral("act_fit_image"), QStringLiteral("画像を合わせる…"), [this] { fit_image_dialog(); }, {},
         QStringLiteral("このページを、縦横比を変えずに幅と高さ（pixel）の中に収まる大きさにします（取り消せます）"));
    make(QStringLiteral("act_lens_correction_batch"), QStringLiteral("レンズ補正…"), [this] { lens_batch_dialog(); }, {},
         QStringLiteral("フォルダー・ファイルの画像に、EXIF から選んだレンズプロファイルでレンズ補正をかけて書き出します"));
    make(QStringLiteral("act_image_processor"), QStringLiteral("画像プロセッサー…"), [this] { image_processor_dialog(); }, {},
         QStringLiteral("画像を JPEG・PSD・TIFF・PNG にまとめて書き出します（サイズに合わせる・sRGB に変換・アクション）"));
    make(QStringLiteral("act_script_events"), QStringLiteral("スクリプトイベントマネージャー…"), [this] { script_events_dialog(); }, {},
         QStringLiteral("起動・新規・開く・保存・閉じる・プリント・書き出しのときに流す手順を結び付けます"));
    make(QStringLiteral("act_script_browse"), QStringLiteral("参照…"), [this] { browse_script(); }, {},
         QStringLiteral("手順のファイル（操作の JSON か 1 行 1 操作）を選んで、開いている原稿に流します（genko script と同じ）"));
    // PC5 I-h: 写真の切り抜きと角度補正, HDR Pro に統合… (new books: the page and the pictures they read are not changed)
    make(QStringLiteral("act_crop_straighten_photos"), QStringLiteral("写真の切り抜きと角度補正"), [this] { crop_photos(); }, {},
         QStringLiteral("スキャンしたページの写真を 1 枚ずつ見つけ、傾きを直して新しい原稿にします（元のページは変わりません）"));
    make(QStringLiteral("act_merge_to_hdr_pro"), QStringLiteral("HDR Pro に統合…"), [this] { merge_to_hdr_dialog(); }, {},
         QStringLiteral("露出違いの画像を 1 枚の HDR 画像（32 bit）か、トーンを整えた 16・8 bit の画像にして新しい原稿にします"));
    build_photomerge_actions();  // (PC5 I-g: Photomerge…, ファイルをスタックとして読み込み…・統計…, 編集's レイヤーを自動整列…・自動合成…)
}

void MainWindow::build_automate_menus() {
    // (the later groups put their commands in automate_menu_ and scripts_menu_ here, in Photoshop's order)
    for (const char* name : {"act_batch", "act_create_droplet"}) automate_menu_->addAction(action(QString::fromLatin1(name)));
    automate_menu_->addSeparator();
    // (PC5 I-h: 写真の切り抜きと角度補正 — I-f's コンタクトシート II… after it —, then a separator)
    automate_menu_->addAction(action(QStringLiteral("act_crop_straighten_photos")));
    automate_menu_->addSeparator();
    for (const char* name : {"act_conditional_mode_change", "act_fit_image", "act_lens_correction_batch"}) {
        automate_menu_->addAction(action(QString::fromLatin1(name)));
    }
    // (PC5 I-h: HDR Pro に統合… last, after a separator — I-g's Photomerge… after it)
    automate_menu_->addSeparator();
    automate_menu_->addAction(action(QStringLiteral("act_merge_to_hdr_pro")));
    // スクリプト ▸: 画像プロセッサー… first, then すべてのマスクを統合 (PC3 B2) / スクリプトイベントマネージャー… / 参照…
    const QList<QAction*> scripts = scripts_menu_->actions();
    scripts_menu_->insertAction(scripts.isEmpty() ? nullptr : scripts.front(), action(QStringLiteral("act_image_processor")));
    // (PC5 I-a: すべての空のレイヤーを削除・すべてのレイヤー効果を統合 after it, before すべてのマスクを統合 — main_window_layers.cpp)
    for (const char* name : {"act_delete_empty_layers", "act_bake_layer_effects"})
        scripts_menu_->insertAction(scripts.isEmpty() ? nullptr : scripts.front(), action(QString::fromLatin1(name)));
    scripts_menu_->addSeparator();
    scripts_menu_->addAction(action(QStringLiteral("act_script_events")));
    scripts_menu_->addSeparator();
    scripts_menu_->addAction(action(QStringLiteral("act_script_browse")));
    build_photomerge_menus();  // (PC5 I-g: Photomerge… last in 自動処理 ▸; ファイルをスタックとして読み込み…・統計… before 参照…)
    automate_menu_->menuAction()->setVisible(!automate_menu_->actions().isEmpty());  // (shown once it holds one)
}

bool MainWindow::steps_allowed(const Json& ops, const QString& label, const QString& hint) {
    try {
        service::automate::check_steps(ops, core::Actor(session_->actor()));
    } catch (const core::Error& error) {
        flash(QStringLiteral("「%1」: %2%3").arg(label, wording::error(std::string(error.what())), hint), 8000, true);
        return false;
    }
    return true;
}

bool MainWindow::run_steps(const Json& steps, bool recorded, const QString& label) {
    const core::Page* page = current_page();
    if (page == nullptr) {
        flash(QStringLiteral("原稿を開いていないので、「%1」を流しませんでした").arg(label), 6000, true);
        return false;
    }
    Json ops = steps;
    if (recorded) {  // (an オートアクション: on the page in front, its layer drawn on and its chosen panel, as 実行 plays it)
        const core::Layer* layer = target_layer();
        const core::Frame* frame = selected_frame();
        ops = actions::replay(steps, page->index.json(), layer != nullptr ? std::optional<std::string>(layer->id) : std::nullopt,
                              frame != nullptr ? std::optional<std::string>(frame->id) : std::nullopt);
    }
    const core::Actor actor(session_->actor());
    if (!steps_allowed(ops, label)) return false;
    // (the journal names each change by the steps; an オートアクション being recorded does not take them in; both set
    // back whatever happens)
    struct Named {
        Session& session;
        std::optional<Json>& recording;
        std::optional<Json> kept;
        ~Named() {
            session.set_script({});
            recording = std::move(kept);
        }
    } named{*session_, recording_, std::move(recording_)};
    recording_.reset();
    session_->set_script(label.toStdString());
    Json pending = Json::array();
    const auto flush = [&]() {
        if (pending.empty()) return true;
        const bool ok = apply_ops(pending);
        pending = Json::array();
        return ok;
    };
    for (const Json& op : ops) {
        const auto name = op.find("op");
        if (name == op.end() || !name->is_string() || !service::automate::pseudo_op(name->get<std::string>())) {
            pending.push_back(op);
            continue;
        }
        if (!flush()) return false;
        Json made;
        try {
            made = service::automate::expand_pseudo(book(), op);
            service::automate::check_steps(made, actor);
        } catch (const std::exception& error) {
            flash(QStringLiteral("「%1」: %2").arg(label, wording::error(std::string(error.what()))), 6000, true);
            return false;
        }
        pending = std::move(made);
        if (!flush()) return false;
    }
    return flush();
}

// --- PC5 I-e: the windows of 自動処理 ▸ and スクリプト ▸ --------------------------------------------------------------------

namespace {

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }
fs::path fpath(const QString& text) { return core::path_from_utf8(QDir::fromNativeSeparators(text.trimmed()).toStdString()); }

}  // namespace

std::optional<service::automate::Inputs> MainWindow::inputs_of(const SourceChoice& source) {
    namespace automate = service::automate;
    std::vector<fs::path> given;
    automate::InputOptions listing;
    if (source.kind() == QLatin1String("folder")) {
        given.push_back(fpath(source.folder->text()));
        listing.recurse = source.recurse->isChecked();
    } else {
        for (int i = 0; i < source.files->count(); ++i) given.push_back(fpath(source.files->item(i)->text()));
    }
    try {
        automate::Inputs inputs = automate::list_inputs(given, listing);
        if (inputs.items.empty()) {
            flash(QStringLiteral("処理するファイルがありません"), 5000, true);
            return std::nullopt;
        }
        return inputs;
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return std::nullopt;
    }
}

bool MainWindow::run_automate(const QString& title, const std::vector<service::automate::Input>& inputs, const Json& steps,
                              const service::automate::Output& output, service::automate::Options options, const Json& skipped) {
    namespace automate = service::automate;
    QProgressDialog progress(QStringLiteral("%1を実行しています…").arg(title), QStringLiteral("中止"), 0, static_cast<int>(inputs.size()), this);
    progress.setWindowTitle(title);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);
    options.actor = core::Actor(session_->actor());  // (the window's person)
    options.open = file_open_options();
    options.book_brushes = false;  // (the app follows its brushes itself)
    options.cancelled = [&progress] {
        QCoreApplication::processEvents();
        return progress.wasCanceled();
    };
    options.progress = [&progress](std::size_t done, std::size_t, const fs::path& now) {
        progress.setValue(static_cast<int>(done));
        if (!now.empty()) progress.setLabelText(QStringLiteral("%1").arg(QDir::toNativeSeparators(qpath(now.filename()))));
    };
    automate::Result result;
    {
        // (the files the run writes and the books it saves raise no script event: Photoshop's batch neither)
        const service::script_events::Firing quiet;
        try {
            result = automate::run(inputs, steps, output, options);
        } catch (const std::exception& error) {
            progress.reset();
            flash(QStringLiteral("%1: %2").arg(title, wording::error(QString::fromUtf8(error.what()))), 8000, true);
            return false;
        }
    }
    progress.reset();
    QStringList said;
    for (const Json& e : result.errors) {
        said << QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(QString::fromStdString(e.value("input", std::string()))),
                                            wording::error(QString::fromStdString(e.value("error", std::string()))));
    }
    for (const Json& s : skipped) {
        said << QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(QString::fromStdString(s.value("path", std::string()))),
                                            QString::fromStdString(s.value("reason", std::string())));
    }
    const QString done = QStringLiteral("%1: %2 個のファイルを処理しました%3").arg(title).arg(result.files.size())
                             .arg(result.cancelled ? QStringLiteral("（中止しました）") : QString());
    if (!said.isEmpty()) {
        const int shown = std::min<int>(static_cast<int>(said.size()), 20);
        QStringList first = said.mid(0, shown);
        if (said.size() > shown) first << QStringLiteral("…ほか %1 件").arg(said.size() - shown);
        ask::warning(this, title, done + QStringLiteral("。処理できなかったもの:\n") + first.join(QLatin1Char('\n')));
    }
    flash(done, 6000, !result.errors.empty());
    return result.errors.empty();
}

void MainWindow::batch_dialog() {
    namespace automate = service::automate;
    BatchDialog dialog(this);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    std::optional<StepsChoice::Chosen> chosen;
    try {
        chosen = dialog.steps.chosen();
    } catch (const std::exception& error) {
        flash(QStringLiteral("バッチ: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return;
    }
    if (!chosen) return;
    const QString label = QStringLiteral("バッチ: %1").arg(QString::fromStdString(chosen->name));
    const QString to = dialog.destination->currentData().toString();
    automate::Output output;
    output.to = to == QLatin1String("folder") ? automate::Output::To::Folder : to == QLatin1String("save") ? automate::Output::To::Book : automate::Output::To::None;
    output.folder = fpath(dialog.folder->text());
    output.format = dialog.format->currentData().toString().toStdString();
    output.quality = dialog.quality->value();
    output.prefix = dialog.prefix->text().toStdString();
    output.suffix = dialog.suffix->text().toStdString();
    output.serial = dialog.numbered->isChecked() ? 1 : 0;
    automate::Options options;
    options.recorded = chosen->recorded;
    options.stop_on_error = dialog.on_error->currentData().toString() == QLatin1String("stop");
    options.script = label.toStdString();
    if (dialog.source.kind() == QLatin1String("open")) {
        if (current_page() == nullptr) {
            flash(QStringLiteral("原稿を開いていないので、バッチを流しませんでした"), 6000, true);
            return;
        }
        // (the book as it is now, held while the batch runs; its pages all read first — as プリント and 書き出し: a page not
        // read would come out blank — the review of PC5 I-e, P2-3)
        const std::shared_ptr<const core::Document> shown = session_->snapshot();
        if (!shown->deferred.empty()) {
            flash(wording::error(QStringLiteral("the book's pages are still being read: try again in a moment")), 6000, true);
            return;
        }
        const bool picture = doc_ >= 0 && documents_[static_cast<std::size_t>(doc_)].picture;
        if (output.to != automate::Output::To::Folder) {
            // (なし: the steps in the window, every page — one change, undone at once —; 保存して閉じる: then saved)
            Json ops;
            try {
                ops = automate::aim_steps(*shown, chosen->steps, picture, chosen->recorded);
            } catch (const std::exception& error) {
                flash(QStringLiteral("%1: %2").arg(label, wording::error(QString::fromUtf8(error.what()))), 6000, true);
                return;
            }
            const service::script_events::Firing quiet;  // (the batch's save raises no event)
            if (!run_steps(ops, false, label)) return;
            if (output.to == automate::Output::To::Book) save();
            return;
        }
        // (フォルダー: the book as it is, run on a copy and written; the window's book unchanged)
        automate::Input input;
        input.book = !picture;
        if (picture && documents_[static_cast<std::size_t>(doc_)].source) {
            input.path = *documents_[static_cast<std::size_t>(doc_)].source;
        } else if (session_->path()) {
            input.path = *session_->path();
        } else {
            input.path = core::path_from_utf8((book().title.empty() ? std::string("無題") : book().title) + (picture ? ".png" : ""));
        }
        options.open_book = shown;
        run_automate(QStringLiteral("バッチ"), {input}, chosen->steps, output, options, Json::array());
        return;
    }
    const auto inputs = inputs_of(dialog.source);
    if (!inputs) return;
    run_automate(QStringLiteral("バッチ"), inputs->items, chosen->steps, output, options, inputs->skipped);
}

void MainWindow::create_droplet_dialog() {
    namespace automate = service::automate;
    DropletDialog dialog(this);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    try {
        const std::optional<StepsChoice::Chosen> chosen = dialog.steps.chosen();
        if (!chosen) return;
        automate::check_steps(chosen->steps, core::Actor(session_->actor()));
        automate::Droplet droplet;
        const fs::path place = fpath(dialog.place->text());
        fs::path stem = place.filename();
        if (stem.extension() == automate::kDropletSuffix) stem.replace_extension();
        droplet.name = core::path_to_utf8(stem);
        droplet.steps = chosen->steps;
        droplet.recorded = chosen->recorded;
        if (dialog.own_folder->isChecked()) droplet.output = fs::absolute(fpath(dialog.folder->text()));
        droplet.format = dialog.format->currentData().toString().toStdString();
        droplet.quality = dialog.quality->value();
        droplet.prefix = dialog.prefix->text().toStdString();
        droplet.suffix = dialog.suffix->text().toStdString();
        const fs::path written = automate::write_droplet(fs::absolute(place), droplet);
        QStringList made{QDir::toNativeSeparators(qpath(written))};
        if (dialog.launcher->isChecked()) {
            const fs::path genko = automate::launcher_program(fpath(QCoreApplication::applicationFilePath()));
            for (const fs::path& l : automate::write_launchers(written, genko)) made << QDir::toNativeSeparators(qpath(l));
        }
        flash(QStringLiteral("ドロップレットを作りました: %1").arg(made.join(QStringLiteral("、"))), 8000);
    } catch (const std::exception& error) {
        flash(QStringLiteral("ドロップレットを作れませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 8000, true);
    }
}

void MainWindow::image_processor_dialog() {
    namespace automate = service::automate;
    ImageProcessorDialog dialog(this);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const automate::ImageProcessor processor = dialog.processor();
    automate::Options options;
    Json steps = Json::array();
    try {
        if (dialog.run_action->isChecked()) {
            if (const auto chosen = dialog.steps.chosen()) {
                steps = chosen->steps;
                options.recorded = chosen->recorded;
            }
        }
        const automate::Output output = automate::processor_output(processor, fpath(dialog.folder->text()));
        options.first = [processor](const core::Document& doc, bool) { return automate::processor_steps(processor, doc); };
        options.script = "画像プロセッサー";
        const auto inputs = inputs_of(dialog.source);
        if (!inputs) return;
        run_automate(QStringLiteral("画像プロセッサー"), inputs->items, steps, output, options, inputs->skipped);
    } catch (const std::exception& error) {
        flash(QStringLiteral("画像プロセッサー: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
    }
}

void MainWindow::fit_image_dialog() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    const int w = static_cast<int>(std::lround(page->spec.width_mm.value() / 25.4 * page->spec.dpi.value()));
    const int h = static_cast<int>(std::lround(page->spec.height_mm.value() / 25.4 * page->spec.dpi.value()));
    FitImageDialog dialog(this, std::max(1, w), std::max(1, h));
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    run_steps(Json::array({dialog.step(page->index.json())}), false, QStringLiteral("画像を合わせる"));
}

void MainWindow::mode_change_dialog() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    ModeChangeDialog dialog(this);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const Json step = dialog.step(page->index.json());
    try {
        if (service::automate::expand_pseudo(book(), step).empty()) {
            flash(QStringLiteral("このページのモードは選んだ変換元のモードではないので、変えませんでした"), 5000);
            return;
        }
    } catch (const std::exception& error) {
        flash(QStringLiteral("条件付きモード変更: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return;
    }
    run_steps(Json::array({step}), false, QStringLiteral("条件付きモード変更"));
}

void MainWindow::lens_batch_dialog() {
    namespace automate = service::automate;
    LensBatchDialog dialog(this);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    automate::Output output;
    output.to = automate::Output::To::Folder;
    output.folder = fpath(dialog.folder->text());
    output.format = dialog.format->currentData().toString().toStdString();
    output.quality = dialog.quality->value();
    automate::Options options;
    const Json settings = dialog.settings();
    options.first = [settings](const core::Document& doc, bool) { return automate::lens_steps(settings, doc); };
    options.script = "レンズ補正";
    const auto inputs = inputs_of(dialog.source);
    if (!inputs) return;
    run_automate(QStringLiteral("レンズ補正"), inputs->items, Json::array(), output, options, inputs->skipped);
}

void MainWindow::script_events_dialog() {
    namespace events = service::script_events;
    const events::Loaded loaded = events::load();
    ScriptEventsDialog dialog(this, loaded.table, loaded.problem);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    // (the table written here only, with its seal: service/script_events.hpp)
    if (const auto why = events::save(dialog.table())) {
        flash(QStringLiteral("スクリプトイベントを保存できませんでした: %1").arg(QString::fromStdString(*why)), 6000, true);
        return;
    }
    script_events_told_ = false;
    flash(QStringLiteral("スクリプトイベントを保存しました"), 3000);
}

void MainWindow::browse_script() {
    namespace automate = service::automate;
    const QString chosen = ask::open_path(this, QStringLiteral("スクリプトを参照"), QStringLiteral("手順 (*.json *.txt *.genkoscript);;すべてのファイル (*)"));
    if (chosen.isEmpty()) return;
    const fs::path file = fpath(chosen);
    const QString name = qpath(file.stem());
    Json steps;
    try {
        steps = automate::read_steps_file(file);
    } catch (const std::exception& error) {
        flash(QStringLiteral("「%1」: %2").arg(name, wording::error(QString::fromUtf8(error.what()))), 8000, true);
        return;
    }
    if (!steps_allowed(steps, name)) return;  // (which step may not be one, said before anything runs)
    if (!ask::question(this, QStringLiteral("スクリプトを参照"),
                       QStringLiteral("「%1」の %2 個の手順を、開いている原稿に流しますか？（元に戻すで戻せます）").arg(name).arg(steps.size()))) {
        return;
    }
    // (the ops as written — as `genko script` applies the same file —; no book open: said)
    run_steps(steps, false, name);
}

void MainWindow::raise_opened(service::script_events::Event event) {
    if (std::exchange(start_pending_, false)) script_event(service::script_events::Event::StartApplication);
    script_event(event);
}

void MainWindow::script_event(service::script_events::Event event) {
    namespace events = service::script_events;
    if (events::Firing::active()) return;  // (the steps an event runs raise none)
    const events::Loaded loaded = events::load();
    if (loaded.problem && !script_events_told_) {
        script_events_told_ = true;
        flash(QString::fromStdString(*loaded.problem), 8000, true);
    }
    const std::vector<events::Binding> bound = events::bound_to(loaded.table, event);
    if (bound.empty()) return;
    const events::Firing firing;
    for (const events::Binding& binding : bound) {
        try {
            const events::Steps steps = events::steps_of(binding, event);
            if (!run_steps(steps.steps, steps.recorded, QString::fromStdString(steps.label))) break;
        } catch (const std::exception& error) {
            flash(QStringLiteral("スクリプトイベント「%1」: %2").arg(QString::fromStdString(binding.name), wording::error(std::string(error.what()))), 6000, true);
            break;
        }
    }
}


// --- PC5 I-h: HDR Pro に統合…・写真の切り抜きと角度補正 --------------------------------------------------------------------------
// (the work on a worker with its progress window and 中止: app/stoppable.hpp's run_stoppable — the review's P2-4)

void MainWindow::crop_photos() {
    const core::Page* page = current_page();
    if (page == nullptr) {
        flash(QStringLiteral("原稿を開いていないので、写真の切り抜きと角度補正をしませんでした"), 6000, true);
        return;
    }
    // (the page shown, by its id — in コンテンツを編集's tab its contents' page —, read as it is now: the book unchanged)
    const std::shared_ptr<const core::Document> now = contents_ ? shown_book() : session_->snapshot();
    const std::optional<std::size_t> index = shown_page_at(now, page);
    if (!index) return;
    const QString title = QStringLiteral("写真の切り抜きと角度補正");
    const QPointer<MainWindow> alive(this);
    std::optional<std::vector<formats::Opened>> made;
    try {
        const formats::OpenOptions options = file_open_options();
        made = run_stoppable(this, title, QStringLiteral("写真を探しています…"), [now, at = *index, options](const std::function<bool()>& stopped) {
            return service::automate::crop_and_straighten(*now, at, options, nullptr, stopped);
        });
    } catch (const std::exception& error) {
        if (alive) flash(QStringLiteral("%1: %2").arg(title, wording::error(QString::fromUtf8(error.what()))), 8000, true);
        return;
    }
    if (!alive) return;
    if (!made) {
        flash(QStringLiteral("%1を中止しました").arg(title), 4000);
        return;
    }
    if (made->empty()) {
        flash(QStringLiteral("%1: 一様な背景の上に写真が見つかりませんでした").arg(title), 6000, true);
        return;
    }
    const std::size_t count = made->size();
    for (formats::Opened& book : *made) add_picture_book(std::move(book.doc), book.notes);
    flash(QStringLiteral("%1: %2 枚の写真を新しい原稿にしました").arg(title).arg(count), 6000);
}

void MainWindow::merge_to_hdr_dialog() {
    namespace automate = service::automate;
    const QString title = QStringLiteral("HDR Pro に統合");
    std::vector<std::pair<QString, int>> open;
    for (std::size_t i = 0; i < documents_.size(); ++i) open.emplace_back(documents_[i].title(), static_cast<int>(i));
    HdrFilesDialog chosen(this, open);
    if (ask::exec(&chosen) != QDialog::Accepted) return;
    // (what to read, in order: a file, or a book open here — its page shown, as it is now)
    std::vector<automate::HdrSource> sources;
    try {
        if (chosen.use->currentData().toString() == QLatin1String("folder")) {
            const automate::Inputs inputs =
                automate::list_inputs({core::path_from_utf8(QDir::fromNativeSeparators(chosen.folder->text().trimmed()).toStdString())});
            for (const automate::Input& input : inputs.items) {
                if (!input.book) sources.push_back(automate::HdrSource{input.path, nullptr, 0, core::path_to_utf8(input.path.filename())});
            }
        } else {
            for (int i = 0; i < chosen.files->count(); ++i) {
                const QString data = chosen.files->item(i)->data(Qt::UserRole).toString();
                if (data.startsWith(QLatin1String("file:"))) {
                    const fs::path file = core::path_from_utf8(data.mid(5).toStdString());
                    sources.push_back(automate::HdrSource{file, nullptr, 0, core::path_to_utf8(file.filename())});
                    continue;
                }
                const int tab = data.mid(5).toInt();
                if (tab < 0 || tab >= static_cast<int>(documents_.size())) continue;
                const Document& d = documents_[static_cast<std::size_t>(tab)];
                automate::HdrSource s;
                s.book = d.session->snapshot();
                s.name = d.title().toStdString();
                if (d.session == session_ && current_page() != nullptr) {
                    if (const auto at = shown_page_at(s.book, current_page())) s.page = *at;
                } else {
                    s.page = static_cast<std::size_t>(std::clamp<int>(d.page_index, 0, std::max<int>(static_cast<int>(s.book->pages.size()) - 1, 0)));
                }
                sources.push_back(std::move(s));
            }
        }
        if (sources.size() < automate::kHdrMinImages || sources.size() > automate::kHdrMaxImages) {
            throw core::Error("value", "Merge to HDR Pro takes " + std::to_string(automate::kHdrMinImages) + " to " +
                                           std::to_string(automate::kHdrMaxImages) + " exposures (" + std::to_string(sources.size()) + " given)");
        }
    } catch (const std::exception& error) {
        flash(QStringLiteral("%1: %2").arg(title, wording::error(QString::fromUtf8(error.what()))), 8000, true);
        return;
    }
    const bool align = chosen.align->isChecked();
    const formats::OpenOptions options = file_open_options();
    const QPointer<MainWindow> alive(this);
    // (read on a worker as the command line reads them — planned from the headers before anything is decoded, then one by
    // one within the budget: service::automate::hdr_read, the review's P1-1 —, then made small for the window's preview)
    using Read = std::pair<std::vector<render::hdr::Shot>, std::vector<render::hdr::Shot>>;
    const auto said = [](const std::exception& error) {
        if (const auto* one = dynamic_cast<const automate::HdrReadError*>(&error))
            return QStringLiteral("%1: %2").arg(QString::fromStdString(one->name), wording::error(QString::fromUtf8(one->reason.c_str())));
        return wording::error(QString::fromUtf8(error.what()));
    };
    std::optional<Read> read;
    try {
        read = run_stoppable(this, title, QStringLiteral("画像を読み込んでいます…"), [sources, options](const std::function<bool()>& stopped) {
            Read out;
            out.first = automate::hdr_read(sources, options, stopped);
            for (const render::hdr::Shot& shot : out.first) {
                if (stopped()) throw render::Cancelled();
                out.second.push_back(render::hdr::reduced(shot, 480));
            }
            return out;
        });
    } catch (const std::exception& error) {
        if (alive) flash(QStringLiteral("%1: %2").arg(title, said(error)), 8000, true);
        return;
    }
    if (!alive) return;
    if (!read) {
        flash(QStringLiteral("%1を中止しました").arg(title), 4000);
        return;
    }
    HdrDialog dialog(this, std::move(read->second), align);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    automate::HdrSettings settings;
    try {
        settings = dialog.settings();
    } catch (const std::exception& error) {
        flash(QStringLiteral("%1: %2").arg(title, wording::error(QString::fromUtf8(error.what()))), 8000, true);
        return;
    }
    settings.title = "HDR";
    std::optional<automate::HdrMade> made;
    try {
        auto shots = std::make_shared<std::vector<render::hdr::Shot>>(std::move(read->first));
        made = run_stoppable(this, title, QStringLiteral("HDR Pro に統合しています…"), [shots, settings](const std::function<bool()>& stopped) {
            return automate::merge_to_hdr(std::move(*shots), settings, stopped);
        });
    } catch (const std::exception& error) {
        if (alive) flash(QStringLiteral("%1: %2").arg(title, wording::error(QString::fromUtf8(error.what()))), 8000, true);
        return;
    }
    if (!alive) return;
    if (!made) {
        flash(QStringLiteral("%1を中止しました").arg(title), 4000);
        return;
    }
    const double stops = made->report.value("stops", 0.0);
    add_picture_book(std::move(made->book.doc), made->book.notes);
    flash(QStringLiteral("%1: %2 枚の画像から %3 bit の画像を作りました（明るさの幅 約 %4 段）")
              .arg(title)
              .arg(sources.size())
              .arg(settings.depth)
              .arg(stops, 0, 'f', 1),
          6000);
}

}  // namespace genko::app
