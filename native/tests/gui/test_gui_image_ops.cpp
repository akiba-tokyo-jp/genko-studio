// PC2c (局所の補正と画像の演算, SPEC.md COLOR-04) on the offscreen platform: レイヤー → 色調補正's シャドウ・ハイライト…,
// HDR トーン…, 色の置き換え…, 色の一致…, 画像操作…, 演算… and トラッピング…, and 編集 → フェード… — each a window of its
// fields whose result shows on the page, applied as one op (one Undo); フェード only right after a filter, a correction or
// a fill (the pixels before in the book's assets), HDR トーン asking before it flattens, トラッピング on a CMYK page only,
// 演算 into a selection leaving the book as it was.
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QMenu>
#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/history_panel.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "core/command_bus.hpp"
#include "core/base64.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "service/wording.hpp"
using namespace genko;
using core::Json;

namespace {

// A book whose page 1 has a paint layer "pic" (a dark ramp left, light right, a red block) under the page's ink layer,
// and a second paint layer "two".
core::Document book_doc() {
    auto doc = core::new_episode("画像", core::Num(1), 2, core::PageSpec::custom(40, 40, 30, 30, 1, 2, 2, 2, 2, 72, "color"));
    std::string bytes;
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 80; ++x) {
            const bool red = x >= 50 && x < 70 && y >= 10 && y < 30;
            const int v = x * 255 / 79;
            for (const int c : red ? std::array<int, 4>{220, 30, 30, 255} : std::array<int, 4>{v, v * 4 / 5, v * 3 / 5 + 20, 255}) bytes.push_back(static_cast<char>(c));
        }
    const render::Image picture = render::Image::frombytes("RGBA", render::Size{80, 80}, bytes);
    return core::CommandBus(render::ops_registry())
        .apply(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                 Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_clip", false}},
                                 Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(picture))}},
                                 Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "two"}},
                                 Json{{"op", "set_layer"}, {"page", 1}, {"id", "two"}, {"panel_clip", false}},
                                 Json{{"op", "put_raster"}, {"page", 1}, {"id", "two"},
                                      {"png_base64", core::b64encode(render::write_png(render::Image::create("RGBA", render::Size{10, 10}, render::Ink{90, 90, 90, 255})))}}}),
               core::Actor("human:tester"))
        .doc;
}

struct Book {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    explicit Book(const core::Document& doc = book_doc()) {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, doc);
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 800);
        window->show();
        QCoreApplication::processEvents();
    }
    const core::Page& page() const { return session->document().page(0); }
    const core::Layer* layer(const std::string& id) const {
        for (const auto& l : page().layers)
            if (l.id == id) return &l;
        return nullptr;
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
};

QStringList menus_of(app::MainWindow* w, QAction* a) {
    QStringList out;
    for (QMenu* menu : w->findChildren<QMenu*>())
        if (menu->actions().contains(a)) out << menu->title();
    return out;
}

// The window's answer: the dialog of `kind` with its fields set by `fill`, accepted (any other: stopped).
std::function<int(QDialog*)> answer(const QString& kind, std::function<void(QDialog*)> fill = {}) {
    return [kind, fill](QDialog* dialog) {
        if (dialog->objectName() != QStringLiteral("adjust_dialog") || dialog->property("kind").toString() != kind) return int(QDialog::Rejected);
        if (fill) fill(dialog);
        return int(QDialog::Accepted);
    };
}

void set_number(QDialog* dialog, const char* key, double value) {
    auto* spin = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("field_%1").arg(QString::fromLatin1(key)));
    QVERIFY2(spin != nullptr, key);
    spin->setValue(value);
}

}  // namespace

