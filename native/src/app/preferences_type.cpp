// 環境設定 → テキスト (PC5 G-d, edit.preferences.type; SPEC.md TEXT-02「環境設定 → テキスト」; PhotoCraft v0.2.0's prefs.rs
// TypePrefs, MIT): its page in the dialog and what 決める keeps (core/prefs_table's type.* — the keys type/…; the app's own
// memories under type/ are state.type.*, not here) — スマート引用符, 欠落グリフの保護, フォント名を英語で表示, Esc キーで確定, フォント
// プレビューのサイズ, 新規テキストレイヤーにプレースホルダーテキスト, 最近使用したフォントの表示数, and スペルチェック's user
// dictionary (one word a line) and its 表記の表….

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QWidget>

#include <algorithm>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/preferences.hpp"
#include "app/theme.hpp"
#include "app/type_dialogs.hpp"
#include "core/prefs_table.hpp"
#include "service/spell.hpp"

namespace genko::app::preferences {

namespace {

core::Json pref(const char* path) {
    const auto store = settings();
    return core::prefs::value(*store, path);
}

QCheckBox* check(QWidget* page, const char* name, const char* path, const QString& label) {
    auto* box = new QCheckBox(label, page);
    box->setObjectName(QString::fromLatin1(name));
    box->setChecked(pref(path) == core::Json(true));
    return box;
}

}  // namespace

QWidget* type_page(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* rows = new QFormLayout(page);
    rows->addRow(QString(), check(page, "prefs_type_smart_quotes", "type.smartQuotes", QStringLiteral("スマート引用符を使用")));
    rows->addRow(QString(), check(page, "prefs_type_glyph_protection", "type.missingGlyphProtection", QStringLiteral("欠落グリフの保護を有効にする")));
    rows->addRow(QString(), check(page, "prefs_type_font_names_english", "type.showFontNamesInEnglish", QStringLiteral("フォント名を英語で表示")));
    rows->addRow(QString(), check(page, "prefs_type_esc_commits", "type.useEscToCommit", QStringLiteral("Esc キーを使用してテキストを確定")));
    rows->addRow(QString(), check(page, "prefs_type_placeholder", "type.fillNewTypeLayersWithPlaceholder", QStringLiteral("新規テキストレイヤーにプレースホルダーテキストを挿入")));
    auto* preview = new QComboBox(page);
    preview->setObjectName(QStringLiteral("prefs_type_font_preview"));
    for (const auto& [label, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"なし", "off"}, {"小", "small"}, {"中", "medium"}, {"大", "large"}, {"特大", "extraLarge"}, {"極大", "huge"}})
        preview->addItem(QString::fromUtf8(label), QString::fromLatin1(value));
    preview->setCurrentIndex(std::max(0, preview->findData(QString::fromStdString(pref("type.fontPreview").get<std::string>()))));
    rows->addRow(QStringLiteral("フォントプレビューのサイズ"), preview);
    auto* recent = new QSpinBox(page);
    recent->setObjectName(QStringLiteral("prefs_type_recent_fonts"));
    recent->setRange(0, 50);
    recent->setValue(static_cast<int>(pref("type.recentFonts").get<double>()));
    rows->addRow(QStringLiteral("最近使用したフォントの表示数"), recent);
    auto* words = new QPlainTextEdit(page);
    words->setObjectName(QStringLiteral("prefs_type_user_dictionary"));
    words->setPlaceholderText(QStringLiteral("スペルチェックで誤りとしない語（1 行に 1 語）"));
    words->setPlainText(QString::fromStdString(pref("type.userDictionary").get<std::string>()).split(QLatin1Char('\t'), Qt::SkipEmptyParts).join(QLatin1Char('\n')));
    words->setMaximumHeight(110);
    rows->addRow(QStringLiteral("ユーザー辞書"), words);
    auto* table = new QPushButton(QStringLiteral("表記の表…"), page);
    table->setObjectName(QStringLiteral("prefs_type_notations"));
    table->setToolTip(QStringLiteral("表記の点検で直すよう知らせる書き方（例: 出来る → できる）"));
    // (its rows kept with the page until 決める writes them — やめる leaves the settings as they were: review P3-8)
    QObject::connect(table, &QPushButton::clicked, page, [page, table] {
        std::optional<std::vector<service::spell::Notation>> staged;
        if (table->property("genko_notations").isValid())
            staged = service::spell::notations_of_text(table->property("genko_notations").toString().toStdString());
        NotationDialog dialog(page, staged);
        if (ask::exec(&dialog) == QDialog::Accepted)
            table->setProperty("genko_notations", QString::fromStdString(service::spell::notations_json(dialog.rows()).dump()));
    });
    rows->addRow(QStringLiteral("表記の点検"), table);
    auto* hint = new QLabel(QStringLiteral("文字の単位は「単位・定規」で決めます。ユーザー辞書と表記の表は、このパソコンのスペルチェック（編集 → スペルチェック…、genko spell）が使います。"), page);
    hint->setWordWrap(true);
    theme::role(hint, "hint");
    rows->addRow(QString(), hint);
    return page;
}

void save_type_page(QWidget* page) {
    if (page == nullptr) return;
    const auto store = settings();
    const auto flag = [&](const char* name, const char* key) {
        if (auto* box = page->findChild<QCheckBox*>(QString::fromLatin1(name))) store->setValue(QString::fromLatin1(key), box->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
    };
    flag("prefs_type_smart_quotes", "type/smart_quotes");
    flag("prefs_type_glyph_protection", "type/glyph_protection");
    flag("prefs_type_font_names_english", "type/font_names_english");
    flag("prefs_type_esc_commits", "type/esc_commits");
    flag("prefs_type_placeholder", "type/placeholder");
    if (auto* box = page->findChild<QComboBox*>(QStringLiteral("prefs_type_font_preview"))) store->setValue(QStringLiteral("type/font_preview"), box->currentData());
    if (auto* box = page->findChild<QSpinBox*>(QStringLiteral("prefs_type_recent_fonts"))) store->setValue(QStringLiteral("type/recent_fonts"), box->value());
    if (auto* words = page->findChild<QPlainTextEdit*>(QStringLiteral("prefs_type_user_dictionary"))) {
        QStringList kept;
        QSet<QString> seen;  // (each once, whatever its case: by its lowercase — review P3-8)
        for (const QString& w : words->toPlainText().split(QLatin1Char('\n'))) {
            const QString t = w.trimmed();
            if (!t.isEmpty() && !seen.contains(t.toLower())) {
                seen.insert(t.toLower());
                kept << t;
            }
        }
        store->setValue(QStringLiteral("type/user_dictionary"), kept.join(QLatin1Char('\t')));
    }
    if (auto* table = page->findChild<QPushButton*>(QStringLiteral("prefs_type_notations")); table != nullptr && table->property("genko_notations").isValid())
        store->setValue(QStringLiteral("type/notations"), table->property("genko_notations").toString());
}

}  // namespace genko::app::preferences
