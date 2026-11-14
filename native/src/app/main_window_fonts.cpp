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
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

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

// A face's name in a sentence (the list's first row is 「アンチック（いつもの書体）」: its name alone).
QString face_name(const QString& key) {
    if (key == QLatin1String("antique")) return QStringLiteral("アンチック");
    for (const lettering::FaceRow& face : lettering::catalog_faces()) {
        if (face.key == key) return face.label;
    }
    return lettering::face_label(key);
}

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

// The band's words: each sentence on a line of its own, then the way to put the band away.
QString band_text(const QString& words) {
    QStringList lines;
    for (const QString& line : words.split(QLatin1Char('\n'))) lines << line.toHtmlEscaped();
    return lines.join(QStringLiteral("<br>")) + QStringLiteral("　<a href=\"close\">この知らせを閉じる</a>");
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
    text_settings_->show_face(armed_face_ == QLatin1String("antique") ? QString() : armed_face_, face_name(armed_face_));
    if (armed_face_.isEmpty()) return;
    text_settings_->make->setCurrentIndex(std::max(0, text_settings_->make->findData(QStringLiteral("line"))));
    if (!PageCanvas::is_text_tool(canvas_->tool())) choose_tool(QStringLiteral("text"));
    flash(QStringLiteral("書体「%1」を選びました。置きたい所をクリックして、台詞を打ちます").arg(face_name(armed_face_)), 6000);
}

void MainWindow::use_font_face(const QString& key, const QString& how) {
    if (!is_face(key)) return;
    const QString name = face_name(key);
    const std::string face = key.toStdString();
    // (a face whose file is not on this computer is not given to anything: said, nothing changed)
    // (asked for by name, a face is read and checked here if it was not yet: one file, the one about to be drawn in)
    if (core::fonts::added(face) && !render::font_files::present(face)) {
        if (font_palette_ != nullptr) font_palette_->refresh_files();
        flash(QStringLiteral("書体「%1」のファイルが、このパソコンにありません。アプリを入れ直すと使えます").arg(name), 6000);
        return;
    }
    if (how == QLatin1String("book")) {  // (この本のいつもの書体: every line that names no face — アンチック: none said)
        const std::string wanted = key == QLatin1String("antique") ? std::string() : face;
        if (book().font_path == wanted) {
            flash(QStringLiteral("この本のいつもの書体は、もう「%1」です").arg(name), 4000);
            return;
        }
        if (!apply_ops(Json::array({Json{{"op", "set_meta"}, {"font_path", wanted}}}))) return;
        show_usual_face();
        flash(QStringLiteral("この本のいつもの書体を「%1」にしました。書体を決めていない台詞が、この書体になります（元に戻す で取り消せます）").arg(name), 6000);
        return;
    }
    if (how == QLatin1String("ruby")) {
        StoryPanel* panel = story();
        if (panel == nullptr || !panel->set_chosen_ruby_font(key == QLatin1String("antique") ? QStringLiteral("antique") : key)) {
            flash(QStringLiteral("先に台詞パネルのルビの一覧で、書体を変えるルビを選びます"), 5000);
            return;
        }
        flash(QStringLiteral("選んだルビの書体を「%1」にしました（元に戻す で取り消せます）").arg(name), 4000);
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
        flash(QStringLiteral("先に選択ツールで、書体を変える台詞をクリックして選びます"), 4000);
        return;
    }
    // (that face already, on its whole: no edit, no step of 取り消し)
    const Json now = record->style.is_object() ? core::get_or(record->style, "font", Json()) : Json();
    if (!type_range() && now.is_string() && now.get<std::string>() == face && !render::text::sets_opentype(*record)) {
        flash(QStringLiteral("選んだ台詞は、もう「%1」です").arg(name), 4000);
        return;
    }
    const std::size_t before = session_->changes_in_memory();
    type_attrs(Json{{"font", face}, {"face_index", nullptr}, {"family", nullptr}, {"font_style", nullptr}, {"postscript", nullptr}}, Json{{"font", face}});
    if (font_palette_ != nullptr) font_palette_->mark(armed_face_);  // (the mark stays on the face the next line takes)
    if (session_->changes_in_memory() == before) return;  // (refused: the refusal was said)
    // (a face that draws its kanji in another says so, once it is chosen)
    const core::fonts::Entry* entry = core::fonts::find(face);
    const core::fonts::Entry* partner = entry != nullptr && !entry->fallback.empty() ? core::fonts::find(entry->fallback) : nullptr;
    flash(partner != nullptr ? QStringLiteral("選んだ台詞の書体を「%1」にしました。この書体に無い漢字は「%2」で描きます（元に戻す で取り消せます）")
                                   .arg(name, QString::fromStdString(partner->label))
                             : QStringLiteral("選んだ台詞の書体を「%1」にしました（元に戻す で取り消せます）").arg(name),
          5000);
}

void MainWindow::show_usual_face() {
    // (the text tool's first choice says what a line that names no face is drawn in: the book's usual face when it is one
    // of Genko's own; a font file of the person's is not named there, as before)
    const QString usual = QString::fromStdString(book().font_path);
    text_settings_->show_usual(is_face(usual) ? face_name(usual) : QStringLiteral("アンチック"));
}

QStringList MainWindow::faces_not_installed() const {
    QStringList out;
    for (const std::string& key : core::fonts::named(book())) {
        if (!render::font_files::file_there(key)) out << face_name(QString::fromStdString(key));
    }
    return out;
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
        fonts_band_->setVisible(!words.isEmpty());
        if (!words.isEmpty()) fonts_band_->setText(band_text(words));
        show_usual_face();
        return;
    }
    std::erase_if(fonts_told_, [](const auto& s) { return s.first.expired(); });
    fonts_told_.emplace_back(session, QString());
    const DocPtr doc = session->snapshot();
    if (!doc) return;
    QStringList absent, changed;
    for (const std::string& key : core::fonts::named(*doc)) {
        if (!render::font_files::file_there(key)) absent << face_name(QString::fromStdString(key));
    }
    for (const std::string& key : core::fonts::named_unannounced(*doc)) {
        if (render::font_files::file_there(key)) changed << face_name(QString::fromStdString(key));
    }
    QStringList lines;
    if (!absent.isEmpty()) {
        lines << QStringLiteral("この原稿が使っている書体%1のファイルが、このパソコンにありません。その文字はいつもの書体で描いています（アプリを入れ直すと元の書体に戻ります）")
                     .arg(named(absent));
    }
    if (!changed.isEmpty()) {
        lines << QStringLiteral("この原稿に名前で書いてあった書体%1は、この版から付属の書体で描きます。前の版ではアンチックで描いていたので、見た目が変わります")
                     .arg(named(changed));
    }
    const QString words = lines.join(QLatin1Char('\n'));
    fonts_told_.back().second = words;
    fonts_band_->setVisible(!words.isEmpty());
    if (!words.isEmpty()) fonts_band_->setText(band_text(words));
    show_usual_face();
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
