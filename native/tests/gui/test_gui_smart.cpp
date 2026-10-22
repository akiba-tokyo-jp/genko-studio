// smart-objects@1 (PC3 A0, design section A 5) in the window: レイヤー → スマートオブジェクト → スマートオブジェクトに変換
// from the menu (the later steps' commands in their places), the layer panel's row of a smart object (its
// kind and its mark 埋め込み), 新規スマートオブジェクト（コピー）and ラスタライズ; ファイル → 埋め込みを配置… (the file itself
// in the middle of the page); a file of 16 bits too large to keep them on a 16-bit page asked about; the 属性 panel's part
// of a smart object (its colours' use, its size); (PC3 A2) its smart filters in the layer panel and レイヤー → スマートフィルター;
// (PC3 A4) スタックモード ▸ and ラスタライズ → テキスト, the text tool's tooltip and コマンドを探す telling the two テキスト apart;
// (PC3 A5) ワープ・パペットワープ and ゆがみ… on a smart object, its bending filters edited again.

#include <QtTest>

#include <QComboBox>
#include <QDockWidget>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <zlib.h>

#include "app/adjust_panels.hpp"
#include "app/canvas.hpp"
#include "app/inject.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/workspace.hpp"
#include "core/base64.hpp"
#include "core/color_model.hpp"
#include "core/color_raster.hpp"
#include "core/smart.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"

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
        gui_test::write_book(path, core::new_episode("t", core::Num(1), 2, core::PageSpec::b4_comic()));
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 800);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        answers.responder->question = [](const QString&, const QString&) { return true; };
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    const core::Document& doc() const { return window->book(); }
    const core::Page& page(std::size_t row = 0) const { return *doc().pages[row]; }
    const core::Layer* layer(const std::string& id) const {
        for (const core::Layer& l : page().layers)
            if (l.id == id) return &l;
        return nullptr;
    }
    std::vector<const core::Layer*> smart_layers() const {
        std::vector<const core::Layer*> out;
        for (const core::Layer& l : page().layers)
            if (l.kind == core::LayerKind::Smart) out.push_back(&l);
        return out;
    }
    QString write(const QString& name, const std::string& bytes) const {
        const QString where = tmp.path() + QStringLiteral("/") + name;
        QFile out(where);
        if (!out.open(QIODevice::WriteOnly)) return {};
        out.write(bytes.data(), static_cast<qint64>(bytes.size()));
        return where;
    }
};

std::string be32(std::uint32_t v) {
    return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}

std::string chunk(const char* type, const std::string& data) {
    const std::string typed = std::string(type, 4) + data;
    return be32(static_cast<std::uint32_t>(data.size())) + typed +
           be32(static_cast<std::uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(typed.data()), static_cast<uInt>(typed.size()))));
}

std::string zipped(const std::string& raw) {
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    std::string packed(size, '\0');
    compress(reinterpret_cast<Bytef*>(packed.data()), &size, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()));
    packed.resize(size);
    return packed;
}

// A grey PNG of 16 bits, every sample `value`.
std::string grey16(int w, int h, std::uint16_t value) {
    std::string row(1 + std::size_t(w) * 2, '\0');
    for (int x = 0; x < w; ++x) {
        row[1 + std::size_t(x) * 2] = static_cast<char>(value >> 8);
        row[2 + std::size_t(x) * 2] = static_cast<char>(value & 0xff);
    }
    std::string raw;
    raw.reserve(row.size() * std::size_t(h));
    for (int y = 0; y < h; ++y) raw += row;
    std::string out("\x89PNG\r\n\x1a\n", 8);
    out += chunk("IHDR", be32(static_cast<std::uint32_t>(w)) + be32(static_cast<std::uint32_t>(h)) + std::string{16, 0, 0, 0, 0});
    out += chunk("IDAT", zipped(raw));
    out += chunk("IEND", "");
    return out;
}

// An RGB PNG of one colour with an ICC profile in it.
std::string with_profile(const std::string& icc) {
    const std::string png = render::write_png(render::Image::create("RGB", render::Size{4, 4}, render::Ink{128, 64, 32}));
    return png.substr(0, 33) + chunk("iCCP", std::string("icc\0\0", 5) + zipped(icc)) + png.substr(33);  // (after the signature and IHDR)
}

std::string two_colours() {
    render::Image picture = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{128, 128, 128, 255});
    picture.paste(render::Ink{30, 160, 90, 255}, render::Box{30, 0, 60, 40});
    return render::write_png(picture);
}

}  // namespace

