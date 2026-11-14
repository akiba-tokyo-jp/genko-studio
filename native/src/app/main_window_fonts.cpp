// 書体 (fonts@1, 書体の追加 F2; the user's 「書体も足りない気がするのですよね」): the palette of lettering faces in the window.
// Its dock (a tab beside 台詞, in ウィンドウ, kept with the layout as every dock, closed until asked for), and what a pick
// does: the chosen line (or text layer) takes the face — one change, one step of 取り消し —; or the face becomes the one
// the next line the text tool places takes (the text tool's 書体 shows it); or, from a face's own menu, a chosen ruby's
// face or the book's usual face. A face whose file is not on this computer is not picked: said, and nothing changed.
//
// And what a person is told when a book is opened (once a book, when all its pages are read): that a face it names is
// not on this computer — its letters drawn in the usual face —, and that a book of before whose words happened to be a
// face's key of today is drawn in that face from now on.

#include <QComboBox>
#include <QDockWidget>
#include <QFutureWatcher>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QtConcurrent>

#include <algorithm>

#include "app/canvas.hpp"
#include "app/font_palette.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/session.hpp"
#include "app/story_panel.hpp"
#include "app/tool_settings.hpp"
#include "core/font_catalog.hpp"
#include "core/pyops.hpp"
#include "render/font_files.hpp"
#include "render/text/lettering.hpp"
#include "render/text/opentype.hpp"

namespace genko::app {

using core::Json;

namespace {

// A face's name in a sentence: the one name it has everywhere (lettering::face_name).
QString face_name(const QString& key) { return lettering::face_name(key); }

bool is_face(const QString& key) {
    const auto& faces = lettering::catalog_faces();
    return std::any_of(faces.begin(), faces.end(), [&](const lettering::FaceRow& face) { return face.key == key; });
}

// Names in a row for a sentence: 「A」「B」, and how many more.
QString named(const QStringList& labels) {
    QString out;
    for (qsizetype i = 0; i < labels.size() && i < 3; ++i) out += QStringLiteral("「%1」").arg(labels[i]);
    if (labels.size() > 3) out += QStringLiteral("ほか %1 つ").arg(labels.size() - 3);
    return out;
}

}  // namespace

void MainWindow::build_font_actions() {
    make("act_font_palette", QStringLiteral("書体のパレットを開く"), [this] { show_dock(QStringLiteral("書体")); }, {},
         QStringLiteral("まんが用の書体を見本で選んで、選んだ台詞に当てるか、次に置く台詞の書体にします"));
}

void MainWindow::build_font_dock() {
    font_palette_ = new FontPalette(this);
    font_dock_ = new QDockWidget(QStringLiteral("書体"), this);
    font_dock_->setObjectName(QStringLiteral("書体"));
    font_dock_->setWidget(font_palette_);
    font_dock_->setMinimumWidth(220);
    font_dock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, font_dock_);
    tabifyDockWidget(lines_dock_, font_dock_);  // (beside the lines it letters)
    font_dock_->hide();  // (ウィンドウ → 書体, ページ → 台詞 → 書体のパレットを開く, the text tool's ほかの書体…)
    view_menu_->addAction(font_dock_->toggleViewAction());
    connect(font_palette_, &FontPalette::picked, this, [this](const QString& key, const QString& how) { use_font_face(key, how); });
    connect(text_settings_, &TextToolSettings::moreFonts, this, [this] { show_dock(QStringLiteral("書体")); });
    // (the text tool's own 書体 chosen: that face — the palette's mark follows it)
    connect(text_settings_->font, &QComboBox::activated, this, [this](int) {
        const QString key = text_settings_->font->currentData().toString();
        armed_face_ = is_face(key) ? key : QString();
        if (font_palette_ != nullptr) font_palette_->mark(armed_face_);
    });
}

