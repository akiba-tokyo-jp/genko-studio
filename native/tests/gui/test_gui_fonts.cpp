// 書体 (fonts@1, 書体の追加 F2-C) in the window: the palette of lettering faces.
//
//   - the dock 書体 (a tab beside 台詞, in ウィンドウ and ページ → 台詞, closed until asked for, kept with the layout), its
//     faces under their categories, each with a sample the renderer set, a mark for a face without kanji and for one whose
//     file is not on the computer; nothing read or drawn before it is shown, the samples one at a time after;
//   - the search; a pick giving the chosen line the face in one step, or the next line placed, a chosen ruby, the book's
//     usual face; a face that is not installed never picked;
//   - the lists of before as they were (the text tool's seven), the 文字 panel's list holding the forty by name with no
//     font of theirs added to Qt; what is said when a book is opened; "replace all missing fonts" leaving Genko's own
//     faces in the book;
//   - every word a person reads here: Japanese, one register, no run of four Latin letters or more, no key.
//
// GENKO_FONTS_GUI_OUT=<folder> also writes the screenshots there (the pictures a person looks at).

#include <QComboBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTextDocumentFragment>
#include <QtTest>

#include <functional>

#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/export_dialog.hpp"
#include "app/font_palette.hpp"
#include "app/ime.hpp"
#include "app/inject.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/story_panel.hpp"
#include "app/theme.hpp"
#include "app/tool_settings.hpp"
#include "app/type_panels.hpp"
#include "core/font_catalog.hpp"
#include "core/ruby.hpp"
#include "gui_support.hpp"
#include "render/font_files.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace inject = genko::app::inject;
namespace files = genko::render::font_files;

namespace {

std::string source_fonts() { return (genko::test::repo_root() + QStringLiteral("/native/resources/fonts")).toStdString(); }

void use_source_fonts() {
    files::set_dir_for_testing(source_fonts());
    render::text::clear_layout_cache();
}

struct Studio {
    QTemporaryDir tmp;
    QTemporaryDir config;
    QByteArray previous_config;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    std::vector<Json> ops;  // every op applied, in order
    // (edit: the book as it is on disk before it opens)
    // (announced false: the book as a build of before 書体の追加 wrote it — its features without fonts@1; later: a
    // feature of a build after this one in its list — the book opens read-only)
    explicit Studio(const std::function<void(core::Document&)>& edit = {}, bool announced = true, const char* later = nullptr) {
        (void)gui_test::config_folder();
        previous_config = qgetenv("GENKO_CONFIG_DIR");
        qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
        const auto path = gui_test::path_of(tmp.path() + "/book");
        core::Document doc = gui_test::new_doc(2);
        if (edit) edit(doc);
        gui_test::write_book(path, doc);
        if (!announced || later != nullptr) {
            const QString file = tmp.path() + QStringLiteral("/book/project.json");
            Json project = core::parse_python_json(genko::test::read_bytes(file));
            if (!announced) project["features"] = Json::array();
            if (later != nullptr) project["features"].push_back(later);
            genko::test::write_bytes(file, core::dump_python_indent2(project) + "\n");
        }
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1300, 900);
        window->show();
        window->activateWindow();
        (void)QTest::qWaitForWindowActive(window.get());
        canvas()->fit_page();
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit)
                for (const Json& op : c.ops) ops.push_back(op);
        });
        QCoreApplication::processEvents();  // (what is said of the book's faces when it is opened)
    }
    ~Studio() {
        window.reset();
        qputenv("GENKO_CONFIG_DIR", previous_config);
    }
    app::PageCanvas* canvas() const { return window->canvas(); }
    app::StoryPanel& panel() const { return *window->story(); }
    app::FontPalette& palette() const { return *window->font_palette(); }
    const core::Page& page() const { return window->book().page(static_cast<std::size_t>(window->page_index())); }
    std::string frame_id() const { return page().leaf_frames().front()->id; }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    Json last() const { return ops.empty() ? Json() : ops.back(); }
    const core::StoryLine* line(const std::string& id) const { return window->line_by_id(id); }
    std::string put(Json op) {
        if (!op.contains("page")) op["page"] = page().index.json();
        if (!op.contains("frame_id")) op["frame_id"] = frame_id();
        const std::size_t before = window->book().story.size();
        if (!window->apply_ops(Json::array({op}))) return {};
        return window->book().story.size() > before ? window->book().story.back().id : std::string();
    }
    std::string a_line(const char* text = "かなと漢字！") {
        return put(Json{{"op", "add_line"}, {"text", text}, {"balloon", "speech"}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 40}});
    }
    void choose(const std::string& id) {
        trigger("act_select");
        canvas()->selected_line_id = id;
        window->on_line_selected(id, false);
    }
    void choose_none() {
        canvas()->selected_line_id.reset();
        panel().list->setCurrentRow(-1);
    }
    void open_palette() {
        window->show_dock(QStringLiteral("書体"));
        QCoreApplication::processEvents();
    }
    void click(const QString& key) {
        QListWidgetItem* item = palette().item(key);
        QVERIFY2(item != nullptr, qPrintable(key));
        emit palette().list()->itemClicked(item);
    }
    void pick(const QString& key, const char* how) { emit palette().picked(key, QString::fromLatin1(how)); }
    QString font_of(const std::string& id) const {
        const core::StoryLine* l = line(id);
        return l != nullptr && l->style.is_object() && l->style.contains("font") && l->style["font"].is_string()
                   ? QString::fromStdString(l->style["font"].get<std::string>())
                   : QString();
    }
    QMenu* menu_at(const QString& path) const {
        QMenu* found = nullptr;
        std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& at) {
            if (at == path) found = menu;
            for (QAction* a : menu->actions())
                if (a->menu() != nullptr) walk(a->menu(), at + QStringLiteral("/") + a->menu()->title());
        };
        for (QAction* top : window->menuBar()->actions())
            if (top->menu() != nullptr) walk(top->menu(), top->menu()->title());
        return found;
    }
    // words typed with the text tool at a place of the page, and kept (Ctrl+Enter): the add_line made
    Json type_at(const QPointF& mm, const QString& words) {
        const std::size_t before = ops.size();
        inject::click_mm(canvas(), mm);
        app::InlineEditor* editor = canvas()->editor();
        if (editor == nullptr) return Json();
        QInputMethodEvent event;
        event.setCommitString(words);
        QCoreApplication::sendEvent(editor, &event);
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QCoreApplication::processEvents();
        for (std::size_t i = before; i < ops.size(); ++i) {
            if (ops[i].value("op", std::string()) == "add_line") return ops[i];
        }
        return Json();
    }
};

core::StoryLine a_story_line(const char* id, const std::string& font, double x = 40) {
    core::StoryLine line;
    line.id = id;
    line.page_index = core::Num(1);
    line.text = "かなと漢字";
    line.balloon = "none";
    line.wrap = "vertical";
    line.x_mm = core::Num(x);
    line.y_mm = core::Num(40.0);
    line.w_mm = core::Num(24.0);
    line.h_mm = core::Num(60.0);
    line.style = Json{{"size_mm", 5.0}};
    if (!font.empty()) line.style["font"] = font;
    return line;
}

// What a person reads: Japanese, with no run of four Latin letters or more and no key of a face or an op.
QString unreadable(const QString& text) {
    static const QRegularExpression latin(QStringLiteral("[A-Za-z]{4,}"));
    static const QRegularExpression kana(QStringLiteral("[ぁ-んァ-ヶ一-龥]"));
    if (text.isEmpty()) return QStringLiteral("(empty)");
    if (!kana.match(text).hasMatch()) return QStringLiteral("no Japanese: ") + text;
    if (latin.match(text).hasMatch()) return QStringLiteral("a Latin word: ") + text;
    if (text.contains(QLatin1Char('_'))) return QStringLiteral("a key: ") + text;
    return {};
}

}  // namespace

