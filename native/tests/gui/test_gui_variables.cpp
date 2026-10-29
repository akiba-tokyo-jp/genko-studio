// PC5 I-d (variables@1, SPEC.md AUTO-01「変数とデータセット」) in the window: ページ (Photoshop's イメージ) → 変数 ▸ 定義…・
// データセット… and データセットを適用… (after 複製…, before 解析 ▸), ファイル → 書き出し → データセットからファイル… (after カラー
// ルックアップテーブル…) and ファイル → 読み込み ▸ 変数データセット… (the first of 読み込み ▸, after PSD をレイヤーのまま読み込む…);
// each window asked, its op one Undo — 定義… (a variable of a layer and of a line, one its target does not take refused), データ
// セット… (values of each type, a picture's file chosen, 新規・削除), データセットを適用… (its preview, applied), データセットから
// ファイル… (a file a data set, the book unchanged), 変数データセット… (a text file read, its data sets the book's).

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>

#include <memory>
#include <string>

#include "app/main_window.hpp"
#include "app/variables_dialog.hpp"
#include "core/command_bus.hpp"
#include "core/variables.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "service/wording.hpp"

using namespace genko;
using core::Json;

namespace {

void write_bytes(const QString& path, const std::string& bytes) {
    QFile out(path);
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(bytes.data(), static_cast<qint64>(bytes.size()));
}

std::string png_of(int w, int h, std::array<int, 4> rgba) {
    std::string bytes(std::size_t(w) * h * 4, '\0');
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        for (int c = 0; c < 4; ++c) bytes[i + std::size_t(c)] = static_cast<char>(rgba[std::size_t(c)]);
    }
    return render::write_png(render::Image::frombytes("RGBA", render::Size{w, h}, bytes));
}

struct Book {
    QTemporaryDir tmp;
    std::filesystem::path path;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;

    Book() {
        (void)gui_test::config_folder();
        path = gui_test::path_of(tmp.path() + QStringLiteral("/b.genko"));
        gui_test::write_book(path, core::new_episode("t", core::Num(1), 2, core::PageSpec::b4_comic()));
        window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->resize(1280, 800);
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        answers.responder->question = [](const QString&, const QString&) { return true; };
        apply(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "photo"}, {"name", "photo"}});
        apply(Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "badge"}, {"name", "badge"}});
        apply(Json{{"op", "add_line"}, {"page", 1}, {"text", "もとの台詞"}, {"x_mm", 60}, {"y_mm", 60}});
    }
    QAction* act(const char* name) const { return window->action(QString::fromLatin1(name)); }
    const core::Document& doc() const { return window->book(); }
    bool apply(const Json& op) { return window->apply_ops(Json::array({op})); }
    const core::Layer& layer(const std::string& id) const {
        for (const core::Layer& l : doc().page(0).layers)
            if (l.id == id) return l;
        throw std::runtime_error("no layer " + id);
    }
    void choose_target(app::VariablesDialog* d, const QString& text) {
        for (int i = 0; i < d->target->count(); ++i) {
            if (d->target->itemText(i).contains(text)) d->target->setCurrentIndex(i);
        }
    }
    void choose(QComboBox* box, const QString& data) { box->setCurrentIndex(box->findData(data)); }
    // 定義… answered: show (badge), words (the line)
    void define() {
        answers.responder->exec = [this](QDialog* dialog) {
            auto* d = qobject_cast<app::VariablesDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            choose_target(d, QStringLiteral("レイヤー: badge"));
            choose(d->type, QStringLiteral("visibility"));
            d->name->setText(QStringLiteral("show"));
            if (!d->add_chosen()) return int(QDialog::Rejected);
            choose_target(d, QStringLiteral("台詞: もとの台詞"));
            choose(d->type, QStringLiteral("text"));
            d->name->setText(QStringLiteral("words"));
            if (!d->add_chosen()) return int(QDialog::Rejected);
            return int(QDialog::Accepted);
        };
        act("act_variables_define")->trigger();
    }
};

QStringList items(QMenu* menu) {
    QStringList out;
    for (QAction* a : menu->actions()) {
        if (a->isSeparator()) out << QStringLiteral("|");
        else if (a->menu() != nullptr) out << a->menu()->title() + QStringLiteral(" ▸");
        else out << a->text();
    }
    return out;
}

QMenu* top_menu(app::MainWindow* w, const QString& title) {
    for (QAction* top : w->menuBar()->actions()) {
        if (top->menu() != nullptr && top->menu()->title() == title) return top->menu();
    }
    return nullptr;
}

}  // namespace

