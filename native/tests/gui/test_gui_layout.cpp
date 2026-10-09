// The screens and the small widgets of SPEC UX-02 / ACCEPTANCE.md AC-UX (M2-G1 試験 4, 5).
//
//   4  the start screen, 新しい原稿, 用紙の設定 (for a new book and for the book's own), ノンブルの設定, 表紙・カバーを足す,
//      the templates, the question before closing, the whole path and the main window, on screens of 1024 × 640,
//      1366 × 768 and 1920 × 1080 at 100, 125, 150 and 200 % scaling: each window is inside the screen, and its buttons
//      (閉じる, 作る, やめる, 決める, キャンセル…) are wholly in sight — the rest of the body scrolls. The size asked for
//      and the size each window got are both written to the log.
//   5  a path of more than 200 characters (Japanese folder names) is cut in the middle to fit, never only in part: a
//      click shows all of it, コピー puts all of it on the clipboard.
//   and the preview that fits a page whatever its shape (I08), the entrance for typed Japanese (input method events:
//   the words being converted, then the words settled), and the lasting cache of page pictures (its key, least
//   recently used pictures removed first, only the rows in sight drawn).
//
// Each screen configuration runs in a child process (this program again, GENKO_LAYOUT_PROBE=1) on the offscreen platform
// with a screen of that size (QT_QPA_PLATFORM=offscreen:configfile=…) and QT_SCALE_FACTOR.

#include <QtTest>

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QInputMethodEvent>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStatusBar>

#include <fstream>
#include <memory>

#include "app/book_dialogs.hpp"
#include "app/canvas.hpp"
#include "app/dialogs.hpp"
#include "app/fit_preview.hpp"
#include "app/icons.hpp"
#include "app/ime.hpp"
#include "app/main_window.hpp"
#include "app/pages_panel.hpp"
#include "app/path_label.hpp"
#include "app/save_status.hpp"
#include "app/theme.hpp"
#include "app/thumbs.hpp"
#include "core/strokes.hpp"
#include "gui_support.hpp"
#include "storage/reader.hpp"

using namespace gui_test;
namespace app = genko::app;
namespace core = genko::core;

