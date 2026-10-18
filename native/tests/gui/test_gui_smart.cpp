// smart-objects@1 (PC3 A0, design section A 5) in the window: レイヤー → スマートオブジェクト → スマートオブジェクトに変換
// from the menu (the later steps' commands in their places, not usable yet), the layer panel's row of a smart object (its
// kind and its mark 埋め込み), 新規スマートオブジェクト（コピー）and ラスタライズ; ファイル → 埋め込みを配置… (the file itself
// in the middle of the page); a file of 16 bits too large to keep them on a 16-bit page asked about; the 属性 panel's part
// of a smart object (its colours' use, its size); (PC3 A2) its smart filters in the layer panel and レイヤー → スマートフィルター.

#include <QtTest>

#include <QComboBox>
#include <QDockWidget>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <zlib.h>

#include "app/adjust_panels.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
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
        // (the later steps' commands: in their places, not usable yet, and said so)
        // (PC3 A1's — コンテンツを編集 and the links, リンクを配置…, パッケージ… — are usable: test_gui_smart_contents;
        // A2's smart filters: test_smart_filters)
        for (const char* later : {"act_smart_stack_median", "act_smart_warp", "act_rasterize_type"}) {
            QVERIFY2(!book.act(later)->isEnabled(), later);
            QVERIFY2(book.act(later)->toolTip().contains(QStringLiteral("この版ではまだ使えません")), later);
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
};

QTEST_MAIN(TestGuiSmart)
#include "test_gui_smart.moc"
