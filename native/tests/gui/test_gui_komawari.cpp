// コマ割り K2 (SPEC LAYOUT-01): ページ → コマ → コマ割りの案を出す… — the window with the 12 ideas drawn as the page cut
// by each (on worker threads: the window never waits for them), the same batch from the same seed (typed, copied and
// pasted), the exact number of panels, variations of an idea and of the page as it is and back to the batch before
// (ガチャに戻る), an idea laid on the page as real panels in one change (one Undo, saved and read back), the question on
// a page with panels or lines, and the layout kept with 今のコマ割りをテンプレートに残す… and laid again from
// テンプレートでコマを割る… (an inset's page and a page of drawn panels too, on the page it came from and on another).

#include <QtTest>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <set>

#include "app/canvas.hpp"
#include "app/dialogs.hpp"
#include "app/frame_tools.hpp"
#include "app/icons.hpp"
#include "app/komawari_dialog.hpp"
#include "app/main_window.hpp"
#include "app/templates.hpp"
#include "app/theme.hpp"
#include "core/frames.hpp"
#include "gui_support.hpp"
#include "service/komawari_shape.hpp"
#include "service/layout_ideas.hpp"

using namespace gui_test;
using genko::app::KomawariDialog;
using genko::app::MainWindow;
using genko::app::Session;
namespace core = genko::core;
namespace ideas = genko::service::layout_ideas;
namespace komawari = genko::service::komawari;

namespace {

struct Studio {
    QTemporaryDir dir;
    fs::path book;
    std::shared_ptr<Session> session;
    std::unique_ptr<Answers> answers = std::make_unique<Answers>();
    std::unique_ptr<MainWindow> window;
    std::vector<Json> ops;

    explicit Studio(int pages = 3) {
        book = path_of(dir.filePath("book.genko"));
        write_book(book, new_doc(pages));
        session = Session::open(book, quick(path_of(dir.filePath("recovery"))));
        window = std::make_unique<MainWindow>(session);
        window->resize(1280, 860);
        window->show();
        QObject::connect(session.get(), &Session::changed, window.get(), [this](const genko::app::BookChange& c) {
            if (c.why == genko::app::BookChange::Why::Edit) ops.push_back(c.ops);
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
    const core::Document& doc() const { return window->session().document(); }
    const core::Page& page() const { return *window->current_page(); }
    std::size_t leaves() const { return page().leaf_frames().size(); }
    // Open the window from the menu; `with` does what the person does in it (its answer: accepted or not).
    void komawari(const std::function<void(KomawariDialog&)>& with) {
        answers->responder->exec = [with](QDialog* dialog) {
            auto* d = qobject_cast<KomawariDialog*>(dialog);
            if (d == nullptr) return 0;
            with(*d);
            return d->result();
        };
        trigger("act_komawari");
    }
};

std::vector<std::string> skeletons(const ideas::Batch& batch) {
    std::vector<std::string> out;
    for (const ideas::Idea& idea : batch.ideas) out.push_back(idea.skeleton.dump());
    return out;
}

std::size_t overs(const core::Page& page) {
    std::size_t n = 0;
    for (const core::Frame* f : page.leaf_frames()) n += f->over ? 1 : 0;
    return n;
}

// The panels of a page as the person sees them: each one's box, look and over, in reading order (no ids).
Json panels_of(const core::Page& page) {
    Json out = Json::array();
    for (const core::Frame* f : page.leaf_frames()) {
        out.push_back(Json::object({{"rect", Json::array({core::py_round(f->rect.x.value(), 1), core::py_round(f->rect.y.value(), 1),
                                                          core::py_round(f->rect.width.value(), 1), core::py_round(f->rect.height.value(), 1)})},
                                    {"bleed", f->bleed}, {"clip", f->clip}, {"border", f->border_mm}, {"over", f->over}}));
    }
    return out;
}

// The same panels, each box from the basic frame's top left (a page of the other side has its basic frame elsewhere).
Json panels_in_frame(const core::Page& page) {
    Json out = panels_of(page);
    const core::Rect root = page.frames.at(0).rect;
    for (Json& p : out) {
        p["rect"][0] = core::py_round(p["rect"][0].get<double>() - root.x.value(), 1);
        p["rect"][1] = core::py_round(p["rect"][1].get<double>() - root.y.value(), 1);
    }
    return out;
}

// The page's panels in its own basic frame (the root where the paper's margins put it).
bool on_basic_frame(const core::Document& doc, const core::Page& page) {
    const core::Rect basic = page.inner_rect_mm(doc.start_side ? std::string_view(*doc.start_side) : std::string_view());
    const core::Rect root = page.frames.at(0).rect;
    return std::abs((root.x - basic.x).value()) < 0.05 && std::abs((root.y - basic.y).value()) < 0.05 &&
           std::abs((root.width - basic.width).value()) < 0.05 && std::abs((root.height - basic.height).value()) < 0.05;
}

const QString kShot = qEnvironmentVariable("GENKO_KOMAWARI_SHOT");

}  // namespace

class TestGuiKomawari : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        config_folder();
        qRegisterMetaType<genko::app::BookChange>();
        genko::app::icons::init_resources();
        genko::app::theme::apply(qApp);
    }

