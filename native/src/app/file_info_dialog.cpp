// ファイル情報… (app/file_info_dialog.hpp).

#include "app/file_info_dialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>

#include "app/look.hpp"
#include "app/theme.hpp"
#include "core/file_info.hpp"

namespace genko::app {

using core::Json;

namespace {

QString qs(const std::string& text) { return QString::fromStdString(text); }

QString join_keywords(const std::vector<std::string>& keywords) {
    QStringList out;
    for (const std::string& k : keywords) out << qs(k);
    return out.join(QStringLiteral("; "));
}

QString number(double v) { return QString::number(v, 'g', 6); }

// 元のファイル: its name, format, size, resolution, bits and colours.
QString source_words(const core::NativeSource& s) {
    QString text = QStringLiteral("%1（%2）\n%3 × %4 px").arg(qs(s.name), qs(s.format).toUpper()).arg(s.width_px).arg(s.height_px);
    if (s.dpi) text += QStringLiteral("、%1 ppi").arg(number(s.dpi->first));
    text += QStringLiteral("、%1 bit、%2").arg(s.depth).arg(qs(s.mode));
    return text;
}

QString camera_words(const core::CameraInfo& c) {
    QStringList parts;
    const QString who = (qs(c.make) + QLatin1Char(' ') + qs(c.model)).trimmed();
    if (!who.isEmpty()) parts << who;
    if (c.iso) parts << QStringLiteral("ISO %1").arg(number(*c.iso));
    if (c.exposure) parts << (*c.exposure < 1 && *c.exposure > 0 ? QStringLiteral("1/%1 秒").arg(std::lround(1.0 / *c.exposure)) : QStringLiteral("%1 秒").arg(number(*c.exposure)));
    if (c.aperture) parts << QStringLiteral("f/%1").arg(number(*c.aperture));
    if (c.focal) parts << QStringLiteral("%1 mm").arg(number(*c.focal));
    return parts.join(QStringLiteral("、"));
}

}  // namespace

FileInfoDialog::FileInfoDialog(QWidget* parent, const core::Document& doc, const core::Page* page) : QDialog(parent) {
    setObjectName(QStringLiteral("fileInfoDialog"));
    setWindowTitle(QStringLiteral("ファイル情報"));
    const core::FileInfo info = doc.file_info.value_or(core::FileInfo{});
    title = new QLineEdit(qs(doc.title));
    author = new QLineEdit(qs(info.author));
    author_title = new QLineEdit(qs(info.author_title));
    description = new QPlainTextEdit(qs(info.description));
    description->setFixedHeight(72);
    keywords = new QLineEdit(join_keywords(info.keywords));
    keywords->setPlaceholderText(QStringLiteral("セミコロン（;）かカンマで区切ります"));
    copyright_status = new QComboBox;
    for (const auto& [label, value] : {std::pair{"不明", "unknown"}, {"著作権あり", "copyrighted"}, {"パブリックドメイン", "public_domain"}})
        copyright_status->addItem(QString::fromUtf8(label), QString::fromLatin1(value));
    copyright_status->setCurrentIndex(std::max(0, copyright_status->findData(info.copyright_status.empty() ? QStringLiteral("unknown") : qs(info.copyright_status))));
    copyright = new QPlainTextEdit(qs(info.copyright));
    copyright->setFixedHeight(56);
    copyright_url = new QLineEdit(qs(info.copyright_url));
    const core::NativeSource* from = page != nullptr && page->native_source ? &*page->native_source : nullptr;
    source = new QLabel(from != nullptr ? source_words(*from) : QStringLiteral("（Genko で作った原稿です）"));
    source->setWordWrap(true);
    camera = new QLabel(from != nullptr && from->camera ? camera_words(*from->camera) : QStringLiteral("（ありません）"));
    camera->setWordWrap(true);
    for (QLabel* label : {source, camera}) theme::role(label, "hint");
    QFormLayout* rows = look::form();
    rows->addRow(look::section(QStringLiteral("基本")));
    rows->addRow(QStringLiteral("ドキュメントのタイトル"), title);
    rows->addRow(QStringLiteral("作成者"), author);
    rows->addRow(QStringLiteral("作成者の役職"), author_title);
    rows->addRow(QStringLiteral("説明"), description);
    rows->addRow(QStringLiteral("キーワード"), keywords);
    rows->addRow(QStringLiteral("著作権情報"), copyright_status);
    rows->addRow(QStringLiteral("著作権表示"), copyright);
    rows->addRow(QStringLiteral("著作権情報 URL"), copyright_url);
    rows->addRow(look::section(QStringLiteral("カメラデータ")));
    rows->addRow(QString(), camera);
    rows->addRow(look::section(QStringLiteral("元のファイル")));
    rows->addRow(QString(), source);
    auto* note = new QLabel(QStringLiteral("書き出すファイル（PNG・JPEG・TIFF・PSD）とコピーを保存… のファイルに、XMP として入ります。"));
    note->setWordWrap(true);
    theme::role(note, "hint");
    rows->addRow(QString(), note);
    look::quiet_labels(rows);
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("決める"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    look::frame(this, look::header(QStringLiteral("ファイル情報"), QStringLiteral("作成者・説明・キーワード・著作権情報")), rows, nullptr, look::footer(buttons));
    look::fit_to_screen(this, QSize(620, 640));
    was_ = values();
}

Json FileInfoDialog::values() const {
    Json out{{"op", "set_file_info"}};
    out["title"] = title->text().trimmed().toStdString();
    out["author"] = author->text().trimmed().toStdString();
    out["author_title"] = author_title->text().trimmed().toStdString();
    out["description"] = description->toPlainText().trimmed().toStdString();
    out["keywords"] = keywords->text().trimmed().toStdString();
    out["copyright_status"] = copyright_status->currentData().toString().toStdString();
    out["copyright"] = copyright->toPlainText().trimmed().toStdString();
    out["copyright_url"] = copyright_url->text().trimmed().toStdString();
    return out;
}

std::optional<Json> FileInfoDialog::op() const {
    const Json now = values();
    Json changed{{"op", "set_file_info"}};
    for (const auto& [key, value] : now.items()) {
        if (key != "op" && (!was_.contains(key) || was_[key] != value)) changed[key] = value;
    }
    if (changed.size() == 1) return std::nullopt;
    return changed;
}

}  // namespace genko::app
