// PC5 I-e (SPEC.md AUTO-01) in the window: ファイル → 自動処理 ▸ (バッチ…・ドロップレットを作成… / 条件付きモード変更…・画像を
// 合わせる…・レンズ補正…) and スクリプト ▸ (画像プロセッサー… / すべてのマスクを統合 / スクリプトイベントマネージャー… / 参照…) in
// Photoshop's order; each window answered — バッチ… (a folder through an オートアクション into files; the book in front through
// it, one Undo), ドロップレットを作成… (the droplet and its launchers where the person chose, nowhere else), 画像プロセッサー…
// (a folder of each format), 画像を合わせる… and 条件付きモード変更… (the page in front, one Undo), レンズ補正… (a picture without
// the EXIF's focal length said, nothing written), スクリプトイベントマネージャー… (a script bound: run on the person's save,
// never on the automatic one; changed since it was bound: not run, said), 参照… (a steps file run on the page, a gate refused
// before anything runs).

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSpinBox>
#include <QTemporaryDir>

#include <memory>
#include <string>

#include "app/actions.hpp"
#include "app/automate_dialogs.hpp"
#include "app/main_window.hpp"
#include "app/session.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "formats/open_file.hpp"
#include "gui_support.hpp"
#include "service/automate.hpp"
#include "service/script_events.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace events = service::script_events;

namespace {

QString q(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

void write(const fs::path& file, const std::string& bytes) {
    QFile out(q(file));
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(bytes.data(), static_cast<qint64>(bytes.size()));
}

// A picture of w × h as a PNG file.
void picture(const fs::path& file, int w, int h) {
    formats::NewImage image;
    image.title = "p";
    image.width = w;
    image.height = h;
    image.background = "#336699";
    formats::write_copy(formats::new_image(image).doc, 0, file);
}

std::pair<double, double> px(const core::Page& page) {
    return {std::round(page.spec.width_mm.value() / 25.4 * page.spec.dpi.value()), std::round(page.spec.height_mm.value() / 25.4 * page.spec.dpi.value())};
}

struct Book {
    QTemporaryDir tmp;
    fs::path root;
    fs::path path;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;

    Book() {
        (void)gui_test::config_folder();
        root = gui_test::path_of(tmp.path());
        path = root / "b.genko";
        gui_test::write_book(path, core::new_episode("t", core::Num(1), 2, core::PageSpec::b4_comic()));
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(root / "recovery")));
        window->resize(1280, 800);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        window->go_to_page(1);
    }
    ~Book() {
        std::error_code ec;
        fs::remove(events::path(), ec);
        fs::remove(events::seal_path(), ec);
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    const core::Document& doc() const { return window->book(); }
    // A dialog of this class answered by `fill` (accepted when it returns true).
    template <typename Dialog>
    void answer(std::function<bool(Dialog*)> fill) {
        answers.responder->exec = [fill](QDialog* dialog) {
            auto* d = qobject_cast<Dialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            return fill(d) ? int(QDialog::Accepted) : int(QDialog::Rejected);
        };
    }
    // A picture of w × h opened in a tab of its own (a book of one page in memory).
    void open_picture(int w, int h) {
        formats::NewImage image;
        image.title = "絵";
        image.width = w;
        image.height = h;
        window->add_picture_book(formats::new_image(image).doc, {});
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    }
};

void choose(QComboBox* box, const QString& data) {
    const int at = box->findData(data);
    QVERIFY2(at >= 0, data.toUtf8().constData());
    box->setCurrentIndex(at);
}

QStringList items(QMenu* menu) {
    QStringList out;
    for (QAction* a : menu->actions()) {
        if (a->isSeparator()) out << QStringLiteral("|");
        else if (a->menu() != nullptr) out << a->menu()->title() + QStringLiteral(" ▸");
        else out << a->text();
    }
    return out;
}

}  // namespace

class TestGuiAutomate : public QObject {
    Q_OBJECT
private slots:
    // Their places and names (Photoshop's); no keys.
    void theMenus() {
        Book b;
        QVERIFY(b.window->automate_menu()->menuAction()->isVisible());
        QCOMPARE(items(b.window->automate_menu()), (QStringList{QStringLiteral("バッチ…"), QStringLiteral("ドロップレットを作成…"), QStringLiteral("|"),
                                                             QStringLiteral("条件付きモード変更…"), QStringLiteral("画像を合わせる…"), QStringLiteral("レンズ補正…")}));
        QCOMPARE(items(b.window->scripts_menu()), (QStringList{QStringLiteral("画像プロセッサー…"), QStringLiteral("すべてのマスクを統合"), QStringLiteral("|"),
                                                            QStringLiteral("スクリプトイベントマネージャー…"), QStringLiteral("|"), QStringLiteral("参照…")}));
        for (const char* name : {"act_batch", "act_create_droplet", "act_conditional_mode_change", "act_fit_image", "act_lens_correction_batch",
                                 "act_image_processor", "act_script_events", "act_script_browse"}) {
            QVERIFY2(b.act(name) != nullptr && b.act(name)->shortcuts().isEmpty(), name);
        }
    }