class TestGuiVariables : public QObject {
    Q_OBJECT
private slots:
    // Their places and names (Photoshop's).
    void theMenus() {
        Book b;
        QMenu* pages = top_menu(b.window.get(), QStringLiteral("ページ"));
        QVERIFY(pages != nullptr);
        const QStringList in_pages = items(pages);
        const qsizetype duplicate = in_pages.indexOf(QStringLiteral("複製…"));
        QVERIFY(duplicate >= 0);
        QCOMPARE(in_pages.mid(duplicate, 4), (QStringList{QStringLiteral("複製…"), QStringLiteral("変数 ▸"), QStringLiteral("データセットを適用…"), QStringLiteral("解析 ▸")}));
        QMenu* variables = b.window->findChild<QMenu*>(QStringLiteral("menu_variables"));
        QVERIFY(variables != nullptr);
        QCOMPARE(items(variables), (QStringList{QStringLiteral("定義…"), QStringLiteral("データセット…")}));
        QMenu* exports = b.window->findChild<QMenu*>(QStringLiteral("menu_export"));
        QVERIFY(exports != nullptr);
        const QStringList out = items(exports);
        QCOMPARE(out.mid(out.size() - 2), (QStringList{QStringLiteral("カラールックアップテーブル…"), QStringLiteral("データセットからファイル…")}));
        QMenu* imports = b.window->findChild<QMenu*>(QStringLiteral("menu_import"));
        QVERIFY(imports != nullptr);
        QCOMPARE(items(imports), (QStringList{QStringLiteral("変数データセット…"), QStringLiteral("注釈…")}));  // (one 読み込み ▸, Photoshop's order)
        QCOMPARE(b.window->findChildren<QMenu*>(QStringLiteral("menu_import")).size(), qsizetype(1));
        const QStringList file = items(top_menu(b.window.get(), QStringLiteral("ファイル")));
        QCOMPARE(file.at(file.indexOf(QStringLiteral("PSD をレイヤーのまま読み込む…")) + 1), QStringLiteral("読み込み ▸"));
        for (const char* name : {"act_variables_define", "act_variables_data_sets", "act_apply_data_set", "act_data_sets_to_files", "act_import_data_sets"}) {
            QVERIFY2(b.act(name) != nullptr && b.act(name)->shortcuts().isEmpty(), name);
        }
    }

    // 定義…: a variable of a layer and one of a line; a type its target does not take refused (said); one Undo.
    void defineInTheWindow() {
        Book b;
        bool refused = false;
        b.answers.responder->exec = [&b, &refused](QDialog* dialog) {
            auto* d = qobject_cast<app::VariablesDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            b.choose_target(d, QStringLiteral("台詞: もとの台詞"));
            b.choose(d->type, QStringLiteral("visibility"));
            refused = !d->add_chosen();
            return int(QDialog::Rejected);
        };
        b.act("act_variables_define")->trigger();
        QVERIFY(refused);
        QVERIFY(!core::uses_variables(b.doc()));
        b.define();
        const core::Variables v = core::variables_of(b.doc());
        QCOMPARE(v.defs.size(), std::size_t(2));
        QCOMPARE(v.defs[0].name, std::string("show"));
        QCOMPARE(v.defs[0].layer, std::optional<std::string>("badge"));
        QVERIFY(v.defs[1].line.has_value());
        // (shown again: its rows)
        int rows = -1;
        b.answers.responder->exec = [&rows](QDialog* dialog) {
            if (auto* d = qobject_cast<app::VariablesDialog*>(dialog)) rows = d->table->rowCount();
            return int(QDialog::Rejected);
        };
        b.act("act_variables_define")->trigger();
        QCOMPARE(rows, 2);
        b.act("act_undo")->trigger();
        QVERIFY(!core::uses_variables(b.doc()));
    }