class TestGuiImageOps : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { (void)gui_test::config_folder(); }

    // The commands where design 5 puts them, without keys (フェード… Photoshop's Ctrl+Shift+F in the Photoshop key set, PC5
    // H-0); フェード off until there is something to fade.
    void commands() {
        Book book;
        for (const char* name : {"act_shadows_highlights", "act_hdr_toning", "act_replace_color", "act_match_color", "act_apply_image",
                                 "act_calculations", "act_trap"}) {
            QAction* a = book.act(name);
            QVERIFY2(a != nullptr, name);
            QVERIFY2(menus_of(book.window.get(), a).contains(QStringLiteral("色調補正")), name);
            QVERIFY(a->shortcuts().isEmpty());
        }
        QAction* fade = book.act("act_fade");
        QVERIFY(fade != nullptr);
        QVERIFY(menus_of(book.window.get(), fade).contains(QStringLiteral("編集")));
        QCOMPARE(fade->shortcuts(), QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+Shift+F"))});
        QVERIFY(!fade->isEnabled());
        // 履歴's words
        QCOMPARE(app::history::describe(Json::array({Json{{"op", "fade_layer"}}})), QStringLiteral("フェードした"));
        QCOMPARE(app::history::describe(Json::array({Json{{"op", "hdr_toning"}}})), QStringLiteral("HDR トーンをかけた"));
    }

    // シャドウ・ハイライト…: its window on the layer drawn on, one op, one Undo; then フェード… (on now) takes it half back,
    // and a change that is no filter turns フェード off again.
    void shadowsHighlightsThenFade() {
        Book book;
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        const core::Bytes before = book.layer("pic")->raster_png;
        answers.responder->exec = answer(QStringLiteral("shadows_highlights"), [](QDialog* d) { set_number(d, "shadow_amount", 80); });
        book.act("act_shadows_highlights")->trigger();
        const core::Bytes filtered = book.layer("pic")->raster_png;
        QVERIFY(filtered != before);
        QVERIFY(book.act("act_fade")->isEnabled());
        // フェード 0 %: the pixels before, their very bytes
        answers.responder->exec = answer(QStringLiteral("fade_layer"), [](QDialog* d) { set_number(d, "opacity", 0); });
        book.act("act_fade")->trigger();
        QCOMPARE(*book.layer("pic")->raster_png, *before);
        QVERIFY(!book.act("act_fade")->isEnabled());  // (a fade is no filter: nothing to fade now)
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QCOMPARE(*book.layer("pic")->raster_png, *filtered);
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QCOMPARE(*book.layer("pic")->raster_png, *before);
        QVERIFY(!book.act("act_fade")->isEnabled());
        // stopped: nothing changes
        answers.responder->exec = [](QDialog*) { return int(QDialog::Rejected); };
        book.act("act_shadows_highlights")->trigger();
        QCOMPARE(*book.layer("pic")->raster_png, *before);
    }

    // Independent review B (E): on a precise layer (a page of 16 bits) フェード… hands the op the pixels before as the
    // book saves them — its tiles —, so a layer of any size fades; 0 %: the pixels before, their very bytes. The refusals
    // of a work too large (C) and of pixels before too large to read whole (E) are said in plain words.
    void fadeOfAPreciseLayer() {
        const core::Document deep = core::CommandBus(render::ops_registry())
                                        .apply(book_doc(), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}),
                                               core::Actor("human:tester"))
                                        .doc;
        Book book(deep);
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        QVERIFY(book.layer("pic")->color_raster);
        const core::Bytes before = book.layer("pic")->color_raster;
        answers.responder->exec = answer(QStringLiteral("shadows_highlights"), [](QDialog* d) { set_number(d, "shadow_amount", 80); });
        book.act("act_shadows_highlights")->trigger();
        QVERIFY(*book.layer("pic")->color_raster != *before);
        QVERIFY(book.act("act_fade")->isEnabled());
        answers.responder->exec = answer(QStringLiteral("fade_layer"), [](QDialog* d) { set_number(d, "opacity", 0); });
        book.act("act_fade")->trigger();
        QCOMPARE(*book.layer("pic")->color_raster, *before);
        const Json sent = book.session->history().done.back().ops.at(0);
        QCOMPARE(sent["op"], Json("fade_layer"));
        QVERIFY2(sent["from"].is_object() && sent["from"].contains("tiles"), sent.dump().c_str());
        const QString large = wording::error(QStringLiteral("ops[0] filter_raster: the pixels are too large to work on at once (past the memory the work may use)"));
        QVERIFY2(large.contains(QStringLiteral("大きすぎ")), qPrintable(large));
        const QString whole = wording::error(QStringLiteral("ops[0] fade_layer: the pixels before are too large to read whole (at most 256 MiB; give a precise "
                                                            "layer's tiles)"));
        QVERIFY2(whole.contains(QStringLiteral("大きすぎ")), qPrintable(whole));
    }

    // 色の置き換え… (the main colour by default), 色の一致… (its sources: none, the page merged, the page's layers, the
    // other page of its size), 画像操作… — each one op.
    void colourWindows() {
        Book book;
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        book.window->pen().rgb = {220, 30, 30};
        const core::Bytes before = book.layer("pic")->raster_png;
        answers.responder->exec = answer(QStringLiteral("replace_color"), [](QDialog* d) { set_number(d, "hue", 120); });
        book.act("act_replace_color")->trigger();
        QVERIFY(book.layer("pic")->raster_png != before);
        int sources = 0;
        answers.responder->exec = answer(QStringLiteral("match_color"), [&sources](QDialog* d) {
            auto* combo = d->findChild<QComboBox*>(QStringLiteral("field__match"));
            QVERIFY(combo != nullptr);
            sources = combo->count();
            combo->setCurrentIndex(1);  // (this page merged)
            set_number(d, "intensity", 50);
        });
        const core::Bytes replaced = book.layer("pic")->raster_png;
        book.act("act_match_color")->trigger();
        QVERIFY(sources >= 3 + static_cast<int>(book.page().layers.size()) - 1);
        QVERIFY(book.layer("pic")->raster_png != replaced);
        answers.responder->exec = answer(QStringLiteral("apply_image"), [](QDialog* d) {
            auto* blend = d->findChild<QComboBox*>(QStringLiteral("field_blending"));
            QVERIFY(blend != nullptr);
            for (int i = 0; i < blend->count(); ++i) QVERIFY(blend->itemText(i) != QStringLiteral("色相"));  // (separable only)
            blend->setCurrentIndex(blend->findText(QStringLiteral("スクリーン")));
        });
        const core::Bytes matched = book.layer("pic")->raster_png;
        book.act("act_apply_image")->trigger();
        QVERIFY(book.layer("pic")->raster_png != matched);
        QVERIFY(book.act("act_fade")->isEnabled());  // (画像操作 is faded as a filter is)
    }

    // HDR トーン… asks before it flattens two layers (no: nothing done); yes: one paint layer left, toned.
    void hdrToningAsksFirst() {
        Book book;
        gui_test::Answers answers;
        answers.responder->exec = answer(QStringLiteral("hdr_toning"));
        const std::size_t layers = book.page().layers.size();
        book.act("act_hdr_toning")->trigger();
        QCOMPARE(book.page().layers.size(), layers);
        QVERIFY(answers.asked.join(QStringLiteral("\n")).contains(QStringLiteral("統合")));
        answers.responder->question = [](const QString&, const QString&) { return true; };
        book.act("act_hdr_toning")->trigger();
        QVERIFY(book.page().layers.size() < layers);
        QVERIFY(book.layer("two") == nullptr || book.layer("pic") == nullptr);
    }

    // 演算… into a selection: the canvas's selection set, the book as it was; into a channel: a new alpha channel.
    void calculationsIntoASelection() {
        Book book;
        gui_test::Answers answers;
        const std::int64_t revision = book.session->document().revision;
        answers.responder->exec = answer(QStringLiteral("calculations"), [](QDialog* d) {
            auto* result = d->findChild<QComboBox*>(QStringLiteral("field_result"));
            QVERIFY(result != nullptr);
            result->setCurrentIndex(2);  // (選択範囲)
            auto* source = d->findChild<QComboBox*>(QStringLiteral("field_source1"));
            source->setCurrentIndex(source->findText(QStringLiteral("レイヤー: "), Qt::MatchStartsWith));
            auto* blend = d->findChild<QComboBox*>(QStringLiteral("field_blending"));
            blend->setCurrentIndex(blend->findText(QStringLiteral("スクリーン")));
        });
        book.act("act_calculations")->trigger();
        QVERIFY(book.window->canvas()->selection().has_value());
        QCOMPARE(book.session->document().revision, revision);
        QVERIFY(book.page().channels.empty());
        answers.responder->exec = answer(QStringLiteral("calculations"));
        book.act("act_calculations")->trigger();
        QCOMPARE(book.page().channels.size(), std::size_t(1));
    }

    // トラッピング… on an RGB page: said, nothing done; on a CMYK page: the op.
    void trapOnCmykOnly() {
        Book book;
        gui_test::Answers answers;
        book.window->set_target_layer("pic");
        int asked = 0;
        answers.responder->exec = [&asked](QDialog* d) {
            ++asked;
            return answer(QStringLiteral("trap"))(d);
        };
        const core::Bytes before = book.layer("pic")->raster_png;
        book.act("act_trap")->trigger();
        QCOMPARE(asked, 0);
        QVERIFY(book.window->last_notice().contains(QStringLiteral("CMYK")));
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}})));
        book.window->set_target_layer("pic");
        const core::Bytes inks = book.layer("pic")->ink_raster;
        QVERIFY(inks != nullptr);
        book.act("act_trap")->trigger();
        QCOMPARE(asked, 1);
        QVERIFY(book.window->last_error().isEmpty() || !book.window->last_error().contains(QStringLiteral("トラッピング")));
        (void)before;
    }
};

QTEST_MAIN(TestGuiImageOps)
#include "test_gui_image_ops.moc"
