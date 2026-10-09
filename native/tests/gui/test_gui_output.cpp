// The book going out and what it went through (M4③b, Python's genko/app/main.py with dialogs.ExportDialog,
// printing.py, history.py, check_panel.py and checks.py): 書き出し… (Ctrl+E), 印刷… (Ctrl+P), 履歴… (Ctrl+H),
// スマホの画面の範囲を表示, 目盛りを表示 (Ctrl+R) and 入稿前の点検 (F9) — Python's words, keys, tips, menus and checks;
//   - the export dialog's fields as Python lays them out for each format, the pages it writes, its preview, and every
//     format it writes through formats::run: the same files as `genko export` writes for the same choices; the check
//     before writing (write anyway, fix it — the 点検 panel opens —, or stop), the refusals and the result in Japanese;
//   - printing to a PDF: the sheets (a spread on one), each page's picture as render_page draws it at the printer's
//     resolution as Python computes it, fitted or at its real size, centred; the dialog's pages, its words, its preview;
//   - 履歴: the changes listed with Python's words (history.describe, the saved times), a click undoes or redoes to just
//     after a change, the changes saved before this session and other people's (in Python's order while the journal
//     undoes), the list of the book in front;
//   - 点検: the issues listed as Python lists them (the stopping ones first, by page), each shown on its page (its place
//     marked, its line chosen, its layer drawn on), out of date after an edit; the select tool's page has the row;
//   - nothing goes out of a book still being read, nor is it checked;
//   - the phone screens (where they end down a tall page, kept as chosen), the scales (a guide pulled from each,
//     add_ruler as Python's window sends it, none when let go on the scale or with the scales off).

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QToolBar>

#include <functional>

#include "../contract/export_files.hpp"
#include "app/canvas.hpp"
#include "app/check_panel.hpp"
#include "app/config.hpp"
#include "app/export_dialog.hpp"
#include "app/history_panel.hpp"
#include "app/icons.hpp"
#include "app/main_window.hpp"
#include "app/print_dialog.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "app/wording.hpp"
#include "core/pynum.hpp"
#include "formats/checks.hpp"
#include "formats/export.hpp"
#include "gui_support.hpp"
#include "render/page.hpp"
#include "render/png.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

QStringList menus_of(app::MainWindow* window, QAction* action) {
    QStringList found;
    const std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& path) {
        for (QAction* a : menu->actions()) {
            if (a == action) found << path;
            if (a->menu() != nullptr) walk(a->menu(), path + QStringLiteral("/") + a->menu()->title());
        }
    };
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() != nullptr) walk(top->menu(), top->menu()->title());
    }
    return found;
}

// The actions of a menu in order (separators as "|").
QStringList menu_items(app::MainWindow* window, const QString& title) {
    QStringList out;
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() == nullptr || top->menu()->title() != title) continue;
        for (QAction* a : top->menu()->actions()) out << (a->isSeparator() ? QStringLiteral("|") : a->text());
    }
    return out;
}

void mouse(QWidget* w, QEvent::Type type, const QPointF& pos, Qt::MouseButtons buttons) {
    QMouseEvent e(type, pos, w->mapToGlobal(pos), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &e);
}

// A book in a window (the session saves 50 ms after a change), the window's questions answered by the test: message
// boxes by the text of the button to click (none: the box's escape), the dialogs by a function each.
struct Studio {
    QTemporaryDir tmp;
    fs::path path;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::vector<Json> ops;
    QStringList clicks;                                // the buttons to click in the next message boxes, in order
    QStringList boxes;                                 // the message boxes shown (their text)
    std::function<void(app::ExportDialog*)> on_export;
    std::function<void(app::PrintDialog*)> on_print;

    explicit Studio(core::Document doc = gui_test::new_doc(3), bool on_disk = true) {
        (void)gui_test::config_folder();
        if (on_disk) {
            path = gui_test::path_of(tmp.path() + QStringLiteral("/book.genko"));
            gui_test::write_book(path, doc);
            session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery"))));
        } else {
            app::Session::Options options;
            options.actor = "human:tester";
            options.autosave = false;
            options.recovery_root = gui_test::path_of(tmp.path() + QStringLiteral("/recovery"));
            session = std::make_shared<app::Session>(std::move(doc), std::nullopt, options);
        }
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1280, 860);
        window->show();
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit) ops.push_back(c.ops);
        });
        answers.responder->exec = [this](QDialog* dialog) -> int {
            answers.asked << QString::fromLatin1(dialog->metaObject()->className());
            if (auto* box = qobject_cast<QMessageBox*>(dialog)) {
                boxes << box->text();
                const QString wanted = clicks.isEmpty() ? QString() : clicks.takeFirst();
                for (QAbstractButton* b : box->buttons()) {
                    if (b->text() == wanted) b->click();
                }
                return box->result();
            }
            if (auto* e = qobject_cast<app::ExportDialog*>(dialog); e != nullptr && on_export) {
                on_export(e);
                return e->result();
            }
            if (auto* p = qobject_cast<app::PrintDialog*>(dialog); p != nullptr && on_print) {
                on_print(p);
                return p->result();
            }
            return QDialog::Rejected;
        };
    }
    ~Studio() {
        window.reset();
        session.reset();
    }
    const core::Document& doc() const { return window->book(); }
    app::PageCanvas* canvas() const { return window->canvas(); }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    QDockWidget* dock(const QString& title) const {
        for (QDockWidget* d : window->findChildren<QDockWidget*>()) {
            if (d->windowTitle() == title) return d;
        }
        return nullptr;
    }
    std::string ink(int page = 0) const { return gui_test::ink_of(doc().page(static_cast<std::size_t>(page)))->id; }
    bool saved() { return session->wait_saved(std::chrono::milliseconds(10000)); }
};

QString qs(const std::string& s) { return QString::fromStdString(s); }

// The pixels of a picture as RGB rows (a QImage's rows are padded to 4 bytes).
std::string rgb_rows(const QImage& image) {
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    std::string out;
    for (int y = 0; y < rgb.height(); ++y) out.append(reinterpret_cast<const char*>(rgb.constScanLine(y)), static_cast<std::size_t>(rgb.width()) * 3);
    return out;
}

// A page as Python's print_pages draws it: render_page in print mode at dpi, crop_to the area, as RGB.
render::Image printed(const core::Document& doc, const core::Page& page, const std::string& area, int dpi) {
    render::RenderOptions o;
    o.mode = "print";
    o.crop_marks = area == "paper";
    return formats::crop_to(render::render_page(page, dpi, o, &doc).image, page, area, dpi).convert("RGB");
}

}  // namespace

