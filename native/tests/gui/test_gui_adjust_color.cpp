// PC2b (色の補正, SPEC.md COLOR-04) in the window: レイヤー → 色調補正 の 自然な彩度…・色相・彩度… (Ctrl+U)・カラーバランス…
// (Ctrl+B)・白黒…・レンズフィルター…・チャンネルミキサー…・特定色域の選択…・彩度を下げる (Ctrl+Shift+U) on the layer drawn
// on — the window's form, the page showing the result while it is open, one filter_raster (Undo) —; each kind's form
// holding its normal form; the layer panel's 色相・彩度 making the upstream form (the user's decision 3) and mending it
// in its window, its native kept.

#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>

#include "gui_support.hpp"
#include "app/adjust_forms.hpp"
#include "app/canvas.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "core/adjust.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "render/raster.hpp"

using namespace genko;
using core::Json;

namespace {

// A page with a paint layer of colours (a red, a green, a blue and a grey band).
core::Document book_doc() {
    auto doc = core::new_episode("色の補正", core::Num(1), 2, core::PageSpec::custom(40, 40, 30, 30, 1, 2, 2, 2, 2, 72, "color"));
    render::Image picture = render::Image::create("RGBA", render::Size{80, 80}, render::Ink{200, 40, 40, 255});
    picture.paste(render::Ink{40, 180, 60, 255}, render::Box{20, 0, 40, 80});
    picture.paste(render::Ink{50, 70, 210, 255}, render::Box{40, 0, 60, 80});
    picture.paste(render::Ink{128, 128, 128, 255}, render::Box{60, 0, 80, 80});
    doc = core::CommandBus(render::ops_registry()).apply(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "pic"}},
                                                     Json{{"op", "set_layer"}, {"page", 1}, {"id", "pic"}, {"panel_each", false}, {"panel_clip", false}},
                                                     Json{{"op", "put_raster"}, {"page", 1}, {"id", "pic"}, {"png_base64", core::b64encode(render::write_png(picture))}}}),
                                   core::Actor("human:tester")).doc;
    return doc;
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
        for (QDockWidget* dock : window->findChildren<QDockWidget*>())
            if (dock->objectName() == QStringLiteral("レイヤー")) dock->raise();
        QCoreApplication::processEvents();
    }
    app::LayerPanel& panel() const { return *window->layer_panel(); }
    const core::Page& page() const { return session->document().page(0); }
    const core::Layer* layer(const std::string& id) const {
        for (const auto& l : page().layers) if (l.id == id) return &l;
        return nullptr;
    }
    bool choose(const std::string& id) const {
        const auto& ids = panel().ids();
        const auto at = std::find(ids.begin(), ids.end(), id);
        if (at == ids.end()) return false;
        panel().list()->setCurrentRow(static_cast<int>(at - ids.begin()));
        return true;
    }
    std::string pixels(const std::string& id) const { return render::raster::layer_pixels(page(), *layer(id)).tobytes(); }
    // A key as a person presses it (a shortcut reaches the active window only).
    void press(Qt::Key key, Qt::KeyboardModifiers modifiers) const {
        window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window.get()));
        QTest::keyClick(window->canvas(), key, modifiers);
    }
    QAction* menu_action(QMenu* menu, const QString& text) const {
        for (QAction* a : menu->actions()) if (a->text() == text) return a;
        for (QAction* a : menu->actions())
            if (a->menu() != nullptr)
                for (QAction* b : a->menu()->actions()) if (b->text() == text) return b;
        return nullptr;
    }
};

// The names of the menus a command is in (its menu and the menus above it).
QStringList menus_of(QAction* action) {
    QStringList out;
    for (QObject* w : action->associatedObjects()) {
        auto* menu = qobject_cast<QMenu*>(w);
        while (menu != nullptr) {
            out << menu->title();
            QAction* up = menu->menuAction();
            menu = nullptr;
            for (QObject* o : up->associatedObjects())
                if (auto* m = qobject_cast<QMenu*>(o)) menu = m;
        }
    }
    return out;
}

app::adjust_forms::Form* form_in(QDialog* dialog) { return dialog->findChild<app::adjust_forms::Form*>(QStringLiteral("adjust_form")); }

// The settings a kind's window gives, as they are (its form untouched), and its normal form.
Json form_defaults(const std::string& kind) {
    gui_test::Answers answers;
    answers.responder->exec = [](QDialog*) { return int(QDialog::Accepted); };
    const auto got = app::adjust_forms::adjust_dialog(nullptr, kind, Json::object(), {});
    return got ? *got : Json();
}

}  // namespace

