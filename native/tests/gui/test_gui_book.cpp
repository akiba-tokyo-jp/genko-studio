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

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTimer>
#include <QtTest>

#include <filesystem>
#include <fstream>
#include <functional>
#include <tuple>

#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/colours.hpp"
#include "app/dialogs.hpp"
#include "app/layer_panel.hpp"
#include "app/main_window.hpp"
#include "app/mode_dialogs.hpp"
#include "app/pages_panel.hpp"
#include "app/tiles.hpp"
#include "app/tool_settings.hpp"
#include "core/base64.hpp"
#include "core/covers.hpp"
#include "core/ink.hpp"
#include "core/geometry.hpp"
#include "core/ids.hpp"
#include "core/pyconv.hpp"
#include "gui_support.hpp"
#include "render/image.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "render/ink.hpp"
#include "service/wording.hpp"
#include "testsupport.hpp"

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
// (meanwhile: done while the menu is open, before the choice is made)
QStringList page_menu(Studio& s, int row, const QString& pick = {}, const std::function<void()>& meanwhile = {}) {
    QStringList texts;
    app::PageList* list = s.window->pages();
    QTimer::singleShot(0, list, [&texts, pick, meanwhile] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr) return;
        texts = menu_texts(menu);
        QAction* chosen = nullptr;
        for (QAction* a : menu->actions()) {
            if (!pick.isEmpty() && a->text() == pick) chosen = a;
        }
        if (meanwhile) meanwhile();
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
            // (as Python's window has it once preferences.retip has run at the start: "<title>（<keys>）\n<tip>", no keys
            // here; without a tip, the title)
            QCOMPARE(a->toolTip(), *e.tip != '\0' ? QString::fromUtf8(e.text) + QStringLiteral("\n") + QString::fromUtf8(e.tip) : QString::fromUtf8(e.text));
            QVERIFY2(!a->isCheckable(), e.attribute);
            QVERIFY2(a->isEnabled(), e.attribute);  // (Python's are always on: what cannot be done is said when chosen)
            QCOMPARE(menus_of(s.window.get(), a), QStringList{QString::fromUtf8(e.menu)});
        }
        // ページ: Python's order (絵柄を選ぶ and チャットでの承認 of M5④ in their places)
        QMenu* pages = top_menu(s.window.get(), QStringLiteral("ページ"));
        QVERIFY(pages != nullptr);
        // (色モード: the page's colour mode and its dialog, the C++ build's own, after Python's — a submenu since
        // color-model@1, as the design's ページ → 色モード has it)
        QCOMPARE(menu_texts(pages).mid(0, 19),
                 QStringList({QStringLiteral("ページを追加（この後ろに）"), QStringLiteral("このページを複製"), QStringLiteral("このページを消す…"), QStringLiteral("|"),
                              QStringLiteral("このページを前へ"), QStringLiteral("このページを後ろへ"), QStringLiteral("次のページと見開きにする／解除"), QStringLiteral("|"),
                              QStringLiteral("原稿用紙の設定…"), QStringLiteral("絵柄を選ぶ…"), QStringLiteral("チャットでの承認…"), QStringLiteral("ノンブルの設定…"), QStringLiteral("このページのノンブルを隠す／出す"),
                              QStringLiteral("表紙・カバーを足す…"), QStringLiteral("このページの担当…"), QStringLiteral("アニメーション（タイムライン）"), QStringLiteral("色モード"),
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

    // The page list's menu while the book changes (another writer reorders or deletes pages while the menu or its
    // question is open; another book comes to the front): each choice goes to the page it was opened on, found by its
    // id, at its number then; the page gone, the pages of a spread no longer side by side, or another book in front:
    // nothing is done, and that is said.
    void pageListMenuFollowsItsPageWhileTheBookChanges() {
        Studio s(5);
        const auto ids = [&s] {
            std::vector<std::string> out;
            for (const auto& p : s.book().pages) out.push_back(p->id);
            return out;
        };
        const auto number_of = [&s](const std::string& id) {
            for (const auto& p : s.book().pages) {
                if (p->id == id) return static_cast<int>(core::py_int(p->index.json()));
            }
            return 0;
        };
        const QString changed = QStringLiteral("確認中に対象の原稿・ページが変更されたため、操作を中止しました。やり直してください。");
        const auto reorder = [&s](const std::vector<int>& order) {
            if (!s.window->apply_ops(Json::array({Json{{"op", "reorder"}, {"order", order}}}))) qFatal("the pages were not reordered meanwhile");
        };
        // このページを消す…: the pages reordered while asked — the page asked about is deleted, now page 1
        const std::string second = ids()[1];
        s.answers.responder->question = [&](const QString&, const QString&) {
            reorder({2, 1, 3, 4, 5});
            return true;
        };
        std::size_t n = s.ops.size();
        page_menu(s, 1, QStringLiteral("このページを消す…"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "reorder", "order": [2, 1, 3, 4, 5]}, {"op": "delete_page", "page": 1}])"));
        QCOMPARE(number_of(second), 0);
        QCOMPARE(s.book().pages.size(), std::size_t{4});
        // …and the page deleted while asked: nothing more
        s.window->flash(QStringLiteral("(before)"), 1, true);
        s.answers.responder->question = [&](const QString&, const QString&) {
            if (!s.window->apply_ops(Json::array({Json{{"op", "delete_page"}, {"page", 2}}}))) qFatal("the page was not deleted meanwhile");
            return true;
        };
        n = s.ops.size();
        page_menu(s, 1, QStringLiteral("このページを消す…"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "delete_page", "page": 2}])"));
        QCOMPARE(s.window->last_error(), changed);
        QCOMPARE(s.book().pages.size(), std::size_t{3});
        s.answers.responder->question = [](const QString&, const QString&) { return true; };
        // the choices without a question: the pages reordered while the menu is open
        const std::string first = ids()[0];
        n = s.ops.size();
        page_menu(s, 0, QStringLiteral("このページを複製"), [&] { reorder({2, 1, 3}); });
        QCOMPARE(s.since(n), std::string(R"([{"op": "reorder", "order": [2, 1, 3]}, {"op": "duplicate_page", "page": 2, "next_to": true}])"));
        QCOMPARE(number_of(first), 2);
        QCOMPARE(s.book().pages.size(), std::size_t{4});
        n = s.ops.size();
        page_menu(s, 1, QStringLiteral("後ろへ移す"), [&] { reorder({1, 3, 2, 4}); });  // (the page that was second is third now)
        QCOMPARE(s.since(n), std::string(R"([{"op": "reorder", "order": [1, 3, 2, 4]}, {"op": "reorder", "order": [1, 2, 4, 3]}])"));
        QCOMPARE(number_of(first), 4);
        n = s.ops.size();
        const std::string third = ids()[2];
        page_menu(s, 2, QStringLiteral("このページのノンブルを隠す"), [&] { reorder({3, 1, 2, 4}); });
        QCOMPARE(s.since(n), std::string(R"([{"op": "reorder", "order": [3, 1, 2, 4]}, {"op": "set_nombre", "page": 1, "numero": false}])"));
        QVERIFY(!s.book().page(0).numero);
        QCOMPARE(s.book().page(0).id, third);
        n = s.ops.size();
        page_menu(s, 3, QStringLiteral("この後ろにページを追加"), [&] { reorder({4, 1, 2, 3}); });
        QCOMPARE(s.since(n), std::string(R"([{"op": "reorder", "order": [4, 1, 2, 3]}, {"op": "add_page", "count": 1, "after": 1}])"));
        // a spread with the page after: both moved, still side by side — made; no longer side by side — said, nothing made
        n = s.ops.size();
        page_menu(s, 1, QStringLiteral("3 ページと見開きにする"), [&] { reorder({1, 4, 2, 3, 5}); });
        QCOMPARE(s.since(n), std::string(R"([{"op": "reorder", "order": [1, 4, 2, 3, 5]}, {"op": "set_spread", "page": 3, "with": 4}, )"
                                         R"({"op": "set_spread", "page": 4, "with": 3}])"));
        n = s.ops.size();
        page_menu(s, 3, QStringLiteral("見開きを解除"), [&] { reorder({4, 3, 1, 2, 5}); });
        QCOMPARE(s.since(n), std::string(R"([{"op": "reorder", "order": [4, 3, 1, 2, 5]}, {"op": "set_spread", "page": 1, "with": null}, )"
                                         R"({"op": "set_spread", "page": 2, "with": null}])"));
        n = s.ops.size();
        s.window->flash(QStringLiteral("(before)"), 1, true);
        page_menu(s, 0, QStringLiteral("2 ページと見開きにする"), [&] { reorder({1, 3, 2, 4, 5}); });
        QCOMPARE(s.since(n), std::string(R"([{"op": "reorder", "order": [1, 3, 2, 4, 5]}])"));
        QCOMPARE(s.window->last_error(), changed);
        // another book in front while asked: nothing done in either book
        const OtherBook other(s.tmp.path() + QStringLiteral("/other.genko"), 2);
        const std::size_t pages = s.book().pages.size();
        s.answers.responder->question = [&](const QString&, const QString&) {
            s.window->add_document(app::Session::open(other.path, gui_test::quick(gui_test::path_of(s.tmp.path() + QStringLiteral("/recovery2")))));
            return true;
        };
        s.window->flash(QStringLiteral("(before)"), 1, true);
        n = s.ops.size();
        page_menu(s, 0, QStringLiteral("このページを消す…"));
        QCOMPARE(s.window->current_document(), 1);
        QCOMPARE(s.window->book().pages.size(), std::size_t{2});
        QCOMPARE(s.ops.size(), n);
        QCOMPARE(s.session->document().pages.size(), pages);
        QCOMPARE(s.window->last_error(), changed);
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
    // ページの色モード… (color-mode@1): the page in front or every page, its mode, depth, profile (chosen as a file: sent
    // as its bytes; the book's: by its ref), intent and greys; sent as set_color_mode, shown on the page and the status.
    void colourModeOfPages() {
        Studio s;
        const std::string icc = genko::test::read_bytes(genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc")));
        const QString icc_file = s.tmp.path() + QStringLiteral("/printer.icc");
        genko::test::write_bytes(icc_file, icc);
        s.answers.responder->open_path = [&](const QString&, const QString&) { return icc_file; };
        QStringList seen;
        std::function<void(QDialog*)> answer;
        int result = QDialog::Accepted;
        s.answers.responder->exec = [&](QDialog* d) {
            seen.clear();
            for (const char* name : {"color_mode_scope", "color_mode_mode", "color_mode_depth", "color_mode_profile", "color_mode_intent", "color_mode_gray"}) {
                auto* box = d->findChild<QComboBox*>(QString::fromLatin1(name));
                seen << (box != nullptr ? label_of(box) + QStringLiteral(": ") + described(box) : QString::fromLatin1(name) + QStringLiteral(" missing"));
            }
            if (answer) answer(d);
            return result;
        };
        s.on_page(2);
        // やめる: nothing
        result = QDialog::Rejected;
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_color_mode"));
        QCOMPARE(seen.at(0), QStringLiteral("対象: このページ（2 ページ）=page|すべてのページと、これから足すページ=all @page"));
        QCOMPARE(seen.at(1), QStringLiteral("色モード: RGB=rgb|グレースケール=gray|CMYK（印刷のインキ）=cmyk|Lab=lab @rgb"));
        QCOMPARE(s.ops.size(), n);
        // CMYK for this page, the profile chosen as a file: OK only once there is one
        result = QDialog::Accepted;
        answer = [](QDialog* d) {
            auto* mode = d->findChild<QComboBox*>(QStringLiteral("color_mode_mode"));
            mode->setCurrentIndex(mode->findData(QStringLiteral("cmyk")));
            auto* ok = d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
            QVERIFY(!ok->isEnabled());
            d->findChild<QPushButton*>(QStringLiteral("color_mode_pick"))->click();
            QVERIFY(ok->isEnabled());
            auto* gray = d->findChild<QComboBox*>(QStringLiteral("color_mode_gray"));
            gray->setCurrentIndex(gray->findData(QStringLiteral("k")));
        };
        QVERIFY(s.trigger("act_color_mode"));
        const Json sent = Json::parse(s.since(n));
        QCOMPARE(sent.size(), std::size_t{1});
        QCOMPARE(QString::fromStdString(sent[0]["op"].get<std::string>()), QStringLiteral("set_color_mode"));
        QCOMPARE(sent[0]["page"].get<int>(), 2);
        QCOMPARE(QString::fromStdString(sent[0]["mode"].get<std::string>()), QStringLiteral("cmyk"));
        QCOMPARE(QString::fromStdString(sent[0]["gray"].get<std::string>()), QStringLiteral("k"));
        QCOMPARE(core::a2b_base64(sent[0]["icc"].get<std::string>()), icc);
        QVERIFY(s.book().page(1).color && s.book().page(1).color->space == core::InkSpace::Cmyk);
        QVERIFY(!s.book().page(0).color);
        QCOMPARE(s.window->last_notice(), QStringLiteral("色モードを変えました"));
        s.window->flash(QString(), 1);  // (the notice's time over: the page's status, with its mode)
        QTRY_VERIFY2(s.window->status_label()->text().contains(QStringLiteral("CMYK 8 bit")), qPrintable(s.window->status_label()->text()));
        // every page, Lab 16 bits: the book's profile is not needed
        answer = [](QDialog* d) {
            auto* scope = d->findChild<QComboBox*>(QStringLiteral("color_mode_scope"));
            scope->setCurrentIndex(scope->findData(QStringLiteral("all")));
            auto* mode = d->findChild<QComboBox*>(QStringLiteral("color_mode_mode"));
            mode->setCurrentIndex(mode->findData(QStringLiteral("lab")));
            auto* depth = d->findChild<QComboBox*>(QStringLiteral("color_mode_depth"));
            depth->setCurrentIndex(depth->findData(16));
        };
        n = s.ops.size();
        QVERIFY(s.trigger("act_color_mode"));
        QCOMPARE(seen.at(1), QStringLiteral("色モード: RGB=rgb|グレースケール=gray|CMYK（印刷のインキ）=cmyk|Lab=lab @cmyk"));  // (the page's mode now)
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_color_mode", "mode": "lab", "depth": 16, "intent": "relative"}])"));
        for (const auto& page : s.book().pages) QVERIFY(page->color && page->color->space == core::InkSpace::Lab && page->color->depth == 16);
        QVERIFY(s.book().color);
        QVERIFY(drawn_as_book(s));
        // back to RGB: 8 bit (the paint layers as PNG) unless 16 is chosen again
        int depth_before = 0, depth_after = 0;
        answer = [&](QDialog* d) {
            auto* mode = d->findChild<QComboBox*>(QStringLiteral("color_mode_mode"));
            auto* depth = d->findChild<QComboBox*>(QStringLiteral("color_mode_depth"));
            depth_before = depth->currentData().toInt();
            mode->setCurrentIndex(mode->findData(QStringLiteral("rgb")));
            depth_after = depth->currentData().toInt();
        };
        result = QDialog::Rejected;
        QVERIFY(s.trigger("act_color_mode"));
        QCOMPARE(depth_before, 16);
        QCOMPARE(depth_after, 8);
    }

    // ページ → 色モード (image.mode.*, color-model@1): its modes and depths checked as the page in front is; each sends
    // set_color_mode for that page — another mode at the page's depth, another depth with the rest of the mode kept (its
    // profile, intent), CMYK in the built-in coated-cmyk; nothing when the page is so already; the mode and its profile in
    // the status; undone as any op. 編集 → プロファイルの指定… / プロファイルに変換… beside.
    void colourModesInTheMenu() {
        Studio s;
        QMenu* pages = top_menu(s.window.get(), QStringLiteral("ページ"));
        QVERIFY(pages != nullptr);
        QMenu* modes = nullptr;
        for (QAction* a : pages->actions()) {
            if (a->menu() != nullptr && a->menu()->title() == QStringLiteral("色モード")) modes = a->menu();
        }
        QVERIFY(modes != nullptr);
        // (PC1d: 2 階調…・インデックスカラー…・ダブルトーン…・マルチチャンネル after the four, カラーテーブル… before ページの色モード…)
        QCOMPARE(menu_texts(modes), QStringList({QStringLiteral("RGB"), QStringLiteral("グレースケール"), QStringLiteral("CMYK"), QStringLiteral("Lab"),
                                                 QStringLiteral("2 階調…"), QStringLiteral("インデックスカラー…"), QStringLiteral("ダブルトーン…"),
                                                 QStringLiteral("マルチチャンネル"), QStringLiteral("|"), QStringLiteral("8 bit/チャンネル"),
                                                 QStringLiteral("16 bit/チャンネル"), QStringLiteral("32 bit/チャンネル（浮動小数）"), QStringLiteral("|"),
                                                 QStringLiteral("カラーテーブル…"), QStringLiteral("ページの色モード…")}));
        for (const char* name : {"act_mode_rgb", "act_mode_gray", "act_mode_cmyk", "act_mode_lab", "act_mode_bitmap", "act_mode_indexed", "act_mode_duotone",
                                 "act_mode_multichannel", "act_depth_8", "act_depth_16", "act_depth_32", "act_color_table", "act_color_mode"}) {
            QCOMPARE(menus_of(s.window.get(), s.window->action(QString::fromLatin1(name))), QStringList{QStringLiteral("ページ/色モード")});
        }
        for (const char* name : {"act_assign_profile", "act_convert_profile"}) {
            QCOMPARE(menus_of(s.window.get(), s.window->action(QString::fromLatin1(name))), QStringList{QStringLiteral("編集")});
        }
        QCOMPARE(s.window->action(QStringLiteral("act_assign_profile"))->text(), QStringLiteral("プロファイルの指定…"));
        QCOMPARE(s.window->action(QStringLiteral("act_convert_profile"))->text(), QStringLiteral("プロファイルに変換…"));
        const auto checked = [&] {
            emit modes->aboutToShow();
            QStringList out;
            for (QAction* a : modes->actions()) {
                if (a->isChecked()) out << a->text();
            }
            return out.join(QStringLiteral("+"));
        };
        const auto status = [&] { s.window->flash(QString(), 1); };  // (the notice's time over: the page's status, with its mode)
        s.on_page(2);
        QCOMPARE(checked(), QStringLiteral("RGB+8 bit/チャンネル"));
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_mode_rgb"));  // (so already: nothing)
        QCOMPARE(s.ops.size(), n);
        QCOMPARE(checked(), QStringLiteral("RGB+8 bit/チャンネル"));
        // grey at the page's depth
        QVERIFY(s.trigger("act_mode_gray"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_color_mode", "page": 2, "mode": "gray", "depth": 8}])"));
        QVERIFY(s.book().page(1).color && s.book().page(1).color->space == core::InkSpace::Gray);
        QVERIFY(!s.book().page(0).color);
        QCOMPARE(checked(), QStringLiteral("グレースケール+8 bit/チャンネル"));
        QCOMPARE(s.window->last_notice(), QStringLiteral("色モードを変えました"));
        status();
        QTRY_VERIFY2(s.window->status_label()->text().contains(QStringLiteral("グレー 8 bit・sGray (sRGB tone curve, Photocraft)")), qPrintable(s.window->status_label()->text()));
        QVERIFY(drawn_as_book(s));
        // 16 bits: the mode as it is
        const std::string sgray = core::builtin_profile("sgray")->ref();
        n = s.ops.size();
        QVERIFY(s.trigger("act_depth_16"));
        QCOMPARE(s.since(n), R"([{"op": "set_color_mode", "page": 2, "mode": "gray", "depth": 16, "profile": ")" + sgray + R"(", "intent": "relative"}])");
        QCOMPARE(s.book().page(1).color->depth, 16);
        QCOMPARE(checked(), QStringLiteral("グレースケール+16 bit/チャンネル"));
        // CMYK: the built-in coated-cmyk, at 16 bits
        n = s.ops.size();
        QVERIFY(s.trigger("act_mode_cmyk"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_color_mode", "page": 2, "mode": "cmyk", "depth": 16, "profile": "coated-cmyk"}])"));
        QVERIFY(s.book().page(1).color->space == core::InkSpace::Cmyk);
        QCOMPARE(s.book().page(1).color->profile, core::builtin_profile("coated-cmyk")->ref());
        QCOMPARE(checked(), QStringLiteral("CMYK+16 bit/チャンネル"));
        status();
        QTRY_VERIFY2(s.window->status_label()->text().contains(QStringLiteral("CMYK 16 bit・Photocraft Coated CMYK")), qPrintable(s.window->status_label()->text()));
        // 32 bits (floats), then RGB at 32
        QVERIFY(s.trigger("act_depth_32"));
        QCOMPARE(s.book().page(1).color->depth, 32);
        QCOMPARE(checked(), QStringLiteral("CMYK+32 bit/チャンネル（浮動小数）"));
        QVERIFY(s.trigger("act_mode_rgb"));
        QVERIFY(!s.book().page(1).color && s.book().page(1).rgb && s.book().page(1).rgb->depth == 32);
        QCOMPARE(checked(), QStringLiteral("RGB+32 bit/チャンネル（浮動小数）"));
        status();
        QTRY_VERIFY2(s.window->status_label()->text().contains(QStringLiteral("RGB 32 bit・sRGB IEC61966-2.1")), qPrintable(s.window->status_label()->text()));
        QVERIFY(drawn_as_book(s));
        // Lab, then all undone: the page as it was
        QVERIFY(s.trigger("act_mode_lab"));
        QVERIFY(s.book().page(1).color->space == core::InkSpace::Lab && s.book().page(1).color->depth == 32);
        QCOMPARE(checked(), QStringLiteral("Lab+32 bit/チャンネル（浮動小数）"));
        for (int i = 0; i < 6; ++i) QVERIFY(s.trigger("act_undo"));
        QVERIFY(!s.book().page(1).color && !s.book().page(1).rgb);
        QCOMPARE(checked(), QStringLiteral("RGB+8 bit/チャンネル"));
        // another page: its own marks
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "lab"}, {"depth", 16}}})));
        s.on_page(3);
        QCOMPARE(checked(), QStringLiteral("Lab+16 bit/チャンネル"));
        s.on_page(1);
        QCOMPARE(checked(), QStringLiteral("RGB+8 bit/チャンネル"));
    }

    // ページ → 色モード with カラー設定 not at its defaults (a working RGB of Display P3, the perceptual intent): another depth
    // of an sRGB page is that depth alone (no profile, no intent: the page keeps its own); another mode converts with the
    // settings' intent but leaves the page its own default way (page_intent), so a CMYK page taken to 8-bit RGB in the
    // working sRGB is a page without a colour model again (Python's).
    // PC1d (SPEC.md COLOR-03「特別な色モード」): ページ → 色モード's 2 階調…・インデックスカラー…・ダブルトーン…・
    // マルチチャンネル and カラーテーブル…, offered as the page in front's mode allows; each dialog's op; the flatten asked
    // when two or more layers hold something (no: nothing sent), not asked for a page of one; the mode in the status;
    // undone as any op.
    void specialModesInTheMenu() {
        Studio s;
        render::Image under = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{128, 128, 128, 255});
        under.paste(render::Ink{30, 160, 90, 255}, render::Box{30, 0, 60, 40});
        render::Image over = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{0, 0, 0, 0});
        over.paste(render::Ink{200, 40, 40, 255}, render::Box{10, 10, 20, 20});
        Json paint = Json::array();
        for (const auto& [page, id, picture] : std::vector<std::tuple<int, std::string, render::Image>>{{2, "u", under}, {2, "o", over}, {3, "g", under}}) {
            paint.push_back(Json{{"op", "add_layer"}, {"page", page}, {"kind", "paint"}, {"id", id}});
            paint.push_back(Json{{"op", "set_layer"}, {"page", page}, {"id", id}, {"panel_each", false}, {"panel_clip", false}});
            paint.push_back(Json{{"op", "put_raster"}, {"page", page}, {"id", id}, {"png_base64", core::b64encode(render::write_png(picture))}});
        }
        QVERIFY(s.window->apply_ops(paint));
        QMenu* pages = top_menu(s.window.get(), QStringLiteral("ページ"));
        QMenu* modes = nullptr;
        for (QAction* a : pages->actions()) {
            if (a->menu() != nullptr && a->menu()->title() == QStringLiteral("色モード")) modes = a->menu();
        }
        QVERIFY(modes != nullptr);
        const auto offered = [&] {
            emit modes->aboutToShow();
            QStringList out;
            for (const char* name : {"act_mode_bitmap", "act_mode_indexed", "act_mode_duotone", "act_mode_multichannel", "act_color_table"}) {
                if (s.window->action(QString::fromLatin1(name))->isEnabled()) out << QString::fromLatin1(name).mid(4);
            }
            return out.join(QLatin1Char(' '));
        };
        const auto status = [&] { s.window->flash(QString(), 1); };
        s.on_page(2);
        QCOMPARE(offered(), QStringLiteral("mode_indexed mode_multichannel"));  // (an RGB page)
        // インデックスカラー…: 8 colours; the flatten asked (two layers of pixels) and declined: nothing
        QStringList dialogs;
        std::function<void(QDialog*)> answer;
        s.answers.responder->exec = [&](QDialog* d) {
            dialogs << d->objectName();
            if (answer) answer(d);
            return int(QDialog::Accepted);
        };
        answer = [](QDialog* d) { d->findChild<QSpinBox*>(QStringLiteral("indexed_colors"))->setValue(8); };
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_mode_indexed"));
        QCOMPARE(dialogs, QStringList{QStringLiteral("indexed_dialog")});
        QCOMPARE(s.ops.size(), n);
        QCOMPARE(s.answers.asked, QStringList{QStringLiteral("画像を統合: インデックスカラーにすると、このページの画素レイヤー（2 枚）を 1 枚に統合します。統合しますか？\n"
                                                             "非表示の作画レイヤーは破棄します（ネーム・下描き・コマ枠は保持）。"
                                                             "台詞・フキダシは要素のまま残ります。編集 → 取り消し で元に戻せます。")});
        // … and accepted: to_indexed for this page, flattened into one layer
        s.answers.responder->question = [&](const QString& title, const QString&) {
            s.answers.asked << title;
            return true;
        };
        QVERIFY(s.trigger("act_mode_indexed"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "to_indexed", "page": 2, "palette": "selective", "colors": 8, "forced": "blackWhite", )"
                                         R"("transparency": true, "dither": "diffusion", "amount": 75}])"));
        QVERIFY(s.book().page(1).rgb && s.book().page(1).rgb->table && !s.book().page(1).color);
        int pixels = 0;
        for (const core::Layer& layer : s.book().page(1).layers) pixels += layer.raster_png && !layer.raster_png->empty();
        QCOMPARE(pixels, 1);
        QCOMPARE(s.window->last_notice(), QStringLiteral("インデックスカラーにしました"));
        QCOMPARE(offered(), QStringLiteral("mode_indexed color_table"));
        status();
        QTRY_VERIFY2(s.window->status_label()->text().contains(QStringLiteral("インデックスカラー（")), qPrintable(s.window->status_label()->text()));
        QVERIFY(drawn_as_book(s));
        // the value under the pointer (genko inspect --pixel's): its index in the table; the colour window: the pen
        // colour's index
        auto* value = s.window->findChild<QLabel*>(QStringLiteral("pixelValue"));
        QVERIFY(value != nullptr);
        emit s.window->canvas()->pointerOver(100.0, 100.0);
        QTRY_VERIFY2(!value->isHidden() && value->text().startsWith(QStringLiteral("値 番号 ")), qPrintable(value->text()));
        auto* table_index = s.window->findChild<QLabel*>(QStringLiteral("colour_table_index"));
        QVERIFY(table_index != nullptr);
        QTRY_VERIFY2(!table_index->isHidden() && table_index->text().startsWith(QStringLiteral("カラーテーブル ")), qPrintable(table_index->text()));
        // カラーテーブル…: the grid holds the table; entry 0 set to a blue, the last one transparent
        const std::size_t colours = s.book().page(1).rgb->table->colors.size();
        answer = [&](QDialog* d) {
            auto* table = qobject_cast<app::ColorTableDialog*>(d);
            QVERIFY(table != nullptr);
            QCOMPARE(table->colors().size(), colours);
            QVERIFY(table->cells.at(0)->isEnabled() && !table->cells.at(colours)->isEnabled());
            table->set_entry(0, {0, 0, 255});
            table->transparent->setCurrentIndex(table->transparent->count() - 1);
        };
        dialogs.clear();
        n = s.ops.size();
        QVERIFY(s.trigger("act_color_table"));
        QCOMPARE(dialogs, QStringList{QStringLiteral("color_table_dialog")});
        const Json table_op = Json::parse(s.since(n));
        QCOMPARE(table_op.size(), std::size_t(1));
        QCOMPARE(table_op[0]["op"], Json("set_color_table"));
        QCOMPARE(table_op[0]["colors"][0], Json::array({0, 0, 255}));
        QCOMPARE(table_op[0]["transparent"], Json(static_cast<int>(colours) - 1));
        QCOMPARE(s.book().page(1).rgb->table->colors[0], (std::array<int, 3>{0, 0, 255}));
        QCOMPARE(s.window->last_notice(), QStringLiteral("カラーテーブルを変えました"));
        // undone twice: the page as it was (two layers, no table)
        QVERIFY(s.trigger("act_undo"));
        QVERIFY(s.trigger("act_undo"));
        QVERIFY(!s.book().page(1).rgb);
        QCOMPARE(offered(), QStringLiteral("mode_indexed mode_multichannel"));
        // マルチチャンネル: asked and done — the page's plates
        s.answers.asked.clear();
        n = s.ops.size();
        QVERIFY(s.trigger("act_mode_multichannel"));
        QCOMPARE(s.answers.asked.size(), 1);
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_color_mode", "page": 2, "mode": "multichannel"}])"));
        QVERIFY(s.book().page(1).color && s.book().page(1).color->space == core::InkSpace::Multi);
        QCOMPARE(offered(), QStringLiteral("mode_multichannel"));  // (no indexed from multichannel: back to RGB first)
        status();
        QTRY_VERIFY2(s.window->status_label()->text().contains(QStringLiteral("マルチチャンネル（3 版）")), qPrintable(s.window->status_label()->text()));
        QVERIFY(drawn_as_book(s));
        emit s.window->canvas()->pointerOver(100.0, 100.0);  // (the plates' values)
        QTRY_VERIFY2(value->text().startsWith(QStringLiteral("値 Cyan ")) && value->text().contains(QStringLiteral(" Yellow ")), qPrintable(value->text()));
        QTRY_VERIFY(table_index->isHidden());
        // page 3 (one layer of pixels) in grey: 2 階調… and ダブルトーン… offered; 2 階調 with no question (nothing to merge)
        s.on_page(3);
        QVERIFY(s.trigger("act_mode_gray"));
        QCOMPARE(offered(), QStringLiteral("mode_bitmap mode_indexed mode_duotone mode_multichannel"));
        s.answers.asked.clear();
        answer = [](QDialog* d) {
            auto* method = d->findChild<QComboBox*>(QStringLiteral("bitmap_method"));
            method->setCurrentIndex(method->findData(QStringLiteral("threshold")));
            QVERIFY(!d->findChild<QDoubleSpinBox*>(QStringLiteral("bitmap_frequency"))->isEnabled());
        };
        dialogs.clear();
        n = s.ops.size();
        QVERIFY(s.trigger("act_mode_bitmap"));
        QCOMPARE(dialogs, QStringList{QStringLiteral("bitmap_dialog")});
        QVERIFY(s.answers.asked.isEmpty());
        QCOMPARE(s.since(n), std::string(R"([{"op": "to_bitmap", "page": 3, "method": "threshold", "frequency": 53.0, "angle": 45.0, "shape": "round"}])"));
        QVERIFY(s.book().page(2).color && s.book().page(2).color->bitmap);
        QCOMPARE(s.window->last_notice(), QStringLiteral("2 階調にしました"));
        QCOMPARE(offered(), QStringLiteral(""));  // (back by グレースケール first)
        QVERIFY(drawn_as_book(s));
        QVERIFY(s.trigger("act_undo"));
        // ダブルトーン…: a tritone, the second ink's curve written by hand; a curve that cannot be read keeps OK off
        answer = [](QDialog* d) {
            auto* type = d->findChild<QComboBox*>(QStringLiteral("duotone_type"));
            type->setCurrentIndex(type->findData(QStringLiteral("tritone")));
            auto* curve = d->findChild<QLineEdit*>(QStringLiteral("duotone_curve_2"));
            auto* ok = d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
            curve->setText(QStringLiteral("0:0, 50"));
            QVERIFY(!ok->isEnabled());
            curve->setText(QStringLiteral("0:0, 50:30, 100:80"));
            QVERIFY(ok->isEnabled());
        };
        dialogs.clear();
        n = s.ops.size();
        QVERIFY(s.trigger("act_mode_duotone"));
        QCOMPARE(dialogs, QStringList{QStringLiteral("duotone_dialog")});
        const Json duo = Json::parse(s.since(n));
        QCOMPARE(duo.size(), std::size_t(1));
        QCOMPARE(duo[0]["op"], Json("set_duotone"));
        QCOMPARE(duo[0]["type"], Json("tritone"));
        QCOMPARE(duo[0]["inks"].size(), std::size_t(3));
        QCOMPARE(duo[0]["inks"][1]["name"], Json("Warm Brown"));
        QCOMPARE(duo[0]["inks"][1]["curve"], Json::parse("[[0.0, 0.0], [50.0, 30.0], [100.0, 80.0]]"));
        QCOMPARE(s.book().page(2).color->duotone.size(), std::size_t(3));
        QCOMPARE(s.window->last_notice(), QStringLiteral("ダブルトーンにしました"));
        status();
        QTRY_VERIFY2(s.window->status_label()->text().contains(QStringLiteral("ダブルトーン")), qPrintable(s.window->status_label()->text()));
        QVERIFY(drawn_as_book(s));
        // back to grey: the inks off
        QVERIFY(s.trigger("act_mode_gray"));
        QVERIFY(s.book().page(2).color && s.book().page(2).color->duotone.empty());
    }

    // Review C: a conversion that flattens asks whenever the flatten would discard a hidden layer that holds something —
    // in 画像をフラット化's words, with their count —, even with one layer shown; for every page too (the hidden layers of
    // all of them); declined: nothing. Accepted: the hidden layer gone, said in the result.
    void flattenAsksOfHiddenLayers() {
        Studio s;
        render::Image dot = render::Image::create("RGBA", render::Size{60, 40}, render::Ink{0, 0, 0, 0});
        dot.paste(render::Ink{200, 40, 40, 255}, render::Box{10, 10, 20, 20});
        Json paint = Json::array();
        for (const auto& [page, id] : std::vector<std::pair<int, std::string>>{{2, "shown"}, {2, "hidden"}, {3, "away"}}) {
            paint.push_back(Json{{"op", "add_layer"}, {"page", page}, {"kind", "paint"}, {"id", id}});
            paint.push_back(Json{{"op", "set_layer"}, {"page", page}, {"id", id}, {"panel_each", false}, {"panel_clip", false}});
            paint.push_back(Json{{"op", "put_raster"}, {"page", page}, {"id", id}, {"png_base64", core::b64encode(render::write_png(dot))}});
        }
        paint.push_back(Json{{"op", "set_layer"}, {"page", 2}, {"id", "hidden"}, {"visible", false}});
        paint.push_back(Json{{"op", "set_layer"}, {"page", 3}, {"id", "away"}, {"visible", false}});
        QVERIFY(s.window->apply_ops(paint));
        s.on_page(2);
        bool all = false;
        s.answers.responder->exec = [&](QDialog* d) {
            if (all) {
                auto* scope = d->findChild<QComboBox*>(QStringLiteral("indexed_scope"));
                scope->setCurrentIndex(scope->findData(QStringLiteral("all")));
            }
            return int(QDialog::Accepted);
        };
        // the page in front: one layer shown, one hidden — asked, declined: nothing
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_mode_indexed"));
        QCOMPARE(s.ops.size(), n);
        QCOMPARE(s.answers.asked, QStringList{QStringLiteral("画像を統合: インデックスカラーにすると、このページの画素レイヤーを 1 枚に統合します。統合しますか？\n"
                                                             "非表示の作画レイヤー（1 枚）は破棄します（ネーム・下描き・コマ枠は保持）。"
                                                             "台詞・フキダシは要素のまま残ります。編集 → 取り消し で元に戻せます。")});
        // マルチチャンネル: the same
        s.answers.asked.clear();
        QVERIFY(s.trigger("act_mode_multichannel"));
        QCOMPARE(s.ops.size(), n);
        QCOMPARE(s.answers.asked.size(), 1);
        QVERIFY2(s.answers.asked[0].startsWith(QStringLiteral("画像を統合: マルチチャンネルにすると、")) &&
                     s.answers.asked[0].contains(QStringLiteral("非表示の作画レイヤー（1 枚）は破棄します（ネーム・下描き・コマ枠は保持）。")),
                 qPrintable(s.answers.asked[0]));
        // every page: the hidden layers of all the pages
        all = true;
        s.answers.asked.clear();
        QVERIFY(s.trigger("act_mode_indexed"));
        QCOMPARE(s.ops.size(), n);
        QCOMPARE(s.answers.asked, QStringList{QStringLiteral("画像を統合: インデックスカラーにすると、すべてのページで、ページごとに画素レイヤーを 1 枚に統合します。統合しますか？\n"
                                                             "非表示の作画レイヤー（2 枚）は破棄します（ネーム・下描き・コマ枠は保持）。"
                                                             "台詞・フキダシは要素のまま残ります。編集 → 取り消し で元に戻せます。")});
        // accepted (the page in front): the hidden layer discarded, said in the result
        all = false;
        s.answers.responder->question = [&](const QString& title, const QString&) {
            s.answers.asked << title;
            return true;
        };
        QVERIFY(s.trigger("act_mode_indexed"));
        QCOMPARE(s.ops.size(), n + 1);
        QVERIFY(s.book().page(1).rgb && s.book().page(1).rgb->table);
        for (const core::Layer& layer : s.book().page(1).layers) QVERIFY(layer.id != "hidden");
        QVERIFY(std::any_of(s.book().page(2).layers.begin(), s.book().page(2).layers.end(), [](const core::Layer& l) { return l.id == "away"; }));
    }

    // Review G: the refusals of the special modes' arguments name the choices as the dialogs show them (their combo
    // boxes' words, in order).
    void modeRefusalsUseTheDialogsWords() {
        Studio s;
        core::Page indexed = s.book().page(0);
        core::RgbMode mode;
        mode.table = core::ColorTable{};
        mode.table->colors = {{0, 0, 0}, {255, 255, 255}};
        indexed.rgb = mode;
        app::IndexedDialog colours(s.window.get(), &indexed);
        app::ColorTableDialog table(s.window.get(), indexed);
        app::BitmapDialog bitmap(s.window.get(), nullptr);
        app::DuotoneDialog duotone(s.window.get(), nullptr);
        const auto words = [](const QComboBox* box) {
            QStringList out;
            for (int i = 0; i < box->count(); ++i) out << box->itemText(i);
            return out.join(QStringLiteral("・"));
        };
        const std::vector<std::pair<std::string, const QComboBox*>> cases{
            {"palette is selective, perceptual, adaptive, exact, systemMac, systemWindows, web or uniform", colours.palette},
            {"forced is blackWhite, none, primaries or web", colours.forced},
            {"dither is diffusion, none, pattern or noise", colours.dither},
            {"table is custom, blackBody, grayscale, spectrum, systemMac, systemWindows or web", table.preset},
            {"method is diffusion, threshold, pattern or halftone", bitmap.method},
            {"shape is round, ellipse, line, square, diamond or cross", bitmap.shape},
            {"type is monotone, duotone, tritone or quadtone", duotone.type},
        };
        for (const auto& [message, box] : cases) {
            const QString said = wording::error(message);
            const QString choices = said.mid(said.indexOf(QStringLiteral("、")) + 1);  // (a space before の after a Latin word)
            QVERIFY2(choices == words(box) + QStringLiteral("のどれかです") || choices == words(box) + QStringLiteral(" のどれかです"),
                     qPrintable(said + QStringLiteral(" / ") + words(box)));
        }
    }

    void colourModesKeepThePagesOwnWay() {
        Studio s;
        app::settings()->setValue(QStringLiteral("color/workingRgb"), QStringLiteral("display-p3"));
        app::settings()->setValue(QStringLiteral("color/intent"), QStringLiteral("perceptual"));
        s.on_page(2);
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_depth_16"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_color_mode", "page": 2, "mode": "rgb", "depth": 16}])"));
        QVERIFY(s.book().page(1).rgb && s.book().page(1).rgb->depth == 16 && s.book().page(1).rgb->profile.empty() && s.book().page(1).rgb->intent == "relative");
        QVERIFY(s.trigger("act_undo"));
        app::settings()->setValue(QStringLiteral("color/workingRgb"), QStringLiteral("srgb"));
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}})));
        s.on_page(2);
        n = s.ops.size();
        QVERIFY(s.trigger("act_mode_rgb"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_color_mode", "page": 2, "mode": "rgb", "depth": 8, "intent": "perceptual", "page_intent": "relative"}])"));
        QVERIFY(!s.book().page(1).color && !s.book().page(1).rgb);
        for (const char* key : {"color/workingRgb", "color/intent"}) app::settings()->remove(QString::fromLatin1(key));
    }

    // ページ → 色モード → a depth on an sRGB page that has an RGB mode of its own (32 bits; review I): the op names no
    // profile (sRGB has none to name) and is done; the refusals the colour model's ops say, in Japanese (review P).
    void depthOfAnSrgbPage() {
        Studio s;
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "rgb"}, {"depth", 32}}})));
        s.on_page(2);
        const std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_depth_16"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_color_mode", "page": 2, "mode": "rgb", "depth": 16, "intent": "relative"}])"));
        QVERIFY(s.book().page(1).rgb && s.book().page(1).rgb->depth == 16 && s.book().page(1).rgb->profile.empty());
        QCOMPARE(wording::error(QStringLiteral("page 2: the layer ab12 (絵) holds HDR values past 0..1, which cannot be converted as they are "
                                               "(clip_hdr: true cuts them and converts)")),
                 QStringLiteral("レイヤー ab12 (絵) に HDR の値（0〜1 の外）があるので、このままでは変換できません（clip_hdr: true で切って変換します）"));
        QCOMPARE(wording::error(QStringLiteral("page 2: the layer tn is a tone layer: a page in precise colour (16 or 32 bits, or of a profile) cannot draw it yet")),
                 QStringLiteral("2 ページのレイヤー tn は、16・32 ビットやプロファイル付きのページではまだ描けません（トーン・描画モード・はみ出し・線・フォルダーなどを確かめてください）"));
    }

    // ページの色モード…, color-model@1: grey and 32 bits among the modes and depths; each mode's profiles — the built-in
    // ones (RGB's from sRGB, grey's from sGray) and the book's —; black point compensation, dither and the HDR cut sent
    // only when they are not the defaults; the page's own choices shown when it is opened again.
    void colourModelInTheDialog() {
        Studio s;
        QStringList seen;
        std::function<void(QDialog*)> answer;
        int result = QDialog::Accepted;
        s.answers.responder->exec = [&](QDialog* d) {
            seen.clear();
            for (const char* name : {"color_mode_mode", "color_mode_depth", "color_mode_profile", "color_mode_gray"}) {
                auto* box = d->findChild<QComboBox*>(QString::fromLatin1(name));
                seen << described(box) + (box != nullptr && !box->isEnabled() ? QStringLiteral(" off") : QString());
            }
            for (const char* name : {"color_mode_bpc", "color_mode_dither", "color_mode_clip_hdr"}) seen << described(d->findChild<QCheckBox*>(QString::fromLatin1(name)));
            if (answer) answer(d);
            return result;
        };
        const QString rgb_profiles = QStringLiteral(
            "sRGB（プロファイルなし）=srgb|Display P3=display-p3|Adobe RGB (1998)-compatible (Photocraft)=adobe-rgb-compat|"
            "ProPhoto RGB-compatible (ROMM primaries, Photocraft)=prophoto-compat|Linear sRGB (Photocraft)=linear-srgb|Rec. ITU-R BT.2020 (Photocraft)=rec2020");
        s.on_page(2);
        result = QDialog::Rejected;
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_color_mode"));
        QCOMPARE(seen.at(0), QStringLiteral("RGB=rgb|グレースケール=gray|CMYK（印刷のインキ）=cmyk|Lab=lab @rgb"));
        QCOMPARE(seen.at(1), QStringLiteral("8 bit=8|16 bit=16|32 bit（浮動小数）=32 @8"));
        QCOMPARE(seen.at(2), rgb_profiles + QStringLiteral(" @srgb"));
        QCOMPARE(seen.at(3), QStringLiteral("プロファイルで変換=profile|K 版だけ（線画向け）=k @profile off"));
        QCOMPARE(seen.mid(4), QStringList({QStringLiteral("黒点の補正 [x]"), QStringLiteral("8 bit に下げるときディザをかける [ ]"), QStringLiteral("0〜1 を超える HDR の値を切る [ ]")}));
        QCOMPARE(s.ops.size(), n);
        // RGB 16 bits in Display P3, without black point compensation (grey's profiles seen on the way)
        result = QDialog::Accepted;
        QString grey_profiles;
        bool greys_on = false;
        answer = [&](QDialog* d) {
            auto* mode = d->findChild<QComboBox*>(QStringLiteral("color_mode_mode"));
            auto* depth = d->findChild<QComboBox*>(QStringLiteral("color_mode_depth"));
            auto* profile = d->findChild<QComboBox*>(QStringLiteral("color_mode_profile"));
            mode->setCurrentIndex(mode->findData(QStringLiteral("gray")));
            grey_profiles = described(profile);
            greys_on = d->findChild<QComboBox*>(QStringLiteral("color_mode_gray"))->isEnabled();
            mode->setCurrentIndex(mode->findData(QStringLiteral("rgb")));
            depth->setCurrentIndex(depth->findData(16));
            profile->setCurrentIndex(profile->findData(QStringLiteral("display-p3")));
            d->findChild<QCheckBox*>(QStringLiteral("color_mode_bpc"))->setChecked(false);
        };
        QVERIFY(s.trigger("act_color_mode"));
        QCOMPARE(grey_profiles, QStringLiteral("Gray Gamma 2.2 (Photocraft)=gray-gamma-2.2|sGray (sRGB tone curve, Photocraft)=sgray @sgray"));
        QVERIFY(greys_on);
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_color_mode", "mode": "rgb", "depth": 16, "page": 2, "profile": "display-p3", "bpc": false}])"));
        const core::Page& page = s.book().page(1);
        QVERIFY(!page.color && page.rgb);
        QCOMPARE(page.rgb->depth, 16);
        QCOMPARE(page.rgb->profile, core::builtin_profile("display-p3")->ref());
        QVERIFY(!page.rgb->bpc);
        s.window->flash(QString(), 1);
        QTRY_VERIFY2(s.window->status_label()->text().contains(QStringLiteral("RGB 16 bit・Display P3")), qPrintable(s.window->status_label()->text()));
        QVERIFY(drawn_as_book(s));
        // opened again: the page's own choices
        result = QDialog::Rejected;
        answer = nullptr;
        QVERIFY(s.trigger("act_color_mode"));
        QCOMPARE(seen.at(1), QStringLiteral("8 bit=8|16 bit=16|32 bit（浮動小数）=32 @16"));
        QCOMPARE(seen.at(2), rgb_profiles + QStringLiteral(" @display-p3"));
        QCOMPARE(seen.at(4), QStringLiteral("黒点の補正 [ ]"));
        // grey, 32 bits (floats), the greys K alone, dither and the HDR cut asked for
        result = QDialog::Accepted;
        answer = [](QDialog* d) {
            auto* mode = d->findChild<QComboBox*>(QStringLiteral("color_mode_mode"));
            mode->setCurrentIndex(mode->findData(QStringLiteral("gray")));
            auto* depth = d->findChild<QComboBox*>(QStringLiteral("color_mode_depth"));
            depth->setCurrentIndex(depth->findData(32));
            auto* gray = d->findChild<QComboBox*>(QStringLiteral("color_mode_gray"));
            gray->setCurrentIndex(gray->findData(QStringLiteral("k")));
            d->findChild<QCheckBox*>(QStringLiteral("color_mode_bpc"))->setChecked(true);
            d->findChild<QCheckBox*>(QStringLiteral("color_mode_dither"))->setChecked(true);
            d->findChild<QCheckBox*>(QStringLiteral("color_mode_clip_hdr"))->setChecked(true);
        };
        n = s.ops.size();
        QVERIFY(s.trigger("act_color_mode"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "set_color_mode", "mode": "gray", "depth": 32, "page": 2, "intent": "relative", "profile": "sgray", "gray": "k", "dither": true, "clip_hdr": true}])"));
        QVERIFY(s.book().page(1).color && s.book().page(1).color->space == core::InkSpace::Gray && s.book().page(1).color->depth == 32);
        QVERIFY(!s.book().page(1).rgb);
        QVERIFY(drawn_as_book(s));
        // CMYK: the built-in profile offered (none chosen until one is)
        answer = [](QDialog* d) {
            auto* mode = d->findChild<QComboBox*>(QStringLiteral("color_mode_mode"));
            mode->setCurrentIndex(mode->findData(QStringLiteral("cmyk")));
            auto* profile = d->findChild<QComboBox*>(QStringLiteral("color_mode_profile"));
            QCOMPARE(described(profile), QStringLiteral("Photocraft Coated CMYK (synthetic, 300% TAC, medium GCR)=coated-cmyk @"));
            auto* ok = d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
            QVERIFY(!ok->isEnabled());
            profile->setCurrentIndex(0);
            QVERIFY(ok->isEnabled());
        };
        n = s.ops.size();
        QVERIFY(s.trigger("act_color_mode"));
        const Json sent = Json::parse(s.since(n));
        QCOMPARE(QString::fromStdString(sent[0]["profile"].get<std::string>()), QStringLiteral("coated-cmyk"));
        QCOMPARE(sent[0]["depth"].get<int>(), 32);
        QVERIFY(s.book().page(1).color->space == core::InkSpace::Cmyk && s.book().page(1).color->depth == 32);
    }

    // プロファイルの指定… / プロファイルに変換… (edit.assignProfile, edit.convertToProfile): the page in front or every
    // page. Assign: the profiles of the page's own space (none: sRGB or sGray; the page's own chosen to begin with), the
    // pixels kept and the profile named — none for a Lab page. Convert: any profile (RGB, grey, CMYK, Lab), its intent
    // and black point compensation; the page in the profile's mode. Undone as any op.
    void profilesAssignedAndConverted() {
        Studio s;
        QStringList seen;
        std::function<void(QDialog*)> answer;
        int result = QDialog::Accepted;
        bool ok_on = false;
        s.answers.responder->exec = [&](QDialog* d) {
            seen.clear();
            seen << d->objectName();
            for (const char* name : {"profile_scope", "profile_choice", "profile_intent"}) {
                if (auto* box = d->findChild<QComboBox*>(QString::fromLatin1(name))) seen << described(box);
            }
            for (const char* name : {"profile_bpc", "profile_dither", "profile_clip_hdr"}) {
                if (auto* box = d->findChild<QCheckBox*>(QString::fromLatin1(name))) seen << described(box);
            }
            ok_on = d->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled();
            if (answer) answer(d);
            return result;
        };
        s.on_page(2);
        // assign: Display P3 to an RGB page
        answer = [](QDialog* d) {
            auto* profile = d->findChild<QComboBox*>(QStringLiteral("profile_choice"));
            profile->setCurrentIndex(profile->findData(QStringLiteral("display-p3")));
        };
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_assign_profile"));
        QCOMPARE(seen, QStringList({QStringLiteral("assign_profile_dialog"), QStringLiteral("このページ（2 ページ）=page|すべてのページ=all @page"),
                                    QStringLiteral("なし（sRGB）=none|Display P3=display-p3|Adobe RGB (1998)-compatible (Photocraft)=adobe-rgb-compat|"
                                                   "ProPhoto RGB-compatible (ROMM primaries, Photocraft)=prophoto-compat|Linear sRGB (Photocraft)=linear-srgb|"
                                                   "Rec. ITU-R BT.2020 (Photocraft)=rec2020 @none")}));
        QCOMPARE(s.since(n), std::string(R"([{"op": "assign_profile", "page": 2, "profile": "display-p3"}])"));
        QVERIFY(s.book().page(1).rgb && s.book().page(1).rgb->profile == core::builtin_profile("display-p3")->ref());
        QVERIFY(!s.book().page(1).color);
        QCOMPARE(s.window->last_notice(), QStringLiteral("プロファイルを指定しました"));
        QVERIFY(drawn_as_book(s));
        // assigned again: the page's own chosen first; none takes it back to sRGB
        answer = [&](QDialog* d) {
            auto* profile = d->findChild<QComboBox*>(QStringLiteral("profile_choice"));
            QCOMPARE(profile->currentData().toString(), QStringLiteral("display-p3"));
            profile->setCurrentIndex(profile->findData(QStringLiteral("none")));
        };
        n = s.ops.size();
        QVERIFY(s.trigger("act_assign_profile"));
        QCOMPARE(s.since(n), std::string(R"([{"op": "assign_profile", "page": 2, "profile": "none"}])"));
        QVERIFY(!s.book().page(1).rgb);
        // convert: into sGray, perceptual, without black point compensation
        answer = [](QDialog* d) {
            auto* profile = d->findChild<QComboBox*>(QStringLiteral("profile_choice"));
            profile->setCurrentIndex(profile->findData(QStringLiteral("sgray")));
            auto* intent = d->findChild<QComboBox*>(QStringLiteral("profile_intent"));
            intent->setCurrentIndex(intent->findData(QStringLiteral("perceptual")));
            d->findChild<QCheckBox*>(QStringLiteral("profile_bpc"))->setChecked(false);
        };
        n = s.ops.size();
        QVERIFY(s.trigger("act_convert_profile"));
        QCOMPARE(seen.at(0), QStringLiteral("convert_profile_dialog"));
        QCOMPARE(seen.at(2), QStringLiteral(
                                 "sRGB（プロファイルなし）=srgb|Display P3=display-p3|Adobe RGB (1998)-compatible (Photocraft)=adobe-rgb-compat|"
                                 "ProPhoto RGB-compatible (ROMM primaries, Photocraft)=prophoto-compat|Linear sRGB (Photocraft)=linear-srgb|"
                                 "Rec. ITU-R BT.2020 (Photocraft)=rec2020|Gray Gamma 2.2 (Photocraft)=gray-gamma-2.2|sGray (sRGB tone curve, Photocraft)=sgray|"
                                 "Photocraft Coated CMYK (synthetic, 300% TAC, medium GCR)=coated-cmyk|Lab D50 identity (Photocraft)=lab-d50 @srgb"));
        QCOMPARE(seen.at(3), QStringLiteral("相対的な色域を維持=relative|知覚的=perceptual|彩度=saturation|絶対的な色域を維持=absolute @relative"));
        QCOMPARE(seen.mid(4), QStringList({QStringLiteral("黒点の補正 [x]"), QStringLiteral("8 bit でディザをかける [ ]"), QStringLiteral("0〜1 を超える HDR の値を切る [ ]")}));
        QCOMPARE(s.since(n), std::string(R"([{"op": "convert_profile", "page": 2, "profile": "sgray", "intent": "perceptual", "bpc": false}])"));
        QVERIFY(s.book().page(1).color && s.book().page(1).color->space == core::InkSpace::Gray && s.book().page(1).color->depth == 8);
        QCOMPARE(s.window->last_notice(), QStringLiteral("プロファイルに変換しました"));
        QVERIFY(drawn_as_book(s));
        // assign on the grey page: grey's profiles, its own (sGray) first
        result = QDialog::Rejected;
        answer = nullptr;
        QVERIFY(s.trigger("act_assign_profile"));
        QCOMPARE(seen.at(2), QStringLiteral("なし（sGray）=none|Gray Gamma 2.2 (Photocraft)=gray-gamma-2.2|sGray (sRGB tone curve, Photocraft)=sgray @sgray"));
        // a Lab page has no profile to assign: OK off
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "lab"}}})));
        s.on_page(3);
        n = s.ops.size();
        QVERIFY(s.trigger("act_assign_profile"));
        QVERIFY(!ok_on);
        QCOMPARE(s.ops.size(), n);
        // undone: page 2 back to RGB, sRGB
        s.on_page(2);
        result = QDialog::Accepted;
        for (int i = 0; i < 4; ++i) QVERIFY(s.trigger("act_undo"));
        QVERIFY(!s.book().page(1).color && !s.book().page(1).rgb);
        QVERIFY(!s.book().page(2).color);
    }

    // color-mode@1, step two: on a CMYK page the colour panel shows the pen colour's inks and marks a colour the page
    // cannot print as it is (色域外); inks chosen there, or picked with the eyedropper, become the pen's colour — in the
    // page's table when its rules would print other inks for it (set_inks).
    void inksInTheColourPanel() {
        Studio s;
        const std::string icc = genko::test::read_bytes(genko::test::test_data(QStringLiteral("colour/photocraft-coated-cmyk.icc")));
        s.on_page(1);
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "cmyk"}, {"icc", core::b64encode(icc)}},
                                                 Json{{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                      {"points", Json::array({Json::array({10, 40}), Json::array({50, 40})})}, {"width_mm", 3}}})));
        app::ColourPanel* panel = s.window->colours();
        QVERIFY(panel != nullptr);
        QTRY_VERIFY(panel->ink_box->isVisibleTo(panel));
        panel->choose({0, 255, 0});
        QVERIFY(panel->gamut->isVisibleTo(panel));  // (a pure green: out)
        panel->choose({20, 20, 20});                 // (Genko's black: K 100% alone, never out)
        QVERIFY(!panel->gamut->isVisibleTo(panel));
        QCOMPARE(panel->ink_spins[0]->value(), 0.0);
        QCOMPARE(panel->ink_spins[3]->value(), 100.0);
        // inks chosen in the panel
        std::size_t n = s.ops.size();
        const std::array<double, 4> chosen{10, 90, 80, 0};
        for (std::size_t i = 0; i < 4; ++i) panel->ink_spins[i]->setValue(chosen[i]);
        emit panel->ink_spins[3]->editingFinished();
        QCOMPARE(s.ops.size(), n + 1);
        QCOMPARE(QString::fromStdString(s.ops.back()["op"].get<std::string>()), QStringLiteral("set_inks"));
        const auto& table = s.book().page(0).color->inks;
        QCOMPARE(table.size(), std::size_t{1});
        QVERIFY(panel->rgb() == table[0].rgb);
        for (std::size_t i = 0; i < 4; ++i) QVERIFY(std::abs(table[0].ink[i] - chosen[i] / 100.0) < 1e-6);
        // the eyedropper on the line: K 100% alone — Genko's black, which every page prints so: no entry
        n = s.ops.size();
        s.window->canvas()->last_pick = std::make_pair(QPointF(30, 40), false);
        emit s.window->canvas()->colourPicked({35, 31, 32});
        QCOMPARE(s.ops.size(), n);
        QVERIFY((panel->rgb() == std::array<int, 3>{20, 20, 20}));
        QCOMPARE(s.book().page(0).color->inks.size(), std::size_t{1});
        QVERIFY(s.window->last_notice().contains(QStringLiteral("K 100.0%")));
        // a box left without a change changes nothing
        n = s.ops.size();
        emit panel->ink_spins[0]->editingFinished();
        QCOMPARE(s.ops.size(), n);
        // another page of the profile: the pair goes into its table too, and a colour its table holds for other inks is
        // never taken (its pair stays)
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "cmyk"}, {"icc", core::b64encode(icc)}}})));
        QCOMPARE(s.book().page(1).color->profile, s.book().page(0).color->profile);
        const std::vector<double> wanted{0.2, 0.7, 0.6, 0.0};
        const std::array<int, 3> held = render::ink::rgb_for_inks(*s.book().page(0).color, wanted);
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_inks"}, {"page", 2},
                                                      {"inks", Json::array({Json::array({Json::array({held[0], held[1], held[2]}), Json::array({1, 0, 0, 0})})})}}})));
        s.on_page(1);
        for (std::size_t i = 0; i < 4; ++i) panel->ink_spins[i]->setValue(wanted[i] * 100.0);
        emit panel->ink_spins[3]->editingFinished();
        QVERIFY(panel->rgb() != held);
        const core::InkEntry* kept = core::ink_for(*s.book().page(1).color, held);
        QVERIFY(kept != nullptr && kept->ink[0] == 1.0);
        for (std::size_t p = 0; p < 2; ++p) {
            const core::InkEntry* both = core::ink_for(*s.book().page(p).color, panel->rgb());
            QVERIFY(both != nullptr && std::abs(both->ink[1] - 0.7) < 1e-6);
        }
    }

    // color-model@1: on a grey page the colour panel shows one ink, K in %; K chosen there makes the pen's colour that grey.
    void greyInTheColourPanel() {
        Studio s;
        s.on_page(1);
        QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}})));
        app::ColourPanel* panel = s.window->colours();
        QVERIFY(panel != nullptr);
        QTRY_VERIFY(panel->ink_box->isVisibleTo(panel));
        QCOMPARE(panel->ink_labels[0]->text(), QStringLiteral("K"));
        QVERIFY(panel->ink_spins[0]->isVisibleTo(panel));
        for (std::size_t i = 1; i < 4; ++i) QVERIFY(!panel->ink_spins[i]->isVisibleTo(panel));
        QCOMPARE(panel->ink_spins[0]->suffix(), QStringLiteral("%"));
        QCOMPARE(panel->ink_spins[0]->maximum(), 100.0);
        panel->choose({255, 255, 255});
        QCOMPARE(panel->ink_spins[0]->value(), 0.0);
        panel->choose({0, 0, 0});
        QCOMPARE(panel->ink_spins[0]->value(), 100.0);
        panel->ink_spins[0]->setValue(30.0);
        emit panel->ink_spins[0]->editingFinished();
        const auto rgb = panel->rgb();
        QVERIFY(rgb[0] == rgb[1] && rgb[1] == rgb[2]);
        QVERIFY2(std::abs(rgb[0] - 179) <= 1, qPrintable(QString::number(rgb[0])));
    }

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

    // ほかの原稿のページを取り込む… with a page whose placed picture this build does not copy (its file a link, or not
    // holding what its name says): refused in the person's words, no page taken in — this book would refer to a picture
    // it does not have (Python copies such files as they are). Pages that do not use it come in.
    void takenInPagesNeedTheirPictures() {
        Studio s(2);
        const auto book_with = [&](const QString& name, const std::string& first, const std::string& second) {
            const fs::path dir = gui_test::path_of(s.tmp.path() + QStringLiteral("/") + name);
            core::Document doc = gui_test::new_doc(2, "ほかの原稿");
            for (const auto& [page, ref] : {std::pair{0, first}, std::pair{1, second}}) {
                core::Layer placed;
                placed.id = "placed-art-" + std::to_string(page);
                placed.kind = core::LayerKind::Placed;
                placed.asset = ref;
                placed.placement_mm = core::Rect{core::Num(10), core::Num(10), core::Num(40), core::Num(40)};
                doc.edit_page(static_cast<std::size_t>(page)).layers.push_back(placed);
            }
            gui_test::write_book(dir, doc);
            return dir;
        };
        const std::string fine = storage::AssetStore::ref(render::write_png(render::Image::frombytes("RGB", {4, 4}, std::string(48, '\x30'))));
        const std::string wrong = storage::AssetStore::ref("what the name says");
        const fs::path corrupt = book_with(QStringLiteral("corrupt.genko"), wrong, fine);
        storage::AssetStore store(corrupt);
        write_bytes(store.path(wrong, ".png"), "something else");
        (void)store.put_bytes(render::write_png(render::Image::frombytes("RGB", {4, 4}, std::string(48, '\x30'))), ".png");
        QString folder = QString::fromStdString(core::path_to_utf8(corrupt));
        QString pages;
        s.answers.responder->existing_dir = [&](const QString&, const QString&) { return folder; };
        s.answers.responder->get_text = [&](const QString&, const QString&, const QString&) { return std::optional<QString>(pages); };
        const auto before = s.session->snapshot();
        std::size_t n = s.ops.size();
        QVERIFY(s.trigger("act_merge_book"));
        QCOMPARE(s.ops.size(), n);
        QCOMPARE(s.window->last_error(), QStringLiteral("取り込む原稿の素材ファイル（%1）を、この原稿に写せないため取り込めません（リンクや、名前と中身の違うファイルは写しません）")
                                             .arg(QString::fromStdString(core::path_to_utf8(store.path(wrong, ".png")))));
        QCOMPARE(s.session->snapshot(), before);
        // its page 2 alone: taken in, its picture with it
        pages = QStringLiteral("2");
        QVERIFY(s.trigger("act_merge_book"));
        QCOMPARE(s.book().pages.size(), std::size_t{3});
        QVERIFY(storage::AssetStore(s.path).has(fine, ".png"));
        // a picture whose file is a link (to a file outside the book)
        const std::string linked = storage::AssetStore::ref("not this book's");
        const fs::path linking = book_with(QStringLiteral("linking.genko"), linked, linked);
        write_bytes(linking.parent_path() / "outside.png", "not this book's");
        const fs::path link = storage::AssetStore(linking).path(linked, ".png");
        fs::create_directories(link.parent_path());
        std::error_code ec;
        fs::create_symlink(linking.parent_path() / "outside.png", link, ec);
        if (ec) return;  // (no links here)
        folder = QString::fromStdString(core::path_to_utf8(linking));
        pages.clear();
        n = s.ops.size();
        QVERIFY(s.trigger("act_merge_book"));
        QCOMPARE(s.ops.size(), n);
        QCOMPARE(s.window->last_error(), QStringLiteral("取り込む原稿の素材ファイル（%1）を、この原稿に写せないため取り込めません（リンクや、名前と中身の違うファイルは写しません）")
                                             .arg(QString::fromStdString(core::path_to_utf8(link))));
        QCOMPARE(s.book().pages.size(), std::size_t{3});
        QVERIFY(!storage::AssetStore(s.path).has(linked, ".png"));
    }

    // A book this build can open only read-only (a feature it does not know) is not taken in: said in the person's
    // words, before the pages are asked for.
    void aBookOpenOnlyToReadIsNotTakenIn() {
        Studio s(2);
        const OtherBook other(s.tmp.path() + QStringLiteral("/future.genko"), 2);
        {
            QFile file(gui_test::qpath(other.path / "project.json"));
            QVERIFY(file.open(QIODevice::ReadOnly));
            Json project = Json::parse(file.readAll().toStdString());
            file.close();
            project["features"] = Json::array({"genko.future-feature"});
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QByteArray::fromStdString(project.dump(2)));
        }
        bool texts = false;
        s.answers.responder->existing_dir = [&](const QString&, const QString&) { return QString::fromStdString(other.text()); };
        s.answers.responder->get_text = [&](const QString&, const QString&, const QString&) {
            texts = true;
            return std::optional<QString>(QString());
        };
        QVERIFY(s.trigger("act_merge_book"));
        QVERIFY(!texts);
        QCOMPARE(s.window->last_error(),
                 QStringLiteral("その原稿を読めませんでした:\n取り込む原稿に、見つからない絵や読めないデータがあるため取り込めません（その原稿を開いて確かめてください）"));
        QCOMPARE(s.book().pages.size(), std::size_t{2});
    }

    // Another book brought to the front while the folder is chosen: nothing is done (the question was about the book
    // in front before), said as for any question whose book changed meanwhile.
    void aBookChangedWhileTheFolderIsChosen() {
        Studio s(2);
        const fs::path second = gui_test::path_of(s.tmp.path() + QStringLiteral("/second.genko"));
        gui_test::write_book(second, gui_test::new_doc(1));
        bool texts = false;
        s.answers.responder->existing_dir = [&](const QString&, const QString&) {
            s.window->add_document(app::Session::open(second, gui_test::quick(gui_test::path_of(s.tmp.path() + QStringLiteral("/recovery2")))));
            return QString::fromStdString(core::path_to_utf8(second));  // (the book in front now)
        };
        s.answers.responder->get_text = [&](const QString&, const QString&, const QString&) {
            texts = true;
            return std::optional<QString>(QString());
        };
        QVERIFY(s.trigger("act_merge_book"));
        QCOMPARE(s.window->current_document(), 1);
        QVERIFY(!texts);
        QCOMPARE(s.window->last_error(), QStringLiteral("確認中に対象の原稿・ページが変更されたため、操作を中止しました。やり直してください。"));
    }

    // The コマ tool's settings: after 形 (断ち切り, 選んだコマの形を元に戻す, コマ番号), 原稿 with 原稿用紙の設定…, as Python's page.
    void thePanelToolsPageHasThePaper() {
        Studio s(1);
        s.window->choose_tool(QStringLiteral("frame"));
        const QWidget* page = s.window->tool_settings()->current_page();
        QStringList rows;
        for (int i = 0; i < page->layout()->count(); ++i) {
            QWidget* w = page->layout()->itemAt(i)->widget();
            if (auto* label = qobject_cast<QLabel*>(w)) rows << label->text();
            if (auto* button = qobject_cast<QAbstractButton*>(w)) rows << button->text();
        }
        const auto text = [&](const char* name) { return s.window->action(QString::fromLatin1(name))->iconText(); };
        QVERIFY2(rows.contains(QStringLiteral("形")), qPrintable(rows.join(QStringLiteral(" | "))));
        QCOMPARE(rows.mid(rows.indexOf(QStringLiteral("形"))),
                 (QStringList{QStringLiteral("形"), text("act_bleed"), text("act_reset_shape"), text("act_frame_numbers"), QStringLiteral("原稿"),
                              text("act_paper"), QStringLiteral("コマを選ぶと、辺の中ほどの ◇ をドラッグで辺を曲げられます（外へふくらむ・内へへこむ）。")}));
    }

    // A change that takes a while (Python's SLOW_OPS: 表紙・3D・ページの取り込み…) says so in the status line, with the
    // busy cursor, while it is made; an ordinary one does not.
    void slowChangesSayTheyAreBusy() {
        Studio s(2);
        QString shown;
        bool busy = false;
        {
            QObject guard;  // (the look is taken while the change is made, or not at all)
            QTimer::singleShot(0, &guard, [&] {
                shown = s.window->status_label()->text();
                busy = QGuiApplication::overrideCursor() != nullptr && QGuiApplication::overrideCursor()->shape() == Qt::WaitCursor;
            });
            QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_cover"}, {"kind", "front"}}})));
        }
        QCOMPARE(shown, QStringLiteral("<b>表紙を作っています…</b>"));
        QVERIFY(busy);
        QVERIFY(QGuiApplication::overrideCursor() == nullptr);
        shown.clear();
        {
            QObject guard;
            QTimer::singleShot(0, &guard, [&] { shown = QStringLiteral("looked"); });
            QVERIFY(s.window->apply_ops(Json::array({Json{{"op", "add_page"}, {"count", 1}}})));
        }
        QCOMPARE(shown, QString());
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