void MainWindow::arm_font_face(const QString& key) {
    armed_face_ = is_face(key) ? key : QString();
    if (font_palette_ != nullptr) font_palette_->mark(armed_face_);
    // (アンチック: its own choice of the text tool's 書体 when the book has a usual face of its own — the line placed says
    // "antique" —, the first choice when it has none: tool_settings.cpp)
    text_settings_->show_face(armed_face_, face_name(armed_face_));
    if (armed_face_.isEmpty()) return;
    text_settings_->make->setCurrentIndex(std::max(0, text_settings_->make->findData(QStringLiteral("line"))));
    if (!PageCanvas::is_text_tool(canvas_->tool())) choose_tool(QStringLiteral("text"));
    say_of_fonts(QStringLiteral("書体「%1」を選びました。置きたい所をクリックして、台詞を打ちます").arg(face_name(armed_face_)), 6000);
}

void MainWindow::use_font_face(const QString& key, const QString& how) {
    if (!is_face(key)) return;
    const QString name = face_name(key);
    const std::string face = key.toStdString();
    // (a face whose file is not on this computer is not given to anything: said, nothing changed)
    // (asked for by name, a face is read and checked here if it was not yet: one file, the one about to be drawn in)
    if (core::fonts::added(face) && !render::font_files::present(face)) {
        if (font_palette_ != nullptr) font_palette_->refresh_files();
        say_of_fonts(QStringLiteral("書体「%1」のファイルが、このパソコンにありません（アプリを入れ直すと使えます）").arg(name), 6000);
        return;
    }
    if (how == QLatin1String("book")) {  // (この本のいつもの書体: every line that names no face — アンチック: none said)
        const std::string wanted = key == QLatin1String("antique") ? std::string() : face;
        if (book().font_path == wanted) {
            say_of_fonts(QStringLiteral("この本のいつもの書体は、もう「%1」です").arg(name), 4000);
            return;
        }
        if (!apply_ops(Json::array({Json{{"op", "set_meta"}, {"font_path", wanted}}}))) return;
        show_usual_face();
        say_of_fonts(QStringLiteral("この本のいつもの書体を「%1」にしました（元に戻す で取り消せます）").arg(name), 6000);
        return;
    }
    if (how == QLatin1String("ruby")) {
        StoryPanel* panel = story();
        switch (panel != nullptr ? panel->set_chosen_ruby_font(key) : StoryPanel::RubyFace::NoRuby) {
        case StoryPanel::RubyFace::NoRuby:
            say_of_fonts(QStringLiteral("先に台詞パネルのルビの一覧で、書体を変えるルビを選びます"), 5000);
            break;
        case StoryPanel::RubyFace::Same:
            say_of_fonts(QStringLiteral("選んだルビは、もう「%1」です").arg(name), 4000);
            break;
        case StoryPanel::RubyFace::Refused:  // (said where it was refused: nothing is claimed here)
            break;
        case StoryPanel::RubyFace::Done:
            say_of_fonts(QStringLiteral("選んだルビの書体を「%1」にしました（元に戻す で取り消せます）").arg(name), 4000);
            break;
        }
        return;
    }
    const core::StoryLine* line = canvas_->selected_line_id ? line_by_id(*canvas_->selected_line_id) : nullptr;
    const core::Page* page = current_page();
    const bool here = line != nullptr && page != nullptr && line->page_index == page->index;
    const bool to_line = how == QLatin1String("line") || (how.isEmpty() && here && !PageCanvas::is_text_tool(canvas_->tool()));
    if (!to_line) {
        arm_font_face(key);
        return;
    }
    const std::optional<Json> target = type_target(true);
    const core::StoryLine* record = target ? type_record(*target) : nullptr;
    if (record == nullptr) {
        say_of_fonts(QStringLiteral("先に選択ツールで、書体を変える台詞をクリックして選びます"), 4000);
        return;
    }
    // (that face already, on its whole: no edit, no step of 取り消し)
    const Json now = record->style.is_object() ? core::get_or(record->style, "font", Json()) : Json();
    if (!type_range() && now.is_string() && now.get<std::string>() == face && !render::text::sets_opentype(*record)) {
        say_of_fonts(QStringLiteral("選んだ台詞は、もう「%1」です").arg(name), 4000);
        return;
    }
    const std::size_t before = session_->changes_in_memory();
    type_attrs(Json{{"font", face}, {"face_index", nullptr}, {"family", nullptr}, {"font_style", nullptr}, {"postscript", nullptr}}, Json{{"font", face}});
    if (font_palette_ != nullptr) font_palette_->mark(armed_face_);  // (the mark stays on the face the next line takes)
    if (session_->changes_in_memory() == before) return;  // (refused: the refusal was said)
    // (a face that draws its kanji in another says so, once it is chosen)
    const core::fonts::Entry* entry = core::fonts::find(face);
    const core::fonts::Entry* partner = entry != nullptr && !entry->fallback.empty() ? core::fonts::find(entry->fallback) : nullptr;
    say_of_fonts(partner != nullptr ? QStringLiteral("選んだ台詞の書体を「%1」にしました（漢字は「%2」で描きます）").arg(name, QString::fromStdString(partner->label))
                                    : QStringLiteral("選んだ台詞の書体を「%1」にしました（元に戻す で取り消せます）").arg(name),
                 5000);
}

