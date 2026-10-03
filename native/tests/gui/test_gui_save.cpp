// Saving, failing to save, and closing in the window (M2-G1 試験 3; ACCEPTANCE.md AC-SAVE 2, 3, 4, 6, 8, 10).
//
//   2  a folder that cannot be written, a full disk (fault injection: ENOSPC) and a failed replace of project.json are
//      shown — why, where, when the book was last saved, and that the work is not in the book — and neither Ctrl+W,
//      nor switching books, nor quitting Genko closes it on its own.
//   3  キャンセル keeps the work; 保存して閉じる closes only after a save that succeeded; 別の場所に保存して閉じる closes
//      once the copy is written, and the copy reads back.
//   4  only the explicit 保存せずに閉じる (and its second question) throws work away: Enter on the question, and Enter on
//      the second one, keep it.
//   6  changes and an undo made while a save runs stay unsaved until they are saved (the first save's success does not
//      clear them); closing the window while a save runs waits for it, or asks.
//   8  a recovery point is written when the book cannot be, and offered (and adopted as a new revision) when the book is
//      opened again; when the recovery area cannot be written either, the state says the work is nowhere but in memory.
//   10 a failed commit (after project.json was replaced) is retried in the same process, then undone, and another
//      process's save is taken in on top: each change is in the book once.

#include <QtTest>

#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryFile>
#include <QTimer>

#include <memory>
#include <optional>

#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/icons.hpp"
#include "app/inject.hpp"
#include "app/main_window.hpp"
#include "app/path_label.hpp"
#include "app/save_status.hpp"
#include "app/session.hpp"
#include "app/theme.hpp"
#include "app/templates.hpp"
#include "core/command_bus.hpp"
#include "gui_support.hpp"
#include "storage/journal.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"

using namespace gui_test;
using genko::app::CloseGuard;
using genko::app::MainWindow;
using genko::app::SaveKind;
using genko::app::Session;
namespace inject = genko::app::inject;
namespace core = genko::core;

namespace {

void make_writable(const fs::path& dir, bool writable) {
    std::error_code ec;
    const auto mode = writable ? fs::perms::owner_all : (fs::perms::owner_read | fs::perms::owner_exec);
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) {
            fs::permissions(it->path(), mode, ec);
        } else {
            fs::permissions(it->path(), writable ? (fs::perms::owner_read | fs::perms::owner_write) : fs::perms::owner_read, ec);
        }
    }
    fs::permissions(dir, mode, ec);
}

struct Asked {
    QString title;           // the book's title in the question
    QStringList buttons;     // the four buttons' words
    bool cancel_is_default;
};

// A book in a window, its session saving 50 ms after a change, the questions answered by the test.
struct Desk {
    QTemporaryDir dir;
    fs::path book;
    fs::path recovery;
    std::shared_ptr<Session> session;
    std::unique_ptr<Answers> answers = std::make_unique<Answers>();
    std::unique_ptr<MainWindow> window;
    std::vector<Asked> asked;
    QString close_answer = QStringLiteral("cancel");  // what the close question is answered: cancel | save | save_as | discard
    bool discard_sure = false;                         // the second question before throwing work away
    QString save_as_target;

    explicit Desk(std::optional<fs::path> recovery_root = std::nullopt) {
        book = path_of(dir.filePath("book.genko"));
        make_book(book, 2);
        // (the app's own recovery folder in the test's config folder, where opening a book looks: a folder per book id)
        recovery = recovery_root ? *recovery_root : genko::app::recovery_root();
        session = Session::open(book, quick(recovery));
        window = std::make_unique<MainWindow>(session);
        window->resize(1200, 860);
        window->show();
        answers->responder->exec = [this](QDialog* dialog) {
            auto* guard = qobject_cast<CloseGuard*>(dialog);
            if (guard == nullptr) return 0;
            asked.push_back(Asked{guard->findChild<QLabel*>() != nullptr ? guard->findChild<QLabel*>()->text() : QString(),
                                  {guard->save_button()->text(), guard->save_as_button()->text(), guard->discard_button()->text(),
                                   guard->cancel_button()->text()},
                                  guard->cancel_button()->isDefault()});
            QPushButton* button = close_answer == QLatin1String("save")      ? guard->save_button()
                                  : close_answer == QLatin1String("save_as") ? guard->save_as_button()
                                  : close_answer == QLatin1String("discard") ? guard->discard_button()
                                                                             : guard->cancel_button();
            button->click();
            return guard->result();
        };
        answers->responder->question = [this](const QString& title, const QString&) {
            return title == QStringLiteral("変更を捨てる") ? discard_sure : false;
        };
        answers->responder->save_path = [this](const QString&, const QString&) { return save_as_target; };
    }
    ~Desk() {
        window.reset();
        session.reset();
        answers.reset();
        make_writable(book, true);
    }
    void draw(double dy = 0) {
        window->choose_tool(QStringLiteral("pen"));
        const QPointF c = window->canvas()->seen_mm().center() + QPointF(0, dy);
        inject::mouse_stroke(window->canvas(), {c, c + QPointF(12, 4), c + QPointF(24, 6)});
    }
    SaveKind kind() const { return window->session().status().kind; }
    QString state_text() const { return window->save_label()->state()->text(); }
    QString failure_text() const { return window->failure_bar()->words()->text(); }
    bool wait_kind(SaveKind wanted, int ms = 15000) const { return wait_for([&] { return kind() == wanted; }, ms); }
    bool wait_failed(int ms = 15000) const {
        return wait_for([&] { return kind() == SaveKind::Failed || kind() == SaveKind::RecoveryOnly; }, ms);
    }
};

}  // namespace