    // バッチ…: a folder through an オートアクション into a folder (each picture's file, the next free names); the book in front
    // through it — every page, one Undo —, or into a folder (the book in the window unchanged).
    void batchInTheWindow() {
        Book b;
        fs::create_directories(b.root / "in");
        picture(b.root / "in" / "a.png", 40, 20);
        picture(b.root / "in" / "b.png", 20, 40);
        QVERIFY(!app::actions::store(QStringLiteral("小さく"), Json::array({Json{{"op", "fit_image"}, {"page", 1}, {"width_px", 10}, {"height_px", 10}}})));
        b.answer<app::BatchDialog>([&b](app::BatchDialog* d) {
            choose(d->steps.box, QStringLiteral("action:小さく"));
            choose(d->source.box, QStringLiteral("folder"));
            d->source.folder->setText(q(b.root / "in"));
            choose(d->destination, QStringLiteral("folder"));
            d->folder->setText(q(b.root / "out"));
            d->suffix->setText(QStringLiteral("_s"));
            d->numbered->setChecked(true);
            return !d->problem();
        });
        b.act("act_batch")->trigger();
        for (const char* name : {"a_s_001.png", "b_s_002.png"}) QVERIFY2(fs::exists(b.root / "out" / name), name);
        const formats::Opened a = formats::open_file(storage::read_file(b.root / "out" / "a_s_001.png"), "a.png");
        QCOMPARE(px(a.doc.page(0)), (std::pair<double, double>{10.0, 5.0}));
        QVERIFY(b.window->status_label()->text().contains(QStringLiteral("2 個のファイルを処理しました")));
        // the book in front, なし: every page, one change
        QVERIFY(!app::actions::store(QStringLiteral("メモ"), Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "batch"}}})));
        const std::size_t done = b.window->session().history().done.size();
        b.answer<app::BatchDialog>([](app::BatchDialog* d) {
            choose(d->steps.box, QStringLiteral("action:メモ"));
            choose(d->source.box, QStringLiteral("open"));
            choose(d->destination, QStringLiteral("none"));
            return !d->problem();
        });
        b.act("act_batch")->trigger();
        QCOMPARE(b.doc().page(0).note, std::string("batch"));
        QCOMPARE(b.doc().page(1).note, std::string("batch"));
        QCOMPARE(b.window->session().history().done.size(), done + 1);
        b.act("act_undo")->trigger();
        QVERIFY(b.doc().page(0).note != "batch");
        // into a folder: written, the window's book as it was
        b.answer<app::BatchDialog>([&b](app::BatchDialog* d) {
            choose(d->steps.box, QStringLiteral("action:メモ"));
            choose(d->source.box, QStringLiteral("open"));
            choose(d->destination, QStringLiteral("folder"));
            d->folder->setText(q(b.root / "pages"));
            return !d->problem();
        });
        b.act("act_batch")->trigger();
        QVERIFY(fs::exists(b.root / "pages" / "b_p001.png"));
        QVERIFY(fs::exists(b.root / "pages" / "b_p002.png"));
        QVERIFY(b.doc().page(0).note != "batch");
        // a folder inside the book: refused, not made
        b.answer<app::BatchDialog>([&b](app::BatchDialog* d) {
            choose(d->steps.box, QStringLiteral("action:メモ"));
            choose(d->source.box, QStringLiteral("folder"));
            d->source.folder->setText(q(b.root / "in"));
            d->folder->setText(q(b.path / "out"));
            return true;
        });
        b.act("act_batch")->trigger();
        QVERIFY(!fs::exists(b.path / "out"));
        QVERIFY(b.window->status_label()->text().contains(QStringLiteral("原稿のフォルダーの中には書き出しません")));
        app::actions::remove(QStringLiteral("小さく"));
        app::actions::remove(QStringLiteral("メモ"));
    }

    // ドロップレットを作成…: the droplet where the person chose it, its launchers beside it (runnable), nothing elsewhere;
    // a step that may not be one refused, nothing written.
    void dropletInTheWindow() {
        Book b;
        fs::create_directories(b.root / "d");
        write(b.root / "fit.txt", "fit_image {\"width_px\": 10, \"height_px\": 10}\n");
        b.answer<app::DropletDialog>([&b](app::DropletDialog* d) {
            d->place->setText(q(b.root / "d" / "小さく.genkodroplet"));
            choose(d->steps.box, QStringLiteral("file"));
            d->steps.file->setText(q(b.root / "fit.txt"));
            return !d->problem();
        });
        b.act("act_create_droplet")->trigger();
        const service::automate::Droplet droplet = service::automate::read_droplet(b.root / "d" / core::path_from_utf8("小さく.genkodroplet"));
        QCOMPARE(droplet.name, std::string("小さく"));
        QCOMPARE(droplet.steps.size(), std::size_t(1));
        std::size_t files = 0;
        for (const auto& entry : fs::directory_iterator(b.root / "d")) {
            (void)entry;
            ++files;
        }
        QCOMPARE(files, 1 + service::automate::launchers_here().size());
#ifndef _WIN32
        const fs::path shell = b.root / "d" / core::path_from_utf8("小さく.sh");
        QVERIFY((fs::status(shell).permissions() & fs::perms::owner_exec) != fs::perms::none);
        QVERIFY(storage::read_file(shell).find(" droplet '") != std::string::npos);
#endif
        write(b.root / "gate.txt", "unlock_page {\"page\": 1}\n");
        b.answer<app::DropletDialog>([&b](app::DropletDialog* d) {
            d->place->setText(q(b.root / "d" / "gate.genkodroplet"));
            choose(d->steps.box, QStringLiteral("file"));
            d->steps.file->setText(q(b.root / "gate.txt"));
            return true;
        });
        b.act("act_create_droplet")->trigger();
        QVERIFY(!fs::exists(b.root / "d" / "gate.genkodroplet"));
        QVERIFY(b.window->status_label()->text().contains(QStringLiteral("手順に入れられません")));
    }

    // 画像プロセッサー…: a folder written in two formats, each into its folder, fitted.
    void imageProcessorInTheWindow() {
        Book b;
        fs::create_directories(b.root / "in");
        picture(b.root / "in" / "a.png", 60, 30);
        b.answer<app::ImageProcessorDialog>([&b](app::ImageProcessorDialog* d) {
            d->source.folder->setText(q(b.root / "in"));
            d->folder->setText(q(b.root / "out"));
            d->jpeg->setChecked(true);
            d->png->setChecked(true);
            d->fit->setChecked(true);
            d->width->setValue(30);
            d->height->setValue(30);
            return !d->problem();
        });
        b.act("act_image_processor")->trigger();
        QVERIFY(fs::exists(b.root / "out" / "JPEG" / "a.jpg"));
        QVERIFY(fs::exists(b.root / "out" / "PNG" / "a.png"));
        const formats::Opened made = formats::open_file(storage::read_file(b.root / "out" / "PNG" / "a.png"), "a.png");
        QCOMPARE(px(made.doc.page(0)), (std::pair<double, double>{30.0, 15.0}));
    }

    // 画像を合わせる… and 条件付きモード変更…: the page in front, one Undo each; a mode not listed left as it is (said).
    void fitAndModeInTheWindow() {
        Book b;
        b.open_picture(80, 40);
        QCOMPARE(px(b.doc().page(0)), (std::pair<double, double>{80.0, 40.0}));
        b.answer<app::FitImageDialog>([](app::FitImageDialog* d) {
            if (d->width->value() != 80) return false;  // (the page's own size first)
            d->width->setValue(20);
            d->height->setValue(20);
            return true;
        });
        b.act("act_fit_image")->trigger();
        QCOMPARE(px(b.doc().page(0)), (std::pair<double, double>{20.0, 10.0}));
        b.act("act_undo")->trigger();
        QCOMPARE(px(b.doc().page(0)), (std::pair<double, double>{80.0, 40.0}));
        b.answer<app::ModeChangeDialog>([](app::ModeChangeDialog* d) {
            for (auto& [mode, box] : d->from) box->setChecked(mode == "cmyk");
            choose(d->to, QStringLiteral("gray"));
            return !d->problem();
        });
        b.act("act_conditional_mode_change")->trigger();
        QCOMPARE(service::automate::page_mode(b.doc().page(0)), std::string("rgb"));
        QVERIFY(b.window->status_label()->text().contains(QStringLiteral("変えませんでした")));
        b.answer<app::ModeChangeDialog>([](app::ModeChangeDialog* d) {
            choose(d->to, QStringLiteral("gray"));
            return !d->problem();
        });
        const std::size_t done = b.window->session().history().done.size();
        b.act("act_conditional_mode_change")->trigger();
        QCOMPARE(service::automate::page_mode(b.doc().page(0)), std::string("gray"));
        QCOMPARE(b.window->session().history().done.size(), done + 1);
        b.act("act_undo")->trigger();
        QCOMPARE(service::automate::page_mode(b.doc().page(0)), std::string("rgb"));
    }

    // レンズ補正…: a picture without the EXIF's focal length — said (the profile auto), nothing written.
    void lensInTheWindow() {
        Book b;
        fs::create_directories(b.root / "in");
        picture(b.root / "in" / "a.png", 40, 20);
        b.answer<app::LensBatchDialog>([&b](app::LensBatchDialog* d) {
            d->source.folder->setText(q(b.root / "in"));
            d->folder->setText(q(b.root / "out"));
            if (d->settings()["profile"] != Json("auto")) return false;
            return !d->problem();
        });
        b.act("act_lens_correction_batch")->trigger();
        QVERIFY(!fs::exists(b.root / "out" / "a.png"));
        QVERIFY(!b.answers.asked.isEmpty());
        QVERIFY2(b.answers.asked.last().contains(QStringLiteral("a.png")), b.answers.asked.last().toUtf8().constData());
    }

    // スクリプトイベントマネージャー…: a script bound to ドキュメントを保存 — run on the person's save (保存), never on the
    // automatic one; the script changed since: not run, said; the table written with its seal by the window only.
    void scriptEventsInTheWindow() {
        Book b;
        write(b.root / "note.txt", "set_note {\"page\": 1, \"note\": \"saved\"}\n");
        b.answer<app::ScriptEventsDialog>([&b](app::ScriptEventsDialog* d) {
            choose(d->event, QStringLiteral("saveDocument"));
            choose(d->kind, QStringLiteral("script"));
            d->script->setText(q(b.root / "note.txt"));
            if (!d->add_chosen()) return false;
            return d->enabled->isChecked();
        });
        b.act("act_script_events")->trigger();
        const events::Loaded loaded = events::load();
        QVERIFY(!loaded.problem);
        QVERIFY(loaded.table.enabled);
        QCOMPARE(loaded.table.bindings.size(), std::size_t(1));
        QCOMPARE(loaded.table.bindings[0].event, std::string("saveDocument"));
        // the automatic save: no event
        b.window->apply_ops(Json::array({Json{{"op", "set_note"}, {"page", 2}, {"note", "x"}}}));
        b.window->session().save_now();
        QVERIFY(b.window->session().wait_saved(std::chrono::milliseconds(30000)));
        QVERIFY(b.doc().page(0).note != "saved");
        // the person's save: the script runs, the journal naming it
        b.act("act_save")->trigger();
        QCOMPARE(b.doc().page(0).note, std::string("saved"));
        b.window->session().save_now();
        QVERIFY(b.window->session().wait_saved(std::chrono::milliseconds(30000)));
        QCOMPARE(storage::journal::stacks(b.path).undo.back().script, std::string("note（ドキュメントを保存）"));
        // changed since it was bound: not run, said
        b.window->apply_ops(Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "before"}}}));
        write(b.root / "note.txt", "set_note {\"page\": 1, \"note\": \"changed\"}\n");
        b.act("act_save")->trigger();
        QCOMPARE(b.doc().page(0).note, std::string("before"));
        QVERIFY(b.window->status_label()->text().contains(QStringLiteral("結び付けた後で変わりました")));
        // the window shows the table, and 削除 empties it
        b.answer<app::ScriptEventsDialog>([](app::ScriptEventsDialog* d) {
            if (d->list->count() != 1) return false;
            d->remove_all();
            return true;
        });
        b.act("act_script_events")->trigger();
        QVERIFY(events::load().table.bindings.empty());
    }

    // 参照…: a steps file run on the page in front (one Undo); one holding a gate refused before anything runs.
    void browseInTheWindow() {
        Book b;
        write(b.root / "two.txt", "# メモ\nset_note {\"note\": \"one\"}\nset_note {\"page\": 2, \"note\": \"two\"}\n");
        b.answers.responder->open_path = [&b](const QString&, const QString&) { return q(b.root / "two.txt"); };
        b.answers.responder->question = [](const QString&, const QString&) { return true; };
        const std::size_t done = b.window->session().history().done.size();
        b.act("act_script_browse")->trigger();
        QCOMPARE(b.doc().page(0).note, std::string("one"));
        QCOMPARE(b.doc().page(1).note, std::string("two"));
        QCOMPARE(b.window->session().history().done.size(), done + 1);
        write(b.root / "gate.txt", "set_note {\"note\": \"no\"}\nname_ok {\"page\": 1}\n");
        b.answers.responder->open_path = [&b](const QString&, const QString&) { return q(b.root / "gate.txt"); };
        b.act("act_script_browse")->trigger();
        QCOMPARE(b.doc().page(0).note, std::string("one"));
        QVERIFY(b.window->status_label()->text().contains(QStringLiteral("手順に入れられません")));
    }
};

QTEST_MAIN(TestGuiAutomate)
#include "test_gui_automate.moc"