class TestGuiOutput : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        (void)gui_test::config_folder();
        qRegisterMetaType<app::BookChange>();
        app::icons::init_resources();
        app::theme::apply(qApp);
    }

    void init() { app::settings()->remove(QStringLiteral("ui/phone_view")); }

    // The six commands as Python's window has them (_build_actions): words, keys, tips (the tooltip "<title>  <key>\n
    // <tip>"), checkable, the menus and their places; 書き出し in the command bar with its picture; the panels hidden.
    void commandsAsPythonsWindowHasThem() {
        Studio s;
        app::MainWindow* w = s.window.get();
        struct Want {
            const char* name;
            const char* text;
            const char* key;
            const char* tip;
            bool checkable;
            const char* menu;
        };
        const std::vector<Want> wanted = {
            {"act_export", "書き出し…", "Ctrl+E", "PDF・TIFF・PSD・縦読み・SNS 用などに書き出します", false, "ファイル"},
            {"act_print", "印刷…", "Ctrl+P", "プリンターで紙に印刷します（仕上がりで切る・用紙全体）", false, "ファイル"},
            {"act_history", "履歴…", "Ctrl+H", "変更の一覧。クリックでその時点まで戻る・進む", false, "編集"},
            {"act_phone", "スマホの画面の範囲を表示", "", "縦読みの原稿で、スマホ 1 画面に入る範囲と画面の切れ目", true, "表示"},
            {"act_scale", "目盛りを表示", "Ctrl+R", "上と左に mm の目盛り。目盛りからドラッグするとガイド線を引けます", true, "表示"},
            {"act_checks", "入稿前の点検", "F9", "はみ出し・文字の重なりや小ささ・解像度などを探します", false, "ページ"},
        };
        for (const Want& e : wanted) {
            QAction* a = w->action(QString::fromLatin1(e.name));
            QVERIFY2(a != nullptr, e.name);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            QCOMPARE(a->shortcuts(), *e.key == '\0' ? QList<QKeySequence>() : QList<QKeySequence>{QKeySequence(QString::fromLatin1(e.key))});
            QCOMPARE(a->statusTip(), QString::fromUtf8(e.tip));
            QCOMPARE(a->toolTip(), (QString::fromUtf8(e.text) + QStringLiteral("  ") + a->shortcut().toString(QKeySequence::NativeText) +
                                    QStringLiteral("\n") + QString::fromUtf8(e.tip)).trimmed());
            QCOMPARE(a->isCheckable(), e.checkable);
            QVERIFY2(menus_of(w, a) == QStringList{QString::fromUtf8(e.menu)}, e.name);
        }
        // their places in Python's menus (among the commands this build has)
        const QStringList file = menu_items(w, QStringLiteral("ファイル"));
        QCOMPARE(file.indexOf(QStringLiteral("書き出し…")), file.indexOf(QStringLiteral("PSD をレイヤーのまま読み込む…")) + 1);
        QCOMPARE(file.indexOf(QStringLiteral("印刷…")), file.indexOf(QStringLiteral("書き出し…")) + 1);
        QCOMPARE(file.at(file.indexOf(QStringLiteral("印刷…")) + 1), QStringLiteral("|"));
        const QStringList edit = menu_items(w, QStringLiteral("編集"));
        QCOMPARE(edit.mid(0, 4), (QStringList{QStringLiteral("元に戻す"), QStringLiteral("やり直す"), QStringLiteral("履歴…"), QStringLiteral("|")}));
        const QStringList view = menu_items(w, QStringLiteral("表示"));
        const int guides = view.indexOf(QStringLiteral("仕上がり線・基本枠を表示"));
        QCOMPARE(view.mid(guides, 4), (QStringList{QStringLiteral("仕上がり線・基本枠を表示"), QStringLiteral("スマホの画面の範囲を表示"),
                                                   QStringLiteral("目盛りを表示"), QStringLiteral("前のページを透かす（オニオンスキン）")}));
        const QStringList pages = menu_items(w, QStringLiteral("ページ"));
        QCOMPARE(pages.indexOf(QStringLiteral("入稿前の点検")), pages.indexOf(QStringLiteral("台詞の検索・置換…")) + 1);
        QCOMPARE(pages.at(pages.indexOf(QStringLiteral("入稿前の点検")) + 1), QStringLiteral("|"));
        // 書き出し… at the end of the command bar (workspace.DEFAULT_COMMANDBAR), with its picture
        QToolBar* bar = w->findChild<QToolBar*>(QStringLiteral("commands"));
        QVERIFY(bar != nullptr);
        QCOMPARE(bar->actions().back(), s.act("act_export"));
        QVERIFY(bar->actions().at(bar->actions().size() - 2)->isSeparator());
        QVERIFY(!s.act("act_export")->icon().isNull());
        // off at first: the phone screens (an A4 book, nothing chosen), the scales; the two panels hidden (Python's
        // OCCASIONAL_PANELS), closable, in the ウィンドウ menu
        QVERIFY(!s.act("act_phone")->isChecked());
        QVERIFY(!s.canvas()->phone_view);
        QVERIFY(!s.act("act_scale")->isChecked());
        QVERIFY(!s.canvas()->show_scale);
        for (const QString& title : {QStringLiteral("履歴"), QStringLiteral("点検")}) {
            QDockWidget* d = s.dock(title);
            QVERIFY(d != nullptr);
            QVERIFY(d->isHidden());
            QVERIFY(d->features() & QDockWidget::DockWidgetClosable);
            QVERIFY(menus_of(w, d->toggleViewAction()).contains(QStringLiteral("ウィンドウ")));
        }
        // the select tool's page: 表示 (全体を表示, 原寸), 原稿 (ストーリーエディター, 入稿前の点検)
        QWidget* select = w->tool_settings()->page_for(QStringLiteral("select"));
        QVERIFY(select != nullptr);
        QPushButton* row = nullptr;
        for (QPushButton* b : select->findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("入稿前の点検")) row = b;
        }
        QVERIFY(row != nullptr);
        row->click();
        QVERIFY(s.dock(QStringLiteral("点検"))->isVisible());
        QVERIFY(w->checks()->report().has_value());
    }

    // 書き出し…: the dialog as Python lays it out for each format (only the options it takes, the colours a PNG has, the
    // dpi it starts at, the official switch where it exists), the pages it writes, its preview.
    void exportDialogFields() {
        core::Document book = gui_test::new_doc(4);
        book.edit_page(1).spread_with = core::Num(3);
        book.edit_page(2).spread_with = core::Num(2);
        Studio s(book);
        s.window->go_to_page(2);
        bool seen = false;
        s.on_export = [&](app::ExportDialog* d) {
            seen = true;
            QCOMPARE(d->windowTitle(), QStringLiteral("書き出し"));
            QCOMPARE(d->format->currentData().toString(), QStringLiteral("png"));  // (PNG unless another is chosen)
            QStringList keys;
            for (int i = 0; i < d->format->count(); ++i) keys << d->format->itemData(i).toString();
            QCOMPARE(keys, (QStringList{"pdf", "tiff", "png", "cmyk", "layers", "psd", "pack", "webtoon", "sns", "epub", "kindle", "strip"}));
            QCOMPARE(d->format->itemText(d->format->findData(QStringLiteral("webtoon"))), QStringLiteral("縦読み（Webtoon）"));
            QCOMPARE(d->folder->text(), QDir::toNativeSeparators(s.tmp.path() + QStringLiteral("/book_書き出し")));
            QCOMPARE(d->which->itemText(1), QStringLiteral("今のページ（2）"));
            QCOMPARE(d->area->currentData().toString(), QStringLiteral("bleed"));
            QCOMPARE(d->area->itemText(0), QStringLiteral("用紙全体（トンボ付き）"));
            const auto shown = [&](const char* key) { return !d->rows.at(key)->isHidden(); };
            const auto check = [&](const QString& key, std::initializer_list<const char*> options) {
                d->format->setCurrentIndex(d->format->findData(key));
                for (const auto& [option, widget] : d->rows) {
                    const bool want = std::any_of(options.begin(), options.end(), [&](const char* o) { return option == o; });
                    QVERIFY2(shown(option.c_str()) == want, qPrintable(key + QStringLiteral(": ") + QString::fromStdString(option)));
                }
            };
            check(QStringLiteral("pdf"), {"dpi", "area", "color", "icc", "screen"});
            QVERIFY(!d->official->isHidden());
            QVERIFY(!d->colour_head->isHidden());
            QCOMPARE(d->dpi->value(), 600);  // (default_dpi: the book's)
            check(QStringLiteral("webtoon"), {"width", "max_height", "jpeg"});
            QVERIFY(d->colour_head->isHidden());
            QVERIFY(!d->jpeg->isChecked());
            check(QStringLiteral("sns"), {"long_edge", "jpeg", "spreads"});
            QVERIFY(d->jpeg->isChecked());
            QCOMPARE(d->long_edge->value(), 2048);
            check(QStringLiteral("kindle"), {"long_edge"});
            QCOMPARE(d->long_edge->value(), 2560);
            QVERIFY(d->official->isHidden());
            check(QStringLiteral("epub"), {"dpi"});
            QCOMPARE(d->dpi->value(), 150);
            check(QStringLiteral("cmyk"), {"dpi", "area", "icc"});
            d->color->setCurrentIndex(d->color->findData(QStringLiteral("cmyk")));
            check(QStringLiteral("png"), {"dpi", "area", "color", "screen"});
            // (a PNG has no CMYK)
            auto* model = qobject_cast<QStandardItemModel*>(d->color->model());
            QVERIFY(!model->item(d->color->findData(QStringLiteral("cmyk")))->isEnabled());
            QCOMPARE(d->color->currentData().toString(), QStringLiteral("auto"));
            QVERIFY(!d->screen_row->isEnabled());  // (greys become dots only in black and white)
            d->color->setCurrentIndex(d->color->findData(QStringLiteral("bitonal")));
            QVERIFY(d->screen_row->isEnabled());
            d->screen_on->setChecked(true);
            QCOMPARE(d->options().screen.value(), (Json{{"lpi", 60.0}, {"shape", "round"}}));
            // the pages: all, this one, its spread, a range (Python's words when it cannot be read)
            QCOMPARE(d->pages(), (std::vector<std::int64_t>{1, 2, 3, 4}));
            d->which->setCurrentIndex(1);
            QCOMPARE(d->pages(), (std::vector<std::int64_t>{2}));
            d->which->setCurrentIndex(2);
            QCOMPARE(d->pages(), (std::vector<std::int64_t>{2, 3}));
            d->which->setCurrentIndex(3);
            QVERIFY(!d->range->isHidden());
            d->range->setText(QStringLiteral("4, 1〜2"));
            QCOMPARE(d->pages(), (std::vector<std::int64_t>{1, 2, 4}));
            d->range->setText(QStringLiteral("x"));
            emit d->range->editingFinished();
            QCOMPARE(d->preview_note->text(), QStringLiteral("ページの指定が読めません: x"));
            d->run();
            QVERIFY(s.answers.asked.contains(QStringLiteral("Genko: ページの指定が読めません: x")));
            d->range->setText(QStringLiteral("2-3"));
            emit d->range->editingFinished();
            QCOMPARE(d->preview_note->text(), QStringLiteral("2 ページ（全 2 ページを書き出す）"));
            QVERIFY(!d->preview->pixmap().isNull());
            // the official switch takes all the pages
            d->format->setCurrentIndex(d->format->findData(QStringLiteral("pdf")));
            d->official->setChecked(true);
            QVERIFY(!d->which->isEnabled());
            QCOMPARE(d->which->currentIndex(), 0);
            d->format->setCurrentIndex(d->format->findData(QStringLiteral("layers")));
            QVERIFY(!d->official->isChecked());
        };
        s.act("act_export")->trigger();
        QVERIFY(seen);
    }

    // 書き出す for every format: formats::run writes the same files as `genko export` does for the same choices.
    void exportWritesWhatGenkoExportWrites() {
        core::Document book = gui_test::new_doc(3, "出力の試し");
        book.add_line(core::Num(1), "台詞です", "", std::nullopt, "", core::Num(60.0), core::Num(60.0), core::Num(30.0), core::Num(20.0));
        Studio s(book);
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", s.ink()},
                                                      {"points", Json::array({Json::array({30, 40, 0.5}), Json::array({120, 200, 0.8})})}, {"width_mm", 1.5}},
                                                 Json{{"op", "set_nombre"}, {"show", true}}})));
        QVERIFY(s.saved());
        const std::string stem = formats::stem(s.doc());
        struct Case {
            QString key;
            std::function<void(app::ExportDialog*)> set;
            QStringList cli;
            QString file;  // the CLI's output file in the folder (none: the folder)
        };
        const std::vector<Case> cases = {
            {"png", [](app::ExportDialog* d) { d->color->setCurrentIndex(d->color->findData(QStringLiteral("rgb"))); },
             {"--format", "png", "--dpi", "72", "--area", "bleed", "--color", "rgb"}, {}},
            {"pdf", [](app::ExportDialog* d) {
                 d->color->setCurrentIndex(d->color->findData(QStringLiteral("gray")));
                 d->area->setCurrentIndex(d->area->findData(QStringLiteral("trim")));
             },
             {"--format", "pdf", "--dpi", "72", "--area", "trim", "--color", "gray"}, {}},
            {"tiff", [](app::ExportDialog* d) {
                 d->color->setCurrentIndex(d->color->findData(QStringLiteral("rgb")));
                 d->area->setCurrentIndex(d->area->findData(QStringLiteral("paper")));
             },
             {"--format", "tiff", "--dpi", "72", "--area", "paper"}, {}},
            {"cmyk", [](app::ExportDialog*) {}, {"--format", "cmyk", "--dpi", "72", "--area", "bleed"}, {}},
            {"layers", [](app::ExportDialog* d) { d->area->setCurrentIndex(d->area->findData(QStringLiteral("trim"))); },
             {"--format", "layers", "--dpi", "72", "--area", "trim"}, {}},
            {"psd", [](app::ExportDialog*) {}, {"--format", "psd", "--dpi", "72"}, {}},
            {"pack", [](app::ExportDialog*) {}, {"--format", "pack", "--dpi", "72"}, {}},
            {"webtoon", [](app::ExportDialog* d) {
                 d->width->setValue(300);
                 d->max_height->setValue(500);
             },
             {"--format", "webtoon", "--width", "300", "--max-height", "500"}, {}},
            {"sns", [](app::ExportDialog* d) {
                 d->long_edge->setValue(600);
                 d->spreads->setChecked(true);
             },
             {"--format", "sns", "--long-edge", "600", "--jpeg", "--spreads"}, {}},
            {"epub", [](app::ExportDialog*) {}, {"--format", "epub", "--dpi", "72"}, qs(stem + ".epub")},
            {"kindle", [](app::ExportDialog* d) { d->long_edge->setValue(400); }, {"--format", "kindle", "--long-edge", "400"}, qs(stem + "_kindle.epub")},
            {"strip", [](app::ExportDialog*) {}, {"--format", "strip", "--dpi", "72"}, qs(stem + "_strip.png")},
        };
        for (const Case& c : cases) {
            const QString app_out = s.tmp.path() + QStringLiteral("/app-") + c.key;
            const QString cli_out = s.tmp.path() + QStringLiteral("/cli-") + c.key;
            s.on_export = [&](app::ExportDialog* d) {
                d->format->setCurrentIndex(d->format->findData(c.key));
                if (!d->rows.at("dpi")->isHidden()) d->dpi->setValue(72);
                c.set(d);
                d->folder->setText(app_out);
                s.clicks << QStringLiteral("閉じる");
                d->run();
                QCOMPARE(d->result(), static_cast<int>(QDialog::Accepted));
                QVERIFY(d->reply.has_value());
                QCOMPARE(QString::fromStdString(s.boxes.back().toStdString()),
                         QStringLiteral("%1 個のファイルを書き出しました。\n%2").arg((*d->reply)["files"].size()).arg(app_out));
            };
            s.act("act_export")->trigger();
            QProcess cli;
            QStringList args{QStringLiteral("export"), gui_test::qpath(s.path), c.file.isEmpty() ? cli_out : cli_out + QStringLiteral("/") + c.file};
            args << c.cli;
            cli.start(QStringLiteral(GENKO_CLI), args);
            QVERIFY(cli.waitForFinished(300000));
            QVERIFY2(cli.exitCode() == 0, cli.readAllStandardError().constData());
            QVERIFY2(!test::exports::tree(app_out).empty(), qPrintable(c.key));
            const QString difference = test::exports::tree_difference(app_out, cli_out);
            QVERIFY2(difference.isEmpty(), qPrintable(c.key + QStringLiteral(": ") + difference));
        }
        // only the pages chosen
        const QString part = s.tmp.path() + QStringLiteral("/part");
        s.on_export = [&](app::ExportDialog* d) {
            d->dpi->setValue(72);
            d->which->setCurrentIndex(3);
            d->range->setText(QStringLiteral("3, 1"));
            d->folder->setText(part);
            s.clicks << QStringLiteral("閉じる");
            d->run();
        };
        s.act("act_export")->trigger();
        QCOMPARE(QDir(part).entryList(QDir::Files).size(), 2);
        QCOMPARE(s.boxes.back(), QStringLiteral("2 個のファイルを書き出しました。\n%1").arg(part));
    }

    // The check before writing (書き出す runs 入稿前の点検 first): what stops a print, on the pages written, asked about —
    // stop (nothing written), fix it (the dialog closes, 点検 opens with the problem), or write anyway; the official
    // export refused in Japanese.
    void exportAsksAboutWhatStopsAPrint() {
        core::Document book = gui_test::new_doc(3);
        book.add_line(core::Num(2), "はみ出した台詞", "", std::nullopt, "", core::Num(-3.0), core::Num(40.0), core::Num(30.0), core::Num(20.0));
        Studio s(book);
        const QString out = s.tmp.path() + QStringLiteral("/out");
        const QString question = QStringLiteral("書き出す前に点検したところ、止まる問題が 1 件ありました。\n・2 ページ: 台詞「はみ出した台詞」が仕上がり線の外にはみ出している（裁ち落とされる）");
        // stop: nothing written; only page 1: nothing stops it
        s.on_export = [&](app::ExportDialog* d) {
            d->dpi->setValue(72);
            d->folder->setText(out);
            s.clicks << QStringLiteral("やめる");
            d->run();
            QCOMPARE(s.boxes.back(), question);
            QVERIFY(!QFileInfo::exists(out));
            QCOMPARE(d->result(), 0);
            d->which->setCurrentIndex(1);
            const auto asked = s.boxes.size();
            s.clicks << QStringLiteral("閉じる");
            d->run();
            QCOMPARE(s.boxes.size(), asked + 1);
            QCOMPARE(s.boxes.back(), QStringLiteral("1 個のファイルを書き出しました。\n%1").arg(out));
            QCOMPARE(d->result(), static_cast<int>(QDialog::Accepted));
        };
        s.act("act_export")->trigger();
        QCOMPARE(QDir(out).entryList(QDir::Files).size(), 1);
        QDir(out).removeRecursively();
        // fix it: the dialog closes and 点検 opens with the problem
        s.on_export = [&](app::ExportDialog* d) {
            d->dpi->setValue(72);
            d->folder->setText(out);
            s.clicks << QStringLiteral("直す（点検パネルを開く）");
            d->run();
            QVERIFY(d->fix_requested);
            QCOMPARE(d->result(), static_cast<int>(QDialog::Rejected));
        };
        s.act("act_export")->trigger();
        QVERIFY(!QFileInfo::exists(out));
        QVERIFY(s.dock(QStringLiteral("点検"))->isVisible());
        QCOMPARE(s.window->checks()->list->item(0)->text(), QStringLiteral("⛔ 2 ページ ・ 台詞「はみ出した台詞」が仕上がり線の外にはみ出している（裁ち落とされる）"));
        // write anyway: every page
        s.on_export = [&](app::ExportDialog* d) {
            d->dpi->setValue(72);
            d->folder->setText(out);
            s.clicks << QStringLiteral("このまま書き出す") << QStringLiteral("閉じる");
            d->run();
        };
        s.act("act_export")->trigger();
        QCOMPARE(QDir(out).entryList(QDir::Files).size(), 3);
        // the official export: refused by this build (formats::run), in Japanese
        s.on_export = [&](app::ExportDialog* d) {
            d->format->setCurrentIndex(d->format->findData(QStringLiteral("pdf")));
            d->official->setChecked(true);
            d->folder->setText(s.tmp.path() + QStringLiteral("/official"));
            d->run();
        };
        s.answers.asked.clear();
        s.act("act_export")->trigger();
        QVERIFY(s.answers.asked.contains(QStringLiteral("Genko: 書き出せませんでした。\nこの版の Genko では、まだ正式な書き出し（点検と書き出しの承認の記録）はできません。"
                                                        "正式な書き出しを外すと書き出せます")));
        QVERIFY(!QFileInfo::exists(s.tmp.path() + QStringLiteral("/official")));
    }

    // 印刷: the pages to a PDF printer — each sheet's picture is render_page's at the printer's resolution (Python's
    // max(150, min(600, resolution))), cut to the area, a spread's two pages side by side (left first), fitted to the
    // paper and centred or at the page's real size; the PDF has a page for each sheet.
    void printingToAPdf() {
        core::Document book = gui_test::new_doc(3);
        book.edit_page(1).spread_with = core::Num(3);
        book.edit_page(2).spread_with = core::Num(2);
        Studio s(book);
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", s.ink()},
                                                      {"points", Json::array({Json::array({20, 30, 0.5}), Json::array({150, 250, 0.9})})}, {"width_mm", 2}},
                                                 Json{{"op", "add_stroke"}, {"page", 3}, {"layer_id", s.ink(2)},
                                                      {"points", Json::array({Json::array({40, 30, 0.5}), Json::array({100, 50, 0.9})})}, {"width_mm", 3}}})));
        const core::Document& doc = s.doc();
        // the sheets: a spread's pages on one (left first), the others one each
        const auto one_each = app::printing::sheets(doc, {1, 2, 3}, false);
        QCOMPARE(one_each.size(), std::size_t{3});
        const auto spread = app::printing::sheets(doc, {1, 2, 3}, true);
        QCOMPARE(spread.size(), std::size_t{2});
        QCOMPARE(spread[1].size(), std::size_t{2});
        QCOMPARE(spread[1][0]->side(), std::string("left"));
        QCOMPARE(app::printing::sheets(doc, {2, 1}, true).size(), std::size_t{2});  // (its partner not printed: alone)
        const QString pdf = s.tmp.path() + QStringLiteral("/print.pdf");
        QPrinter printer(QPrinter::ScreenResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(pdf);
        const int resolution = printer.resolution();
        const int dpi = app::printing::print_dpi(printer);
        QCOMPARE(dpi, std::max(150, std::min(600, resolution != 0 ? resolution : 300)));
        std::vector<QImage> pictures;
        std::vector<QRectF> targets;
        std::vector<std::pair<int, int>> progress;
        QCOMPARE(app::printing::print_pages(
                     doc, printer, {1, 3}, QStringLiteral("trim"), [&](int done, int of) { progress.emplace_back(done, of); }, QStringLiteral("fit"), false,
                     [&](const QImage& picture, const QRectF& target) {
                         pictures.push_back(picture);
                         targets.push_back(target);
                     }),
                 2);
        QCOMPARE(progress, (std::vector<std::pair<int, int>>{{1, 2}, {2, 2}}));
        for (std::size_t i = 0; i < 2; ++i) {
            const render::Image want = printed(doc, doc.page(i == 0 ? 0 : 2), "trim", dpi);
            QCOMPARE(pictures[i].size(), QSize(want.width(), want.height()));
            QVERIFY(rgb_rows(pictures[i]) == want.tobytes());
        }
        const QByteArray data = [&] {
            QFile f(pdf);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        }();
        QVERIFY(data.startsWith("%PDF"));
        QCOMPARE(data.count("/Type /Page") - data.count("/Type /Pages"), 2);
        // fitted and centred on the paper (the printer's whole paint area: QPainter's viewport on it)
        const QRectF room(0, 0, printer.width(), printer.height());
        for (const QRectF& t : targets) {
            QVERIFY(std::abs(t.center().x() - room.center().x()) < 1e-6 && std::abs(t.center().y() - room.center().y()) < 1e-6);
            QVERIFY(std::abs(t.width() - room.width()) < 1e-6 || std::abs(t.height() - room.height()) < 1e-6);
        }
        // a spread on one sheet, the whole paper with its crop marks, at the page's real size
        const QString pdf2 = s.tmp.path() + QStringLiteral("/spread.pdf");
        QPrinter second(QPrinter::ScreenResolution);
        second.setOutputFormat(QPrinter::PdfFormat);
        second.setOutputFileName(pdf2);
        pictures.clear();
        targets.clear();
        QCOMPARE(app::printing::print_pages(doc, second, {2, 3}, QStringLiteral("paper"), {}, QStringLiteral("actual"), true,
                                            [&](const QImage& picture, const QRectF& target) {
                                                pictures.push_back(picture);
                                                targets.push_back(target);
                                            }),
                 1);
        const render::Image left = printed(doc, *spread[1][0], "paper", dpi);
        const render::Image right = printed(doc, *spread[1][1], "paper", dpi);
        render::Image joined = render::Image::create("RGB", render::Size{left.width() + right.width(), std::max(left.height(), right.height())},
                                                     render::Ink{255, 255, 255});
        joined.paste(left, render::Point{0, 0});
        joined.paste(right, render::Point{left.width(), 0});
        QVERIFY(rgb_rows(pictures[0]) == joined.tobytes());
        const double factor = static_cast<double>(second.resolution() != 0 ? second.resolution() : dpi) / dpi;
        QCOMPARE(targets[0].width(), pictures[0].width() * factor);
        QCOMPARE(targets[0].height(), pictures[0].height() * factor);
    }

    // 印刷…: the dialog's pages (1-N, このページだけ, Python's words for pages it cannot read), printing to the printer
    // given (the status line says how many sheets), none without a printer chosen, its preview.
    void printDialog() {
        core::Document book = core::new_episode("小さな本", core::Num(1), 3, core::PageSpec::custom(60, 60, 54, 54, 2, 5, 5, 4, 4, 600));
        Studio s(book);
        s.window->go_to_page(2);
        const QString pdf = s.tmp.path() + QStringLiteral("/p.pdf");
        bool seen = false;
        s.on_print = [&](app::PrintDialog* d) {
            seen = true;
            QCOMPARE(d->windowTitle(), QStringLiteral("印刷"));
            QCOMPARE(d->pages->text(), QStringLiteral("1-3"));
            QCOMPARE(d->pages->toolTip(), QStringLiteral("例: 1-4, 7（全ページなら 1-3）"));
            QCOMPARE(d->chosen(), (std::vector<std::int64_t>{1, 2, 3}));
            QStringList areas, scales;
            for (int i = 0; i < d->area->count(); ++i) areas << d->area->itemText(i);
            for (int i = 0; i < d->scale->count(); ++i) scales << d->scale->itemText(i);
            QCOMPARE(areas, (QStringList{QStringLiteral("仕上がりで切る"), QStringLiteral("裁ち落としまで"), QStringLiteral("用紙全体（トンボ付き）")}));
            QCOMPARE(scales, (QStringList{QStringLiteral("用紙に合わせる"), QStringLiteral("原寸（100%）")}));
            QCOMPARE(d->spreads->text(), QStringLiteral("見開きは 2 ページを 1 枚に"));
            QCOMPARE(d->print_button->text(), QStringLiteral("プリンターを選んで印刷…"));
            d->this_page->click();
            QCOMPARE(d->pages->text(), QStringLiteral("2"));
            d->pages->setText(QString());
            d->print();
            QVERIFY(s.answers.asked.contains(QStringLiteral("印刷: 印刷するページがありません")));
            d->pages->setText(QStringLiteral("9"));
            d->print();
            QVERIFY(s.answers.asked.contains(QStringLiteral("印刷: 9 ページはありません（1〜3）")));
            // no printer chosen (the system's dialog cancelled): nothing printed
            d->pages->setText(QStringLiteral("1-3"));
            d->print();
            QVERIFY(s.answers.asked.contains(QStringLiteral("QPrintDialog")));
            QCOMPARE(d->result(), 0);
            // its preview
            QPrintPreviewDialog* preview = d->preview();
            QVERIFY(preview != nullptr);
            QCOMPARE(preview->windowTitle(), QStringLiteral("印刷のプレビュー"));
            preview->close();
            QPrinter printer(QPrinter::ScreenResolution);
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(pdf);
            d->pages->setText(QStringLiteral("1, 3"));
            d->print(&printer);
            QCOMPARE(d->result(), static_cast<int>(QDialog::Accepted));
        };
        s.act("act_print")->trigger();
        QVERIFY(seen);
        QCOMPARE(s.window->statusBar()->currentMessage(), QStringLiteral("2 枚を印刷に送りました"));
        QFile f(pdf);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray data = f.readAll();
        QCOMPARE(data.count("/Type /Page") - data.count("/Type /Pages"), 2);
    }

    // 履歴: Python's words for the changes (history.describe), the present marked, a click undoes or redoes to just
    // after a change (the book and the list follow), the saved times; a book's changes saved before this session and
    // another's, from the journal.
    void historyPanel() {
        QCOMPARE(app::history::describe(Json::array()), QStringLiteral("原稿を変えた"));
        QCOMPARE(app::history::describe(Json::array({Json{{"op", "add_stroke"}}, Json{{"op", "add_stroke"}}})), QStringLiteral("ペンで描いた（2 回）"));
        QCOMPARE(app::history::describe(Json::array({Json{{"op", "split_frame"}}, Json{{"op", "erase"}}, Json{{"op", "zzz"}}})),
                 QStringLiteral("コマを割った ほか 2 件"));
        QCOMPARE(app::history::describe(Json::array({Json{{"op", "no_such_op"}}})), QStringLiteral("原稿を変えた"));
        QCOMPARE(app::history::when(std::nullopt), QString());
        const QDateTime now = QDateTime::currentDateTime();
        QCOMPARE(app::history::when(static_cast<double>(now.toSecsSinceEpoch())), QStringLiteral("　") + now.toString(QStringLiteral("HH:mm")));
        const QDateTime before = now.addDays(-3);
        QCOMPARE(app::history::when(static_cast<double>(before.toSecsSinceEpoch())), QStringLiteral("　") + before.toString(QStringLiteral("MM/dd HH:mm")));

        // a book with two changes saved before this session: one by someone else
        QTemporaryDir tmp;
        const fs::path path = gui_test::path_of(tmp.path() + QStringLiteral("/book.genko"));
        gui_test::write_book(path, gui_test::new_doc(2));
        {
            auto options = gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")));
            options.actor = "ai:hermes";
            auto other = app::Session::open(path, options);
            other->apply(Json::array({Json{{"op", "add_page"}, {"count", 1}}}));
            QVERIFY(other->wait_saved(std::chrono::milliseconds(10000)));
        }
        {
            auto mine = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery"))));
            mine->apply(Json::array({Json{{"op", "set_nombre"}, {"show", true}}}));
            QVERIFY(mine->wait_saved(std::chrono::milliseconds(10000)));
        }
        core::Document unused = gui_test::read_book(path);
        QCOMPARE(unused.pages.size(), std::size_t{3});
        Studio s(gui_test::new_doc(1), false);  // (the answers and the config folder; the window used is below)
        auto session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery"))));
        app::MainWindow w(session);
        w.resize(1280, 860);
        w.show();
        QDockWidget* dock = nullptr;
        for (QDockWidget* d : w.findChildren<QDockWidget*>()) {
            if (d->windowTitle() == QStringLiteral("履歴")) dock = d;
        }
        QVERIFY(dock != nullptr && dock->isHidden());
        w.action(QStringLiteral("act_history"))->trigger();
        QVERIFY(dock->isVisible());
        app::HistoryPanel* panel = w.history();
        QListWidget* list = panel->list();
        const auto texts = [&] {
            QStringList out;
            for (int i = 0; i < list->count(); ++i) out << list->item(i)->text();
            return out;
        };
        const auto stamp = [](const QString& text) { return text.section(QStringLiteral("　"), 0, -2); };  // (without the time)
        QStringList shown = texts();
        QCOMPARE(shown.size(), 3);
        QCOMPARE(shown[0], QStringLiteral("（はじめ）"));
        QCOMPARE(stamp(shown[1]), QStringLiteral("ページを足した　〔hermes〕"));
        QVERIFY(shown[1].endsWith(QStringLiteral("　") + QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"))) ||
                shown[1].endsWith(QStringLiteral("　") + QDateTime::currentDateTime().addSecs(-60).toString(QStringLiteral("HH:mm"))));
        QVERIFY(shown[2].startsWith(QStringLiteral("▶ ノンブルを変えた　")));
        QVERIFY(list->item(2)->font().bold());
        QCOMPARE(list->currentRow(), 2);
        QCOMPARE(panel->done(), 2);
        // this session's changes: in memory first, then saved (their time added)
        QVERIFY(w.apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", gui_test::ink_of(w.book().page(0))->id},
                                              {"points", Json::array({Json::array({10, 10, 0.5}), Json::array({50, 60, 0.5})})}}})));
        QVERIFY(w.apply_ops(Json::array({Json{{"op", "split_frame"}, {"page", 1}, {"frame_id", w.book().page(0).leaf_frames().front()->id},
                                              {"axis", "horizontal"}}, Json{{"op", "erase"}, {"page", 1},
                                              {"layer_id", gui_test::ink_of(w.book().page(0))->id}, {"points", Json::array({Json::array({10, 10}), Json::array({20, 20})})},
                                              {"width_mm", 2}}})));
        QVERIFY(gui_test::wait_for([&] { return texts().size() == 5; }));
        QVERIFY(texts()[3] == QStringLiteral("ペンで描いた") || texts()[3].startsWith(QStringLiteral("ペンで描いた　")));  // (saved by now, or not yet)
        QVERIFY(texts()[4].startsWith(QStringLiteral("▶ コマを割った ほか 1 件")));
        QVERIFY(session->wait_saved(std::chrono::milliseconds(10000)));
        QVERIFY(gui_test::wait_for([&] { return texts()[4].startsWith(QStringLiteral("▶ コマを割った ほか 1 件　")); }));
        // back to just after the stroke: the split and the erase undone, shown grey as undone
        list->scrollToItem(list->item(3));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(3)).center());
        QCOMPARE(panel->done(), 3);
        QCOMPARE(w.book().page(0).leaf_frames().size(), std::size_t{1});
        QCOMPARE(gui_test::ink_strokes(w.book(), 0), std::size_t{1});
        shown = texts();
        QVERIFY(shown[3].startsWith(QStringLiteral("▶ ペンで描いた")));
        QVERIFY(shown[4].startsWith(QStringLiteral("コマを割った ほか 1 件（戻した操作）")));
        QCOMPARE(list->item(4)->foreground().color(), QColor(app::theme::tokens().faint));
        // forward again
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(4)).center());
        QCOMPARE(panel->done(), 4);
        QCOMPARE(w.book().page(0).leaf_frames().size(), std::size_t{2});
        // back past this session: the change saved before it, undone through the journal
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(1)).center());
        QVERIFY(gui_test::wait_for([&] { return session->wait_idle(std::chrono::milliseconds(50)) && !w.book().nombre.value("show", false); }));
        QCOMPARE(w.book().pages.size(), std::size_t{3});
        // (the book read again after the journal's undo: the changes undone, next first, as redo takes them)
        QVERIFY2(gui_test::wait_for([&] {
                     return panel->done() == 1 && texts().size() == 5 && texts()[2].startsWith(QStringLiteral("ノンブルを変えた（戻した操作）"));
                 }),
                 qPrintable(texts().join(QStringLiteral(" | "))));
        QVERIFY(texts()[1].startsWith(QStringLiteral("▶ ページを足した　〔hermes〕")));
        QVERIFY(texts()[3].startsWith(QStringLiteral("ペンで描いた（戻した操作）")));
        QVERIFY(texts()[4].startsWith(QStringLiteral("コマを割った ほか 1 件（戻した操作）")));
        // and forward again through the journal: the nombre back
        QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(2)).center());
        QVERIFY(gui_test::wait_for([&] { return session->wait_idle(std::chrono::milliseconds(50)) && w.book().nombre.value("show", false); }));
        QVERIFY(gui_test::wait_for([&] { return panel->done() == 2 && texts()[2].startsWith(QStringLiteral("▶ ノンブルを変えた　")); }));
        // a book not saved anywhere: its changes in memory, no times
        Studio fresh(gui_test::new_doc(2), false);
        fresh.act("act_history")->trigger();
        QVERIFY(fresh.window->apply_ops(Json::array({Json{{"op", "add_page"}, {"count", 1}}})));
        fresh.window->history()->refresh();
        QCOMPARE(fresh.window->history()->list()->item(1)->text(), QStringLiteral("▶ ページを足した"));
        fresh.window->history()->go_to(0);
        QCOMPARE(fresh.doc().pages.size(), std::size_t{2});
        QCOMPARE(fresh.window->history()->list()->item(1)->text(), QStringLiteral("ページを足した（戻した操作）"));
    }

    // 入稿前の点検 (F9) and the 点検 panel: Python's issues, the stopping ones first and by page, with Python's words;
    // the summary and the status line; a click shows the problem on its page; after an edit the list is out of date.
    void checksPanel() {
        core::Document book = gui_test::new_doc(3);
        book.add_line(core::Num(3), "はみ出した台詞", "", std::nullopt, "", core::Num(-3.0), core::Num(40.0), core::Num(30.0), core::Num(20.0));
        core::Layer paint;
        paint.id = "paintlayer01";
        paint.kind = core::LayerKind::Raster;
        paint.title = "塗り";
        paint.raster_png = std::make_shared<const std::string>(render::write_png(render::Image::create("RGBA", render::Size{20, 20}, render::Ink{0, 0, 0, 255})));
        book.edit_page(1).layers.push_back(paint);
        Studio s(book);
        s.window->go_to_page(1);
        s.act("act_checks")->trigger();
        app::CheckPanel* panel = s.window->checks();
        QVERIFY(s.dock(QStringLiteral("点検"))->isVisible());
        // Python's report (formats/checks is held to it by test_contract_checks), listed as Python lists it: the stopping
        // ones first, then by page; "<mark> <page> ページ ・ <message>"
        const Json report = formats::checks::book(s.doc(), s.session->path());
        QCOMPARE(report["errors"].get<int>(), 1);
        QCOMPARE(report["warnings"].get<int>(), 2);
        QStringList rows;
        for (int i = 0; i < panel->list->count(); ++i) rows << panel->list->item(i)->text();
        QCOMPARE(rows, (QStringList{QStringLiteral("⛔ 3 ページ ・ 台詞「はみ出した台詞」が仕上がり線の外にはみ出している（裁ち落とされる）"),
                                    QStringLiteral("⚠ 1 ページ ・ 1 ページに何も描かれていない"),
                                    QStringLiteral("⚠ 2 ページ ・ ペイントのレイヤー「塗り」は 200 dpi で持っている（細い線はペンのレイヤーで描くと 600 dpi でもきれい）")}));
        QCOMPARE(panel->list->item(0)->toolTip(), QStringLiteral("クリックでそのページのその場所を表示"));
        QCOMPARE(panel->summary->text(), QStringLiteral("止まる問題 1 件、確かめた方がよいこと 2 件。"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("止まる問題 1 件・確かめた方がよいこと 2 件（点検パネル）"));
        // a click: the page in front, the place marked, the line chosen, the words in the status line
        QTest::mouseClick(panel->list->viewport(), Qt::LeftButton, {}, panel->list->visualItemRect(panel->list->item(0)).center());
        QCOMPARE(s.window->page_index(), 2);
        QVERIFY(s.canvas()->highlight_box.has_value());
        QCOMPARE(*s.canvas()->highlight_box, (std::array<double, 4>{-3.0, 40.0, 30.0, 20.0}));
        QCOMPARE(s.canvas()->selected_line_id.value(), s.doc().story.front().id);
        QCOMPARE(s.window->last_notice(), QStringLiteral("台詞「はみ出した台詞」が仕上がり線の外にはみ出している（裁ち落とされる）"));
        // a layer's problem: that layer drawn on; the mark gone with another page (the box of a page-wide problem: none)
        QTest::mouseClick(panel->list->viewport(), Qt::LeftButton, {}, panel->list->visualItemRect(panel->list->item(2)).center());
        QCOMPARE(s.window->page_index(), 1);
        QCOMPARE(s.window->target_layer()->id, std::string("paintlayer01"));
        QVERIFY(!s.canvas()->highlight_box.has_value());
        // the list after an edit: out of date until run again
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_page"}, {"count", 1}}})));
        QCOMPARE(panel->summary->text(), QStringLiteral("止まる問題 1 件、確かめた方がよいこと 2 件。\n原稿が変わりました。「点検する」で最新にします。"));
        panel->run_button->click();
        QCOMPARE(panel->summary->text(), QStringLiteral("止まる問題 1 件、確かめた方がよいこと 3 件。"));
        // a book with nothing to fix
        Studio clean(gui_test::new_doc(1));
        QVERIFY(clean.window->apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", clean.ink()},
                                                          {"points", Json::array({Json::array({40, 40, 0.5}), Json::array({60, 70, 0.5})})}}})));
        clean.act("act_checks")->trigger();
        QCOMPARE(clean.window->checks()->summary->text(), QStringLiteral("直すところは見つかりませんでした。書き出せます。"));
        QCOMPARE(clean.window->last_notice(), QStringLiteral("直すところは見つかりませんでした"));
        QCOMPARE(clean.window->checks()->list->count(), 0);
    }

    // スマホの画面の範囲を表示: where each screen ends on a tall page (the page's width filling a phone held upright),
    // drawn over the page; the choice kept (ui/phone_view) and taken by the next window; on at first for a tall strip.
    void phoneScreens() {
        Studio tall(core::new_episode("縦読み", core::Num(1), 1, core::PageSpec::webtoon()));
        QVERIFY(tall.act("act_phone")->isChecked());  // (nothing chosen: on for a tall strip)
        QVERIFY(tall.canvas()->phone_view);
        const core::Rect trim = tall.doc().page(0).trim_rect_mm();
        const double step = trim.width.value() * (844.0 / 390.0);  // (Python's PHONE_ASPECT = 844 / 390, then times the width)
        std::vector<double> want;
        for (double y = trim.y.value() + step; y < trim.y.value() + trim.height.value() - 1e-6; y += step) want.push_back(y);
        QCOMPARE(want.size(), std::size_t{2});  // (Python's test: two breaks on the webtoon paper)
        QCOMPARE(tall.canvas()->phone_screens(), want);
        tall.canvas()->fit_page();
        const QImage on = tall.canvas()->grab().toImage();
        tall.act("act_phone")->trigger();
        QVERIFY(!tall.canvas()->phone_view);
        QCOMPARE(app::settings()->value(QStringLiteral("ui/phone_view")).toString(), QStringLiteral("false"));
        const QImage off = tall.canvas()->grab().toImage();
        QVERIFY(on != off);  // (the screens' breaks were drawn)
        Studio next(core::new_episode("縦読み", core::Num(1), 1, core::PageSpec::webtoon()));
        QVERIFY(!next.act("act_phone")->isChecked());  // (as chosen)
        next.act("act_phone")->trigger();
        QCOMPARE(app::settings()->value(QStringLiteral("ui/phone_view")).toString(), QStringLiteral("true"));
        Studio a4;
        QVERIFY(a4.act("act_phone")->isChecked());  // (as chosen, whatever the paper)
        QVERIFY(a4.canvas()->phone_view);
    }

    // 目盛りを表示: the scales along the top and the left; a guide pulled from the top (across) or the left (down) and
    // let go on the page: add_ruler {"kind": "guide", "axis", "at"} as Python's window sends it, with its words; none
    // when let go on a scale, none with the scales off.
    void scalesAndGuides() {
        Studio s;
        app::PageCanvas* c = s.canvas();
        c->fit_page();
        const QImage before = c->grab().toImage();
        s.act("act_scale")->trigger();
        QVERIFY(c->show_scale);
        const QImage after = c->grab().toImage();
        QVERIFY(before != after);
        QVERIFY(before.copy(0, 0, c->width(), 16) != after.copy(0, 0, c->width(), 16));  // (the strip along the top)
        const auto pull = [&](const QPointF& from, const QPointF& to, const QPointF& release) {
            mouse(c, QEvent::MouseButtonPress, from, Qt::LeftButton);
            mouse(c, QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
            mouse(c, QEvent::MouseMove, to, Qt::LeftButton);
            mouse(c, QEvent::MouseButtonRelease, release, Qt::NoButton);
        };
        const int page_no = 1;
        const QPointF across = c->to_widget(QPointF(50.0, 80.123));
        s.ops.clear();
        pull(QPointF(across.x(), 8), across, across);
        QCOMPARE(s.ops.size(), std::size_t{1});
        Json op = s.ops[0][0];
        QVERIFY(op["id"].is_string() && op["id"].get<std::string>().size() == 12);
        op.erase("id");
        QCOMPARE(qs(op.dump()), qs(Json{{"op", "add_ruler"}, {"page", page_no}, {"kind", "guide"}, {"axis", "h"},
                                        {"at", core::py_round(c->to_mm(across).y(), 2)}}.dump()));
        QCOMPARE(s.window->last_notice(), QStringLiteral("ガイド線を引きました。近くで描き始めた線が沿います（定規 → 選んだ定規を消す で消す）"));
        const QPointF down = c->to_widget(QPointF(30.5, 100.0));
        pull(QPointF(8, down.y()), down, down);
        QCOMPARE(s.ops.size(), std::size_t{2});
        QCOMPARE(qs(s.ops[1][0]["axis"].dump()), QStringLiteral("\"v\""));
        QCOMPARE(s.ops[1][0]["at"].get<double>(), core::py_round(c->to_mm(down).x(), 2));
        const Json& rulers = s.doc().page(0).rulers;
        QCOMPARE(rulers.size(), std::size_t{2});
        QCOMPARE(qs(rulers[0]["kind"].get<std::string>()), QStringLiteral("guide"));
        // let go on the scale, or never moved: nothing
        pull(QPointF(across.x(), 8), across, QPointF(across.x(), 6));
        mouse(c, QEvent::MouseButtonPress, QPointF(across.x(), 8), Qt::LeftButton);
        mouse(c, QEvent::MouseButtonRelease, across, Qt::NoButton);
        QCOMPARE(s.ops.size(), std::size_t{2});
        // the corner where the scales meet starts nothing; scales off: the top of the canvas is the page again
        s.act("act_scale")->trigger();
        QVERIFY(!c->show_scale);
        pull(QPointF(across.x(), 8), across, across);
        QCOMPARE(s.ops.size(), std::size_t{2});
    }

    // While the rest of a book is still being read (its first page shown first, SPEC PERF-01), nothing of it goes out
    // and it is not checked — the pages not read yet would come out blank and be found empty: 書き出し, 印刷 and 入稿前の
    // 点検 say so (as an op that reaches those pages is refused), and so do the 点検する button, printing and an export
    // dialog holding such a book.
    void nothingGoesOutWhileTheBookIsRead() {
        QTemporaryDir tmp;
        const fs::path path = gui_test::path_of(tmp.path() + QStringLiteral("/book.genko"));
        const fs::path recovery = gui_test::path_of(tmp.path() + QStringLiteral("/recovery"));
        gui_test::write_book(path, gui_test::new_doc(3));
        {
            auto drawing = app::Session::open(path, gui_test::quick(recovery));
            for (int page = 1; page <= 3; ++page) {
                drawing->apply(Json::array({Json{{"op", "add_stroke"}, {"page", page}, {"layer", "ink"},
                                                 {"points", Json::array({Json::array({20, 30, 0.5}), Json::array({60, 90, 0.5})})}}}));
            }
            QVERIFY(drawing->wait_saved(std::chrono::milliseconds(10000)));
        }
        app::Session::Options options = gui_test::quick(recovery);
        options.defer_pages = true;
        options.read_rest_now = false;
        auto session = app::Session::from(app::Session::read(path, options), path, options);
        QVERIFY(session->loading());
        Studio s(gui_test::new_doc(1), false);  // (the answers and the config folder; the window used is below)
        app::MainWindow w(session);
        w.resize(1280, 860);
        w.show();
        const QString wait = QStringLiteral("原稿の残りのページを読み込み中です。読み込みが終わってから、もう一度操作してください");
        for (const char* name : {"act_export", "act_print", "act_checks"}) {
            w.flash(QString(), 1, true);
            w.action(QString::fromLatin1(name))->trigger();
            QCOMPARE(w.last_error(), wait);
        }
        QVERIFY2(s.answers.asked.isEmpty(), qPrintable(s.answers.asked.join(QStringLiteral(" | "))));
        w.checks()->run_button->click();
        QCOMPARE(w.checks()->summary->text(), QStringLiteral("点検できませんでした。") + wait);
        QCOMPARE(w.checks()->list->count(), 0);
        QPrinter printer(QPrinter::ScreenResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(tmp.path() + QStringLiteral("/print.pdf"));
        QString printed;
        try {
            app::printing::print_pages(*session->snapshot(), printer, {1, 2, 3});
        } catch (const core::Error& e) {
            printed = QString::fromStdString(e.code());
        }
        QCOMPARE(printed, QStringLiteral("page_not_loaded"));
        app::ExportDialog dialog(&w, session->snapshot(), session->path(), session->actor());
        dialog.folder->setText(tmp.path() + QStringLiteral("/out"));
        dialog.run();
        QCOMPARE(s.answers.asked, QStringList{QStringLiteral("Genko: ") + wait});
        QVERIFY(!QFileInfo::exists(tmp.path() + QStringLiteral("/out")));
        // read whole: checked
        session->read_rest();
        QVERIFY(gui_test::wait_for([&] { return !session->loading(); }));
        w.action(QStringLiteral("act_checks"))->trigger();
        QVERIFY(w.checks()->report().has_value());
    }

    // 履歴 while the journal undoes a change saved before this session (a click back past this session's changes,
    // its undo held up by another writer): what can be redone is listed in the order Redo takes it — the journal's
    // change first, then this session's (as Python's journal gives them back) —, a click forward while that undo is
    // written waits (said; nothing changed), and the book is saved as before (no change it cannot take held back);
    // then forward again in that order.
    void historyWhileTheJournalUndoes() {
        QTemporaryDir tmp;
        const fs::path path = gui_test::path_of(tmp.path() + QStringLiteral("/book.genko"));
        const fs::path recovery = gui_test::path_of(tmp.path() + QStringLiteral("/recovery"));
        gui_test::write_book(path, gui_test::new_doc(2));
        {
            auto options = gui_test::quick(recovery);
            options.actor = "ai:hermes";
            auto other = app::Session::open(path, options);
            other->apply(Json::array({Json{{"op", "add_page"}, {"count", 1}}}));
            QVERIFY(other->wait_saved(std::chrono::milliseconds(10000)));
        }
        {
            auto mine = app::Session::open(path, gui_test::quick(recovery));
            mine->apply(Json::array({Json{{"op", "set_nombre"}, {"show", true}}}));
            QVERIFY(mine->wait_saved(std::chrono::milliseconds(10000)));
        }
        Studio s(gui_test::new_doc(1), false);  // (the answers and the config folder; the window used is below)
        auto session = app::Session::open(path, gui_test::quick(recovery));
        app::MainWindow w(session);
        w.resize(1280, 860);
        w.show();
        w.action(QStringLiteral("act_history"))->trigger();
        app::HistoryPanel* panel = w.history();
        QListWidget* list = panel->list();
        const auto texts = [&] {
            QStringList out;
            for (int i = 0; i < list->count(); ++i) out << list->item(i)->text();
            return out;
        };
        const auto click = [&](int row) {
            list->scrollToItem(list->item(row));
            QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(row)).center());
        };
        QVERIFY(w.apply_ops(Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", gui_test::ink_of(w.book().page(0))->id},
                                              {"points", Json::array({Json::array({10, 10, 0.5}), Json::array({50, 60, 0.5})})}}})));
        QVERIFY(w.apply_ops(Json::array({Json{{"op", "split_frame"}, {"page", 1}, {"frame_id", w.book().page(0).leaf_frames().front()->id},
                                              {"axis", "horizontal"}}})));
        QVERIFY(session->wait_saved(std::chrono::milliseconds(10000)));
        QVERIFY(gui_test::wait_for([&] { return texts().size() == 5 && texts()[4].startsWith(QStringLiteral("▶ コマを割った　")); }));
        auto other = std::make_unique<storage::ProjectLock>(path, "ai:other");
        other->try_acquire();
        click(1);
        QCOMPARE(panel->done(), 1);
        QVERIFY(session->job_running());
        const QStringList waiting = texts();
        QVERIFY2(waiting.size() == 5 && waiting[2].startsWith(QStringLiteral("ノンブルを変えた（戻した操作）")) &&
                     waiting[3].startsWith(QStringLiteral("ペンで描いた（戻した操作）")) && waiting[4].startsWith(QStringLiteral("コマを割った（戻した操作）")),
                 qPrintable(waiting.join(QStringLiteral(" | "))));
        click(3);
        QCOMPARE(panel->done(), 1);
        QCOMPARE(w.last_error(), QStringLiteral("取り消しを原稿に書き込み中です。書き終わってからやり直してください。"));
        other->release();
        QVERIFY(gui_test::wait_for([&] { return session->wait_idle(std::chrono::milliseconds(50)); }));
        QVERIFY2(session->status().kind == app::SaveKind::Saved, qPrintable(session->status().code));
        QVERIFY(!w.book().nombre.value("show", false));
        QCOMPARE(gui_test::ink_strokes(w.book(), 0), std::size_t{0});
        // forward to just after the line: the nombre, then the line; the split stays undone
        QVERIFY(gui_test::wait_for([&] { return texts().size() == 5 && texts()[1].startsWith(QStringLiteral("▶ ページを足した")); }));
        click(3);
        QVERIFY(gui_test::wait_for([&] { return session->wait_idle(std::chrono::milliseconds(50)) && gui_test::ink_strokes(w.book(), 0) == 1; }));
        QVERIFY(w.book().nombre.value("show", false));
        QCOMPARE(w.book().page(0).leaf_frames().size(), std::size_t{1});
        QCOMPARE(session->status().kind, app::SaveKind::Saved);
        QVERIFY2(gui_test::wait_for([&] {
                     return texts().size() == 5 && texts()[3].startsWith(QStringLiteral("▶ ペンで描いた")) &&
                            texts()[4].startsWith(QStringLiteral("コマを割った（戻した操作）"));
                 }),
                 qPrintable(texts().join(QStringLiteral(" | "))));
    }

    // 履歴 follows the book in front: another book's tab brings its list at once (a click on it is about that book),
    // and while the panel is behind another tab it is not read again after each change — only when it comes back.
    void historyFollowsTheBookInFront() {
        Studio s(gui_test::new_doc(2));
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_page"}, {"count", 1}}})));
        QVERIFY(s.saved());
        s.act("act_history")->trigger();
        QListWidget* list = s.window->history()->list();
        QVERIFY(gui_test::wait_for([&] { return list->count() == 2; }));
        const fs::path second = gui_test::path_of(s.tmp.path() + QStringLiteral("/second.genko"));
        gui_test::write_book(second, gui_test::new_doc(1));
        s.window->add_document(app::Session::open(second, gui_test::quick(gui_test::path_of(s.tmp.path() + QStringLiteral("/recovery2")))));
        QCOMPARE(s.window->current_document(), 1);
        QCOMPARE(list->count(), 1);  // (its own: nothing done in it yet)
        s.window->switch_document(0);
        QCOMPARE(s.window->current_document(), 0);
        QCOMPARE(list->count(), 2);
        // behind the page list: left as it is until it is in front again
        s.dock(QStringLiteral("ページ"))->raise();
        QVERIFY(gui_test::wait_for([&] { return s.dock(QStringLiteral("履歴"))->geometry().right() < 0; }));
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_page"}, {"count", 1}}})));
        QTest::qWait(400);
        QCOMPARE(list->count(), 2);
        s.dock(QStringLiteral("履歴"))->raise();
        QVERIFY(gui_test::wait_for([&] { return list->count() == 3; }));
    }

    // 印刷のプレビュー of a book with something this build does not draw yet: said before the preview opens (an empty
    // preview, and its own 印刷 button printing nothing, would say nothing); a sheet that cannot be drawn at the
    // printer's resolution, said after the preview has tried it.
    void printPreviewSaysWhatItCannotDraw() {
        core::Document book = gui_test::new_doc(2);
        core::Layer placed;
        placed.id = "placed000001";
        placed.kind = core::LayerKind::Placed;
        book.edit_page(1).layers.push_back(placed);
        Studio s(book);
        bool seen = false;
        s.on_print = [&](app::PrintDialog* d) {
            seen = true;
            QVERIFY(d->preview() == nullptr);
            QCOMPARE(s.answers.asked.size(), 2);
            QVERIFY2(s.answers.asked[1].startsWith(QStringLiteral("印刷: この版の Genko では、まだ扱えないもの（")), qPrintable(s.answers.asked[1]));
            d->pages->setText(QStringLiteral("1"));  // (the page this build draws: its preview)
            QPrintPreviewDialog* preview = d->preview();
            QVERIFY(preview != nullptr);
            preview->close();
        };
        s.act("act_print")->trigger();
        QVERIFY(seen);
        // a page too large to draw at the printer's resolution (2 m square): the preview opens, then says why it is empty
        Studio huge(core::new_episode("大きな紙", core::Num(1), 1, core::PageSpec::custom(2000, 2000, 1990, 1990, 2, 5, 5, 4, 4, 600)));
        bool opened = false;
        huge.on_print = [&](app::PrintDialog* d) {
            QPrintPreviewDialog* preview = d->preview();
            opened = preview != nullptr;
            if (!opened) return;
            QVERIFY2(gui_test::wait_for([&] { return huge.answers.asked.size() >= 2; }), qPrintable(huge.answers.asked.join(QStringLiteral(" | "))));
            QCOMPARE(huge.answers.asked[1], QStringLiteral("印刷: この解像度では絵が大きすぎて扱えません（解像度を下げてください）"));
            preview->close();
        };
        huge.act("act_print")->trigger();
        QVERIFY(opened);
    }

    // A filter plugin's correction layer (plugins off): printing says it as the plugin's refusal (not as something
    // this build does not draw); one not installed does nothing, as in Python, and the page prints.
    void printingSaysAPluginsOwnRefusal() {
        const QString plugins = gui_test::config_folder().path() + QStringLiteral("/plugins");
        QVERIFY(QDir().mkpath(plugins));
        {
            QFile file(plugins + QStringLiteral("/offplug.py"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("NAME = 'off'\n\ndef run(image, **kw):\n    return image\n");
        }
        core::Document book = gui_test::new_doc(2);
        for (const auto& [page, kind] : {std::pair{0, "plugin:offplug"}, std::pair{1, "plugin:absent"}}) {
            core::Layer adjust;
            adjust.id = "adjust00000" + std::to_string(page);
            adjust.kind = core::LayerKind::Adjust;
            adjust.adjust = Json{{"kind", kind}};
            book.edit_page(static_cast<std::size_t>(page)).layers.push_back(adjust);
        }
        Studio s(book);
        const auto print = [&](std::int64_t page) {
            QPrinter printer(QPrinter::ScreenResolution);
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(s.tmp.path() + QStringLiteral("/p%1.pdf").arg(page));
            try {
                return QString::number(app::printing::print_pages(s.doc(), printer, {page}));
            } catch (const core::Error& e) {
                return app::wording::error(QString::fromUtf8(e.what()));
            }
        };
        QCOMPARE(print(1), QStringLiteral("プラグイン「offplug」は使う設定になっていません（レイヤー → プラグインの設定で選びます）"));
        QCOMPARE(print(2), QStringLiteral("1"));
        QFile::remove(plugins + QStringLiteral("/offplug.py"));
    }

    // An export that could not put back a file it had set aside (the files there before are kept until every new one
    // is in place) says where that file is now — each of them, when it could not put back several.
    void anExportSaysWhereAFileItCouldNotPutBackIs() {
        QCOMPARE(app::wording::error(QStringLiteral("[Errno 13] Permission denied: '/b/x.png'; the file that was there is kept as "
                                                    "'/b/.genko-0123456789abcdef.old'")),
                 QStringLiteral("書き出したファイルを置けず、前からあったファイルも元の場所に戻せませんでした。前のファイルは「/b/.genko-0123456789abcdef.old」にあります"));
        QCOMPARE(app::wording::error(QStringLiteral("[Errno 13] Permission denied: '/b/z.png'; the file that was there is kept as "
                                                    "'/b/.genko-0123456789abcdef.old'; the file that was there is kept as "
                                                    "'/c/.genko-fedcba9876543210.old'")),
                 QStringLiteral("書き出したファイルを置けず、前からあったファイルも元の場所に戻せませんでした。前のファイルは"
                                "「/b/.genko-0123456789abcdef.old」「/c/.genko-fedcba9876543210.old」にあります"));
    }

    // A book without pages: its export dialog opens, with nothing to show.
    void exportDialogOfABookWithoutPages() {
        Studio s;
        core::Document empty = gui_test::new_doc(1);
        empty.pages.clear();
        app::ExportDialog dialog(s.window.get(), std::make_shared<const core::Document>(empty), std::nullopt, "human:tester");
        QVERIFY(dialog.preview->pixmap().isNull());
        dialog.format->setCurrentIndex(dialog.format->findData(QStringLiteral("pdf")));
        QVERIFY(dialog.preview->pixmap().isNull());
    }
};

QTEST_MAIN(TestGuiOutput)
#include "test_gui_output.moc"
