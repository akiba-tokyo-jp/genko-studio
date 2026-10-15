// Checks of Python's tests that no other C++ test made (the ledger's tport-core-c list), ported one slot per Python
// test, with the same inputs and the same assertions in substance: tests/test_h1.py (only the pages a change touches
// are copied, strokes read again shared, a small picture's lines, a page not seen lately shown roughly then
// properly), tests/test_m7_session.py (the app's actor is a person), tests/test_w1_panels.py (one's own layouts on
// other paper, a page of drawn panels cleared, a layout taken away), tests/test_w1_panels_gui.py (panels drawn corner
// by corner and freehand, the panel-by-panel switch of a layer), tests/test_w9_view.py (the zoom typed in, the view
// upside down, the magnifier's rectangle, the pivot and a mesh warp on the canvas), tests/test_h5.py (materials, fonts
// and a book's own brushes listed, one layer rendered alone), tests/test_j12.py (the onion skin's colours and the
// light table, camera work between keys, numbered frames of a chosen width), tests/test_w10_rulers.py (a selection
// from an ellipse ruler, a polygon ruler's repeated click dropped), tests/test_w12_figure.py (one's own pose chosen
// in the 3D panel), tests/test_w2_lines.py and tests/test_w3_fill.py (what the brush panel sends with a line and a
// fill), tests/test_w7_tone.py (a layer's screen of an unknown dot shape refused), tests/test_w8_filters.py (the
// curve editor, the levels dialog's histogram), tests/test_j10.py (import_psd refused by the name gate),
// tests/test_w15_color.py (a brush mixing with the colour under it, the stops editor and the gradient dialogs).
//
// DEPS genko_app genko_test_support GUI

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gui_support.hpp"
#include "testsupport.hpp"
#include "app/brush_panel.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/guide_panel.hpp"
#include "app/inject.hpp"
#include "app/layer_dialogs.hpp"
#include "app/main_window.hpp"
#include "app/session.hpp"
#include "app/templates.hpp"
#include "app/tiles.hpp"
#include "core/anim.hpp"
#include "core/brushes.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/poses.hpp"
#include "core/pynum.hpp"
#include "core/pyrandom.hpp"
#include "formats/export.hpp"
#include "render/anim.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "service/book.hpp"
#include "service/layout.hpp"
#include "service/studio_service.hpp"
#include "storage/reader.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;
namespace inject = genko::app::inject;

namespace {

// The ops of this build, as the app and the command line apply them (Python's apply_ops).
core::Document run(const core::Document& doc, const Json& ops, const std::string& actor = "genko") {
    static const core::CommandBus bus(render::ops_registry());
    return bus.apply(doc, ops, core::Actor(actor)).doc;
}

// The words of the ApplyError a batch is refused with ("" when it is applied).
std::string refusal(const core::Document& doc, const Json& ops, const std::string& actor = "genko") {
    try {
        run(doc, ops, actor);
    } catch (const core::ApplyError& error) {
        return std::string("ApplyError: ") + error.what();
    }
    return {};
}

// tests/test_h1.py::_thick: B4, `pages` pages of `lines` ink lines of 20 points, pressure 0.7, random.Random(1).
core::Document thick(int pages, int lines) {
    core::PyRandom rng(1);
    core::Document doc = core::new_episode("厚い本", core::Num(1), pages, core::PageSpec::b4_comic());
    for (std::size_t p = 0; p < doc.pages.size(); ++p) {
        core::Page& page = doc.edit_page(p);
        for (auto& layer : page.layers) {
            if (layer.role != core::LayerRole::Ink) continue;
            std::vector<core::StrokePtr> items;
            for (int n = 0; n < lines; ++n) {
                const double x = core::py_round(rng.uniform(20, 230), 3);
                const double y = core::py_round(rng.uniform(20, 340), 3);
                auto stroke = std::make_shared<core::Stroke>();
                stroke->id = core::new_id();
                for (int i = 0; i < 20; ++i) stroke->points.push_back({core::py_round(x + i * 1.2, 3), core::py_round(y + rng.uniform(-2, 2), 3)});
                stroke->pressure.assign(20, 0.7);
                items.push_back(stroke);
            }
            layer.strokes = core::make_strokes(std::move(items));
        }
    }
    return doc;
}

core::Document b5(int pages = 1) { return core::new_episode("t", core::Num(1), pages, core::PageSpec::b5_doujin()); }

const core::Layer* layer_by_id(const core::Page& page, const std::string& id) {
    for (const core::Layer& layer : page.layers) {
        if (layer.id == id) return &layer;
    }
    return nullptr;
}

bool approx(double value, double want, double within = 1e-6) { return std::abs(value - want) <= within; }
bool approx(const QPointF& p, double x, double y, double within = 1e-6) { return approx(p.x(), x, within) && approx(p.y(), y, within); }
bool approx(const Json& value, double want, double within = 1e-6) { return value.is_number() && approx(value.get<double>(), want, within); }

// A book on disk in a window (the window's size as Python's test has it), and the ops of each change made.
struct Desk {
    QTemporaryDir tmp;
    gui_test::Answers answers;
    fs::path book;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    std::vector<Json> ops;

    explicit Desk(const core::Document& doc, QSize size = QSize(1280, 860)) {
        (void)gui_test::config_folder();
        book = gui_test::path_of(tmp.path() + QStringLiteral("/book.genko"));
        gui_test::write_book(book, doc);
        session = app::Session::open(book, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery"))));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(size);
        window->show();
        canvas()->fit_page();
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit)
                for (const Json& op : c.ops) ops.push_back(op);
        });
    }
    ~Desk() {
        window.reset();
        session.reset();
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    const core::Page& page() const { return *window->current_page(); }
    std::size_t leaves() const { return page().leaf_frames().size(); }
    void trigger(const char* name) const {
        QAction* action = window->action(QString::fromLatin1(name));
        QVERIFY2(action != nullptr, name);
        action->trigger();
    }
    const Json* prim(const std::string& id) const {
        for (const Json& p : page().prims) {
            if (p.is_object() && p.value("id", std::string()) == id) return &p;
        }
        return nullptr;
    }
    // A drag with the left button through the points (page mm), as the panel tool gets it.
    void drag(const std::vector<QPointF>& points) const {
        inject::mouse(canvas(), inject::Phase::Press, points.front());
        for (std::size_t i = 1; i < points.size(); ++i) inject::mouse(canvas(), inject::Phase::Move, points[i]);
        inject::mouse(canvas(), inject::Phase::Release, points.back());
    }
};