class TestGuiSave : public QObject {
    Q_OBJECT

private slots:
    void modalActionsKeepTheirOriginalTarget_data() {
        QTest::addColumn<QString>("action_name");
        QTest::addColumn<bool>("external_change");
        for (const auto& name : {"act_border_colour", "act_border", "act_corner", "act_save_template", "act_del_page"}) {
            QTest::newRow((QString::fromLatin1(name) + "-normal").toUtf8().constData()) << QString::fromLatin1(name) << false;
            QTest::newRow((QString::fromLatin1(name) + "-external-delete").toUtf8().constData()) << QString::fromLatin1(name) << true;
        }
    }
    void modalActionsKeepTheirOriginalTarget() {
        QFETCH(QString, action_name);
        QFETCH(bool, external_change);
        QTemporaryDir tmp;
        const auto book = path_of(tmp.filePath("modal.genko"));
        auto doc = new_doc(3);
        doc.edit_page(0).selected_frame_id = doc.page(0).frames.front().id;
        write_book(book, doc);
        auto options = quick(path_of(tmp.filePath("recovery")));
        options.autosave = false;
        auto session = Session::open(book, options);
        Answers answers;
        MainWindow window(session);
        window.show();
        window.choose_tool(QStringLiteral("select"));
        window.canvas()->fit_page();
        const auto rect = window.current_page()->frames.front().rect;
        inject::click_mm(window.canvas(), QPointF(rect.x.value() + rect.width.value() / 2,
                                                 rect.y.value() + rect.height.value() / 2));
        QVERIFY(window.current_page()->selected_frame_id.is_string());
        // Selection is a real Session edit. Commit it before the external writer
        // so this fixture exercises clean rebase during a modal, not S03 conflict.
        session->save_now();
        QVERIFY(session->wait_saved(5s));
        genko::app::DocPtr after;
        std::uint64_t generation = 0;
        const auto mine = genko::app::templates::mine().size();
        const auto name = QStringLiteral("modal-%1-%2").arg(action_name).arg(external_change);
        const auto during_dialog = [&] {
            if (external_change) {
                genko::storage::ProjectLock lock(book, "ai:other");
                lock.try_acquire();
                const auto loaded = genko::storage::load_document(book);
                const auto result = core::CommandBus().apply(loaded.document,
                    Json::array({Json::object({{"op", "delete_page"}, {"page", 1}})}), core::Actor("ai:other"));
                genko::storage::SaveRequest request;
                request.actor = "ai:other";
                request.base_revision = loaded.document.revision;
                request.ops = result.journal_ops;
                genko::storage::Saver(lock).save(result.doc, request);
                lock.release();
                session->check_outside();
                QVERIFY(session->wait_idle(5s));
                QCOMPARE(session->document().pages.size(), std::size_t{2});
            }
            after = session->snapshot();
            generation = session->generation();
        };
        answers.responder->colour = [&](const QColor&, const QString&) {
            during_dialog();
            return std::optional<QColor>(QColor(10, 30, 70));
        };
        answers.responder->get_double = [&](const QString&, const QString&, double, double, double, int) {
            during_dialog();
            return std::optional<double>(1.3);
        };
        answers.responder->get_text = [&](const QString&, const QString&, const QString&) {
            during_dialog();
            return std::optional<QString>(name);
        };
        answers.responder->question = [&](const QString&, const QString&) {
            during_dialog();
            return true;
        };
        window.action(action_name)->trigger();
        QVERIFY(after != nullptr);
        if (external_change) {
            QCOMPARE(session->snapshot().get(), after.get());
            QCOMPARE(session->generation(), generation);
            QCOMPARE(genko::app::templates::mine().size(), mine);
            QVERIFY(window.last_notice().contains(QStringLiteral("変更")));
        } else if (action_name == QStringLiteral("act_save_template")) {
            QCOMPARE(genko::app::templates::mine().size(), mine + 1);
        } else {
            QVERIFY(session->snapshot().get() != after.get());
            QVERIFY(session->generation() > generation);
        }
    }

    void initTestCase() {
        config_folder();
        qRegisterMetaType<genko::app::StrokeInput>();
        qRegisterMetaType<genko::app::BookChange>();
        genko::app::icons::init_resources();
        genko::app::theme::apply(qApp);
        if (!genko::storage::fault::compiled_in()) QSKIP("this build has no fault injection (a release configuration)");
    }