    // The window from ページ → コマ and the panel tool's settings; 12 ideas, each drawn as the page cut by it, the
    // window open before the pictures are (they are drawn on worker threads while the window keeps answering).
    void theWindowShowsTwelveIdeas() {
        Studio studio;
        MainWindow* w = studio.window.get();
        QAction* act = w->action(QStringLiteral("act_komawari"));
        QVERIFY(act != nullptr);
        QCOMPARE(act->text(), QStringLiteral("コマ割りの案を出す…"));
        bool in_menu = false;
        for (QAction* top : w->menuBar()->actions()) {
            if (top->text() != QStringLiteral("ページ") || top->menu() == nullptr) continue;
            for (QAction* sub : top->menu()->actions()) {
                if (sub->menu() == nullptr || sub->text() != QStringLiteral("コマ")) continue;
                const QList<QAction*> items = sub->menu()->actions();
                const auto at = std::find(items.begin(), items.end(), act);
                const auto beside = std::find(items.begin(), items.end(), w->action(QStringLiteral("act_template")));
                in_menu = at != items.end() && beside != items.end() && at - beside == 1;  // (next to テンプレートでコマを割る…)
            }
        }
        QVERIFY(in_menu);
        int opened = 0;
        qint64 open_ms = 0, drawn_ms = 0, longest_gap = 0;
        studio.komawari([&](KomawariDialog& d) {
            ++opened;
            QCOMPARE(d.windowTitle(), QStringLiteral("コマ割りの案を出す"));
            QCOMPARE(d.list()->count(), 12);
            QCOMPARE(d.count->currentData().toInt(), 12);
            QVERIFY(d.seed->text().startsWith(QStringLiteral("km-")));
            QVERIFY(d.state->text().contains(QStringLiteral("12 案")));
            QVERIFY(!d.varying());
            QVERIFY(!d.back->isEnabled());
            QCOMPARE(d.apply_button->text(), QStringLiteral("このページに当てる"));
            // the pictures: drawn while the event loop runs on (the gaps between its ticks small)
            QElapsedTimer clock;
            clock.start();
            qint64 last = 0;
            QTimer tick;
            QObject::connect(&tick, &QTimer::timeout, [&] {
                longest_gap = std::max(longest_gap, clock.elapsed() - last);
                last = clock.elapsed();
            });
            tick.start(10);
            QVERIFY(d.wait_pictures(60000));
            drawn_ms = clock.elapsed();
            for (int row = 0; row < d.list()->count(); ++row) {
                const QList<QSize> sizes = d.list()->item(row)->icon().availableSizes();
                QVERIFY(!sizes.isEmpty());
                QVERIFY2(sizes.front() != QSize(120, 170), qPrintable(QString::number(row)));  // (the page drawn, not the blank)
                QVERIFY(!d.list()->item(row)->toolTip().isEmpty());
            }
            // a new batch: 12 more, again at once
            QElapsedTimer again;
            again.start();
            d.new_batch->click();
            open_ms = again.elapsed();
            QCOMPARE(d.list()->count(), 12);
            QVERIFY(d.wait_pictures(60000));
            if (!kShot.isEmpty()) {
                d.resize(1000, 680);
                d.show();
                QCoreApplication::processEvents();
                d.list()->setCurrentRow(4);
                QCoreApplication::processEvents();
                QVERIFY(d.grab().save(kShot));
            }
        });
        QCOMPARE(opened, 1);
        qInfo("komawari: a batch of 12 shown in %lld ms, its pictures drawn in %lld ms, the longest wait of the event loop %lld ms",
              open_ms, drawn_ms, longest_gap);
        QVERIFY2(open_ms < 1000, qPrintable(QString::number(open_ms)));          // (a batch never holds the window: the pictures are the workers')
        QVERIFY2(longest_gap < 1000, qPrintable(QString::number(longest_gap)));
        // the panel tool's settings have it too
        w->choose_tool(QStringLiteral("frame"));
        QPushButton* button = nullptr;
        for (QPushButton* b : w->findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("案を出す…")) button = b;
        }
        QVERIFY(button != nullptr);
        QVERIFY(button->toolTip().contains(QStringLiteral("コマ割りの案を束で出し")));
        int from_button = 0;
        studio.answers->responder->exec = [&from_button](QDialog* dialog) {
            from_button += qobject_cast<KomawariDialog*>(dialog) != nullptr ? 1 : 0;
            return 0;
        };
        button->click();
        QCOMPARE(from_button, 1);
        QVERIFY(studio.ops.empty());  // (the window changed nothing)
    }

