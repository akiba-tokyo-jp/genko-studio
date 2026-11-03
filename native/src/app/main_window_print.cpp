// PC5 I-f (SPEC.md AUTO-01「プリント」「画像アセット」「コンタクトシート」): ファイル → 1 部プリント (Alt+Shift+Ctrl+P), 生成 ▸ 画像アセット
// and 自動処理 ▸ コンタクトシート II… in the window — プリント… itself is app/print_dialog.cpp, the assets formats/image_assets,
// the sheets service::automate::contact_sheet.

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QMenu>
#include <QPrinter>
#include <QPrinterInfo>
#include <QProgressDialog>
#include <QSignalBlocker>

#include <algorithm>
#include <filesystem>

#include "app/ask.hpp"
#include "app/automate_dialogs.hpp"
#include "app/main_window.hpp"
#include "app/open_dialog.hpp"
#include "app/print_dialog.hpp"
#include "app/session.hpp"
#include "core/actor.hpp"
#include "core/paths.hpp"
#include "formats/exporting.hpp"
#include "formats/image_assets.hpp"
#include "service/automate.hpp"
#include "service/script_events.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;
namespace fs = std::filesystem;

namespace {

QString native_path(const fs::path& path) { return QDir::toNativeSeparators(QString::fromStdString(core::path_to_utf8(path))); }

}  // namespace

void MainWindow::build_print_actions() {
    make(QStringLiteral("act_print_one_copy"), QStringLiteral("1 部プリント"), [this] { print_one_copy(); }, {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+P"))},
         QStringLiteral("前回のプリントの設定で、全ページを 1 部プリントします（窓を出さずにプリンターに送ります）"));
    make(QStringLiteral("act_image_assets"), QStringLiteral("画像アセット"), [this] { toggle_image_assets(); }, {},
         QStringLiteral("名前が「logo.png」「200% logo@2x.png」などのレイヤーを、保存するたびに「原稿の名前-assets」フォルダーへ書き出します"), true);
    make(QStringLiteral("act_contact_sheet"), QStringLiteral("コンタクトシート II…"), [this] { contact_sheet_dialog(); }, {},
         QStringLiteral("画像の縮小版を並べたシートを、新しい画像として作ります（ファイル名のキャプション付き）"));
}

void MainWindow::build_print_menus(QMenu* file) {
    const QList<QAction*> items = file->actions();
    // 生成 ▸ 画像アセット after 書き出し ▸ (Photoshop's)
    auto* generate = new QMenu(QStringLiteral("生成"), file);
    generate->setObjectName(QStringLiteral("menu_generate"));
    generate->addAction(action(QStringLiteral("act_image_assets")));
    const auto after = [&items](QAction* of) -> QAction* {
        const qsizetype at = items.indexOf(of);
        return at >= 0 && at + 1 < items.size() ? items[at + 1] : nullptr;
    };
    file->insertMenu(export_menu_ != nullptr ? after(export_menu_->menuAction()) : nullptr, generate);
    connect(generate, &QMenu::aboutToShow, this, [this] {
        QAction* assets = action(QStringLiteral("act_image_assets"));
        const QSignalBlocker quiet(assets);
        assets->setChecked(book().image_assets);
    });
    // 1 部プリント after プリント…
    file->insertAction(after(action(QStringLiteral("act_print"))), action(QStringLiteral("act_print_one_copy")));
    // 自動処理 ▸: コンタクトシート II… in its own part before 条件付きモード変更… (Photoshop's; I-h's 写真の切り抜きと角度補正 joins it)
    if (QAction* mode = action(QStringLiteral("act_conditional_mode_change")); automate_menu_ != nullptr && mode != nullptr) {
        automate_menu_->insertAction(mode, action(QStringLiteral("act_contact_sheet")));
        automate_menu_->insertSeparator(mode);
    }
}

bool MainWindow::print_one_copy(QPrinter* given) {
    if (!book().deferred.empty()) {
        flash(wording::error(QStringLiteral("the book's pages are still being read: try again in a moment")), 6000, true);
        return false;
    }
    session_->save_now();  // (as プリント…)
    formats::print::Setup setup = printing::last_setup();
    setup.copies = 1;
    const auto count = static_cast<std::int64_t>(book().pages.size());
    std::vector<std::int64_t> pages;
    try {
        pages = formats::parse_pages(count > 1 ? "1-" + std::to_string(count) : std::string("1"), count);
    } catch (const std::exception& error) {
        flash(QStringLiteral("1 部プリント: %1").arg(wording::error(QString::fromUtf8(error.what()))), 6000, true);
        return false;
    }
    QPrinter own(QPrinter::HighResolution);
    QPrinter* printer = given != nullptr ? given : &own;
    if (given == nullptr) {
        const std::string& title = book().title;
        printer->setDocName(title.empty() ? QStringLiteral("Genko") : QString::fromStdString(title));
        // (the printer the last print went to, while the computer has it; else its default one)
        if (const QString last = printing::last_printer(); !last.isEmpty() && QPrinterInfo::availablePrinterNames().contains(last)) {
            printer->setPrinterName(last);
        } else if (QPrinterInfo::defaultPrinter().isNull()) {
            flash(QStringLiteral("1 部プリント: プリンターが見つかりません。ファイル → プリント… でプリンターを選んでください"), 8000, true);
            return false;
        }
    }
    printer->setCopyCount(1);
    std::size_t printed = 0;
    try {
        printed = printing::print_sheets(book(), *printer, pages, setup).size();
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("1 部プリント"), wording::error(QString::fromUtf8(error.what())));
        return false;
    }
    flash(QStringLiteral("1 部プリントしました（%1 枚）").arg(printed), 5000);
    script_event(service::script_events::Event::Print);  // (PC5 I-e: スクリプトイベント)
    return true;
}

