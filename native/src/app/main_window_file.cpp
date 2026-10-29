// PC4 F-0 (SPEC.md FILE-01; 節 F 5.1・5.2, the coordinator's reading of PC3's decision 5): ファイル in Photoshop's order and
// names, and the frames of 節 F's new commands. PC4 F-b connects its own — 開く…・指定形式で開く… (app/open_dialog.hpp)・
// 最近使用したファイルを開く (books and pictures)・すべてを閉じる・その他を閉じる・コピーを保存…・復帰・ファイル情報… and 画像 →
// 複製…, the drop on the window —; PC4 F-c connects 書き出し ▸ (app/main_window_export.cpp).

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPushButton>
#include <QTabBar>
#include <QUrl>
#include <QtConcurrent>

#include <fstream>

#include "app/ask.hpp"
#include "app/color_settings_dialog.hpp"
#include "app/dialogs.hpp"
#include "app/documents.hpp"
#include "app/file_info_dialog.hpp"
#include "app/look.hpp"
#include "app/main_window.hpp"
#include "app/off_ui.hpp"
#include "app/open_dialog.hpp"
#include "app/preferences.hpp"
#include "app/save_status.hpp"
#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "formats/open_file.hpp"
#include "render/codecs/fidelity.hpp"
#include "render/ops_registry.hpp"
#include "service/wording.hpp"
#include "storage/fsutil.hpp"

namespace genko::app {

namespace fs = std::filesystem;
using core::Json;

namespace {

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }
fs::path fpath(const QString& path) { return core::path_from_utf8(path.toStdString()); }

bool is_book_folder(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path / "project.json", ec);
}

// 別名で保存… and コピーを保存…'s kinds of files: a .genko, or the page as one of formats::write_copy's.
QString save_filters() {
    // (PC4 F-a: and every format the codecs write; PC4 F-d: and a PSB, Photoshop's ラージドキュメント形式; PC4 F-f: PhotoCraft's .pcraft)
    return QStringLiteral("Genko の原稿 (*.genko);;Photoshop (*.psd);;TIFF (*.tif *.tiff);;PNG (*.png);;JPEG (*.jpg *.jpeg);;WebP (*.webp);;"
                          "GIF (*.gif);;BMP (*.bmp);;TGA (*.tga);;ICO (*.ico);;Netpbm (*.pnm *.pbm *.pgm *.ppm *.pam *.pfm);;QOI (*.qoi);;"
                          "OpenEXR (*.exr);;Radiance HDR (*.hdr);;AVIF (*.avif);;ラージドキュメント形式 (*.psb);;PhotoCraft (*.pcraft)");
}

// The extension of a path a picture is written to (lower case, no dot), "" for none (a .genko).
QString picture_extension(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    for (const std::string_view ext : formats::copy_extensions()) {
        if (suffix == QString::fromLatin1(ext.data(), static_cast<qsizetype>(ext.size()))) return suffix;
    }
    return suffix == QLatin1String("tiff") ? QStringLiteral("tif") : QString();
}

// The file a picture is opened from: its bytes (core::Error "io" when it cannot be read).
std::string file_bytes(const fs::path& path) { return storage::read_file(path); }

}  // namespace

void MainWindow::build_file_actions() {
    const auto key = [](const char* text) { return QList<QKeySequence>{QKeySequence(QString::fromLatin1(text))}; };
    make(QStringLiteral("act_open_as"), QStringLiteral("指定形式で開く…"), [this] { open_dialog(true); }, key("Ctrl+Alt+Shift+O"),
         QStringLiteral("ファイルを、選んだ形式のものとして開きます"));
    make(QStringLiteral("act_close_all"), QStringLiteral("すべてを閉じる"), [this] { close_all(); }, key("Ctrl+Alt+W"), QStringLiteral("開いている原稿をすべて閉じます"));
    make(QStringLiteral("act_close_others"), QStringLiteral("その他を閉じる"), [this] { close_others(); }, key("Ctrl+Alt+P"),
         QStringLiteral("この原稿のほかの原稿を閉じます"));
    make(QStringLiteral("act_save_copy"), QStringLiteral("コピーを保存…"), [this] { save_copy(); }, key("Ctrl+Alt+S"),
         QStringLiteral("原稿はそのままに、別のファイル（.genko・PSD・TIFF・PNG・JPEG など）に書きます"));
    make(QStringLiteral("act_revert"), QStringLiteral("復帰"), [this] { revert(); }, key("F12"),
         QStringLiteral("最後に保存したとき（無ければ開いたとき）の状態に戻します（取り消せます）"));
    build_export_actions();  // (PC4 F-c: 書き出し ▸ and レイヤー's: app/main_window_export.cpp)
    make(QStringLiteral("act_file_info"), QStringLiteral("ファイル情報…"), [this] { file_info_dialog(); }, key("Ctrl+Alt+Shift+I"),
         QStringLiteral("作成者・説明・キーワード・著作権情報"));
    // (画像 → 複製…, image.duplicate: placed in ページ's menu after the paper's commands)
    make(QStringLiteral("act_duplicate_image"), QStringLiteral("複製…"), [this] { duplicate_dialog(); }, {},
         QStringLiteral("このページを、名前の無い 1 ページの原稿として複製します（統合した画像だけにもできます）"));
    build_automate_actions();  // (PC5 I-0: 自動処理 ▸・スクリプト ▸'s commands, app/main_window_automate.cpp)
    setAcceptDrops(true);
}