    void nativeWindowSaveFailureCancelAndReopen() {
        QTemporaryDir tmp;
        const auto blocker = path_of(tmp.filePath("recovery-blocker"));
        genko::storage::write_atomic(blocker, "blocked");
        Desk desk(blocker);
        const QString proof = qEnvironmentVariable("GENKO_GUI_PROOF_DIR");
        const auto capture = [&](QWidget* widget, const QString& name) {
            if (proof.isEmpty()) return;
            QVERIFY(QDir().mkpath(proof));
            QApplication::processEvents();
            QVERIFY(widget->grab().save(proof + QLatin1Char('/') + name + QStringLiteral(".png")));
        };
        qInfo() << "native proof Qt platform:" << QGuiApplication::platformName();
        if (!proof.isEmpty()) QCOMPARE(QGuiApplication::platformName(), QStringLiteral("xcb"));
        desk.draw();
        QVERIFY(desk.session->wait_saved(5s));
        QCOMPARE(desk.kind(), SaveKind::Saved);
        capture(desk.window.get(), QStringLiteral("01-saved"));
        make_writable(desk.book, false);
        desk.draw(8);
        QVERIFY(desk.wait_kind(SaveKind::Failed));
        QVERIFY(desk.session->status().nowhere);
        QVERIFY(desk.window->failure_bar()->isVisible());
        capture(desk.window.get(), QStringLiteral("02-failed-memory-only"));
        const auto before = desk.session->snapshot();
        desk.answers->responder->exec = [&](QDialog* dialog) {
            auto* guard = qobject_cast<CloseGuard*>(dialog);
            if (!guard) return 0;
            QTimer::singleShot(120, guard, [&, guard] {
                capture(guard, QStringLiteral("03-close-cancel-default"));
                QTest::mouseClick(guard->cancel_button(), Qt::LeftButton);
            });
            return guard->exec();
        };
        QVERIFY(!desk.window->close());
        QCOMPARE(desk.session->snapshot().get(), before.get());
        QVERIFY(desk.window->isVisible());
        capture(desk.window.get(), QStringLiteral("04-cancel-preserved"));
        make_writable(desk.book, true);
        desk.session->save_now();
        QVERIFY(desk.session->wait_saved(5s));
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{2});
        const auto reopened = Session::open(desk.book, quick(desk.recovery));
        MainWindow fresh(reopened);
        fresh.resize(1200, 860);
        fresh.show();
        QCOMPARE(ink_strokes(fresh.book()), std::size_t{2});
        QCOMPARE(fresh.session().status().kind, SaveKind::Saved);
        capture(&fresh, QStringLiteral("05-reopened-saved"));
        QVERIFY(fresh.close());
        QVERIFY(desk.window->close());
    }

    void saveAsDuringCloseNeverSavesAReplacementSession() {
        Desk desk;
        make_writable(desk.book, false);
        desk.draw();
        QVERIFY(desk.wait_failed());
        const auto first = desk.session;
        const auto before = first->snapshot();
        const auto copy = path_of(desk.dir.filePath("requested-for-first.genko"));
        const auto other = path_of(desk.dir.filePath("other.genko"));
        make_book(other);
        const auto second = Session::open(other, quick(desk.recovery));
        const auto second_before = second->snapshot();
        desk.close_answer = QStringLiteral("save_as");
        bool replaced = false;
        desk.answers->responder->save_path = [&](const QString&, const QString&) {
            // Equivalent adoption point to an open_project finished notification.
            desk.close_answer = QStringLiteral("discard");
            desk.discard_sure = true;
            desk.window->add_document(second);
            replaced = &desk.window->session() == second.get();
            return qpath(copy);
        };
        QVERIFY(!desk.window->close());
        QVERIFY(replaced);
        QCOMPARE(first->snapshot().get(), before.get());
        QVERIFY(first->unsaved());
        QCOMPARE(second->snapshot().get(), second_before.get());
        QVERIFY(!fs::exists(copy));
        QVERIFY(desk.window->isVisible());
        QVERIFY(!first->wait_saved(100ms));
    }

    void dirtyBookSwitchingUsesTheSameGuard_data() {
        QTest::addColumn<bool>("existing_tab");
        QTest::newRow("add-new-book") << false;
        QTest::newRow("switch-existing-tab") << true;
    }
    void dirtyBookSwitchingUsesTheSameGuard() {
        QFETCH(bool, existing_tab);
        Answers answers;
        auto options = quick(genko::app::recovery_root());
        options.autosave = false;
        auto first = std::make_shared<Session>(new_doc(1, "原稿1"), std::nullopt, options);
        auto second = std::make_shared<Session>(new_doc(1, "原稿2"), std::nullopt, options);
        MainWindow window(first);
        first->apply(Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "未保存1"}})}));
        if (existing_tab) {
            answers.responder->question = [](const QString&, const QString&) { return true; };
            answers.responder->exec = [](QDialog* dialog) {
                auto* guard = qobject_cast<CloseGuard*>(dialog);
                if (!guard) return 0;
                guard->discard_button()->click();
                return guard->result();
            };
            window.add_document(second);
            second->apply(Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "未保存2"}})}));
        }
        const auto active = window.documents()[static_cast<std::size_t>(window.current_document())].session;
        const auto before = active->snapshot();
        const auto index = window.current_document();
        const auto count = window.documents().size();
        int prompts = 0;
        answers.responder->exec = [&](QDialog* dialog) {
            auto* guard = qobject_cast<CloseGuard*>(dialog);
            if (!guard) return 0;
            ++prompts;
            guard->cancel_button()->click();
            return guard->result();
        };
        if (existing_tab) window.switch_document(0);
        else window.add_document(second);
        QCOMPARE(prompts, 1);
        QCOMPARE(window.current_document(), index);
        QCOMPARE(window.documents().size(), count);
        QCOMPARE(window.session().snapshot().get(), before.get());
        QVERIFY(active->unsaved());
        // Explicit permission to switch keeps the original session and its edits.
        answers.responder->question = [](const QString&, const QString&) { return true; };
        answers.responder->exec = [](QDialog* dialog) {
            auto* guard = qobject_cast<CloseGuard*>(dialog);
            if (!guard) return 0;
            guard->discard_button()->click();
            return guard->result();
        };
        if (existing_tab) window.switch_document(0);
        else window.add_document(second);
        QCOMPARE(window.current_document(), existing_tab ? 0 : 1);
        QCOMPARE(active->snapshot().get(), before.get());
        active->apply(Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "後でも編集可能"}})}));
        QCOMPARE(active->document().page(0).note, std::string("後でも編集可能"));
    }

    void cancellingLaterBookDoesNotDiscardEarlierBook() {
        Answers answers;
        auto options = quick(genko::app::recovery_root());
        options.autosave = false;
        auto first = std::make_shared<Session>(new_doc(1, "先の原稿"), std::nullopt, options);
        auto second = std::make_shared<Session>(new_doc(1, "後の原稿"), std::nullopt, options);
        MainWindow window(first);
        first->apply(Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "先の未保存"}})}));
        answers.responder->question = [](const QString&, const QString&) { return true; };
        answers.responder->exec = [](QDialog* dialog) {
            auto* guard = qobject_cast<CloseGuard*>(dialog);
            if (!guard) return 0;
            guard->discard_button()->click();
            return guard->result();
        };
        window.add_document(second);
        QCOMPARE(window.documents().size(), std::size_t{2});
        second->apply(Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "後の未保存"}})}));
        window.show();
        const auto first_before = first->snapshot();
        const auto second_before = second->snapshot();
        const auto waiting = first->waiting();
        int prompts = 0;
        answers.responder->question = [](const QString& title, const QString&) { return title == QStringLiteral("変更を捨てる"); };
        answers.responder->exec = [&](QDialog* dialog) {
            auto* guard = qobject_cast<CloseGuard*>(dialog);
            if (!guard) return 0;
            if (++prompts == 1) guard->discard_button()->click();
            else guard->cancel_button()->click();
            return guard->result();
        };
        QVERIFY(!window.close());
        QCOMPARE(prompts, 2);
        QVERIFY(!window.closed());
        QVERIFY(window.isVisible());
        QCOMPARE(window.documents().size(), std::size_t{2});
        QCOMPARE(first->snapshot().get(), first_before.get());
        QCOMPARE(second->snapshot().get(), second_before.get());
        QCOMPARE(first->waiting(), waiting);
        try {
            first->apply(Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "続けて編集"}})}));
        } catch (const core::Error&) {
            QFAIL("Cancelling quit left the earlier book as a closed session");
        }
        QCOMPARE(first->document().page(0).note, std::string("続けて編集"));
        // All-confirmed discard still closes both books.
        answers.responder->exec = [](QDialog* dialog) {
            auto* guard = qobject_cast<CloseGuard*>(dialog);
            if (!guard) return 0;
            guard->discard_button()->click();
            return guard->result();
        };
        QVERIFY(window.close());
        QVERIFY(window.closed());
    }

    void untitledRecoveryFailureIsPersistentInTheWindow() {
        QTemporaryDir tmp;
        const auto recovery = path_of(tmp.filePath("blocked"));
        genko::storage::write_atomic(recovery, "blocked");
        auto options = quick(recovery);
        options.autosave = false;
        auto session = std::make_shared<Session>(new_doc(1, "無題"), std::nullopt, options);
        Answers answers;
        MainWindow window(session);
        window.show();
        session->apply(Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "メモリにだけある"}})}));
        session->save_now();
        QVERIFY(session->wait_idle(5s));
        QCOMPARE(session->status().kind, SaveKind::Failed);
        QVERIFY(window.failure_bar()->isVisible());
        QVERIFY(window.failure_bar()->words()->text().contains(QStringLiteral("まだどこにも保存されていません")));
        const auto words = window.failure_bar()->words()->text();
        QTest::qWait(150);
        QCOMPARE(window.failure_bar()->words()->text(), words);
        QVERIFY(!window.close());
        QCOMPARE(session->document().page(0).note, std::string("メモリにだけある"));
        fs::remove(recovery);
        window.failure_bar()->retry_button()->click();
        QVERIFY(session->wait_idle(5s));
        QCOMPARE(session->status().kind, SaveKind::RecoveryOnly);
        QVERIFY(window.failure_bar()->words()->text().contains(QStringLiteral("変更は復旧用のコピーに書きました")));
        QVERIFY(!window.failure_bar()->words()->text().contains(QStringLiteral("まだどこにも保存されていません")));
    }

    void corruptRecoveryIsReportedWithoutReplacingCurrentBook() {
        QTemporaryDir tmp;
        const fs::path book = path_of(tmp.filePath("broken-recovery.genko"));
        make_book(book, 2);
        auto options = quick(genko::app::recovery_root());
        options.autosave = false;
        fs::path state;
        {
            auto original = Session::open(book, options);
            original->apply(Json::array({Json::object({{"op", "set_note"}, {"page", 1}, {"note", "復旧にだけある内容"}})}));
            original->write_recovery_copy();
            QVERIFY(original->wait_idle(10000ms));
            state = options.recovery_root / original->document().book_id / "state.json";
        }
        Answers answers;
        int warnings = 0;
        QString warning_text;
        answers.responder->warning = [&](const QString&, const QString& text) { ++warnings; warning_text = text; };
        answers.responder->question = [&](const QString& title, const QString&) {
            if (title != QStringLiteral("復旧用のコピー")) return false;
            // The copy changes while the real GUI question is being answered.
            genko::storage::write_atomic(state, "{broken json");
            return true;
        };
        MainWindow window;
        window.show();
        const auto before = window.session().snapshot();
        const auto count = window.documents().size();
        const auto project = genko::storage::read_file(book / "project.json");
        window.open_project(book);
        QVERIFY(wait_for([&] { return warnings != 0; }));
        QCOMPARE(warnings, 1);
        QVERIFY(warning_text.contains(QStringLiteral("復旧")));
        QCOMPARE(window.documents().size(), count);
        QVERIFY(window.session().snapshot() == before);
        QCOMPARE(genko::storage::read_file(book / "project.json"), project);
        QCOMPARE(genko::storage::read_file(state), std::string("{broken json"));
    }

    // AC-SAVE 2: a full disk
    void aFullDiskIsShownAndNothingClosesOnItsOwn() {
        Desk desk;
        const fs::path other = path_of(desk.dir.filePath("other.genko"));
        make_book(other, 1);  // (another book, for switching to below)
        desk.draw();
        QVERIFY(desk.wait_kind(SaveKind::Saved));
        QVERIFY(desk.state_text().startsWith(QStringLiteral("保存済み・")));
        QVERIFY(desk.state_text().endsWith(QStringLiteral("（世代 2）")));
        const std::size_t strokes = ink_strokes(desk.window->book());
        // (the book's writes fail with ENOSPC until 再試行 below; the recovery area is written)
        auto full = std::make_unique<Fault>("assets:fail");
        desk.draw(10);
        QVERIFY(desk.wait_kind(SaveKind::RecoveryOnly));
        // why, where, the last save, and that the work is only in the recovery copy
        QCOMPARE(desk.state_text(), QStringLiteral("復旧用コピーのみ（原稿には未保存）"));
        QVERIFY(desk.window->failure_bar()->isVisible());
        QVERIFY(desk.failure_text().startsWith(QStringLiteral("⚠ 原稿に保存できませんでした: ディスクの空きがありません。")));
        QVERIFY(desk.failure_text().contains(QStringLiteral("最後に保存できたのは ")));
        QVERIFY(desk.failure_text().contains(QStringLiteral("（世代 2）です。")));
        QVERIFY(desk.failure_text().contains(QStringLiteral("変更は復旧用のコピーに書きました")));
        QCOMPARE(desk.window->failure_bar()->where()->path(), qpath(desk.book));
        QCOMPARE(desk.window->failure_bar()->retry_button()->text(), QStringLiteral("再試行"));
        QCOMPARE(desk.window->failure_bar()->save_as_button()->text(), QStringLiteral("別の場所に保存…"));
        QCOMPARE(desk.window->failure_bar()->recovery_button()->text(), QStringLiteral("復旧用コピーを作る"));
        QCOMPARE(ink_strokes(desk.window->book()), strokes + 1);
        QCOMPARE(ink_strokes(read_book(desk.book)), strokes);
        // Ctrl+W: asked, キャンセル keeps it
        desk.window->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(desk.window.get()));
        QVERIFY(desk.window->action(QStringLiteral("act_close"))->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+W"))));
        QTest::keyClick(desk.window.get(), Qt::Key_W, Qt::ControlModifier);
        QCOMPARE(desk.asked.size(), std::size_t{1});
        QCOMPARE(desk.asked[0].buttons, (QStringList{"保存して閉じる", "別の場所に保存して閉じる", "保存せずに閉じる…", "キャンセル"}));
        QVERIFY(desk.asked[0].cancel_is_default);
        QVERIFY(!desk.window->closed());
        QVERIFY(desk.window->isVisible());
        QCOMPARE(ink_strokes(desk.window->book()), strokes + 1);
        QCOMPARE(desk.kind(), SaveKind::RecoveryOnly);
        // switching to another book: asked (nothing is thrown away), キャンセル stays
        desk.window->add_document(Session::open(other, quick(desk.recovery)));
        QCOMPARE(desk.window->current_document(), 0);
        QCOMPARE(desk.window->documents().size(), std::size_t{1});
        QCOMPARE(desk.asked.size(), std::size_t{2});
        QCOMPARE(desk.asked[1].buttons[2], QStringLiteral("そのまま切り替える"));
        // Explicit continuation opens the other tab without throwing away the failed book.
        desk.close_answer = QStringLiteral("discard");
        desk.discard_sure = true;
        desk.window->add_document(Session::open(other, quick(desk.recovery)));
        QCOMPARE(desk.window->current_document(), 1);
        desk.close_answer = QStringLiteral("cancel");
        desk.window->switch_document(0);
        QCOMPARE(desk.window->current_document(), 0);
        desk.window->next_document(1);
        QCOMPARE(desk.asked.size(), std::size_t{4});
        QCOMPARE(desk.asked[3].buttons[2], QStringLiteral("そのまま切り替える"));
        QCOMPARE(desk.window->current_document(), 0);
        // Genko を終わる: asked, キャンセル keeps the window
        desk.window->action(QStringLiteral("act_quit"))->trigger();
        QCOMPARE(desk.asked.size(), std::size_t{5});
        QVERIFY(!desk.window->closed());
        QVERIFY(desk.window->isVisible());
        // and once the disk has room again, 再試行 writes it
        full.reset();
        desk.window->failure_bar()->retry_button()->click();
        QVERIFY(desk.wait_kind(SaveKind::Saved));
        QVERIFY(!desk.window->failure_bar()->isVisible());
        QCOMPARE(ink_strokes(read_book(desk.book)), strokes + 1);
    }

    // AC-SAVE 2: a folder that cannot be written
    void anUnwritableFolderIsShown() {
        Desk desk;
        make_writable(desk.book, false);
        desk.draw();
        QVERIFY(desk.wait_failed());
        QVERIFY2(desk.failure_text().contains(QStringLiteral("この場所には書き込めません（読み取り専用か、書き込みが許されていません）")),
                 qPrintable(desk.failure_text()));
        QVERIFY(desk.failure_text().contains(QStringLiteral("この原稿を開いてからは、まだ保存できていません。")));
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{0});
        make_writable(desk.book, true);
        desk.window->failure_bar()->retry_button()->click();
        QVERIFY(desk.wait_kind(SaveKind::Saved));
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{1});
    }

    // AC-SAVE 2: project.json cannot be replaced
    void aFailedReplaceIsShown() {
        Desk desk;
        {
            Fault replace("project:fail");
            desk.draw();
            QVERIFY(desk.wait_failed());
        }
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{0});
        QVERIFY(desk.failure_text().startsWith(QStringLiteral("⚠ 原稿に保存できませんでした: ")));
        QVERIFY(desk.state_text() == QStringLiteral("保存失敗") || desk.state_text() == QStringLiteral("復旧用コピーのみ（原稿には未保存）"));
        desk.window->failure_bar()->retry_button()->click();
        QVERIFY(desk.wait_kind(SaveKind::Saved));
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{1});
    }

    // AC-SAVE 3: 保存して閉じる only after a save that succeeded; 別の場所に保存して閉じる once the copy is written
    void savingBeforeClosing() {
        Desk desk;
        make_writable(desk.book, false);
        desk.draw();
        desk.draw(8);
        QVERIFY(desk.wait_failed());
        desk.close_answer = QStringLiteral("save");
        QVERIFY(!desk.window->close());
        QVERIFY(!desk.window->closed());
        QVERIFY(desk.window->last_error().startsWith(QStringLiteral("保存できなかったので、閉じずに残しました: ")));
        QCOMPARE(ink_strokes(desk.window->book()), std::size_t{2});
        // a copy that cannot be written either (a file where its folder would go): still open
        const fs::path blocker = path_of(desk.dir.filePath("blocker"));
        QVERIFY(QFile(qpath(blocker)).open(QIODevice::WriteOnly));
        desk.close_answer = QStringLiteral("save_as");
        desk.save_as_target = qpath(blocker / "copy");
        QVERIFY(!desk.window->close());
        QVERIFY(!desk.window->closed());
        // a copy in a place that can be written: closed, and the copy has the work
        desk.save_as_target = desk.dir.filePath("写し");
        QVERIFY(desk.window->close());
        QVERIFY(desk.window->closed());
        const fs::path copy = path_of(desk.dir.filePath("写し.genko"));
        const core::Document copied = read_book(copy);
        QCOMPARE(ink_strokes(copied), std::size_t{2});
        QVERIFY(copied.book_id != read_book(desk.book).book_id);
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{0});
    }

    // AC-SAVE 4: Enter on the question never throws work away
    void enterOnTheQuestionKeepsTheWork() {
        genko::app::SaveStatus status;
        status.kind = SaveKind::Failed;
        status.reason = QStringLiteral("No space left on device");
        for (const int key : {static_cast<int>(Qt::Key_Return), static_cast<int>(Qt::Key_Enter), static_cast<int>(Qt::Key_Escape)}) {
            CloseGuard guard(nullptr, status, QStringLiteral("試し 第1話"));
            guard.show();
            QVERIFY(QTest::qWaitForWindowExposed(&guard));
            QVERIFY(guard.cancel_button()->isDefault());
            QTest::keyClick(&guard, static_cast<Qt::Key>(key));
            QCOMPARE(guard.choice(), CloseGuard::Choice::Cancel);
            QCOMPARE(guard.result(), static_cast<int>(QDialog::Rejected));
        }
        // the second question before throwing work away: Enter goes back (the real message box, no answers given)
        genko::app::ask::set_responder(nullptr);
        QTimer::singleShot(200, [] {
            if (QWidget* box = QApplication::activeModalWidget()) QTest::keyClick(box, Qt::Key_Return);
        });
        QVERIFY(!CloseGuard::confirm_discard(nullptr, QStringLiteral("試し 第1話")));
    }

    // AC-SAVE 4: 保存せずに閉じる, and only it, throws the work away (after the second question)
    void onlyAnExplicitDiscardThrowsAway() {
        Desk desk;
        {
            Fault full("assets:fail");
            desk.draw();
            QVERIFY(desk.wait_failed());
        }
        QVERIFY(fs::exists(desk.recovery));
        desk.close_answer = QStringLiteral("discard");
        desk.discard_sure = false;  // 戻る
        QVERIFY(!desk.window->close());
        QVERIFY(!desk.window->closed());
        QCOMPARE(ink_strokes(desk.window->book()), std::size_t{1});
        desk.discard_sure = true;
        QVERIFY(desk.window->close());
        QVERIFY(desk.window->closed());
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{0});
        // (the recovery copy of the thrown-away work goes too)
        QVERIFY(desk.session->wait_idle(10s));
        QVERIFY(!genko::app::find_recovery(desk.recovery, read_book(desk.book).book_id).has_value());
    }

    // AC-SAVE 6: changes and an undo made while a save waits; the first save's success leaves them unsaved
    void editsDuringASaveStayUnsaved() {
        Desk desk;
        // every time the state says 保存済み, the book on disk has what the window shows
        QStringList wrong;
        QObject::connect(desk.session.get(), &Session::statusChanged, desk.window.get(), [&] {
            if (desk.session->status().kind != SaveKind::Saved) return;
            const std::size_t disk = ink_strokes(read_book(desk.book));
            if (disk != ink_strokes(desk.session->document())) wrong << QStringLiteral("保存済み with %1 on disk").arg(disk);
        });
        auto holder = std::make_unique<genko::storage::ProjectLock>(desk.book, "test-holder");
        holder->try_acquire();
        QVERIFY(holder->held());
        desk.draw();
        QVERIFY(wait_for([&] { return desk.kind() == SaveKind::Saving; }));
        desk.draw(8);
        desk.draw(16);
        QVERIFY(desk.session->status().newer_waiting);
        QVERIFY(desk.state_text().startsWith(QStringLiteral("保存中…")));
        desk.window->action(QStringLiteral("act_undo"))->trigger();  // (an undo while the save waits)
        QCOMPARE(ink_strokes(desk.window->book()), std::size_t{2});
        holder.reset();
        QVERIFY(desk.wait_kind(SaveKind::Saved));
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{2});
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(QStringLiteral("; "))));
    }

    // AC-SAVE 6: closing while a save runs — it is waited for
    void closingWaitsForASaveThatEnds() {
        Desk desk;
        auto holder = std::make_unique<genko::storage::ProjectLock>(desk.book, "test-holder");
        holder->try_acquire();
        QVERIFY(holder->held());
        desk.draw();
        QVERIFY(wait_for([&] { return desk.kind() == SaveKind::Saving; }));
        QTimer::singleShot(500, [&] { holder.reset(); });
        QVERIFY(desk.window->close());
        QVERIFY(desk.window->closed());
        QVERIFY(desk.asked.empty());
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{1});
    }

    // AC-SAVE 6: closing while a save runs — one that does not end soon is asked about
    void closingAsksWhenASaveDoesNotEnd() {
        Desk desk;
        auto holder = std::make_unique<genko::storage::ProjectLock>(desk.book, "test-holder");
        holder->try_acquire();
        QVERIFY(holder->held());
        desk.draw();
        QVERIFY(wait_for([&] { return desk.kind() == SaveKind::Saving; }));
        QVERIFY(!desk.window->close());  // (キャンセル)
        QCOMPARE(desk.asked.size(), std::size_t{1});
        QVERIFY(!desk.window->closed());
        holder.reset();
        QVERIFY(desk.wait_kind(SaveKind::Saved, 20000));
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{1});
        QVERIFY(desk.window->close());
    }

    // AC-SAVE 8: a recovery point when the book cannot be written; offered and adopted when it is opened again
    void theRecoveryPointBringsTheWorkBack() {
        Desk desk;
        const fs::path book = desk.book;
        const fs::path recovery = desk.recovery;
        desk.draw();
        QVERIFY(desk.wait_kind(SaveKind::Saved));
        {
            make_writable(book, false);  // (the book's folder cannot be written; the recovery area can)
            desk.draw(8);
            desk.draw(16);
            QVERIFY(desk.wait_kind(SaveKind::RecoveryOnly));
            QVERIFY(desk.failure_text().contains(QStringLiteral("変更は復旧用のコピーに書きました")));
            QVERIFY(desk.failure_text().contains(QStringLiteral("原稿そのものには、まだ入っていません。")));
            // the book on disk is the last save that succeeded (one line); the recovery copy has three
            QCOMPARE(ink_strokes(read_book(book)), std::size_t{1});
            // Genko stops here (no question answered: the window simply goes)
            desk.window.reset();
            desk.session.reset();
            make_writable(book, true);
        }
        const auto point = genko::app::find_recovery(recovery, read_book(book).book_id);
        QVERIFY(point.has_value());
        // opened again: offered (時刻・ページ数), not adopted without asking
        const Session::Opened opened = Session::read(book, quick(recovery));
        QVERIFY(opened.offer.has_value());
        QCOMPARE(opened.offer->pages, std::size_t{2});
        QCOMPARE(ink_strokes(opened.doc), std::size_t{1});
        Answers answers;
        QStringList questions;
        answers.responder->question = [&](const QString& title, const QString& text) {
            questions << title + QStringLiteral(": ") + text;
            return title == QStringLiteral("復旧用のコピー");
        };
        MainWindow window;
        window.show();
        window.open_project(book);
        QVERIFY(wait_for([&] { return window.session().path().has_value(); }));
        QCOMPARE(questions.size(), 1);
        QVERIFY(questions[0].contains(QStringLiteral("保存できなかった変更の復旧用コピーがあります")));
        QCOMPARE(ink_strokes(window.book()), std::size_t{3});
        QVERIFY(wait_for([&] { return window.session().status().kind == SaveKind::Saved; }, 20000));
        QCOMPARE(ink_strokes(read_book(book)), std::size_t{3});
        QVERIFY(!genko::app::find_recovery(recovery, read_book(book).book_id).has_value());
    }

    // AC-SAVE 8: declined, the book opens as it was last saved
    void aDeclinedRecoveryOpensTheLastSave() {
        Desk desk;
        make_writable(desk.book, false);
        desk.draw();
        QVERIFY(desk.wait_kind(SaveKind::RecoveryOnly));
        desk.window.reset();
        desk.session.reset();
        make_writable(desk.book, true);
        QVERIFY(genko::app::find_recovery(desk.recovery, read_book(desk.book).book_id).has_value());
        Answers answers;  // (every question answered no)
        MainWindow window;
        window.show();
        window.open_project(desk.book);
        QVERIFY(wait_for([&] { return window.session().path().has_value(); }));
        QCOMPARE(ink_strokes(window.book()), std::size_t{0});
        QCOMPARE(window.session().status().kind, SaveKind::Saved);
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{0});
    }

    // AC-SAVE 8: a commit that failed after project.json was replaced: opened again, the book is a consistent one (the
    // old one or the new one, as the journal's repair settles it), the same on disk as in the window
    void aCommitThatFailedIsSettledOnOpening() {
        Desk desk;
        {
            Fault commit("commit:fail");
            desk.draw();
            QVERIFY(desk.wait_failed());
        }
        desk.window.reset();
        desk.session.reset();
        Answers answers;  // (every question answered no)
        MainWindow window;
        window.show();
        window.open_project(desk.book);
        QVERIFY(wait_for([&] { return window.session().path().has_value(); }));
        const std::size_t opened = ink_strokes(window.book());
        QVERIFY(opened == 0 || opened == 1);
        QCOMPARE(window.session().status().kind, SaveKind::Saved);
        QCOMPARE(window.session().base_revision(), disk_revision(desk.book));
        QCOMPARE(ink_strokes(read_book(desk.book)), opened);
    }

    // AC-SAVE 8: neither the book nor the recovery area can be written
    void nowhereToWriteIsSaid() {
        QTemporaryDir tmp;
        // (a file where the recovery folder's parent would be: nothing can be made there, on any system)
        const fs::path blocked = path_of(tmp.filePath("blocked"));
        QVERIFY(QFile(qpath(blocked)).open(QIODevice::WriteOnly));
        Desk desk(blocked / "recovery");
        {
            Fault full("assets:fail");
            desk.draw();
            QVERIFY(wait_for([&] { return desk.kind() == SaveKind::Failed && desk.window->session().status().nowhere; }));
        }
        QCOMPARE(desk.state_text(), QStringLiteral("保存失敗"));
        QVERIFY(desk.failure_text().contains(QStringLiteral("原稿にも復旧用の場所にも書き込めません。この変更は、まだどこにも保存されていません。")));
        desk.window->failure_bar()->retry_button()->click();
        QVERIFY(desk.wait_kind(SaveKind::Saved));
    }

    // A book this build may only read (a feature it does not know): opened read-only, the reason shown, every edit
    // refused in the person's words, nothing written
    void aReadOnlyBookSaysWhyAndRefusesEdits() {
        QTemporaryDir dir;
        const fs::path book = path_of(dir.filePath("book.genko"));
        make_book(book, 2);
        {
            QFile file(qpath(book / "project.json"));
            QVERIFY(file.open(QIODevice::ReadOnly));
            Json project = Json::parse(file.readAll().toStdString());
            file.close();
            project["features"] = Json::array({"genko.future-feature"});
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QByteArray::fromStdString(project.dump(2)));
        }
        const std::int64_t revision = disk_revision(book);
        Answers answers;
        MainWindow window;
        window.show();
        window.open_project(book);
        QVERIFY(wait_for([&] { return window.session().path().has_value(); }));
        QVERIFY(!window.session().read_only_reason().empty());
        QVERIFY(window.read_only_band()->isVisible());
        QVERIFY(window.read_only_band()->text().startsWith(QStringLiteral("この原稿は読み取り専用で開いています（変更はできません）: ")));
        window.choose_tool(QStringLiteral("pen"));
        const QPointF c = window.canvas()->seen_mm().center();
        inject::mouse_stroke(window.canvas(), {c, c + QPointF(12, 4)});
        QCOMPARE(ink_strokes(window.book()), std::size_t{0});
        QCOMPARE(window.last_error(), QStringLiteral("この原稿は読み取り専用で開いています（変更できません）"));
        QVERIFY(window.canvas()->live() == nullptr && window.canvas()->overlays() == 0);  // (the line drawn went)
        window.action(QStringLiteral("act_add_page"))->trigger();
        QCOMPARE(window.book().pages.size(), std::size_t{2});
        QCOMPARE(window.session().status().kind, SaveKind::Saved);
        QVERIFY(window.close());  // (nothing unsaved: closed without a question)
        QCOMPARE(disk_revision(book), revision);
    }

    // AC-SAVE 10: a failed commit retried in the same process, undone, and another process's save taken in
    void aFailedCommitIsRetriedThenUndoneThenJoined() {
        Desk desk;
        {
            Fault commit("commit:fail");
            desk.draw();
            QVERIFY(desk.wait_failed());
        }
        QCOMPARE(ink_strokes(desk.window->book()), std::size_t{1});
        desk.window->failure_bar()->retry_button()->click();
        QVERIFY(desk.wait_kind(SaveKind::Saved));
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{1});
        QCOMPARE(disk_revision(desk.book), std::int64_t{2});  // (the change committed once)
        desk.window->action(QStringLiteral("act_undo"))->trigger();
        QVERIFY(wait_for([&] { return ink_strokes(desk.window->book()) == 0 && desk.kind() == SaveKind::Saved; }));
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{0});
        // another process adds a line (genko apply); the window takes it in
        QTemporaryFile ops;
        QVERIFY(ops.open());
        ops.write(R"([{"op": "add_stroke", "page": 1, "layer": "ink", "points": [[30, 40, 0.7], [60, 45, 0.7]], "stabilize": 0}])");
        ops.close();
        QProcess cli;
        cli.start(QString::fromUtf8(GENKO_CLI), {QStringLiteral("apply"), qpath(desk.book), ops.fileName()});
        QVERIFY(cli.waitForFinished(60000));
        QCOMPARE(cli.exitCode(), 0);
        desk.window->session().check_outside();
        QVERIFY(wait_for([&] { return ink_strokes(desk.window->book()) == 1 && desk.kind() == SaveKind::Saved; }));
        desk.draw(10);
        QVERIFY(desk.wait_kind(SaveKind::Saved));
        QCOMPARE(ink_strokes(read_book(desk.book)), std::size_t{2});
    }

    // AC-SAVE 10: a failed commit, then another process writes before the retry; the retry goes on top of it
    void anotherWriterBeforeTheRetry() {
        Desk desk;
        {
            Fault commit("commit:fail");
            desk.draw();
            QVERIFY(desk.wait_failed());
        }
        QTemporaryFile ops;
        QVERIFY(ops.open());
        ops.write(R"([{"op": "add_stroke", "page": 2, "layer": "ink", "points": [[30, 40, 0.7], [60, 45, 0.7]], "stabilize": 0}])");
        ops.close();
        QProcess cli;
        cli.start(QString::fromUtf8(GENKO_CLI), {QStringLiteral("apply"), qpath(desk.book), ops.fileName()});
        QVERIFY(cli.waitForFinished(60000));
        QCOMPARE(cli.exitCode(), 0);
        desk.window->failure_bar()->retry_button()->click();
        QVERIFY(desk.wait_kind(SaveKind::Saved, 20000));
        const core::Document disk = read_book(desk.book);
        QCOMPARE(ink_strokes(disk, 0), std::size_t{1});  // ours, once
        QCOMPARE(ink_strokes(disk, 1), std::size_t{1});  // theirs
        QCOMPARE(ink_strokes(desk.window->book(), 1), std::size_t{1});
    }
};

QTEST_MAIN(TestGuiSave)
#include "test_gui_save.moc"
