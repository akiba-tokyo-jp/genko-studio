// The book and its pages in the window (M4②c, Python's genko/app/main.py): 次のページと見開きにする／解除
// (_toggle_spread, set_spread), ノンブルの設定… (pages_panel.nombre_dialog), 原稿用紙の設定… (_paper_settings with
// dialogs.PaperDialog), このページのノンブルを隠す／出す (_toggle_page_nombre), 表紙・カバーを足す… (_add_cover_dialog),
// このページの担当… (_assignee_dialog), ほかの原稿のページを取り込む… (_merge_book: merge.copy_assets, then
// merge.import_op) and PSD をレイヤーのまま読み込む… (_import_psd); and the page list's menu (pages_panel.PageList._menu:
// the spreads and the page's nombre). The commands as Python's window has them; each dialog's fields as Python makes
// them; the ops each one sends, recorded as the window applies them and compared with the ops Python's window sends
// (written as Python's json.dumps writes them; the numbers from the Python reference on the same book: ops.facing_problem,
// PageSpec.describe, covers.spec_for, main._page_list, fileops.import_psd); the page list and the canvas after them; a
// book that changed while a question was open left alone; the refusals in the person's words.

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <filesystem>
#include <fstream>
#include <functional>

#include "app/canvas.hpp"
#include "app/dialogs.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/pages_panel.hpp"
#include "app/tiles.hpp"
#include "core/covers.hpp"
#include "core/geometry.hpp"
#include "core/ids.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

// A book of `pages` pages (A4, Python's new_episode("試し", 1, pages, PageSpec.a4_mono())) on disk, in a window (the
// session saves 50 ms after a change); none on disk: a new book not saved yet. Every op the window applies is recorded.
struct Studio {
    QTemporaryDir tmp;
    fs::path path;
    std::shared_ptr<app::Session> session;
    std::unique_ptr<app::MainWindow> window;
    gui_test::Answers answers;
    std::vector<Json> ops;
    std::function<void()> on_change;  // (called as each change is applied, before the window shows it)
    QMetaObject::Connection recording;

    // (written: the book's folder once it is written, before it is opened)
    explicit Studio(int pages = 4, const std::function<void(core::Document&)>& edit = {}, bool on_disk = true,
                    const std::function<void(const fs::path&)>& written = {}) {
        (void)gui_test::config_folder();
        core::Document doc = gui_test::new_doc(pages);
        if (edit) edit(doc);
        if (on_disk) {
            path = gui_test::path_of(tmp.path() + QStringLiteral("/book.genko"));
            gui_test::write_book(path, doc);
            if (written) written(path);
            session = app::Session::open(path, gui_test::quick(gui_test::path_of(tmp.path() + QStringLiteral("/recovery"))));
        } else {
            app::Session::Options options;
            options.actor = "human:tester";
            options.autosave = false;
            options.recovery_root = gui_test::path_of(tmp.path() + QStringLiteral("/recovery"));
            session = std::make_shared<app::Session>(doc, std::nullopt, options);
        }
        recording = QObject::connect(session.get(), &app::Session::changed, session.get(), [this](const app::BookChange& c) {
            if (c.why != app::BookChange::Why::Edit) return;
            if (on_change) on_change();
            for (Json op : c.ops) {
                if (op.is_object()) op.erase("_report");
                ops.push_back(op);
            }
        });
        window = std::make_unique<app::MainWindow>(session);
        window->resize(1300, 900);
        window->show();
        (void)QTest::qWaitForWindowExposed(window.get());
        window->canvas()->fit_page();
    }
    ~Studio() {
        window.reset();
        QObject::disconnect(recording);
    }

    const core::Document& book() const { return window->book(); }
    // The command by Python's name, run (false: the window has no such command).
    bool trigger(const char* name) const {
        QAction* act = window->action(QString::fromLatin1(name));
        if (act == nullptr) return false;
        act->trigger();
        return true;
    }
    // The page with this number in front (as a click in the page list).
    void on_page(int index) const { window->go_to_page(index); }
    // The ops applied since `from`, as Python's json.dumps writes them.
    std::string since(std::size_t from) const {
        Json out = Json::array();
        for (std::size_t i = from; i < ops.size(); ++i) out.push_back(ops[i]);
        return core::dump_python(out);
    }
    QString row_text(int row) const {
        QListWidgetItem* item = window->pages()->item(row);
        return item != nullptr ? item->text() : QString();
    }
};

QStringList menu_texts(const QMenu* menu) {
    QStringList out;
    for (QAction* a : menu->actions()) out << (a->isSeparator() ? QStringLiteral("|") : a->text());
    return out;
}

QMenu* top_menu(app::MainWindow* window, const QString& title) {
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() != nullptr && top->menu()->title() == title) return top->menu();
    }
    return nullptr;
}

QStringList menus_of(app::MainWindow* window, QAction* wanted) {
    QStringList found;
    std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& at) {
        for (QAction* a : menu->actions()) {
            if (a == wanted) found << at;
            if (a->menu() != nullptr) walk(a->menu(), at + QStringLiteral("/") + a->menu()->title());
        }
    };
    for (QAction* top : window->menuBar()->actions()) {
        if (top->menu() != nullptr) walk(top->menu(), top->menu()->title());
    }
    return found;
}

// The page list's right-click menu on this row: its items ("|" a separator), and the one named `pick` chosen.
QStringList page_menu(Studio& s, int row, const QString& pick = {}) {
    QStringList texts;
    app::PageList* list = s.window->pages();
    QTimer::singleShot(0, list, [&texts, pick] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr) return;
        texts = menu_texts(menu);
        QAction* chosen = nullptr;
        for (QAction* a : menu->actions()) {
            if (!pick.isEmpty() && a->text() == pick) chosen = a;
        }
        menu->close();
        if (chosen != nullptr) chosen->trigger();
    });
    emit list->customContextMenuRequested(list->visualItemRect(list->item(row)).center());
    return texts;
}

// A widget of a dialog in words, to compare with what Python's dialog has.
QString described(const QComboBox* box) {
    if (box == nullptr) return QStringLiteral("missing");
    QStringList items;
    for (int i = 0; i < box->count(); ++i) items << box->itemText(i) + QStringLiteral("=") + box->itemData(i).toString();
    return items.join(QStringLiteral("|")) + QStringLiteral(" @") + box->currentData().toString();
}

QString described(const QDoubleSpinBox* box) {
    if (box == nullptr) return QStringLiteral("missing");
    return QStringLiteral("%1..%2 step %3 decimals %4 '%5' = %6%7%8")
        .arg(box->minimum())
        .arg(box->maximum())
        .arg(box->singleStep())
        .arg(box->decimals())
        .arg(box->suffix())
        .arg(box->value())
        .arg(box->isEnabled() ? QString() : QStringLiteral(" off"))
        .arg(box->toolTip().isEmpty() ? QString() : QStringLiteral(" tip ") + box->toolTip());
}

QString described(const QSpinBox* box) {
    if (box == nullptr) return QStringLiteral("missing");
    return QStringLiteral("%1..%2 = %3%4")
        .arg(box->minimum())
        .arg(box->maximum())
        .arg(box->value())
        .arg(box->toolTip().isEmpty() ? QString() : QStringLiteral(" tip ") + box->toolTip());
}

QString described(const QCheckBox* box) {
    if (box == nullptr) return QStringLiteral("missing");
    return box->text() + (box->isChecked() ? QStringLiteral(" [x]") : QStringLiteral(" [ ]"));
}

// The name of a form's row (QFormLayout's label for the field).
QString label_of(QWidget* field) {
    if (field == nullptr) return QStringLiteral("missing");
    for (QWidget* up = field->parentWidget(); up != nullptr; up = up->parentWidget()) {
        for (QFormLayout* form : up->findChildren<QFormLayout*>()) {
            if (auto* label = qobject_cast<QLabel*>(form->labelForField(field))) return label->text();
        }
        if (auto* form = qobject_cast<QFormLayout*>(up->layout())) {
            if (auto* label = qobject_cast<QLabel*>(form->labelForField(field))) return label->text();
        }
    }
    return QString();
}