QMenu* MainWindow::import_menu(QMenu* file) {
    if (import_menu_ != nullptr) return import_menu_;
    for (QAction* item : file->actions()) {
        if (item->menu() != nullptr && item->menu()->objectName() == QLatin1String("menu_import")) import_menu_ = item->menu();
    }
    if (import_menu_ != nullptr) return import_menu_;
    import_menu_ = new QMenu(QStringLiteral("読み込み"), file);
    import_menu_->setObjectName(QStringLiteral("menu_import"));
    const QList<QAction*> items = file->actions();
    const auto psd = std::find(items.begin(), items.end(), action(QStringLiteral("act_import_psd")));
    if (psd != items.end() && std::next(psd) != items.end()) {
        file->insertMenu(*std::next(psd), import_menu_);
    } else {
        file->addMenu(import_menu_);
    }
    return import_menu_;
}

void MainWindow::add_to_import_menu(QMenu* file, QAction* command) {
    // (Photoshop's order of 読み込み ▸; a command not listed goes last)
    static const QStringList kOrder{QStringLiteral("act_import_data_sets"), QStringLiteral("act_video_frames_to_layers"),
                                    QStringLiteral("act_import_notes")};
    QMenu* menu = import_menu(file);
    const auto rank = [](const QAction* a) {
        const qsizetype at = kOrder.indexOf(a->property("genko_attribute").toString());
        return at < 0 ? kOrder.size() : at;
    };
    for (QAction* item : menu->actions()) {
        if (rank(item) > rank(command)) {
            menu->insertAction(item, command);
            return;
        }
    }
    menu->addAction(command);
}