void MainWindow::toggle_image_assets() {
    QAction* assets = action(QStringLiteral("act_image_assets"));
    const bool on = !book().image_assets;
    const bool changed = apply_ops(Json::array({Json{{"op", "set_image_assets"}, {"on", on}}}));
    {
        const QSignalBlocker quiet(assets);
        assets->setChecked(book().image_assets);
    }
    if (!changed) return;
    if (!on) {
        flash(QStringLiteral("画像アセット: 書き出しをやめました（書き出したファイルはそのままです）"), 5000);
        return;
    }
    const QString where = session_->path() ? QStringLiteral("「%1」").arg(native_path(formats::image_assets::assets_dir(*session_->path())))
                                           : QStringLiteral("原稿の隣の「原稿の名前-assets」");
    const QString none = formats::image_assets::named_layers(book()) == 0
                             ? QStringLiteral("（名前が「logo.png」のようなレイヤーはまだありません）")
                             : QString();
    flash(QStringLiteral("画像アセット: 保存するたびに %1 フォルダーへ書き出します%2").arg(where, none), 8000);
}

void MainWindow::image_assets_after_save() {
    if (!book().image_assets || !session_->path()) return;
    // (the review's P3-5: drawn and written on a worker thread, the book as it was saved — the window keeps answering; a
    // save while they are being written writes them again once they are done)
    if (property("image_assets_running").toBool()) {
        setProperty("image_assets_again", true);
        return;
    }
    setProperty("image_assets_running", true);
    const fs::path dir = formats::image_assets::assets_dir(*session_->path());
    const std::shared_ptr<const core::Document> doc = session_->snapshot();
    const formats::WriteGuard guard = export_guard();
    using Made = std::pair<formats::image_assets::Result, QString>;
    auto* watcher = new QFutureWatcher<Made>(this);
    connect(watcher, &QFutureWatcher<Made>::finished, this, [this, watcher, dir] {
        const Made made = watcher->result();
        watcher->deleteLater();
        setProperty("image_assets_running", false);
        const formats::image_assets::Result& result = made.first;
        if (!made.second.isEmpty()) {
            flash(QStringLiteral("画像アセットを書き出せませんでした: %1").arg(wording::error(made.second)), 8000, true);
        } else if (result.errors.empty()) {
            if (!result.files.empty()) {
                flash(QStringLiteral("保存しました。画像アセット %1 個を書き出しました: %2").arg(result.files.size()).arg(native_path(dir)), 5000);
            }
        } else {
            const Json& first = result.errors.front();
            flash(QStringLiteral("画像アセット: %1 個を書き出し、%2 個は書けませんでした（%3: %4）")
                      .arg(result.files.size())
                      .arg(result.errors.size())
                      .arg(QString::fromStdString(first.value("asset", std::string())),
                           wording::error(QString::fromStdString(first.value("error", std::string())))),
                  10000, true);
        }
        ++image_assets_done_;
        if (property("image_assets_again").toBool()) {
            setProperty("image_assets_again", false);
            image_assets_after_save();
        }
    });
    watcher->setFuture(QtConcurrent::run([doc, dir, guard]() -> Made {
        try {
            return {formats::image_assets::generate(*doc, dir, guard), QString()};
        } catch (const std::exception& error) {
            return {formats::image_assets::Result{}, QString::fromUtf8(error.what())};
        }
    }));
}

