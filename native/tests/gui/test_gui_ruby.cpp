// TEXT-01 in the window (AC-TEXT, label gui): a line's ruby typed with the input method in the canvas's editor (the
// table's cases with "gui": the words being converted never change the book; the settled words follow the edit rules),
// the ruby settings beside the select tool (書体・大きさ・揃え・本文との距離・字間: each an edit_line of its own key),
// rubies that need review (要確認) marked in the 台詞 list and listed with the line, reassigned or cleared there, Undo
// giving back the text, range and reading, 台詞の検索・置換 following the same rules, 縦書き・横書き kept, and the book
// saved and read back with its ruby.

#include <QtTest>

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>

#include <memory>
#include <string>
#include <vector>

#include "app/canvas.hpp"
#include "app/ime.hpp"
#include "app/main_window.hpp"
#include "app/story_editor.hpp"
#include "app/story_panel.hpp"
#include "gui_support.hpp"
#include "storage/asset_store.hpp"
#include "storage/writer.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;

namespace {

struct Studio {
    QTemporaryDir tmp;
    std::filesystem::path path;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::vector<Json> ops;

    Studio() {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + "/book");
        gui_test::write_book(path, gui_test::new_doc(2));
        session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + "/recovery")));
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1300, 900);
        window->show();
        window->activateWindow();
        (void)QTest::qWaitForWindowActive(window.get());
        window->canvas()->fit_page();
        QObject::connect(session.get(), &app::Session::changed, window.get(), [this](const app::BookChange& c) {
            if (c.why == app::BookChange::Why::Edit)
                for (const Json& op : c.ops) ops.push_back(op);
        });
    }
    ~Studio() { window.reset(); }
    app::PageCanvas* canvas() const { return window->canvas(); }
    app::StoryPanel& panel() const { return *window->story(); }
    void trigger(const char* name) const { window->action(QString::fromLatin1(name))->trigger(); }
    Json last() const { return ops.empty() ? Json() : ops.back(); }
    std::string frame_id() const { return window->book().page(0).leaf_frames().front()->id; }
    bool put(Json op) {
        op["page"] = 1;
        if (!op.contains("frame_id")) op["frame_id"] = frame_id();
        return window->apply_ops(Json::array({op}));
    }
    // The line as the book would write it (project.json v4).
    Json line(const std::string& id) const {
        storage::AssetStore store(gui_test::path_of(tmp.path() + "/payload-assets"));
        const Json payload = storage::project_payload_v4(window->book(), store);  // (named: the loop reads into it)
        for (const Json& l : payload["story"]) {
            if (l["id"] == id) return l;
        }
        return Json::object();
    }
    template <class W>
    W* find(const char* name) const {
        return window->findChild<W*>(QString::fromLatin1(name));
    }
};

void ime_preedit(QWidget* widget, const QString& words) {
    QInputMethodEvent event(words, {});
    QCoreApplication::sendEvent(widget, &event);
}

void ime_commit(QWidget* widget, const QString& words) {
    QInputMethodEvent event;
    event.setCommitString(words);
    QCoreApplication::sendEvent(widget, &event);
}

void ctrl_enter(QWidget* editor) {
    QTest::keyPress(editor, Qt::Key_Control);
    QTest::keyPress(editor, Qt::Key_Return, Qt::ControlModifier);
    QWidget* focus = QApplication::focusWidget() != nullptr ? QApplication::focusWidget() : editor;
    QTest::keyRelease(focus, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyRelease(focus, Qt::Key_Control);
}

Json spans_of(const Json& line) { return line.contains("ruby_spans") ? line["ruby_spans"] : Json(); }

}  // namespace