void MainWindow::show_usual_face() {
    // (the text tool's first choice says what a line that names no face is drawn in: the book's usual face — one of
    // Genko's own by its name, a font file of the person's as 書体ファイル —, and アンチック has a choice of its own
    // while the book has a usual face; the palette's row of that face says この本のいつもの書体. Called whenever the
    // book changes: 元に戻す, やり直す and an agent's set_meta change the usual face as a pick does)
    const QString usual = QString::fromStdString(book().font_path);
    const bool own = !usual.isEmpty();
    text_settings_->show_usual(!own ? QStringLiteral("アンチック") : is_face(usual) ? face_name(usual) : QStringLiteral("書体ファイル"), own);
    if (armed_face_ == QLatin1String("antique")) {  // (アンチック waits for the next line: its choice, wherever that is now)
        const int at = text_settings_->font->findData(QStringLiteral("antique"));
        text_settings_->font->setCurrentIndex(std::max(0, at));
    }
    if (font_palette_ != nullptr) font_palette_->set_usual(is_face(usual) ? usual : own ? QStringLiteral("-") : QString());
}

QStringList MainWindow::faces_not_installed() const {
    // (one stat a face, and what was learnt of its file when the book was opened — its content checked then, off this
    // thread — or when it was drawn)
    QStringList out;
    for (const std::string& key : core::fonts::named(book())) {
        if (!render::font_files::file_there(key)) out << face_name(QString::fromStdString(key));
    }
    return out;
}

void MainWindow::say_of_fonts(const QString& words, int ms) {
    // (a sentence the one-line status label would cut — a narrow window, long names — is shown whole in the band)
    const QFontMetrics metrics([this] {
        QFont bold = status_->font();
        bold.setBold(true);
        return bold;
    }());
    const int room = status_->contentsRect().width();
    if (!isVisible() || room <= 0 || metrics.horizontalAdvance(words) + 4 <= room) {
        flash(words, ms);
        return;
    }
    tell_of_fonts(words);
    flash(QStringLiteral("書体の知らせを、ページの上に出しました"), ms);
}

void MainWindow::tell_of_fonts(const QString& words) {
    auto mine = std::find_if(fonts_told_.begin(), fonts_told_.end(), [this](const auto& told) { return told.first.lock() == session_; });
    if (mine == fonts_told_.end()) {
        fonts_told_.emplace_back(session_, QString());
        mine = fonts_told_.end() - 1;
    }
    mine->second = words;
    show_fonts_band(words);
}

void MainWindow::show_fonts_band(const QString& words) {
    fonts_band_->setVisible(!words.isEmpty());
    if (words.isEmpty()) return;
    QStringList lines;
    for (const QString& line : words.split(QLatin1Char('\n'))) lines << line.toHtmlEscaped();
    fonts_band_text_->setText(lines.join(QStringLiteral("<br>")));
}

