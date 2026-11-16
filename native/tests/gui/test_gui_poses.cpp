// 人形のポーズ集 G4 (SPEC FIGURE-02): ツール → 3D → ポーズ集… and the 定規・3D panel's ポーズ集… beside 体型・手… — the
// window of the pose library for the selected figure on the offscreen platform: the categories with their counts (すべて
// first), a search in Japanese and English, 正面・斜め・横, 左右反転, 足もとを保つ (on), ランダム from a seed (the same seed,
// the same pick, within the category), the small pictures drawn on worker threads with the figure's own body (the same
// bytes as render::figure_thumbs draws), a double-click and Enter putting the pose on the figure as one change through
// the window's ops (pose_figure {page, id, pose_id, mirror, keep}; Undo takes it back), the presets' list kept.

#include <QtTest>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>

#include <algorithm>
#include <memory>
#include <set>

#include "app/guide_panel.hpp"
#include "app/main_window.hpp"
#include "app/pose_gallery.hpp"
#include "core/mesh3d.hpp"
#include "core/pose_library.hpp"
#include "gui_support.hpp"
#include "render/figure_thumbs.hpp"

using namespace gui_test;
using genko::app::MainWindow;
using genko::app::PoseGallery;
using genko::app::Session;
using genko::core::Json;
namespace library = genko::core::pose_library;
namespace fs = std::filesystem;

namespace {

genko::core::Document new_doc() {
    genko::core::Document doc = genko::core::new_episode("ポーズ", genko::core::Num(1), 1, genko::core::PageSpec::custom(160, 200, 140, 180, 1, 2, 2, 2, 2, 72, "mono"));
    doc.edit_page(0).name_ok = true;
    return doc;
}

struct Studio {
    QTemporaryDir dir;
    std::shared_ptr<Session> session;
    std::unique_ptr<Answers> answers = std::make_unique<Answers>();
    std::unique_ptr<MainWindow> window;
    std::vector<Json> ops;

    Studio() {
        (void)config_folder();
        const fs::path book = path_of(dir.filePath("book.genko"));
        write_book(book, new_doc());
        session = Session::open(book, quick(path_of(dir.filePath("recovery"))));
        window = std::make_unique<MainWindow>(session);
        window->resize(1280, 860);
        window->show();
        QObject::connect(session.get(), &Session::changed, window.get(), [this](const genko::app::BookChange& c) {
            if (c.why == genko::app::BookChange::Why::Edit)
                for (const Json& op : c.ops) ops.push_back(op);
        });
    }
    ~Studio() {
        window.reset();
        session.reset();
        answers.reset();
    }
    void trigger(const char* name) const {
        QAction* a = window->action(QString::fromLatin1(name));
        QVERIFY2(a != nullptr, name);
        a->trigger();
    }
    const genko::core::Page& page() const { return *window->current_page(); }
    const Json* figure() const {
        for (const Json& p : page().prims)
            if (p.value("kind", std::string()) == "figure") return &p;
        return nullptr;
    }
    Json last() const { return ops.empty() ? Json() : ops.back(); }
};

QStringList menus_of(MainWindow* w, QAction* act) {
    QStringList out;
    const auto walk = [&](const auto& self, QMenu* menu, const QString& path) -> void {
        for (QAction* a : menu->actions()) {
            if (a == act) out << path;
            if (a->menu() != nullptr) self(self, a->menu(), path + "/" + a->menu()->title().remove('&'));
        }
    };
    for (QAction* top : w->menuBar()->actions())
        if (top->menu() != nullptr) walk(walk, top->menu(), top->menu()->title().remove('&'));
    return out;
}

void press(QWidget* on, int key) {
    QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(on, &down);
}

}  // namespace