namespace {

// A folder path of more than 200 characters, most of them Japanese.
QString long_path(const QString& root) {
    QString path = root;
    for (int i = 0; i < 6; ++i) path += QStringLiteral("/第%1部_とても長い日本語のフォルダー名・原稿置き場_描きかけの作品").arg(i + 1);
    return path + QStringLiteral("/最終話「さよならの向こう側」.genko");
}

Json rect_json(const QRect& r) { return Json::array({r.x(), r.y(), r.width(), r.height()}); }

// --- the child process: one screen configuration -------------------------------------------------------------------

struct Seen {
    QString label;
    QWidget* widget;
    bool footer;  // must be wholly in sight (else: in the window, reachable by scrolling)
};

Json look_at(const QString& name, QWidget* window, const QSize& asked, const std::vector<Seen>& seen) {
    window->show();
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    Json r = Json::object();
    r["name"] = name.toStdString();
    r["asked"] = Json::array({asked.width(), asked.height()});
    r["window"] = rect_json(window->frameGeometry());
    QScreen* screen = window->screen() != nullptr ? window->screen() : QGuiApplication::primaryScreen();
    r["screen"] = rect_json(screen->availableGeometry());
    r["dpr"] = screen->devicePixelRatio();
    Json items = Json::array();
    for (const Seen& s : seen) {
        const QRect in_window(s.widget->mapTo(window, QPoint(0, 0)), s.widget->size());
        const QRect visible = s.widget->visibleRegion().boundingRect();
        Json item = Json::object({{"label", s.label.toStdString()},
                                  {"rect", rect_json(in_window)},
                                  {"visible", s.widget->isVisible()},
                                  {"whole", visible == s.widget->rect()},
                                  {"footer", s.footer}});
        // in a body that scrolls: where its viewport is in the window, and whether the item is in the scrolled content
        for (QWidget* up = s.widget->parentWidget(); up != nullptr && up != window; up = up->parentWidget()) {
            if (auto* scroll = qobject_cast<QScrollArea*>(up)) {
                const QWidget* port = scroll->viewport();
                item["scroll"] = rect_json(QRect(port->mapTo(window, QPoint(0, 0)), port->size()));
                item["in_content"] = scroll->widget() != nullptr &&
                                     scroll->widget()->rect().contains(QRect(s.widget->mapTo(scroll->widget(), QPoint(0, 0)), s.widget->size()));
                break;
            }
        }
        items.push_back(item);
    }
    r["items"] = items;
    window->close();
    return r;
}

std::vector<Seen> buttons_of(QWidget* parent, bool footer) {
    std::vector<Seen> out;
    for (QDialogButtonBox* box : parent->findChildren<QDialogButtonBox*>()) {
        for (QAbstractButton* b : box->buttons()) out.push_back(Seen{b->text(), b, footer});
    }
    return out;
}

int probe(int argc, char** argv) {
    QApplication application(argc, argv);
    app::icons::init_resources();
    app::theme::apply(&application);
    std::ofstream out(qEnvironmentVariable("GENKO_LAYOUT_OUT").toStdString());
    QTemporaryDir dir;
    const fs::path book = path_of(dir.filePath("試しの原稿.genko"));
    make_book(book, 4);
    app::remember_project(book);  // (a recent book on the start screen)
    {
        app::StartDialog start;
        std::vector<Seen> seen = {{QStringLiteral("閉じる"), start.close_button(), true}};
        for (QPushButton* card : start.cards()) seen.push_back(Seen{card->text(), card, false});
        seen.push_back(Seen{QStringLiteral("開く"), start.open_button(), false});
        out << look_at(QStringLiteral("start"), &start, QSize(960, 640), seen).dump() << "\n";
    }
    {
        app::NewProjectDialog dialog;
        out << look_at(QStringLiteral("new"), &dialog, QSize(780, 500),
                       {{QStringLiteral("作る"), dialog.ok_button(), true}, {QStringLiteral("やめる"), dialog.cancel_button(), true}})
                   .dump()
            << "\n";
    }
    {
        app::PaperDialog dialog(nullptr, core::PageSpec::b4_comic());
        out << look_at(QStringLiteral("paper"), &dialog, QSize(760, 520),
                       {{QStringLiteral("決める"), dialog.ok_button(), true}, {QStringLiteral("やめる"), dialog.cancel_button(), true}})
                   .dump()
            << "\n";
    }
    {
        // 原稿用紙の設定… (the book's paper changed: its question whether what is on the pages moves)
        app::PaperDialog dialog(nullptr, core::PageSpec::b4_comic(), true);
        out << look_at(QStringLiteral("paper-change"), &dialog, QSize(760, 520),
                       {{QStringLiteral("変える"), dialog.ok_button(), true}, {QStringLiteral("やめる"), dialog.cancel_button(), true},
                        {QStringLiteral("動かす"), dialog.move, false}})
                   .dump()
            << "\n";
    }
    {
        // ノンブルの設定… and 表紙・カバーを足す… (M4: Python's forms; their rows scroll, their buttons stay in sight)
        const core::Document doc = new_doc(2);
        app::NombreDialog nombre(nullptr, doc);
        std::vector<Seen> seen = buttons_of(&nombre, true);
        for (QWidget* field : std::vector<QWidget*>{nombre.shown, nombre.position, nombre.face, nombre.size_mm, nombre.start, nombre.hidden,
                                                    nombre.hidden_mm})
            seen.push_back(Seen{field->objectName(), field, false});
        out << look_at(QStringLiteral("nombre"), &nombre, nombre.size(), seen).dump() << "\n";
        app::CoverDialog cover(nullptr, doc);
        seen = buttons_of(&cover, true);
        for (QWidget* field : std::vector<QWidget*>{cover.kind, cover.spine, cover.flap, cover.band}) seen.push_back(Seen{field->objectName(), field, false});
        out << look_at(QStringLiteral("cover"), &cover, cover.size(), seen).dump() << "\n";
    }
    {
        auto doc = std::make_shared<const core::Document>(new_doc(2));
        app::TemplateDialog dialog(nullptr, doc, 0, "human:tester");
        out << look_at(QStringLiteral("template"), &dialog, QSize(720, 520), buttons_of(&dialog, true)).dump() << "\n";
    }
    {
        app::SaveStatus status;
        status.kind = app::SaveKind::Failed;
        status.reason = QStringLiteral("No space left on device");
        app::CloseGuard guard(nullptr, status, QStringLiteral("とても長い題名の原稿をここに書いておく 第12話"));
        out << look_at(QStringLiteral("close"), &guard, guard.size(),
                       {{QStringLiteral("保存して閉じる"), guard.save_button(), true},
                        {QStringLiteral("別の場所に保存して閉じる"), guard.save_as_button(), true},
                        {QStringLiteral("保存せずに閉じる…"), guard.discard_button(), true},
                        {QStringLiteral("キャンセル"), guard.cancel_button(), true}})
                   .dump()
            << "\n";
    }
    {
        app::PathDialog dialog(nullptr, QStringLiteral("保存先"), long_path(dir.path()));
        std::vector<Seen> seen;
        for (QPushButton* b : dialog.findChildren<QPushButton*>()) seen.push_back(Seen{b->text(), b, true});
        out << look_at(QStringLiteral("path"), &dialog, dialog.size(), seen).dump() << "\n";
    }
    {
        app::MainWindow window(app::Session::open(book, quick(path_of(dir.filePath("recovery")))));
        std::vector<Seen> seen = {{QStringLiteral("canvas"), window.canvas(), false},
                                  {QStringLiteral("status"), window.statusBar(), true}};
        out << look_at(QStringLiteral("main"), &window, QSize(1280, 800), seen).dump() << "\n";
    }
    return 0;
}

}  // namespace