void MainWindow::note_fonts_on_open(const std::shared_ptr<Session>& session) {
    if (!session || session != session_) return;
    if (session->loading()) {
        // (its later pages are being read: looked at once they are all there — a type layer of a page read later names
        // its face too)
        auto once = std::make_shared<QMetaObject::Connection>();
        QPointer<MainWindow> self(this);
        std::weak_ptr<Session> weak = session;
        *once = connect(session.get(), &Session::changed, this, [self, weak, once](const BookChange&) {
            const auto s = weak.lock();
            if (!self || !s || s->loading()) return;
            QObject::disconnect(*once);
            QTimer::singleShot(0, self.data(), [self, weak] {
                if (const auto later = weak.lock(); self && later) self->note_fonts_on_open(later);
            });
        });
        return;
    }
    // (looked at once a book: a tab come back to shows what was said of it, unless that was closed)
    for (const auto& [told, words] : fonts_told_) {
        if (told.lock() != session) continue;
        show_fonts_band(words);
        show_usual_face();
        return;
    }
    std::erase_if(fonts_told_, [](const auto& s) { return s.first.expired(); });
    fonts_told_.emplace_back(session, QString());
    show_usual_face();
    const DocPtr doc = session->snapshot();
    if (!doc) return;
    const std::vector<std::string> named_keys = core::fonts::named(*doc);
    if (named_keys.empty()) return;  // (a book that uses none of the forty: nothing looked at, nothing said)
    const std::vector<std::string> unannounced = core::fonts::named_unannounced(*doc);
    // Whether each face the book names is installed — its file read whole and checked against the catalogue's — is found
    // out on a worker: megabytes a face, not the screen's thread's to read. What it learns stays with render::font_files
    // (the page is drawn from the same answers), and comes back here as the faces that are not installed.
    auto* checked = new QFutureWatcher<std::vector<std::string>>(this);
    std::weak_ptr<Session> weak = session;
    connect(checked, &QFutureWatcher<std::vector<std::string>>::finished, this, [this, checked, weak, named_keys, unannounced] {
        checked->deleteLater();
        const std::shared_ptr<Session> of = weak.lock();
        if (!of) return;
        const std::vector<std::string> lost = checked->result();
        const auto is_lost = [&lost](const std::string& key) { return std::find(lost.begin(), lost.end(), key) != lost.end(); };
        QStringList absent, changed;
        for (const std::string& key : named_keys) {
            if (is_lost(key)) absent << face_name(QString::fromStdString(key));
        }
        for (const std::string& key : unannounced) {
            if (!is_lost(key)) changed << face_name(QString::fromStdString(key));
        }
        QStringList lines;
        if (!absent.isEmpty()) {
            lines << QStringLiteral("この本が使っている書体%1のファイルが、このパソコンにありません。"
                                    "その文字は代わりの書体（台詞はアンチック）で描いています（アプリを入れ直すと元の書体に戻ります）")
                         .arg(named(absent));
        }
        if (!changed.isEmpty()) {
            lines << QStringLiteral("この本が指定している書体%1は、前の版には無かったためアンチックで描いていました。"
                                    "この版からはその書体で描くので、見た目が変わります")
                         .arg(named(changed));
        }
        const QString words = lines.join(QLatin1Char('\n'));
        for (auto& [told, kept] : fonts_told_) {
            if (told.lock() == of) kept = words;
        }
        if (of == session_) {
            show_fonts_band(words);
            if (font_palette_ != nullptr && font_palette_->isVisible()) font_palette_->refresh_files();
        }
    });
    checked->setFuture(QtConcurrent::run([named_keys] {
        std::vector<std::string> lost;
        for (const std::string& key : named_keys) {
            if (!render::font_files::present(key)) lost.push_back(key);
        }
        return lost;
    }));
}

QString MainWindow::fonts_notice() const {
    if (fonts_band_ == nullptr || fonts_band_->isHidden()) return {};
    for (const auto& [told, words] : fonts_told_) {
        if (told.lock() == session_) return words;
    }
    return {};
}

void MainWindow::close_fonts_notice() {
    for (auto& [told, words] : fonts_told_) {
        if (told.lock() == session_) words.clear();
    }
    if (fonts_band_ != nullptr) fonts_band_->hide();
}

}  // namespace genko::app
