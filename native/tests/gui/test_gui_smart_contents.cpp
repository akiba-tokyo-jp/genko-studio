// smart-objects@1, PC3 A1 (SPEC.md COLOR-05「中身とリンク」, the user's decision 1) in the window: コンテンツを編集 opens
// the contents in a tab of the same book — what is done there is the book's at once (the parent page drawn again), undone
// with the book, its steps named 「中身:」 in the history, what the tab cannot use greyed, the tabs of one book switched
// and closed without asking —; a picture's contents made a page first (asked); the linked files: リンクを配置…, marked
// 更新あり and リンク切れ, 変更されたコンテンツを更新 (a place the book recorded — an agent's — asked about before it is read),
// ファイルに再リンク…, ファイルマネージャーで表示, 埋め込みに変換, リンクに変換…; コンテンツを書き出し…, コンテンツを置換…,
// パッケージ…; the tiles of a page whose contents changed drawn again.

#include <QtTest>

#include <QDockWidget>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QTabBar>
#include <QTemporaryDir>

#include <memory>
#include <string>
#include <vector>

#include "app/history_panel.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/studio_widgets.hpp"
#include "app/tiles.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "core/smart.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "service/human_service.hpp"
#include "service/studio_service.hpp"
#include "storage/asset_store.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

struct Book {
    QTemporaryDir tmp;
    std::filesystem::path path;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;

    Book() {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        gui_test::write_book(path, core::new_episode("t", core::Num(1), 2, core::PageSpec::webtoon()));
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 800);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        answers.responder->question = [this](const QString& title, const QString& text) {
            answers.asked << title + QStringLiteral(": ") + text;
            return yes;
        };
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    const core::Document& doc() const { return window->book(); }
    const core::Page& page(std::size_t row = 0) const { return *doc().pages[row]; }
    const core::Layer* layer(const std::string& id, const core::Page* on = nullptr) const {
        for (const core::Layer& l : (on != nullptr ? *on : page()).layers)
            if (l.id == id) return &l;
        return nullptr;
    }
    const core::SmartContents* contents(const std::string& id) const {
        const core::Layer* l = layer(id);
        return l != nullptr ? core::smart::contents_of(doc(), *l) : nullptr;
    }
    QString write(const QString& name, const std::string& bytes) const {
        const QString where = tmp.path() + QStringLiteral("/") + name;
        QFile out(where);
        if (!out.open(QIODevice::WriteOnly)) return {};
        out.write(bytes.data(), static_cast<qint64>(bytes.size()));
        return where;
    }
    // The layer panel's row of a layer (its words).
    QString row(const std::string& id) const {
        app::LayerPanel* panel = window->layer_panel();
        panel->refresh();
        const core::Layer* l = layer(id, window->current_page());
        for (int i = 0; i < panel->list()->count(); ++i) {
            if (l != nullptr && panel->list()->item(i)->text().contains(QString::fromStdString(l->title))) return panel->list()->item(i)->text();
        }
        return {};
    }
    std::string smart_id() const {
        for (const core::Layer& l : page().layers)
            if (l.kind == core::LayerKind::Smart) return l.id;
        return {};
    }
    bool yes = true;
};

std::string one_colour(int w, int h, render::Ink ink) { return render::write_png(render::Image::create("RGB", render::Size{w, h}, ink)); }

std::string two_colours() {
    render::Image picture = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{128, 128, 128, 255});
    picture.paste(render::Ink{30, 160, 90, 255}, render::Box{30, 0, 60, 40});
    return render::write_png(picture);
}

Json square(double size) {
    return Json{{"poly", Json::array({Json::array({0, 0}), Json::array({size, 0}), Json::array({size, size}), Json::array({0, size})})}};
}

render::Image drawn(const core::Document& doc, std::size_t page = 0) {
    render::RenderOptions options;
    options.mode = "proof";
    return render::render_page(doc.page(page), 30, options, &doc).image.convert("RGB");
}