void MainWindow::contact_sheet_dialog() {
    namespace automate = service::automate;
    ContactSheetDialog dialog(this);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    const auto inputs = inputs_of(dialog.source);
    if (!inputs) return;
    const QString title = QStringLiteral("コンタクトシート II");
    QProgressDialog progress(QStringLiteral("%1を作っています…").arg(title), QStringLiteral("中止"), 0, static_cast<int>(inputs->items.size()), this);
    progress.setWindowTitle(title);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);
    automate::Options options;
    options.actor = core::Actor(session_->actor());
    options.open = file_open_options();
    options.book_brushes = false;  // (the app follows its brushes itself)
    options.cancelled = [&progress] {
        QCoreApplication::processEvents();
        return progress.wasCanceled();
    };
    options.progress = [&progress](std::size_t done, std::size_t, const fs::path& now) {
        progress.setValue(static_cast<int>(done));
        if (!now.empty()) progress.setLabelText(QDir::toNativeSeparators(QString::fromStdString(core::path_to_utf8(now.filename()))));
    };
    automate::ContactSheetResult result;
    try {
        result = automate::contact_sheet(inputs->items, dialog.sheet(), options);
    } catch (const std::exception& error) {
        progress.reset();
        flash(QStringLiteral("%1: %2").arg(title, wording::error(QString::fromUtf8(error.what()))), 8000, true);
        return;
    }
    progress.reset();
    const std::size_t sheets = result.books.size();
    for (core::Document& sheet : result.books) add_picture_book(std::move(sheet), {});
    QStringList said;
    for (const Json& e : result.errors) {
        said << QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(QString::fromStdString(e.value("input", std::string()))),
                                            wording::error(QString::fromStdString(e.value("error", std::string()))));
    }
    const QString done = QStringLiteral("%1: %2 枚の画像を %3 枚のシートに置きました%4")
                             .arg(title)
                             .arg(result.images)
                             .arg(sheets)
                             .arg(result.cancelled ? QStringLiteral("（中止しました）") : QString());
    if (!said.isEmpty()) {
        const int shown = std::min<int>(static_cast<int>(said.size()), 20);
        QStringList first = said.mid(0, shown);
        if (said.size() > shown) first << QStringLiteral("…ほか %1 件").arg(said.size() - shown);
        ask::warning(this, title, done + QStringLiteral("。置けなかったもの:\n") + first.join(QLatin1Char('\n')));
    }
    flash(done, 6000, !result.errors.empty());
}

}  // namespace genko::app