QString cancel_text(QDialog* dialog) {
    for (QDialogButtonBox* box : dialog->findChildren<QDialogButtonBox*>()) {
        if (QPushButton* b = box->button(QDialogButtonBox::Cancel)) return b->text();
    }
    return QStringLiteral("missing");
}

QImage rendered(const core::Document& book, std::size_t index, int dpi) {
    render::RenderOptions options;
    options.mode = "proof";
    options.skip_unported = true;
    const auto image = render::render_page(book.page(index), dpi, options, &book).image;
    const auto bytes = image.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), image.width(), image.height(), image.width() * 3, QImage::Format_RGB888)
        .convertToFormat(QImage::Format_RGB32);
}

// The page on the canvas, drawn as render_page draws the book now.
QImage shown(Studio& s) {
    if (!s.window->canvas()->wait_rendered(60000)) return {};
    return s.window->canvas()->renderer().compose(s.window->canvas()->base_dpi());
}

bool drawn_as_book(Studio& s) {
    const QImage now = shown(s);
    return !now.isNull() &&
           now == rendered(s.book(), static_cast<std::size_t>(s.window->page_index()), s.window->canvas()->base_dpi());
}

// An 8-bit RGB PSD of width × height with these layers (bottom first), each a box of one colour with its own
// transparency channel, its name in Unicode (luni) with an ASCII one beside it; no merged picture. (The same bytes as
// the Python reference's writer that checked them: genko.psd.read_psd reads 40×30 with 線画 and 色.)
struct PsdLayer {
    QString name;
    std::string ascii;
    std::uint32_t top, left, width, height;
    std::array<unsigned char, 4> rgba;
};

std::string psd_bytes(std::uint32_t width, std::uint32_t height, const std::vector<PsdLayer>& layers) {
    const auto u16 = [](std::string& out, unsigned v) { out += {static_cast<char>(v >> 8 & 0xFF), static_cast<char>(v & 0xFF)}; };
    const auto u32 = [](std::string& out, std::uint32_t v) {
        out += {static_cast<char>(v >> 24 & 0xFF), static_cast<char>(v >> 16 & 0xFF), static_cast<char>(v >> 8 & 0xFF),
                static_cast<char>(v & 0xFF)};
    };
    std::string records, data;
    for (const PsdLayer& l : layers) {
        u32(records, l.top);
        u32(records, l.left);
        u32(records, l.top + l.height);
        u32(records, l.left + l.width);
        u16(records, 4);
        for (const int channel : {-1, 0, 1, 2}) {
            u16(records, static_cast<unsigned>(channel) & 0xFFFF);
            u32(records, 2 + l.width * l.height);
        }
        records += "8BIMnorm";
        records += {static_cast<char>(255), '\0', '\0', '\0'};
        std::string pascal(1, static_cast<char>(l.ascii.size()));
        pascal += l.ascii;
        while (pascal.size() % 4 != 0) pascal += '\0';
        std::string luni;
        u32(luni, static_cast<std::uint32_t>(l.name.size()));
        for (const QChar c : l.name) u16(luni, c.unicode());
        while (luni.size() % 4 != 0) luni += '\0';
        std::string extra;
        u32(extra, 0);  // no mask
        u32(extra, 0);  // no blending ranges
        extra += pascal;
        extra += "8BIMluni";
        u32(extra, static_cast<std::uint32_t>(luni.size()));
        extra += luni;
        u32(records, static_cast<std::uint32_t>(extra.size()));
        records += extra;
        for (const unsigned char v : {l.rgba[3], l.rgba[0], l.rgba[1], l.rgba[2]}) {
            u16(data, 0);
            data.append(static_cast<std::size_t>(l.width) * l.height, static_cast<char>(v));
        }
    }
    std::string info;
    u16(info, static_cast<unsigned>(layers.size()));
    info += records;
    info += data;
    if (info.size() % 2 != 0) info += '\0';
    std::string out = "8BPS";
    u16(out, 1);
    out.append(6, '\0');
    u16(out, 3);
    u32(out, height);
    u32(out, width);
    u16(out, 8);
    u16(out, 3);
    u32(out, 0);
    u32(out, 0);
    u32(out, static_cast<std::uint32_t>(4 + info.size() + 4));
    u32(out, static_cast<std::uint32_t>(info.size()));
    out += info;
    u32(out, 0);
    return out;
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Another book to take pages from: `pages` pages, its page 2 holding a placed picture (an asset of its own), and in
// its assets/ what copy_assets does not take: a link named as an asset to a file outside the book, and a file whose
// bytes are not those its name says.
struct OtherBook {
    fs::path path;
    std::string picture;  // the placed picture's asset
    std::string secret;   // the link's name as an asset
    std::string wrong;    // the file that does not hold what its name says
    bool links = false;

    OtherBook(const QString& folder, int pages) : path(gui_test::path_of(folder)) {
        storage::AssetStore store(path);
        const std::string png = render::write_png(render::Image::frombytes("RGB", {4, 4}, std::string(48, '\x40')));
        picture = store.put_bytes(png, ".png");
        core::Document doc = gui_test::new_doc(pages, "ほかの原稿");
        if (pages >= 2) {
            core::Layer placed;
            placed.id = "placed-art";
            placed.kind = core::LayerKind::Placed;
            placed.asset = picture;
            placed.placement_mm = core::Rect{core::Num(10), core::Num(10), core::Num(40), core::Num(40)};
            doc.edit_page(1).layers.push_back(placed);
        }
        gui_test::write_book(path, doc);
        const fs::path outside = path.parent_path() / "outside.png";
        write_bytes(outside, "not this book's");
        secret = storage::AssetStore::ref("not this book's");
        std::error_code ec;
        fs::create_directories(store.path(secret, ".png").parent_path());
        fs::create_symlink(outside, store.path(secret, ".png"), ec);
        links = !ec;
        wrong = storage::AssetStore::ref("what the name says");
        write_bytes(store.path(wrong, ".png"), "something else");
    }
    std::string text() const { return core::path_to_utf8(path); }
};

}  // namespace

