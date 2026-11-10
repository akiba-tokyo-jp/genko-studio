// PC5 G-d (SPEC.md TEXT-02「文字・段落・スタイル・字形のパネル」「スペルチェック」「検索と置換」「書体の置き換え」「環境設定 →
// テキスト」): the window's part of the type panels — ウィンドウ → 文字・段落・文字スタイル・段落スタイル・字形 and 書式 → パネル ▸,
// 書式 → フォントプレビューのサイズ ▸, 見つからないフォントを解決…・すべての見つからないフォントを置換, 編集 → スペルチェック…
// (検索と置換… is 台詞の検索・置換… — act_replace — renamed and moved to 編集) — and what the panels ask of the window: the
// range chosen in the editor typing over a text, their changes as ops, a glyph put in, the fonts used last.

#include <QActionGroup>
#include <QDockWidget>
#include <QMenu>
#include <QScrollArea>
#include <QTextCursor>

#include <algorithm>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/config.hpp"
#include "app/ime.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/type_dialogs.hpp"
#include "app/type_panels.hpp"
#include "core/prefs_table.hpp"
#include "core/ruby.hpp"
#include "core/type_text.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"
#include "render/text/opentype.hpp"
#include "service/wording.hpp"

namespace genko::app {

using core::Json;

namespace {

// ウィンドウ's panels and 書式 → パネル ▸'s (Photoshop's names): the dock's object name, its name in ウィンドウ, in パネル ▸.
struct TypeDock {
    const char* key;
    const char* window_name;
    const char* menu_name;
    const char* tip;
};
constexpr TypeDock kTypeDocks[] = {
    {"character", "文字", "文字パネル", "選んだ文字のフォント・サイズ・行送り・トラッキング・カラーなど"},
    {"paragraph", "段落", "段落パネル", "選んだ段落の揃え・インデント・アキ・禁則処理など"},
    {"character_styles", "文字スタイル", "文字スタイルパネル", "本の文字スタイル（作る・当てる・再定義・上書きを消去）"},
    {"paragraph_styles", "段落スタイル", "段落スタイルパネル", "本の段落スタイル（作る・当てる・再定義・上書きを消去）"},
    {"glyphs", "字形", "字形パネル", "書体の字形・異体字を選んで入れる"},
};

// 書式 → フォントプレビューのサイズ ▸ (Photoshop's), its value in 環境設定 → テキスト.
constexpr std::pair<const char*, const char*> kPreviews[] = {{"off", "なし"}, {"small", "小"}, {"medium", "中"}, {"large", "大"}, {"extraLarge", "特大"}, {"huge", "極大"}};

}  // namespace

void MainWindow::build_type_panel_actions() {
    for (const TypeDock& d : kTypeDocks) {
        const QString key = QString::fromLatin1(d.key);
        // (ウィンドウ: the panel's switch — checked while it shows; 書式 → パネル ▸: brings it)
        make(QStringLiteral("act_panel_") + key, QString::fromUtf8(d.window_name), [] {}, {}, QString::fromUtf8(d.tip), true);
        make(QStringLiteral("act_type_panel_") + key, QString::fromUtf8(d.menu_name), [this, key] { show_dock(QStringLiteral("type_") + key); }, {}, QString::fromUtf8(d.tip));
    }
    auto* sizes = new QActionGroup(this);
    sizes->setExclusive(true);
    for (const auto& [value, label] : kPreviews) {
        const QString v = QString::fromLatin1(value);
        sizes->addAction(make(QStringLiteral("act_type_preview_") + v, QString::fromUtf8(label), [this, v] { set_font_preview(v); }, {},
                              QStringLiteral("文字パネルのフォントの一覧に、それぞれの書体の見本をこの大きさで出します"), true));
    }
    set_font_preview(font_preview());
    make("act_type_replace_missing", QStringLiteral("すべての見つからないフォントを置換"), [this] { replace_all_missing_fonts(); }, {},
         QStringLiteral("原稿の文字が使っている書体のうち、このパソコンに無いものを、いつもの書体にします"));
    make("act_type_resolve_missing", QStringLiteral("見つからないフォントを解決…"), [this] { resolve_missing_fonts(); }, {},
         QStringLiteral("このパソコンに無い書体を一覧し、それぞれ置き換える書体を選びます"));
    make("act_spelling", QStringLiteral("スペルチェック…"), [this] { open_spelling(); }, {},
         QStringLiteral("台詞とテキストレイヤーの欧文の綴りを英語の辞書で、和文の表記を表記の表で確かめます"));
    // (the editor typing over a text: what it chooses is the panels' range; 環境設定 → テキスト's typing)
    connect(canvas_, &PageCanvas::editorOpened, this, [this](InlineEditor* editor) { watch_type_editor(editor); });
}

void MainWindow::build_type_docks() {
    for (const TypeDock& d : kTypeDocks) {
        const QString key = QString::fromLatin1(d.key);
        QWidget* panel = nullptr;
        if (key == QLatin1String("character")) panel = character_panel_ = new CharacterPanel(this);
        if (key == QLatin1String("paragraph")) panel = paragraph_panel_ = new ParagraphPanel(this);
        if (key == QLatin1String("character_styles")) panel = character_styles_panel_ = new TypeStylesPanel(this, false);
        if (key == QLatin1String("paragraph_styles")) panel = paragraph_styles_panel_ = new TypeStylesPanel(this, true);
        if (key == QLatin1String("glyphs")) panel = glyphs_panel_ = new GlyphsPanel(this);
        auto* dock = new QDockWidget(QString::fromUtf8(d.window_name), this);
        dock->setObjectName(QStringLiteral("type_") + key);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        dock->setWidget(scroll);  // (in the dock first, then the panel in it: styled once)
        scroll->setWidget(panel);
        dock->setMinimumWidth(220);
        dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
        addDockWidget(Qt::RightDockWidgetArea, dock);
        if (layers_dock_ != nullptr) tabifyDockWidget(layers_dock_, dock);  // (beside the layers, as Photoshop's 文字 and 段落)
        dock->hide();  // (ウィンドウ → 文字… brings it)
        type_docks_.push_back(dock);
        QAction* toggle = action(QStringLiteral("act_panel_") + key);
        view_menu_->addAction(toggle);
        // (the window the context — its parts' signals let go of it as it goes, ~MainWindow: PC6, the review's P3-5 —, the
        // dock looked for)
        connect(toggle, &QAction::triggered, this, [this, dock = QPointer<QDockWidget>(dock)](bool on) {
            if (!dock) return;
            dock->setVisible(on);
            if (on) {
                dock->raise();
                refresh_type_panels();
            }
        });
        connect(dock, &QDockWidget::visibilityChanged, this, [this, toggle, dock](bool) {
            const QSignalBlocker quiet(toggle);
            toggle->setChecked(dock->isVisible());
            if (dock->isVisible()) refresh_type_panels();
        });
    }
}

// --- the range chosen ----------------------------------------------------------------------------------------------------

void MainWindow::watch_type_editor(InlineEditor* editor) {
    if (editor == nullptr) return;
    type_selection_.reset();
    {
        const auto store = settings();
        editor->smart_quotes = core::prefs::value(*store, "type.smartQuotes") == Json(true);
        editor->esc_commits = core::prefs::value(*store, "type.useEscToCommit") == Json(true);
    }
    // (the editor shows the text with its marks — ｜《》 —: what is chosen counted in the text without them)
    const auto chosen = [this, editor] {
        const QString typed = editor->toPlainText();
        const QTextCursor cursor = editor->textCursor();
        const int a = std::min(cursor.selectionStart(), cursor.selectionEnd()), b = std::max(cursor.selectionStart(), cursor.selectionEnd());
        const std::string text = lettering::parse_marks(typed).text;
        const std::int64_t start = core::ruby::count(lettering::parse_marks(typed.left(a)).text);
        const std::int64_t end = core::ruby::count(lettering::parse_marks(typed.left(b)).text);
        type_selection_ = TypeSelection{text, start, std::max(start, end)};
    };
    connect(editor, &QPlainTextEdit::cursorPositionChanged, this, chosen);
    connect(editor, &QPlainTextEdit::selectionChanged, this, chosen);
}

std::optional<std::pair<std::int64_t, std::int64_t>> MainWindow::type_range() const {
    if (!type_selection_ || type_selection_->end <= type_selection_->start) return std::nullopt;
    auto* self = const_cast<MainWindow*>(this);
    const std::optional<Json> target = self->type_target(false);
    const core::StoryLine* record = target ? self->type_record(*target) : nullptr;
    if (record == nullptr || record->text != type_selection_->text) return std::nullopt;
    const std::int64_t n = core::ruby::count(record->text);
    if (type_selection_->start >= n) return std::nullopt;
    // (the whole text chosen: the whole — its own attributes, not a range of them)
    if (type_selection_->start == 0 && type_selection_->end >= n) return std::nullopt;
    return std::make_pair(type_selection_->start, std::min(type_selection_->end, n));
}

// --- the panels' changes -------------------------------------------------------------------------------------------------

void MainWindow::type_attrs(const Json& attrs, const Json& style) {
    const std::optional<Json> target = type_target(true);
    const core::StoryLine* record = target ? type_record(*target) : nullptr;
    if (record == nullptr) return;
    const auto range = type_range();
    // (a line of before, on its whole, by what its style says: as before — its look kept)
    if (!range && target->contains("line") && style.is_object() && !style.empty() && !render::text::sets_opentype(*record)) {
        apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", (*target)["line"]}, {"style", style}}}));
        refresh_type_panels();
        return;
    }
    Json op{{"op", "set_type"}};
    for (const auto& [key, value] : target->items()) op[key] = value;
    if (range) op["range"] = Json::array({range->first, range->second});
    for (const auto& [key, value] : attrs.items()) op[key] = value;
    apply_ops(Json::array({op}));
    refresh_type_commands();
    refresh_type_frame();
}

void MainWindow::type_style_key(const Json& style) {
    const std::optional<Json> target = type_target(true);
    if (!target) return;
    if (!target->contains("line")) {
        flash(QStringLiteral("行間は台詞の書式です（テキストレイヤーは文字パネルの「行送り」で）"), 4000);
        refresh_type_panels();
        return;
    }
    apply_ops(Json::array({Json{{"op", "edit_line"}, {"id", (*target)["line"]}, {"style", style}}}));
    refresh_type_panels();
}

void MainWindow::refresh_type_panels() {
    const auto shows = [](QWidget* panel) {
        for (QWidget* w = panel; w != nullptr; w = w->parentWidget()) {
            if (auto* dock = qobject_cast<QDockWidget*>(w)) return dock->isVisible();
        }
        return false;
    };
    if (character_panel_ != nullptr && shows(character_panel_)) character_panel_->refresh();
    if (paragraph_panel_ != nullptr && shows(paragraph_panel_)) paragraph_panel_->refresh();
    if (character_styles_panel_ != nullptr && shows(character_styles_panel_)) character_styles_panel_->refresh();
    if (paragraph_styles_panel_ != nullptr && shows(paragraph_styles_panel_)) paragraph_styles_panel_->refresh();
    if (glyphs_panel_ != nullptr && shows(glyphs_panel_)) glyphs_panel_->refresh();
}

void MainWindow::insert_glyph(const QString& glyph) {
    if (InlineEditor* editor = canvas_->editor()) {  // (typing: at its caret, over what it chose)
        editor->insertPlainText(glyph);
        return;
    }
    const std::optional<Json> target = type_target(true);
    const core::StoryLine* record = target ? type_record(*target) : nullptr;
    if (record == nullptr) return;
    // (where the caret was last in this text, else its end)
    std::int64_t start = core::ruby::count(record->text), end = start;
    if (type_selection_ && type_selection_->text == record->text) {
        start = std::min(type_selection_->start, start);
        end = std::min(type_selection_->end, core::ruby::count(record->text));
    }
    Json op{{"op", "edit_type"}, {"replace", Json{{"start", start}, {"end", end}, {"text", glyph.toStdString()}}}};
    for (const auto& [key, value] : target->items()) op[key] = value;
    if (!apply_ops(Json::array({op}))) return;
    // (the caret after it, as typing leaves it)
    if (const core::StoryLine* now = type_record(*target)) {
        const std::int64_t at = start + core::ruby::count(glyph.toStdString());
        type_selection_ = TypeSelection{now->text, at, at};
    }
    refresh_type_frame();
}

void MainWindow::remember_font(const QString& data) {
    const auto store = settings();
    QStringList used = store->value(QStringLiteral("type/recent_fonts_used")).toString().split(QLatin1Char('\t'), Qt::SkipEmptyParts);
    used.removeAll(data);
    used.prepend(data);
    store->setValue(QStringLiteral("type/recent_fonts_used"), used.mid(0, 50).join(QLatin1Char('\t')));
}

QString MainWindow::font_preview() const {
    const auto store = settings();
    return QString::fromStdString(core::prefs::value(*store, "type.fontPreview").get<std::string>());
}

void MainWindow::set_font_preview(const QString& size) {
    if (size != font_preview()) settings()->setValue(QStringLiteral("type/font_preview"), size);
    for (const auto& [value, label] : kPreviews) {
        if (QAction* act = action(QStringLiteral("act_type_preview_") + QString::fromLatin1(value))) act->setChecked(size == QLatin1String(value));
    }
    if (character_panel_ != nullptr) character_panel_->fill_fonts();
}

// --- 環境設定 → テキスト's 欠落グリフの保護 -----------------------------------------------------------------------------------

void MainWindow::note_missing_glyphs(const core::StoryLine& record) {
    const auto store = settings();
    if (core::prefs::value(*store, "type.missingGlyphProtection") != Json(true)) return;
    // (the letters its own font has not: drawn in another — the bundled Gothic or the composite's other half —, said)
    try {
        const auto [face, sfx] = render::text::line_face(record, std::optional<std::string>(book().font_path));
        (void)sfx;
        if (face.composite()) return;  // (the bundled アンチック: its halves cover each other — as before)
        render::text::Fonts fonts;
        QString lacking;
        for (const char32_t c : core::ruby::code_points(record.text)) {
            if (c == U'\n' || c == U' ' || c == U'　') continue;
            const std::u32string one(1, c);
            if (fonts.path_for(face, one) != face.other && !lacking.contains(QString::fromUcs4(&c, 1))) lacking += QString::fromUcs4(&c, 1);
            if (lacking.size() >= 8) break;
        }
        if (!lacking.isEmpty()) flash(QStringLiteral("「%1」は選んだ書体に無いので、ほかの書体で描きます（欠落グリフの保護）").arg(lacking), 5000);
    } catch (const std::exception&) {
    }
}

// --- 編集 → スペルチェック…, 書式 → 見つからないフォント --------------------------------------------------------------------

void MainWindow::open_spelling() {
    SpellingDialog dialog(this);
    ask::exec(&dialog);
}

void MainWindow::resolve_missing_fonts() {
    if (!apply_ops(Json::array({Json{{"op", "replace_fonts"}}}))) return;
    Json missing = Json::array();
    for (const Json& result : last_results_) {
        if (result.is_object() && result.value("op", Json()) == Json("replace_fonts")) missing = result.value("missing", Json::array());
    }
    if (missing.empty()) {
        flash(QStringLiteral("見つからないフォントはありません"), 3000);
        return;
    }
    MissingFontsDialog dialog(this, missing);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    if (apply_ops(Json::array({Json{{"op", "replace_fonts"}, {"map", dialog.map()}}}))) {
        update_all_type();
        flash(QStringLiteral("見つからないフォントを置き換えました（元に戻すで戻せます）"), 4000);
    }
}

void MainWindow::replace_all_missing_fonts() {
    if (!apply_ops(Json::array({Json{{"op", "replace_fonts"}, {"all", true}}}))) return;
    std::int64_t replaced = 0;
    for (const Json& result : last_results_) {
        if (result.is_object() && result.value("op", Json()) == Json("replace_fonts")) replaced = result.value("replaced", Json(0)).get<std::int64_t>();
    }
    if (replaced > 0) update_all_type();
    flash(replaced > 0 ? QStringLiteral("見つからないフォントを、いつもの書体にしました（%1 か所）").arg(replaced) : QStringLiteral("見つからないフォントはありません"), 4000);
}

}  // namespace genko::app