class TestGuiSmart : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { (void)gui_test::config_folder(); }

    void convertFromTheMenu() {
        Book book;
        app::MainWindow* w = book.window.get();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"name", "絵"}},
                                          Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(two_colours())}}})));
        w->set_target_layer("pic");
        // the menus: レイヤー → スマートオブジェクト・スマートフィルター・ラスタライズ, ファイル → 埋め込みを配置… after 画像を読み込む…
        auto* smart = w->findChild<QMenu*>(QStringLiteral("menu_smart"));
        auto* filters = w->findChild<QMenu*>(QStringLiteral("menu_smart_filters"));
        auto* rasterize = w->findChild<QMenu*>(QStringLiteral("menu_rasterize"));
        QVERIFY(smart != nullptr && filters != nullptr && rasterize != nullptr);
        QCOMPARE(smart->title(), QStringLiteral("スマートオブジェクト"));
        QVERIFY(smart->actions().contains(book.act("act_smart_convert")));
        QVERIFY(filters->actions().contains(book.act("act_smart_filters_convert")));
        QVERIFY(rasterize->actions().contains(book.act("act_rasterize_all")));
        QCOMPARE(book.act("act_smart_convert")->text(), QStringLiteral("スマートオブジェクトに変換"));
        QCOMPARE(book.act("act_place_embedded")->text(), QStringLiteral("埋め込みを配置…"));
        // (the later steps' commands, in their places and usable: PC3 A1's — コンテンツを編集 and the links, リンクを配置…,
        // パッケージ… — test_gui_smart_contents; A2's smart filters: test_smart_filters; A4's stack modes and ラスタライズ →
        // テキスト: stackModesAndTheTextRasterized; A5's ワープ・遠近法ワープ・パペットワープ: warpsOfASmartObject)
        for (const char* later : {"act_smart_warp", "act_smart_perspective_warp", "act_smart_puppet_warp"}) {
            QVERIFY2(smart->actions().contains(book.act(later)), later);
            QVERIFY2(book.act(later)->isEnabled(), later);
        }
        QVERIFY(book.act("act_smart_convert")->isEnabled());

        book.act("act_smart_convert")->trigger();
        const core::Layer* so = w->target_layer();
        QVERIFY(so != nullptr && so->kind == core::LayerKind::Smart);
        QVERIFY(book.layer("pic") == nullptr);
        const core::SmartContents* contents = core::smart::contents_of(book.doc(), *so);
        QVERIFY(contents != nullptr && contents->kind == "page" && contents->page && contents->page->layers.size() == 1);
        QCOMPARE(contents->page->layers[0].id, std::string("pic"));
        const std::string id = so->id;
        // the layer panel: its kind and its mark
        app::LayerPanel* panel = w->layer_panel();
        panel->refresh();
        QListWidgetItem* row = nullptr;
        for (int i = 0; i < panel->list()->count(); ++i)
            if (panel->list()->item(i)->toolTip() == QStringLiteral("スマートオブジェクト")) row = panel->list()->item(i);
        QVERIFY(row != nullptr);
        QVERIFY2(row->text().contains(QStringLiteral("埋め込み")) && row->text().contains(QStringLiteral("絵")), qPrintable(row->text()));
        QVERIFY(!row->icon().isNull());

        // 新規スマートオブジェクト（コピー）: contents of its own
        book.act("act_smart_copy")->trigger();
        QCOMPARE(book.smart_layers().size(), std::size_t(2));
        QVERIFY(core::smart::contents_id(*book.smart_layers()[0]) != core::smart::contents_id(*book.smart_layers()[1]));
        // ラスタライズ → スマートオブジェクト: the copy a paint layer
        const std::string copy = w->target_layer()->id;
        book.act("act_rasterize_smart")->trigger();
        QCOMPARE(book.layer(copy)->kind, core::LayerKind::Raster);
        QCOMPARE(book.smart_layers().size(), std::size_t(1));
        // undone: the copy back as a smart object
        book.act("act_undo")->trigger();
        QCOMPARE(book.layer(copy)->kind, core::LayerKind::Smart);
        // レイヤーの内容オプション… on a smart object: コンテンツを編集 (PC3 A1: its contents in a tab of the book)
        w->set_target_layer(id);
        book.act("act_layer_content_options")->trigger();
        QCOMPARE(w->contents_tab(), core::smart::contents_id(*book.layer(id)));
    }

    void placeEmbeddedInTheMiddle() {
        Book book;
        const std::string png = render::write_png(render::Image::create("RGB", render::Size{200, 100}, render::Ink{200, 40, 90}));
        const QString file = book.write(QStringLiteral("photo.png"), png);
        QString filter;
        book.answers.responder->open_path = [&filter, file](const QString&, const QString& f) {
            filter = f;
            return file;
        };
        book.act("act_place_embedded")->trigger();
        QVERIFY2(filter.contains(QStringLiteral("*.png")) && filter.contains(QStringLiteral("*.gif")), qPrintable(filter));
        QCOMPARE(book.smart_layers().size(), std::size_t(1));
        const core::Layer& so = *book.smart_layers()[0];
        QCOMPARE(so.title, std::string("photo"));
        const core::SmartContents* contents = core::smart::contents_of(book.doc(), so);
        QVERIFY(contents != nullptr && contents->kind == "file" && contents->suffix == ".png");
        QCOMPARE(*storage::AssetStore(book.path).get_bytes(contents->asset, ".png"), png);
        // (in the middle of the page, at its own size: no larger than the page)
        const auto [cw, ch] = core::smart::size_mm(*contents);
        const core::smart::Matrix m = core::smart::matrix_of(so);
        const auto [x, y] = m.apply(cw / 2, ch / 2);
        QVERIFY(std::abs(x - book.page().spec.width_mm.value() / 2) < 1e-6 && std::abs(y - book.page().spec.height_mm.value() / 2) < 1e-6);
        QVERIFY(m.a <= 1 + 1e-9 && std::abs(m.a - m.d) < 1e-12);
        // a book not saved yet: it asks for a place first
        app::MainWindow untitled;
        QVERIFY(!untitled.place_embedded(file));
    }

    void aLargeFileOnASixteenBitPageIsAsked() {
        Book book;
        app::MainWindow* w = book.window.get();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}})));
        // (small: its 16 bits kept, nothing asked)
        QStringList asked;
        book.answers.responder->question = [&asked](const QString& title, const QString& text) {
            asked << title + QStringLiteral(": ") + text;
            return false;
        };
        QVERIFY(w->place_embedded(book.write(QStringLiteral("small.png"), grey16(8, 8, 1000))));
        QVERIFY(asked.isEmpty());
        QCOMPARE(book.smart_layers().size(), std::size_t(1));
        QCOMPARE(core::smart::contents_of(book.doc(), *book.smart_layers()[0])->depth, 16);
        // (larger than a page's precise pictures may be: asked; "no" places nothing)
        const int side = static_cast<int>(std::ceil(std::sqrt(double(core::kColorRasterMaxPixels) + 1)));
        const QString large = book.write(QStringLiteral("large.png"), grey16(side, side, 1000));
        QVERIFY(!w->place_embedded(large));
        QCOMPARE(asked.size(), 1);
        QVERIFY2(asked[0].contains(QStringLiteral("16 bit のままでは大きすぎます")), qPrintable(asked[0]));
        QCOMPARE(book.smart_layers().size(), std::size_t(1));
    }

    void thePropertiesPanelChangesTheColourUse() {
        Book book;
        app::MainWindow* w = book.window.get();
        int choice = 0;  // (埋め込まれたプロファイル: 変換しない)
        book.answers.responder->get_item = [&choice](const QString&, const QString&, const QStringList& items, int, bool) {
            return std::optional<QString>(items.at(choice));
        };
        // (画像を読み込む…: laid over the page, as the placed picture was)
        QVERIFY(w->import_image(book.write(QStringLiteral("p3.png"), with_profile(*core::builtin_profile("display-p3")->bytes))));
        const std::string id = book.smart_layers().at(0)->id;
        const auto use = [&] { return core::smart::contents_of(book.doc(), *book.layer(id))->profile.value("use", std::string()); };
        const auto drawn = [&] {
            const render::Image alone = render::layer_image(book.page(), *book.layer(id), 20, &book.doc());
            std::vector<double> p = alone.getpixel(alone.width() / 2, alone.height() / 2);
            p.resize(3);
            return p;
        };
        QCOMPARE(use(), std::string("keep"));
        QCOMPARE(drawn(), (std::vector<double>{128, 64, 32}));
        w->set_target_layer(id);
        auto* properties = w->findChild<QDockWidget*>(QStringLiteral("属性"));
        QVERIFY(properties != nullptr);
        auto* panel = qobject_cast<app::adjust_forms::PropertiesPanel*>(properties->widget());
        QVERIFY(panel != nullptr);
        panel->refresh();
        auto* about = panel->findChild<QLabel*>(QStringLiteral("properties_smart_contents"));
        QVERIFY(about != nullptr && about->text().contains(QStringLiteral("p3.png")) && about->text().contains(QStringLiteral("PNG 8 bit")));
        QVERIFY(panel->findChild<QPushButton*>(QStringLiteral("properties_smart_edit"))->isEnabled());  // (PC3 A1: コンテンツを編集)
        auto* colours = panel->findChild<QComboBox*>(QStringLiteral("properties_smart_profile"));
        QVERIFY(colours != nullptr);
        colours->setCurrentIndex(1);  // 埋め込みから変換
        QCOMPARE(use(), std::string("embedded"));
        QVERIFY(drawn() != (std::vector<double>{128, 64, 32}));
        book.act("act_undo")->trigger();
        QCOMPARE(use(), std::string("keep"));
        // its size: W 50 % applied as smart_transform (its contents untouched)
        panel->refresh();
        auto* width = panel->findChild<QDoubleSpinBox*>(QStringLiteral("properties_smart_w"));
        const core::smart::Matrix before = core::smart::matrix_of(*book.layer(id));
        width->setValue(width->value() / 2);
        panel->findChild<QPushButton*>(QStringLiteral("properties_smart_apply"))->click();
        const core::smart::Matrix after = core::smart::matrix_of(*book.layer(id));
        QVERIFY(std::abs(after.a - before.a / 2) < 1e-4);  // (the box shows hundredths of a percent)
        // (what was left as it was shown: as it was, not its rounded number)
        QVERIFY(std::abs(after.d - before.d) < 1e-9 && std::abs(after.e - before.e) < 1e-9 && std::abs(after.f - before.f) < 1e-9);
        QCOMPARE(use(), std::string("keep"));
    }

    // (independent review A P2-2) The 属性 panel's 変形を適用 moves the smart object as the Move tool does (transform_layers):
    // its corners and its shear kept, the layers linked to it along; nothing where its position is locked.
    void thePropertiesPanelTransformsAsTheMoveToolDoes() {
        Book book;
        app::MainWindow* w = book.window.get();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"name", "絵"}},
                                          Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(two_colours())}},
                                          Json{{"op", "smart_convert"}, {"page", 1}, {"id", "pic"}, {"new_id", "so"}},
                                          Json{{"op", "add_layer"}, {"page", 1}, {"kind", "pen"}, {"id", "other"}, {"name", "線"}, {"panel_each", false}},
                                          Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "other"}, {"points", Json::array({Json::array({20, 30}), Json::array({80, 60})})},
                                               {"width_mm", 1.0}},
                                          Json{{"op", "link_layers"}, {"page", 1}, {"ids", Json::array({"so", "other"})}}})));
        const auto corners = Json::array({Json::array({10, 20}), Json::array({200, 30}), Json::array({190, 300}), Json::array({20, 280})});
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so"}, {"perspective", corners}}})));
        w->set_target_layer("so");
        auto* properties = w->findChild<QDockWidget*>(QStringLiteral("属性"));
        QVERIFY(properties != nullptr);
        auto* panel = qobject_cast<app::adjust_forms::PropertiesPanel*>(properties->widget());
        QVERIFY(panel != nullptr);
        // (the panel shown again for the book as it is now: what it showed before let go)
        const auto again = [panel] {
            panel->refresh();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        };
        again();
        const auto line_x = [&] { return book.layer("other")->strokes->items.at(0)->points.at(0).x; };
        const double line_before = line_x();
        auto* x = panel->findChild<QDoubleSpinBox*>(QStringLiteral("properties_smart_x"));
        QVERIFY(x != nullptr);
        x->setValue(x->value() + 5);
        panel->findChild<QPushButton*>(QStringLiteral("properties_smart_apply"))->click();
        // (its corners moved 5 mm, still its own; the linked line along)
        const Json& moved = (*book.layer("so")->smart)["perspective"];
        QVERIFY2(moved.is_array() && moved.size() == 4, moved.dump().c_str());
        for (std::size_t i = 0; i < 4; ++i) {
            QVERIFY(std::abs(moved[i][0].get<double>() - (corners[i][0].get<double>() + 5)) < 1e-6);
            QVERIFY(std::abs(moved[i][1].get<double>() - corners[i][1].get<double>()) < 1e-6);
        }
        QVERIFY(std::abs(line_x() - (line_before + 5)) < 1e-6);
        // a shear kept when its width is halved
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "smart_transform"}, {"page", 1}, {"id", "so"}, {"set_matrix", Json::array({1, 0, 0.3, 1, 10, 10})}}})));
        again();
        auto* width = panel->findChild<QDoubleSpinBox*>(QStringLiteral("properties_smart_w"));
        width->setValue(width->value() / 2);
        panel->findChild<QPushButton*>(QStringLiteral("properties_smart_apply"))->click();
        const core::smart::Matrix sheared = core::smart::matrix_of(*book.layer("so"));
        QVERIFY2(std::abs(sheared.a - 0.5) < 1e-6 && std::abs(sheared.b) < 1e-6 && std::abs(sheared.c - 0.3) < 1e-6 && std::abs(sheared.d - 1) < 1e-6 &&
                     std::abs(sheared.e - 10) < 1e-6 && std::abs(sheared.f - 10) < 1e-6,
                 core::smart::to_json(sheared).dump().c_str());
        // its position locked: it stays
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", "so"}, {"lock_position", true}}})));
        again();
        x = panel->findChild<QDoubleSpinBox*>(QStringLiteral("properties_smart_x"));
        x->setValue(x->value() + 5);
        panel->findChild<QPushButton*>(QStringLiteral("properties_smart_apply"))->click();
        QCOMPARE(core::smart::matrix_of(*book.layer("so")), sheared);
    }

    // PC3 A2: the layer panel's かける… on a smart object adds a smart filter (asked so); its rows under the smart object —
    // スマートフィルター and each filter, the top one first — with their eyes; スマートフィルターを編集… in the filter's own
    // window, 描画オプション…, moving one; レイヤー → スマートフィルター's commands; undone.
    void smartFiltersInTheLayerPanel() {
        Book book;
        app::MainWindow* w = book.window.get();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"name", "絵"}},
                                          Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(two_colours())}},
                                          Json{{"op", "smart_convert"}, {"page", 1}, {"id", "pic"}, {"new_id", "so"}}})));
        w->set_target_layer("so");
        const auto filters = [&] { return book.layer("so")->smart->value("filters", Json::array()); };
        app::LayerPanel* panel = w->layer_panel();
        panel->refresh();
        QStringList asked;
        book.answers.responder->question = [&asked](const QString&, const QString& text) {
            asked << text;
            return true;
        };
        panel->filter_box()->setCurrentIndex(panel->filter_box()->findData(QStringLiteral("invert")));
        panel->apply_filter();
        QCOMPARE(asked.size(), 1);
        QVERIFY2(asked[0].contains(QStringLiteral("スマートフィルターとして足します")), qPrintable(asked[0]));
        QCOMPARE(filters().size(), std::size_t(1));
        QCOMPARE(filters()[0]["kind"], Json("invert"));
        // (a second one, from an op: the rows list the top one first)
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "smart_filter_add"}, {"page", 1}, {"id", "so"}, {"kind", "blur"}, {"radius", 2}}})));
        panel->refresh();
        const auto rows = [&] {
            std::vector<std::pair<QString, QListWidgetItem*>> out;
            for (int i = 0; i < panel->list()->count(); ++i) {
                QListWidgetItem* item = panel->list()->item(i);
                const QString what = item->data(Qt::UserRole + 4).toString();
                if (!what.isEmpty()) out.emplace_back(what, item);
            }
            return out;
        };
        auto shown = rows();
        QCOMPARE(shown.size(), std::size_t(3));
        QCOMPARE(shown[0].first, QStringLiteral("smart_filters"));
        QCOMPARE(shown[1].first, QStringLiteral("smart_filter:1"));
        QCOMPARE(shown[2].first, QStringLiteral("smart_filter:0"));
        QVERIFY2(shown[0].second->text().contains(QStringLiteral("スマートフィルター")), qPrintable(shown[0].second->text()));
        QCOMPARE(panel->row_kind(panel->list()->row(shown[1].second)), 1);
        // (a filter's eye: hidden; the header's: all of them off)
        shown[2].second->setCheckState(Qt::Unchecked);
        QCOMPARE(filters()[0]["visible"], Json(false));
        shown = rows();
        shown[0].second->setCheckState(Qt::Unchecked);
        QCOMPARE(book.layer("so")->smart->value("filters_on", true), false);
        book.act("act_smart_filters_off")->trigger();  // (on again)
        QVERIFY(!book.layer("so")->smart->contains("filters_on"));
        // スマートフィルターを編集…: the filter's own window, its settings now; what the person set there kept
        w->set_target_layer("so");
        panel->show_target();
        book.answers.responder->exec = [](QDialog* dialog) {
            for (auto* box : dialog->findChildren<QDoubleSpinBox*>()) box->setValue(5);
            for (auto* box : dialog->findChildren<QSpinBox*>()) box->setValue(5);
            return int(QDialog::Accepted);
        };
        panel->edit_smart_filter(1);
        QCOMPARE(filters()[1]["kind"], Json("blur"));
        QCOMPARE(filters()[1]["params"]["radius"].get<double>(), 5.0);
        // 描画オプション…: its blend mode and opacity
        book.answers.responder->exec = [](QDialog* dialog) {
            auto* mode = dialog->findChild<QComboBox*>(QStringLiteral("smart_filter_blend"));
            auto* opacity = dialog->findChild<QSpinBox*>(QStringLiteral("smart_filter_opacity"));
            if (mode == nullptr || opacity == nullptr) return int(QDialog::Rejected);
            mode->setCurrentIndex(mode->findData(QStringLiteral("multiply")));
            opacity->setValue(40);
            return int(QDialog::Accepted);
        };
        panel->smart_filter_blending(1);
        QCOMPARE(filters()[1]["blend"], Json("multiply"));
        QCOMPARE(filters()[1]["opacity"], Json(0.4));
        // moved down (as the right button's 下へ)
        panel->smart_filter_op(Json{{"index", 1}, {"to", 0}}, "smart_filter_move");
        QCOMPARE(filters()[0]["kind"], Json("blur"));
        // レイヤー → スマートフィルター: the mask's commands say there is none; クリア takes every filter; undone
        book.act("act_smart_filter_mask_delete")->trigger();
        QVERIFY2(w->status_label()->text().contains(QStringLiteral("フィルターマスクがありません")), qPrintable(w->status_label()->text()));
        book.act("act_smart_filters_clear")->trigger();
        QVERIFY(!book.layer("so")->smart->contains("filters"));
        panel->refresh();
        QVERIFY(rows().empty());
        book.act("act_undo")->trigger();
        QCOMPARE(filters().size(), std::size_t(2));
        // (a selection: its filter mask, and the mask's commands)
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "smart_filter_state"}, {"page", 1}, {"id", "so"}, {"mask", "from_area"},
                                               {"area", Json{{"poly", Json::array({Json::array({0, 0}), Json::array({50, 0}), Json::array({50, 50})})}}}}})));
        book.act("act_smart_filter_mask_off")->trigger();
        QCOMPARE(book.layer("so")->smart->at("filter_mask")["enabled"], Json(false));
        book.act("act_smart_filter_mask_delete")->trigger();
        QVERIFY(!book.layer("so")->smart->contains("filter_mask"));
    }

    // PC3 A5: レイヤー → スマートオブジェクト → ワープ on the smart object drawn on (a style chosen, Enter: its warp, its
    // contents as they were; again: the box starts from its warp), パペットワープ (its smart filter; スマートフィルターを編集…
    // from the layer panel: the box with its pins, Enter sets them again) and ゆがみ… on it (its smart filter; edited again
    // with its strokes); without a smart object, a word.
    void warpsOfASmartObject() {
        Book book;
        app::MainWindow* w = book.window.get();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}, {"name", "絵"}},
                                          Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(two_colours())}},
                                          Json{{"op", "smart_convert"}, {"page", 1}, {"id", "pic"}, {"new_id", "so"}}})));
        const auto contents = core::smart::contents_id(*book.layer("so"));
        const auto* held = book.doc().smart_contents.at(*contents).get();
        w->set_target_layer("so");
        app::PageCanvas* canvas = w->canvas();
        // ワープ: the arc chosen
        book.act("act_smart_warp")->trigger();
        QVERIFY(canvas->transforming());
        QCOMPARE(canvas->transform_mode(), QStringLiteral("warp"));
        auto* style = w->findChild<QComboBox*>(QStringLiteral("warp_style"));
        QVERIFY(style != nullptr);
        style->setCurrentIndex(style->findData(QStringLiteral("arc")));
        QTest::keyClick(canvas, Qt::Key_Return);
        QVERIFY(!canvas->transforming());
        QCOMPARE((*book.layer("so")->smart)["warp"]["style"], Json("arc"));
        QVERIFY(book.doc().smart_contents.at(*contents).get() == held);  // (its contents untouched)
        // (again: from its arc)
        book.act("act_smart_warp")->trigger();
        QCOMPARE(canvas->transform_warp()["style"], Json("arc"));
        style = w->findChild<QComboBox*>(QStringLiteral("warp_style"));
        QCOMPARE(style->currentData().toString(), QStringLiteral("arc"));
        QTest::keyClick(canvas, Qt::Key_Escape);
        QVERIFY(!canvas->transforming());
        // 遠近法ワープ: the same box as 編集's, on it
        book.act("act_smart_perspective_warp")->trigger();
        QCOMPARE(canvas->transform_mode(), QStringLiteral("perspective_warp"));
        QTest::keyClick(canvas, Qt::Key_Escape);
        // パペットワープ: three pins, one pulled — its smart filter
        const auto filters = [&] { return book.layer("so")->smart->value("filters", Json::array()); };
        book.act("act_smart_puppet_warp")->trigger();
        QCOMPARE(canvas->transform_mode(), QStringLiteral("puppet"));
        app::inject::click_mm(canvas, QPointF(80, 120));
        app::inject::click_mm(canvas, QPointF(180, 120));
        app::inject::click_mm(canvas, QPointF(130, 280));
        app::inject::mouse_stroke(canvas, {QPointF(180, 120), QPointF(186, 122), QPointF(192, 124)});
        QTest::keyClick(canvas, Qt::Key_Return);
        QCOMPARE(filters().size(), std::size_t(1));
        QCOMPARE(filters()[0]["kind"], Json("puppet_warp"));
        QCOMPARE(filters()[0]["params"]["pins"].size(), std::size_t(3));
        QVERIFY(std::fabs(filters()[0]["params"]["pins"][1]["dst"][0].get<double>() - 192) < 0.3);
        QVERIFY(core::smart::is_identity(core::smart::matrix_of(*book.layer("so"))));
        // (スマートフィルターを編集…: the box with its pins; one pulled further, Enter: the filter set again)
        app::LayerPanel* panel = w->layer_panel();
        panel->refresh();
        panel->edit_smart_filter(0);
        QVERIFY(canvas->transforming());
        QCOMPARE(canvas->transform_mode(), QStringLiteral("puppet"));
        QCOMPARE(canvas->puppet_pins().size(), std::size_t(3));
        app::inject::mouse_stroke(canvas, {QPointF(192, 124), QPointF(196, 126), QPointF(200, 128)});
        QTest::keyClick(canvas, Qt::Key_Return);
        QCOMPARE(filters().size(), std::size_t(1));
        QVERIFY(std::fabs(filters()[0]["params"]["pins"][1]["dst"][0].get<double>() - 200) < 0.3);
        // ゆがみ… on it: its smart filter; edited again with the strokes it has
        const auto pushed = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("liquify_dialog")) return int(QDialog::Rejected);
            QWidget* view = dialog->findChild<QWidget*>(QStringLiteral("liquify_view"));
            if (view == nullptr) return int(QDialog::Rejected);
            const double scale = view->width() / 257.0;  // (the B4 page's mm on the window)
            const auto send = [view](QEvent::Type type, QPointF at) {
                const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
                const Qt::MouseButtons down = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
                QMouseEvent event(type, at, view->mapToGlobal(at), button, down, Qt::NoModifier);
                QCoreApplication::sendEvent(view, &event);
            };
            send(QEvent::MouseButtonPress, QPointF(100 * scale, 150 * scale));
            for (int i = 1; i <= 6; ++i) send(QEvent::MouseMove, QPointF((100 + 2 * i) * scale, 150 * scale));
            send(QEvent::MouseButtonRelease, QPointF(112 * scale, 150 * scale));
            return int(QDialog::Accepted);
        };
        book.answers.responder->exec = pushed;
        book.act("act_liquify_filter")->trigger();
        QCOMPARE(filters().size(), std::size_t(2));
        QCOMPARE(filters()[1]["kind"], Json("liquify"));
        QCOMPARE(filters()[1]["params"]["strokes"].size(), std::size_t(1));
        panel->refresh();
        panel->edit_smart_filter(1);
        QCOMPARE(filters().size(), std::size_t(2));
        QCOMPARE(filters()[1]["params"]["strokes"].size(), std::size_t(2));
        // (without a smart object: a word)
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "plain"}}})));
        w->set_target_layer("plain");
        book.act("act_smart_warp")->trigger();
        QVERIFY(!canvas->transforming());
        QCOMPARE(w->last_notice(), QStringLiteral("先にスマートオブジェクトを選びます"));
    }

    // PC3 A4: レイヤー → スマートオブジェクト → スタックモード ▸ on the smart object drawn on (the 属性 panel's スタックモード
    // showing and setting it too); ラスタライズ → テキスト on the line chosen with the select tool (undone: the line again);
    // the text tool's tooltip its name and key alone while コマンドを探す tells テキスト of ツール and of レイヤー → ラスタライズ
    // apart (the user's decision 8 of PC2).
    void stackModesAndTheTextRasterized() {
        Book book;
        app::MainWindow* w = book.window.get();
        Json ops = Json::array();
        for (const char* id : {"f0", "f1", "f2"}) {
            ops.push_back(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", id}});
            ops.push_back(Json{{"op", "put_raster"}, {"page", 1}, {"id", id}, {"png_base64", core::b64encode(two_colours())}});
        }
        ops.push_back(Json{{"op", "smart_convert"}, {"page", 1}, {"ids", Json::array({"f0", "f1", "f2"})}, {"new_id", "so"}});
        QVERIFY(w->apply_ops(ops));
        w->set_target_layer("so");
        const auto stack = [&] { return book.layer("so")->smart->value("stack", std::string("none")); };
        const QStringList modes{QStringLiteral("entropy"), QStringLiteral("kurtosis"),          QStringLiteral("maximum"),   QStringLiteral("mean"),
                                QStringLiteral("median"),  QStringLiteral("minimum"),           QStringLiteral("range"),     QStringLiteral("skewness"),
                                QStringLiteral("standard_deviation"), QStringLiteral("summation"), QStringLiteral("variance"), QStringLiteral("none")};
        auto* menu = w->findChild<QMenu*>(QStringLiteral("menu_smart"));
        QMenu* stack_menu = nullptr;
        for (QAction* a : menu->actions())
            if (a->menu() != nullptr && a->text() == QStringLiteral("スタックモード")) stack_menu = a->menu();
        QVERIFY(stack_menu != nullptr);
        QCOMPARE(stack_menu->actions().size(), modes.size());
        for (const QString& mode : modes) {
            QAction* a = w->action(QStringLiteral("act_smart_stack_%1").arg(mode));
            QVERIFY2(a != nullptr && a->isEnabled() && stack_menu->actions().contains(a), qPrintable(mode));
            a->trigger();
            QCOMPARE(QString::fromStdString(stack()), mode);
        }
        QCOMPARE(w->action(QStringLiteral("act_smart_stack_median"))->text(), QStringLiteral("中央値"));
        book.act("act_smart_stack_median")->trigger();
        QCOMPARE(stack(), std::string("median"));
        book.act("act_undo")->trigger();
        QCOMPARE(stack(), std::string("none"));
        // (the 属性 panel: the mode shown, and set there)
        auto* panel = qobject_cast<app::adjust_forms::PropertiesPanel*>(w->findChild<QDockWidget*>(QStringLiteral("属性"))->widget());
        QVERIFY(panel != nullptr);
        panel->refresh();
        auto* box = panel->findChild<QComboBox*>(QStringLiteral("properties_smart_stack"));
        QVERIFY(box != nullptr && box->isEnabled());
        QCOMPARE(box->count(), 12);
        QCOMPARE(box->currentText(), QStringLiteral("なし"));
        box->setCurrentIndex(box->findData(QStringLiteral("mean")));
        QCOMPARE(stack(), std::string("mean"));
        panel->refresh();
        QCOMPARE(panel->findChild<QComboBox*>(QStringLiteral("properties_smart_stack"))->currentText(), QStringLiteral("平均"));
        // (a layer that is not a smart object: said, nothing done)
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "plain"}}})));
        w->set_target_layer("plain");
        book.act("act_smart_stack_median")->trigger();
        QVERIFY2(w->status_label()->text().contains(QStringLiteral("スマートオブジェクトを選んでください")), qPrintable(w->status_label()->text()));

        // ラスタライズ → テキスト: the line chosen, a paint layer at the top; undone, the line again
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_line"}, {"page", 1}, {"id", "l1"}, {"text", "台詞です"}, {"x_mm", 40}, {"y_mm", 60}}})));
        w->canvas()->selected_line_id.reset();
        book.act("act_rasterize_type")->trigger();
        QVERIFY2(w->status_label()->text().contains(QStringLiteral("台詞")), qPrintable(w->status_label()->text()));
        QCOMPARE(book.doc().story.size(), std::size_t(1));
        w->canvas()->selected_line_id = std::string("l1");
        book.act("act_rasterize_type")->trigger();
        QVERIFY(book.doc().story.empty());
        const core::Layer& made = book.page().layers.back();
        QCOMPARE(made.kind, core::LayerKind::Raster);
        QCOMPARE(w->target_layer()->id, made.id);
        QVERIFY(!w->canvas()->selected_line_id);
        book.act("act_undo")->trigger();
        QCOMPARE(book.doc().story.size(), std::size_t(1));
        QCOMPARE(book.doc().story[0].id, std::string("l1"));

        // the text tool's tooltip: its name and key alone; ラスタライズ → テキスト's: its words and its place
        QCOMPARE(book.act("act_text")->toolTip(), QStringLiteral("テキスト（T）\nクリックした所に台詞を入力します（縦書き）"));
        QVERIFY2(book.act("act_rasterize_type")->toolTip().startsWith(QStringLiteral("テキスト（レイヤー → ラスタライズ）\n")),
                 qPrintable(book.act("act_rasterize_type")->toolTip()));
        // コマンドを探す: both found, each with its place; the names kept for the quick access and the command bar their own
        app::CommandSearch* search = w->find_command();
        search->query->setText(QStringLiteral("テキスト"));
        const auto& found = search->found();
        QVERIFY(std::find(found.begin(), found.end(), book.act("act_text")) != found.end());
        QVERIFY(std::find(found.begin(), found.end(), book.act("act_rasterize_type")) != found.end());
        QStringList rows;
        for (int i = 0; i < search->results->count(); ++i) rows << search->results->item(i)->text();
        for (const QString& row : {QStringLiteral("テキスト　（ツール）"), QStringLiteral("テキスト　（レイヤー → ラスタライズ）")}) {
            QVERIFY2(std::any_of(rows.begin(), rows.end(), [&row](const QString& r) { return r.startsWith(row); }), qPrintable(rows.join(QStringLiteral(" | "))));
        }
        search->close();
        const auto names = app::by_text(w);
        QCOMPARE(names.at(QStringLiteral("テキスト（ツール）")), book.act("act_text"));
        QCOMPARE(names.at(QStringLiteral("テキスト（レイヤー → ラスタライズ）")), book.act("act_rasterize_type"));
        QCOMPARE(names.at(QStringLiteral("テキスト")), book.act("act_text"));  // (the words alone: the tool's, as before)
    }
};

QTEST_MAIN(TestGuiSmart)
#include "test_gui_smart.moc"