class TestGuiAdjustColor : public QObject {
    Q_OBJECT
private slots:
    // The commands in レイヤー → 色調補正, their keys.
    void commandsAndKeys() {
        Book book;
        const std::vector<std::pair<const char*, QString>> keys{
            {"act_adjust_vibrance", QString()},
            {"act_adjust_hue_saturation", QStringLiteral("Ctrl+U")},
            {"act_adjust_color_balance", QStringLiteral("Ctrl+B")},
            {"act_adjust_black_white", QString()},
            {"act_adjust_photo_filter", QString()},
            {"act_adjust_channel_mixer", QString()},
            {"act_adjust_selective_color", QString()},
            {"act_adjust_desaturate", QStringLiteral("Ctrl+Shift+U")},
        };
        for (const auto& [name, key] : keys) {
            QAction* a = book.window->action(QString::fromLatin1(name));
            QVERIFY2(a != nullptr, name);
            QCOMPARE(a->shortcut().toString(QKeySequence::PortableText), key);
            const QStringList where = menus_of(a);
            QVERIFY2(where.contains(QStringLiteral("色調補正")) && where.contains(QStringLiteral("レイヤー")), qPrintable(where.join(QStringLiteral(", "))));
        }
        QCOMPARE(book.window->action(QStringLiteral("act_adjust_hue_saturation"))->text(), QStringLiteral("色相・彩度…"));
        QCOMPARE(book.window->action(QStringLiteral("act_adjust_desaturate"))->text(), QStringLiteral("彩度を下げる"));
    }

    // Ctrl+U: the window, the page showing the result while it is open (the book as it was), かける — one filter_raster of
    // hue_saturation on the layer drawn on, undone in one —; やめる leaves it.
    void hueSaturationWithItsResultShown() {
        Book book;
        gui_test::Answers answers;
        QVERIFY(book.choose("pic"));
        const std::string before = book.pixels("pic");
        auto* canvas = book.window->canvas();
        bool previewed = false;
        const auto with_preview = [&](int answer) {
            return [&, answer](QDialog* dialog) {
                if (dialog->objectName() != QStringLiteral("adjust_dialog") || form_in(dialog) == nullptr) return int(QDialog::Rejected);
                form_in(dialog)->set_values(Json{{"hue", 120}, {"reds", Json{{"saturation", -50}}}});
                previewed = gui_test::wait_for([&] { return canvas->doc() != book.session->snapshot(); }, 10000);
                return answer;
            };
        };
        answers.responder->exec = with_preview(QDialog::Rejected);
        book.press(Qt::Key_U, Qt::ControlModifier);
        QVERIFY(previewed);
        QVERIFY(book.pixels("pic") == before);
        QTRY_VERIFY(canvas->doc() == book.session->snapshot());
        previewed = false;
        answers.responder->exec = with_preview(QDialog::Accepted);
        book.window->action(QStringLiteral("act_adjust_hue_saturation"))->trigger();
        QVERIFY(previewed);
        const std::string after = book.pixels("pic");
        QVERIFY(after != before);
        // the red band turned towards green (hue 120)
        QVERIFY(static_cast<unsigned char>(after[(10 * 80 + 5) * 4 + 1]) > static_cast<unsigned char>(after[(10 * 80 + 5) * 4]));
        QTRY_VERIFY(canvas->doc() == book.session->snapshot());
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QVERIFY(book.pixels("pic") == before);
    }