class TestGuiPoses : public QObject {
    Q_OBJECT

private slots:
    // Its command (ツール → 3D → ポーズ集…) and the panel's button beside 体型・手…, on only for a figure; without a figure
    // the window does not open (a word says to place one).
    void theCommandAndTheButton() {
        Studio s;
        QAction* act = s.window->action(QStringLiteral("act_pose_gallery"));
        QVERIFY(act != nullptr);
        QCOMPARE(act->text(), QStringLiteral("ポーズ集…"));
        QVERIFY(act->shortcuts().isEmpty());
        QVERIFY(menus_of(s.window.get(), act).contains(QStringLiteral("ツール/3D")));
        genko::app::GuidePanel* panel = s.window->guides();
        QVERIFY(panel->gallery_button != nullptr);
        QCOMPARE(panel->gallery_button->text(), QStringLiteral("ポーズ集…"));
        QCOMPARE(panel->gallery_button->parentWidget(), panel->body_button->parentWidget());
        s.trigger("act_pose_gallery");
        QVERIFY(panel->gallery() == nullptr);  // (no figure yet)
        s.trigger("act_add_box");
        panel->refresh();
        panel->prims->setCurrentRow(0);
        QVERIFY(!panel->gallery_button->isEnabled());
        s.trigger("act_add_figure");
        panel->refresh();
        panel->prims->setCurrentRow(1);
        QVERIFY(panel->gallery_button->isEnabled());
        QVERIFY(panel->body_button->isEnabled());
        // the presets' list stays as it was
        QVERIFY(panel->preset->findData(QStringLiteral("think")) >= 0);
        QVERIFY(panel->preset->findData(QStringLiteral("walk")) >= 0);
        panel->gallery_button->click();
        QVERIFY(panel->gallery() != nullptr);
        QVERIFY(panel->gallery()->isVisible());
        QCOMPARE(panel->gallery()->windowTitle(), QStringLiteral("ポーズ集"));
    }

    // The categories with their counts (すべて first), the poses of one, a search in either language (words each to
    // match), the views and the switches as they start.
    void theCategoriesAndTheSearch() {
        Studio s;
        s.trigger("act_add_figure");
        s.trigger("act_pose_gallery");
        PoseGallery* g = s.window->guides()->gallery();
        QVERIFY(g != nullptr);
        const auto cats = library::categories();
        QCOMPARE(g->categories->count(), static_cast<int>(cats.size()) + 1);
        std::size_t all = 0;
        for (const auto& c : cats) all += c.count;
        QCOMPARE(g->categories->item(0)->text(), QStringLiteral("すべて（%1）").arg(all));
        QCOMPARE(g->shown().size(), all);
        for (std::size_t i = 0; i < cats.size(); ++i) {
            QListWidgetItem* item = g->categories->item(static_cast<int>(i) + 1);
            QCOMPARE(item->data(Qt::UserRole).toString().toStdString(), cats[i].id);
            QCOMPARE(item->text(), QStringLiteral("%1（%2）").arg(QString::fromStdString(cats[i].ja)).arg(cats[i].count));
        }
        const auto row_of = [&](const char* id) {
            for (int r = 0; r < g->categories->count(); ++r)
                if (g->categories->item(r)->data(Qt::UserRole).toString() == QLatin1String(id)) return r;
            return -1;
        };
        g->categories->setCurrentRow(row_of("lie"));
        library::Filter lie;
        lie.category = "lie";
        QCOMPARE(g->shown().size(), library::list(lie).size());
        QCOMPARE(g->grid()->count(), static_cast<int>(g->shown().size()));
        for (const std::string& id : g->shown()) QVERIFY2(id.rfind("lie/", 0) == 0, id.c_str());
        QCOMPARE(g->grid()->item(0)->text(), QString::fromStdString(library::find(g->shown().front())->value("ja", std::string())));
        // search: Japanese, English (any case), two words
        g->categories->setCurrentRow(0);
        g->search->setText(QStringLiteral("あぐら"));
        QVERIFY(!g->shown().empty());
        for (const std::string& id : g->shown()) QVERIFY2(id.find("agura") != std::string::npos || id.find("meditation") != std::string::npos, id.c_str());
        g->search->setText(QStringLiteral("PHONE"));
        const std::size_t phones = g->shown().size();
        QVERIFY(phones >= 4);
        g->search->setText(QStringLiteral("phone 寝る"));
        QVERIFY(!g->shown().empty() && g->shown().size() < phones);
        g->search->setText(QStringLiteral("zzzz"));
        QVERIFY(g->shown().empty());
        g->search->clear();
        // the views and the switches
        QCOMPARE(g->view->count(), 3);
        QCOMPARE(g->view->itemText(0), QStringLiteral("正面"));
        QCOMPARE(g->view->itemText(1), QStringLiteral("斜め"));
        QCOMPARE(g->view->itemText(2), QStringLiteral("横"));
        QCOMPARE(g->view->itemData(2).toDouble(), genko::render::figure_thumbs::kSide);
        QCOMPARE(g->mirror->text(), QStringLiteral("左右反転"));
        QVERIFY(!g->mirror->isChecked());
        QCOMPARE(g->keep->text(), QStringLiteral("足もとを保つ"));
        QVERIFY(g->keep->isChecked());
        QCOMPARE(g->random_button->text(), QStringLiteral("ランダム"));
    }