class TestGuiBook : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(gui_test::config_folder().isValid()); }

    void commandsAsPythonsWindowHasThem() {
        Studio s;
        struct Expected {
            const char* attribute;
            const char* text;
            const char* tip;
            const char* menu;
        };
        const std::vector<Expected> expected{
            {"act_spread", "次のページと見開きにする／解除", "", "ページ"},
            {"act_nombre", "ノンブルの設定…", "位置・書体・大きさ・始まりの番号・隠しノンブル", "ページ"},
            {"act_paper", "原稿用紙の設定…", "用紙・仕上がり・裁ち落とし・基本枠。変えるとコマや台詞も新しい枠に合わせて動きます", "ページ"},
            {"act_page_nombre", "このページのノンブルを隠す／出す", "", "ページ"},
            {"act_add_cover", "表紙・カバーを足す…", "表紙・裏表紙、または背と袖のあるカバー 1 枚", "ページ"},
            {"act_assignee", "このページの担当…", "ページを誰が描くかを決めます（ページ一覧に出ます）", "ページ"},
            {"act_merge_book", "ほかの原稿のページを取り込む…", "別の原稿（.genko）のページを、台詞や絵ごとこの原稿の後ろに足します（作品の結合）", "ファイル"},
            {"act_import_psd", "PSD をレイヤーのまま読み込む…",
             "Photoshop・CLIP STUDIO PAINT などの PSD／PSB を、レイヤー・フォルダー・マスク・合成モードのままこのページに", "ファイル"},
        };
        for (const Expected& e : expected) {
            QAction* a = s.window->action(QString::fromLatin1(e.attribute));
            QVERIFY2(a != nullptr, e.attribute);
            QCOMPARE(a->text(), QString::fromUtf8(e.text));
            QVERIFY2(a->shortcuts().isEmpty(), e.attribute);
            QCOMPARE(a->statusTip(), QString::fromUtf8(e.tip));
            // (Python's tooltip: "<title>  <key>\n<tip>".strip(); without a tip, Qt's own: the title)
            QCOMPARE(a->toolTip(), *e.tip != '\0' ? QString::fromUtf8(e.text) + QStringLiteral("  \n") + QString::fromUtf8(e.tip) : QString::fromUtf8(e.text));
            QVERIFY2(!a->isCheckable(), e.attribute);
            QVERIFY2(a->isEnabled(), e.attribute);  // (Python's are always on: what cannot be done is said when chosen)
            QCOMPARE(menus_of(s.window.get(), a), QStringList{QString::fromUtf8(e.menu)});
        }
        // ページ: Python's order (絵柄を選ぶ and チャットでの承認, of M5, not yet here)
        QMenu* pages = top_menu(s.window.get(), QStringLiteral("ページ"));
        QVERIFY(pages != nullptr);
        QCOMPARE(menu_texts(pages).mid(0, 16),
                 QStringList({QStringLiteral("ページを追加（この後ろに）"), QStringLiteral("このページを複製"), QStringLiteral("このページを消す…"), QStringLiteral("|"),
                              QStringLiteral("このページを前へ"), QStringLiteral("このページを後ろへ"), QStringLiteral("次のページと見開きにする／解除"), QStringLiteral("|"),
                              QStringLiteral("原稿用紙の設定…"), QStringLiteral("ノンブルの設定…"), QStringLiteral("このページのノンブルを隠す／出す"),
                              QStringLiteral("表紙・カバーを足す…"), QStringLiteral("このページの担当…"), QStringLiteral("アニメーション（タイムライン）"),
                              QStringLiteral("|"), QStringLiteral("コマ")}));
        // ファイル: after the scans, before the export (書き出し and 印刷, M4③b: test_gui_output)
        QMenu* file = top_menu(s.window.get(), QStringLiteral("ファイル"));
        QVERIFY(file != nullptr);
        const QStringList in_file = menu_texts(file);
        const int scanner = static_cast<int>(in_file.indexOf(QStringLiteral("スキャナーから取り込む…")));
        QVERIFY(scanner > 0);
        QCOMPARE(in_file.mid(scanner - 1, 7), QStringList({QStringLiteral("スキャン画像を線画にして取り込む…"), QStringLiteral("スキャナーから取り込む…"),
                                                           QStringLiteral("ほかの原稿のページを取り込む…"), QStringLiteral("PSD をレイヤーのまま読み込む…"),
                                                           QStringLiteral("書き出し…"), QStringLiteral("印刷…"), QStringLiteral("|")}));
    }

    // 次のページと見開きにする／解除: with the next page when they face each other; else, when the page before faces it,
    // asked first; else said. Again: the spread undone, both pages.
    void spreadWithTheFacingPage() {
        Studio s;  // (pages 1|2-3|4: a book bound on the right, page 1 on the left; facing_problem in Python's words)
        s.on_page(2);
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_spread"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_spread", "page": 2, "with": 3}, {"op": "set_spread", "page": 3, "with": 2}])"));
        QVERIFY(s.answers.asked.isEmpty());
        QVERIFY(s.book().page(1).spread_with && *s.book().page(1).spread_with == core::Num(3));
        QVERIFY(s.book().page(2).spread_with && *s.book().page(2).spread_with == core::Num(2));
        QVERIFY2(s.row_text(1).contains(QStringLiteral("\n見開き 2–3")), qPrintable(s.row_text(1)));
        QVERIFY2(s.row_text(2).contains(QStringLiteral("\n見開き 2–3")), qPrintable(s.row_text(2)));
        QVERIFY(drawn_as_book(s));
        // again: undone, both
        n = s.ops.size();
        QVERIFY(s.trigger("act_spread"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_spread", "page": 2, "with": null}, {"op": "set_spread", "page": 3, "with": null}])"));
        QVERIFY(!s.book().page(1).spread_with && !s.book().page(2).spread_with);
        QVERIFY(!s.row_text(1).contains(QStringLiteral("見開き")));
        // page 3: with page 4 it would be the two sides of one leaf; the page it faces is 2: asked
        s.on_page(3);
        n = s.ops.size();
        QVERIFY(s.trigger("act_spread"));
        QCOMPARE(s.answers.asked, QStringList{QStringLiteral("見開き: 3 ページと 4 ページは 1 枚の紙の表と裏なので、見開きになりません。\n"
                                                             "3 ページと向かい合うのは 2 ページです。2・3 ページを見開きにしますか？")});
        QCOMPARE(s.ops.size(), n);  // (no)
        s.answers.responder->question = [](const QString&, const QString&) { return true; };
        QVERIFY(s.trigger("act_spread"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_spread", "page": 2, "with": 3}, {"op": "set_spread", "page": 3, "with": 2}])"));
        // page 3 again: undone (page 3 first)
        n = s.ops.size();
        QVERIFY(s.trigger("act_spread"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_spread", "page": 3, "with": null}, {"op": "set_spread", "page": 2, "with": null}])"));
        // pages 1 and 4 face no page: said, nothing asked
        s.answers.asked.clear();
        for (const int page : {1, 4}) {
            s.on_page(page);
            n = s.ops.size();
            QVERIFY(s.trigger("act_spread"));
            QCOMPARE(s.window->last_notice(), QStringLiteral("%1 ページには向かい合うページがありません").arg(page));
            QCOMPARE(s.ops.size(), n);
        }
        QVERIFY(s.answers.asked.isEmpty());
        // one step to undo
        s.on_page(2);
        QVERIFY(s.trigger("act_spread"));
        QVERIFY(s.trigger("act_undo"));
        QVERIFY(!s.book().page(1).spread_with && !s.book().page(2).spread_with);
    }

    // The page list's menu (pages_panel.PageList._menu): a spread with the page after or before, or undone; the page's
    // nombre hidden or shown.
    void pageListMenuHasSpreadsAndTheNombre() {
        Studio s;
        const QStringList head{QStringLiteral("この後ろにページを追加"), QStringLiteral("このページを複製"), QStringLiteral("|"),
                               QStringLiteral("前へ移す"), QStringLiteral("後ろへ移す"), QStringLiteral("|")};
        const QStringList tail{QStringLiteral("|"), QStringLiteral("このページを消す…")};
        QCOMPARE(page_menu(s, 0), head + QStringList({QStringLiteral("2 ページと見開きにする"), QStringLiteral("このページのノンブルを隠す")}) + tail);
        QCOMPARE(page_menu(s, 1), head + QStringList({QStringLiteral("3 ページと見開きにする"), QStringLiteral("1 ページと見開きにする"),
                                                      QStringLiteral("このページのノンブルを隠す")}) + tail);
        QCOMPARE(page_menu(s, 3), head + QStringList({QStringLiteral("3 ページと見開きにする"), QStringLiteral("このページのノンブルを隠す")}) + tail);
        std::size_t n = s.ops.size();
        page_menu(s, 1, QStringLiteral("3 ページと見開きにする"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_spread", "page": 2, "with": 3}, {"op": "set_spread", "page": 3, "with": 2}])"));
        QVERIFY(s.row_text(2).contains(QStringLiteral("見開き 2–3")));
        QCOMPARE(page_menu(s, 2), head + QStringList({QStringLiteral("見開きを解除"), QStringLiteral("このページのノンブルを隠す")}) + tail);
        n = s.ops.size();
        page_menu(s, 2, QStringLiteral("見開きを解除"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_spread", "page": 3, "with": null}, {"op": "set_spread", "page": 2, "with": null}])"));
        // (offered, as Python's menu offers it, though pages 1 and 2 are one leaf: a person's book takes it)
        n = s.ops.size();
        page_menu(s, 1, QStringLiteral("1 ページと見開きにする"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_spread", "page": 2, "with": 1}, {"op": "set_spread", "page": 1, "with": 2}])"));
        n = s.ops.size();
        page_menu(s, 3, QStringLiteral("このページのノンブルを隠す"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_nombre", "page": 4, "numero": false}])"));
        QVERIFY(s.row_text(3).contains(QStringLiteral("\nノンブルなし")));
        QVERIFY(page_menu(s, 3).contains(QStringLiteral("このページのノンブルを出す")));
        n = s.ops.size();
        page_menu(s, 3, QStringLiteral("このページのノンブルを出す"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_nombre", "page": 4, "numero": true}])"));
        QVERIFY(!s.row_text(3).contains(QStringLiteral("ノンブルなし")));
    }

    // このページのノンブルを隠す／出す: the page in front's nombre, and the page drawn without it.
    void thePagesNombreIsHiddenAndShown() {
        Studio s;
        s.on_page(2);
        const QImage before = shown(s);
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_page_nombre"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_nombre", "page": 2, "numero": false}])"));
        QVERIFY(!s.book().page(1).numero);
        QVERIFY(s.row_text(1).contains(QStringLiteral("\nノンブルなし")));
        QVERIFY(drawn_as_book(s));
        QVERIFY(shown(s) != before);  // (the nombre gone from the page)
        n = s.ops.size();
        QVERIFY(s.trigger("act_page_nombre"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_nombre", "page": 2, "numero": true}])"));
        QVERIFY(s.book().page(1).numero);
        QVERIFY(drawn_as_book(s));
        QCOMPARE(shown(s), before);
    }

    // ノンブルの設定…: Python's dialog (pages_panel.nombre_dialog), the book's settings in it, and every setting sent.
    void nombreDialogAsPythons() {
        Studio s(2);
        QStringList seen;
        std::function<void(QDialog*)> answer;
        int result = QDialog::Accepted;
        s.answers.responder->exec = [&](QDialog* d) {
            seen.clear();
            seen << d->objectName() << d->windowTitle();
            auto* show = d->findChild<QCheckBox*>(QStringLiteral("nombre_show"));
            auto* position = d->findChild<QComboBox*>(QStringLiteral("nombre_position"));
            auto* font = d->findChild<QComboBox*>(QStringLiteral("nombre_font"));
            auto* size = d->findChild<QDoubleSpinBox*>(QStringLiteral("nombre_size"));
            auto* start = d->findChild<QSpinBox*>(QStringLiteral("nombre_start"));
            auto* hidden = d->findChild<QCheckBox*>(QStringLiteral("nombre_hidden"));
            auto* hidden_size = d->findChild<QDoubleSpinBox*>(QStringLiteral("nombre_hidden_size"));
            seen << described(show) << label_of(position) + QStringLiteral(": ") + described(position)
                 << label_of(font) + QStringLiteral(": ") + described(font) << label_of(size) + QStringLiteral(": ") + described(size)
                 << label_of(start) + QStringLiteral(": ") + described(start) << described(hidden)
                 << label_of(hidden_size) + QStringLiteral(": ") + described(hidden_size) << cancel_text(d);
            if (answer && show && position && font && size && start && hidden && hidden_size) answer(d);
            return result;
        };
        const QString fonts = QStringLiteral(
            "アンチック（かなは明朝・漢字はゴシック）=antique|ゴシック=gothic|明朝=mincho|丸ゴシック=maru|手書き風=hand|極太（効果音）=sfx|勢い（効果音）=sfx_pop");
        const QString positions = QStringLiteral("下の真ん中=bottom_center|下の外側=bottom_outside|上の外側=top_outside|外側の真ん中=side_outside");
        // as it is: every setting sent, the defaults (nombre.DEFAULTS)
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_nombre"));
        QCOMPARE(seen, QStringList({QStringLiteral("nombre_dialog"), QStringLiteral("ノンブルの設定"), QStringLiteral("ノンブルを入れる [x]"),
                                    QStringLiteral("位置: ") + positions + QStringLiteral(" @bottom_center"),
                                    QStringLiteral("書体: ") + fonts + QStringLiteral(" @gothic"),
                                    QStringLiteral("大きさ: 1..20 step 0.5 decimals 2 ' mm' = 3"),
                                    QStringLiteral("始まりの番号: 0..9999 = 1 tip 1 ページ目の番号（前の話から続けるとき）"),
                                    QStringLiteral("隠しノンブルを入れる（のど側の下、製本で見えなくなる所） [ ]"),
                                    QStringLiteral("隠しノンブルの大きさ: 1..10 step 1 decimals 2 ' mm' = 2"), QStringLiteral("やめる")}));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_nombre", "show": true, "position": "bottom_center", "font": "gothic", "size_mm": 3.0, "start": 1, "hidden": false, "hidden_size_mm": 2.0}])"));
        // each changed
        answer = [](QDialog* d) {
            auto* position = d->findChild<QComboBox*>(QStringLiteral("nombre_position"));
            position->setCurrentIndex(position->findData(QStringLiteral("top_outside")));
            auto* font = d->findChild<QComboBox*>(QStringLiteral("nombre_font"));
            font->setCurrentIndex(font->findData(QStringLiteral("mincho")));
            d->findChild<QDoubleSpinBox*>(QStringLiteral("nombre_size"))->setValue(4.5);
            d->findChild<QSpinBox*>(QStringLiteral("nombre_start"))->setValue(3);
            d->findChild<QCheckBox*>(QStringLiteral("nombre_hidden"))->setChecked(true);
            d->findChild<QDoubleSpinBox*>(QStringLiteral("nombre_hidden_size"))->setValue(2.5);
        };
        const QImage before = shown(s);
        n = s.ops.size();
        QVERIFY(s.trigger("act_nombre"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_nombre", "show": true, "position": "top_outside", "font": "mincho", "size_mm": 4.5, "start": 3, "hidden": true, "hidden_size_mm": 2.5}])"));
        QCOMPARE(s.book().nombre.value("start", Json()), Json(3));
        QVERIFY(drawn_as_book(s));
        QVERIFY(shown(s) != before);
        // shown again as the book has them now; やめる: nothing
        answer = nullptr;
        result = QDialog::Rejected;
        n = s.ops.size();
        QVERIFY(s.trigger("act_nombre"));
        QCOMPARE(seen.mid(2, 7), QStringList({QStringLiteral("ノンブルを入れる [x]"), QStringLiteral("位置: ") + positions + QStringLiteral(" @top_outside"),
                                              QStringLiteral("書体: ") + fonts + QStringLiteral(" @mincho"),
                                              QStringLiteral("大きさ: 1..20 step 0.5 decimals 2 ' mm' = 4.5"),
                                              QStringLiteral("始まりの番号: 0..9999 = 3 tip 1 ページ目の番号（前の話から続けるとき）"),
                                              QStringLiteral("隠しノンブルを入れる（のど側の下、製本で見えなくなる所） [x]"),
                                              QStringLiteral("隠しノンブルの大きさ: 1..10 step 1 decimals 2 ' mm' = 2.5")}));
        QCOMPARE(s.ops.size(), n);
    }

    // Settings written into the book by hand, shown as Python's lists and spin boxes take them (a value they do not
    // have: the first; numbers to two places, within their range; the number of page 1 from text), and sent so.
    void nombreDialogTakesTheBooksSettingsAsPythons() {
        Studio s(2, [](core::Document& doc) {
            doc.nombre = Json::object({{"size_mm", 3.333}, {"start", "7"}, {"font", "nope"}, {"position", 5}, {"show", 0}, {"hidden_size_mm", 30}});
        });
        s.answers.responder->exec = [](QDialog*) { return int(QDialog::Accepted); };
        const std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_nombre"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_nombre", "show": false, "position": "bottom_center", "font": "antique", "size_mm": 3.33, "start": 7, "hidden": false, "hidden_size_mm": 10.0}])"));
        // what Python cannot show (float("abc")): not opened, said; the book as it was
        Studio t(2, [](core::Document& doc) {
            doc.nombre = Json::object({{"size_mm", "abc"}});
            for (std::size_t i = 0; i < doc.pages.size(); ++i) doc.edit_page(i).numero = false;
        });
        const auto before = t.session->snapshot();
        QVERIFY(t.trigger("act_nombre"));
        QVERIFY(!t.answers.asked.contains(QStringLiteral("genko::app::NombreDialog")));
        QVERIFY(t.answers.asked.isEmpty());
        QCOMPARE(t.window->last_error(), QStringLiteral("ノンブルの値が不正です。原稿は変更していません。"));
        QCOMPARE(t.session->snapshot(), before);
    }

    // 原稿用紙の設定…: the paper dialog as Python's (changing: コマ・台詞・絵を… shown, 変える), a preset or every number
    // sent; the book on its new paper, the canvas fitted to it and drawn as the book; numbers that do not fit not taken.
    void paperDialogChangesThePaper() {
        Studio s;
        QStringList seen;
        std::function<int(app::PaperDialog*)> answer = [](app::PaperDialog*) { return int(QDialog::Accepted); };
        s.answers.responder->exec = [&](QDialog* d) {
            auto* paper = qobject_cast<app::PaperDialog*>(d);
            if (paper == nullptr) return int(QDialog::Rejected);
            seen = QStringList{paper->windowTitle(), described(paper->move), QString::number(paper->move->isVisibleTo(paper)), paper->ok_button()->text(),
                               paper->cancel_button()->text(), paper->preset->currentData().toString()};
            return answer(paper);
        };
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_paper"));
        QCOMPARE(seen, QStringList({QStringLiteral("原稿用紙の設定"), QStringLiteral("コマ・台詞・絵を新しい基本枠に合わせて動かす [x]"), QStringLiteral("1"),
                                    QStringLiteral("変える"), QStringLiteral("やめる"), QStringLiteral("a4")}));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_page_spec", "preset": "a4", "move": true}])"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("原稿用紙を変えました: 用紙 210×297 mm ・ 仕上がり 204×291 mm ・ 裁ち落とし 3 mm ・ 基本枠 184×271 mm ・ 600 dpi"));
        // a preset
        answer = [](app::PaperDialog* d) {
            d->preset->setCurrentIndex(d->preset->findData(QStringLiteral("b5")));
            return int(QDialog::Accepted);
        };
        n = s.ops.size();
        QVERIFY(s.trigger("act_paper"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_page_spec", "preset": "b5", "move": true}])"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("原稿用紙を変えました: 用紙 208×283 mm ・ 仕上がり 182×257 mm ・ 裁ち落とし 3 mm ・ 基本枠 150×220 mm ・ 600 dpi"));
        QCOMPARE(QString::fromStdString(s.book().spec.describe()), QStringLiteral("用紙 208×283 mm ・ 仕上がり 182×257 mm ・ 裁ち落とし 3 mm ・ 基本枠 150×220 mm ・ 600 dpi"));
        QCOMPARE(s.window->pages()->count(), 4);
        QVERIFY(drawn_as_book(s));
        // every number, without moving what is on the pages
        answer = [](app::PaperDialog* d) {
            d->paper_w->setValue(220);
            d->paper_h->setValue(300);
            d->trim_w->setValue(200);
            d->trim_h->setValue(280);
            d->bleed->setValue(4);
            d->top->setValue(15);
            d->bottom->setValue(16);
            d->inner->setValue(17);
            d->outer->setValue(18);
            d->dpi->setValue(350);
            d->move->setChecked(false);
            return int(QDialog::Accepted);
        };
        n = s.ops.size();
        const auto frames_before = s.book().page(0).frames;
        QVERIFY(s.trigger("act_paper"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_page_spec", "paper": [220.0, 300.0], "trim": [200.0, 280.0], "bleed_mm": 4.0, "margins": [15.0, 16.0, 17.0, 18.0], "dpi": 350, "move": false}])"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("原稿用紙を変えました: 用紙 220×300 mm ・ 仕上がり 200×280 mm ・ 裁ち落とし 4 mm ・ 基本枠 165×249 mm ・ 350 dpi"));
        QCOMPARE(core::rect_to_json(s.book().page(0).frames.front().rect), core::rect_to_json(frames_before.front().rect));  // (not moved)
        QVERIFY(drawn_as_book(s));
        // undone and done again: the page on each paper in turn
        QVERIFY(s.trigger("act_undo"));
        QCOMPARE(QString::fromStdString(s.book().spec.describe()), QStringLiteral("用紙 208×283 mm ・ 仕上がり 182×257 mm ・ 裁ち落とし 3 mm ・ 基本枠 150×220 mm ・ 600 dpi"));
        QVERIFY(drawn_as_book(s));
        QVERIFY(s.trigger("act_redo"));
        QVERIFY(drawn_as_book(s));
        // numbers that do not fit: 変える off, the reason in the person's words; nothing sent
        answer = [&seen](app::PaperDialog* d) {
            d->trim_w->setValue(300);
            seen << QString::number(d->ok_button()->isEnabled()) << d->summary->text();
            d->ok_button()->click();
            return d->result();
        };
        n = s.ops.size();
        QVERIFY(s.trigger("act_paper"));
        QCOMPARE(seen.mid(6), QStringList({QStringLiteral("0"), QStringLiteral("用紙が、仕上がりと裁ち落としより小さくなっています")}));
        QCOMPARE(s.ops.size(), n);
        // やめる
        answer = [](app::PaperDialog*) { return int(QDialog::Rejected); };
        QVERIFY(s.trigger("act_paper"));
        QCOMPARE(s.ops.size(), n);
    }

    // 表紙・カバーを足す…: the kinds the book has not got yet; the spine and flaps for a jacket or a band, the band's
    // height for a band (the others off); the cover at the end, shown; when all four are there, said.
    void coversAreAdded() {
        Studio s;
        QStringList seen;
        std::function<void(QDialog*)> answer;
        int result = QDialog::Accepted;
        s.answers.responder->exec = [&](QDialog* d) {
            seen.clear();
            auto* kind = d->findChild<QComboBox*>(QStringLiteral("cover_kind"));
            auto* spine = d->findChild<QDoubleSpinBox*>(QStringLiteral("cover_spine"));
            auto* flap = d->findChild<QDoubleSpinBox*>(QStringLiteral("cover_flap"));
            auto* band = d->findChild<QDoubleSpinBox*>(QStringLiteral("cover_band"));
            seen << d->windowTitle() << label_of(kind) + QStringLiteral(": ") + described(kind) << label_of(spine) + QStringLiteral(": ") + described(spine)
                 << label_of(flap) + QStringLiteral(": ") + described(flap) << label_of(band) + QStringLiteral(": ") + described(band);
            if (kind != nullptr && spine != nullptr && flap != nullptr && band != nullptr) {
                for (int i = 0; i < kind->count(); ++i) {  // (each kind: which numbers it takes)
                    kind->setCurrentIndex(i);
                    seen << kind->currentData().toString() + QStringLiteral(" ") + QString::number(spine->isEnabled()) + QString::number(flap->isEnabled()) +
                                QString::number(band->isEnabled());
                }
                kind->setCurrentIndex(0);
                if (answer) answer(d);
            }
            return result;
        };
        const QString kinds_all = QStringLiteral("表紙=front|裏表紙=back|カバー（表紙・背・裏表紙・袖）=jacket|帯（表・背・裏・袖、低い紙）=obi");
        // やめる: nothing
        result = QDialog::Rejected;
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_add_cover"));
        QCOMPARE(seen, QStringList({QStringLiteral("表紙・カバーを足す"), QStringLiteral("種類: ") + kinds_all + QStringLiteral(" @front"),
                                    QStringLiteral("背幅（カバー・帯）: 1..100 step 1 decimals 2 ' mm' = 10 off tip 背幅（ページ数と紙の厚さで決まります。印刷所に聞きます）"),
                                    QStringLiteral("袖（カバー・帯）: 0..200 step 1 decimals 2 ' mm' = 70 off"),
                                    QStringLiteral("帯の高さ: 15..200 step 1 decimals 2 ' mm' = 50 off tip 帯の高さ"), QStringLiteral("front 000"),
                                    QStringLiteral("back 000"), QStringLiteral("jacket 110"), QStringLiteral("obi 111")}));
        QCOMPARE(s.ops.size(), n);
        // 表紙
        result = QDialog::Accepted;
        QVERIFY(s.trigger("act_add_cover"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "add_cover", "kind": "front"}])"));
        QCOMPARE(s.book().pages.size(), std::size_t{5});
        QCOMPARE(s.window->page_index(), 4);  // (the cover in front)
        QCOMPARE(s.row_text(4), QStringLiteral("表紙"));
        QVERIFY(drawn_as_book(s));
        // カバー: 背幅 12.5, 袖 80 (its paper: covers.spec_for in Python, 586.5 × 297, finished 580.5 × 291)
        answer = [](QDialog* d) {
            auto* kind = d->findChild<QComboBox*>(QStringLiteral("cover_kind"));
            kind->setCurrentIndex(kind->findData(QStringLiteral("jacket")));
            d->findChild<QDoubleSpinBox*>(QStringLiteral("cover_spine"))->setValue(12.5);
            d->findChild<QDoubleSpinBox*>(QStringLiteral("cover_flap"))->setValue(80);
        };
        n = s.ops.size();
        QVERIFY(s.trigger("act_add_cover"));
        QCOMPARE(seen.at(1), QStringLiteral("種類: 裏表紙=back|カバー（表紙・背・裏表紙・袖）=jacket|帯（表・背・裏・袖、低い紙）=obi @back"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "add_cover", "kind": "jacket", "spine_mm": 12.5, "flap_mm": 80.0}])"));
        const core::PageSpec& jacket = s.book().page(5).spec;
        QCOMPARE(jacket.width_mm.value(), 586.5);
        QCOMPARE(jacket.height_mm.value(), 297.0);
        QCOMPARE(jacket.trim_size().first.value(), 580.5);
        QCOMPARE(jacket.trim_size().second.value(), 291.0);
        QCOMPARE(s.window->page_index(), 5);
        QCOMPARE(s.window->pages()->currentRow(), 5);
        QCOMPARE(s.row_text(5), QStringLiteral("カバー（表紙・背・裏表紙・袖）"));
        QVERIFY(drawn_as_book(s));
        // 帯: the spine and flaps as they are, 60 high (564 × 66, finished 558 × 60)
        answer = [](QDialog* d) {
            auto* kind = d->findChild<QComboBox*>(QStringLiteral("cover_kind"));
            kind->setCurrentIndex(kind->findData(QStringLiteral("obi")));
            d->findChild<QDoubleSpinBox*>(QStringLiteral("cover_band"))->setValue(60);
        };
        n = s.ops.size();
        QVERIFY(s.trigger("act_add_cover"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "add_cover", "kind": "obi", "spine_mm": 10.0, "flap_mm": 70.0, "height_mm": 60.0}])"));
        QCOMPARE(s.book().page(6).spec.width_mm.value(), 564.0);
        QCOMPARE(s.book().page(6).spec.height_mm.value(), 66.0);
        // 裏表紙, the last there is
        answer = nullptr;
        n = s.ops.size();
        QVERIFY(s.trigger("act_add_cover"));
        QCOMPARE(seen.at(1), QStringLiteral("種類: 裏表紙=back @back"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "add_cover", "kind": "back"}])"));
        QCOMPARE(s.row_text(7), QStringLiteral("裏表紙"));
        // all four there: said, no dialog
        seen.clear();
        n = s.ops.size();
        QVERIFY(s.trigger("act_add_cover"));
        QVERIFY(seen.isEmpty());
        QCOMPARE(s.window->last_notice(), QStringLiteral("表紙・裏表紙・カバー・帯は、もう全部あります"));
        QCOMPARE(s.ops.size(), n);
    }

    // このページの担当…: asked as Python asks (the name now in it), sent for the page in front, shown in the page list;
    // empty: none.
    void theAssigneeIsAskedAndShown() {
        Studio s(3);
        s.on_page(2);
        QStringList asked;
        std::optional<QString> answer = QStringLiteral("田中");
        s.answers.responder->get_text = [&](const QString& title, const QString& label, const QString& text) {
            asked << title << label << text;
            return answer;
        };
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_assignee"));
        QCOMPARE(asked, QStringList({QStringLiteral("このページの担当"), QStringLiteral("名前（空にすると担当なし）"), QString()}));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_assignee", "pages": [2], "who": "田中"}])"));
        QVERIFY2(s.row_text(1).endsWith(QStringLiteral("\n担当 田中")), qPrintable(s.row_text(1)));
        // again: the name in the question; emptied: none
        asked.clear();
        answer = QString();
        n = s.ops.size();
        QVERIFY(s.trigger("act_assignee"));
        QCOMPARE(asked.value(2), QStringLiteral("田中"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_assignee", "pages": [2], "who": ""}])"));
        QVERIFY(!s.row_text(1).contains(QStringLiteral("担当")));
        // cancelled: nothing
        answer = std::nullopt;
        n = s.ops.size();
        QVERIFY(s.trigger("act_assignee"));
        QCOMPARE(s.ops.size(), n);
    }

    // ほかの原稿のページを取り込む…: the other book's folder, then which pages; its asset files copied into this book
    // first (storage::copy_assets: those laid out as assets and holding what their names say; a link not followed),
    // then the pages taken in by import_pages (Python's merge.import_op: "from" as str(Path(folder)) writes it).
    void anotherBooksPagesAreTakenIn() {
        Studio s;
        const OtherBook other(s.tmp.path() + QStringLiteral("/other.genko"), 3);
        QStringList asked;
        QString folder = QString::fromStdString(other.text());
        std::optional<QString> pages = QStringLiteral("3、1〜2");
        s.answers.responder->existing_dir = [&](const QString& caption, const QString& start) {
            asked << caption << start;
            return folder;
        };
        s.answers.responder->get_text = [&](const QString& title, const QString& label, const QString& text) {
            asked << title << label << text;
            return pages;
        };
        const storage::AssetStore mine(s.path);
        bool copied_first = false;
        s.on_change = [&] { copied_first = mine.has(other.picture, ".png"); };
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_merge_book"));
        QCOMPARE(asked, QStringList({QStringLiteral("ページを取り込む原稿（.genko のフォルダ）"), QString(), QStringLiteral("作品の結合"),
                                     QStringLiteral("取り込むページ（1〜3。例: 1-4, 7。空ならすべて）"), QString()}));
        QCOMPARE(s.since(n), core::dump_python(Json::array({Json{{"op", "import_pages"}, {"from", other.text()}, {"pages", Json::array({3, 1, 2})}}})));
        QVERIFY(copied_first);  // (the picture was there when the pages went in)
        QVERIFY(mine.has(other.picture, ".png"));
        QVERIFY(!mine.has(other.secret, ".png"));
        QVERIFY(!mine.has(other.wrong, ".png"));
        QVERIFY(other.links);
        QCOMPARE(s.window->last_notice(), QStringLiteral("「other」の 3 ページを後ろに足しました"));
        QCOMPARE(s.book().pages.size(), std::size_t{7});
        QCOMPARE(s.window->pages()->count(), 7);
        bool refers = false;  // (its page 2, now page 7, with the picture this book has now)
        for (const core::Layer& layer : s.book().page(6).layers) refers = refers || (layer.asset && *layer.asset == other.picture);
        QVERIFY(refers);
        s.on_page(5);
        QVERIFY(drawn_as_book(s));
        QVERIFY(s.trigger("act_undo"));
        QCOMPARE(s.book().pages.size(), std::size_t{4});
        // all its pages (an empty answer); the folder written as Python's Path writes it (no closing slash)
        pages = QString();
        folder += QStringLiteral("/");
        n = s.ops.size();
        QVERIFY(s.trigger("act_merge_book"));
        QCOMPARE(s.since(n), core::dump_python(Json::array({Json{{"op", "import_pages"}, {"from", other.text()}}})));
        QCOMPARE(s.book().pages.size(), std::size_t{7});
        // cancelled at the pages: nothing (nor copied)
        Studio t;
        const OtherBook third(t.tmp.path() + QStringLiteral("/third.genko"), 2);
        t.answers.responder->existing_dir = [&](const QString&, const QString&) { return QString::fromStdString(third.text()); };
        n = t.ops.size();
        QVERIFY(t.trigger("act_merge_book"));
        QCOMPARE(t.ops.size(), n);
        QVERIFY(!storage::AssetStore(t.path).has(third.picture, ".png"));
    }

    // Which pages, read as Python's main._page_list reads them (、 for a comma, 〜 and ～ for a dash, ranges either way
    // round, each page once, Python's int(): spaces, a sign, underscores, any decimal digits): the answers and the pages
    // sent, from the Python reference; what it cannot read, said.
    void whichPagesAsPythonReadsThem() {
        Studio s(2);
        const OtherBook other(s.tmp.path() + QStringLiteral("/seven.genko"), 7);
        // (main._page_list(text, 7) in the Python reference; null: every page)
        const Json table = Json::parse(R"([["0_7", [7]], ["1__0", "error"], ["_1", "error"], ["1_", "error"], ["\t1\n", [1]],
            ["99999999999999999999", "error"], ["٣-١", [1, 2, 3]], ["٣", [3]], ["", null], ["　", null], ["1-4, 7", [1, 2, 3, 4, 7]],
            ["4-1", [1, 2, 3, 4]], ["3、1〜2", [3, 1, 2]], ["1～2", [1, 2]], ["2,2,1", [2, 1]], ["0", "error"], ["8", "error"],
            ["1-", "error"], ["-1", "error"], ["a", "error"], [",,", "error"], ["１", [1]], [" 1 - 2 ", [1, 2]], ["+1", [1]],
            ["1_0", "error"], ["7", [7]], ["1,,3", [1, 3]], ["1-2-3", "error"], ["2-2", [2]], ["1.0", "error"], ["１－３", "error"],
            ["3, 1-2", [3, 1, 2]], ["1e1", "error"], ["  ,1", [1]], ["1 2", "error"],
            ["1-99999999999999999999", "error"], ["1-100000000000", "error"]])");
        // (the last two: Python's range() of them fails with OverflowError, or holds the whole range before it finds it
        // too long; this build says it cannot read them, as for any page outside the book)
        s.answers.responder->existing_dir = [&](const QString&, const QString&) { return QString::fromStdString(other.text()); };
        QString text;
        s.answers.responder->get_text = [&](const QString&, const QString&, const QString&) { return std::optional<QString>(text); };
        for (const Json& row : table) {
            text = QString::fromStdString(row[0].get<std::string>());
            const std::size_t n = s.ops.size();
            const std::size_t before = s.book().pages.size();
            QVERIFY(s.trigger("act_merge_book"));
            if (row[1] == Json("error")) {
                QVERIFY2(s.ops.size() == n, row.dump().c_str());
                QCOMPARE(s.window->last_error(), QStringLiteral("ページの書き方が読めません（例: 1-4, 7）"));
                continue;
            }
            Json op{{"op", "import_pages"}, {"from", other.text()}};
            if (!row[1].is_null()) op["pages"] = row[1];
            QCOMPARE(s.since(n), core::dump_python(Json::array({op})));
            QCOMPARE(s.book().pages.size(), before + (row[1].is_null() ? 7 : row[1].size()));
            QVERIFY(s.trigger("act_undo"));
            QCOMPARE(s.book().pages.size(), before);
        }
    }

    // What cannot be taken in, said in the person's words, and nothing changed: a book not saved yet, a folder that is
    // not a book, this book itself (also through a link), a book that cannot be read, an asset file past what one may
    // hold, a book whose asset folder cannot be written.
    void whatCannotBeTakenInIsSaid() {
        {
            Studio s(2, {}, false);
            bool asked = false;
            s.answers.responder->existing_dir = [&](const QString&, const QString&) {
                asked = true;
                return QString();
            };
            QVERIFY(s.trigger("act_merge_book"));
            QVERIFY(!asked);
            QCOMPARE(s.window->last_notice(), QStringLiteral("取り込む前に、この原稿を保存します（ファイル → 別の場所に保存）"));
        }
        Studio s(2);
        const auto before = s.session->snapshot();
        QString folder;
        bool texts = false;
        s.answers.responder->existing_dir = [&](const QString&, const QString&) { return folder; };
        s.answers.responder->get_text = [&](const QString&, const QString&, const QString&) {
            texts = true;
            return std::optional<QString>(QString());
        };
        QVERIFY(s.trigger("act_merge_book"));  // (no folder chosen)
        QVERIFY(!texts && s.answers.asked.isEmpty());
        folder = s.tmp.path() + QStringLiteral("/empty");
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(s.trigger("act_merge_book"));
        QCOMPARE(s.answers.asked, QStringList{QStringLiteral("Genko: Genko の原稿ではありません（.genko のフォルダを選びます）")});
        folder = QString::fromStdString(core::path_to_utf8(s.path));
        QVERIFY(s.trigger("act_merge_book"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("同じ原稿は取り込めません（ページの複製を使います）"));
        std::error_code ec;
        fs::create_directory_symlink(s.path, gui_test::path_of(s.tmp.path() + QStringLiteral("/same.genko")), ec);
        if (!ec) {
            folder = s.tmp.path() + QStringLiteral("/same.genko");
            s.window->flash(QString());
            QVERIFY(s.trigger("act_merge_book"));
            QCOMPARE(s.window->last_notice(), QStringLiteral("同じ原稿は取り込めません（ページの複製を使います）"));
        }
        folder = s.tmp.path() + QStringLiteral("/broken.genko");
        write_bytes(gui_test::path_of(folder + QStringLiteral("/project.json")), "{not json");
        QVERIFY(s.trigger("act_merge_book"));
        QVERIFY2(s.window->last_error().startsWith(QStringLiteral("その原稿を読めませんでした:\n")), qPrintable(s.window->last_error()));
        QVERIFY(!texts);
        QCOMPARE(s.session->snapshot(), before);
        // an asset file past the most one may hold (a sparse file): said, no page taken in
        const OtherBook big(s.tmp.path() + QStringLiteral("/big.genko"), 2);
        const std::string huge = "sha256:" + std::string(64, '0');
        const fs::path huge_file = storage::AssetStore(big.path).path(huge, ".png");
        write_bytes(huge_file, "");
        fs::resize_file(huge_file, storage::kAssetMaxBytes + 1);
        folder = QString::fromStdString(big.text());
        QVERIFY(s.trigger("act_merge_book"));
        QCOMPARE(s.window->last_error(), QStringLiteral("取り込む原稿の素材ファイル（%1）が大きすぎて取り込めません（1 ファイル 256 MiB まで）")
                                             .arg(QString::fromStdString(core::path_to_utf8(huge_file))));
        QCOMPARE(s.session->snapshot(), before);
        fs::remove(huge_file);
        // this book's assets cannot be written: said, no page taken in
        const fs::path assets = s.path / "assets";
        fs::create_directories(assets);
        fs::permissions(assets, fs::perms::owner_read | fs::perms::owner_exec, ec);
        QVERIFY(s.trigger("act_merge_book"));
        fs::permissions(assets, fs::perms::owner_all, ec);
        QCOMPARE(s.window->last_error(),
                 QStringLiteral("取り込む原稿の素材ファイルを、この原稿に写せませんでした: この場所には書き込めません（読み取り専用か、書き込みが許されていません）"));
        QCOMPARE(s.session->snapshot(), before);
        // and then it can
        QVERIFY(s.trigger("act_merge_book"));
        QCOMPARE(s.book().pages.size(), std::size_t{4});
    }

    // A book open read-only (it uses a feature this build does not know): another book's pages refused as any change
    // is, and nothing of the other book copied into its folder. An agent's book (strict gates): a spread of two pages
    // that are one leaf, offered by the page list's menu as Python's offers it, refused in the person's words.
    void refusalsAreSaidInThePersonsWords() {
        {
            Studio s(2, {}, true, [](const fs::path& book) {
                QFile file(gui_test::qpath(book / "project.json"));
                QVERIFY(file.open(QIODevice::ReadOnly));
                Json project = Json::parse(file.readAll().toStdString());
                file.close();
                project["features"] = Json::array({"genko.future-feature"});
                QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
                file.write(QByteArray::fromStdString(project.dump(2)));
            });
            QVERIFY(!s.session->read_only_reason().empty());
            const OtherBook other(s.tmp.path() + QStringLiteral("/other.genko"), 2);
            s.answers.responder->existing_dir = [&](const QString&, const QString&) { return QString::fromStdString(other.text()); };
            s.answers.responder->get_text = [](const QString&, const QString&, const QString&) { return std::optional<QString>(QString()); };
            QVERIFY(s.trigger("act_merge_book"));
            QCOMPARE(s.window->last_error(), QStringLiteral("この原稿は読み取り専用で開いています（変更できません）"));
            QCOMPARE(s.book().pages.size(), std::size_t{2});
            QVERIFY(!storage::AssetStore(s.path).has(other.picture, ".png"));
        }
        Studio s(4, [](core::Document& doc) { doc.strict_gates = true; });
        const std::size_t n = s.ops.size();
        page_menu(s, 1, QStringLiteral("1 ページと見開きにする"));
        QCOMPARE(s.ops.size(), n);
        QVERIFY2(s.window->last_error().startsWith(QStringLiteral("1 ページと 2 ページは 1 枚の紙の表と裏なので、見開きになりません（set_spread の書き方: {")),
                 qPrintable(s.window->last_error()));
        QVERIFY(!s.book().page(1).spread_with);
    }

    // PSD をレイヤーのまま読み込む…: a PSD chosen (Python's caption and filter), its layers put just above the layer chosen
    // in the layer panel, on the page in front (Python's op; the layers as fileops.import_psd makes them from the same
    // file: 線画 then 色, from the bottom); the canvas and the layer panel after it; a file that is not a PSD, said.
    void aPsdComesInAsLayers() {
        Studio s(2);
        const fs::path file = gui_test::path_of(s.tmp.path() + QStringLiteral("/small.psd"));
        write_bytes(file, psd_bytes(40, 30, {{QStringLiteral("線画"), "line", 2, 3, 10, 8, {10, 20, 30, 255}},
                                             {QStringLiteral("色"), "colour", 5, 6, 20, 12, {200, 100, 50, 128}}}));
        const std::string chosen = s.book().page(0).layers.at(1).id;
        app::LayerPanel* layers = s.window->layer_panel();
        const auto& ids = layers->ids();
        const auto row = std::find(ids.begin(), ids.end(), chosen) - ids.begin();
        QVERIFY(row < static_cast<std::ptrdiff_t>(ids.size()));
        layers->list()->setCurrentRow(static_cast<int>(row));
        QCOMPARE(layers->selected_ids(), std::vector<std::string>{chosen});
        QStringList asked;
        QString answer;
        s.answers.responder->open_path = [&](const QString& caption, const QString& filter) {
            asked << caption << filter;
            return answer;
        };
        // cancelled
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_import_psd"));
        QCOMPARE(asked, QStringList({QStringLiteral("PSD を読み込む"), QStringLiteral("PSD (*.psd *.psb)")}));
        QCOMPARE(s.ops.size(), n);
        answer = QString::fromStdString(core::path_to_utf8(file));
        const QImage before = shown(s);
        QVERIFY(s.trigger("act_import_psd"));
        QCOMPARE(s.since(n), core::dump_python(Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"path", core::path_to_utf8(file)},
                                                                 {"fit", "bleed"}, {"after", chosen}}})));
        const auto& now = s.book().page(0).layers;
        QCOMPARE(now.size(), std::size_t{6});
        QCOMPARE(now.at(1).id, chosen);
        QCOMPARE(QString::fromStdString(now.at(2).title), QStringLiteral("線画"));
        QCOMPARE(QString::fromStdString(now.at(3).title), QStringLiteral("色"));
        QCOMPARE(now.at(2).kind, core::LayerKind::Raster);
        QCOMPARE(s.window->last_notice(), QStringLiteral("「small.psd」をレイヤーのまま読み込みました"));
        QVERIFY(std::find(layers->ids().begin(), layers->ids().end(), now.at(3).id) != layers->ids().end());
        QVERIFY(drawn_as_book(s));
        QVERIFY(shown(s) != before);
        // not a PSD: said, nothing changed
        const fs::path junk = gui_test::path_of(s.tmp.path() + QStringLiteral("/junk.psd"));
        write_bytes(junk, "not a psd at all");
        answer = QString::fromStdString(core::path_to_utf8(junk));
        n = s.ops.size();
        QVERIFY(s.trigger("act_import_psd"));
        QCOMPARE(s.ops.size(), n);
        // (with the op's usage after it, as Python's wording.error writes a refusal that carries it)
        QVERIFY2(s.window->last_error().startsWith(QStringLiteral("PSD を読み込めません（PSD のファイルではありません）（import_psd の書き方: {")),
                 qPrintable(s.window->last_error()));
    }

    // A question or a dialog open while the book changes (another window, an agent): nothing is done when it is
    // answered (the page or the book it was about may be gone), and that is said.
    void aBookThatChangedWhileAskingIsLeftAlone_data() {
        QTest::addColumn<QString>("command");
        for (const char* name : {"act_assignee", "act_nombre", "act_paper", "act_add_cover", "act_spread", "act_merge_book", "act_import_psd"})
            QTest::newRow(name) << QString::fromLatin1(name);
        QTest::newRow("act_assignee-page") << QStringLiteral("act_assignee");
    }

    void aBookThatChangedWhileAskingIsLeftAlone() {
        QFETCH(QString, command);
        const bool page_only = QString::fromLatin1(QTest::currentDataTag()).endsWith(QStringLiteral("-page"));
        Studio s;
        const OtherBook other(s.tmp.path() + QStringLiteral("/other.genko"), 2);
        const fs::path psd = gui_test::path_of(s.tmp.path() + QStringLiteral("/small.psd"));
        write_bytes(psd, psd_bytes(8, 8, {{QStringLiteral("線画"), "line", 0, 0, 8, 8, {0, 0, 0, 255}}}));
        s.on_page(3);
        // (the change made while asking: a note on page 1, as another window or an agent would; or another page in front)
        const auto change = [&] {
            if (page_only) {
                s.on_page(1);
            } else {
                s.window->apply_ops(Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "その間に"}}}));
            }
        };
        bool asked = false;
        s.answers.responder->exec = [&](QDialog*) {
            asked = true;
            change();
            return int(QDialog::Accepted);
        };
        s.answers.responder->get_text = [&](const QString&, const QString&, const QString&) {
            asked = true;
            change();
            return std::optional<QString>(QStringLiteral("田中"));
        };
        s.answers.responder->question = [&](const QString&, const QString&) {
            asked = true;
            change();
            return true;
        };
        s.answers.responder->existing_dir = [&](const QString&, const QString&) { return QString::fromStdString(other.text()); };
        s.answers.responder->open_path = [&](const QString&, const QString&) {
            asked = true;
            change();
            return QString::fromStdString(core::path_to_utf8(psd));
        };
        const std::size_t pages = s.book().pages.size();
        QVERIFY(s.trigger(command.toLatin1().constData()));
        QVERIFY(asked);
        // only the test's own change, if any
        for (const Json& op : s.ops) QCOMPARE(op.value("op", std::string()), std::string("set_note"));
        QCOMPARE(s.book().pages.size(), pages);
        QCOMPARE(s.window->last_error(), QStringLiteral("確認中に対象の原稿・ページが変更されたため、操作を中止しました。やり直してください。"));
        QVERIFY(!storage::AssetStore(s.path).has(other.picture, ".png"));
    }
};

QTEST_MAIN(TestGuiBook)
#include "test_gui_book.moc"