    // データセット…: a data set of each type of value (a picture chosen), 新規; データセットを適用… with its preview; one Undo each.
    void dataSetsAndApply() {
        Book b;
        b.define();
        b.answers.responder->exec = [&b](QDialog* dialog) {
            auto* d = qobject_cast<app::VariablesDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            b.choose_target(d, QStringLiteral("レイヤー: photo"));
            b.choose(d->type, QStringLiteral("pixels"));
            b.choose(d->method, QStringLiteral("conform"));
            d->name->setText(QStringLiteral("img"));
            return int(d->add_chosen() ? QDialog::Accepted : QDialog::Rejected);
        };
        b.act("act_variables_define")->trigger();
        QCOMPARE(core::variables_of(b.doc()).defs.size(), std::size_t(3));
        const QString picture = b.tmp.path() + QStringLiteral("/blue.png");
        write_bytes(picture, png_of(4, 4, {0, 0, 255, 255}));
        b.answers.responder->exec = [&picture](QDialog* dialog) {
            auto* d = qobject_cast<app::DataSetsDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            d->add_set();
            d->name->setText(QStringLiteral("A"));
            emit d->name->editingFinished();
            for (int r = 0; r < d->values->rowCount(); ++r) {
                const QString variable = d->values->item(r, 0)->text();
                if (variable == QStringLiteral("show")) qobject_cast<QComboBox*>(d->values->cellWidget(r, 2))->setCurrentIndex(2);  // (非表示)
                if (variable == QStringLiteral("words")) qobject_cast<QLineEdit*>(d->values->cellWidget(r, 2))->setText(QStringLiteral("新しい台詞"));
            }
            if (!d->set_picture(QStringLiteral("img"), picture)) return int(QDialog::Rejected);
            d->add_set();  // (B: no values)
            d->add_set();
            d->remove_set();  // (the third taken away)
            return int(QDialog::Accepted);
        };
        b.act("act_variables_data_sets")->trigger();
        const core::Variables v = core::variables_of(b.doc());
        QCOMPARE(v.data_sets.size(), std::size_t(2));
        QCOMPARE(v.data_sets[0].name, std::string("A"));
        QCOMPARE(v.data_sets[0].values.size(), std::size_t(3));
        QCOMPARE(v.data_sets[1].values.size(), std::size_t(0));
        // データセットを適用…: its preview drawn, A applied
        bool previewed = false;
        b.answers.responder->exec = [&previewed](QDialog* dialog) {
            auto* d = qobject_cast<app::ApplyDataSetDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            d->list->setCurrentRow(0);
            previewed = !d->preview->pixmap().isNull();
            return int(QDialog::Accepted);
        };
        b.act("act_apply_data_set")->trigger();
        QVERIFY(previewed);
        QVERIFY(!b.layer("badge").visible);
        QCOMPARE(b.doc().story.front().text, std::string("新しい台詞"));
        QVERIFY(b.layer("photo").raster_png != nullptr);
        QCOMPARE(core::variables_of(b.doc()).active, std::optional<std::int64_t>(0));
        b.act("act_undo")->trigger();
        QVERIFY(b.layer("badge").visible);
        QCOMPARE(b.doc().story.front().text, std::string("もとの台詞"));
        b.act("act_undo")->trigger();
        QVERIFY(core::variables_of(b.doc()).data_sets.empty());
    }

    // データセットからファイル…: a file a data set, the book unchanged; 変数データセット…: a text file's data sets the book's.
    void filesInAndOut() {
        Book b;
        b.define();
        const QString csv = b.tmp.path() + QStringLiteral("/sets.csv");
        write_bytes(csv, "データセット,show,words\n一,false,いち\n二,true,に\n");
        b.answers.responder->exec = [&csv](QDialog* dialog) {
            auto* d = qobject_cast<app::ImportDataSetsDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            d->file->setText(csv);
            return int(QDialog::Accepted);
        };
        b.act("act_import_data_sets")->trigger();
        core::Variables v = core::variables_of(b.doc());
        QCOMPARE(v.data_sets.size(), std::size_t(2));
        QCOMPARE(v.data_sets[1].name, std::string("二"));
        // (not replacing: added after)
        b.answers.responder->exec = [&csv](QDialog* dialog) {
            auto* d = qobject_cast<app::ImportDataSetsDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            d->file->setText(csv);
            d->replace->setChecked(false);
            return int(QDialog::Accepted);
        };
        b.act("act_import_data_sets")->trigger();  // (two of one name each: refused, the book as it was)
        QCOMPARE(core::variables_of(b.doc()).data_sets.size(), std::size_t(2));
        const QString folder = b.tmp.path() + QStringLiteral("/out");
        const std::string before = b.doc().story.front().text;
        b.answers.responder->exec = [&folder](QDialog* dialog) {
            auto* d = qobject_cast<app::DataSetsToFilesDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            d->folder->setText(folder);
            d->naming->setText(QStringLiteral("{index}"));
            return int(QDialog::Accepted);
        };
        b.act("act_data_sets_to_files")->trigger();
        const QStringList written = QDir(folder).entryList(QDir::Files, QDir::Name);
        QVERIFY2(written.size() >= 2, qPrintable(written.join(QStringLiteral(", "))));
        QVERIFY2(written.front().startsWith(QStringLiteral("001")), qPrintable(written.join(QStringLiteral(", "))));
        QCOMPARE(b.doc().story.front().text, before);
        QCOMPARE(core::variables_of(b.doc()).active, std::optional<std::int64_t>());
    }