void MainWindow::build_file_menu(QMenu* file) {
    for (const char* name : {"act_new", "act_open", "act_open_as"}) file->addAction(action(name));
    recent_menu_ = file->addMenu(QStringLiteral("最近使用したファイルを開く"));
    recent_menu_->setProperty("remade", true);
    connect(recent_menu_, &QMenu::aboutToShow, this, [this] {
        recent_menu_->clear();
        // (PC4 F-b: the books and the pictures opened, as many as 環境設定 → ファイル管理 says; one gone is not listed)
        const auto paths = recent_files();
        if (paths.empty()) {
            recent_menu_->addAction(QStringLiteral("（まだありません）"))->setEnabled(false);
            return;
        }
        for (const fs::path& path : paths) {
            const bool book = is_book_folder(path);
            const QString name = QString::fromStdString(core::path_to_utf8(book ? path.stem() : path.filename()));
            QAction* item = recent_menu_->addAction(name, this, [this, path] { open_path(path); });
            item->setToolTip(QDir::toNativeSeparators(qpath(path)));  // (the whole path)
        }
    });
    file->addSeparator();
    file->addAction(action("act_ai"));
    file->addSeparator();
    for (const char* name : {"act_close", "act_close_all", "act_close_others"}) file->addAction(action(name));
    file->addSeparator();
    for (const char* name : {"act_save", "act_save_as", "act_save_copy", "act_revert"}) file->addAction(action(name));
    file->addSeparator();
    // 書き出し ▸ (Photoshop's; 原稿を書き出す… is Genko's own book export, Ctrl+E; カラールックアップテーブル… is put last by
    // レイヤー → 色調補正's group, app/adjust_forms_tone.cpp)
    export_menu_ = file->addMenu(QStringLiteral("書き出し"));
    export_menu_->setObjectName(QStringLiteral("menu_export"));
    for (const char* name : {"act_quick_export_png", "act_export_as"}) export_menu_->addAction(action(name));
    export_menu_->addSeparator();
    export_menu_->addAction(action("act_export_prefs"));
    export_menu_->addSeparator();
    export_menu_->addAction(action("act_save_for_web"));
    export_menu_->addSeparator();
    export_menu_->addAction(action("act_export"));
    export_menu_->addSeparator();
    export_menu_->addAction(action("act_layers_to_files"));  // (PC4 F-c: Photoshop's レイヤーからファイル…)
    export_menu_->addAction(action("act_export_paths_ai"));
    file->addSeparator();
    for (const char* name : {"act_place_embedded", "act_place_linked", "act_package"}) file->addAction(action(name));
    file->addSeparator();
    for (const char* name : {"act_import", "act_import_scan", "act_scanner", "act_merge_book", "act_import_psd"}) file->addAction(action(name));
    file->addSeparator();
    file->addAction(action("act_file_info"));
    file->addSeparator();
    file->addAction(action("act_print"));
    file->addSeparator();
    for (const char* name : {"act_timelapse", "act_timelapse_export"}) file->addAction(action(name));
    file->addSeparator();
    // (PC5 I-0: 自動処理 ▸ above スクリプト ▸, as Photoshop's; their commands: app/main_window_automate.cpp)
    automate_menu_ = file->addMenu(QStringLiteral("自動処理"));
    automate_menu_->setObjectName(QStringLiteral("menu_automate"));
    scripts_menu_ = file->addMenu(QStringLiteral("スクリプト"));
    scripts_menu_->setObjectName(QStringLiteral("menu_scripts"));
    scripts_menu_->addAction(action("act_flatten_masks"));  // (PC3 B2: file.scripts.*)
    build_automate_menus();
    actions_menu_ = file->addMenu(QStringLiteral("オートアクション"));
    actions_menu_->setProperty("remade", true);
    connect(actions_menu_, &QMenu::aboutToShow, this, &MainWindow::fill_actions_menu);
    fill_actions_menu();
    file->addSeparator();
    file->addAction(action("act_quit"));
}

// --- 開く (file.open, file.openAs) -----------------------------------------------------------------------------------

void MainWindow::open_dialog(bool as) {
    OpenDialog dialog(this, as);
    if (const auto recent = recent_projects(); !recent.empty()) dialog.setDirectory(qpath(recent.front().parent_path()));
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const auto format = dialog.format();
    for (const fs::path& path : dialog.chosen()) open_path(path, format);
}

void MainWindow::open_path(const fs::path& path, std::optional<render::codecs::Format> as) {
    std::error_code ec;
    // (PC4 F-f: a .pcraft folder — its name says so, as the open window's — read as PhotoCraft's)
    if (fs::is_directory(path, ec) && qpath(path.filename().empty() ? path.parent_path() : path).endsWith(QStringLiteral(".pcraft"), Qt::CaseInsensitive) &&
        fs::is_regular_file(path / "manifest.json", ec) &&
        !is_book_folder(path)) {
        open_pcraft_folder(path);
        return;
    }
    if (fs::is_directory(path, ec)) {
        if (!is_book_folder(path)) {
            ask::warning(this, QStringLiteral("Genko"), QStringLiteral("Genko の原稿ではありません（.genko のフォルダか、画像のファイルを選びます）"));
            return;
        }
        open_project(path);
        return;
    }
    if (!fs::is_regular_file(path, ec)) {
        forget_recent(path);
        ask::warning(this, QStringLiteral("Genko"), QStringLiteral("ファイルが見つかりません:\n%1").arg(QDir::toNativeSeparators(qpath(path))));
        return;
    }
    // (a picture opened again while it is open here unchanged: its tab comes to the front)
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        const Document& d = documents_[i];
        if (d.source && fs::equivalent(*d.source, path, ec) && !d.session->path() && d.session->generation() == 0) {
            switch_document(static_cast<int>(i));
            return;
        }
    }
    // (PC4 F-e: a camera RAW file through Camera Raw's window — Shift held: as 環境設定 → Camera Raw sets it, no window)
    if (!as && is_raw_path(path)) {
        open_raw_file(path, !(QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier));
        return;
    }
    remember_file(path);
    flash(QStringLiteral("開いています: %1").arg(QDir::toNativeSeparators(qpath(path))), 30000);
    // (decoded on a worker thread: a large picture takes a moment, the window keeps answering)
    using Read = std::pair<std::optional<formats::Opened>, QString>;
    auto* watcher = new QFutureWatcher<Read>(this);
    formats::OpenOptions options = file_open_options();
    options.as = as;
    connect(watcher, &QFutureWatcher<Read>::finished, this, [this, watcher, path] {
        Read read = watcher->result();
        watcher->deleteLater();
        flash_timer_.stop();
        if (!read.first) {
            ask::warning(this, QStringLiteral("Genko"), QStringLiteral("ファイルを開けませんでした:\n%1\n%2").arg(QDir::toNativeSeparators(qpath(path)), wording::error(read.second)));
            refresh_status();
            return;
        }
        if (read.first->profile_missing) ask_missing_profile(*read.first);
        add_picture_book(std::move(read.first->doc), read.first->notes, path);
    });
    watcher->setFuture(QtConcurrent::run([path, options]() -> Read {
        try {
            const std::string bytes = file_bytes(path);
            formats::OpenOptions here = options;
            here.folder = path.parent_path();  // (PC5 I-d: a .pcraft's data sets' pictures beside it)
            return {formats::open_file(bytes, core::path_to_utf8(path.filename()), here), QString()};
        } catch (const std::exception& error) {
            return {std::nullopt, QString::fromUtf8(error.what())};
        }
    }));
}