    // The same settings and seed: the same batch — typed into the field, or copied and pasted.
    void theSameSeedGivesTheSameBatch() {
        Studio studio;
        studio.komawari([&](KomawariDialog& d) {
            const std::string seed = d.batch().seed;
            const auto first = skeletons(d.batch());
            QCOMPARE(first.size(), std::size_t{12});
            d.copy_seed->click();
            QCOMPARE(QApplication::clipboard()->text(), QString::fromStdString(seed));
            d.new_batch->click();
            QVERIFY(d.batch().seed != seed);
            QVERIFY(skeletons(d.batch()) != first);
            d.paste_seed->click();
            QCOMPARE(d.batch().seed, seed);
            QCOMPARE(skeletons(d.batch()), first);
            // typed, and Enter (the window stays: Enter never lays an idea)
            d.new_batch->click();
            d.seed->setText(QString::fromStdString(seed));
            QTest::keyClick(d.seed, Qt::Key_Return);
            QVERIFY(d.isVisible() || d.result() == 0);
            QVERIFY(!d.plan);
            QCOMPARE(skeletons(d.batch()), first);
            // the same as the service gives for the request
            ideas::Request r = d.request();
            r.seed = seed;
            QCOMPARE(skeletons(ideas::generate(r)), first);
            // the settings change the batch, the seed stays
            d.preset->setCurrentIndex(2);  // (アクション)
            QVERIFY(QTest::qWaitFor([&] { return skeletons(d.batch()) != first; }, 5000));
            QCOMPARE(d.batch().seed, seed);
            QCOMPARE(d.knobs.front().second->value(), 65);  // (斜め: アクション's)
            // 5 and 24
            d.count->setCurrentIndex(0);
            d.make_batch(seed);
            QCOMPARE(d.list()->count(), 5);
            d.count->setCurrentIndex(2);
            d.make_batch(seed);
            QCOMPARE(d.list()->count(), 24);
        });
    }