class TestGuiLayout : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        config_folder();
        app::icons::init_resources();
        app::theme::apply(qApp);
    }

    void theWindowsFitEveryScreen_data() {
        QTest::addColumn<QSize>("screen");
        QTest::addColumn<double>("scale");
        QTest::addColumn<bool>("colon_directory");
        for (const QSize size : {QSize(1024, 640), QSize(1366, 768), QSize(1920, 1080)}) {
            for (const double scale : {1.0, 1.25, 1.5, 2.0}) {
                const QString name = QStringLiteral("%1x%2@%3").arg(size.width()).arg(size.height()).arg(scale);
                QTest::newRow(qPrintable(name)) << size << scale << false;
#ifndef Q_OS_WIN
                // Qt splits platform options at colons; reproduce the Windows drive delimiter on Linux too.
                QTest::newRow(qPrintable(name + QStringLiteral("-colon-path"))) << size << scale << true;
#endif
            }
        }
    }

    void theWindowsFitEveryScreen() {
        QFETCH(QSize, screen);
        QFETCH(double, scale);
        QFETCH(bool, colon_directory);
        QTemporaryDir dir(QDir::tempPath() + (colon_directory ? QStringLiteral("/genko:layout-XXXXXX") : QStringLiteral("/genko-layout-XXXXXX")));
        QVERIFY(dir.isValid());
        const QString config = dir.filePath("screen.json");
        {
            QFile file(config);
            QVERIFY(file.open(QIODevice::WriteOnly));
            const Json screens = Json::object(
                {{"synchronousWindowSystemEvents", false},
                 {"windowFrameMargins", false},
                 {"screens", Json::array({Json::object({{"name", "probe"}, {"x", 0}, {"y", 0}, {"width", screen.width()}, {"height", screen.height()},
                                                        {"logicalDpi", 96}, {"logicalBaseDpi", 96}, {"dpr", 1}})})}});
            file.write(QByteArray::fromStdString(screens.dump()));
        }
        const QString report = dir.filePath("report.jsonl");
        QProcess child;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        // The platform parser splits at ':' even inside a Windows drive path.
        // Resolve the config relative to this child's private working directory instead.
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen:configfile=screen.json"));
        env.insert(QStringLiteral("QT_SCALE_FACTOR"), QString::number(scale));
        env.insert(QStringLiteral("GENKO_LAYOUT_PROBE"), QStringLiteral("1"));
        env.insert(QStringLiteral("GENKO_LAYOUT_OUT"), report);
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), dir.filePath("config"));
        child.setProcessEnvironment(env);
        child.setWorkingDirectory(dir.path());
        child.start(QCoreApplication::applicationFilePath(), {});
        QVERIFY(child.waitForFinished(120000));
        QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                 qPrintable(QStringLiteral("status %1, exit %2: %3").arg(child.exitStatus()).arg(child.exitCode()).arg(QString::fromUtf8(child.readAllStandardError()))));
        QFile file(report);
        QVERIFY(file.open(QIODevice::ReadOnly));
        int windows = 0;
        const double logical_w = screen.width() / scale;
        const double logical_h = screen.height() / scale;
        while (!file.atEnd()) {
            const Json r = Json::parse(file.readLine().toStdString());
            ++windows;
            const QString name = QString::fromStdString(r["name"].get<std::string>());
            const Json& w = r["window"];
            const Json& s = r["screen"];
            const QRect window(w[0].get<int>(), w[1].get<int>(), w[2].get<int>(), w[3].get<int>());
            const QRect room(s[0].get<int>(), s[1].get<int>(), s[2].get<int>(), s[3].get<int>());
            // (the screen the child had: the configured one, in logical pixels)
            QVERIFY2(std::abs(room.width() - logical_w) <= 1 && std::abs(room.height() - logical_h) <= 1,
                     qPrintable(QStringLiteral("screen %1×%2").arg(room.width()).arg(room.height())));
            qInfo("%s: screen %d×%d at %.0f%% (%d×%d logical); asked %d×%d, got %d×%d at %d,%d", qPrintable(name), screen.width(), screen.height(),
                  scale * 100, room.width(), room.height(), r["asked"][0].get<int>(), r["asked"][1].get<int>(), window.width(), window.height(),
                  window.x(), window.y());
            QVERIFY2(room.contains(window), qPrintable(name + QStringLiteral(": the window goes out of the screen")));
            for (const Json& item : r["items"]) {
                const QString label = QString::fromStdString(item["label"].get<std::string>());
                const Json& b = item["rect"];
                const QRect at(b[0].get<int>(), b[1].get<int>(), b[2].get<int>(), b[3].get<int>());
                QVERIFY2(item["visible"].get<bool>(), qPrintable(name + QStringLiteral(": 「") + label + QStringLiteral("」 is hidden")));
                QVERIFY2(at.width() > 0 && at.height() > 0, qPrintable(name + QStringLiteral(": 「") + label + QStringLiteral("」 has no room")));
                const QRect inside(QPoint(0, 0), window.size());
                if (item.contains("scroll") && !item["footer"].get<bool>()) {
                    // (in the scrolling body: reachable — the body's viewport is in the window, the item in its content)
                    const Json& v = item["scroll"];
                    const QRect port(v[0].get<int>(), v[1].get<int>(), v[2].get<int>(), v[3].get<int>());
                    QVERIFY2(inside.contains(port) && port.height() > 0,
                             qPrintable(name + QStringLiteral(": the scrolling body of 「") + label + QStringLiteral("」 is outside its window")));
                    QVERIFY2(item["in_content"].get<bool>(), qPrintable(name + QStringLiteral(": 「") + label + QStringLiteral("」 cannot be scrolled to")));
                } else {
                    QVERIFY2(inside.contains(at), qPrintable(name + QStringLiteral(": 「") + label + QStringLiteral("」 is outside its window")));
                }
                if (item["footer"].get<bool>()) {
                    QVERIFY2(item["whole"].get<bool>(), qPrintable(name + QStringLiteral(": 「") + label + QStringLiteral("」 is not wholly in sight")));
                }
            }
        }
        QCOMPARE(windows, 10);
    }

    // 試験 5: a long Japanese path, cut to fit, all of it one click away, copied whole
    void aLongPathIsShownWhole() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = long_path(dir.path() + QStringLiteral("/漫画家/作品"));
        const QString native = QDir::toNativeSeparators(path);
        QVERIFY(path.size() > 200);
        app::PathLabel label;
        label.resize(320, 24);
        label.set_path(path, QStringLiteral("保存先: "));
        label.show();
        QVERIFY(QTest::qWaitForWindowExposed(&label));
        QCOMPARE(label.path(), path);
        QVERIFY(label.shown().startsWith(QStringLiteral("保存先: ")));
        // cut in the middle: a head of the path, "…", a tail of it (the end, where the book's own name is, kept)
        const QString cut = label.shown().mid(QStringLiteral("保存先: ").size());
        QCOMPARE(cut.count(QStringLiteral("…")), 1);
        const QString head = cut.section(QStringLiteral("…"), 0, 0);
        const QString tail = cut.section(QStringLiteral("…"), 1, 1);
        QVERIFY(!head.isEmpty() && native.startsWith(head));
        QVERIFY(tail.endsWith(QStringLiteral(".genko")) && native.endsWith(tail));
        QVERIFY(label.shown().size() < path.size());
        QVERIFY(label.toolTip().contains(native));
        QSignalSpy opened(&label, &app::PathLabel::opened);
        QTest::mouseClick(&label, Qt::LeftButton);
        QCOMPARE(opened.count(), 1);
        auto* dialog = opened.at(0).at(0).value<app::PathDialog*>();
        QVERIFY(dialog != nullptr);
        QCOMPARE(dialog->shown(), native);
        QCOMPARE(dialog->findChild<QPlainTextEdit*>(QStringLiteral("fullPath"))->toPlainText(), native);
        dialog->copy_button()->click();
        QCOMPARE(QGuiApplication::clipboard()->text(), native);
        QCOMPARE(dialog->open_button()->text(), QStringLiteral("フォルダーを開く"));
        QCOMPARE(app::PathLabel::folder_of(path), path.left(path.lastIndexOf(QLatin1Char('/'))));
        dialog->close();
        // Enter on the label opens it too (the keyboard reaches it)
        label.setFocus();
        QTest::keyClick(&label, Qt::Key_Return);
        QCOMPARE(opened.count(), 2);
        opened.at(1).at(0).value<app::PathDialog*>()->close();
        // wider room: more of it shown, still the whole end
        label.resize(900, 24);
        QVERIFY(label.shown().size() > 40);
    }

    // I08: a page of any shape fits the preview, its proportions kept, again when the preview is resized
    void thePreviewFitsAnyShape() {
        app::FitPreview preview;
        QImage tall(200, 1200, QImage::Format_RGB32);
        tall.fill(Qt::white);
        preview.set_image(tall);
        preview.resize(300, 300);
        preview.show();
        QVERIFY(QTest::qWaitForWindowExposed(&preview));
        QRectF shown = preview.shown_rect();
        QVERIFY(QRectF(preview.rect()).contains(shown));
        QVERIFY(std::abs(shown.width() / shown.height() - 200.0 / 1200.0) < 0.01);
        QVERIFY(std::abs(shown.height() - (300 - 2 * 6)) < 1.0);
        preview.resize(600, 200);
        QCoreApplication::processEvents();
        shown = preview.shown_rect();
        QVERIFY(QRectF(preview.rect()).contains(shown));
        QVERIFY(std::abs(shown.width() / shown.height() - 200.0 / 1200.0) < 0.01);
        QVERIFY(std::abs(shown.height() - (200 - 2 * 6)) < 1.0);
        const QRectF wide = app::FitPreview::fit(QSizeF(3000, 1000), QSizeF(400, 400));
        QVERIFY(std::abs(wide.width() - 388) < 0.5);
        QVERIFY(std::abs(wide.height() - 388.0 / 3) < 0.5);
    }

    // the entrance for typed words: the input method's words being converted, then the words settled, handed on whole
    void typedJapaneseArrivesWhole() {
        app::ImeEntry entry;
        entry.resize(300, 32);
        entry.show();
        QVERIFY(QTest::qWaitForWindowExposed(&entry));
        QVERIFY(entry.testAttribute(Qt::WA_InputMethodEnabled));
        QVERIFY(entry.inputMethodQuery(Qt::ImEnabled).toBool());
        QSignalSpy committed(&entry, &app::ImeEntry::committed);
        QSignalSpy converting(&entry, &app::ImeEntry::preeditChanged);
        QInputMethodEvent typing(QStringLiteral("かんじ"), {});
        QCoreApplication::sendEvent(&entry, &typing);
        QCOMPARE(entry.preedit(), QStringLiteral("かんじ"));
        QCOMPARE(committed.count(), 0);
        QCOMPARE(converting.count(), 1);
        QInputMethodEvent settled;
        settled.setCommitString(QStringLiteral("漢字"));
        QCoreApplication::sendEvent(&entry, &settled);
        QCOMPARE(committed.count(), 1);
        QCOMPARE(committed.at(0).at(0).toString(), QStringLiteral("漢字"));
        QCOMPARE(entry.text(), QStringLiteral("漢字"));
        QVERIFY(entry.preedit().isEmpty());
        // a plain key types itself; a key while converting does not commit anything
        QTest::keyClick(&entry, Qt::Key_A);
        QCOMPARE(entry.text(), QStringLiteral("漢字a"));
        QCOMPARE(entry.inputMethodQuery(Qt::ImSurroundingText).toString(), QStringLiteral("漢字a"));
        entry.clear();
        QVERIFY(entry.text().isEmpty());
    }

    // the lasting cache of page pictures: the key follows the page, the least recently used go first
    void thePagePicturesAreCached() {
        QTemporaryDir dir;
        core::Document doc = new_doc(3);
        const core::Page& page = doc.page(0);
        const std::string key = app::ThumbCache::key(doc, page, 100, "proof");
        QCOMPARE(key, app::ThumbCache::key(doc, page, 100, "proof"));
        QVERIFY(key != app::ThumbCache::key(doc, page, 120, "proof"));
        QVERIFY(key != app::ThumbCache::key(doc, page, 100, "name"));
        QVERIFY(key != app::ThumbCache::key(doc, doc.page(1), 100, "proof"));
        core::Document changed = doc;
        changed.edit_page(0).frames[0].border_mm = 1.5;
        QVERIFY(key != app::ThumbCache::key(changed, changed.page(0), 100, "proof"));
        QCOMPARE(app::ThumbCache::key(changed, changed.page(1), 100, "proof"), app::ThumbCache::key(doc, doc.page(1), 100, "proof"));
        // limits: three pictures at most; the one used last stays
        const app::ThumbCache cache(path_of(dir.filePath("thumbs")), 3, 1ull << 30);
        QImage picture(40, 60, QImage::Format_RGB32);
        picture.fill(Qt::white);
        std::vector<std::string> keys;
        for (int i = 0; i < 4; ++i) keys.push_back(app::ThumbCache::key(doc, doc.page(static_cast<std::size_t>(i % 3)), 100 + i, "proof"));
        for (int i = 0; i < 3; ++i) {
            cache.put(keys[static_cast<std::size_t>(i)], picture);
            QThread::msleep(20);
        }
        QVERIFY(cache.get(keys[0]).has_value());  // (used now: the newest)
        QThread::msleep(20);
        cache.put(keys[3], picture);
        cache.trim();
        QVERIFY(cache.get(keys[0]).has_value());
        QVERIFY(!cache.get(keys[1]).has_value());  // (the least recently used went)
        QVERIFY(cache.get(keys[2]).has_value());
        QVERIFY(cache.get(keys[3]).has_value());
    }

    // the page list draws only the pictures of the rows in sight, and draws a picture once (then from the cache)
    void thePageListDrawsWhatIsSeen() {
        auto doc = std::make_shared<const core::Document>(new_doc(40));
        app::PageList list;
        list.resize(180, 360);
        list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        list.fill(doc, 0);
        QVERIFY(list.wait_pictures(60000));
        QVERIFY(list.has_picture(0));
        QVERIFY(!list.has_picture(39));
        const int drawn = list.maker().drawn();
        QVERIFY(drawn > 0 && drawn < 40);
        // the same book again (another list): the pictures come from the cache, none drawn
        app::PageList again;
        again.resize(180, 360);
        again.show();
        QVERIFY(QTest::qWaitForWindowExposed(&again));
        again.fill(doc, 0);
        QVERIFY(again.wait_pictures(60000));
        QVERIFY(again.has_picture(0));
        QCOMPARE(again.maker().drawn(), 0);
        // scrolled to the end: those rows' pictures now
        list.scrollToBottom();
        list.request_visible();
        QVERIFY(list.wait_pictures(60000));
        QVERIFY(list.has_picture(39));
    }

    // SPEC PERF-01 (必要頁の遅延読込): a book read for its first page only — the others are read while it is shown —
    // draws no picture of the pages not read yet (neither in the list nor on the canvas, nor into the cache); once read,
    // they are drawn as any page. A window opens a book so, and the person can draw on its first page at once.
    void pagesNotReadYetAreNotDrawn() {
        QTemporaryDir dir;
        const fs::path book = path_of(dir.filePath("遅れて読む.genko"));
        core::Document drawn = new_doc(3, "遅れて読む");
        for (std::size_t page = 0; page < drawn.pages.size(); ++page) {
            const double y = 30.0 + 10.0 * static_cast<double>(page);
            drawn.edit_page(page).layer_for(core::LayerRole::Ink).strokes = core::make_strokes(
                {std::make_shared<const core::Stroke>(core::coerce_stroke(Json::array({Json::array({20.0, y, 0.7}), Json::array({150.0, y + 40, 0.7})})))});
        }
        write_book(book, drawn);
        genko::storage::LoadOptions first;
        first.assets_of_page = 0;
        auto partial = std::make_shared<const core::Document>(genko::storage::load_document(book, first).document);
        QVERIFY(!partial->is_deferred(0));
        QVERIFY(partial->is_deferred(1) && partial->is_deferred(2));

        app::PageList list;
        list.resize(180, 600);
        list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        const auto kept = [&list] {
            std::size_t files = 0;
            std::error_code ec;
            if (fs::exists(list.maker().cache().root(), ec)) {
                for (const auto& entry : fs::recursive_directory_iterator(list.maker().cache().root())) files += entry.is_regular_file() ? 1 : 0;
            }
            return files;
        };
        const std::size_t kept_before = kept();
        list.fill(partial, 0);
        QVERIFY(list.wait_pictures(60000));
        QVERIFY(list.has_picture(0));
        QVERIFY(!list.has_picture(1) && !list.has_picture(2));
        QCOMPARE(list.maker().drawn(), 1);
        QCOMPARE(kept(), kept_before);  // (no picture is kept while pages are not read: one may show through another)

        app::PageCanvas canvas;
        canvas.resize(500, 600);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        canvas.set_page(partial, 1);
        QTest::qWait(300);
        QVERIFY(!canvas.renderer().any_shown());

        // read whole: the same rows and canvas draw them
        auto whole = std::make_shared<const core::Document>(genko::storage::load_document(book).document);
        list.fill(whole, 0);
        QVERIFY(list.wait_pictures(60000));
        QVERIFY(list.has_picture(1) && list.has_picture(2));
        QCOMPARE(kept(), kept_before + 3);
        canvas.set_page(whole, 1);
        QVERIFY(canvas.wait_rendered(60000));
        QVERIFY(canvas.renderer().any_shown());

        // a window: the book opened, its first page drawn on, the rest read, saved whole
        app::MainWindow window;
        window.resize(1280, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.open_project(book);
        QVERIFY(QTest::qWaitFor([&] { return window.session().path() == std::optional<fs::path>(book); }, 60000));
        app::Session& session = window.session();
        QVERIFY(session.read_in_parts());  // (opened with its first page first)
        QVERIFY(session.apply(Json::array({Json::object({{"op", "add_stroke"}, {"page", 1}, {"layer", "ink"},
                                                         {"points", Json::array({Json::array({40.0, 200.0, 0.7}), Json::array({90.0, 210.0, 0.7})})},
                                                         {"stabilize", 0}})})).applied.size() == 1);
        QVERIFY(QTest::qWaitFor([&] { return !session.loading(); }, 60000));
        QVERIFY2(session.wait_saved(30000ms), qPrintable(QStringLiteral("%1 %2 (kind %3)").arg(session.status().code, session.status().reason)
                                                            .arg(static_cast<int>(session.status().kind))));
        const core::Document disk = genko::storage::load_document(book).document;
        QCOMPARE(ink_strokes(disk, 0), std::size_t{2});
        QCOMPARE(ink_strokes(disk, 1), std::size_t{1});
        QCOMPARE(ink_strokes(disk, 2), std::size_t{1});
        window.close();
    }
};

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsSet("GENKO_LAYOUT_PROBE")) return probe(argc, argv);
    QApplication application(argc, argv);
    TestGuiLayout test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "test_gui_layout.moc"