class TestGuiRuby : public QObject {
    Q_OBJECT

private slots:
    // The table's cases typed in the canvas's editor with the input method.
    void typedWithTheInputMethod() {
        const Json table = genko::test::read_json(genko::test::test_data("ruby_cases.json"));
        int ran = 0;
        for (const Json& c : table["cases"]) {
            if (!c.value("gui", false)) continue;
            const QString name = QString::fromStdString(c["n"].get<std::string>());
            Studio s;
            Json add{{"op", "add_line"}, {"id", "L1"}, {"text", c["text"]}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50}};
            if (c.contains("spans")) add["ruby_spans"] = c["spans"];
            QVERIFY2(s.put(add), qPrintable(name));
            const Json before = s.line("L1");
            s.window->on_line_selected("L1", false);
            s.trigger("act_line_edit");
            app::InlineEditor* editor = s.canvas()->editor();
            QVERIFY2(editor != nullptr, qPrintable(name));
            QCOMPARE(editor->toPlainText(), QString::fromStdString(c["ime"]["shown"].get<std::string>()));
            const std::size_t count = s.ops.size();
            for (const Json& step : c["ime"]["steps"]) {
                QTextCursor cursor = editor->textCursor();
                cursor.setPosition(step["cursor"].get<int>());
                editor->setTextCursor(cursor);
                if (step.contains("preedit")) {
                    ime_preedit(editor, QString::fromStdString(step["preedit"].get<std::string>()));
                    QCOMPARE(editor->preedit(), QString::fromStdString(step["preedit"].get<std::string>()));
                    // the words being converted: nothing in the book changes
                    QCOMPARE(s.ops.size(), count);
                    QVERIFY2(genko::test::strict_equal(s.line("L1"), before), qPrintable(name));
                }
                if (step.contains("commit")) ime_commit(editor, QString::fromStdString(step["commit"].get<std::string>()));
            }
            if (c["ime"].value("cancel", false)) {
                QTest::keyClick(editor, Qt::Key_Escape);
            } else {
                ctrl_enter(editor);
            }
            QVERIFY2(s.canvas()->editor() == nullptr, qPrintable(name));
            const Json& want = c["want"];
            const Json got = s.line("L1");
            QVERIFY2(got["text"] == want["text"], qPrintable(name + QStringLiteral(": ") + QString::fromStdString(got["text"].dump())));
            std::string where;
            QVERIFY2(genko::test::same_content(spans_of(got), want["spans"], &where),
                     qPrintable(name + QStringLiteral(": ") + QString::fromStdString(spans_of(got).dump() + " " + where)));
            QVERIFY2(genko::test::strict_equal(got["ruby_runs"], want["runs"], &where), qPrintable(name));
            ++ran;
        }
        QVERIFY(ran >= 5);
    }

    // The settings beside the select tool: each its own key in one edit_line (the panel's other settings untouched).
    void rubySettings() {
        Studio s;
        QVERIFY(s.put(Json{{"op", "add_line"}, {"id", "L1"}, {"text", "東京へ"}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50},
                           {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "とうきょう"}])")}}));
        s.window->on_line_selected("L1", false);
        auto* font = s.find<QComboBox>("story_ruby_font");
        auto* align = s.find<QComboBox>("story_ruby_align");
        auto* gap = s.find<QDoubleSpinBox>("story_ruby_gap");
        auto* tracking = s.find<QDoubleSpinBox>("story_ruby_tracking");
        QVERIFY(font && align && gap && tracking);
        QCOMPARE(font->currentText(), QStringLiteral("本文と同じ"));
        QCOMPARE(align->currentData().toString(), QStringLiteral("center"));
        QCOMPARE(gap->suffix(), QStringLiteral(" mm"));
        font->setCurrentIndex(font->findData(QStringLiteral("gothic")));
        emit font->activated(font->currentIndex());
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", "L1"}, {"style", Json{{"ruby_font", "gothic"}}}}).dump());
        align->setCurrentIndex(align->findData(QStringLiteral("head")));
        emit align->activated(align->currentIndex());
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", "L1"}, {"style", Json{{"ruby_align", "head"}}}}).dump());
        gap->setValue(1.5);
        emit gap->editingFinished();
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", "L1"}, {"style", Json{{"ruby_gap_mm", 1.5}}}}).dump());
        tracking->setValue(0.25);
        emit tracking->editingFinished();
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", "L1"}, {"style", Json{{"ruby_tracking", 0.25}}}}).dump());
        const Json style = s.line("L1")["style"];
        QCOMPARE(style["ruby_font"], Json("gothic"));
        QCOMPARE(style["ruby_align"], Json("head"));
        QCOMPARE(style["ruby_gap_mm"], Json(1.5));
        QCOMPARE(style["ruby_tracking"], Json(0.25));
        // back to the defaults: the keys go
        align->setCurrentIndex(align->findData(QStringLiteral("center")));
        emit align->activated(align->currentIndex());
        QCOMPARE(s.last()["style"], Json({{"ruby_align", nullptr}}));
        // the panel shows the line's settings again
        s.window->on_line_selected("L1", false);
        QTRY_COMPARE(font->currentData().toString(), QStringLiteral("gothic"));
        QCOMPARE(gap->value(), 1.5);
    }

    // 要確認: marked in the list, listed with the line, reassigned (the base chosen, its reading given) or cleared.
    void reviewReassignAndClear() {
        Studio s;
        QVERIFY(s.put(Json{{"op", "add_line"}, {"id", "L1"}, {"text", "東新京へ"}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50},
                           {"ruby_spans", Json::parse(R"([{"start": 0, "end": 3, "ruby": "とうきょう", "base": "東京", "review": "edited"}])")}}));
        s.window->on_line_selected("L1", false);
        app::StoryPanel& panel = s.panel();
        QVERIFY(panel.list->count() == 1);
        QVERIFY2(panel.list->item(0)->text().contains(QStringLiteral("ルビ要確認")), qPrintable(panel.list->item(0)->text()));
        auto* rubies = s.find<QListWidget>("story_rubies");
        auto* reassign = s.find<QPushButton>("story_ruby_reassign");
        auto* clear = s.find<QPushButton>("story_ruby_clear");
        QVERIFY(rubies && reassign && clear);
        QCOMPARE(rubies->count(), 1);
        QVERIFY2(rubies->item(0)->text().contains(QStringLiteral("要確認")), qPrintable(rubies->item(0)->text()));
        QVERIFY(rubies->item(0)->text().contains(QStringLiteral("東京《とうきょう》")));
        rubies->setCurrentRow(0);
        // 振り直す: the base asked (the old one offered), then its reading (the old one offered)
        QStringList asked;
        s.answers.responder->get_text = [&](const QString& title, const QString& label, const QString& text) {
            asked << title + QStringLiteral("|") + label + QStringLiteral("|") + text;
            if (label.contains(QStringLiteral("親文字"))) return std::optional<QString>(QStringLiteral("東新京"));
            return std::optional<QString>(QStringLiteral("とうしんきょう"));
        };
        reassign->click();
        QCOMPARE(asked.size(), 2);
        QVERIFY2(asked[0].endsWith(QStringLiteral("|東京")), qPrintable(asked[0]));
        QVERIFY2(asked[1].endsWith(QStringLiteral("|とうきょう")), qPrintable(asked[1]));
        QCOMPARE(spans_of(s.line("L1")).dump(), Json::parse(R"([{"start": 0, "end": 3, "base": "東新京", "ruby": "とうしんきょう"}])").dump());
        QTRY_VERIFY(!panel.list->item(0)->text().contains(QStringLiteral("ルビ要確認")));
        // a base the text does not have: said, nothing changes
        s.answers.responder->get_text = [](const QString&, const QString& label, const QString&) {
            return std::optional<QString>(label.contains(QStringLiteral("親文字")) ? QStringLiteral("大阪") : QStringLiteral("おおさか"));
        };
        const std::size_t count = s.ops.size();
        rubies->setCurrentRow(0);
        reassign->click();
        QCOMPARE(s.ops.size(), count);
        QVERIFY2(s.window->last_notice().contains(QStringLiteral("見つかりません")), qPrintable(s.window->last_notice()));
        // 外す
        rubies->setCurrentRow(0);
        clear->click();
        QVERIFY(spans_of(s.line("L1")).is_null());
        QCOMPARE(s.line("L1")["ruby_runs"], Json::array());
        QTRY_COMPARE(rubies->count(), 0);
        // the same word twice: the one meant is chosen from a list
        QVERIFY(s.put(Json{{"op", "add_line"}, {"id", "L2"}, {"text", "東京と東京"}, {"x_mm", 100}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50},
                           {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "とうきょう", "base": "東京", "review": "ambiguous"}])")}}));
        s.window->on_line_selected("L2", false);
        QTRY_COMPARE(rubies->count(), 1);
        rubies->setCurrentRow(0);
        s.answers.responder->get_text = [](const QString&, const QString& label, const QString& text) {
            return std::optional<QString>(label.contains(QStringLiteral("親文字")) ? QStringLiteral("東京") : text);
        };
        QStringList choices;
        s.answers.responder->get_item = [&](const QString&, const QString&, const QStringList& items, int, bool) {
            choices = items;
            return items.size() == 2 ? std::optional<QString>(items[1]) : std::optional<QString>();
        };
        reassign->click();
        QCOMPARE(choices.size(), 2);
        QCOMPARE(spans_of(s.line("L2")).dump(), Json::parse(R"([{"start": 3, "end": 5, "base": "東京", "ruby": "とうきょう"}])").dump());
    }

    // Undo after the whole base was deleted: the text, the range and the reading come back.
    void undoGivesTheRubyBack() {
        Studio s;
        QVERIFY(s.put(Json{{"op", "add_line"}, {"id", "L1"}, {"text", "東京へ"}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50},
                           {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "とうきょう"}])")}}));
        const Json before = s.line("L1");
        QVERIFY(s.window->apply_ops(Json::parse(R"([{"op": "edit_line", "id": "L1", "text": "へ"}])")));
        QVERIFY(spans_of(s.line("L1")).is_null());
        s.trigger("act_undo");
        QTRY_VERIFY(gui_test::wait_for([&] { return s.line("L1")["text"] == Json("東京へ"); }));
        std::string where;
        QVERIFY2(genko::test::strict_equal(s.line("L1"), before, &where), where.c_str());
    }

    // 台詞の検索・置換 under the same rules: a part of the base replaced → 要確認, shown in the list; 縦書き・横書き
    // keeps the ranges; the book saved and read back with its ruby.
    void replaceTurnAndSave() {
        Studio s;
        QVERIFY(s.put(Json{{"op", "add_line"}, {"id", "L1"}, {"text", "東京と東京へ"}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50},
                           {"ruby_spans", Json::parse(R"([{"start": 3, "end": 5, "ruby": "とうきょう"}])")}}));
        s.answers.responder->exec = [&](QDialog* dialog) {
            auto* d = qobject_cast<app::ReplaceDialog*>(dialog);
            if (d == nullptr) return 0;
            d->find->setText(QStringLiteral("と東"));
            d->replace->setText(QStringLiteral("の西"));
            d->replace_all();
            return 0;
        };
        s.trigger("act_replace");
        QCOMPARE(s.line("L1")["text"], Json("東京の西京へ"));
        QCOMPARE(spans_of(s.line("L1")).dump(),
                 Json::parse(R"([{"start": 2, "end": 5, "base": "東京", "ruby": "とうきょう", "review": "edited"}])").dump());
        s.window->on_line_selected("L1", false);
        QVERIFY(s.panel().list->item(0)->text().contains(QStringLiteral("ルビ要確認")));
        s.trigger("act_undo");
        QTRY_COMPARE(s.line("L1")["text"], Json("東京と東京へ"));
        // across and back down: the ranges as they were
        const Json spans = spans_of(s.line("L1"));
        s.window->on_line_selected("L1", false);
        s.trigger("act_line_wrap");
        QCOMPARE(s.line("L1")["wrap"], Json("horizontal"));
        QCOMPARE(spans_of(s.line("L1")).dump(), spans.dump());
        s.trigger("act_line_wrap");
        QCOMPARE(spans_of(s.line("L1")).dump(), spans.dump());
        // saved, read back
        QVERIFY(s.session->wait_saved(std::chrono::milliseconds(20000)));
        const core::Document disk = gui_test::read_book(s.path);
        QCOMPARE(disk.features, std::vector<std::string>({"ruby-layout@1"}));
        storage::AssetStore store(gui_test::path_of(s.tmp.path() + "/disk-assets"));
        const Json story = storage::project_payload_v4(disk, store)["story"];
        QCOMPARE(story[0]["ruby_spans"].dump(), spans.dump());
    }
};

QTEST_MAIN(TestGuiRuby)
#include "test_gui_ruby.moc"