    // コマ数 1〜8: every idea has exactly that many panels; 段数 and 見せ場 too.
    void anExactNumberOfPanels() {
        Studio studio;
        studio.komawari([&](KomawariDialog& d) {
            for (int n = 1; n <= 8; ++n) {
                d.panels->setCurrentIndex(n);
                d.make_batch("exact");
                QVERIFY2(d.list()->count() >= 3, qPrintable(QString::number(n)));
                for (const ideas::Idea& idea : d.batch().ideas) QCOMPARE(idea.shape.count(), std::size_t(n));
            }
            d.panels->setCurrentIndex(5);
            d.min_tiers->setValue(3);
            d.max_tiers->setValue(3);
            d.make_batch("tiers");
            QVERIFY(d.list()->count() > 0);
            for (const ideas::Idea& idea : d.batch().ideas) {
                QCOMPARE(idea.shape.count(), std::size_t{5});
                QCOMPARE(std::count(idea.shape.pattern.begin(), idea.shape.pattern.end(), '-'), 2L);  // (3 tiers)
            }
            d.min_tiers->setValue(0);
            d.max_tiers->setValue(0);
            d.focus->setValue(2);
            d.make_batch("focus");
            QVERIFY(d.list()->count() > 0);
            for (const ideas::Idea& idea : d.batch().ideas) QCOMPARE(komawari::largest(idea.shape), std::size_t{2});
            // 見せ場 past the panels: held to them
            d.panels->setCurrentIndex(1);
            QCOMPARE(d.focus->value(), 1);
        });
    }

    // これを元に変異: a batch of variations of the idea chosen (its panels' number kept), by the 変異度; ガチャに戻る:
    // the batch before, as it was, the same idea chosen.
    void variationsAndBackToTheBatch() {
        Studio studio;
        studio.komawari([&](KomawariDialog& d) {
            const auto gacha = skeletons(d.batch());
            const std::string seed = d.batch().seed;
            d.list()->setCurrentRow(3);
            const std::size_t n = d.batch().ideas[3].shape.count();
            d.variation->setValue(40);
            QVERIFY(d.vary->isEnabled());
            d.vary->click();
            QVERIFY(d.varying());
            QVERIFY(d.back->isEnabled());
            QVERIFY2(d.state->text().startsWith(QStringLiteral("変異: 案 4 を元に、変異度 0.40")), qPrintable(d.state->text()));
            QVERIFY(d.list()->count() > 0);
            QVERIFY(skeletons(d.batch()) != gacha);
            for (const ideas::Idea& idea : d.batch().ideas) QCOMPARE(idea.shape.count(), n);
            QVERIFY(d.wait_pictures(60000));
            // its seed gives it again
            const auto varied = skeletons(d.batch());
            const QString vseed = d.seed->text();
            d.new_batch->click();  // (new variations of the same idea)
            QVERIFY(d.varying());
            QVERIFY(skeletons(d.batch()) != varied);
            d.seed->setText(vseed);
            d.from_seed_field();
            QCOMPARE(skeletons(d.batch()), varied);
            // a variation of a variation, then back to the batch before them all
            d.list()->setCurrentRow(0);
            d.vary->click();
            QVERIFY(d.state->text().startsWith(QStringLiteral("変異: 変異の案 1 を元に")));
            d.back->click();
            QVERIFY(!d.varying());
            QCOMPARE(skeletons(d.batch()), gacha);
            QCOMPARE(d.batch().seed, seed);
            QCOMPARE(d.list()->currentRow(), 3);
            QVERIFY(!d.back->isEnabled());
            // a blank page as it is: one panel to vary
            QVERIFY(d.page_can_be_base());
        });
    }