    // The review's P1-1: a book whose variables cannot be read opens read only — the commands that change the book greyed (and
    // refused, said, if reached), データセットからファイル… saying it cannot use them: no command throws out of the window.
    void unreadableVariables() {
        (void)gui_test::config_folder();
        QTemporaryDir tmp;
        const std::filesystem::path path = gui_test::path_of(tmp.path() + QStringLiteral("/broken.genko"));
        core::Document doc = core::new_episode("t", core::Num(1), 1, core::PageSpec::b4_comic());
        const core::CommandBus bus(render::ops_registry());
        doc = bus.apply(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "photo"}, {"name", "photo"}},
                                          Json{{"op", "set_variables"}, {"defs", Json::array({Json{{"name", "show"}, {"type", "visibility"}, {"page", 1}, {"layer", "photo"}}})}},
                                          Json{{"op", "set_data_sets"}, {"data_sets", Json::array({Json{{"name", "A"}, {"values", Json::array({Json{{"variable", "show"}, {"value", false}}})}}})}}}),
                        core::Actor("human:test"))
                  .doc;
        gui_test::write_book(path, doc);
        const QString project = QString::fromStdString((path / "project.json").string());
        QFile in(project);
        QVERIFY(in.open(QIODevice::ReadOnly));
        Json saved = Json::parse(in.readAll().toStdString());
        in.close();
        saved["variables"]["defs"][0]["unknown"] = 1;  // (a key this build does not know)
        write_bytes(project, saved.dump());
        gui_test::Answers answers;
        answers.responder->exec = [](QDialog*) { return int(QDialog::Accepted); };
        auto window = std::make_unique<app::MainWindow>(app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery")))));
        window->show();
        for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
        QVERIFY(!window->book().read_only_reason.empty());
        for (const char* name : {"act_variables_define", "act_variables_data_sets", "act_apply_data_set", "act_import_data_sets"}) {
            QAction* act = window->action(QString::fromLatin1(name));
            QVERIFY2(act != nullptr && !act->isEnabled(), name);
            act->setEnabled(true);  // (reached anyway — a key before the menu was greyed: refused, said)
            act->trigger();
            QVERIFY2(window->last_error().contains(QStringLiteral("読み取り専用")), qPrintable(window->last_error()));
        }
        QAction* out = window->action(QStringLiteral("act_data_sets_to_files"));
        QVERIFY(out->isEnabled());
        out->trigger();
        QVERIFY2(window->last_error().contains(QStringLiteral("変数とデータセットを使えません")), qPrintable(window->last_error()));
    }

    // The review's P3-3: the words for a variable's name with spaces (the window's own names, 変数 1) said in Japanese.
    void wordsForNamesWithSpaces() {
        QCOMPARE(wording::error(std::string("variable 変数 1: a printed layer's pixels change only after name_ok on page 2 (strict_gates)")),
                 QStringLiteral("変数「変数 1」: 2 ページのネームが承認されるまで、印刷するレイヤーの画素は変えられません"));
        QCOMPARE(wording::error(std::string("data_sets[0].values[1]: show it (visibility) takes true or false")),
                 QStringLiteral("変数「show it」（表示）の値は true か false です"));
    }

    // The review's P3-4: 定義… binds the layer of the page it was opened on, though another page holds a layer of that id.
    void defineOnItsPage() {
        Book b;
        QVERIFY(b.apply(Json{{"op", "add_layer"}, {"page", 2}, {"kind", "paint"}, {"id", "photo"}, {"name", "photo on 2"}}));
        b.answers.responder->exec = [&b](QDialog* dialog) {
            auto* d = qobject_cast<app::VariablesDialog*>(dialog);
            if (d == nullptr) return int(QDialog::Rejected);
            b.choose_target(d, QStringLiteral("レイヤー: photo"));
            b.choose(d->type, QStringLiteral("visibility"));
            d->name->setText(QStringLiteral("p"));
            return int(d->add_chosen() ? QDialog::Accepted : QDialog::Rejected);
        };
        b.act("act_variables_define")->trigger();
        const core::Variables v = core::variables_of(b.doc());
        QCOMPARE(v.defs.size(), std::size_t(1));
        QCOMPARE(v.defs[0].page, b.doc().page(0).id);
    }
};

QTEST_MAIN(TestGuiVariables)
#include "test_gui_variables.moc"