    // The pictures are drawn on worker threads with the figure's own body, the same bytes as figure_thumbs draws; the
    // view and the mirror draw them again.
    void thePictures() {
        Studio s;
        s.trigger("act_add_figure");
        const Json* fig = s.figure();
        QVERIFY(fig != nullptr);
        s.window->apply_ops(Json::array({Json::object({{"op", "pose_figure"}, {"page", s.page().index.json()}, {"id", (*fig)["id"]},
                                                       {"body", Json::object({{"sex", "female"}, {"heads", 7.0}})}})}));
        s.trigger("act_pose_gallery");
        PoseGallery* g = s.window->guides()->gallery();
        g->search->setText(QStringLiteral("upper_body/shh"));
        QCOMPARE(g->shown().size(), std::size_t{1});
        QVERIFY(g->wait_pictures(60000));
        const Json body = (*s.figure())["body"];
        QCOMPARE(body["sex"], Json("female"));
        const auto drawn = [&](const std::string& id, double turn) {
            return genko::render::figure_thumbs::grey(genko::render::figure_thumbs::figure_for(*library::resolve(id), body), turn, PoseGallery::kWidth,
                                                      PoseGallery::kHeight, 2);
        };
        QVERIFY(g->picture_of(0) == drawn("upper_body/shh", genko::render::figure_thumbs::kThreeQuarter));
        QVERIFY(!g->grid()->item(0)->icon().isNull());
        g->view->setCurrentIndex(0);
        g->mirror->setChecked(true);
        QVERIFY(g->wait_pictures(60000));
        QVERIFY(g->picture_of(0) == drawn("upper_body/shh~m", genko::render::figure_thumbs::kFront));
        // a category's pictures, all drawn
        g->search->clear();
        for (int r = 0; r < g->categories->count(); ++r)
            if (g->categories->item(r)->data(Qt::UserRole).toString() == QLatin1String("point")) g->categories->setCurrentRow(r);
        QVERIFY(g->wait_pictures(120000));
        for (int r = 0; r < g->grid()->count(); ++r) QVERIFY(!g->picture_of(r).empty());
        // (GENKO_POSE_GALLERY_SHOT=<file.png>: the window as it is now, for a person to look at)
        if (const QString shot = qEnvironmentVariable("GENKO_POSE_GALLERY_SHOT"); !shot.isEmpty()) QVERIFY(g->grab().save(shot));
    }