class TestGuiFonts : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        (void)gui_test::config_folder();
        use_source_fonts();
    }
    void init() {
        if (files::dir() != source_fonts()) use_source_fonts();
    }

    // The palette: a dock named 書体, a tab beside 台詞, in ウィンドウ and ページ → 台詞, closed until asked for; its rows the
    // eleven categories as headings (not to be chosen) and under each its faces — the seven of before and the forty —,
    // アンチック first of all; a face's row says its name, a tooltip what it is for.
    void thePaletteListsTheFacesByCategory() {
        Studio s;
        QDockWidget* dock = s.window->font_dock();
        QVERIFY(dock != nullptr);
        QCOMPARE(dock->windowTitle(), QStringLiteral("書体"));
        QCOMPARE(dock->objectName(), QStringLiteral("書体"));
        QCOMPARE(dock->widget(), static_cast<QWidget*>(&s.palette()));
        QVERIFY(!dock->isVisible());
        QVERIFY(dock->features().testFlag(QDockWidget::DockWidgetClosable));
        QMenu* windows = s.menu_at(QStringLiteral("ウィンドウ"));
        QVERIFY(windows != nullptr && windows->actions().contains(dock->toggleViewAction()));
        QAction* open = s.window->action(QStringLiteral("act_font_palette"));
        QVERIFY(open != nullptr);
        QCOMPARE(open->text(), QStringLiteral("書体のパレットを開く"));
        QVERIFY(open->shortcuts().isEmpty());
        QVERIFY(s.menu_at(QStringLiteral("ページ/台詞"))->actions().contains(open));
        open->trigger();
        QVERIFY(dock->isVisible());
        QVERIFY(s.window->tabifiedDockWidgets(dock).contains(s.window->findChild<QDockWidget*>(QStringLiteral("台詞"))));

        QListWidget* list = s.palette().list();
        const auto& faces = app::lettering::catalog_faces();
        const auto& categories = app::lettering::face_categories();
        QCOMPARE(faces.size(), std::size_t(47));
        QCOMPARE(categories.size(), std::size_t(11));
        QCOMPARE(list->count(), 47 + 11);
        // (the rows in order: a heading, its faces)
        int row = 0;
        std::size_t face = 0;
        for (const auto& [id, name] : categories) {
            QListWidgetItem* head = list->item(row++);
            QCOMPARE(head->text(), name);
            QCOMPARE(head, s.palette().heading(id));
            QVERIFY(!head->flags().testFlag(Qt::ItemIsSelectable) && !head->flags().testFlag(Qt::ItemIsEnabled));
            QVERIFY(head->font().bold());
            int under = 0;
            while (face < faces.size() && faces[face].category == id) {
                QListWidgetItem* item = list->item(row++);
                QCOMPARE(item, s.palette().item(faces[face].key));
                QVERIFY2(item->text().startsWith(faces[face].row), qPrintable(item->text()));
                QVERIFY(item->toolTip().startsWith(faces[face].row + QLatin1Char('\n') + faces[face].use));
                QVERIFY(item->flags().testFlag(Qt::ItemIsSelectable));
                ++face;
                ++under;
            }
            QVERIFY2(under >= 1, qPrintable(name));
        }
        QCOMPARE(face, faces.size());
        QCOMPARE(row, list->count());
        QCOMPARE(faces[0].key, QStringLiteral("antique"));
        // (アンチック: this book's usual face — it has none of its own —, and its row says so)
        QCOMPARE(faces[0].label, QStringLiteral("アンチック"));
        QCOMPARE(list->item(1)->text(), QStringLiteral("アンチック\nこの本のいつもの書体"));
        QCOMPARE(s.palette().usual(), QStringLiteral("antique"));
        // (one name for a face wherever it is read: the six of before keep the names the text tool has always shown,
        // with their font's own name in brackets in the palette's row — the names the text tool's 書体 lists)
        const QMap<QString, QPair<QString, QString>> six = {
            {QStringLiteral("gothic"), {QStringLiteral("ゴシック"), QStringLiteral("ゴシック（ZEN角ゴシック B）")}},
            {QStringLiteral("mincho"), {QStringLiteral("明朝"), QStringLiteral("明朝（ZENオールド明朝 SB）")}},
            {QStringLiteral("maru"), {QStringLiteral("丸ゴシック"), QStringLiteral("丸ゴシック（ZEN丸ゴシック B）")}},
            {QStringLiteral("hand"), {QStringLiteral("手書き風"), QStringLiteral("手書き風（よもぎ）")}},
            {QStringLiteral("sfx"), {QStringLiteral("極太（効果音）"), QStringLiteral("極太（効果音・デラゴシック）")}},
            {QStringLiteral("sfx_pop"), {QStringLiteral("勢い（効果音）"), QStringLiteral("勢い（効果音・レゲエ）")}}};
        int of_before = 0;
        for (const auto& f : faces) {
            if (f.added || f.key == QLatin1String("antique")) {
                QCOMPARE(f.row, f.label);
                continue;
            }
            QVERIFY2(six.contains(f.key), qPrintable(f.key));
            QCOMPARE(f.label, six[f.key].first);
            QCOMPARE(f.row, six[f.key].second);
            QCOMPARE(app::lettering::face_label(f.key), f.label);
            const int at = s.window->text_settings()->font->findData(f.key);
            QVERIFY(at > 0);
            QCOMPARE(s.window->text_settings()->font->itemText(at), f.label);
            ++of_before;
        }
        QCOMPARE(of_before, 6);
        // (every key of the catalogue, and the composite of before, once)
        QSet<QString> keys;
        for (const auto& f : faces) keys.insert(f.key);
        QCOMPARE(keys.size(), 47);
        for (const core::fonts::Entry& e : core::fonts::entries()) QVERIFY2(keys.contains(QString::fromStdString(e.key)), e.key.c_str());
        // a row: the name on a line of its own above the sample, and under the name what there is to know of the face —
        // whole, not cut, in the dock as it opens; the row as tall as both
        const int room = list->viewport()->width() - 16;
        for (const auto& f : faces) {
            QListWidgetItem* item = s.palette().item(f.key);
            QFont bold = item->font();
            bold.setBold(true);
            QFont lesser = item->font();
            lesser.setPointSizeF(lesser.pointSizeF() * 0.86);
            QVERIFY2(QFontMetrics(bold).horizontalAdvance(f.row) <= room, qPrintable(f.row));
            QVERIFY2(QFontMetrics(lesser).horizontalAdvance(f.mark) <= room, qPrintable(f.mark));
            const int lines = (f.mark.isEmpty() ? 1 : 2) + (f.key == QLatin1String("antique") ? 1 : 0);
            QVERIFY2(list->visualItemRect(item).height() >= QFontMetrics(item->font()).height() * lines + app::face_sample_size().height(), qPrintable(f.label));
            QVERIFY(list->visualItemRect(item).width() >= app::face_sample_size().width() + 16);
        }
        // reached with the keys: Enter on a row picks it
        QCOMPARE(s.palette().focusProxy(), static_cast<QWidget*>(list));
        QSignalSpy picked(&s.palette(), &app::FontPalette::picked);
        list->setCurrentItem(s.palette().item(QStringLiteral("klee")));
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked[0][0].toString(), QStringLiteral("klee"));
    }

    // Opening the window reads no font file and sets no sample; shown, the samples come one at a time between the
    // screen's events — never all at once —, each face's file read for its own sample.
    void noSampleIsMadeBeforeItIsShown() {
        files::clear_cache();
        Studio s;
        QCOMPARE(s.palette().samples_made(), 0);
        QVERIFY(!s.palette().samples_drawn());
        QCOMPARE(files::cached_bytes(), std::uint64_t(0));
        (void)s.a_line();  // (work on the page in the faces of before: still nothing of the forty read)
        QCoreApplication::processEvents();
        QCOMPARE(s.palette().samples_made(), 0);
        QCOMPARE(files::cached_bytes(), std::uint64_t(0));
        s.window->show_dock(QStringLiteral("書体"));
        QCOMPARE(s.palette().samples_made(), 0);  // (shown: asked for, none made in the same turn)
        QCOMPARE(s.palette().files_read_ahead(), 0);
        QCoreApplication::processEvents();
        const int after_one_turn = s.palette().samples_made();
        QVERIFY2(after_one_turn >= 1 && after_one_turn < 47, qPrintable(QString::number(after_one_turn)));
        // (never the list in a turn; each of the forty's files read ahead of its sample by a worker, not by the screen's
        // thread — which only sets the letters from the bytes that are there)
        QElapsedTimer all, turn;
        all.start();
        qint64 slowest = 0;
        int most = 0, turns = 0;
        while (!s.palette().samples_drawn() && all.elapsed() < 60000) {
            const int before_turn = s.palette().samples_made();
            turn.restart();
            QTest::qWait(2);
            slowest = std::max(slowest, turn.elapsed());
            most = std::max(most, s.palette().samples_made() - before_turn);
            ++turns;
        }
        qInfo("the samples: %lld ms in all, %d turns, the slowest turn %lld ms, %d samples at the most in a turn, the slowest sample %d ms",
              static_cast<long long>(all.elapsed()), turns, static_cast<long long>(slowest), most, s.palette().slowest_sample_ms());
        QVERIFY2(most < 47, qPrintable(QString::number(most)));
        QCOMPARE(s.palette().files_read_ahead(), 40);
        QVERIFY(s.palette().samples_drawn());
        QCOMPARE(s.palette().samples_made(), 47);
        QVERIFY(files::cached_bytes() > 0 && files::cached_bytes() <= files::kCacheBytes);
        for (const auto& face : app::lettering::catalog_faces()) QVERIFY2(!s.palette().item(face.key)->icon().isNull(), qPrintable(face.key));
        // (hidden and shown again: nothing made twice)
        s.window->font_dock()->hide();
        s.window->show_dock(QStringLiteral("書体"));
        QCoreApplication::processEvents();
        QCOMPARE(s.palette().samples_made(), 47);
    }

    // A sample is what the renderer sets for a line in the face: 「あア漢字！」 across — the letters' own shapes (their
    // coverage is the lettering's), the kanji of a face without them its partner's, each face's sample its own.
    void aSampleIsWhatTheRendererSets() {
        const QSize room = app::face_sample_size();
        QMap<QString, QByteArray> seen;
        for (const char* key : {"antique", "gothic", "klee", "gothic_kiwami", "cute_daruma", "dot12", "brush_syuku", "sfx_outline"}) {
            const QImage sample = app::face_sample(QString::fromLatin1(key));
            QVERIFY2(!sample.isNull(), key);
            QCOMPARE(sample.size(), room);
            core::StoryLine line;
            line.text = "あア漢字！";
            line.balloon = "none";
            line.wrap = "horizontal";
            line.w_mm = core::Num(60.0);
            line.h_mm = core::Num(10.0);
            line.style = Json{{"font", key}, {"size_mm", 4.6}};
            const render::text::Layout layout = render::text::text_layout(line, 96);
            const render::Image alpha = layout.image.getchannel(layout.image.bands() - 1);
            // (the lettering fits the sample's room for these faces: its coverage, pixel for pixel, from the left about the middle)
            QVERIFY2(alpha.width() <= room.width() && alpha.height() <= room.height(), key);
            const std::string cover = alpha.tobytes();
            const int top = (room.height() - alpha.height()) / 2;
            bool same = true;
            for (int y = 0; y < room.height() && same; ++y) {
                for (int x = 0; x < room.width() && same; ++x) {
                    const int ly = y - top;
                    const int expected = ly >= 0 && ly < alpha.height() && x < alpha.width()
                                             ? static_cast<unsigned char>(cover[static_cast<std::size_t>(ly) * static_cast<std::size_t>(alpha.width()) + static_cast<std::size_t>(x)])
                                             : 0;
                    same = qAbs(qAlpha(sample.pixel(x, y)) - expected) <= 1;
                }
            }
            QVERIFY2(same, key);
            const QByteArray bytes(reinterpret_cast<const char*>(sample.constBits()), static_cast<qsizetype>(sample.sizeInBytes()));
            QVERIFY2(!seen.values().contains(bytes), key);
            seen.insert(QString::fromLatin1(key), bytes);
        }
        // (twice the pixels on a screen of twice the scale)
        const QImage twice = app::face_sample(QStringLiteral("klee"), 2.0);
        QCOMPARE(twice.size(), room * 2);
        QCOMPARE(twice.devicePixelRatio(), 2.0);
    }

    // The box above narrows the list by a face's name, its use, its category — and by its key or the font's own name,
    // for those who know them —; a category with no face left hides its heading; nothing found is said.
    void searchNarrowsByNameKeyAndUse() {
        Studio s;
        s.open_palette();
        QLineEdit* box = s.palette().search();
        QCOMPARE(box->placeholderText(), QStringLiteral("書体をさがす（名前・使い道）"));
        QCOMPARE(s.palette().shown().size(), 47);
        box->setText(QStringLiteral("ホラー"));
        const QStringList horror = s.palette().shown();
        QVERIFY(horror.contains(QStringLiteral("horror_tegomin")) && horror.contains(QStringLiteral("horror_chokoku")));
        QVERIFY(horror.size() < 6);
        QVERIFY(!s.palette().heading(QStringLiteral("horror"))->isHidden());
        QVERIFY(s.palette().heading(QStringLiteral("maru"))->isHidden());
        box->setText(QStringLiteral("源暎"));
        QCOMPARE(s.palette().shown(), (QStringList{QStringLiteral("antique_genei"), QStringLiteral("gothic_kiwami")}));
        box->setText(QStringLiteral("klee"));
        QCOMPARE(s.palette().shown(), QStringList{QStringLiteral("klee")});
        box->setText(QStringLiteral("Yusei"));  // (the font's own name, any case)
        QCOMPARE(s.palette().shown(), QStringList{QStringLiteral("marker_yusei")});
        box->setText(QStringLiteral("叫び 極太"));  // (two words: both)
        const QStringList shouts = s.palette().shown();
        QVERIFY(shouts.contains(QStringLiteral("gothic_black")) && shouts.contains(QStringLiteral("mincho_black")));
        QVERIFY(!shouts.contains(QStringLiteral("klee")));
        box->setText(QStringLiteral("かなだけ"));
        QCOMPARE(s.palette().shown().size(), 6);
        QLabel* none = s.palette().findChild<QLabel*>(QStringLiteral("font_none"));
        QVERIFY(none != nullptr && !none->isVisible());
        box->setText(QStringLiteral("ぬぬぬ"));
        QVERIFY(s.palette().shown().isEmpty());
        QVERIFY(none->isVisible());
        QCOMPARE(none->text(), QStringLiteral("当てはまる書体がありません"));
        for (const auto& [id, name] : app::lettering::face_categories()) QVERIFY(s.palette().heading(id)->isHidden());
        box->clear();
        QCOMPARE(s.palette().shown().size(), 47);
        QVERIFY(!none->isVisible());
    }

    // A click with a line chosen and the select tool in hand gives that line the face: one edit of its style, one step of
    // 元に戻す, said. The same face again changes nothing and makes no step. A face without kanji says what draws them.
    void aPickGivesTheChosenLineTheFaceInOneStep() {
        Studio s;
        const std::string id = s.a_line();
        s.open_palette();
        s.choose(id);
        const std::size_t before = s.ops.size();
        const std::size_t steps = s.session->changes_in_memory();
        s.click(QStringLiteral("gothic_kiwami"));
        QCOMPARE(s.ops.size(), before + 1);
        QCOMPARE(s.session->changes_in_memory(), steps + 1);
        QCOMPARE(s.last(), (Json{{"op", "edit_line"}, {"id", id}, {"style", Json{{"font", "gothic_kiwami"}}}}));
        QCOMPARE(s.font_of(id), QStringLiteral("gothic_kiwami"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("選んだ台詞の書体を「源暎きわみゴ」にしました（元に戻す で取り消せます）"));
        QVERIFY(core::fonts::uses_feature(s.window->book()));
        QVERIFY(s.window->armed_face().isEmpty());                  // (the line's face, not the next line's)
        QCOMPARE(s.window->text_settings()->font->count(), 7);
        // (the same again: nothing)
        s.click(QStringLiteral("gothic_kiwami"));
        QCOMPARE(s.ops.size(), before + 1);
        QCOMPARE(s.session->changes_in_memory(), steps + 1);
        QCOMPARE(s.window->last_notice(), QStringLiteral("選んだ台詞は、もう「源暎きわみゴ」です"));
        // (a face without kanji: its partner said)
        s.click(QStringLiteral("cute_daruma"));
        QCOMPARE(s.font_of(id), QStringLiteral("cute_daruma"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("選んだ台詞の書体を「だるまどろっぷ」にしました（漢字は「モッチーポップ」で描きます）"));
        // (a face of before, and アンチック by its name)
        s.click(QStringLiteral("maru"));
        QCOMPARE(s.font_of(id), QStringLiteral("maru"));
        s.click(QStringLiteral("antique"));
        QCOMPARE(s.font_of(id), QStringLiteral("antique"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("選んだ台詞の書体を「アンチック」にしました（元に戻す で取り消せます）"));
        QVERIFY(!core::fonts::uses_feature(s.window->book()));
        // 元に戻す takes each back, one at a time
        s.trigger("act_undo");
        QCOMPARE(s.font_of(id), QStringLiteral("maru"));
        s.trigger("act_undo");
        QCOMPARE(s.font_of(id), QStringLiteral("cute_daruma"));
        // (the face's own menu: the chosen line, whatever tool is in hand; with no line chosen, said and nothing done)
        s.trigger("act_line_type");
        s.canvas()->selected_line_id = id;
        const std::size_t now = s.ops.size();
        s.pick(QStringLiteral("brush_syuku"), "line");
        QCOMPARE(s.ops.size(), now + 1);
        QCOMPARE(s.font_of(id), QStringLiteral("brush_syuku"));
    }

    // With the text tool in hand, or no line chosen, a click makes the face the next line's: nothing in the book changes;
    // the text tool's 書体 gains that one face, chosen; the row is marked; the next line typed is in it. Another pick
    // takes its place; the text tool's own choice lets it go.
    void aPickWithTheTextToolIsTheNextLinesFace() {
        Studio s;
        s.open_palette();
        s.choose_none();
        s.trigger("act_select");
        const std::size_t before = s.ops.size();
        s.click(QStringLiteral("horror_tegomin"));
        QCOMPARE(s.ops.size(), before);
        QCOMPARE(s.window->armed_face(), QStringLiteral("horror_tegomin"));
        QCOMPARE(s.palette().marked(), QStringLiteral("horror_tegomin"));
        QVERIFY(s.palette().item(QStringLiteral("horror_tegomin"))->font().bold());
        QVERIFY(app::PageCanvas::is_text_tool(s.canvas()->tool()));  // (the text tool taken in hand)
        QComboBox* font = s.window->text_settings()->font;
        QCOMPARE(font->count(), 8);
        QCOMPARE(font->currentData().toString(), QStringLiteral("horror_tegomin"));
        QCOMPARE(font->currentText(), QStringLiteral("ニューテゴミン"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("書体「ニューテゴミン」を選びました。置きたい所をクリックして、台詞を打ちます"));
        const Json made = s.type_at(QPointF(100, 100), QStringLiteral("こわい"));
        QVERIFY2(made.is_object(), "no line was typed");
        QCOMPARE(made["style"]["font"], Json("horror_tegomin"));
        // (the line typed is the chosen one now, the select tool in hand: a click there would be that line's face)
        s.choose_none();
        // (another: in its place — the box never grows past one more than the seven)
        s.click(QStringLiteral("dot16"));
        QCOMPARE(font->count(), 8);
        QCOMPARE(font->currentData().toString(), QStringLiteral("dot16"));
        QCOMPARE(s.palette().marked(), QStringLiteral("dot16"));
        QVERIFY(!s.palette().item(QStringLiteral("horror_tegomin"))->font().bold());
        // (a face of before from the palette: the box's own row, nothing added)
        s.click(QStringLiteral("maru"));
        QCOMPARE(font->count(), 7);
        QCOMPARE(font->currentData().toString(), QStringLiteral("maru"));
        QCOMPARE(s.palette().marked(), QStringLiteral("maru"));
        s.click(QStringLiteral("antique"));
        QCOMPARE(font->currentIndex(), 0);
        // (the text tool's own choice: the palette's mark follows it)
        s.click(QStringLiteral("klee"));
        font->setCurrentIndex(font->findData(QStringLiteral("gothic")));
        emit font->activated(font->currentIndex());
        QCOMPARE(s.window->armed_face(), QStringLiteral("gothic"));
        QCOMPARE(s.palette().marked(), QStringLiteral("gothic"));
        // (the face's own menu, a line chosen all the same: 次に置く台詞)
        const std::string id = s.a_line();
        s.choose(id);
        const std::size_t now = s.ops.size();
        s.pick(QStringLiteral("stick"), "next");
        QCOMPARE(s.ops.size(), now);
        QCOMPARE(s.font_of(id), QString());
        QCOMPARE(s.window->armed_face(), QStringLiteral("stick"));
    }

    // The faces without kanji, or with only some, say so under their names, with the face that draws the rest; no other
    // face has a mark.
    void kanaOnlyFacesAreMarked() {
        Studio s;
        const QMap<QString, QString> marks = {{QStringLiteral("cute_cherry"), QStringLiteral("かなだけ（漢字はZEN丸ゴシック Bk）")},
                                              {QStringLiteral("cute_daruma"), QStringLiteral("かなだけ（漢字はモッチーポップ）")},
                                              {QStringLiteral("hand_slackside"), QStringLiteral("かなだけ（漢字は油性マジック）")},
                                              {QStringLiteral("horror_chokoku"), QStringLiteral("かなだけ（漢字はZENオールド明朝 Bk）")},
                                              {QStringLiteral("techno_monomaniac"), QStringLiteral("かなだけ（漢字はZEN角ゴシック Bk）")},
                                              {QStringLiteral("sfx_aoboshi"), QStringLiteral("かなだけ（漢字は解星 特ミン EB）")},
                                              {QStringLiteral("sfx_inline"), QStringLiteral("漢字は一部（残りはZEN角ゴシック Bk）")}};
        for (const auto& face : app::lettering::catalog_faces()) {
            QCOMPARE(face.mark, marks.value(face.key));
            const QString text = s.palette().item(face.key)->text();
            const QString usual = face.key == QLatin1String("antique") ? QStringLiteral("\nこの本のいつもの書体") : QString();
            QCOMPARE(text, (face.mark.isEmpty() ? face.row : face.row + QLatin1Char('\n') + face.mark) + usual);
        }
    }

    // A face whose file is not on this computer: its row greyed and saying ファイルなし, its menu's choices off; picked, it
    // is said and nothing changes — no edit, no face for the next line. The faces of before are never so. A file of the
    // right size that is not the face's is found out when its sample is made, and said the same.
    void aMissingFaceIsGreyedAndSaid() {
        QTemporaryDir some;
        for (const char* file : {"KleeOne-SemiBold.ttf", "Stick-Regular.ttf"}) {
            QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + "/" + file, some.path() + "/" + file));
        }
        {  // (DotGothic16's name and size, nothing of its content)
            QFile wrong(some.path() + QStringLiteral("/DotGothic16-Regular.ttf"));
            QVERIFY(wrong.open(QIODevice::WriteOnly));
            wrong.write(QByteArray(static_cast<qsizetype>(core::fonts::find("dot16")->bytes), '\0'));
        }
        files::set_dir_for_testing(some.path().toStdString());
        render::text::clear_layout_cache();
        {
            Studio s;
            const std::string id = s.a_line();
            s.open_palette();
            // (fainter than a row that can be picked: the look's own faint colour, not the words' colour)
            const QColor grey(app::theme::tokens().faint);
            QVERIFY(grey != s.palette().palette().color(QPalette::Active, QPalette::Text));
            int gone = 0;
            for (const auto& face : app::lettering::catalog_faces()) {
                const bool expected = face.added && face.key != QLatin1String("klee") && face.key != QLatin1String("stick") && face.key != QLatin1String("dot16");
                QCOMPARE(s.palette().missing(face.key), expected);
                QListWidgetItem* item = s.palette().item(face.key);
                QCOMPARE(item->text().endsWith(QStringLiteral("\nファイルなし")), expected);
                if (expected) {
                    QCOMPARE(item->foreground().color(), grey);
                    QVERIFY(item->toolTip().contains(QStringLiteral("この書体のファイルが、このパソコンにありません")));
                    ++gone;
                }
            }
            QCOMPARE(gone, 37);
            // picked: said, nothing done — the chosen line as it was, no face for the next line
            s.choose(id);
            const std::size_t before = s.ops.size();
            s.click(QStringLiteral("gothic_kiwami"));
            QCOMPARE(s.ops.size(), before);
            QCOMPARE(s.font_of(id), QString());
            QCOMPARE(s.window->last_notice(), QStringLiteral("書体「源暎きわみゴ」のファイルが、このパソコンにありません（アプリを入れ直すと使えます）"));
            for (const char* how : {"line", "next", "ruby", "book"}) {
                s.pick(QStringLiteral("gothic_kiwami"), how);
                QCOMPARE(s.ops.size(), before);
            }
            QVERIFY(s.window->armed_face().isEmpty());
            QCOMPARE(s.window->text_settings()->font->count(), 7);
            QVERIFY(s.window->book().font_path.empty());
            const std::unique_ptr<QMenu> menu = s.palette().item_menu(QStringLiteral("gothic_kiwami"));
            QCOMPARE(menu->actions().size(), 4);
            for (QAction* act : menu->actions()) QVERIFY(!act->isEnabled());
            const std::unique_ptr<QMenu> fine = s.palette().item_menu(QStringLiteral("klee"));
            for (QAction* act : fine->actions()) QVERIFY(act->isEnabled());
            // one that is there is picked as ever
            s.click(QStringLiteral("klee"));
            QCOMPARE(s.font_of(id), QStringLiteral("klee"));
            // the file that is not the face's: there by its size until its sample is made — then said, and not picked
            QVERIFY(!s.palette().missing(QStringLiteral("dot16")));
            s.palette().draw_samples();
            QVERIFY(s.palette().missing(QStringLiteral("dot16")));
            QVERIFY(s.palette().item(QStringLiteral("dot16"))->text().endsWith(QStringLiteral("\nファイルなし")));
            const std::size_t now = s.ops.size();
            s.click(QStringLiteral("dot16"));
            QCOMPARE(s.ops.size(), now);
            QCOMPARE(s.font_of(id), QStringLiteral("klee"));
        }
        use_source_fonts();
    }

    // The lists of before are as they were: the text tool's 書体 holds its seven, the first saying the usual face, with
    // ほかの書体… beside it opening the palette; a ruby's 書体 its eight.
    void theTextToolsListStillHoldsTheSeven() {
        Studio s;
        auto* settings = s.window->text_settings();
        QCOMPARE(settings->font->count(), 7);
        QCOMPARE(settings->font->itemText(0), QStringLiteral("いつもの書体（アンチック）"));
        QCOMPARE(settings->font->itemData(0).toString(), QString());
        const QStringList rest = {QStringLiteral("gothic"), QStringLiteral("mincho"), QStringLiteral("maru"), QStringLiteral("hand"), QStringLiteral("sfx"),
                                  QStringLiteral("sfx_pop")};
        for (int i = 1; i < 7; ++i) QCOMPARE(settings->font->itemData(i).toString(), rest[i - 1]);
        QVERIFY(settings->more_fonts != nullptr);
        QCOMPARE(settings->more_fonts->text(), QStringLiteral("ほかの書体…"));
        QVERIFY(!s.window->font_dock()->isVisible());
        settings->more_fonts->click();
        QVERIFY(s.window->font_dock()->isVisible());
        QCOMPARE(s.panel().ruby_font->count(), 8);
        QCOMPARE(s.panel().ruby_font->itemText(0), QStringLiteral("本文と同じ"));
        // (a line in an added face shows its name there and in the 文字 panel, not its key)
        const std::string id = s.put(Json{{"op", "add_line"}, {"text", "漢字のルビ"}, {"balloon", "speech"}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 40},
                                          {"style", Json{{"font", "klee"}}}, {"ruby_spans", Json::array({Json{{"start", 0}, {"end", 2}, {"ruby", "かんじ"}, {"font", "maru_black"}}})}});
        s.choose(id);
        s.panel().rubies->setCurrentRow(0);
        QCOMPARE(s.panel().ruby_font->currentText(), QStringLiteral("ZEN丸ゴシック Bk"));
        s.window->show_dock(QStringLiteral("文字"));
        QCoreApplication::processEvents();
        s.window->character_panel()->refresh();
        QCOMPARE(s.window->character_panel()->family->currentText(), QStringLiteral("クレー One SB"));
        QCOMPARE(s.window->character_panel()->family->currentData().toString(), QStringLiteral("klee"));
    }

    // The 文字 panel's list holds the forty after the seven, under their categories (rows that cannot be chosen), by name —
    // and filling it adds none of their fonts to Qt (the application's fonts are the ones it had); one chosen there is
    // the line's face. The 字形 panel shows an added face's letters with one font added for it at a time.
    void openingTheFontListAddsNoAddedFaceToQt() {
        Studio s;
        const std::string id = s.a_line();
        s.choose(id);
        s.window->show_dock(QStringLiteral("文字"));
        QCoreApplication::processEvents();
        app::CharacterPanel* panel = s.window->character_panel();
        const QStringList before = QFontDatabase::families();
        panel->fill_fonts();
        QCOMPARE(QFontDatabase::families(), before);
        for (const char* family : {"Klee One", "GenEi Kiwami Gothic", "DotGothic16", "Darumadrop One"}) {
            QVERIFY2(!QFontDatabase::families().contains(QString::fromLatin1(family)), family);
        }
        auto* model = qobject_cast<QStandardItemModel*>(panel->family->model());
        QVERIFY(model != nullptr);
        int added = 0, headings = 0;
        for (int i = 0; i < panel->family->count(); ++i) {
            const QString data = panel->family->itemData(i).toString();
            if (core::fonts::added(data.toStdString())) {
                QCOMPARE(panel->family->itemText(i), QString::fromStdString(core::fonts::find(data.toStdString())->label));
                QVERIFY(model->item(i)->isEnabled());
                QVERIFY(!panel->family->itemData(i, Qt::FontRole).isValid());  // (by name: no font of its own set for the row)
                ++added;
            } else if (panel->family->itemText(i).startsWith(QStringLiteral("— "))) {
                QVERIFY(!model->item(i)->isEnabled());
                ++headings;
            }
        }
        QCOMPARE(added, 40);
        QCOMPARE(headings, 11);
        const int at = panel->family->findData(QStringLiteral("mincho_hina"));
        QVERIFY(at > 7);
        panel->family->setCurrentIndex(at);
        emit panel->family->activated(at);
        QCOMPARE(s.font_of(id), QStringLiteral("mincho_hina"));
        // 字形: an added face's letters, with one font of Genko's files in Qt at a time (the one before it let go)
        s.window->show_dock(QStringLiteral("字形"));
        QCoreApplication::processEvents();
        app::GlyphsPanel* glyphs = s.window->glyphs_panel();
        QVERIFY(glyphs != nullptr);
        glyphs->refresh();
        QCoreApplication::processEvents();
        // (what Qt gained for the face shown — a font file may bring more than one family's name)
        const auto gained = [&before] {
            QStringList out;
            for (const QString& family : QFontDatabase::families()) {
                if (!before.contains(family)) out << family;
            }
            return out;
        };
        const QStringList first = gained();
        QVERIFY2(first.size() >= 1 && first.join(QLatin1Char(' ')).contains(QStringLiteral("Hina")), qPrintable(first.join(QLatin1Char('|'))));
        s.choose(id);
        panel->family->setCurrentIndex(panel->family->findData(QStringLiteral("klee")));
        emit panel->family->activated(panel->family->currentIndex());
        QCOMPARE(s.font_of(id), QStringLiteral("klee"));
        glyphs->refresh();
        QCoreApplication::processEvents();
        const QStringList second = gained();
        QVERIFY2(second.size() >= 1 && second.join(QLatin1Char(' ')).contains(QStringLiteral("Klee")), qPrintable(second.join(QLatin1Char('|'))));
        for (const QString& family : first) QVERIFY2(!second.contains(family), qPrintable(family));  // (the one before: let go)

        // (and the panel names the face it shows by its name, as every list does — never by its key)
        QCOMPARE(glyphs->font->currentData().toString(), QStringLiteral("klee"));
        QCOMPARE(glyphs->font->currentText(), QStringLiteral("クレー One SB"));
        for (int i = 0; i < glyphs->font->count(); ++i) QVERIFY2(!glyphs->font->itemText(i).contains(QLatin1Char('_')), qPrintable(glyphs->font->itemText(i)));
    }

    // この本のいつもの書体にする: the book's usual face (every line that names none) set in one step, said, and shown as
    // the text tool's first choice; the same again changes nothing; アンチック takes it away again.
    void theBooksUsualFaceIsSetAndShown() {
        Studio s;
        const std::string id = s.a_line();
        s.open_palette();
        QComboBox* font = s.window->text_settings()->font;
        const auto row = [&](const char* key) { return s.palette().item(QString::fromLatin1(key))->text(); };
        const std::size_t before = s.ops.size();
        s.pick(QStringLiteral("antique_genei"), "book");
        QCOMPARE(s.ops.size(), before + 1);
        QCOMPARE(s.last(), (Json{{"op", "set_meta"}, {"font_path", "antique_genei"}}));
        QCOMPARE(s.window->book().font_path, std::string("antique_genei"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("この本のいつもの書体を「源暎アンチック」にしました（元に戻す で取り消せます）"));
        QCOMPARE(font->itemText(0), QStringLiteral("いつもの書体（源暎アンチック）"));
        // (アンチック is not what the first choice gives any more: it has a choice of its own, after the first)
        QCOMPARE(font->count(), 8);
        QCOMPARE(font->itemText(1), QStringLiteral("アンチック"));
        QCOMPARE(font->itemData(1).toString(), QStringLiteral("antique"));
        // (the palette's rows say whose the usual face is)
        QCOMPARE(s.palette().usual(), QStringLiteral("antique_genei"));
        QCOMPARE(row("antique_genei"), QStringLiteral("源暎アンチック\nこの本のいつもの書体"));
        QCOMPARE(row("antique"), QStringLiteral("アンチック"));
        // (the line that names no face is set in it; its own style untouched)
        QCOMPARE(s.font_of(id), QString());
        QCOMPARE(render::text::line_face(*s.line(id), std::optional<std::string>(s.window->book().font_path)).first.key, std::string("antique_genei"));
        QVERIFY(core::fonts::uses_feature(s.window->book()));
        s.pick(QStringLiteral("antique_genei"), "book");
        QCOMPARE(s.ops.size(), before + 1);
        QCOMPARE(s.window->last_notice(), QStringLiteral("この本のいつもの書体は、もう「源暎アンチック」です"));
        s.pick(QStringLiteral("antique"), "book");
        QCOMPARE(s.last(), (Json{{"op", "set_meta"}, {"font_path", ""}}));
        QCOMPARE(font->itemText(0), QStringLiteral("いつもの書体（アンチック）"));
        QCOMPARE(font->count(), 7);
        QCOMPARE(font->findData(QStringLiteral("antique")), -1);
        QCOMPARE(row("antique"), QStringLiteral("アンチック\nこの本のいつもの書体"));
        QCOMPARE(row("antique_genei"), QStringLiteral("源暎アンチック"));
        QVERIFY(!core::fonts::uses_feature(s.window->book()));
        // 元に戻す and やり直す change the usual face as a pick does: the text tool's first choice and the palette's rows
        // follow at once, not at the next pick (the second review's P3-4)
        s.trigger("act_undo");
        QCOMPARE(s.window->book().font_path, std::string("antique_genei"));
        QCOMPARE(font->itemText(0), QStringLiteral("いつもの書体（源暎アンチック）"));
        QCOMPARE(font->count(), 8);
        QCOMPARE(s.palette().usual(), QStringLiteral("antique_genei"));
        s.trigger("act_undo");
        QCOMPARE(s.window->book().font_path, std::string());
        QCOMPARE(font->itemText(0), QStringLiteral("いつもの書体（アンチック）"));
        QCOMPARE(font->count(), 7);
        QCOMPARE(s.palette().usual(), QStringLiteral("antique"));
        s.trigger("act_redo");
        QCOMPARE(font->itemText(0), QStringLiteral("いつもの書体（源暎アンチック）"));
        // (… and a change that did not come from the palette — an agent's set_meta is this op —, to one of the six of
        // before and to a font file of the person's: said by the name the text tool lists, and as a file)
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_meta"}, {"font_path", "mincho"}}})));
        QCOMPARE(font->itemText(0), QStringLiteral("いつもの書体（明朝）"));
        QCOMPARE(font->count(), 8);
        QCOMPARE(s.palette().usual(), QStringLiteral("mincho"));
        QCOMPARE(row("mincho"), QStringLiteral("明朝（ZENオールド明朝 SB）\nこの本のいつもの書体"));
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_meta"}, {"font_path", "/no/such/font.otf"}}})));
        QCOMPARE(font->itemText(0), QStringLiteral("いつもの書体（書体ファイル）"));
        QCOMPARE(font->count(), 8);
        QCOMPARE(s.palette().usual(), QString());
    }

    // The book has a usual face of its own, and アンチック is picked for the next line: the line placed is in アンチック —
    // it says so in its style —, not in the book's usual face (the second review's P2-1: the text tool's first choice,
    // which names no face, was chosen for it). Without a usual face of its own the first choice is アンチック, as ever.
    void antiqueForTheNextLineInABookWithItsOwnUsualFace() {
        Studio s;
        s.open_palette();
        QComboBox* font = s.window->text_settings()->font;
        s.pick(QStringLiteral("antique_genei"), "book");
        QCOMPARE(s.window->book().font_path, std::string("antique_genei"));
        s.choose_none();
        s.trigger("act_text");
        s.click(QStringLiteral("antique"));
        QCOMPARE(s.window->armed_face(), QStringLiteral("antique"));
        QCOMPARE(s.palette().marked(), QStringLiteral("antique"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("書体「アンチック」を選びました。置きたい所をクリックして、台詞を打ちます"));
        QCOMPARE(font->currentData().toString(), QStringLiteral("antique"));
        QCOMPARE(font->currentText(), QStringLiteral("アンチック"));
        QCOMPARE(font->currentIndex(), 1);
        const Json made = s.type_at(QPointF(100, 100), QStringLiteral("かなと漢字"));
        QVERIFY2(made.is_object(), "no line was typed");
        QCOMPARE(made["style"]["font"], Json("antique"));
        const std::string id = s.window->book().story.back().id;
        QCOMPARE(render::text::line_face(*s.line(id), std::optional<std::string>(s.window->book().font_path)).first.key, std::string("antique"));
        // (the same words with the first choice: the book's usual face — and not the same picture)
        s.choose_none();
        s.trigger("act_text");
        font->setCurrentIndex(0);
        emit font->activated(0);
        QVERIFY(s.window->armed_face().isEmpty());
        const Json usual = s.type_at(QPointF(40, 100), QStringLiteral("かなと漢字"));
        QVERIFY2(usual.is_object(), "no line was typed");
        QVERIFY(!usual.contains("style") || !usual["style"].contains("font"));
        const core::StoryLine in_usual = s.window->book().story.back();
        QCOMPARE(render::text::line_face(in_usual, std::optional<std::string>(s.window->book().font_path)).first.key, std::string("antique_genei"));
        // (アンチック waiting for the next line when the book's usual face goes: the first choice again — still アンチック)
        s.choose_none();
        s.trigger("act_text");
        s.click(QStringLiteral("antique"));
        QCOMPARE(font->currentIndex(), 1);
        s.pick(QStringLiteral("antique"), "book");
        QCOMPARE(s.window->book().font_path, std::string());
        QCOMPARE(font->count(), 7);
        QCOMPARE(font->currentIndex(), 0);
        const Json plain = s.type_at(QPointF(150, 100), QStringLiteral("かなと漢字"));
        QVERIFY2(plain.is_object(), "no line was typed");
        QVERIFY(!plain.contains("style") || !plain["style"].contains("font"));
        // (and put back while it waits: its own choice again)
        s.choose_none();
        s.trigger("act_text");
        s.click(QStringLiteral("antique"));
        QCOMPARE(font->currentIndex(), 0);
        s.trigger("act_undo");  // (the line typed)
        s.trigger("act_undo");  // (the usual face taken away)
        QCOMPARE(s.window->book().font_path, std::string("antique_genei"));
        QCOMPARE(font->count(), 8);
        QCOMPARE(font->currentData().toString(), QStringLiteral("antique"));
    }

    // 選んだルビをこの書体にする: the ruby chosen in the 台詞 panel's list takes the face (an edit of the line's rubies);
    // with none chosen, said and nothing done.
    void aChosenRubyTakesTheFace() {
        Studio s;
        const std::string id = s.put(Json{{"op", "add_line"}, {"text", "漢字のルビ"}, {"balloon", "speech"}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 40},
                                          {"ruby_spans", Json::array({Json{{"start", 0}, {"end", 2}, {"ruby", "かんじ"}}})}});
        s.open_palette();
        s.choose(id);
        s.panel().rubies->setCurrentRow(-1);
        const std::size_t before = s.ops.size();
        s.pick(QStringLiteral("maru_regular"), "ruby");
        QCOMPARE(s.ops.size(), before);
        QCOMPARE(s.window->last_notice(), QStringLiteral("先に台詞パネルのルビの一覧で、書体を変えるルビを選びます"));
        s.panel().rubies->setCurrentRow(0);
        s.pick(QStringLiteral("maru_regular"), "ruby");
        QCOMPARE(s.ops.size(), before + 1);
        QVERIFY(s.line(id)->ruby_spans.has_value());
        QCOMPARE((*s.line(id)->ruby_spans)[0].font, std::optional<std::string>("maru_regular"));
        QCOMPARE(s.font_of(id), QString());  // (the line's own face as it was)
        QCOMPARE(s.window->last_notice(), QStringLiteral("選んだルビの書体を「ZEN丸ゴシック R」にしました（元に戻す で取り消せます）"));
        // (the same face again: nothing done, and said as that — not as done)
        const std::size_t steps = s.session->changes_in_memory();
        s.panel().rubies->setCurrentRow(0);
        s.pick(QStringLiteral("maru_regular"), "ruby");
        QCOMPARE(s.ops.size(), before + 1);
        QCOMPARE(s.session->changes_in_memory(), steps);
        QCOMPARE(s.window->last_notice(), QStringLiteral("選んだルビは、もう「ZEN丸ゴシック R」です"));
    }

    // A ruby's face that could not be changed — the book is open read-only — is not said to have been changed: what is
    // said is the refusal (the second review's P3-5: 「…にしました」 was said over it).
    void aRefusedRubyPickIsNotSaidAsDone() {
        Studio s([](core::Document& doc) {
            core::StoryLine line = a_story_line("a", "");
            line.text = "漢字のルビ";
            line.ruby_runs = {Json::array({"漢字", "かんじ"})};
            line.ruby_spans = core::ruby::from_runs(line.text, line.ruby_runs, false);
            doc.story.push_back(std::move(line));
        }, true, "of-a-later-genko@9");
        QVERIFY(!s.session->read_only_reason().empty());
        s.open_palette();
        s.choose("a");
        QVERIFY(s.panel().rubies->count() >= 1);
        s.panel().rubies->setCurrentRow(0);
        const std::size_t steps = s.session->changes_in_memory();
        s.pick(QStringLiteral("maru_regular"), "ruby");
        QCOMPARE(s.session->changes_in_memory(), steps);
        QVERIFY(s.ops.empty());
        QVERIFY(!(*s.line("a")->ruby_spans)[0].font.has_value());
        QVERIFY2(!s.window->last_notice().contains(QStringLiteral("にしました")), qPrintable(s.window->last_notice()));
        // (the line's face and the book's usual face the same: refused, nothing claimed)
        s.pick(QStringLiteral("klee"), "line");
        QVERIFY2(!s.window->last_notice().contains(QStringLiteral("にしました")), qPrintable(s.window->last_notice()));
        s.pick(QStringLiteral("klee"), "book");
        QVERIFY2(!s.window->last_notice().contains(QStringLiteral("にしました")), qPrintable(s.window->last_notice()));
        QCOMPARE(s.window->book().font_path, std::string());
        QCOMPARE(s.font_of("a"), QString());
        QCOMPARE(s.session->changes_in_memory(), steps);
    }

    // The dock is kept with the layout as every dock: open or closed as it was saved.
    void theDockIsKeptWithTheLayout() {
        Studio s;
        QDockWidget* dock = s.window->font_dock();
        s.open_palette();
        const QByteArray open_layout = s.window->saveState();
        dock->hide();
        const QByteArray closed_layout = s.window->saveState();
        QVERIFY(s.window->restoreState(open_layout));
        QVERIFY(dock->isVisible());
        QVERIFY(s.window->restoreState(closed_layout));
        QVERIFY(!dock->isVisible());
    }

    // When a book is opened a person is told, once, in the status line: that a face it uses is not on this computer (its
    // letters drawn in the usual face) — by the faces' names, a type layer's on a later page too —; and that a book of
    // before whose words happened to be a face's key of today is drawn in that face from now on. A book that names none
    // of the forty, or that said fonts@1 and has its files, is opened without a word of fonts.
    void whatIsSaidWhenABookIsOpened() {
        {  // (nothing to say)
            Studio s([](core::Document& doc) { doc.story.push_back(a_story_line("a", "gothic")); });
            QVERIFY2(s.window->fonts_notice().isEmpty(), qPrintable(s.window->fonts_notice()));
            QVERIFY(s.window->fonts_band()->isHidden());
            QVERIFY2(!s.window->last_notice().contains(QStringLiteral("書体")), qPrintable(s.window->last_notice()));
        }
        {  // (a book of now, its files here)
            Studio s([](core::Document& doc) {
                doc.story.push_back(a_story_line("a", "klee"));
                doc.features.emplace_back(core::fonts::kFontsFeature);
            });
            QVERIFY(s.window->book().features == std::vector<std::string>{"fonts@1"});
            QTest::qWait(400);  // (its face's file is read and checked off this thread: nothing comes of it)
            QVERIFY(files::present("klee"));
            QVERIFY2(s.window->fonts_notice().isEmpty(), qPrintable(s.window->fonts_notice()));
            QVERIFY(s.window->fonts_band()->isHidden());
            QVERIFY2(!s.window->last_notice().contains(QStringLiteral("書体")), qPrintable(s.window->last_notice()));
        }
        {  // (a book of before F2 that wrote "gothic_black" and "dot16" as fonts' names: no fonts@1 in it)
            QTemporaryDir tmp, config;
            const QByteArray previous = qgetenv("GENKO_CONFIG_DIR");
            qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
            const auto path = gui_test::path_of(tmp.path() + "/book");
            core::Document doc = gui_test::new_doc(2);
            doc.story.push_back(a_story_line("a", "gothic_black"));
            doc.story.push_back(a_story_line("b", "dot16", 80));
            gui_test::write_book(path, doc);
            // (as a build of before wrote it: the same book without the feature's name)
            const QString file = tmp.path() + QStringLiteral("/book/project.json");
            Json project = core::parse_python_json(genko::test::read_bytes(file));
            QCOMPARE(project["features"], Json::array({"fonts@1"}));
            project["features"] = Json::array();
            genko::test::write_bytes(file, core::dump_python_indent2(project) + "\n");
            auto session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
            auto window = std::make_unique<app::MainWindow>(session);
            window->show();
            QTRY_VERIFY_WITH_TIMEOUT(window->fonts_notice().contains(QStringLiteral("この版からはその書体で描く")), 10000);
            QCOMPARE(window->fonts_notice(),
                     QStringLiteral("この本が指定している書体「ドットゴシック16」「ZEN角ゴシック Bk」は、前の版には無かったためアンチックで描いていました。"
                                    "この版からはその書体で描くので、見た目が変わります"));
            // (in a band above the page — the whole sentence, wrapped, not the status line's one row —, there until closed
            // with the button beside it: a real button, with a name of its own, reached with the keys as any other)
            QWidget* band = window->fonts_band();
            QVERIFY(band->isVisible());
            QCOMPARE(band->objectName(), QStringLiteral("fontsBand"));
            QCOMPARE(window->findChildren<QWidget*>(QStringLiteral("fontsBand")).size(), 1);
            QVERIFY(window->fonts_band_text()->wordWrap());
            QCOMPARE(QTextDocumentFragment::fromHtml(window->fonts_band_text()->text()).toPlainText(), window->fonts_notice());
            QPushButton* close = window->fonts_band_close();
            QCOMPARE(close->text(), QStringLiteral("この知らせを閉じる"));
            QCOMPARE(close->objectName(), QStringLiteral("fontsBandClose"));
            QVERIFY(close->isVisible() && close->isEnabled());
            QVERIFY((close->focusPolicy() & Qt::TabFocus) != 0);
            QVERIFY(band->mapTo(window.get(), QPoint(0, 0)).y() < window->canvas()->mapTo(window.get(), QPoint(0, 0)).y());
            // (another book opened beside it has nothing said of it; come back to, this one's band is there again)
            {
                const auto other = gui_test::path_of(tmp.path() + "/other");
                gui_test::write_book(other, gui_test::new_doc(1));
                window->open_path(other);
                QTRY_VERIFY_WITH_TIMEOUT(&window->session() != session.get(), 10000);
                QCoreApplication::processEvents();
                QVERIFY(band->isHidden());
                QVERIFY(window->fonts_notice().isEmpty());
                window->switch_document(0);
                QTRY_VERIFY_WITH_TIMEOUT(&window->session() == session.get(), 10000);
                QTRY_VERIFY_WITH_TIMEOUT(band->isVisible(), 10000);
                QVERIFY(window->fonts_notice().contains(QStringLiteral("この版からはその書体で描く")));
            }
            // (closed with the keys — the button takes them: Space —: gone, and not said again for this book)
            close->setFocus();
            QTest::keyClick(close, Qt::Key_Space);
            QVERIFY(band->isHidden());
            QVERIFY(window->fonts_notice().isEmpty());
            window->note_fonts_on_open(session);
            QCoreApplication::processEvents();
            QVERIFY(band->isHidden());
            window.reset();
            qputenv("GENKO_CONFIG_DIR", previous);
        }
        {  // (faces whose files are not on this computer: a line's, a type layer's on the second page, the book's usual face)
            QTemporaryDir empty;
            files::set_dir_for_testing(empty.path().toStdString());
            render::text::clear_layout_cache();
            {
                Studio s([](core::Document& doc) {
                    doc.story.push_back(a_story_line("a", "klee"));
                    core::StoryLine record = a_story_line("", "brush_syuku");
                    core::Layer layer;
                    layer.id = "ly_type";
                    layer.kind = core::LayerKind::Type;
                    layer.title = "文字";
                    layer.type = std::make_shared<const core::StoryLine>(std::move(record));
                    doc.edit_page(1).layers.push_back(std::move(layer));
                    doc.font_path = "antique_genei";
                    doc.features.emplace_back(core::fonts::kFontsFeature);
                });
                QTRY_VERIFY_WITH_TIMEOUT(s.window->fonts_notice().contains(QStringLiteral("このパソコンにありません")), 10000);
                QCOMPARE(s.window->fonts_notice(),
                         QStringLiteral("この本が使っている書体「源暎アンチック」「佑字 肅」「クレー One SB」のファイルが、このパソコンにありません。"
                                        "その文字は代わりの書体（台詞はアンチック）で描いています（アプリを入れ直すと元の書体に戻ります）"));
                QVERIFY(s.window->fonts_band()->isVisible());
                QCOMPARE(s.window->faces_not_installed(), (QStringList{QStringLiteral("源暎アンチック"), QStringLiteral("佑字 肅"), QStringLiteral("クレー One SB")}));
                // (the text tool's first choice still names the book's usual face)
                QCOMPARE(s.window->text_settings()->font->itemText(0), QStringLiteral("いつもの書体（源暎アンチック）"));
            }
            use_source_fonts();
        }
        {  // (a file of the face's name and size that is not the face's: read and checked when the book is opened — off this
           // thread —, and named in the band as one that is not there; the list of the faces not installed says it too)
            QTemporaryDir other;
            {
                QFile wrong(other.path() + QStringLiteral("/KleeOne-SemiBold.ttf"));
                QVERIFY(wrong.open(QIODevice::WriteOnly));
                wrong.write(QByteArray(static_cast<qsizetype>(core::fonts::find("klee")->bytes), '\0'));
            }
            QVERIFY(QFile::copy(QString::fromStdString(source_fonts()) + "/Stick-Regular.ttf", other.path() + "/Stick-Regular.ttf"));
            files::set_dir_for_testing(other.path().toStdString());
            render::text::clear_layout_cache();
            {
                Studio s([](core::Document& doc) {
                    doc.story.push_back(a_story_line("a", "klee"));
                    doc.story.push_back(a_story_line("b", "stick", 80));
                    doc.features.emplace_back(core::fonts::kFontsFeature);
                });
                QTRY_VERIFY_WITH_TIMEOUT(s.window->fonts_notice().contains(QStringLiteral("このパソコンにありません")), 10000);
                QVERIFY2(s.window->fonts_notice().startsWith(QStringLiteral("この本が使っている書体「クレー One SB」のファイルが、")), qPrintable(s.window->fonts_notice()));
                QCOMPARE(s.window->faces_not_installed(), QStringList{QStringLiteral("クレー One SB")});
                QVERIFY(files::damaged("klee"));
            }
            use_source_fonts();
        }
    }

    // A long book opened with only its first page read: nothing is said until the rest is — then the band names the face
    // of a type layer on a page read later (the look at the book waits for its pages; it is not lost).
    void aBookStillBeingReadIsToldOfWhenItIsRead() {
        QTemporaryDir empty, tmp, config;
        files::set_dir_for_testing(empty.path().toStdString());
        render::text::clear_layout_cache();
        const QByteArray previous = qgetenv("GENKO_CONFIG_DIR");
        qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
        {
            const auto path = gui_test::path_of(tmp.path() + "/book");
            core::Document doc = gui_test::new_doc(6);
            core::Layer layer;
            layer.id = "ly_type";
            layer.kind = core::LayerKind::Type;
            layer.title = "文字";
            layer.type = std::make_shared<const core::StoryLine>(a_story_line("", "brush_syuku"));
            doc.edit_page(4).layers.push_back(std::move(layer));
            doc.features.emplace_back(core::fonts::kFontsFeature);
            gui_test::write_book(path, doc);
            app::Session::Options options = gui_test::quick(gui_test::path_of(tmp.path() + "/recovery"));
            options.defer_pages = true;
            options.read_rest_now = false;
            auto session = app::Session::from(app::Session::read(path, options), path, options);
            QVERIFY(session->loading());
            app::MainWindow window(session);
            window.show();
            QCoreApplication::processEvents();
            QTest::qWait(50);
            QVERIFY(window.fonts_band()->isHidden());  // (its fifth page is not read: nothing known of its faces yet)
            QVERIFY(window.fonts_notice().isEmpty());
            session->read_rest();
            QVERIFY(gui_test::wait_for([&] { return !session->loading(); }));
            QTRY_VERIFY_WITH_TIMEOUT(window.fonts_band()->isVisible(), 10000);
            QCOMPARE(window.fonts_notice(), QStringLiteral("この本が使っている書体「佑字 肅」のファイルが、このパソコンにありません。"
                                                           "その文字は代わりの書体（台詞はアンチック）で描いています（アプリを入れ直すと元の書体に戻ります）"));
        }
        qputenv("GENKO_CONFIG_DIR", previous);
        use_source_fonts();
    }

    // すべての見つからないフォントを置換 on a computer without Genko's font files: the font file that is not there is
    // replaced; the faces Genko comes with stay in the book, and that is said. 見つからないフォントを解決… with only those
    // says so and asks nothing.
    void replacingMissingFontsLeavesGenkosFaces() {
        QTemporaryDir empty;
        files::set_dir_for_testing(empty.path().toStdString());
        render::text::clear_layout_cache();
        {
            Studio s([](core::Document& doc) {
                doc.story.push_back(a_story_line("a", "klee"));
                doc.story.push_back(a_story_line("b", "/no/such/font.otf", 80));
                doc.features.emplace_back(core::fonts::kFontsFeature);
            });
            s.trigger("act_type_replace_missing");
            QCOMPARE(s.font_of("a"), QStringLiteral("klee"));
            QCOMPARE(s.font_of("b"), QString());
            // (the short word in the status line; why the face was left, whole, in the band above the page)
            QCOMPARE(s.window->last_notice(), QStringLiteral("見つからないフォントを、いつもの書体にしました（1 か所）"));
            QCOMPARE(s.window->fonts_notice(),
                     QStringLiteral("付属の書体「クレー One SB」は、ファイルがこのパソコンに無いだけなので、そのままにしました（アプリを入れ直すと元の書体で描けます）"));
            QVERIFY(s.window->fonts_band()->isVisible());
            QVERIFY(core::fonts::uses_feature(s.window->book()));
            s.trigger("act_type_resolve_missing");  // (nothing left to replace: no dialog, the faces said)
            QCOMPARE(s.font_of("a"), QStringLiteral("klee"));
            QCOMPARE(s.window->last_notice(), QStringLiteral("見つからないフォントはありません"));
            QCOMPARE(s.window->fonts_notice(),
                     QStringLiteral("付属の書体「クレー One SB」のファイルが、このパソコンにありません。置き換えずに、アプリを入れ直すと元の書体で描けます"));
        }
        use_source_fonts();
    }

    // Light and dark: the samples are in the list's own text colour (dark letters on the light look, light on the dark),
    // drawn again when the look changes; a face that cannot be picked is fainter; the marked row takes the accent's soft
    // colour of the look.
    void theSamplesFollowTheLook() {
        Studio s;
        const auto ink = [&](const QString& key) {
            const QImage picture = s.palette().item(key)->icon().pixmap(app::face_sample_size()).toImage().convertToFormat(QImage::Format_ARGB32);
            qint64 light = 0, weight = 0;
            for (int y = 0; y < picture.height(); ++y) {
                for (int x = 0; x < picture.width(); ++x) {
                    const QRgb p = picture.pixel(x, y);
                    light += static_cast<qint64>(qGray(p)) * qAlpha(p);
                    weight += qAlpha(p);
                }
            }
            return weight > 0 ? static_cast<int>(light / weight) : -1;
        };
        app::settings()->setValue(QStringLiteral("ui/theme"), QStringLiteral("light"));
        app::theme::apply(qApp);
        s.open_palette();
        s.palette().draw_samples();
        QCoreApplication::processEvents();
        s.window->arm_font_face(QStringLiteral("klee"));
        const int on_light = ink(QStringLiteral("gothic_kiwami"));
        QVERIFY2(on_light >= 0 && on_light < 90, qPrintable(QString::number(on_light)));
        QCOMPARE(s.palette().item(QStringLiteral("klee"))->background().color(), QColor(app::theme::tokens().accent_soft));
        app::settings()->setValue(QStringLiteral("ui/theme"), QStringLiteral("dark"));
        app::theme::apply(qApp);
        QCoreApplication::processEvents();
        const int on_dark = ink(QStringLiteral("gothic_kiwami"));
        QVERIFY2(on_dark > 160, qPrintable(QString::number(on_dark)));
        QVERIFY(app::theme::tokens().dark);
        QCOMPARE(s.palette().item(QStringLiteral("klee"))->background().color(), QColor(app::theme::tokens().accent_soft));
        QCOMPARE(s.palette().marked(), QStringLiteral("klee"));
        app::settings()->remove(QStringLiteral("ui/theme"));
        app::theme::apply(qApp);
    }

    // Every word a person reads of the palette: Japanese, with no run of four Latin letters or more and no key — its note,
    // the search box, the headings, each face's row and tooltip (with and without its file), the menu, the text tool's
    // button, and every sentence the window says of a pick (です・ます, as the window speaks).
    void everyWordReadsInJapanese() {
        QStringList words;
        QStringList said;
        {
            Studio s;
            s.open_palette();
            for (QLabel* label : s.palette().findChildren<QLabel*>()) words << label->text();
            words << s.palette().search()->placeholderText() << s.palette().search()->accessibleName() << s.palette().list()->accessibleName();
            words << s.window->font_dock()->windowTitle() << s.window->action(QStringLiteral("act_font_palette"))->text()
                  << s.window->action(QStringLiteral("act_font_palette"))->toolTip();
            words << s.window->text_settings()->more_fonts->text() << s.window->text_settings()->more_fonts->toolTip();
            for (int i = 0; i < s.palette().list()->count(); ++i) {
                words << s.palette().list()->item(i)->text();
                if (!s.palette().list()->item(i)->toolTip().isEmpty()) words << s.palette().list()->item(i)->toolTip();
            }
            const std::unique_ptr<QMenu> menu = s.palette().item_menu(QStringLiteral("klee"));
            for (QAction* act : menu->actions()) words << act->text();
            // the sentences of every kind of pick
            const std::string id = s.put(Json{{"op", "add_line"}, {"text", "漢字のルビ"}, {"balloon", "speech"}, {"x_mm", 60}, {"y_mm", 60}, {"w_mm", 30}, {"h_mm", 40},
                                              {"ruby_spans", Json::array({Json{{"start", 0}, {"end", 2}, {"ruby", "かんじ"}}})}});
            // (each as the status line shows it: whole — the label is as wide as its words need, or wider)
            int cut = 0;
            const auto note = [&] {
                said << s.window->last_notice();
                QCoreApplication::processEvents();
                if (s.window->status_label()->sizeHint().width() > s.window->status_label()->width()) ++cut;
            };
            s.choose_none();
            s.pick(QStringLiteral("klee"), "line"), note();
            s.pick(QStringLiteral("klee"), "ruby"), note();
            s.choose(id);
            for (const auto& face : app::lettering::catalog_faces()) {
                s.click(face.key), note();
            }
            s.click(QStringLiteral("klee")), note();
            s.click(QStringLiteral("klee")), note();
            s.panel().rubies->setCurrentRow(0);
            s.pick(QStringLiteral("maru_black"), "ruby"), note();
            s.panel().rubies->setCurrentRow(0);
            s.pick(QStringLiteral("maru_black"), "ruby"), note();
            for (const auto& face : app::lettering::catalog_faces()) {
                s.pick(face.key, "book"), note();
                s.pick(face.key, "next"), note();
            }
            s.pick(QStringLiteral("antique_genei"), "book"), note();
            s.pick(QStringLiteral("antique_genei"), "book"), note();
            words << s.window->text_settings()->font->currentText() << s.window->text_settings()->font->itemText(0) << s.window->text_settings()->font->itemText(1);
            QCOMPARE(cut, 0);
            QVERIFY(s.window->fonts_notice().isEmpty());  // (none was too long for the line: none went to the band)
        }
        QTemporaryDir empty;
        files::set_dir_for_testing(empty.path().toStdString());
        render::text::clear_layout_cache();
        {
            Studio s([](core::Document& doc) {
                doc.story.push_back(a_story_line("a", "klee"));
                doc.story.push_back(a_story_line("b", "/no/such/font.otf", 80));
                doc.features.emplace_back(core::fonts::kFontsFeature);
            });
            QTRY_VERIFY_WITH_TIMEOUT(s.window->fonts_notice().contains(QStringLiteral("このパソコンにありません")), 10000);
            said << s.window->fonts_notice();
            words << QTextDocumentFragment::fromHtml(s.window->fonts_band_text()->text()).toPlainText() << s.window->fonts_band_close()->text();
            s.open_palette();
            for (int i = 0; i < s.palette().list()->count(); ++i) {
                words << s.palette().list()->item(i)->text();
                if (!s.palette().list()->item(i)->toolTip().isEmpty()) words << s.palette().list()->item(i)->toolTip();
            }
            for (const auto& face : app::lettering::catalog_faces()) {
                if (!face.added) continue;
                s.click(face.key);
                said << s.window->last_notice();
                QCoreApplication::processEvents();
                QVERIFY2(s.window->status_label()->sizeHint().width() <= s.window->status_label()->width(), qPrintable(s.window->last_notice()));
            }
            s.trigger("act_type_replace_missing");
            said << s.window->last_notice() << s.window->fonts_notice();
            s.trigger("act_type_resolve_missing");
            said << s.window->last_notice() << s.window->fonts_notice();
            // (the 文字 panel's rows for the forty)
            s.window->show_dock(QStringLiteral("文字"));
            QCoreApplication::processEvents();
            s.window->character_panel()->fill_fonts();
            for (int i = 0; i < s.window->character_panel()->family->count(); ++i) {
                const QString data = s.window->character_panel()->family->itemData(i).toString();
                if (core::fonts::added(data.toStdString()) || s.window->character_panel()->family->itemText(i).startsWith(QStringLiteral("— "))) {
                    words << s.window->character_panel()->family->itemText(i);
                }
            }
        }
        use_source_fonts();
        {  // (a book of before whose words are a face's key of today)
            Studio s([](core::Document& doc) { doc.story.push_back(a_story_line("a", "gothic_black")); }, false);
            QTRY_VERIFY_WITH_TIMEOUT(s.window->fonts_notice().contains(QStringLiteral("この版から")), 10000);
            said << s.window->fonts_notice();
        }
        said << QStringLiteral("書体の知らせを、ページの上に出しました");  // (what the status line says of a sentence it sends to the band)
        QVERIFY(words.size() > 250);
        QVERIFY2(said.size() > 150, qPrintable(QString::number(said.size())));
        // (この本 everywhere — never この原稿 for the same book)
        for (const QString& text : words + said) QVERIFY2(!text.contains(QStringLiteral("この原稿")), qPrintable(text));
        for (const QString& text : words + said) {
            const QString why = unreadable(text);
            QVERIFY2(why.isEmpty(), qPrintable(why));
        }
        // (the window's sentences: one register — です・ます —, never the plain form of a check's message)
        static const QRegularExpression polite(QStringLiteral("(ます|です|ません|ました)(。|（[^（）]*）)?$"));
        for (const QString& text : said) {
            for (const QString& sentence : text.split(QLatin1Char('\n'))) {
                QVERIFY2(polite.match(sentence).hasMatch(), qPrintable(sentence));
                QVERIFY2(!sentence.contains(QStringLiteral("描いた（")) && !sentence.contains(QStringLiteral("描ける）")), qPrintable(sentence));
            }
        }
    }

    // No sentence of the fonts is cut by the one-line status label (the second review's P3-9), however wide the window
    // is: at the width a window opens with, and at a narrow one, each sentence is either whole in the status line or —
    // too long for it — whole in the band above the page, wrapped, with a short word in the status line that it is there.
    void noSentenceIsCutInTheStatusLine() {
        const QString pointer = QStringLiteral("書体の知らせを、ページの上に出しました");
        for (const int width : {0, 1300, 760}) {  // (0: as the window opens, nobody having sized it)
            QTemporaryDir tmp, config;
            const QByteArray previous = qgetenv("GENKO_CONFIG_DIR");
            qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
            {
                const auto path = gui_test::path_of(tmp.path() + "/book");
                core::Document doc = gui_test::new_doc(1);
                doc.story.push_back(a_story_line("a", ""));
                gui_test::write_book(path, doc);
                auto session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
                app::MainWindow window(session);
                if (width > 0) window.resize(width, 800);
                window.show();
                (void)QTest::qWaitForWindowExposed(&window);
                QCoreApplication::processEvents();
                QLabel* status = window.status_label();
                int in_line = 0, in_band = 0;
                const auto whole = [&](const QString& sentence) {
                    QCoreApplication::processEvents();
                    const bool line_cut = status->sizeHint().width() > status->width();
                    if (window.last_notice() == sentence) {  // (in the status line: as wide as its words need, or wider)
                        ++in_line;
                        return !line_cut;
                    }
                    // (in the band: all of it, in a label that wraps; the line says where it is, whole — or, too short
                    // even for that, says nothing of it)
                    if (status->text().contains(sentence.left(8))) return false;
                    if (status->text().contains(pointer) && line_cut) return false;
                    const bool there = window.fonts_notice() == sentence && window.fonts_band()->isVisible() && window.fonts_band_text()->wordWrap() &&
                                       QTextDocumentFragment::fromHtml(window.fonts_band_text()->text()).toPlainText() == sentence;
                    in_band += there ? 1 : 0;
                    return there;
                };
                window.canvas()->selected_line_id = std::string("a");
                window.on_line_selected("a", false);
                for (const auto& face : app::lettering::catalog_faces()) {
                    const core::fonts::Entry* entry = core::fonts::find(face.key.toStdString());
                    const core::fonts::Entry* partner = entry != nullptr && !entry->fallback.empty() ? core::fonts::find(entry->fallback) : nullptr;
                    window.use_font_face(face.key, QStringLiteral("line"));
                    QVERIFY2(whole(partner != nullptr ? QStringLiteral("選んだ台詞の書体を「%1」にしました（漢字は「%2」で描きます）").arg(face.label, QString::fromStdString(partner->label))
                                                      : QStringLiteral("選んだ台詞の書体を「%1」にしました（元に戻す で取り消せます）").arg(face.label)),
                             qPrintable(QStringLiteral("%1 at %2: %3").arg(face.key).arg(window.width()).arg(window.last_notice())));
                    window.use_font_face(face.key, QStringLiteral("next"));
                    QVERIFY2(whole(QStringLiteral("書体「%1」を選びました。置きたい所をクリックして、台詞を打ちます").arg(face.label)),
                             qPrintable(QStringLiteral("%1 at %2: last [%3] status [%4] %5/%6 band [%7]")
                                            .arg(face.key)
                                            .arg(window.width())
                                            .arg(window.last_notice(), status->text())
                                            .arg(status->sizeHint().width())
                                            .arg(status->width())
                                            .arg(window.fonts_notice())));
                    if (face.key == QLatin1String("antique")) continue;  // (the book's usual face already)
                    window.use_font_face(face.key, QStringLiteral("book"));
                    QVERIFY2(whole(QStringLiteral("この本のいつもの書体を「%1」にしました（元に戻す で取り消せます）").arg(face.label)), qPrintable(face.key));
                }
                // (a wide window shows every one in its line; a narrow one sends the long ones to the band)
                if (width == 1300) QCOMPARE(in_band, 0);
                if (width == 760) QVERIFY2(in_band > 0 && window.width() < 1000, qPrintable(QStringLiteral("%1 in the band at %2").arg(in_band).arg(window.width())));
                qInfo("a window %d wide: %d sentences in the status line, %d in the band", window.width(), in_line, in_band);
            }
            qputenv("GENKO_CONFIG_DIR", previous);
        }
    }

    // The dock put away, or the window closed, while a face's file is being read off the screen's thread: nothing is
    // made while the dock is away, the rest comes when it is shown again, and a window closed in the middle leaves
    // nothing behind that the next one trips over.
    void theDockOrTheWindowClosedWhileAFileIsRead() {
        files::clear_cache();
        {
            Studio s;
            s.window->show_dock(QStringLiteral("書体"));
            QTRY_VERIFY_WITH_TIMEOUT(s.palette().samples_made() >= 1, 10000);
            s.window->font_dock()->hide();
            QTest::qWait(300);  // (whatever was being read is read by now)
            const int made = s.palette().samples_made();
            QVERIFY(made < 47);
            QTest::qWait(300);
            QCOMPARE(s.palette().samples_made(), made);  // (away: no more is made)
            s.window->show_dock(QStringLiteral("書体"));
            QTRY_VERIFY_WITH_TIMEOUT(s.palette().samples_drawn(), 60000);
            QCOMPARE(s.palette().samples_made(), 47);
            for (const auto& face : app::lettering::catalog_faces()) QVERIFY2(!s.palette().item(face.key)->icon().isNull(), qPrintable(face.key));
        }
        files::clear_cache();
        for (int round = 0; round < 3; ++round) {  // (closed at once, a file being read: three times over)
            Studio s;
            s.window->show_dock(QStringLiteral("書体"));
            QTRY_VERIFY_WITH_TIMEOUT(s.palette().samples_made() >= 1 + round, 10000);
        }
        {
            Studio s;
            s.open_palette();
            QTRY_VERIFY_WITH_TIMEOUT(s.palette().samples_drawn(), 60000);
            QCOMPARE(s.palette().files_read_ahead(), 40);
        }
    }

    // Moved to a screen of another scale, the samples are made again for it, one at a time as the first time — and each
    // face's file is read ahead by the worker again (the forty are more than is kept: the screen's thread would read the
    // ones let go), not taken as read because it was read once.
    void aScreenOfAnotherScaleMakesTheSamplesAgain() {
        Studio s;
        s.open_palette();
        QTRY_VERIFY_WITH_TIMEOUT(s.palette().samples_drawn(), 60000);
        QCOMPARE(s.palette().files_read_ahead(), 40);
        files::clear_cache();  // (as when many faces were used since: their bytes let go)
        QEvent moved(QEvent::DevicePixelRatioChange);
        QCoreApplication::sendEvent(&s.palette(), &moved);
        QCOMPARE(s.palette().samples_made(), 0);
        QCOMPARE(s.palette().files_read_ahead(), 0);
        QVERIFY(!s.palette().samples_drawn());
        QTRY_VERIFY_WITH_TIMEOUT(s.palette().samples_drawn(), 60000);
        QCOMPARE(s.palette().samples_made(), 47);
        QCOMPARE(s.palette().files_read_ahead(), 40);
        QVERIFY(files::cached_bytes() > 0);
    }

    // The export window's note on an editable PSD, for a line in a face whose file is not on this computer: the face by
    // its name — not its key —, and what happens as it is: drawn otherwise here, named in the PSD (the second review's
    // P3-6); for a face with only some kanji, that the PSD names it for every letter.
    void thePsdNoteNamesTheFaceNotItsKey() {
        QTemporaryDir empty;
        files::set_dir_for_testing(empty.path().toStdString());
        render::text::clear_layout_cache();
        {
            Studio s([](core::Document& doc) {
                doc.story.push_back(a_story_line("a", "gothic_kiwami"));
                doc.story.push_back(a_story_line("b", "sfx_inline", 80));
                doc.story.push_back(a_story_line("c", "/no/such/font.otf", 120));
                doc.features.emplace_back(core::fonts::kFontsFeature);
            });
            app::ExportDialog dialog(s.window.get(), s.session->snapshot(), gui_test::path_of(s.tmp.path() + "/book"), "human:test");
            dialog.format->setCurrentIndex(dialog.format->findData(QStringLiteral("psd")));
            dialog.psd_text->setCurrentIndex(dialog.psd_text->findData(QStringLiteral("editable")));
            QCoreApplication::processEvents();
            const QString notes = dialog.psd_notes->text();
            QVERIFY2(notes.contains(QStringLiteral("書体「源暎きわみゴ」のファイルがこのパソコンに無いため、ここでは代わりの書体で描きます（PSD にはこの書体の名前を書きます）")),
                     qPrintable(notes));
            QVERIFY2(notes.contains(QStringLiteral("書体「トレイン（インライン）」のファイルがこのパソコンに無いため、ここでは代わりの書体で描きます"
                                                   "（PSD にはこの書体の名前を書きます。この書体に無い漢字も、PSD ではこの書体の名前になります）")),
                     qPrintable(notes));
            QVERIFY2(!notes.contains(QStringLiteral("gothic_kiwami")) && !notes.contains(QStringLiteral("sfx_inline")), qPrintable(notes));
            // (a font file of the person's that is not there: said as it always was)
            QVERIFY2(notes.contains(QStringLiteral("が見つからないため、既定の書体で描きます")), qPrintable(notes));
        }
        use_source_fonts();
    }

    // --- the screenshots (written when asked: GENKO_FONTS_GUI_OUT) -----------------------------------------------------
    // (not a test of their own, and never a SKIP: the audit of a stage's run refuses every skipped slot)
    void cleanupTestCase() {
        const QByteArray out = qgetenv("GENKO_FONTS_GUI_OUT");
        if (!out.isEmpty()) write_screenshots(out);
        files::set_dir_for_testing(std::nullopt);
    }

private:
    void write_screenshots(const QByteArray& out) {
        const QString dir = QString::fromUtf8(out);
        QDir().mkpath(dir);
        for (const char* mode : {"light", "dark"}) {
            const QString shade = QLatin1String(mode) == QLatin1String("light") ? QString() : QStringLiteral("-dark");
            {
                use_source_fonts();
                Studio s;
                app::settings()->setValue(QStringLiteral("ui/theme"), QString::fromLatin1(mode));
                app::theme::apply(qApp);
                const std::string id = s.put(Json{{"op", "add_line"}, {"text", "なんだ\nって！？"}, {"balloon", "shout"}, {"x_mm", 60}, {"y_mm", 50}, {"w_mm", 40}, {"h_mm", 44},
                                                  {"style", Json{{"font", "gothic_kiwami"}}}});
                s.put(Json{{"op", "add_line"}, {"text", "ちょっと\n待って…"}, {"balloon", "speech"}, {"x_mm", 120}, {"y_mm", 60}, {"w_mm", 34}, {"h_mm", 44},
                           {"style", Json{{"font", "antique_genei"}}}});
                s.put(Json{{"op", "add_line"}, {"text", "ピコーン"}, {"balloon", "none"}, {"x_mm", 70}, {"y_mm", 130}, {"w_mm", 50}, {"h_mm", 16}, {"wrap", "horizontal"},
                           {"style", Json{{"font", "dot16"}, {"size_mm", 8.0}}}});
                s.open_palette();
                QTRY_VERIFY_WITH_TIMEOUT(s.palette().samples_drawn(), 60000);
                s.choose(id);
                QTest::qWait(300);
                QVERIFY(s.window->grab().save(dir + QStringLiteral("/gui-fonts-window%1.png").arg(shade)));
                // (the palette alone, tall enough for a good part of the list; and searched)
                s.window->font_dock()->setFloating(true);
                s.window->font_dock()->resize(300, 1040);
                QTest::qWait(200);
                QVERIFY(s.window->font_dock()->grab().save(dir + QStringLiteral("/gui-fonts-palette%1.png").arg(shade)));
                s.window->arm_font_face(QStringLiteral("horror_tegomin"));
                QTest::qWait(200);
                QVERIFY(s.window->font_dock()->grab().save(dir + QStringLiteral("/gui-fonts-palette-marked%1.png").arg(shade)));
                s.palette().list()->scrollToBottom();
                QTest::qWait(200);
                QVERIFY(s.window->font_dock()->grab().save(dir + QStringLiteral("/gui-fonts-palette-end%1.png").arg(shade)));
                s.palette().search()->setText(QStringLiteral("叫び"));
                QTest::qWait(200);
                QVERIFY(s.window->font_dock()->grab().save(dir + QStringLiteral("/gui-fonts-search%1.png").arg(shade)));
                QVERIFY(s.window->text_settings()->grab().save(dir + QStringLiteral("/gui-fonts-text-tool%1.png").arg(shade)));
            }
            {  // (a computer without the files: the notice when the book opens, and the rows that say ファイルなし)
                QTemporaryDir some;
                for (const char* file : {"KleeOne-SemiBold.ttf", "GenEiAntiqueNv6-M.ttf", "ShipporiAntique-Regular.ttf"}) {
                    QFile::copy(QString::fromStdString(source_fonts()) + "/" + file, some.path() + "/" + file);
                }
                files::set_dir_for_testing(some.path().toStdString());
                render::text::clear_layout_cache();
                Studio s([](core::Document& doc) {
                    doc.story.push_back(a_story_line("a", "gothic_kiwami"));
                    doc.story.push_back(a_story_line("b", "klee", 80));
                    doc.features.emplace_back(core::fonts::kFontsFeature);
                });
                app::settings()->setValue(QStringLiteral("ui/theme"), QString::fromLatin1(mode));
                app::theme::apply(qApp);
                QTRY_VERIFY_WITH_TIMEOUT(s.window->fonts_notice().contains(QStringLiteral("このパソコンにありません")), 10000);
                QTest::qWait(300);
                QVERIFY(s.window->grab().save(dir + QStringLiteral("/gui-fonts-notice-no-file%1.png").arg(shade)));
                s.open_palette();
                QTRY_VERIFY_WITH_TIMEOUT(s.palette().samples_drawn(), 60000);
                s.window->font_dock()->setFloating(true);
                s.window->font_dock()->resize(300, 1040);
                QTest::qWait(300);
                QVERIFY(s.window->font_dock()->grab().save(dir + QStringLiteral("/gui-fonts-no-file%1.png").arg(shade)));
            }
            {  // (the width a window opens with, nobody having sized it: the longest sentences, whole — the one a command
               // leaves in the band, wrapped, and the longest the status line itself takes)
                QTemporaryDir empty, tmp;
                files::set_dir_for_testing(empty.path().toStdString());
                render::text::clear_layout_cache();
                const auto path = gui_test::path_of(tmp.path() + "/book");
                core::Document doc = gui_test::new_doc(2);
                doc.story.push_back(a_story_line("a", "antigothic_medium"));
                doc.story.push_back(a_story_line("b", "sfx_outline", 70));
                doc.story.push_back(a_story_line("c", "dot8", 100));
                doc.story.push_back(a_story_line("d", "/no/such/font.otf", 130));
                doc.features.emplace_back(core::fonts::kFontsFeature);
                gui_test::write_book(path, doc);
                auto session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
                auto window = std::make_unique<app::MainWindow>(session);
                app::settings()->setValue(QStringLiteral("ui/theme"), QString::fromLatin1(mode));
                app::theme::apply(qApp);
                window->show();
                QTRY_VERIFY_WITH_TIMEOUT(window->fonts_notice().contains(QStringLiteral("このパソコンにありません")), 10000);
                window->action(QStringLiteral("act_type_replace_missing"))->trigger();
                QTest::qWait(300);
                // (what was done and what was left, both whole: in the band here — the line is too short for either)
                QVERIFY(window->fonts_notice().contains(QStringLiteral("そのままにしました")));
                QVERIFY(!window->status_label()->text().contains(QStringLiteral("見つからないフォント")) ||
                        window->status_label()->sizeHint().width() <= window->status_label()->width());
                QVERIFY(window->grab().save(dir + QStringLiteral("/gui-fonts-default-width-band%1.png").arg(shade)));
                use_source_fonts();
                window->canvas()->selected_line_id = std::string("a");
                window->on_line_selected("a", false);
                window->use_font_face(QStringLiteral("hand_slackside"), QStringLiteral("line"));
                QTest::qWait(300);
                QVERIFY(window->grab().save(dir + QStringLiteral("/gui-fonts-default-width-status%1.png").arg(shade)));
                qInfo("the window as it opens: %d wide, the status line %d", window->width(), window->status_label()->width());
            }
            {  // (a book of before whose words are a face's key of today: the notice)
                use_source_fonts();
                QTemporaryDir tmp;
                const auto path = gui_test::path_of(tmp.path() + "/book");
                core::Document doc = gui_test::new_doc(2);
                doc.story.push_back(a_story_line("a", "gothic_black"));
                gui_test::write_book(path, doc);
                const QString file = tmp.path() + QStringLiteral("/book/project.json");
                Json project = core::parse_python_json(genko::test::read_bytes(file));
                project["features"] = Json::array();
                genko::test::write_bytes(file, core::dump_python_indent2(project) + "\n");
                auto session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
                auto window = std::make_unique<app::MainWindow>(session);
                app::settings()->setValue(QStringLiteral("ui/theme"), QString::fromLatin1(mode));
                app::theme::apply(qApp);
                window->resize(1300, 900);
                window->show();
                QTRY_VERIFY_WITH_TIMEOUT(window->fonts_notice().contains(QStringLiteral("この版から")), 10000);
                QTest::qWait(300);
                QVERIFY(window->grab().save(dir + QStringLiteral("/gui-fonts-notice-book-of-before%1.png").arg(shade)));
            }
            app::settings()->remove(QStringLiteral("ui/theme"));
        }
        app::theme::apply(qApp);
        use_source_fonts();
    }
};

QTEST_MAIN(TestGuiFonts)
#include "test_gui_fonts.moc"
