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
#include <QCryptographicHash>
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
#include <map>
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
#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/frames.hpp"
#include "render/ops_registry.hpp"
#include "core/ids.hpp"
#include "core/pynum.hpp"
#include "storage/asset_store.hpp"
#include "storage/writer.hpp"
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

    explicit Studio(int pages = 3, bool strict = false) {
        book = path_of(dir.filePath("book.genko"));
        core::Document doc = new_doc(pages);
        doc.strict_gates = strict;
        write_book(book, doc);
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

std::string sha256(const std::string& bytes) {
    return QCryptographicHash::hash(QByteArray::fromStdString(bytes), QCryptographicHash::Sha256).toHex().toStdString();
}

// A template laid on page `index` of `doc` (ids counted from 1000): the sha of its ops with the ids they give, and of the
// book saved afterwards (project.json). The same values as テンプレートでコマを割る… gave before コマ割り K2 (e9601687).
std::pair<std::string, std::string> laid_sha(const core::Document& doc, std::size_t index, const genko::app::templates::Template& chosen) {
    genko::core::ScopedIdSource ids(genko::core::counting_ids(1000));
    const genko::app::templates::Plan plan = genko::app::templates::plan(doc, index, chosen, "human:tester");
    core::Document laid;
    {
        genko::core::ScopedIdScript script(plan.ids);
        laid = core::CommandBus(genko::render::ops_registry()).apply(doc, plan.ops, core::Actor("human:tester")).doc;
    }
    Json ids_json = Json::array();
    for (const std::string& id : plan.ids) ids_json.push_back(id);
    QTemporaryDir dir;
    genko::storage::AssetStore store(dir.path().toStdString());
    return {sha256(plan.ops.dump() + ids_json.dump()), sha256(genko::storage::project_json_v4(laid, store))};
}

// A book of `pages` pages made with counted ids (and a book id of its own), `ops` applied.
core::Document counted_book(int pages, const Json& ops) {
    genko::core::ScopedIdSource ids(genko::core::counting_ids());
    core::Document doc = new_doc(pages);
    doc.book_id = "0123456789abcdef0123456789abcdef";
    if (!ops.empty()) doc = core::CommandBus(genko::render::ops_registry()).apply(doc, ops, core::Actor("human:tester")).doc;
    return doc;
}

// One's own template of page `index` as 今のコマ割りをテンプレートに残す… keeps it: before コマ割り K2 (no "frame"), or now.
genko::app::templates::Template own(const core::Document& doc, std::size_t index, bool with_frame) {
    const core::Page& page = doc.page(index);
    genko::app::templates::Template t;
    t.key = "own";
    t.mine = true;
    t.spec = Json::object({{"name", "own"}, {"size", Json::array({page.spec.width_mm.json(), page.spec.height_mm.json()})},
                           {"tree", genko::app::frame_tree(page.frames.at(0))}});
    if (with_frame) {
        const core::Rect r = page.inner_rect_mm(doc.start_side ? std::string_view(*doc.start_side) : std::string_view());
        t.spec["frame"] = Json::array({core::py_round(r.x.value(), 3), core::py_round(r.y.value(), 3), core::py_round(r.width.value(), 3),
                                       core::py_round(r.height.value(), 3)});
    }
    return t;
}

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

    // テンプレートでコマを割る… lays what it laid before コマ割り K2 (e9601687), op for op and byte for byte: drawn panels
    // (add_frame: their root is the whole paper) kept and laid again on their own page — a template of before (no
    // "frame") and one kept now —, slanted cuts the same, and a built-in template.
    void templatesLayAsBefore() {
        const Json drawn_ops = Json::array({Json{{"op", "add_frame"}, {"page", 1}, {"rect", Json::array({20, 22, 80, 60})}},
                                            Json{{"op", "add_frame"}, {"page", 1}, {"points", Json::parse("[[110, 20], [180, 20], [170, 90], [112, 84]]")}},
                                            Json{{"op", "add_frame"}, {"page", 1}, {"rect", Json::array({25, 120, 150, 70})}}});
        const core::Document drawn = counted_book(2, drawn_ops);
        QVERIFY(core::is_free(drawn.page(0).frames[0]));
        const core::Document cut = counted_book(2, Json::array({Json{{"op", "split_frame"}, {"page", 1}, {"axis", "horizontal"}, {"ratio", 0.4}, {"tilt_mm", 14}},
                                                                Json{{"op", "split_frame"}, {"page", 1}, {"axis", "vertical"}, {"ratio", 0.55}}}));
        std::map<std::string, std::pair<std::string, std::string>> got;
        got["drawn, before"] = laid_sha(drawn, 0, own(drawn, 0, false));
        got["drawn, now"] = laid_sha(drawn, 0, own(drawn, 0, true));
        got["cut, before"] = laid_sha(cut, 0, own(cut, 0, false));
        got["cut, now"] = laid_sha(cut, 0, own(cut, 0, true));
        const auto built_in = genko::app::templates::builtin();
        QVERIFY(built_in.size() > 3);
        got["built-in 0"] = laid_sha(cut, 1, built_in[0]);
        for (const auto& t : built_in) {
            if (t.spec.contains("slant")) {
                got["built-in slanted"] = laid_sha(drawn, 0, t);
                break;
            }
        }
        for (const auto& [name, sha] : got) qInfo("%s: ops %s, book %s", name.c_str(), sha.first.c_str(), sha.second.c_str());
        // (the values e9601687's templates.cpp gives: build this test against it to see them again)
        const std::map<std::string, std::pair<std::string, std::string>> before = {
            {"drawn, before", {"eeb0d19bc36875996384d0e4b085fed68848b41559d02e5b9c55585b763515be", "dccaa8d656cde1fc21b517f360c7f8cb9ca71eba18496429ca8fd58c7bfba77a"}},
            {"cut, before", {"3e9d67cc3092316d904a59b84e113e3c9dafad892a642e43dafd3bea9f422910", "0f6017b1df9e14d9bddfe9f3df106d63ba55525466853fa5b1c9972c7da28f13"}},
            {"built-in 0", {"821bf06b4dcb406ea508a4a992eadc22f29850cd208ba24aea7c29148de8ccf1", "92df3c5bb77a110b62727b1c846c941758c208e4b9d074e373ea03c7f6ffc741"}},
            {"built-in slanted", {"81bed71e0aa00f95dc03587a834ed20f1f98548027077d99cef47d26f319c2dd", "ac94808aca05ebe783406d5ee9e9e895bf6f4c05419517b21ed745f76f694150"}},
        };
        for (const auto& [name, sha] : before) {
            QVERIFY2(got.count(name) == 1, name.c_str());
            QCOMPARE(QString::fromStdString(got[name].first), QString::fromStdString(sha.first));
            QCOMPARE(QString::fromStdString(got[name].second), QString::fromStdString(sha.second));
        }
        // kept now ("frame") and laid on its own page: the same as before
        QCOMPARE(got["drawn, now"], got["drawn, before"]);
        QCOMPARE(got["cut, now"], got["cut, before"]);
        // kept now and laid on the page of the other side: the root stays the paper, each panel the same in its basic frame
        const auto side = [&drawn](std::size_t i) {
            return drawn.page(i).inner_rect_mm(drawn.start_side ? std::string_view(*drawn.start_side) : std::string_view());
        };
        const genko::app::templates::Plan plan = genko::app::templates::plan(drawn, 1, own(drawn, 0, true), "human:tester");
        core::Document other;
        {
            genko::core::ScopedIdScript script(plan.ids);
            other = core::CommandBus(genko::render::ops_registry()).apply(drawn, plan.ops, core::Actor("human:tester")).doc;
        }
        const core::Frame& root = other.page(1).frames.at(0);
        QCOMPARE(root.rect.width.value(), other.page(1).spec.width_mm.value());
        QCOMPARE(root.rect.x.value(), 0.0);
        const auto a = drawn.page(0).leaf_frames();
        const auto b = other.page(1).leaf_frames();
        QCOMPARE(a.size(), b.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            QVERIFY(std::abs((a[i]->rect.x - side(0).x).value() - (b[i]->rect.x - side(1).x).value()) < 0.02);
            QVERIFY(std::abs((a[i]->rect.width - b[i]->rect.width).value()) < 0.02);
        }
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
                // 標準 and アクション (…/dialog.png, …/dialog-action.png)
                d.resize(1000, 680);
                d.show();
                QCoreApplication::processEvents();
                d.list()->setCurrentRow(4);
                QCoreApplication::processEvents();
                QVERIFY(d.grab().save(kShot));
                const auto before = skeletons(d.batch());
                d.preset->setCurrentIndex(2);
                QVERIFY(QTest::qWaitFor([&] { return skeletons(d.batch()) != before; }, 5000));
                QVERIFY(d.wait_pictures(60000));
                d.list()->setCurrentRow(4);
                QCoreApplication::processEvents();
                QString action = kShot;
                action.replace(QStringLiteral(".png"), QStringLiteral("-action.png"));
                QVERIFY(d.grab().save(action));
                d.hide();
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
            // typed, and Enter — with the window shown, as the person has it (its button box has a default button then):
            // the batch again, the window stays, nothing laid; Enter in a number's box neither
            d.show();
            QVERIFY(QTest::qWaitForWindowExposed(&d));
            d.new_batch->click();
            d.seed->setFocus();
            d.seed->setText(QString::fromStdString(seed));
            QTest::keyClick(d.seed, Qt::Key_Return);
            QVERIFY(d.isVisible());
            QVERIFY(!d.plan);
            QCOMPARE(d.result(), 0);
            QCOMPARE(skeletons(d.batch()), first);
            for (QWidget* box : std::vector<QWidget*>{d.focus, d.min_tiers, d.panels}) {
                box->setFocus();
                QTest::keyClick(box, Qt::Key_Enter);
                QTest::keyClick(box, Qt::Key_Return);
                QVERIFY(d.isVisible());
                QVERIFY(!d.plan);
            }
            QCOMPARE(skeletons(d.batch()), first);
            // copied: the batch's seed, not what is typed in the field
            d.seed->setText(QStringLiteral("書きかけ"));
            d.copy_seed->click();
            QCOMPARE(QApplication::clipboard()->text(), QString::fromStdString(seed));
            d.seed->setText(QString::fromStdString(seed));
            d.hide();
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

    // A page whose name a person approved (strict_gates): a person's window lays an idea on it, as テンプレートでコマを割る…
    // does — the same gate (the command bus's: an agent is refused, a person not).
    void anApprovedPageIsLaidByAPerson() {
        Studio studio(3, true);
        MainWindow* w = studio.window.get();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "name_ok"}, {"page", 1}}})));
        QVERIFY(studio.page().name_ok);
        studio.komawari([&](KomawariDialog& d) {
            d.panels->setCurrentIndex(4);
            d.make_batch("approved");
            d.list()->setCurrentRow(0);
            d.choose();
            QVERIFY(d.plan.has_value());
        });
        QCOMPARE(studio.leaves(), std::size_t{4});
        QVERIFY(studio.page().name_ok);
        // an agent's change of the same ops is refused by the gate (the window is a person's)
        const genko::app::templates::Plan plan = genko::app::idea_plan(*studio.session->snapshot(), 0, ideas::generate([] {
            ideas::Request r;
            r.panels = 3;
            r.seed = "agent";
            return r;
        }()).ideas.front().skeleton, "human:tester");
        {
            // (with the ids the ops name: only the gate can refuse them — a person's same change goes through)
            const core::CommandBus bus(genko::render::ops_registry());
            QString refused;
            try {
                core::ScopedIdScript script(plan.ids);
                (void)bus.apply(*studio.session->snapshot(), plan.ops, core::Actor("ai:helper"));
            } catch (const std::exception& error) {
                refused = QString::fromUtf8(error.what());
            }
            QVERIFY2(refused.contains(QStringLiteral("the name is approved")), qPrintable(refused));
            core::ScopedIdScript script(plan.ids);
            QCOMPARE(bus.apply(*studio.session->snapshot(), plan.ops, core::Actor("human:tester")).doc.page(0).leaf_frames().size(), std::size_t{3});
        }
        // テンプレートでコマを割る… on the same page: the same answer
        studio.answers->responder->question = [](const QString&, const QString&) { return true; };
        studio.answers->responder->exec = [](QDialog* dialog) {
            auto* d = qobject_cast<genko::app::TemplateDialog*>(dialog);
            if (d == nullptr) return 0;
            d->list()->setCurrentRow(d->list()->count() - 1);
            d->choose();
            return d->result();
        };
        const Json before = panels_of(studio.page());
        studio.trigger("act_template");
        QVERIFY(panels_of(studio.page()) != before);
    }

    // A page of one drawn panel (blank, its root a node of drawn panels): the pictures are drawn, it asks first (the
    // drawn panel goes), and the idea is laid — as テンプレートでコマを割る… does. Its one panel is no base to vary.
    void aPageOfOneDrawnPanel() {
        Studio studio;
        MainWindow* w = studio.window.get();
        QVERIFY(w->apply_ops(Json::array({Json{{"op", "add_frame"}, {"page", 1}, {"rect", Json::array({30, 30, 80, 90})}}})));
        QVERIFY(core::is_free(studio.page().frames[0]));
        QCOMPARE(studio.leaves(), std::size_t{1});
        studio.answers->asked.clear();
        studio.answers->responder->question = [](const QString&, const QString& text) { return text.contains(QStringLiteral("コマ割りの案で割り直します")); };
        studio.komawari([&](KomawariDialog& d) {
            QVERIFY(d.needs_clearing);
            QVERIFY(!d.page_can_be_base());
            QVERIFY(d.wait_pictures(60000));
            for (int row = 0; row < d.list()->count(); ++row) QVERIFY(d.list()->item(row)->icon().availableSizes().front() != QSize(120, 170));
            d.panels->setCurrentIndex(5);
            d.make_batch("one-drawn");
            d.list()->setCurrentRow(0);
            d.choose();
            QVERIFY(d.plan.has_value());
        });
        QCOMPARE(studio.leaves(), std::size_t{5});
        QVERIFY(on_basic_frame(studio.doc(), studio.page()));
        studio.trigger("act_undo");
        QCOMPARE(studio.leaves(), std::size_t{1});
        QVERIFY(core::is_free(studio.page().frames[0]));
    }

    // The window closed while its pictures are being drawn (a new batch, and another, then gone): nothing is left to
    // answer to, the event loop runs on.
    void closedWhileDrawing() {
        Studio studio;
        for (int i = 0; i < 3; ++i) {
            studio.komawari([&](KomawariDialog& d) {
                d.count->setCurrentIndex(2);  // (24)
                d.make_batch({});
                d.new_batch->click();
            });
            QTest::qWait(50);
        }
        QTest::qWait(300);
        QVERIFY(studio.ops.empty());
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
        for (const int page : {2, 1, 3}) {  // (the other side, the same, and its own page)
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