// The book an agent works on (tests/test_h5.py's fixture): two pages made by ai:hermes, page 1's name approved by a
// person.
std::unique_ptr<service::StudioService> demo_agent(const fs::path& root) {
    auto agent = std::make_unique<service::StudioService>(root, "ai:hermes");
    if (!agent->call("create_project", Json::array({"demo.genko", "demo", 2}), Json::object()).ok) return nullptr;
    {
        genko::service::Session person(agent->project_path(Json("demo.genko")), "human:leaf");
        person.apply(Json::array({Json{{"op", "name_ok"}, {"page", 1}}}), "human:leaf");
        person.save("human:leaf");
    }
    return agent;
}

// tests/test_j12.py::_ball and _animated: a ball in three places (cels a, b and c shown two frames each) over a
// background line.
Json ball(int x, const std::string& cel) {
    return Json{{"op", "add_stroke"},
                {"page", 1},
                {"layer_id", cel},
                {"points", Json::array({Json::array({x, 100}), Json::array({x + 20, 100}), Json::array({x + 20, 120}), Json::array({x, 120}),
                                        Json::array({x, 100})})},
                {"width_mm", 3}};
}

core::Document animated(int frames = 6) {
    core::Document doc = b5();
    doc.edit_page(0).name_ok = true;
    doc = run(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"}, {"points", Json::parse("[[20, 200], [160, 200]]")}},
                                Json{{"op", "set_animation"}, {"page", 1}, {"fps", 8}, {"frames", frames}},
                                Json{{"op", "add_anim_folder"}, {"page", 1}, {"id", "ball"}, {"name", "ボール"}},
                                Json{{"op", "add_cel"}, {"page", 1}, {"folder", "ball"}, {"id", "a"}, {"name", "A"}},
                                Json{{"op", "add_cel"}, {"page", 1}, {"folder", "ball"}, {"id", "b"}, {"name", "B"}, {"at", 3}},
                                Json{{"op", "add_cel"}, {"page", 1}, {"folder", "ball"}, {"id", "c"}, {"name", "C"}, {"at", 5}}}));
    for (const auto& [cel, x] : {std::pair<std::string, int>{"a", 30}, {"b", 80}, {"c", 130}}) doc = run(doc, Json::array({ball(x, cel)}));
    return doc;
}

// A ready-made row of colours chosen in a stops editor's list (見本から選ぶ…).
void choose_preset(app::StopsEditor* editor, const QString& name) {
    auto* presets = editor->findChild<QComboBox*>();
    QVERIFY(presets != nullptr);
    const int at = presets->findData(name);
    QVERIFY(at >= 0);
    presets->setCurrentIndex(at);
    emit presets->activated(at);
}

}  // namespace

