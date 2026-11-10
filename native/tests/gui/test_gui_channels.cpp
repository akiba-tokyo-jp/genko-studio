// チャンネル (channels@1, SPEC.md COLOR-03; PhotoCraft's Channels panel) in the window, on the offscreen platform: the panel
// beside the layers (its rows — the composite, the plates, the channels, the kept areas —, their eyes, the row clicked
// made the target), the target keys Ctrl+Alt+2…9, a fill with an alpha channel targeted changing that channel alone, the
// view on screen (a plate alone, a channel laid over), the options, split and merge spot from the panel's menu.
#include <QtTest>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include "gui_support.hpp"
#include "app/canvas.hpp"
#include "app/channel_panel.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/thumbs.hpp"
#include "app/tiles.hpp"
#include "core/base64.hpp"
#include "core/channels.hpp"
#include "core/command_bus.hpp"
#include "core/ink.hpp"
#include "render/channels.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
using namespace genko;
using core::Json;
namespace inject = genko::app::inject;

namespace {

// A book with one colour page (60 × 60 mm, no panels) and a paint layer of one colour over it.
core::Document book_doc() {
    auto doc = core::new_episode("チャンネル", core::Num(1), 2, core::PageSpec::custom(60, 60, 50, 50, 1, 2, 2, 2, 2, 72, "color"));
    for (std::size_t i = 0; i < doc.pages.size(); ++i) doc.edit_page(i).frames.clear();
    core::Layer paint;
    paint.id = "paint0000001";
    paint.kind = core::LayerKind::Raster;
    paint.title = "絵";
    paint.panel_clip = false;
    paint.raster_png = std::make_shared<const std::string>(
        render::write_png(render::Image::create("RGBA", render::Size{472, 472}, render::Ink{200, 150, 100, 255})));
    doc.edit_page(0).layers.push_back(std::move(paint));
    return doc;
}

struct Studio {
    QTemporaryDir tmp;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    Studio() {
        (void)gui_test::config_folder();
        const auto path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, book_doc());
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1200, 860);
        window->show();
        window->canvas()->fit_page();
        window->set_target_layer("paint0000001");
    }
    app::ChannelPanel* panel() const { return window->channel_panel(); }
    const core::Page& page() const { return session->document().page(0); }
    app::PageRenderer::ChannelView shown() const { return window->canvas()->renderer().channel_view(); }
    QListWidgetItem* row(const std::string& key) const {
        const auto& keys = panel()->keys();
        const auto it = std::find(keys.begin(), keys.end(), key);
        return it == keys.end() ? nullptr : panel()->list()->item(static_cast<int>(it - keys.begin()));
    }
    std::string pixels() const {
        for (const auto& l : page().layers) {
            if (l.id == "paint0000001") return render::open_image(*l.raster_png).convert("RGBA").tobytes();
        }
        return {};
    }
};

}  // namespace