// PC4 F-f: a .pcraft folder (PhotoCraft's folder bundle) opened as a .pcraft's zip is: a book of one page in memory, read on a
// worker thread.
void MainWindow::open_pcraft_folder(const fs::path& path) {
    remember_file(path);
    flash(QStringLiteral("開いています: %1").arg(QDir::toNativeSeparators(qpath(path))), 30000);
    using Read = std::pair<std::optional<formats::Opened>, QString>;
    auto* watcher = new QFutureWatcher<Read>(this);
    connect(watcher, &QFutureWatcher<Read>::finished, this, [this, watcher, path] {
        Read read = watcher->result();
        watcher->deleteLater();
        flash_timer_.stop();
        if (!read.first) {
            ask::warning(this, QStringLiteral("Genko"), QStringLiteral("ファイルを開けませんでした:\n%1\n%2").arg(QDir::toNativeSeparators(qpath(path)), wording::error(read.second)));
            refresh_status();
            return;
        }
        add_picture_book(std::move(read.first->doc), read.first->notes, path);
    });
    watcher->setFuture(QtConcurrent::run([path]() -> Read {
        try {
            return {formats::open_folder(path), QString()};
        } catch (const std::exception& error) {
            return {std::nullopt, QString::fromUtf8(error.what())};
        }
    }));
}

// カラー設定's "ask when a picture has no profile" (Photoshop's Missing Profiles: Ask When Opening, PC4 F-b): a file that
// carried none opens with the working space's profile; asked, the person may have it opened without one instead — sRGB's
// colours (an RGB page), the default grey's (a grey page) —, as part of opening it (no change to undo). Only when the
// working space is not the default one: otherwise both are the same.
void MainWindow::ask_missing_profile(formats::Opened& opened) {
    namespace cs = color_settings;
    const cs::Settings s = cs::load();
    if (!s.ask_missing || opened.doc.pages.empty() || opened.doc.is_deferred(0)) return;
    const core::Page& page = opened.doc.page(0);
    if (page.rgb && page.rgb->table) return;  // (a colour table's page is sRGB's)
    std::optional<cs::Space> space;
    if (!page.color) space = cs::Space::Rgb;
    else if (page.color->space == core::InkSpace::Gray && !page.color->bitmap && page.color->duotone.empty()) space = cs::Space::Gray;
    if (!space) return;  // (CMYK: only a CMYK profile can be its; Lab: none)
    const cs::Working w = cs::working(*space, s);
    if (w.is_default || !w.usable || !w.icc) return;
    const QString without = *space == cs::Space::Rgb ? QStringLiteral("プロファイルなしで開く（sRGB の色として扱う）")
                                                     : QStringLiteral("プロファイルなしで開く（既定のグレーとして扱う）");
    const QStringList choices{QStringLiteral("作業用の空間（%1）を割り当てる").arg(w.name), without};
    const auto chosen = ask::get_item(this, QStringLiteral("プロファイルのない画像"),
                                      QStringLiteral("「%1」にはプロファイルがありません。どうしますか？（あとから 編集 → プロファイルの指定… で変えられます）")
                                          .arg(QString::fromStdString(opened.doc.title)),
                                      choices, 0);
    if (!chosen || *chosen != without) return;  // (closed without a choice: the working space's, as before)
    try {
        const Json ops = Json::array({Json{{"op", "assign_profile"}, {"page", 1}, {"profile", "none"}, {"default", true}}});
        opened.doc = core::CommandBus(render::ops_registry()).apply(opened.doc, ops, core::Actor("human:genko")).doc;
    } catch (const std::exception& error) {
        opened.notes.push_back(std::string("プロファイルを外せませんでした: ") + error.what());
    }
}