    // ランダム: 24 poses of the category shown (all of them when fewer), each once; the same seed, the same pick; back to
    // the list.
    void randomPicks() {
        Studio s;
        s.trigger("act_add_figure");
        s.trigger("act_pose_gallery");
        PoseGallery* g = s.window->guides()->gallery();
        const std::string seed = g->pick_random("fp-00000000000000a5");
        QCOMPARE(seed, std::string("fp-00000000000000a5"));
        const std::vector<std::string> first = g->shown();
        QCOMPARE(first.size(), std::size_t{24});
        QCOMPARE(std::set<std::string>(first.begin(), first.end()).size(), first.size());
        QVERIFY(g->all_button->isEnabled());
        g->pick_random("fp-00000000000000a5");
        QVERIFY(g->shown() == first);
        const std::string fresh = g->pick_random();
        QVERIFY(library::seed_of(fresh).has_value());
        for (int r = 0; r < g->categories->count(); ++r)
            if (g->categories->item(r)->data(Qt::UserRole).toString() == QLatin1String("turn")) g->categories->setCurrentRow(r);
        g->pick_random("fp-0000000000000001");
        library::Filter turn;
        turn.category = "turn";
        QCOMPARE(g->shown().size(), std::min<std::size_t>(24, library::list(turn).size()));
        for (const std::string& id : g->shown()) QVERIFY2(id.rfind("turn/", 0) == 0, id.c_str());
        g->all_button->click();
        QCOMPARE(g->shown().size(), library::list(turn).size());
        QVERIFY(!g->all_button->isEnabled());
    }

    // A double-click puts the pose on the figure: one pose_figure through the window (keep ground), the figure's pose_id
    // set and its name in the panel's list; 左右反転 and Enter give the mirror; 足もとを保つ off keeps the pelvis; Undo takes
    // each back.
    void applyingAPose() {
        Studio s;
        s.trigger("act_add_figure");
        genko::app::GuidePanel* panel = s.window->guides();
        panel->refresh();
        panel->prims->setCurrentRow(0);
        const Json before = *s.figure();
        panel->gallery_button->click();
        PoseGallery* g = panel->gallery();
        g->search->setText(QStringLiteral("lie/back_knees_up"));
        QCOMPARE(g->grid()->count(), 1);
        g->grid()->setCurrentRow(0);
        const std::size_t n = s.ops.size();
        emit g->grid()->itemDoubleClicked(g->grid()->item(0));
        QCOMPARE(s.ops.size(), n + 1);
        const Json op = s.last();
        QCOMPARE(op["op"], Json("pose_figure"));
        QCOMPARE(op["id"], before["id"]);
        QCOMPARE(op["pose_id"], Json("lie/back_knees_up"));
        QCOMPARE(op["mirror"], Json(false));
        QCOMPARE(op["keep"], Json("ground"));
        QCOMPARE((*s.figure())["pose_id"], Json("lie/back_knees_up"));
        QCOMPARE((*s.figure())["joints"], (*library::find("lie/back_knees_up"))["joints"]);
        panel->refresh();
        QVERIFY2(panel->prims->item(0)->text().contains(QStringLiteral("仰向けで膝を立てる")), qPrintable(panel->prims->item(0)->text()));
        s.trigger("act_undo");
        QVERIFY(!s.figure()->contains("pose_id"));
        QCOMPARE(*s.figure(), before);
        // 左右反転 and Enter in the grid
        g->mirror->setChecked(true);
        g->grid()->setFocus();
        g->grid()->setCurrentRow(0);
        press(g->grid(), Qt::Key_Return);
        QCOMPARE(s.last()["mirror"], Json(true));
        QCOMPARE((*s.figure())["pose_id"], Json("lie/back_knees_up~m"));
        panel->refresh();
        QVERIFY(panel->prims->item(0)->text().contains(QStringLiteral("反転")));
        // 足もとを保つ off: the pelvis kept
        g->mirror->setChecked(false);
        g->keep->setChecked(false);
        g->search->setText(QStringLiteral("stand/neutral"));
        g->grid()->setCurrentRow(0);
        const Json pos = (*s.figure())["pos"];
        QVERIFY(g->apply_chosen());
        QCOMPARE(s.last()["keep"], Json("pelvis"));
        QCOMPARE((*s.figure())["pos"], pos);
        s.trigger("act_undo");
        s.trigger("act_undo");
        QCOMPARE(*s.figure(), before);
        // nothing chosen: nothing applied
        g->grid()->setCurrentRow(-1);
        const std::size_t m = s.ops.size();
        QVERIFY(!g->apply_chosen());
        QCOMPARE(s.ops.size(), m);
    }
};

QTEST_MAIN(TestGuiPoses)
#include "test_gui_poses.moc"