class TestGuiChannels : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<genko::app::StrokeInput>(); }

    // The dock beside the layers (hidden until asked for, 表示 → チャンネル); its rows: the composite, the plates, a new
    // alpha channel (made the target and shown alone, as PhotoCraft's), a spot channel, a kept area; a click makes a row
    // the target; the eyes show and hide.
    void panelRowsAndTargets() {
        Studio studio;
        QDockWidget* dock = studio.window->channels_dock();
        QVERIFY(dock != nullptr);
        QCOMPARE(dock->objectName(), QStringLiteral("チャンネル"));
        QVERIFY(!dock->isVisible());
        dock->show();
        QVERIFY(studio.panel()->isVisible());
        QCOMPARE(studio.panel()->keys(), (std::vector<std::string>{"composite", "red", "green", "blue"}));
        QCOMPARE(studio.row("red")->text(), QStringLiteral("レッド"));
        studio.panel()->button(QStringLiteral("channel_new"))->click();
        QCOMPARE(studio.page().channels.size(), std::size_t{1});
        const std::string alpha = studio.page().channels[0].id;
        QCOMPARE(studio.panel()->keys().back(), alpha);
        QCOMPARE(studio.row(alpha)->text(), QStringLiteral("Alpha 1"));
        QCOMPARE(studio.window->channel_view().target, alpha);
        QCOMPARE(studio.shown().planes, (std::optional<std::vector<bool>>(std::vector<bool>{false, false, false})));
        QCOMPARE(studio.shown().overlays, (std::vector<std::string>{alpha}));
        QCOMPARE(studio.panel()->list()->currentItem(), studio.row(alpha));
        // the composite clicked: every plate back, the channel still laid over
        studio.panel()->list()->itemClicked(studio.row("composite"));
        QCOMPARE(studio.window->channel_view().target, std::string());
        QVERIFY(!studio.shown().planes);
        QCOMPARE(studio.shown().overlays, (std::vector<std::string>{alpha}));
        // its eye off: not laid over
        studio.row(alpha)->setCheckState(Qt::Unchecked);
        QVERIFY(studio.shown().overlays.empty());
        // a plate clicked: it alone, in grey
        studio.panel()->list()->itemClicked(studio.row("green"));
        QCOMPARE(studio.window->channel_view().target, std::string("green"));
        QCOMPARE(studio.shown().planes, (std::optional<std::vector<bool>>(std::vector<bool>{false, true, false})));
        // a spot channel: printed, its eye takes it away
        studio.panel()->button(QStringLiteral("channel_spot"))->click();
        const std::string spot = studio.page().channels[1].id;
        QCOMPARE(studio.row(spot)->text(), QStringLiteral("特色 1（特色）"));
        QVERIFY(!studio.shown().spots);
        studio.row(spot)->setCheckState(Qt::Unchecked);
        QCOMPARE(studio.shown().spots, (std::optional<std::vector<std::string>>(std::vector<std::string>{})));
        // a kept area: a row of its own
        studio.window->apply_ops(Json::array({Json{{"op", "store_area"}, {"page", 1}, {"name", "空"},
                                                   {"area", Json{{"poly", Json::array({Json::array({0, 0}), Json::array({30, 0}), Json::array({30, 30})})}}}}}));
        QCOMPARE(studio.panel()->keys().back(), std::string("stock:空"));
        studio.panel()->list()->itemClicked(studio.row("stock:空"));
        QVERIFY(studio.window->selection_area().has_value());
        // another page: its own view
        studio.window->go_to_page(2);
        QVERIFY(gui_test::wait_for([&] { return studio.window->page_index() == 1; }));
        QCOMPARE(studio.window->channel_view().target, std::string());
        QCOMPARE(studio.panel()->keys(), (std::vector<std::string>{"composite", "red", "green", "blue"}));
    }

    // Ctrl+2…9 in the Photoshop key set (PC5 H-0, the user's decision 8; PhotoCraft's ⌘2…⌘9 — Ctrl+Alt+2…9 in the Genko set,
    // where Ctrl+2 is 描画定規にスナップ): 2 the composite, 3… the plates, then the channels;
    // a slot with nothing said so.
    void targetKeys() {
        Studio studio;
        studio.window->apply_ops(Json::array({Json{{"op", "add_channel"}, {"page", 1}}}));
        const std::string alpha = studio.page().channels[0].id;
        QCOMPARE(studio.window->action(QStringLiteral("act_channel_composite"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+2")));
        for (int slot = 3; slot <= 9; ++slot) {
            QCOMPARE(studio.window->action(QStringLiteral("act_channel_%1").arg(slot))->shortcut(), QKeySequence(QStringLiteral("Ctrl+%1").arg(slot)));
        }
        QVERIFY(studio.window->action(QStringLiteral("act_snap"))->shortcut().isEmpty());  // (PC5 H-0: Photoshop's Ctrl+2…9)
        studio.window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(studio.window.get()));
        QTest::keyClick(studio.window->canvas(), Qt::Key_3, Qt::ControlModifier);
        QCOMPARE(studio.window->channel_view().target, std::string("red"));
        QCOMPARE(studio.shown().planes, (std::optional<std::vector<bool>>(std::vector<bool>{true, false, false})));
        QTest::keyClick(studio.window->canvas(), Qt::Key_6, Qt::ControlModifier);
        QCOMPARE(studio.window->channel_view().target, alpha);
        QTest::keyClick(studio.window->canvas(), Qt::Key_2, Qt::ControlModifier);
        QCOMPARE(studio.window->channel_view().target, std::string());
        studio.window->action(QStringLiteral("act_channel_9"))->trigger();
        QCOMPARE(studio.window->channel_view().target, std::string());
    }

    // A fill with an alpha channel targeted changes that channel alone (the paint layer as it was); with the red plate
    // targeted only the layer's red; with the composite the layer.
    void fillOnTheChannel() {
        Studio studio;
        studio.window->apply_ops(Json::array({Json{{"op", "add_channel"}, {"page", 1}}}));
        const std::string alpha = studio.page().channels[0].id;
        const std::string before = studio.pixels();
        studio.window->target_channel(alpha);
        studio.window->pen().rgb = {255, 255, 255};
        studio.window->canvas()->set_selection(Json{{"poly", Json::array({Json::array({0, 0}), Json::array({30, 0}), Json::array({30, 60}), Json::array({0, 60})})}});
        studio.window->action(QStringLiteral("act_fill_selection"))->trigger();
        QCOMPARE(studio.pixels(), before);
        const render::channels::Plane plane = render::channels::plane_of(studio.page(), studio.page().channels[0]);
        QCOMPARE(plane.values[std::size_t(100) * plane.width + 10], 1.0f);
        QCOMPARE(plane.values[std::size_t(100) * plane.width + 400], 0.0f);
        // the red plate
        studio.window->target_channel("red");
        studio.window->pen().rgb = {0, 0, 0};
        studio.window->action(QStringLiteral("act_fill_selection"))->trigger();
        const std::string red = studio.pixels();
        const std::size_t at = (std::size_t(100) * 472 + 10) * 4;
        QCOMPARE(static_cast<unsigned char>(red[at]), 0);
        QCOMPARE(red.substr(at + 1, 3), before.substr(at + 1, 3));
        // the composite: the layer itself
        studio.window->target_channel("");
        studio.window->action(QStringLiteral("act_fill_selection"))->trigger();
        const std::string all = studio.pixels();
        QVERIFY(all.empty() || all != red || !studio.page().layers.back().patches.empty());
    }

    // The panel's options (name, how it shows, colour, opacity or solidity) through set_channel; split from the menu
    // (asked first); merge spot from the menu.
    void optionsSplitAndMergeSpot() {
        Studio studio;
        studio.window->channels_dock()->show();
        studio.window->apply_ops(Json::array({Json{{"op", "add_channel"}, {"page", 1}}}));
        const std::string alpha = studio.page().channels[0].id;
        studio.panel()->refresh();
        studio.panel()->list()->setCurrentItem(studio.row(alpha));
        studio.answers.responder->exec = [](QDialog* dialog) {
            if (dialog->objectName() != QStringLiteral("channel_options")) return 0;
            dialog->findChild<QLineEdit*>(QStringLiteral("channel_name"))->setText(QStringLiteral("  髪  "));
            auto* kind = dialog->findChild<QComboBox*>(QStringLiteral("channel_kind"));
            kind->setCurrentIndex(kind->findData(QStringLiteral("spot")));
            dialog->findChild<QSpinBox*>(QStringLiteral("channel_amount"))->setValue(40);
            return static_cast<int>(QDialog::Accepted);
        };
        studio.panel()->options();
        const core::Channel& changed = studio.page().channels[0];
        QCOMPARE(changed.name, std::string("髪"));
        QVERIFY(changed.kind == core::ChannelKind::Spot);
        QCOMPARE(changed.solidity, 0.4);
        // merge spot (the menu): the page flattened, the channel gone
        QAction* merge_spot = nullptr;
        for (QAction* a : studio.panel()->menu()->actions()) if (a->text() == QStringLiteral("特色チャンネルを合成")) merge_spot = a;
        QVERIFY(merge_spot != nullptr);
        merge_spot->trigger();
        QVERIFY(studio.page().channels.empty());
        // split (asked first: no, then yes)
        QAction* split = nullptr;
        for (QAction* a : studio.panel()->menu()->actions()) if (a->text() == QStringLiteral("チャンネルを分割")) split = a;
        QVERIFY(split != nullptr);
        const std::size_t pages = studio.session->document().pages.size();
        split->trigger();
        QCOMPARE(studio.session->document().pages.size(), pages);
        studio.answers.responder->question = [](const QString&, const QString&) { return true; };
        split->trigger();
        QCOMPARE(studio.session->document().pages.size(), pages + 2);  // (three grey pages for the one)
        QVERIFY(studio.session->document().page(0).color && studio.session->document().page(0).color->space == core::InkSpace::Gray);
    }

    // The window gives the pixel ops it sends — and the previews it shows — the target: levels on a CMYK page with the
    // cyan plate targeted change the cyan alone (a CMYK page's plate is this build's "channel" there, Python having no
    // such pages); a filter previewed with an alpha channel targeted shows the channel changed and the layer as it was, as
    // the filter applied does.
    void targetForLevelsAndPreviews() {
        {
            Studio studio;
            QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}})));
            const auto inks = [&] {
                for (const auto& l : studio.page().layers) {
                    if (l.id == "paint0000001") return core::decode_ink_raster(*l.ink_raster);
                }
                return core::InkPixels{};
            };
            const core::InkPixels was = inks();
            studio.window->target_channel("cyan");
            QCOMPARE(studio.window->channel_view().target, std::string("cyan"));
            QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint0000001"}, {"kind", "levels"},
                                                                {"black", 0}, {"white", 128}, {"channel", "rgb"}}})));
            const core::InkPixels now = inks();
            QCOMPARE(now.pixel_count(), was.pixel_count());
            bool cyan = false;
            for (std::size_t i = 0; i < now.pixel_count(); i += 53) {
                cyan = cyan || now.samples[i * 5] != was.samples[i * 5];
                for (unsigned c = 1; c < 5; ++c) QCOMPARE(now.samples[i * 5 + c], was.samples[i * 5 + c]);
            }
            QVERIFY(cyan);
        }
        Studio studio;
        QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "add_channel"}, {"page", 1}}})));
        const std::string alpha = studio.page().channels[0].id;
        studio.window->target_channel(alpha);
        const std::string before = studio.pixels();
        studio.window->preview_ops(Json::array({Json{{"op", "filter_raster"}, {"page", 1}, {"id", "paint0000001"}, {"kind", "brightness_contrast"},
                                                     {"brightness", 60}, {"contrast", 0}}}));
        const core::Page* shown = studio.window->canvas()->page();
        QVERIFY(shown != nullptr);
        for (const auto& l : shown->layers) {
            if (l.id == "paint0000001") QCOMPARE(render::open_image(*l.raster_png).convert("RGBA").tobytes(), before);
        }
        QVERIFY(shown->channels.at(0).raster);  // (the channel brightened in the preview)
        studio.window->preview_ops(std::nullopt);
        QVERIFY(!studio.window->canvas()->page()->channels.at(0).raster);
    }

    // PC1d (channel.delete of a colour plate): a plate deleted from the panel asks first — the page becomes multichannel
    // (no: nothing) —, then sends delete_channel with to_multichannel; on the multichannel page a plate goes without a
    // question; the last plate stays (said); undone as any op.
    void deletePlateToMultichannel() {
        Studio studio;
        studio.window->channels_dock()->show();
        QStringList asked;
        bool yes = false;
        studio.answers.responder->question = [&](const QString& title, const QString&) {
            asked << title;
            return yes;
        };
        const auto remove = [&](const std::string& key) {
            QListWidgetItem* row = studio.row(key);
            QVERIFY2(row != nullptr, key.c_str());
            studio.panel()->list()->setCurrentItem(row);
            studio.panel()->button(QStringLiteral("channel_delete"))->click();
        };
        remove("green");
        QCOMPARE(asked, QStringList{QStringLiteral("色の版を削除")});
        QVERIFY(!studio.page().color);
        yes = true;
        remove("green");
        QVERIFY(studio.page().color && studio.page().color->space == core::InkSpace::Multi);
        QCOMPARE(core::plate_labels(studio.page()), (std::vector<std::string>{"Cyan", "Yellow"}));
        QCOMPARE(studio.panel()->keys(), (std::vector<std::string>{"composite", "cyan", "yellow"}));
        QCOMPARE(studio.row("composite")->text(), QStringLiteral("マルチチャンネル"));
        // the multichannel page's plate: no question
        asked.clear();
        remove("yellow");
        QVERIFY(asked.isEmpty());
        QCOMPARE(core::plate_labels(studio.page()), (std::vector<std::string>{"Cyan"}));
        // one plate left: the composite alone (as a grey page's), nothing to delete
        QCOMPARE(studio.panel()->keys(), (std::vector<std::string>{"composite"}));
        remove("composite");
        QCOMPARE(studio.window->last_notice(), QStringLiteral("削除するチャンネルを選びます"));
        QCOMPARE(studio.page().color->plates.size(), std::size_t{1});
        // undone: the RGB page again
        studio.window->action(QStringLiteral("act_undo"))->trigger();
        studio.window->action(QStringLiteral("act_undo"))->trigger();
        QVERIFY(!studio.page().color);
        QCOMPARE(studio.panel()->keys(), (std::vector<std::string>{"composite", "red", "green", "blue"}));
    }

    // Review C: deleting a colour plate flattens the page: the question says so in 画像をフラット化's words — the hidden
    // layer it would discard counted —; declined, nothing changes. Merging a spot channel (which flattens too) asks when
    // a hidden layer would be discarded.
    void deletePlateSaysWhatItDiscards() {
        Studio studio;
        studio.window->channels_dock()->show();
        QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "hidden000001"}},
                                                      Json{{"op", "set_layer"}, {"page", 1}, {"id", "hidden000001"}, {"panel_clip", false}},
                                                      Json{{"op", "put_raster"}, {"page", 1}, {"id", "hidden000001"},
                                                           {"png_base64", core::b64encode(render::write_png(render::Image::create("RGBA", render::Size{20, 20}, render::Ink{0, 0, 0, 255})))}},
                                                      Json{{"op", "set_layer"}, {"page", 1}, {"id", "hidden000001"}, {"visible", false}}})));
        QStringList asked;
        studio.answers.responder->question = [&](const QString& title, const QString& text) {
            asked << title + QStringLiteral(": ") + text;
            return false;
        };
        studio.panel()->refresh();
        studio.panel()->list()->setCurrentItem(studio.row("green"));
        studio.panel()->button(QStringLiteral("channel_delete"))->click();
        QCOMPARE(asked, QStringList{QStringLiteral("色の版を削除: 色の版を削除すると、このページはマルチチャンネルになります（画素レイヤーは 1 枚に統合され、"
                                                   "残りの版はそれぞれインキの版になります）。削除しますか？\n"
                                                   "非表示の作画レイヤー（1 枚）は破棄します（ネーム・下描き・コマ枠は保持）。編集 → 取り消し で元に戻せます。")});
        QVERIFY(!studio.page().color);
        // merging a spot channel flattens the page too: asked as well (declined: the channel stays), then merged
        QVERIFY(studio.window->apply_ops(Json::array({Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}}})));
        QAction* merge_spot = nullptr;
        for (QAction* a : studio.panel()->menu()->actions()) if (a->text() == QStringLiteral("特色チャンネルを合成")) merge_spot = a;
        QVERIFY(merge_spot != nullptr);
        asked.clear();
        merge_spot->trigger();
        QCOMPARE(asked, QStringList{QStringLiteral("特色チャンネルを合成: 特色を合成すると、このページの画素レイヤーを 1 枚に統合します。統合しますか？\n"
                                                   "非表示の作画レイヤー（1 枚）は破棄します（ネーム・下描き・コマ枠は保持）。編集 → 取り消し で元に戻せます。")});
        QCOMPARE(studio.page().channels.size(), std::size_t(1));
        studio.answers.responder->question = [](const QString&, const QString&) { return true; };
        merge_spot->trigger();
        QVERIFY(studio.page().channels.empty());
    }

    // The pages' small pictures follow their channels: a channel added, drawn on, its colour, opacity, solidity, kind or
    // what it marks changed gives the page another key (the cache's version raised for the pictures made before), and
    // deleted the key of the page without it; the other page's key stays.
    void thumbnailsFollowTheChannels() {
        QCOMPARE(std::string(app::kThumbVersion), std::string("genko-native-render/2"));
        const core::CommandBus bus(render::ops_registry());
        const auto run = [&](const core::Document& d, const Json& op) { return bus.apply(d, Json::array({op}), core::Actor("human:test")).doc; };
        core::Document doc = book_doc();
        std::vector<std::string> seen{app::page_fingerprint(doc, doc.page(0))};
        const std::string other = app::page_fingerprint(doc, doc.page(1));
        const auto changed = [&](const core::Document& d) {
            const std::string key = app::page_fingerprint(d, d.page(0));
            const bool fresh = std::find(seen.begin(), seen.end(), key) == seen.end();
            seen.push_back(key);
            return fresh && app::page_fingerprint(d, d.page(1)) == other;
        };
        doc = run(doc, Json{{"op", "add_channel"}, {"page", 1}, {"kind", "spot"}, {"name", "金"}, {"fill", "white"}});
        QVERIFY(changed(doc));
        const std::string spot = doc.page(0).channels[0].id;
        doc = run(doc, Json{{"op", "erase_raster"}, {"page", 1}, {"layer_id", "paint0000001"}, {"channel", spot}, {"width_mm", 5},
                            {"points", Json::array({Json::array({10, 10}), Json::array({30, 10})})}});
        QVERIFY(changed(doc));
        doc = run(doc, Json{{"op", "set_channel"}, {"page", 1}, {"channel", spot}, {"color", Json::array({0, 0, 255})}});
        QVERIFY(changed(doc));
        doc = run(doc, Json{{"op", "set_channel"}, {"page", 1}, {"channel", spot}, {"solidity", 0.5}});
        QVERIFY(changed(doc));
        doc = run(doc, Json{{"op", "set_channel"}, {"page", 1}, {"channel", spot}, {"kind", "alpha"}});
        QVERIFY(changed(doc));
        doc = run(doc, Json{{"op", "set_channel"}, {"page", 1}, {"channel", spot}, {"opacity", 0.2}});
        QVERIFY(changed(doc));
        doc = run(doc, Json{{"op", "set_channel"}, {"page", 1}, {"channel", spot}, {"indicates", "selected"}});
        QVERIFY(changed(doc));
        doc = run(doc, Json{{"op", "delete_channel"}, {"page", 1}, {"channel", spot}});
        QCOMPARE(app::page_fingerprint(doc, doc.page(0)), seen.front());  // (the page as it was before: its picture again)
    }
};

QTEST_MAIN(TestGuiChannels)
#include "test_gui_channels.moc"