void MainWindow::add_picture_book(core::Document doc, const std::vector<std::string>& notes, std::optional<fs::path> source) {
    Session::Options options;
    options.idle = std::min(options.longest, std::chrono::milliseconds(preferences::save_after_ms()));
    // (PC4 F-e: a camera RAW's colours are its development's — Camera Raw's ワークフロー chose its space —, never the
    // colour settings' policies)
    const bool developed = !doc.pages.empty() && doc.page(0).native_source && doc.page(0).native_source->format == "raw";
    auto session = std::make_shared<Session>(std::move(doc), std::nullopt, options);
    const std::shared_ptr<Session> added = session;
    add_document(std::move(session));
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (documents_[i].session != added) continue;
        documents_[i].picture = true;
        documents_[i].source = source;
        doc_tabs_->setTabText(static_cast<int>(i), documents_[i].title());
    }
    if (session_ == added) {
        refresh_status();
        if (!developed) color_policy_on_open(added);  // (カラー設定's policies: a profile not the working one kept, converted or asked)
    }
    if (!notes.empty()) {
        QStringList said;
        for (const std::string& note : notes) said << QString::fromStdString(note);
        flash(said.join(QStringLiteral("。")), 8000);
    }
    if (source && session_ == added) raise_opened(service::script_events::Event::OpenDocument);  // (PC5 I-e: a file opened)
}

// --- 閉じる (file.closeAll, file.closeOthers) ------------------------------------------------------------------------

bool MainWindow::close_all() {
    // (each asked first as 閉じる asks; the last one closes into an untitled book, the window staying)
    while (documents_.size() > 1) {
        if (!close_document(static_cast<int>(documents_.size()) - 1)) return false;
    }
    const std::shared_ptr<Session> last = documents_.front().session;
    if (!documents::open_elsewhere(last.get(), this) && !settle(0)) return false;
    auto fresh = std::make_shared<Session>(core::new_episode("無題", core::Num(std::int64_t{1}), 8, core::PageSpec::a4_mono()), std::nullopt);
    documents_.front() = Document{fresh};
    doc_ = -1;
    show_document(0);
    canvas_->fit_page();
    flash(QStringLiteral("原稿をすべて閉じました"), 3000);
    return true;
}

bool MainWindow::close_others() {
    // (from the last tab down: closing one never moves the tabs still to be looked at; the kept book's tabs — its
    // contents' too — stay)
    const std::shared_ptr<Session> kept = session_;
    for (int i = static_cast<int>(documents_.size()) - 1; i >= 0; --i) {
        if (documents_[static_cast<std::size_t>(i)].session == kept) continue;
        if (!close_document(i)) return false;
    }
    return true;
}

// --- 保存 (file.saveAs, file.saveACopy, file.revert) ----------------------------------------------------------------

void MainWindow::revert() {
    const std::shared_ptr<Session> s = session_;
    // (what is waiting goes to the book first: the revert is one change on top of it, never under another writer's)
    if (s->path() && s->unsaved()) {
        s->save_now();
        s->wait_saved(std::chrono::milliseconds(30000));
    }
    if (s != session_) return;
    try {
        s->revert();
    } catch (const core::Error& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, error.code() != "nothing_to_revert");
        return;
    }
    flash(QStringLiteral("最後に保存したときの状態に戻しました（取り消せます）"), 4000);
}