    // このページに当てる: real panels, the idea's, in one change (one Undo takes them back, Redo again); saved and read
    // back the same; a page with panels asks first (no: as it was); the page as it is can be varied.
    void anIdeaLaidIsRealPanels() {
        Studio studio;
        MainWindow* w = studio.window.get();
        ideas::Idea chosen;
        studio.komawari([&](KomawariDialog& d) {
            d.knobs[4].second->setValue(100);  // (重ね: an inset or an overlap likely)
            d.make_batch("lay");
            int row = 0;
            for (int i = 0; i < d.list()->count(); ++i) {
                if (d.batch().ideas[static_cast<std::size_t>(i)].skeleton.contains("insets")) row = i;
            }
            d.list()->setCurrentRow(row);
            chosen = d.batch().ideas[static_cast<std::size_t>(row)];
            QVERIFY(!d.needs_clearing);
            d.choose();
            QVERIFY(d.plan.has_value());
        });
        QCOMPARE(studio.ops.size(), std::size_t{1});  // (one change)
        QCOMPARE(studio.leaves(), chosen.shape.count());
        QVERIFY(w->last_notice().startsWith(QStringLiteral("コマ割りの案で割りました")));
        const auto shape = komawari::shape_of_page(studio.doc(), studio.page());
        QVERIFY(shape.has_value());
        QCOMPARE(komawari::difference(*shape, chosen.shape), 0);
        const Json laid = panels_of(studio.page());
        QVERIFY(chosen.skeleton.contains("insets") ? overs(studio.page()) > 0 : true);
        studio.trigger("act_undo");
        QCOMPARE(studio.leaves(), std::size_t{1});
        studio.trigger("act_redo");
        QCOMPARE(panels_of(studio.page()), laid);
        // saved and read back
        studio.session->save_now();
        QVERIFY(studio.session->wait_saved(10s));
        const core::Document read = read_book(studio.book);
        QCOMPARE(panels_of(read.page(0)), laid);
        // a page with panels: asked first; no: as it was
        studio.answers->asked.clear();
        studio.komawari([&](KomawariDialog& d) {
            QVERIFY(d.needs_clearing);
            d.list()->setCurrentRow(1);
            d.choose();
        });
        QVERIFY2(studio.answers->asked.join(QStringLiteral(" | ")).contains(QStringLiteral("1 ページのコマと台詞を消して、コマ割りの案で割り直します")),
                 qPrintable(studio.answers->asked.join(QStringLiteral(" | "))));
        QCOMPARE(panels_of(studio.page()), laid);
        QCOMPARE(studio.ops.size(), std::size_t{1});  // (no change: Undo and Redo are not edits)
        // yes, with a line on the page: the line and the panels made again, one Undo back to both
        const core::Num number = studio.page().index;
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_line"}, {"page", number.json()}, {"text", "消える台詞"}, {"x_mm", 40}, {"y_mm", 60}}})));
        studio.answers->responder->question = [](const QString&, const QString& text) { return text.contains(QStringLiteral("コマ割りの案で割り直します")); };
        ideas::Idea second;
        studio.komawari([&](KomawariDialog& d) {
            d.panels->setCurrentIndex(3);
            d.make_batch("again");
            d.list()->setCurrentRow(0);
            second = d.batch().ideas.front();
            d.choose();
        });
        QCOMPARE(studio.leaves(), std::size_t{3});
        QVERIFY(studio.doc().story_for_page(number).empty());
        studio.trigger("act_undo");
        QCOMPARE(panels_of(studio.page()), laid);
        QCOMPARE(studio.doc().story_for_page(number).size(), std::size_t{1});
        studio.trigger("act_redo");
        // the page as it is, varied
        studio.komawari([&](KomawariDialog& d) {
            QVERIFY(d.page_can_be_base());
            d.base->setCurrentIndex(1);
            QVERIFY(d.vary->isEnabled());
            d.vary->click();
            QVERIFY(d.state->text().startsWith(QStringLiteral("変異: 今のページのコマ割り を元に")));
            QVERIFY(d.list()->count() > 0);
            for (const ideas::Idea& idea : d.batch().ideas) QCOMPARE(idea.shape.count(), std::size_t{3});
        });
    }