class TestGuiLedgerC : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        (void)gui_test::config_folder();
        qRegisterMetaType<genko::app::BookChange>();
    }

    // --- tests/test_h1.py ------------------------------------------------------------------------------------------

    // Python's test_deleting_a_line_leaves_other_pages_alone: deleting a line of dialogue copies no page but its own
    // (the other pages stay shared with the book before it), and the line is gone from its page.
    void deletingALineLeavesOtherPagesAlone() {
        const core::Document doc = run(thick(4, 5), Json::array({Json{{"op", "add_line"}, {"page", 2}, {"text", "消す"}, {"id", "L1"}, {"x_mm", 40},
                                                                      {"y_mm", 40}, {"w_mm", 20}, {"h_mm", 30}}}));
        QVERIFY(!doc.story_for_page(core::Num(2)).empty());
        const core::Document after = run(doc, Json::array({Json{{"op", "delete_line"}, {"id", "L1"}}}));
        for (const std::size_t i : {std::size_t{0}, std::size_t{2}, std::size_t{3}}) {
            QVERIFY2(after.pages[i].get() == doc.pages[i].get(), qPrintable(QStringLiteral("page %1 was copied").arg(i + 1)));
        }
        QVERIFY(after.story_for_page(core::Num(2)).empty());
    }

    // Python's test_reading_a_saved_layer_again_reuses_the_strokes: a book read twice shares the strokes of a layer
    // it read before. Python keeps one cache of read blobs for the process (blobcache); this build shares what one
    // folder's loads read through the LoadCache they are given (storage::LoadOptions::cache), so the two reads here
    // are given the same one. (Python also checks that the two layers' lists are their own: a Python list can be
    // changed in place; a C++ StrokeList never is, so sharing it is safe.)
    void readingASavedLayerAgainReusesTheStrokes() {
        QTemporaryDir tmp;
        const fs::path book = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        gui_test::write_book(book, thick(2, 10));
        storage::LoadOptions options;
        options.cache = std::make_shared<storage::LoadCache>();
        const core::Document first = storage::load_document(book, options).document;
        const core::Document second = storage::load_document(book, options).document;
        const core::Layer* a = gui_test::ink_of(first.page(0));
        const core::Layer* b = gui_test::ink_of(second.page(0));
        QVERIFY(a != nullptr && b != nullptr);
        QCOMPARE(a->stroke_count(), std::size_t{10});
        QVERIFY(a->strokes->items[0].get() == b->strokes->items[0].get());
    }

    // Python's test_small_pictures_draw_lines_plainly (without its time bound, which belongs to the perf tests): a
    // page of 200 lines drawn at 12 dpi for proof still shows its lines (something darker than mid grey).
    void smallPicturesDrawLinesPlainly() {
        const core::Document doc = thick(1, 200);
        const render::Image image = render::render_page(doc.page(0), 12, render::proof_options(), &doc).image;
        QVERIFY(image.convert("L").getextrema()[0].first < 128);
    }

    // Python's test_a_page_not_seen_lately_shows_roughly_then_properly: with the remembered lines forgotten, turning to
    // a page of 400 lines shows it first as its rough picture (plain polylines: the first picture up is not the page
    // as it is) and then as it really is.
    void aPageNotSeenLatelyShowsRoughlyThenProperly() {
        Desk s(thick(3, 400), QSize(1280, 800));
        app::PageCanvas* canvas = s.canvas();
        QVERIFY(canvas->wait_rendered(60000));
        render::clear_render_caches();
        bool shown = false;
        bool rough_first = false;
        const auto whole = [](const core::Page& page) { return QRectF(0, 0, page.spec.width_mm.value(), page.spec.height_mm.value()); };
        const QMetaObject::Connection link = QObject::connect(&canvas->renderer(), &app::PageRenderer::firstShown, canvas, [&] {
            const core::Page* page = canvas->renderer().page();
            shown = true;
            rough_first = page != nullptr && !canvas->renderer().settled() && !canvas->renderer().current(canvas->base_dpi(), whole(*page));
        });
        s.window->go_to_page(3);
        QCOMPARE(s.window->page_index(), 2);
        QVERIFY(render::rough_needed(s.page(), canvas->base_dpi()));  // (what makes the canvas draw it roughly first)
        QVERIFY(canvas->wait_rendered(60000));
        QObject::disconnect(link);
        QVERIFY(shown);
        QVERIFY(rough_first);
        QVERIFY(canvas->renderer().current(canvas->base_dpi(), whole(s.page())));  // (the real picture now)
    }

    // --- tests/test_m7_session.py ----------------------------------------------------------------------------------

    // Python's test_actor_is_a_person: with GENKO_USER=leaf the app's actor is human:leaf, and a session given no
    // actor works as that person.
    void actorIsAPerson() {
        const QByteArray kept = qgetenv("GENKO_USER");
        qputenv("GENKO_USER", "leaf");
        QTemporaryDir tmp;
        const std::string actor = app::default_actor();
        std::string session_actor;
        {
            app::Session::Options options;  // (no actor: the default one)
            options.recovery_root = gui_test::path_of(tmp.path() + QStringLiteral("/recovery"));
            const auto session = std::make_shared<app::Session>(gui_test::new_doc(1), std::nullopt, options);
            session_actor = session->actor();
        }
        qputenv("GENKO_USER", kept);
        QCOMPARE(actor, std::string("human:leaf"));
        QCOMPARE(session_actor, std::string("human:leaf"));
    }

    // --- tests/test_w1_panels.py -----------------------------------------------------------------------------------

    // Python's test_own_layouts_are_kept_and_come_back_on_other_paper: drawn panels (one with round corners and a
    // dashed border) kept as one's own layout come back on B4 paper scaled to it with their corner and dash; a page of
    // drawn panels is cleared back to its basic frame; the layout taken away is no longer listed.
    void ownLayoutsAreKeptAndComeBackOnOtherPaper() {
        core::Document doc = run(b5(2), Json::array({Json{{"op", "add_frame"}, {"page", 1}, {"rect", Json::array({20, 20, 80, 60})}, {"id", "a"}},
                                                     Json{{"op", "add_frame"}, {"page", 1}, {"points", Json::parse("[[110, 20], [180, 20], [170, 80], [110, 80]]")},
                                                          {"id", "b"}},
                                                     Json{{"op", "set_frame"}, {"page", 1}, {"frame_id", "a"}, {"corner_mm", 5},
                                                          {"line", Json{{"kind", "dashed"}, {"dash_mm", 4}}}}}));
        app::templates::save_mine(QStringLiteral("二つ"), doc.page(0));
        const auto kept = app::templates::mine();
        QCOMPARE(kept.size(), std::size_t{1});
        QCOMPARE(kept[0].key, std::string("二つ"));
        // on other paper
        const core::Document other = core::new_episode("t", core::Num(1), 1, core::PageSpec::b4_comic());
        const app::templates::Plan plan = app::templates::plan(other, 0, kept[0], "human:leaf");
        core::Document laid;
        {
            core::ScopedIdScript ids(plan.ids);
            laid = run(other, plan.ops, "human:leaf");
        }
        const auto leaves = laid.page(0).leaf_frames();
        QCOMPARE(leaves.size(), std::size_t{2});
        QVERIFY(laid.page(0).frames[0].split_axis == std::optional<std::string>("free"));
        const core::Frame* rounded = nullptr;
        for (const core::Frame* frame : leaves) {
            if (frame->corner_mm != 0.0) rounded = frame;
        }
        QVERIFY(rounded != nullptr);
        QVERIFY(rounded->line.has_value() && rounded->line->contains("dash_mm"));
        QVERIFY(approx(rounded->line->at("dash_mm"), 4));
        QVERIFY(rounded->rect.width.value() > 80);  // (B4 is wider than B5)
        // a page of drawn panels is cleared back to its basic frame for a built-in template
        service::Work work;
        work.doc = doc;
        service::layout::clear_page(work, 0, "human:leaf");
        QCOMPARE(work.doc.page(0).leaf_frames().size(), std::size_t{1});
        QVERIFY(!work.doc.page(0).frames[0].split_axis.has_value());
        app::templates::remove_mine(QStringLiteral("二つ"));
        QVERIFY(app::templates::mine().empty());
    }

    // --- tests/test_w1_panels_gui.py -------------------------------------------------------------------------------

    // Python's test_panels_are_drawn_three_ways_and_one_is_deleted: with the panel tool a rectangle dragged becomes the
    // page's first drawn panel (in place of the basic frame), corners clicked and closed on the first one another
    // (nothing left being drawn), a freehand outline a third; the chosen one is deleted and the others stay.
    void panelsAreDrawnThreeWaysAndOneIsDeleted() {
        Desk s(b5(), QSize(1280, 720));
        s.window->choose_tool(QStringLiteral("frame"));
        auto* mode = s.window->findChild<QComboBox*>(QStringLiteral("frame_mode"));
        QVERIFY(mode != nullptr);
        const int index = mode->findData(QStringLiteral("rect"));
        QVERIFY(index >= 0);
        mode->setCurrentIndex(index);
        emit mode->activated(index);
        QCOMPARE(s.canvas()->frame_mode, QStringLiteral("rect"));
        s.drag({QPointF(20, 20), QPointF(60, 50), QPointF(100, 80)});
        QCOMPARE(s.leaves(), std::size_t{1});
        QVERIFY(s.page().frames[0].split_axis == std::optional<std::string>("free"));
        // corner by corner, closed on the first corner
        s.canvas()->frame_mode = QStringLiteral("poly");
        for (const QPointF& p : {QPointF(110, 20), QPointF(180, 20), QPointF(170, 80), QPointF(110, 80)}) inject::click_mm(s.canvas(), p);
        inject::click_mm(s.canvas(), QPointF(110, 20));
        QCOMPARE(s.leaves(), std::size_t{2});
        QTest::keyClick(s.canvas(), Qt::Key_Return);  // (no corners left being drawn: Enter makes no other panel)
        QCOMPARE(s.leaves(), std::size_t{2});
        // freehand
        s.canvas()->frame_mode = QStringLiteral("free");
        std::vector<QPointF> outline;
        for (int t = 0; t <= 40; ++t) {
            const double a = t / 40.0 * 2 * 3.14159265358979323846;
            outline.emplace_back(100 + 40 * std::cos(a), 150 + 25 * std::sin(a));
        }
        s.drag(outline);
        QCOMPARE(s.leaves(), std::size_t{3});
        // the chosen one goes, the others stay
        const core::Frame& first = *s.page().leaf_frames()[0];
        const std::string gone = first.id;
        const QPointF centre(first.rect.x.value() + first.rect.width.value() / 2, first.rect.y.value() + first.rect.height.value() / 2);
        s.window->choose_tool(QStringLiteral("select"));
        inject::click_mm(s.canvas(), centre);
        QVERIFY(s.page().selected_frame_id.is_string());
        QCOMPARE(s.page().selected_frame_id.get<std::string>(), gone);
        s.trigger("act_delete_frame");
        QCOMPARE(s.leaves(), std::size_t{2});
        for (const core::Frame* frame : s.page().leaf_frames()) QVERIFY(frame->id != gone);
    }

    // Python's test_the_layer_switch_for_panel_by_panel: the layer panel's 「描き始めたコマの中だけに描く」 switch shows the
    // ink layer's panel_each (on by default) and a click turns it off in the book.
    void theLayerSwitchForPanelByPanel() {
        Desk s(b5(), QSize(1280, 720));
        QCheckBox* box = nullptr;
        for (QCheckBox* b : s.window->findChildren<QCheckBox*>()) {
            if (b->text() == QStringLiteral("描き始めたコマの中だけに描く")) box = b;
        }
        QVERIFY(box != nullptr);
        const std::string ink = gui_test::ink_of(s.page())->id;
        s.window->set_target_layer(ink);
        QVERIFY(gui_test::ink_of(s.page())->panel_each);
        QVERIFY(box->isChecked());
        box->click();
        QVERIFY(!gui_test::ink_of(s.page())->panel_each);
    }

    // --- tests/test_w9_view.py -------------------------------------------------------------------------------------

    // Python's test_zoom_typed_in_upside_down_and_the_magnifier: a zoom set shows in the status bar's box and a number
    // typed there zooms; 上下反転して見る turns the view upside down (said beside the zoom: 上下反転; a point above the
    // middle is the page point below it) until the view is reset; the magnifier's rectangle fills the view.
    void zoomTypedInUpsideDownAndTheMagnifier() {
        Desk s(gui_test::new_doc(3), QSize(1280, 800));
        app::PageCanvas* c = s.canvas();
        c->set_zoom_percent(200);
        QVERIFY(std::abs(c->zoom_percent() - 200) <= 1);
        QSpinBox* zoom_box = nullptr;
        for (QSpinBox* box : s.window->findChildren<QSpinBox*>()) {
            if (box->toolTip() == QStringLiteral("表示倍率。数を打ち込んで Enter")) zoom_box = box;
        }
        QVERIFY(zoom_box != nullptr);
        QCOMPARE(zoom_box->value(), c->zoom_percent());
        zoom_box->setValue(50);
        QVERIFY(std::abs(c->zoom_percent() - 50) <= 1);
        s.trigger("act_view_flip_v");
        QVERIFY(c->flipped_vertical());
        QTRY_VERIFY_WITH_TIMEOUT(s.window->zoom_label()->text().contains(QStringLiteral("上下反転")), 5000);
        const QPointF mid(c->width() / 2.0, c->height() / 2.0);
        const QPointF mm = c->to_mm(QPointF(mid.x(), mid.y() - 50));
        QVERIFY(approx(mm.y() * c->scale() + c->pan().y(), mid.y() + 50));  // (50 px above the middle: the view's point 50 px below)
        c->reset_view();
        QVERIFY(!c->flipped_vertical());
        s.window->choose_tool(QStringLiteral("zoom"));
        const int before = c->zoom_percent();
        c->zoom_to_rect(20, 20, 60, 50);
        QVERIFY(c->zoom_percent() > before);
        const QPointF top_left = c->to_mm(QPointF(0, 0));
        const QPointF bottom_right = c->to_mm(QPointF(c->width(), c->height()));
        QVERIFY(top_left.x() <= 20 && bottom_right.x() >= 60 && top_left.y() <= 20 && bottom_right.y() >= 50);  // (the area fills the view)
    }

    // Python's test_the_pivot_moves_and_the_selection_turns_about_it: a selection turns about its middle until the pivot
    // is put elsewhere (the middle moves the selection: no pivot there); 基準位置を動かす puts it at the next press and
    // the turn handle then turns the selection about it (a quarter turn leaves the pivot where it is); a mesh warp of a
    // chosen grid (3 × 1 cells: 4 × 2 points) is sent with its grid; a new selection forgets the pivot.
    void thePivotMovesAndTheSelectionTurnsAboutIt() {
        Desk s(gui_test::new_doc(3), QSize(1280, 800));
        app::PageCanvas* c = s.canvas();
        s.window->choose_tool(QStringLiteral("rect"));
        QCOMPARE(c->tool(), QStringLiteral("marquee"));
        const Json square{{"poly", Json::parse("[[20, 20], [60, 20], [60, 40], [20, 40]]")}};
        c->set_selection(square);
        QVERIFY(approx(c->selection_pivot(), 40, 30));
        std::vector<QVector<double>> transformed;
        std::vector<Json> warped;
        QObject::connect(c, &app::PageCanvas::selectionTransformed, c, [&transformed](const QVector<double>& m) { transformed.push_back(m); });
        QObject::connect(c, &app::PageCanvas::selectionWarped, c, [&warped](const Json& w) { warped.push_back(w); });
        // the middle is for moving the selection
        s.drag({QPointF(40, 30), QPointF(43, 31.5), QPointF(45, 33)});
        QCOMPARE(transformed.size(), std::size_t{1});
        const QVector<double> moved = transformed.back();
        QVERIFY(approx(moved[0], 1) && approx(moved[1], 0) && approx(moved[2], 0) && approx(moved[3], 1) && approx(moved[4], 5) && approx(moved[5], 3));
        c->set_selection(square);
        // 基準位置を動かす: the next press puts the pivot
        c->pivot_mode = true;
        inject::click_mm(c, QPointF(20, 20));
        QVERIFY(approx(c->selection_pivot(), 20, 20));
        QVERIFY(!c->pivot_mode);
        // the turn handle (18 px over the top middle) dragged a quarter turn clockwise about the pivot
        const double h = 18 / c->scale();
        s.drag({QPointF(40, 20 - h), QPointF(35, 35), QPointF(20 + h, 40)});
        QCOMPARE(transformed.size(), std::size_t{2});
        const QVector<double> m = transformed.back();
        QVERIFY(approx(m[0] * 20 + m[2] * 20 + m[4], 20) && approx(m[1] * 20 + m[3] * 20 + m[5], 20));  // (about the top-left corner)
        QVERIFY(approx(std::abs(m[1]), 1));
        // a mesh warp with a grid of 3 × 1 cells
        c->set_selection(square);
        QVERIFY(c->start_warp(QStringLiteral("mesh"), 3, 1));
        QVERIFY(c->warping());
        c->finish_warp();
        QCOMPARE(warped.size(), std::size_t{1});
        QCOMPARE(warped[0]["mesh"].size(), std::size_t{8});
        QCOMPARE(warped[0]["grid"], Json::array({4, 2}));
        // a new selection forgets the pivot
        c->set_selection(square);
        c->pivot_mode = true;
        inject::click_mm(c, QPointF(20, 20));
        QVERIFY(approx(c->selection_pivot(), 20, 20));
        c->set_selection(square);
        QVERIFY(approx(c->selection_pivot(), 40, 30));
    }

    // --- tests/test_h5.py ------------------------------------------------------------------------------------------

    // Python's test_materials_fonts_and_brushes_can_be_listed: an agent lists the materials (each with its id, name,
    // kind and folder; a tone listed can be placed), the fonts (the bundled ones and the system's) and the brushes
    // (the built-in ones, and a brush the book defines); an unknown target is refused.
    void materialsFontsAndBrushesCanBeListed() {
        QTemporaryDir root;
        const auto agent = demo_agent(gui_test::path_of(root.path()));
        QVERIFY(agent != nullptr);
        const service::ToolResult listed = agent->call("inspect", Json::array({"demo.genko", "materials"}), Json::object());
        QVERIFY(listed.ok);
        const Json& materials = listed.data.at("materials");
        QVERIFY(materials.is_array() && !materials.empty());
        const Json* tone = nullptr;
        for (const Json& m : materials) {
            for (const char* key : {"id", "name", "kind", "folder"}) QVERIFY2(m.contains(key), key);
            if (tone == nullptr && m.at("kind") == Json("tone")) tone = &m;
        }
        QVERIFY(tone != nullptr);
        // what is listed can be placed
        const std::string frame = service::load_episode(agent->project_path(Json("demo.genko"))).page(0).leaf_frames()[0]->id;
        QVERIFY(agent->call("apply_ops",
                            Json::array({"demo.genko", Json::array({Json{{"op", "stamp_material"}, {"page", 1}, {"material_id", tone->at("id")}, {"frame_id", frame}}})}),
                            Json{{"commit", true}})
                    .ok);
        const service::ToolResult fonts = agent->call("inspect", Json::array({"demo.genko", "fonts"}), Json::object());
        QVERIFY(fonts.ok);
        std::vector<std::string> bundled;
        for (const Json& f : fonts.data.at("fonts").at("bundled")) bundled.push_back(f.at("key").get<std::string>());
        for (const char* key : {"antique", "gothic", "mincho", "sfx"}) QVERIFY2(std::find(bundled.begin(), bundled.end(), key) != bundled.end(), key);
        QVERIFY(fonts.data.at("fonts").contains("system"));
        service::ToolResult brushes = agent->call("inspect", Json::array({"demo.genko", "brushes"}), Json::object());
        QVERIFY(brushes.ok);
        std::vector<std::string> builtin;
        for (const Json& b : brushes.data.at("brushes").at("builtin")) builtin.push_back(b.at("key").get<std::string>());
        for (const char* key : {"gpen", "maru", "fude"}) QVERIFY2(std::find(builtin.begin(), builtin.end(), key) != builtin.end(), key);
        QVERIFY(agent->call("apply_ops",
                            Json::array({"demo.genko", Json::array({Json{{"op", "define_brush"}, {"key", "my_dry"}, {"label", "かすれ"}, {"base", "fude"}}})}),
                            Json{{"commit", true}})
                    .ok);
        brushes = agent->call("inspect", Json::array({"demo.genko", "brushes"}), Json::object());
        QVERIFY(brushes.ok);
        bool own = false;
        for (const Json& b : brushes.data.at("brushes").at("book")) {
            own = own || (b.value("key", std::string()) == "my_dry" && b.value("label", std::string()) == "かすれ");
        }
        QVERIFY(own);
        QVERIFY(!agent->call("inspect", Json::array({"demo.genko", "castle"}), Json::object()).ok);
    }

    // Python's test_render_one_layer_or_the_print_look: an agent renders the ink layer alone (its line there, and not
    // the page with its panels and lettering), the page as it prints, and is refused a layer that is not there and a
    // mode that is not one.
    void renderOneLayerOrThePrintLook() {
        QTemporaryDir root;
        const auto agent = demo_agent(gui_test::path_of(root.path()));
        QVERIFY(agent != nullptr);
        const std::string ink = gui_test::ink_of(service::load_episode(agent->project_path(Json("demo.genko"))).page(0))->id;
        QVERIFY(agent->call("apply_ops",
                            Json::array({"demo.genko",
                                         Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", ink}, {"points", Json::parse("[[40, 60], [200, 60]]")}, {"width_mm", 3}},
                                                      Json{{"op", "add_line"}, {"page", 1}, {"text", "やあ"}, {"x_mm", 150}, {"y_mm", 150}, {"w_mm", 30}, {"h_mm", 40}}})}),
                            Json{{"commit", true}})
                    .ok);
        const service::ToolResult alone = agent->call("render", Json::array({"demo.genko", 1, "proof", 600}), Json{{"layer_id", ink}});
        QVERIFY(alone.ok);
        QCOMPARE(alone.data.at("layer_id"), Json(ink));
        QVERIFY(!alone.images.empty());
        const render::Image image = render::read_png(alone.images[0]).convert("L");
        QVERIFY(image.getextrema()[0].first < 80);  // the line is there
        const service::ToolResult page = agent->call("render", Json::array({"demo.genko", 1, "proof", 600}), Json::object());
        QVERIFY(page.ok && !page.images.empty());
        const render::Image whole = render::read_png(page.images[0]).convert("L");
        QVERIFY(image.tobytes() != whole.tobytes());  // (no frames, no lettering: the layer alone)
        const service::ToolResult printed = agent->call("render", Json::array({"demo.genko", 1, "print", 600}), Json::object());
        QVERIFY(printed.ok);
        QCOMPARE(printed.data.at("mode"), Json("print"));
        QVERIFY(!agent->call("render", Json::array({"demo.genko", 1, "proof", 600}), Json{{"layer_id", "nope"}}).ok);
        QVERIFY(!agent->call("render", Json::array({"demo.genko", 1, "sepia", 600}), Json::object()).ok);
    }

    // --- tests/test_j12.py -----------------------------------------------------------------------------------------

    // Python's test_onion_skin_and_light_table: at frame 3 the onion skin shows the cel of the frame before in red and
    // the one after in blue, and not the cel shown now; a cel on the light table shows even with no frames around.
    void onionSkinAndLightTable() {
        core::Document doc = animated();
        const auto px = [](double mm) { return render::mm_to_px(mm, 30); };
        const std::optional<render::Image> ghost = render::anim::onion(doc.page(0), 3, 30);
        QVERIFY(ghost.has_value());
        const auto before = ghost->getpixel(px(40), px(100));  // (cel a, the frame before: red)
        const auto after = ghost->getpixel(px(140), px(100));  // (cel c, the frame after: blue)
        QVERIFY(before[3] > 0 && before[0] > before[2]);
        QVERIFY(after[3] > 0 && after[2] > after[0]);
        QCOMPARE(ghost->getpixel(px(90), px(100))[3], 0.0);  // (not the cel shown now)
        doc = run(doc, Json::array({Json{{"op", "set_light_table"}, {"page", 1}, {"cels", Json::array({"c"})}}}));
        const std::optional<render::Image> lit = render::anim::onion(doc.page(0), 1, 30, 0, 0);
        QVERIFY(lit.has_value());
        QVERIFY(lit->getpixel(px(140), px(100))[3] > 0);
    }

    // Python's test_camera_work_moves_between_keys: the camera moves evenly between its keys and stays at the last
    // one; a frame is cut to the camera's rect; with a key taken away the camera is where the one left puts it.
    void cameraWorkMovesBetweenKeys() {
        core::Document doc = run(animated(), Json::array({Json{{"op", "set_camera_key"}, {"page", 1}, {"frame", 1}, {"rect", Json::array({0, 0, 100, 140})}},
                                                         Json{{"op", "set_camera_key"}, {"page", 1}, {"frame", 5}, {"rect", Json::array({80, 80, 100, 140})}}}));
        const auto at = [&doc](std::int64_t frame) { return core::anim::camera_at(doc.page(0), frame); };
        const auto is = [](const std::optional<std::vector<double>>& rect, std::array<double, 4> want) {
            if (!rect || rect->size() != 4) return false;
            for (std::size_t i = 0; i < 4; ++i) {
                if (!approx((*rect)[i], want[i])) return false;
            }
            return true;
        };
        QVERIFY(is(at(3), {40, 40, 100, 140}));
        QVERIFY(is(at(6), {80, 80, 100, 140}));
        const render::Image one = render::anim::render_frame(doc.page(0), 1, 20, &doc);
        QVERIFY((one.width() == static_cast<int>(std::lround(100 / 25.4 * 20)) && one.height() == static_cast<int>(std::lround(140 / 25.4 * 20))) ||
                std::abs(one.width() - 79) <= 1);
        doc = run(doc, Json::array({Json{{"op", "set_camera_key"}, {"page", 1}, {"frame", 5}, {"rect", nullptr}}}));
        QVERIFY(is(at(5), {0, 0, 100, 140}));
    }

    // Python's test_writing_it_out (the numbered frames at a chosen width; the moving pictures are compared with
    // Python's elsewhere): six frames, each 100 pixels across; a page that is not an animation is refused.
    void writingItOut() {
        QTemporaryDir tmp;
        const core::Document doc = animated();
        const auto frames = render::anim::export_animation(doc.page(0), gui_test::path_of(tmp.path() + QStringLiteral("/frames")), &doc, 20,
                                                           std::string("frames"), true, "trim", 100);
        QCOMPARE(frames.size(), std::size_t{6});
        QCOMPARE(render::picture_size(genko::test::read_bytes(gui_test::qpath(frames[0]))).width, 100);
        const core::Document plain = b5();
        bool refused = false;
        try {
            render::anim::export_animation(plain.page(0), gui_test::path_of(tmp.path() + QStringLiteral("/x.gif")));
        } catch (const core::PyValueError&) {
            refused = true;
        }
        QVERIFY(refused);
    }

    // --- tests/test_w10_rulers.py ----------------------------------------------------------------------------------

    // Python's test_shape_rulers_and_a_selection_from_a_ruler_in_the_app: an ellipse ruler made the selection gives its
    // outline (many points, as wide as the ruler's box); a rect ruler dragged is placed on its two corners; a polygon
    // ruler clicked with a corner clicked twice is placed without the repeated one.
    void shapeRulersAndASelectionFromARulerInTheApp() {
        Desk s(gui_test::new_doc(3));
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_ruler"}, {"page", s.page().index.json()}, {"kind", "ellipse"}, {"id", "e"},
                                                      {"points", Json::parse("[[20, 20], [80, 60]]")}}})));
        s.canvas()->selected_ruler_id = "e";
        s.trigger("act_ruler_selection");
        const auto& selection = s.canvas()->selection();
        QVERIFY(selection.has_value() && selection->area.contains("poly"));
        const Json& poly = selection->area.at("poly");
        QVERIFY(poly.size() > 20);
        double lo = 1e9, hi = -1e9;
        for (const Json& p : poly) {
            lo = std::min(lo, p[0].get<double>());
            hi = std::max(hi, p[0].get<double>());
        }
        QVERIFY(approx(lo, 20, 0.1) && approx(hi, 80, 0.1));
        std::vector<Json> placed;
        QObject::connect(s.canvas(), &app::PageCanvas::rulerPlaced, s.canvas(), [&placed](const Json& ruler) { placed.push_back(ruler); });
        s.window->choose_tool(QStringLiteral("ruler"));
        s.canvas()->ruler_kind = QStringLiteral("rect");
        inject::mouse_stroke(s.canvas(), {QPointF(10, 10), QPointF(40, 30)});
        QCOMPARE(placed.size(), std::size_t{1});
        QCOMPARE(placed[0]["points"].size(), std::size_t{2});
        QVERIFY(approx(placed[0]["points"][0][0], 10, 0.01) && approx(placed[0]["points"][0][1], 10, 0.01));
        QVERIFY(approx(placed[0]["points"][1][0], 40, 0.01) && approx(placed[0]["points"][1][1], 30, 0.01));
        s.canvas()->ruler_kind = QStringLiteral("polygon");
        for (const QPointF& p : {QPointF(0, 0), QPointF(0, 0), QPointF(10, 0), QPointF(5, 8)}) inject::click_mm(s.canvas(), p);
        QTest::keyClick(s.canvas(), Qt::Key_Return);
        QCOMPARE(placed.size(), std::size_t{2});
        const Json& corners = placed[1]["points"];
        QCOMPARE(corners.size(), std::size_t{3});
        const std::array<std::array<double, 2>, 3> want{{{0, 0}, {10, 0}, {5, 8}}};
        for (std::size_t i = 0; i < 3; ++i) QVERIFY(approx(corners[i][0], want[i][0], 0.01) && approx(corners[i][1], want[i][1], 0.01));
    }

    // --- tests/test_w12_figure.py ----------------------------------------------------------------------------------

    // Python's test_the_3d_panel_keeps_a_pose_and_offers_it: one's own pose (kept with the app's settings) is offered
    // in the 3D panel's list as 「自分: 手を振る」, and choosing it gives the figure its joints.
    void the3dPanelKeepsAPoseAndOffersIt() {
        Desk s(gui_test::new_doc(3));
        app::GuidePanel* panel = s.window->guides();
        QVERIFY(panel != nullptr);
        s.trigger("act_add_figure");
        QVERIFY(s.canvas()->selected_prim_id.has_value());
        const std::string prim_id = *s.canvas()->selected_prim_id;
        const Json* prim = s.prim(prim_id);
        QVERIFY(prim != nullptr);
        Json pose = *prim;
        pose["joints"] = Json{{"r_arm", Json{{"z", -2.0}}}};
        core::poses::save_pose("手を振る", pose);
        panel->refresh();
        panel->select_prim(prim_id);
        QStringList labels;
        for (int i = 0; i < panel->preset->count(); ++i) labels << panel->preset->itemText(i);
        QVERIFY(labels.contains(QStringLiteral("自分: 手を振る")));
        const int at = static_cast<int>(labels.indexOf(QStringLiteral("自分: 手を振る")));
        panel->preset->setCurrentIndex(at);
        emit panel->preset->activated(at);
        prim = s.prim(prim_id);
        QVERIFY(prim != nullptr);
        QVERIFY(approx(prim->at("joints").at("r_arm").at("z"), -2.0));
    }

    // --- tests/test_w7_tone.py -------------------------------------------------------------------------------------

    // Python's test_layer_toning_takes_a_shape_and_an_offset: a layer's screen keeps its dot shape and its offset, and
    // a dot shape that is not one (star) is refused.
    void layerToningTakesAShapeAndAnOffset() {
        const core::Document doc = run(b5(), Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"layer", "ink"},
                                                               {"screen", Json{{"lpi", 40}, {"shape", "diamond"}, {"offset_mm", Json::array({1, 0})}}}}}));
        const core::Layer* ink = gui_test::ink_of(doc.page(0));
        QVERIFY(ink != nullptr && ink->screen.has_value());
        QCOMPARE(ink->screen->at("shape"), Json("diamond"));
        const Json& offset = ink->screen->at("offset_mm");
        QVERIFY(offset.is_array() && offset.size() == 2 && approx(offset[0], 1.0) && approx(offset[1], 0.0));
        QVERIFY(!refusal(doc, Json::array({Json{{"op", "set_layer"}, {"page", 1}, {"layer", "ink"}, {"screen", Json{{"lpi", 40}, {"shape", "star"}}}}})).empty());
    }

    // --- tests/test_w8_filters.py ----------------------------------------------------------------------------------

    // Python's test_the_filter_dialog_curve_and_choices: a point added on the tone curve comes back with the curve's
    // points and its channel, and the page's preview is taken away when the dialog closes.
    void theFilterDialogCurveAndChoices() {
        gui_test::Answers answers;
        answers.responder->exec = [](QDialog* dialog) {
            auto* curve = dialog->findChild<app::CurveEditor*>();
            if (curve == nullptr) return int(QDialog::Rejected);
            const double side = app::CurveEditor::kSide;
            const QPointF at(1 + 128.0 / 255 * side, 1 + (255 - 190.0) / 255 * side);  // (the point 128 → 190)
            QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(curve, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(curve, &release);
            return int(QDialog::Accepted);
        };
        std::vector<std::optional<Json>> seen;
        const std::optional<Json> values =
            app::filter_params(nullptr, "curve", Json::object(), [&seen](const std::optional<Json>& preview) { seen.push_back(preview); });
        QVERIFY(values.has_value());
        QCOMPARE(values->at("points"), Json::parse("[[0, 0], [128, 190], [255, 255]]"));
        QCOMPARE(values->at("channel"), Json("rgb"));
        QVERIFY(!seen.empty() && !seen.back().has_value());  // (the preview taken away when the dialog closes)
    }

    // Python's test_the_levels_dialog_shows_the_layers_histogram: the levels dialog shows the layer's histogram as a
    // picture.
    void theLevelsDialogShowsTheLayersHistogram() {
        gui_test::Answers answers;
        bool found = false;
        answers.responder->exec = [&found](QDialog* dialog) {
            for (QLabel* label : dialog->findChildren<QLabel*>()) found = found || !label->pixmap().isNull();
            return int(QDialog::Rejected);
        };
        std::vector<int> counts(256, 0);
        counts[30] = 500;
        counts[200] = 900;
        QVERIFY(!app::filter_params(nullptr, "levels", Json::object(), {}, counts).has_value());
        QVERIFY(found);
    }

    // --- tests/test_j10.py -----------------------------------------------------------------------------------------

    // Python's test_import_psd_respects_the_name_gate: under strict_gates an AI cannot import a PSD onto a page whose
    // name is not approved yet.
    void importPsdRespectsTheNameGate() {
        QTemporaryDir tmp;
        const core::Document source = b5();
        const fs::path psd = formats::export_page_psd(source, source.page(0), gui_test::path_of(tmp.path() + QStringLiteral("/a.psd")), std::int64_t{30});
        core::Document doc = b5();
        doc.strict_gates = true;
        const std::string refused =
            refusal(doc, Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"path", core::path_to_utf8(psd)}}}), "ai:hermes");
        QVERIFY2(!refused.empty(), "import_psd was applied");
        QVERIFY2(refused.find("name_ok") != std::string::npos, refused.c_str());  // (refused by the name gate, not by the file)
    }

    // --- tests/test_w15_color.py -----------------------------------------------------------------------------------

    // Python's test_a_brush_that_mixes_with_the_colour_already_there: a blue line drawn with mix and stretch across a red
    // one becomes short pieces that meet, picking the red up over it and carrying it on past it, fading; without
    // stretch nothing is carried past the red; a brush keeps its mix and stretch, and a mix out of range is refused.
    void aBrushThatMixesWithTheColourAlreadyThere() {
        core::Document doc = run(core::new_episode("t", core::Num(1), 1, core::PageSpec::b4_comic()),
                                 Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "c"}},
                                              Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "c"}, {"points", Json::parse("[[40, 100], [40, 200]]")},
                                                   {"width_mm", 20}, {"rgb", Json::array({255, 0, 0})}, {"stabilize", 0}, {"taper", false}}}));
        const Json across = Json::parse("[[20, 150], [60, 150], [90, 150], [120, 150]]");
        doc = run(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "c"}, {"points", across}, {"width_mm", 3},
                                         {"rgb", Json::array({0, 0, 255})}, {"mix", 0.8}, {"stretch", 0.9}, {"stabilize", 0}, {"taper", false}}}));
        const core::Layer* layer = layer_by_id(doc.page(0), "c");
        QVERIFY(layer != nullptr);
        const std::vector<core::StrokePtr> pieces(layer->strokes->items.begin() + 1, layer->strokes->items.end());
        QVERIFY(pieces.size() > 3);
        std::vector<std::vector<std::int64_t>> colours;
        for (const core::StrokePtr& piece : pieces) {
            QVERIFY(piece->rgb.has_value() && piece->rgb->size() == 3);
            colours.push_back(*piece->rgb);
        }
        std::size_t reddest = 0;
        for (std::size_t i = 1; i < colours.size(); ++i) {
            if (colours[i][0] > colours[reddest][0]) reddest = i;
        }
        QVERIFY(colours[0][0] < 20 && colours[reddest][0] > 60);  // (over the red: red is picked up)
        QVERIFY(0 < colours.back()[0] && colours.back()[0] < colours[reddest][0]);  // (carried on past it, fading)
        QVERIFY(pieces[0]->points.back() == pieces[1]->points.front());  // (the pieces meet)
        Json lower = Json::array();
        for (const Json& p : across) lower.push_back(Json::array({p[0], 180}));
        doc = run(doc, Json::array({Json{{"op", "add_stroke"}, {"page", 1}, {"layer_id", "c"}, {"points", lower}, {"width_mm", 3},
                                         {"rgb", Json::array({0, 0, 255})}, {"mix", 0.8}, {"stretch", 0.0}, {"stabilize", 0}, {"taper", false}}}));
        layer = layer_by_id(doc.page(0), "c");
        QVERIFY(layer != nullptr && layer->strokes->items.size() > pieces.size() + 1);
        const core::StrokePtr& last = layer->strokes->items.back();
        QVERIFY(last->rgb == std::optional<std::vector<std::int64_t>>(std::vector<std::int64_t>{0, 0, 255}));  // (nothing carried past the red)
        const core::Brush mixer = core::brush_from_dict("my_mix", Json{{"label", "混ぜ"}, {"mix", 0.5}, {"stretch", 0.7}}, std::nullopt, {});
        QCOMPARE(mixer.mix, 0.5);
        QVERIFY(approx(core::brush_to_dict(mixer).at("stretch"), 0.7));
        bool refused = false;
        try {
            core::brush_from_dict("my_mix", Json{{"label", "混ぜ"}, {"mix", 2}}, std::nullopt, {});
        } catch (const core::PyValueError&) {
            refused = true;
        }
        QVERIFY(refused);
    }

    // Python's test_the_stops_editor_and_the_gradient_dialog: the stops editor takes a ready-made row (虹: six colours),
    // adds one and takes one away; a gradient's two ends become two stops; the gradient and gradient-map dialogs give
    // back the colours chosen in them (夕焼け: four stops; the map's as [place, colour]).
    void theStopsEditorAndTheGradientDialog() {
        app::StopsEditor editor;
        choose_preset(&editor, QStringLiteral("虹"));
        QCOMPARE(editor.stops().size(), std::size_t{6});
        auto* add = editor.findChild<QPushButton*>(QStringLiteral("stops_add"));
        QVERIFY(add != nullptr);
        add->click();
        QCOMPARE(editor.stops().size(), std::size_t{7});
        // (each row's × takes its colour away; the rows made last are the ones shown)
        QList<QPushButton*> removes;
        for (QPushButton* button : editor.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("×")) removes << button;
        }
        QVERIFY(removes.size() >= 7);
        removes[removes.size() - 7]->click();
        QCOMPARE(editor.stops().size(), std::size_t{6});
        const auto two = app::stops_from(Json{{"rgb_from", Json::array({1, 2, 3})}, {"rgb_to", Json::array({4, 5, 6})}});
        QCOMPARE(two.size(), std::size_t{2});
        QCOMPARE(two[1].rgb, (std::array<int, 3>{4, 5, 6}));
        gui_test::Answers answers;
        answers.responder->exec = [](QDialog* dialog) {
            const auto editors = dialog->findChildren<app::StopsEditor*>();
            if (editors.isEmpty()) return int(QDialog::Rejected);
            choose_preset(editors[0], QStringLiteral("夕焼け"));
            return int(QDialog::Accepted);
        };
        const core::Document doc = core::new_episode("t", core::Num(1), 1, core::PageSpec::b4_comic());
        const std::optional<Json> out = app::gradient_dialog(nullptr, &doc.page(0), Json::object());
        QVERIFY(out.has_value() && out->contains("stops"));
        QCOMPARE(out->at("stops").size(), std::size_t{4});
        const std::optional<Json> mapped = app::gradient_map_dialog(nullptr, Json::object(), {{0, 0, 0}, {255, 255, 255}});
        QVERIFY(mapped.has_value() && mapped->contains("stops"));
        QCOMPARE(mapped->at("stops").size(), std::size_t{4});
        QCOMPARE(mapped->at("stops")[0].size(), std::size_t{2});
    }

    // --- tests/test_w2_lines.py and tests/test_w3_fill.py (last: the brush panel keeps what is set in this test
    // process's settings) ---------------------------------------------------------------------------------------------

    // Python's test_the_brush_panel_sends_them: a line from the brush panel carries the taper's start length (and not
    // its end's, left to the brush: -1), the ink's pressure, the steadiness that follows the speed and the after-fit.
    void theBrushPanelSendsThem() {
        app::BrushPanel panel;
        panel.taper->setChecked(true);
        panel.taper_in->setValue(2.5);
        panel.taper_out->setValue(-1);
        panel.ink_pressure->setValue(40);
        panel.steady->setValue(4);
        panel.speed_steady->setChecked(true);
        panel.post_fit->setValue(0.4);
        const Json fields = panel.stroke_fields();
        QVERIFY(fields.contains("taper_in_mm") && approx(fields.at("taper_in_mm"), 2.5));
        QVERIFY(!fields.contains("taper_out_mm"));
        QVERIFY(fields.contains("pressure_opacity") && approx(fields.at("pressure_opacity"), 0.4));
        QVERIFY(fields.contains("stabilize_speed") && fields.at("stabilize_speed") == Json(true));
        QVERIFY(fields.contains("post_fit") && approx(fields.at("post_fit"), 0.4));
    }

    // Python's test_the_brush_panel_gives_the_fill_its_settings: a fill from the brush panel carries its tolerance, its
    // spread and the layers it leaves out (the drafts); the lasso fill offers its ways.
    void theBrushPanelGivesTheFillItsSettings() {
        app::BrushPanel panel;
        panel.tolerance->setValue(55);
        panel.expand->setValue(0.3);
        panel.skip_draft->setChecked(true);
        const Json fields = panel.fill_fields();
        QCOMPARE(fields.at("tolerance"), Json(55));
        QVERIFY(approx(fields.at("expand_mm"), 0.3));
        QVERIFY(fields.contains("ignore"));
        QCOMPARE(fields.at("ignore"), Json::array({"draft"}));
        QVERIFY(panel.lasso_mode->findData(QStringLiteral("enclosed")) >= 0 && panel.lasso_mode->findData(QStringLiteral("gaps")) >= 0);
    }
};

QTEST_MAIN(TestGuiLedgerC)
#include "test_gui_ledger_c.moc"