std::optional<std::size_t> MainWindow::shown_page_at(const std::shared_ptr<const core::Document>& doc, const core::Page* page) {
    if (doc && page != nullptr) {
        for (std::size_t i = 0; i < doc->pages.size(); ++i) {
            if (!doc->pages[i] || doc->pages[i]->id != page->id) continue;
            if (doc->is_deferred(i)) {
                flash(QStringLiteral("このページを読み込んでいます。読み込みが終わってから、もう一度選んでください。"), 6000, true);
                return std::nullopt;
            }
            return i;
        }
    }
    flash(QStringLiteral("見ているページが見つかりません（原稿は変わりません）"), 6000, true);
    return std::nullopt;
}

bool MainWindow::save_copy_to(const QString& chosen) {
    const QString extension = picture_extension(chosen);
    QString path = chosen;
    if (preferences::lowercase_extension() && !extension.isEmpty()) path = QFileInfo(chosen).path() + QLatin1Char('/') + QFileInfo(chosen).completeBaseName() + QLatin1Char('.') + QFileInfo(chosen).suffix().toLower();
    if (extension.isEmpty() && render::codecs::from_extension(QFileInfo(chosen).suffix().toStdString())) {
        // (a picture's extension this build does not write — since PC4 F-a none of the codecs' formats —: said, not made a book's
        // folder of that name)
        ask::warning(this, QStringLiteral("コピーを保存"),
                     QStringLiteral("この形式（.%1）には、まだ書けません。画像の形式か、Genko の原稿（.genko）を選んでください。").arg(QFileInfo(chosen).suffix()));
        return false;
    }
    if (extension.isEmpty()) {
        // (a .genko: the book as it is now, written to a new folder; this window goes on where it was)
        const QString target = path.endsWith(QStringLiteral(".genko")) ? path : path + QStringLiteral(".genko");
        const std::shared_ptr<Session> origin = session_;
        bool done = false, ok = false;
        QString message;
        QObject guard;
        const auto link = connect(origin.get(), &Session::savedCopy, &guard, [&](bool success, const QString& why) {
            done = true;
            ok = success;
            message = why;
        });
        origin->save_copy(fpath(target));
        QElapsedTimer clock;
        clock.start();
        while (!done && clock.elapsed() < 120000) QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QObject::disconnect(link);
        if (!ok) {
            flash(QStringLiteral("コピーを保存できませんでした: %1").arg(wording::error(message)), 6000, true);
            return false;
        }
        flash(QStringLiteral("コピーを保存しました（原稿はそのままです）: %1").arg(QDir::toNativeSeparators(target)), 4000);
        return true;
    }
    // (the user's decision 4: a picture a book was opened from — in any window here — is never written; its copy goes to
    // another file)
    for (MainWindow* window : documents::windows()) {
        for (const Document& d : window->documents()) {
            std::error_code ec;
            if (d.source && fs::exists(fpath(path), ec) && fs::equivalent(*d.source, fpath(path), ec)) {
                ask::warning(this, QStringLiteral("コピーを保存"),
                             QStringLiteral("開いた元の画像には書きません。別の名前か場所を選んでください:\n%1").arg(QDir::toNativeSeparators(path)));
                return false;
            }
        }
    }
    // (the page shown, by its id: the book's — the session's book, as before —, or in コンテンツを編集's tab its contents'
    // page; a page not read yet is said, never another page in its place)
    const std::shared_ptr<const core::Document> shown = contents_ ? shown_book() : session_->snapshot();
    const std::optional<std::size_t> index = shown_page_at(shown, current_page());
    if (!index) return false;
    formats::CopyOptions options;
    if (extension == QLatin1String("jpg") || extension == QLatin1String("jpeg")) {
        const auto quality = ask::get_int(this, QStringLiteral("JPEG のオプション"), QStringLiteral("画質（0〜12。12 が最高）"), 10, 0, 12);
        if (!quality) return false;
        options.quality = *quality;
    } else if (extension == QLatin1String("psd") || extension == QLatin1String("psb")) {
        options.layers = ask::question(this, QStringLiteral("コピーを保存"), QStringLiteral("レイヤーを残して書きますか？（いいえ: 統合した 1 枚の画像）"));
    }
    try {
        // (the picture and its bytes made on a worker — the window goes on drawing —; what the file will not keep asked
        // about before it is written; then written)
        const fs::path dest = fpath(path);
        const formats::PlannedCopy plan = off_ui([shown, at = *index, dest, options] { return formats::plan_copy(*shown, at, dest, options); });
        if (!plan.notes.empty()) {
            QStringList notes;
            for (const std::string& note : plan.notes) notes << QStringLiteral("・") + QString::fromStdString(note);
            if (!ask::question(this, QStringLiteral("コピーを保存（残せないもの）"),
                               QStringLiteral("このファイルには、次のものを残せません:\n%1\n\nこのまま書きますか？").arg(notes.join(QLatin1Char('\n'))))) {
                return false;
            }
        }
        off_ui([&plan] { return formats::write_planned(plan); });
        QString words = QStringLiteral("コピーを保存しました（原稿はそのままです）: %1").arg(QDir::toNativeSeparators(path));
        flash(words, 6000);
        remember_file(fpath(path));
        script_event(service::script_events::Event::Export);  // (PC5 I-e: スクリプトイベント — a picture's copy is an export)
        return true;
    } catch (const std::exception& error) {
        flash(QStringLiteral("コピーを保存できませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return false;
    }
}

void MainWindow::save_copy() {
    const QString stem = QString::fromStdString(book().title.empty() ? std::string("無題") : book().title);
    const QString path = ask::save_path(this, QStringLiteral("コピーを保存"), stem + QStringLiteral(" のコピー.png"), save_filters());
    if (path.isEmpty()) return;
    save_copy_to(path);
}

// --- ファイル情報 (file.fileInfo), 複製 (image.duplicate) -------------------------------------------------------------

void MainWindow::file_info_dialog() {
    if (!session_->read_only_reason().empty()) {
        flash(QStringLiteral("読み取り専用の原稿です"), 4000, true);
        return;
    }
    FileInfoDialog dialog(this, book(), current_page());
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    if (const auto op = dialog.op()) {
        if (apply_ops(Json::array({*op}))) flash(QStringLiteral("ファイル情報を変えました（取り消せます）"), 3000);
    }
}

void MainWindow::duplicate_dialog() {
    const core::Page* page = current_page();
    if (page == nullptr) return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("duplicateDialog"));
    dialog.setWindowTitle(QStringLiteral("複製"));
    auto* name = new QLineEdit(QString::fromStdString(book().title.empty() ? std::string("無題") : book().title) + QStringLiteral(" のコピー"));
    name->setObjectName(QStringLiteral("duplicate_name"));
    auto* merged = new QCheckBox(QStringLiteral("統合されたレイヤーのみ複製"));
    merged->setObjectName(QStringLiteral("duplicate_merged"));
    QFormLayout* rows = look::form();
    rows->addRow(QStringLiteral("複製の名前"), name);
    rows->addRow(QString(), merged);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("複製する"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    look::frame(&dialog, look::header(QStringLiteral("複製"), QStringLiteral("このページを、名前の無い 1 ページの原稿にします")), rows, nullptr, look::footer(buttons));
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    // (the page shown, by its id: the book's — the session's book, as before —, or in コンテンツを編集's tab its contents'
    // page; a page not read yet is said, never another page in its place)
    const std::shared_ptr<const core::Document> now = contents_ ? shown_book() : session_->snapshot();
    const std::optional<std::size_t> index = shown_page_at(now, page);
    if (!index) return;
    try {
        // (the merged picture made on a worker: the window goes on drawing)
        const std::string title = name->text().trimmed().toStdString();
        const bool merged_only = merged->isChecked();
        const formats::OpenOptions options = file_open_options();
        formats::Opened copy = off_ui([now, at = *index, title, merged_only, options] { return formats::duplicate_page(*now, at, title, merged_only, options); });
        add_picture_book(std::move(copy.doc), copy.notes);
    } catch (const std::exception& error) {
        flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
    }
}

// --- the drop (節 F 5.3) --------------------------------------------------------------------------------------------

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    QMainWindow::dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event) {
    if (!event->mimeData()->hasUrls()) return QMainWindow::dropEvent(event);
    // (on the page: a file is placed embedded, as 埋め込みを配置… places it; elsewhere — the tabs, around the page —: opened)
    const QPoint at = canvas_->mapFrom(this, event->position().toPoint());
    const bool on_page = canvas_->rect().contains(at) && current_page() != nullptr;
    event->acceptProposedAction();
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const fs::path path = fpath(url.toLocalFile());
        std::error_code ec;
        if (on_page && fs::is_regular_file(path, ec) && session_->path()) {
            place_embedded(url.toLocalFile());
        } else {
            open_path(path);
        }
    }
}

}  // namespace genko::app
