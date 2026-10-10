// TEXT-01 in the window (AC-TEXT, label gui): a line's ruby typed with the input method in the canvas's editor (the
// table's cases with "gui": the words being converted never change the book; the settled words follow the edit rules,
// each ruby keeping its own settings), each ruby's own settings under the line's ruby list (書体・大きさ・揃え・本文との
// 距離・字間: an edit_line of the line's ruby_spans), rubies that need review (要確認) marked in the 台詞 list and listed
// with the line, reassigned (keeping the ruby's settings) or cleared there, Undo
// giving back the text, range and reading, 台詞の検索・置換 following the same rules, 縦書き・横書き kept, and the book
// saved and read back with its ruby.

#include <QtTest>

#include <QApplication>
#include <QCheckBox>
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
    // (one row a case: each reported on its own)
    void typedWithTheInputMethod_data() {
        QTest::addColumn<int>("index");
        const Json table = genko::test::read_json(genko::test::test_data("ruby_cases.json"));
        int rows = 0;
        for (std::size_t i = 0; i < table["cases"].size(); ++i) {
            const Json& c = table["cases"][i];
            if (!c.value("gui", false)) continue;
            QTest::newRow(c["n"].get<std::string>().c_str()) << static_cast<int>(i);
            ++rows;
        }
        QVERIFY(rows >= 9);
    }

    void typedWithTheInputMethod() {
        QFETCH(int, index);
        const Json table = genko::test::read_json(genko::test::test_data("ruby_cases.json"));
        {
            const Json& c = table["cases"][static_cast<std::size_t>(index)];
            const QString name = QString::fromStdString(c["n"].get<std::string>());
            Studio s;
            Json add{{"op", "add_line"}, {"id", "L1"}, {"text", c["text"]}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50}};
            if (c.contains("spans")) add["ruby_spans"] = c["spans"];
            if (c.contains("runs")) add["ruby_runs"] = c["runs"];
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
                if (step.contains("delete")) {  // (the editor's characters [from, to) selected and deleted)
                    cursor.setPosition(step["delete"][0].get<int>());
                    cursor.setPosition(step["delete"][1].get<int>(), QTextCursor::KeepAnchor);
                    editor->setTextCursor(cursor);
                    QTest::keyClick(editor, Qt::Key_Delete);
                    QCOMPARE(s.ops.size(), count);
                    continue;
                }
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
        }
    }

    // The chosen ruby's own settings under the line's ruby list: each change an edit_line of the line's ruby_spans, that
    // ruby's entry carrying it (the other rubies and the line's style untouched; back to the default: the key goes). The
    // select tool's style page keeps Python's ruby_scale and mono_ruby only. 振り直す… keeps the ruby's settings.
    void rubySettings() {
        Studio s;
        QVERIFY(s.put(Json{{"op", "add_line"}, {"id", "L1"}, {"text", "東京と大阪"}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50},
                           {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "とうきょう"}, {"start": 3, "end": 5, "ruby": "おおさか"}])")}}));
        s.window->on_line_selected("L1", false);
        app::StoryPanel& panel = s.panel();
        auto* rubies = s.find<QListWidget>("story_rubies");
        auto* font = s.find<QComboBox>("story_ruby_font");
        auto* ratio = s.find<QDoubleSpinBox>("story_ruby_ratio");
        auto* align = s.find<QComboBox>("story_ruby_align");
        auto* gap = s.find<QDoubleSpinBox>("story_ruby_gap");
        auto* tracking = s.find<QDoubleSpinBox>("story_ruby_tracking");
        auto* reassign = s.find<QPushButton>("story_ruby_reassign");
        QVERIFY(rubies && font && ratio && align && gap && tracking && reassign);
        // with the ruby list, not on the style page
        for (QWidget* w : std::vector<QWidget*>{font, ratio, align, gap, tracking}) {
            QVERIFY2(panel.ruby_box->isAncestorOf(w), qPrintable(w->objectName()));
            QVERIFY2(!panel.style_box->isAncestorOf(w), qPrintable(w->objectName()));
        }
        QVERIFY(panel.style_box->isAncestorOf(panel.ruby_scale) && panel.style_box->isAncestorOf(panel.mono_ruby));
        // no ruby chosen: nothing to set
        rubies->setCurrentRow(-1);
        QTRY_VERIFY(!font->isEnabled() && !ratio->isEnabled() && !align->isEnabled() && !gap->isEnabled() && !tracking->isEnabled());
        rubies->setCurrentRow(1);
        QTRY_VERIFY(font->isEnabled() && ratio->isEnabled() && align->isEnabled() && gap->isEnabled() && tracking->isEnabled());
        // nothing set: the line's look (the ratio the line's)
        QCOMPARE(font->currentText(), QStringLiteral("本文と同じ"));
        QCOMPARE(ratio->value(), ratio->minimum());
        QVERIFY2(ratio->text().contains(QStringLiteral("行の設定")), qPrintable(ratio->text()));
        QCOMPARE(align->currentData().toString(), QStringLiteral("center"));
        QCOMPARE(gap->value(), 0.0);
        QCOMPARE(gap->suffix(), QStringLiteral(" mm"));
        QCOMPARE(tracking->value(), 0.0);
        const auto op = [](const char* id, const char* spans) {
            return Json{{"op", "edit_line"}, {"id", id}, {"ruby_spans", Json::parse(spans)}}.dump();
        };
        font->setCurrentIndex(font->findData(QStringLiteral("gothic")));
        emit font->activated(font->currentIndex());
        QCOMPARE(s.last().dump(), op("L1", R"([{"start": 0, "end": 2, "ruby": "とうきょう"}, {"start": 3, "end": 5, "ruby": "おおさか", "font": "gothic"}])"));
        ratio->setValue(0.6);
        emit ratio->editingFinished();
        QCOMPARE(s.last().dump(), op("L1", R"([{"start": 0, "end": 2, "ruby": "とうきょう"},
            {"start": 3, "end": 5, "ruby": "おおさか", "font": "gothic", "scale": 0.6}])"));
        align->setCurrentIndex(align->findData(QStringLiteral("head")));
        emit align->activated(align->currentIndex());
        gap->setValue(1.5);
        emit gap->editingFinished();
        tracking->setValue(0.25);
        emit tracking->editingFinished();
        QCOMPARE(s.last().dump(), op("L1", R"([{"start": 0, "end": 2, "ruby": "とうきょう"},
            {"start": 3, "end": 5, "ruby": "おおさか", "font": "gothic", "scale": 0.6, "align": "head", "gap_mm": 1.5, "tracking": 0.25}])"));
        QCOMPARE(spans_of(s.line("L1")).dump(), Json::parse(R"([{"start": 0, "end": 2, "base": "東京", "ruby": "とうきょう"},
            {"start": 3, "end": 5, "base": "大阪", "ruby": "おおさか", "font": "gothic", "scale": 0.6, "align": "head", "gap_mm": 1.5,
             "tracking": 0.25}])").dump());
        const Json style = s.line("L1")["style"];
        for (const char* key : {"ruby_font", "ruby_align", "ruby_gap_mm", "ruby_tracking", "ruby_scale"}) QVERIFY2(!style.contains(key), key);
        // a size under the least a ruby takes (0.25) but above 「行の設定」: the least
        ratio->setValue(0.22);
        emit ratio->editingFinished();
        QCOMPARE(s.last()["ruby_spans"][1]["scale"], Json(0.25));
        QCOMPARE(ratio->value(), 0.25);
        ratio->setValue(0.6);
        emit ratio->editingFinished();
        // back to the default: the key goes
        align->setCurrentIndex(align->findData(QStringLiteral("center")));
        emit align->activated(align->currentIndex());
        ratio->setValue(ratio->minimum());
        emit ratio->editingFinished();
        QCOMPARE(s.last().dump(), op("L1", R"([{"start": 0, "end": 2, "ruby": "とうきょう"},
            {"start": 3, "end": 5, "ruby": "おおさか", "font": "gothic", "gap_mm": 1.5, "tracking": 0.25}])"));
        // each ruby shows its own
        rubies->setCurrentRow(0);
        QTRY_COMPARE(font->currentData().toString(), QString());
        QCOMPARE(gap->value(), 0.0);
        rubies->setCurrentRow(1);
        QTRY_COMPARE(font->currentData().toString(), QStringLiteral("gothic"));
        QCOMPARE(gap->value(), 1.5);
        QCOMPARE(tracking->value(), 0.25);
        // the line chosen again: the same ruby's
        s.window->on_line_selected("L1", false);
        QTRY_COMPARE(font->currentData().toString(), QStringLiteral("gothic"));
        // 振り直す…: the reading changed, the ruby's settings kept
        rubies->setCurrentRow(1);
        s.answers.responder->get_text = [](const QString&, const QString& label, const QString& text) {
            return std::optional<QString>(label.contains(QStringLiteral("親文字")) ? text : QStringLiteral("おおざか"));
        };
        reassign->click();
        QCOMPARE(spans_of(s.line("L1")).dump(), Json::parse(R"([{"start": 0, "end": 2, "base": "東京", "ruby": "とうきょう"},
            {"start": 3, "end": 5, "base": "大阪", "ruby": "おおざか", "font": "gothic", "gap_mm": 1.5, "tracking": 0.25}])").dump());
        // a line of Python's (ruby_runs): its ruby given a setting goes by place
        QVERIFY(s.put(Json{{"op", "add_line"}, {"id", "L2"}, {"text", "東京へ"}, {"x_mm", 100}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50},
                           {"ruby_runs", Json::parse(R"([["東京", "とうきょう"]])")}}));
        s.window->on_line_selected("L2", false);
        QTRY_COMPARE(rubies->count(), 1);
        rubies->setCurrentRow(0);
        QTRY_VERIFY(tracking->isEnabled());
        tracking->setValue(0.5);
        emit tracking->editingFinished();
        QCOMPARE(s.last().dump(), op("L2", R"([{"start": 0, "end": 2, "ruby": "とうきょう", "tracking": 0.5}])"));
        QCOMPARE(spans_of(s.line("L2")).dump(), Json::parse(R"([{"start": 0, "end": 2, "base": "東京", "ruby": "とうきょう", "tracking": 0.5}])").dump());
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

    // 振り直す… and 外す on a line of Python's (ruby_runs): Python's op when its runs say the change (the line stays
    // Python's), the rubies by place when they cannot (the same word twice, the second chosen).
    void pythonLineReassignAndClear() {
        Studio s;
        QVERIFY(s.put(Json{{"op", "add_line"}, {"id", "L1"}, {"text", "東京と大阪"}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50},
                           {"ruby_runs", Json::parse(R"([["東京", "A"], ["大阪", "B"]])")}}));
        s.window->on_line_selected("L1", false);
        auto* rubies = s.find<QListWidget>("story_rubies");
        auto* reassign = s.find<QPushButton>("story_ruby_reassign");
        auto* clear = s.find<QPushButton>("story_ruby_clear");
        QVERIFY(rubies && reassign && clear);
        QTRY_COMPARE(rubies->count(), 2);
        rubies->setCurrentRow(1);
        s.answers.responder->get_text = [](const QString&, const QString& label, const QString& text) {
            return std::optional<QString>(label.contains(QStringLiteral("親文字")) ? text : QStringLiteral("C"));
        };
        reassign->click();
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", "L1"}, {"ruby_runs", Json::parse(R"([["東京", "A"], ["大阪", "C"]])")}}).dump());
        QVERIFY(!s.line("L1").contains("ruby_spans"));
        rubies->setCurrentRow(0);
        clear->click();
        QCOMPARE(s.last().dump(), (Json{{"op", "edit_line"}, {"id", "L1"}, {"ruby_runs", Json::parse(R"([["大阪", "C"]])")}}).dump());
        QVERIFY(!s.line("L1").contains("ruby_spans"));
        QCOMPARE(s.line("L1")["ruby_runs"], Json::parse(R"([["大阪", "C"]])"));
        // the same word twice, the second chosen: Python's runs would put it on the first
        QVERIFY(s.put(Json{{"op", "add_line"}, {"id", "L2"}, {"text", "東京と東京"}, {"x_mm", 100}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50},
                           {"ruby_runs", Json::parse(R"([["東京", "A"]])")}}));
        s.window->on_line_selected("L2", false);
        QTRY_COMPARE(rubies->count(), 1);
        rubies->setCurrentRow(0);
        s.answers.responder->get_text = [](const QString&, const QString&, const QString& text) { return std::optional<QString>(text); };
        s.answers.responder->get_item = [](const QString&, const QString&, const QStringList& items, int, bool) {
            return items.size() == 2 ? std::optional<QString>(items[1]) : std::optional<QString>();
        };
        reassign->click();
        QCOMPARE(spans_of(s.line("L2")).dump(), Json::parse(R"([{"start": 3, "end": 5, "base": "東京", "ruby": "A"}])").dump());
    }

    // Undo after the whole base was deleted: the text, the range, the reading and the ruby's settings come back.
    void undoGivesTheRubyBack() {
        Studio s;
        QVERIFY(s.put(Json{{"op", "add_line"}, {"id", "L1"}, {"text", "東京へ"}, {"x_mm", 40}, {"y_mm", 40}, {"w_mm", 40}, {"h_mm", 50},
                           {"ruby_spans", Json::parse(R"([{"start": 0, "end": 2, "ruby": "とうきょう", "align": "tail", "gap_mm": 0.5}])")}}));
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