    // Ctrl+Shift+U: straight on, every pixel a grey; Ctrl+B through its tones' records.
    void desaturateAndColorBalance() {
        Book book;
        gui_test::Answers answers;
        QVERIFY(book.choose("pic"));
        const std::string before = book.pixels("pic");
        book.press(Qt::Key_U, Qt::ControlModifier | Qt::ShiftModifier);
        const std::string grey = book.pixels("pic");
        QVERIFY(grey != before);
        for (std::size_t i = 0; i < grey.size(); i += 4) QVERIFY(grey[i] == grey[i + 1] && grey[i] == grey[i + 2]);
        book.window->action(QStringLiteral("act_undo"))->trigger();
        QVERIFY(book.pixels("pic") == before);
        Json given;
        answers.responder->exec = [&given](QDialog* dialog) {
            auto* form = form_in(dialog);
            if (form == nullptr) return int(QDialog::Rejected);
            auto* chooser = form->findChild<QComboBox*>(QStringLiteral("record_chooser"));
            if (chooser->currentText() != QStringLiteral("中間調")) return int(QDialog::Rejected);  // (Photoshop's first)
            chooser->setCurrentIndex(0);  // シャドウ (the picture's colours are darker than the middle)
            form->findChild<QDoubleSpinBox*>(QStringLiteral("record_0"))->setValue(60);
            given = form->values();
            return int(QDialog::Accepted);
        };
        book.press(Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(given["shadows"], Json::array({60, 0, 0}));
        QCOMPARE(given["midtones"], Json::array({0, 0, 0}));
        QVERIFY(book.pixels("pic") != before);
    }

    // Each kind's form: untouched, its settings are the kind's normal form (the table's defaults); set from a layer's
    // settings and read back the same.
    void formsHoldTheNormalForm() {
        for (const char* kind : {"hue_saturation", "vibrance", "color_balance", "black_white", "photo_filter", "channel_mixer", "selective_color"}) {
            Json got = form_defaults(kind);
            QVERIFY2(got.is_object(), kind);
            got["kind"] = kind;
            QCOMPARE(core::normalize_adjust(got, nullptr), core::normalize_adjust(Json{{"kind", kind}}, nullptr));
        }
        // a layer's settings shown and given back (the ranges', the records' fields included)
        const Json now = core::normalize_adjust(Json{{"kind", "hue_saturation"}, {"hue", -30}, {"saturation", 20},
                                                     {"greens", Json{{"hue", 15}, {"lightness", -10}, {"range", Json::array({75, 105, 135, 165})}}}},
                                                nullptr);
        gui_test::Answers answers;
        answers.responder->exec = [](QDialog*) { return int(QDialog::Accepted); };
        Json back = *app::adjust_forms::adjust_dialog(nullptr, "hue_saturation", now, {});
        back["kind"] = "hue_saturation";
        QCOMPARE(core::normalize_adjust(back, nullptr), now);
        const Json mixer = core::normalize_adjust(Json{{"kind", "selective_color"}, {"method", "absolute"}, {"neutrals", Json::array({5, -5, 10, 20})}}, nullptr);
        back = *app::adjust_forms::adjust_dialog(nullptr, "selective_color", mixer, {});
        back["kind"] = "selective_color";
        QCOMPARE(core::normalize_adjust(back, nullptr), mixer);
    }

    // The forms' own parts: a colorize's numbers 0..360 and 0..100; a photo filter's preset sets its colour (another
    // colour shows カスタム); a monochrome mixer shows its grey row; the hue band's slider dragged.
    void formParts() {
        gui_test::Answers answers;
        Json given;
        answers.responder->exec = [&given](QDialog* dialog) {
            auto* form = form_in(dialog);
            auto* colorize = form->findChild<QCheckBox*>(QStringLiteral("field_colorize"));
            auto* hue = form->findChild<QDoubleSpinBox*>(QStringLiteral("field_hue"));
            colorize->setChecked(true);
            if (hue->maximum() != 360 || hue->minimum() != 0) return int(QDialog::Rejected);
            hue->setValue(300);
            // the reds' band: its second slider dragged right
            auto* band = form->findChild<QWidget*>(QStringLiteral("hue_band"));
            colorize->setChecked(false);  // (the ranges back)
            hue->setValue(10);
            band->resize(372, 34);
            const double x = 6 + (-15.0 + 180) / 360 * (372 - 12);  // (the reds round 0: the band from −180°)
            const auto send = [band](QEvent::Type type, double at, Qt::MouseButtons buttons) {
                QMouseEvent event(type, QPointF(at, 30), band->mapToGlobal(QPointF(at, 30)), Qt::LeftButton, buttons, Qt::NoModifier);
                QCoreApplication::sendEvent(band, &event);
            };
            send(QEvent::MouseButtonPress, x, Qt::LeftButton);
            send(QEvent::MouseMove, x + 10, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, x + 10, Qt::NoButton);
            given = form->values();
            return int(QDialog::Accepted);
        };
        QVERIFY(app::adjust_forms::adjust_dialog(nullptr, "hue_saturation", Json::object(), {}));
        QCOMPARE(given["hue"], Json(10));
        QCOMPARE(given["colorize"], Json(false));
        QCOMPARE(given["reds"]["range"], Json::array({-45, -5, 15, 45}));
        // photo filter's presets
        answers.responder->exec = [&given](QDialog* dialog) {
            auto* form = form_in(dialog);
            auto* presets = form->findChild<QComboBox*>(QStringLiteral("photo_filter_preset"));
            if (presets->currentData().toString() != QStringLiteral("warming85")) return int(QDialog::Rejected);  // (the default colour's)
            const int cooling = presets->findData(QStringLiteral("cooling80"));
            presets->setCurrentIndex(cooling);
            emit presets->activated(cooling);
            given = form->values();
            Json other = given;
            other["color"] = Json::array({1, 2, 3});
            form->set_values(other);
            if (!presets->currentData().toString().isEmpty()) return int(QDialog::Rejected);  // (カスタム)
            return int(QDialog::Accepted);
        };
        QVERIFY(app::adjust_forms::adjust_dialog(nullptr, "photo_filter", Json::object(), {}));
        QCOMPARE(given["color"], Json::array({0, 109, 255}));
        // channel mixer: monochrome shows the grey row
        answers.responder->exec = [](QDialog* dialog) {
            auto* form = form_in(dialog);
            form->findChild<QCheckBox*>(QStringLiteral("field_monochrome"))->setChecked(true);
            auto* chooser = form->findChild<QComboBox*>(QStringLiteral("record_chooser"));
            const bool grey = chooser->currentIndex() == 3 && form->findChild<QDoubleSpinBox*>(QStringLiteral("record_0"))->value() == 40;
            return int(grey ? QDialog::Accepted : QDialog::Rejected);
        };
        QVERIFY(app::adjust_forms::adjust_dialog(nullptr, "channel_mixer", Json::object(), {}));
    }

    // The layer panel (the user's decision 3): 色調補正のレイヤー → 色相・彩度・明度… makes hue_saturation; the C++ build's own
    // kinds are there; a double click mends one in its window, its native kept. A layer of Python's hue is mended in its
    // own window still.
    void correctionLayers() {
        Book book;
        gui_test::Answers answers;
        auto& panel = book.panel();
        QVERIFY(book.choose("pic"));
        answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("adjust_dialog")) return int(QDialog::Rejected);
            form_in(dialog)->set_values(Json{{"saturation", -40}});
            return int(QDialog::Accepted);
        };
        QAction* hue = book.menu_action(panel.special_menu(), QStringLiteral("色相・彩度・明度…"));
        QVERIFY(hue != nullptr);
        hue->trigger();
        const core::Layer* made = book.window->target_layer();
        QCOMPARE(made->kind, core::LayerKind::Adjust);
        QCOMPARE((*made->adjust)["kind"], Json("hue_saturation"));
        QCOMPARE((*made->adjust)["saturation"], Json(-40));
        QCOMPARE(made->title, std::string("色相・彩度"));
        for (const QString& title : {QStringLiteral("自然な彩度…"), QStringLiteral("カラーバランス…"), QStringLiteral("白黒…"), QStringLiteral("レンズフィルター…"),
                                     QStringLiteral("チャンネルミキサー…"), QStringLiteral("特定色域の選択…")}) {
            QVERIFY2(book.menu_action(panel.special_menu(), title) != nullptr, qPrintable(title));
        }
        QVERIFY(book.menu_action(panel.special_menu(), QStringLiteral("色相・彩度…")) == nullptr);  // (once: in Python's place)
        // mended in its window, its native kept
        const std::string id = made->id;
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"id", id},
                                                         {"adjust", Json{{"kind", "hue_saturation"}, {"saturation", -40}, {"native", Json{{"blend", "multiply"}}}}}}})));
        QVERIFY(book.choose(id));
        answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("adjust_dialog")) return int(QDialog::Rejected);
            if (form_in(dialog)->values()["saturation"] != Json(-40)) return int(QDialog::Rejected);
            Json now = form_in(dialog)->values();
            now["lightness"] = 20;
            form_in(dialog)->set_values(now);
            return int(QDialog::Accepted);
        };
        panel.edit_special();
        QCOMPARE((*book.layer(id)->adjust)["lightness"], Json(20));
        QCOMPARE((*book.layer(id)->adjust)["native"], (Json{{"blend", "multiply"}}));
        // Python's hue: its own window (filter_params)
        QVERIFY(book.window->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "adjust"}, {"id", "py"},
                                                         {"adjust", Json{{"kind", "hue"}, {"shift", 30}}}}})));
        QVERIFY(book.choose("py"));
        QString asked;
        answers.responder->exec = [&asked](QDialog* dialog) {
            asked = dialog->objectName();
            return int(QDialog::Rejected);
        };
        panel.edit_special();
        QCOMPARE(asked, QStringLiteral("filter_dialog"));
        QCOMPARE((*book.layer("py")->adjust)["kind"], Json("hue"));
    }
};

QTEST_MAIN(TestGuiAdjustColor)
#include "test_gui_adjust_color.moc"