    // 今のコマ割りをテンプレートに残す… after an idea with an inset, and テンプレートでコマを割る… with it on another page
    // (of the other side) and on the same page: the same panels in the basic frame, the inset over its host. A page of
    // drawn panels too.
    void keptAsATemplateAndLaidAgain() {
        Studio studio(4);
        MainWindow* w = studio.window.get();
        ideas::Idea chosen;
        studio.komawari([&](KomawariDialog& d) {
            d.knobs[4].second->setValue(100);
            for (const char* seed : {"inset-1", "inset-2", "inset-3", "inset-4"}) {
                d.make_batch(seed);
                for (const ideas::Idea& idea : d.batch().ideas) {
                    if (idea.skeleton.contains("insets") && chosen.skeleton.is_null()) chosen = idea;
                }
            }
            QVERIFY(!chosen.skeleton.is_null());
            d.make_batch({});
            d.plan = genko::app::idea_plan(*studio.session->snapshot(), 0, chosen.skeleton, "human:tester");
            d.chosen = QString::fromStdString(chosen.summary);
            d.accept();
        });
        QVERIFY(overs(studio.page()) > 0);
        const Json laid = panels_in_frame(studio.page());
        studio.answers->responder->get_text = [](const QString& title, const QString&, const QString&) {
            return title == QStringLiteral("テンプレートに残す") ? std::optional<QString>(QStringLiteral("差し込みの案")) : std::nullopt;
        };
        studio.trigger("act_save_template");
        QVERIFY(w->last_notice().startsWith(QStringLiteral("コマ割りを「差し込みの案」として残しました")));
        const auto lay_mine = [&](const QString& name) {
            studio.answers->responder->exec = [name](QDialog* dialog) {
                auto* d = qobject_cast<genko::app::TemplateDialog*>(dialog);
                if (d == nullptr) return 0;
                for (int row = 0; row < d->list()->count(); ++row) {
                    if (d->list()->item(row)->text() == QStringLiteral("自分: ") + name) d->list()->setCurrentRow(row);
                }
                if (!d->wait_pictures(30000)) return 0;
                d->choose();
                return d->result();
            };
            studio.trigger("act_template");
        };
        studio.answers->responder->question = [](const QString&, const QString& text) { return text.contains(QStringLiteral("テンプレートで割り直します")); };
        for (const int page : {1, 2, 0}) {  // (the other side, the same side, the page itself)
            w->select_page(page);
            const std::size_t before = studio.ops.size();
            lay_mine(QStringLiteral("差し込みの案"));
            QCOMPARE(studio.ops.size(), before + 1);
            QCOMPARE(panels_in_frame(studio.page()), laid);
            QVERIFY(on_basic_frame(studio.doc(), studio.page()));
            QCOMPARE(overs(studio.page()), overs(studio.doc().page(0)));
            studio.trigger("act_undo");
            QCOMPARE(panels_in_frame(studio.page()) == laid, page == 0);  // (back to what it was: page 1 had the idea)
            studio.trigger("act_redo");
            QCOMPARE(panels_in_frame(studio.page()), laid);
        }
        // a page of drawn panels kept and laid again
        w->select_page(3);
        const core::Page& drawn = studio.page();
        const core::Rect inner = drawn.frames.at(0).rect;
        const double x = inner.x.value(), y = inner.y.value(), wd = inner.width.value(), ht = inner.height.value();
        const Json tree = Json{{"rect_mm", Json::array({x, y, wd, ht})}, {"axis", "free"},
                               {"children", Json::array({Json{{"rect_mm", Json::array({x, y, wd, ht / 2 - 3})}, {"custom", true}},
                                                         Json{{"rect_mm", Json::array({x + 10, y + ht / 2 + 3, wd - 20, ht / 2 - 3})}, {"custom", true}}})}};
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "set_layout"}, {"page", drawn.index.json()}, {"tree", tree}, {"force", true}}})));
        const Json hand = panels_in_frame(studio.page());
        studio.answers->responder->get_text = [](const QString& title, const QString&, const QString&) {
            return title == QStringLiteral("テンプレートに残す") ? std::optional<QString>(QStringLiteral("描いたコマ")) : std::nullopt;
        };
        studio.trigger("act_save_template");
        for (const int page : {2, 1}) {
            w->select_page(page);
            lay_mine(QStringLiteral("描いたコマ"));
            QCOMPARE(panels_in_frame(studio.page()), hand);
            QVERIFY(on_basic_frame(studio.doc(), studio.page()));
        }
        // saved and read back: the inset's page and the drawn one
        studio.session->save_now();
        QVERIFY(studio.session->wait_saved(10s));
        const core::Document read = read_book(studio.book);
        QCOMPARE(panels_in_frame(read.page(0)), laid);
        QCOMPARE(panels_in_frame(read.page(1)), hand);
        QCOMPARE(panels_in_frame(read.page(2)), hand);
    }
};

QTEST_MAIN(TestGuiKomawari)
#include "test_gui_komawari.moc"