QImage as_qimage(const render::Image& image) {
    const std::string bytes = image.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), image.width(), image.height(), image.width() * 3, QImage::Format_RGB888)
        .convertToFormat(QImage::Format_RGB32);
}

}  // namespace

class TestGuiSmartContents : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { (void)gui_test::config_folder(); }

    void editContentsInATabOfTheBook() {
        Book book;
        app::MainWindow* w = book.window.get();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"name", "絵"}},
                                          Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                          Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(two_colours())}}})));
        w->set_target_layer("pic");
        book.act("act_smart_convert")->trigger();
        const std::string so = w->target_layer()->id;
        const std::string id = *core::smart::contents_id(*book.layer(so));
        const render::Image before = drawn(book.doc());
        QVERIFY(book.act("act_smart_edit")->isEnabled());
        QVERIFY(book.act("act_add_page")->isEnabled());

        // コンテンツを編集: a second tab of the same book, showing the contents' page
        book.act("act_smart_edit")->trigger();
        QCOMPARE(w->documents().size(), std::size_t(2));
        QCOMPARE(w->contents_tab(), std::optional<std::string>(id));
        QCOMPARE(w->documents()[1].session, w->documents()[0].session);
        QCOMPARE(w->doc_tabs()->tabText(1), QStringLiteral("絵の中身"));
        QVERIFY(w->current_page() != nullptr && book.layer("pic", w->current_page()) != nullptr);
        // (what the tab cannot use: greyed; what it can, not)
        for (const char* off : {"act_add_page", "act_del_page", "act_split_h", "act_text", "act_line_type", "act_template", "act_ai", "act_package"})
            QVERIFY2(!book.act(off)->isEnabled(), off);
        for (const char* on : {"act_undo", "act_smart_convert", "act_layer_paint", "act_import"}) QVERIFY2(book.act(on)->isEnabled(), on);

        // painted there (as the canvas sends it): the book's contents and the page showing them change at once
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "fill_area"}, {"page", 1}, {"layer_id", "pic"}, {"area", square(60)}, {"rgb", Json::array({0, 0, 255})}}})));
        QVERIFY(book.layer(so) != nullptr);
        const render::Image after = drawn(book.doc());
        QVERIFY(after.tobytes() != before.tobytes());
        // (its step named in the history as the contents')
        QCOMPARE(app::history::describe(Json::array({Json{{"op", "fill_area"}, {"contents", id}, {"page", 1}}})), QStringLiteral("中身: 囲って塗った"));
        QCOMPARE(app::history::describe(Json::array({Json{{"op", "fill_area"}, {"page", 1}}})), QStringLiteral("囲って塗った"));
        // an op of the book's (a line) refused there
        QVERIFY(!w->apply_ops(Json::array({Json{{"op", "add_line"}, {"page", 1}, {"text", "台詞"}}})));
        QVERIFY2(w->status_label()->text().contains(QStringLiteral("スマートオブジェクトの中では使えません")), qPrintable(w->status_label()->text()));

        // the book's tab: the page as it is now on the canvas (its tiles drawn again), no question asked on the way
        book.answers.asked.clear();
        w->switch_document(0);
        QVERIFY(!w->contents_tab());
        QVERIFY(book.answers.asked.isEmpty());
        QVERIFY(book.act("act_add_page")->isEnabled());
        QVERIFY(w->canvas()->wait_rendered(60000));
        render::RenderOptions options;
        options.mode = "proof";
        options.skip_unported = true;
        const int dpi = w->canvas()->base_dpi();
        QCOMPARE(w->canvas()->renderer().compose(dpi), as_qimage(render::render_page(book.page(), dpi, options, &book.doc()).image.convert("RGB")));

        // undone (with the book): as before; the tab again, closed without asking, the book open
        book.act("act_undo")->trigger();
        QCOMPARE(drawn(book.doc()).tobytes(), before.tobytes());
        w->switch_document(1);
        QCOMPARE(w->contents_tab(), std::optional<std::string>(id));
        QVERIFY(w->close_document(1));
        QVERIFY(book.answers.asked.isEmpty());
        QCOMPARE(w->documents().size(), std::size_t(1));
        QVERIFY(!w->contents_tab());
        // レイヤーの内容オプション… on it: the tab again (one, not two)
        w->set_target_layer(so);
        book.act("act_layer_content_options")->trigger();
        book.act("act_smart_edit")->trigger();
        QCOMPARE(w->documents().size(), std::size_t(2));
        // the contents gone (the conversion undone in the book's tab): said in their tab
        w->switch_document(0);
        book.act("act_undo")->trigger();
        QVERIFY(book.layer(so) == nullptr);
        w->switch_document(1);
        QVERIFY(w->current_page() == nullptr);
        QVERIFY2(w->status_label()->text().contains(QStringLiteral("中身は、原稿にもうありません")), qPrintable(w->status_label()->text()));
    }

    void aPicturesContentsMadeAPageFirst() {
        Book book;
        app::MainWindow* w = book.window.get();
        QVERIFY(w->place_embedded(book.write(QStringLiteral("leaf.png"), one_colour(40, 30, render::Ink{200, 40, 90}))));
        const std::string so = book.smart_id();
        w->set_target_layer(so);
        // (asked: no — nothing changes)
        book.yes = false;
        QVERIFY(!w->edit_contents());
        QCOMPARE(book.contents(so)->kind, std::string("file"));
        QCOMPARE(w->documents().size(), std::size_t(1));
        book.yes = true;
        book.answers.asked.clear();
        QVERIFY(w->edit_contents());
        QVERIFY2(book.answers.asked.join(QString()).contains(QStringLiteral("編集できるページ")), qPrintable(book.answers.asked.join(QStringLiteral("|"))));
        QCOMPARE(book.contents(so)->kind, std::string("page"));
        QVERIFY(w->contents_tab().has_value());
        QCOMPARE(w->current_page()->layers.size(), std::size_t(1));
        QCOMPARE(w->current_page()->layers[0].title, std::string("leaf"));
    }

    void linkedFiles() {
        Book book;
        app::MainWindow* w = book.window.get();
        const std::string first = one_colour(40, 30, render::Ink{200, 40, 90});
        const QString file = book.write(QStringLiteral("photo.png"), first);
        QString filter;
        book.answers.responder->open_path = [&filter, file](const QString&, const QString& f) {
            filter = f;
            return file;
        };
        // リンクを配置…: its copy in the book, its place recorded; the row says リンク
        book.act("act_place_linked")->trigger();
        const std::string so = book.smart_id();
        QVERIFY(!so.empty());
        const core::SmartContents* c = book.contents(so);
        QVERIFY(c != nullptr && c->link.has_value());
        QCOMPARE(QString::fromStdString((*c->link)["path"].get<std::string>()), QFileInfo(file).absoluteFilePath());
        QCOMPARE((*c->link)["relative"], Json("../photo.png"));
        QCOMPARE(*storage::AssetStore(book.path).get_bytes(c->asset, ".png"), first);
        w->set_target_layer(so);
        QVERIFY2(book.row(so).contains(QStringLiteral("リンク")) && !book.row(so).contains(QStringLiteral("更新あり")), qPrintable(book.row(so)));
        QCOMPARE(w->link_status(*core::smart::contents_id(*book.layer(so))), std::string("ok"));
        // (not changed: nothing to update)
        QVERIFY(!w->update_contents(false));
        // the file changed: 更新あり, then taken in without asking (the person chose it)
        const std::string second = one_colour(40, 30, render::Ink{20, 140, 220});
        book.write(QStringLiteral("photo.png"), second);
        w->check_links();
        QVERIFY2(book.row(so).contains(QStringLiteral("更新あり")), qPrintable(book.row(so)));
        book.answers.asked.clear();
        book.act("act_smart_update")->trigger();
        QVERIFY(book.answers.asked.isEmpty());
        QCOMPARE(book.contents(so)->asset, core::bytes_ref(second));
        QVERIFY(!book.row(so).contains(QStringLiteral("更新あり")));
        // ファイルマネージャーで表示: its folder
        book.answers.asked.clear();
        book.act("act_smart_reveal")->trigger();
        QCOMPARE(book.answers.asked, QStringList{QStringLiteral("open_url: ") + QUrl::fromLocalFile(QFileInfo(file).absolutePath()).toString()});
        // コンテンツを編集 of a linked file: opened in its own app
        book.answers.asked.clear();
        book.act("act_smart_edit")->trigger();
        QCOMPARE(book.answers.asked, QStringList{QStringLiteral("open_url: ") + QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath()).toString()});
        QCOMPARE(w->documents().size(), std::size_t(1));
        // the file gone: リンク切れ (the page drawn from the copy)
        QVERIFY(QFile::remove(file));
        w->check_links();
        QVERIFY2(book.row(so).contains(QStringLiteral("リンク切れ")), qPrintable(book.row(so)));
        QVERIFY(!w->update_contents(false));

        // a place an agent recorded (an op gave it): shown and asked about before it is read
        const std::string third = one_colour(40, 30, render::Ink{90, 90, 10});
        const QString theirs = book.write(QStringLiteral("theirs.png"), third);
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "smart_relink"}, {"page", 1}, {"id", so}, {"data", core::b64encode(third)},
                                               {"link", Json{{"path", QFileInfo(theirs).absoluteFilePath().toStdString()}}}}})));
        book.write(QStringLiteral("theirs.png"), first);
        w->check_links();
        book.yes = false;
        book.answers.asked.clear();
        QVERIFY(!w->update_contents(true));
        QVERIFY2(book.answers.asked.join(QString()).contains(QFileInfo(theirs).absoluteFilePath()), qPrintable(book.answers.asked.join(QStringLiteral("|"))));
        QCOMPARE(book.contents(so)->asset, core::bytes_ref(third));
        book.yes = true;
        QVERIFY(w->update_contents(true));  // (変更されたすべてのコンテンツを更新)
        QCOMPARE(book.contents(so)->asset, core::bytes_ref(first));

        // ファイルに再リンク… (the file chosen here), 埋め込みに変換, リンクに変換…
        const QString again = book.write(QStringLiteral("again.png"), second);
        QVERIFY(w->relink_contents(again));
        QCOMPARE(QString::fromStdString((*book.contents(so)->link)["path"].get<std::string>()), QFileInfo(again).absoluteFilePath());
        QCOMPARE(book.contents(so)->asset, core::bytes_ref(second));
        book.act("act_smart_embed")->trigger();
        QVERIFY(!book.contents(so)->link);
        QVERIFY2(book.row(so).contains(QStringLiteral("埋め込み")), qPrintable(book.row(so)));
        const QString out = book.tmp.path() + QStringLiteral("/written.png");
        QVERIFY(w->link_contents(out));
        QCOMPARE(QString::fromStdString((*book.contents(so)->link)["path"].get<std::string>()), QFileInfo(out).absoluteFilePath());
        QFile written(out);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(written.readAll().toStdString(), second);
        w->check_links();
        QCOMPARE(w->link_status(*core::smart::contents_id(*book.layer(so))), std::string("ok"));
    }

    void writtenOutReplacedAndPacked() {
        Book book;
        app::MainWindow* w = book.window.get();
        const std::string first = one_colour(40, 30, render::Ink{200, 40, 90});
        QVERIFY(w->place_embedded(book.write(QStringLiteral("leaf.png"), first)));
        const std::string so = book.smart_id();
        w->set_target_layer(so);
        // コンテンツを書き出し…: the file as it is
        const QString out = book.tmp.path() + QStringLiteral("/out.png");
        book.answers.responder->save_path = [out](const QString&, const QString&) { return out; };
        book.act("act_smart_export")->trigger();
        QFile file(out);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll().toStdString(), first);
        // コンテンツを置換…
        const std::string other = one_colour(20, 20, render::Ink{1, 2, 3});
        const QString replacement = book.write(QStringLiteral("other.png"), other);
        book.answers.responder->open_path = [replacement](const QString&, const QString&) { return replacement; };
        book.act("act_smart_replace")->trigger();
        QCOMPARE(book.contents(so)->asset, core::bytes_ref(other));
        QCOMPARE(book.contents(so)->name, std::string("other.png"));
        // パッケージ…: the book with its links' copies
        const QString linked = book.write(QStringLiteral("link.png"), first);
        QVERIFY(w->place_linked(linked));
        const QString folder = book.tmp.path() + QStringLiteral("/package");
        QVERIFY(QDir().mkpath(folder));
        book.answers.responder->existing_dir = [folder](const QString&, const QString&) { return folder; };
        book.act("act_package")->trigger();
        QVERIFY2(w->status_label()->text().contains(QStringLiteral("パッケージを作りました")), qPrintable(w->status_label()->text()));
        QFile copy(folder + QStringLiteral("/b.genko/Links/link.png"));
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll().toStdString(), first);
        const core::Document packed = gui_test::read_book(gui_test::path_of(folder + QStringLiteral("/b.genko")));
        QVERIFY(packed.read_only_reason.empty());
        QVERIFY(packed.book_id != book.doc().book_id);
    }

    void adoptingACandidateSaysThePicturesSmartObject() {
        // (a book made with agents: page 1's panel with a candidate; its name approved, as art waits on it)
        QTemporaryDir tmp;
        const std::filesystem::path root = gui_test::path_of(tmp.path());
        service::StudioService agent(root, "ai:hermes");
        QVERIFY(agent.call("create_project", Json::array({"s.genko", "スタジオ", 1}), Json::object()).ok);
        const std::filesystem::path path = root / "s.genko";
        const std::string art = storage::AssetStore(path).put_bytes(one_colour(80, 60, render::Ink{120, 120, 120}), ".png");
        const std::string leaf = gui_test::read_book(path).page(0).leaf_frames().at(0)->id;
        (void)service::HumanService(path, "human:作者").approve_name(Json::array({1}));
        const auto made = agent.call(
            "apply_ops",
            Json::array({"s.genko", Json::array({Json{{"op", "open_request"}, {"request", Json{{"id", "rq_1"}, {"purpose", "art"}, {"mode", "new"},
                                                                                             {"target", Json{{"page", 1}, {"frame_id", leaf}}}}}},
                                                 Json{{"op", "import_candidates"}, {"request_id", "rq_1"},
                                                      {"candidates", Json::array({Json{{"id", "cd_1"}, {"asset", art}, {"px", {80, 60}},
                                                                                       {"origin", Json{{"kind", "agent"}}}}})}}})}),
            Json{{"commit", true}});
        QVERIFY2(made.ok, made.to_dict().dump().c_str());
        gui_test::Answers answers;
        auto session = app::Session::open(path, gui_test::quick(root / "recovery"));
        app::MainWindow window(session);
        window.resize(1366, 900);
        window.show();
        // a picture laid in the panel as 画像を読み込む… lays it: a smart object of the file
        QVERIFY(window.apply_ops(Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(one_colour(30, 20, render::Ink{9, 9, 9}))},
                                                   {"name", "photo.png"}, {"title", "photo"}, {"frame_id", leaf}}})));
        QDockWidget* dock = nullptr;
        for (QDockWidget* d : window.findChildren<QDockWidget*>())
            if (d->windowTitle() == QStringLiteral("コマの詳細")) dock = d;
        QVERIFY(dock != nullptr);
        dock->show();
        dock->raise();
        QVERIFY(gui_test::wait_for([&] { return dock->isVisible(); }, 60000));
        app::PanelView* view = window.panel_view();
        emit window.canvas()->frameSelected(QString::fromStdString(leaf));
        QVERIFY(gui_test::wait_for([&] { return view->candidates->count() == 1; }, 60000));
        view->candidates->setCurrentRow(0);
        view->adopt();
        // (the candidate is the panel's placed art; the smart object stays, and is said)
        bool placed = false, smart = false;
        for (const core::Layer& l : window.book().page(0).layers) {
            placed = placed || (l.kind == core::LayerKind::Placed && l.frame_id == leaf);
            smart = smart || (l.kind == core::LayerKind::Smart && l.frame_id == leaf);
        }
        QVERIFY(placed && smart);
        QVERIFY2(window.status_label()->text().contains(QStringLiteral("画像を読み込む…")) && window.status_label()->text().contains(QStringLiteral("photo")),
                 qPrintable(window.status_label()->text()));
    }

    void tilesOfAPageWhoseContentsChangedAreDrawnAgain() {
        // (the parent page is the same page: its smart object's contents another value)
        core::Document base = core::new_episode("t", core::Num(1), 1, core::PageSpec::webtoon());
        const core::CommandBus bus(render::ops_registry());
        base = bus.apply(base, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                            Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                            Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(two_colours())}},
                                            Json{{"op", "smart_convert"}, {"page", 1}, {"id", "pic"}, {"new_id", "so"}},
                                            Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so"}, {"matrix", Json::array({0.5, 0, 0, 0.5, 5, 5})}}}),
                         core::Actor("human:t"))
                   .doc;
        auto doc = std::make_shared<core::Document>(base);
        app::PageRenderer renderer;
        renderer.show(doc, 0);
        const double w_mm = doc->page(0).spec.width_mm.value(), h_mm = doc->page(0).spec.height_mm.value();
        renderer.want(40, 40, QRectF(0, 0, w_mm, h_mm));
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 30000);
        const QImage before = renderer.compose(40);
        std::string id;
        for (const core::Layer& l : doc->page(0).layers)
            if (l.id == "so") id = *core::smart::contents_id(l);
        QVERIFY(!id.empty());
        auto changed = std::make_shared<core::Document>(
            bus.apply(*doc, Json::array({Json{{"op", "fill_area"}, {"contents", id}, {"page", 1}, {"layer_id", "pic"}, {"area", square(400)}, {"rgb", Json::array({0, 0, 255})}}}),
                      core::Actor("human:t"))
                .doc);
        const auto generation = renderer.generation();
        renderer.show(changed, 0);
        QVERIFY(renderer.generation() > generation);
        QTRY_VERIFY_WITH_TIMEOUT(renderer.settled(), 30000);
        const QImage after = renderer.compose(40);
        QVERIFY(after != before);
        render::RenderOptions options;
        options.mode = "proof";
        options.skip_unported = true;
        QCOMPARE(after, as_qimage(render::render_page(changed->page(0), 40, options, changed.get()).image.convert("RGB")));
    }

    // PC4 F-f (design 2.8): 埋め込みを配置… of PhotoCraft's .pcraft — read on a worker thread, then placed by the op: a smart
    // object whose contents are its page (kind page: its layers, channels), what it could not keep said.
    void placingAPcraft() {
        Book book;
        app::MainWindow* w = book.window.get();
        const std::string bytes = genko::test::read_bytes(genko::test::test_data(QStringLiteral("pcraft/rich_rgb8.pcraft")));
        const QString file = book.write(QStringLiteral("rich.pcraft"), bytes);
        QVERIFY(w->place_embedded(file));
        const std::string so = book.smart_id();
        QVERIFY(!so.empty());
        const core::SmartContents* c = book.contents(so);
        QVERIFY(c != nullptr && c->kind == "page" && c->page);
        QVERIFY(c->page->layers.size() > 10);
        QVERIFY(!c->page->channels.empty());
        QCOMPARE(book.layer(so)->title, std::string("rich"));
        // (a broken one: said, nothing placed)
        const std::size_t layers = book.page().layers.size();
        const QString broken = book.write(QStringLiteral("broken.pcraft"), bytes.substr(0, bytes.size() / 2));
        QVERIFY(!w->place_embedded(broken));
        QCOMPARE(book.page().layers.size(), layers);
    }
};

QTEST_MAIN(TestGuiSmartContents)
#include "test_gui_smart_contents.moc"
