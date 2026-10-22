// 開く… and 指定形式で開く… (app/open_dialog.hpp).

#include "app/open_dialog.hpp"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>

#include "app/color_settings_dialog.hpp"
#include "app/preferences.hpp"
#include "core/paths.hpp"

namespace genko::app {

namespace fs = std::filesystem;
namespace codecs = render::codecs;

namespace {

bool is_book(const QString& path) { return QFileInfo(path).isDir() && QFileInfo::exists(path + QStringLiteral("/project.json")); }

// The patterns of a format's extensions ("*.png *.PNG").
QString patterns(codecs::Format format) {
    QStringList out;
    for (const std::string_view ext : codecs::extensions(format)) {
        const QString e = QString::fromLatin1(ext.data(), static_cast<qsizetype>(ext.size()));
        out << QStringLiteral("*.") + e << QStringLiteral("*.") + e.toUpper();
    }
    return out.join(QLatin1Char(' '));
}

}  // namespace

formats::OpenOptions file_open_options() {
    namespace cs = color_settings;
    const cs::Settings s = cs::load();
    formats::OpenOptions options;
    const auto bytes = [&s](cs::Space space) -> std::string {
        const cs::Working w = cs::working(space, s);
        return w.usable && w.icc ? *w.icc : std::string();
    };
    options.working_rgb = bytes(cs::Space::Rgb);
    options.working_cmyk = bytes(cs::Space::Cmyk);
    options.working_gray = bytes(cs::Space::Gray);
    options.ignore_exif_profile = preferences::ignore_exif_profile();
    return options;
}

QString open_filters() {
    // (the formats this build reads; PSD・PSB, Camera Raw and PhotoCraft's .pcraft join when their groups read them)
    QStringList all{QStringLiteral("*.genko")};
    QStringList each{QStringLiteral("Genko の原稿 (*.genko)")};
    for (const codecs::Format format : codecs::kFormats) {
        if (!codecs::caps(format).read) continue;
        all << patterns(format);
        const std::string_view name = codecs::name(format);
        each << QStringLiteral("%1 (%2)").arg(QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size())), patterns(format));
    }
    return QStringLiteral("すべての形式 (%1);;%2;;すべてのファイル (*)").arg(all.join(QLatin1Char(' ')), each.join(QStringLiteral(";;")));
}

OpenDialog::OpenDialog(QWidget* parent, bool as) : QFileDialog(parent) {
    setObjectName(QStringLiteral("openDialog"));
    setWindowTitle(as ? QStringLiteral("指定形式で開く") : QStringLiteral("開く"));
    setOption(QFileDialog::DontUseNativeDialog, true);  // (a .genko folder and files in one window)
    setFileMode(QFileDialog::ExistingFiles);
    setAcceptMode(QFileDialog::AcceptOpen);
    setNameFilters(open_filters().split(QStringLiteral(";;")));
    setLabelText(QFileDialog::Accept, QStringLiteral("開く"));
    setLabelText(QFileDialog::Reject, QStringLiteral("やめる"));
    // (a .genko folder double-clicked: the book, not its inside)
    connect(this, &QFileDialog::directoryEntered, this, [this](const QString& dir) {
        if (!is_book(dir)) return;
        choose({dir}, format());
        setDirectory(QFileInfo(dir).absolutePath());
        QDialog::accept();
    });
    if (as) {
        format_ = new QComboBox(this);
        format_->setObjectName(QStringLiteral("openAsFormat"));
        for (const codecs::Format format : codecs::kFormats) {
            if (!codecs::caps(format).read) continue;
            const std::string_view name = codecs::name(format);
            format_->addItem(QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size())), static_cast<int>(format));
        }
        if (auto* grid = qobject_cast<QGridLayout*>(layout())) {
            const int row = grid->rowCount();
            grid->addWidget(new QLabel(QStringLiteral("形式:"), this), row, 0);
            grid->addWidget(format_, row, 1);
        }
    }
}

std::optional<codecs::Format> OpenDialog::format() const {
    if (format_ == nullptr || format_->currentIndex() < 0) return std::nullopt;
    return static_cast<codecs::Format>(format_->currentData().toInt());
}

void OpenDialog::choose(const QStringList& paths, std::optional<codecs::Format> as) {
    chosen_.clear();
    for (const QString& path : paths) chosen_.push_back(core::path_from_utf8(QDir::cleanPath(path).toStdString()));
    if (format_ != nullptr && as) format_->setCurrentIndex(std::max(0, format_->findData(static_cast<int>(*as))));
}

void OpenDialog::accept() {
    const QStringList files = selectedFiles();
    // (a folder chosen: a book is opened; another folder is gone into, as the window does)
    if (files.size() == 1 && QFileInfo(files.front()).isDir() && !is_book(files.front())) {
        QFileDialog::accept();
        return;
    }
    choose(files, format());
    QDialog::accept();
}

}  // namespace genko::app
