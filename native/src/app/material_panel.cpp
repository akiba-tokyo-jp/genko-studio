#include "app/material_panel.hpp"
#include "app/config.hpp"
#include "app/material_preview_cache.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <utility>

#include <QComboBox>
#include <QFile>
#include <QHash>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QResource>
#include <QSet>
#include <QVBoxLayout>

// Q_INIT_RESOURCE declares the resource symbol in the current namespace; keep this function global.
static void initialize_material_resources() { Q_INIT_RESOURCE(genko_materials); }

namespace genko::app {

namespace {
// The original Python library remains read-only. No absolute/nested/reparse image path is accepted.
QJsonArray user_entries(const QString& root) {
    const QFileInfo directory(root), file(root + "/library.json");
    if (!directory.isDir() || directory.isSymLink() || directory.canonicalFilePath()!=directory.absoluteFilePath() ||
        file.isSymLink() || !file.isFile() || file.size()> (1<<20)) return {};
    QFile input(file.absoluteFilePath());if(!input.open(QIODevice::ReadOnly))return {};
    const auto document=QJsonDocument::fromJson(input.read((1<<20)+1));
    if(!document.isArray())return {};
    QJsonArray result;QSet<QString> ids;
    const QSet<QString> kinds={"tone","effect","image","lines","lettering","brush","prim"};
    for(const auto& value:document.array()) {
        if(!value.isObject())continue;
        auto entry=value.toObject();const auto id=entry.value("id").toString();
        if(!id.startsWith("u-") || id.size()>128 || ids.contains(id) || !kinds.contains(entry.value("kind").toString()))continue;
        if(entry.value("name").toString().size()>512 || entry.value("folder").toString().size()>512)continue;
        entry["user"]=true;entry.remove("preview");ids.insert(id);result.append(entry);
    }
    return result;
}
using UserPreview=std::pair<QImage,bool>;
UserPreview user_preview(const QString& library,const QString& filename,const QString& cacheRoot) {
    const QFileInfo directory(library),file(QDir(library).filePath(filename));
    if(filename.isEmpty() || filename=="." || filename==".." || filename.contains('/') || filename.contains('\\') ||
       directory.isSymLink() || directory.canonicalFilePath()!=directory.absoluteFilePath() || file.isSymLink() || !file.isFile() ||
       file.canonicalPath()!=directory.canonicalFilePath() || file.size()<=0 || file.size()>(64<<20))return {};
    QFile source(file.absoluteFilePath());if(!source.open(QIODevice::ReadOnly))return {};
    const QByteArray bytes=source.read((64<<20)+1);if(bytes.size()>(64<<20))return {};
    QBuffer probe;probe.setData(bytes);probe.open(QIODevice::ReadOnly);QImageReader reader(&probe,"PNG");const auto dimensions=reader.size();
    if(!dimensions.isValid() || qint64(dimensions.width())*dimensions.height()>4194304)return {};
    MaterialPreviewSpec spec;spec.render_settings="user-image/png/preserve-aspect";
    const QByteArray digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
    MaterialPreviewCache cache(cacheRoot);bool generated=false;
    QImage image;
    try { image=cache.obtain(digest,spec,[&]{
        generated=true;reader.setScaledSize(dimensions.scaled(spec.pixels,Qt::KeepAspectRatio));
        const QImage decoded=reader.read();if(decoded.isNull())return QImage();
        QImage preview(spec.pixels,QImage::Format_ARGB32);preview.fill(QColor::fromRgba(spec.background));
        QPainter painter(&preview);painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QSize size=decoded.size().scaled(spec.pixels,Qt::KeepAspectRatio);
        painter.drawImage(QRect(QPoint((spec.pixels.width()-size.width())/2,(spec.pixels.height()-size.height())/2),size),decoded);painter.end();
        return preview;
    }); } catch (...) { return {}; } // A failed decode/allocation is a failed preview, never a GUI exception.
    return {image,generated};
}
}

QWidget* make_builtin_material_panel(QWidget* parent, std::function<void(const QString&, const QString&)> activate) {
    initialize_material_resources();
    auto* widget = new QWidget(parent);
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(6, 6, 6, 6);
    auto* label = new QLabel(QStringLiteral("素材（プレビュー）"));
    widget->setProperty("userPreviewGenerations",0);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* folder = new QComboBox;
    folder->setObjectName(QStringLiteral("materialFolder"));
    folder->addItem(QStringLiteral("すべて"), QString());
    auto* search = new QLineEdit;
    search->setObjectName(QStringLiteral("materialSearch"));
    search->setPlaceholderText(QStringLiteral("名前・タグ・種類で探す"));
    search->setClearButtonEnabled(true);
    auto* list = new QListWidget;
    list->setObjectName(QStringLiteral("materialList"));
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(56, 56));
    list->setGridSize(QSize(84, 100));
    list->setResizeMode(QListView::Adjust);
    list->setWordWrap(true);
    list->setMinimumSize(100, 100);
    layout->addWidget(folder);
    layout->addWidget(search);
    layout->addWidget(list, 1);

    QFile input(QStringLiteral(":/genko/materials/catalog.json"));
    if (!input.open(QIODevice::ReadOnly)) {
        label->setText(QStringLiteral("組込み素材の一覧を読めませんでした。原稿は変更していません。"));
        return widget;
    }
    const QJsonDocument catalog = QJsonDocument::fromJson(input.readAll());
    if (!catalog.isArray()) {
        label->setText(QStringLiteral("組込み素材の一覧が壊れています。原稿は変更していません。"));
        return widget;
    }
    const QString library=QString::fromStdString((config_dir()/"materials").string());
    const QString cacheRoot=QString::fromStdString((config_dir()/"cache"/"material-previews").string());
    QJsonArray entries = catalog.array();
    for(const auto& value:user_entries(library))entries.append(value);
    // Python materials.KIND_WORDS; do not add the raw internal kind to the person's search corpus.
    static const QHash<QString, QString> kind_words = {
        {QStringLiteral("tone"), QStringLiteral("トーン")},
        {QStringLiteral("effect"), QStringLiteral("効果線")},
        {QStringLiteral("image"), QStringLiteral("画像")},
        {QStringLiteral("lines"), QStringLiteral("パーツ 線")},
        {QStringLiteral("lettering"), QStringLiteral("描き文字 効果音")},
        {QStringLiteral("brush"), QStringLiteral("ブラシ")},
        {QStringLiteral("prim"), QStringLiteral("3d 立体")},
    };
    QSet<QString> folders;
    for (const QJsonValue& value : entries) {
        const QString name = value.toObject().value(QStringLiteral("folder")).toString();
        if (!name.isEmpty() && !folders.contains(name)) {
            folders.insert(name);
            folder->addItem(name, name);
        }
    }
    // Only one visible user image is in flight. Widgets and original libraries never enter the worker.
    auto* timer=new QTimer(widget);timer->setSingleShot(true);
    auto* watcher=new QFutureWatcher<UserPreview>(widget);
    QObject::connect(timer,&QTimer::timeout,widget,[list,watcher,library,cacheRoot]{
        if(!list->isVisible() || watcher->isRunning())return;
        list->doItemsLayout();
        for(int row=0;row<list->count();++row){auto* item=list->item(row);
            if(!item->data(Qt::UserRole+1).toBool() || item->data(Qt::UserRole+3).toBool() ||
               !list->visualItemRect(item).intersects(list->viewport()->rect()))continue;
            const QString filename=item->data(Qt::UserRole+2).toString();
            if(filename.isEmpty()){item->setData(Qt::UserRole+3,true);continue;}
            watcher->setProperty("entryId",item->data(Qt::UserRole));
            watcher->setProperty("entryFile",filename);
            watcher->setFuture(QtConcurrent::run([library,filename,cacheRoot]{return user_preview(library,filename,cacheRoot);}));
            break;
        }
    });
    QObject::connect(watcher,&QFutureWatcher<UserPreview>::finished,widget,[widget,list,watcher,timer]{
        UserPreview result;
        try { result=watcher->result(); } catch (...) { /* Future failures follow the same original-preserving UI path. */ }
        if(result.second)widget->setProperty("userPreviewGenerations",widget->property("userPreviewGenerations").toInt()+1);
        for(int row=0;row<list->count();++row){auto* item=list->item(row);
            if(item->data(Qt::UserRole)!=watcher->property("entryId") || item->data(Qt::UserRole+2)!=watcher->property("entryFile"))continue;
            item->setData(Qt::UserRole+3,true);
            if(!result.first.isNull())item->setIcon(QIcon(QPixmap::fromImage(result.first)));
            else item->setToolTip(item->toolTip()+QStringLiteral("（画像を読み込めません。原本は変更していません）"));
        }
        timer->start(0);
    });
    QObject::connect(list->verticalScrollBar(),&QScrollBar::valueChanged,widget,[timer](int){timer->start(0);});
    QObject::connect(list->verticalScrollBar(),&QScrollBar::rangeChanged,widget,[timer](int,int){timer->start(0);});
    if(activate)QObject::connect(list,&QListWidget::itemDoubleClicked,widget,[activate](QListWidgetItem* item){if(item)activate(item->data(Qt::UserRole).toString(),item->data(Qt::UserRole+4).toString());});
    const auto fill = [list, folder, search, entries, timer]() {
        const QString selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        list->clear();
        const QString where = folder->currentData().toString();
        const QStringList words = search->text().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QJsonValue& value : entries) {
            const QJsonObject entry = value.toObject();
            const QString name = entry.value(QStringLiteral("name")).toString();
            const QString category = entry.value(QStringLiteral("folder")).toString();
            if (!where.isEmpty() && category != where) continue;
            const QString kind = entry.value(QStringLiteral("kind")).toString();
            QString hay = name + QLatin1Char(' ') + category + QLatin1Char(' ') + kind_words.value(kind);
            for (const QJsonValue& tag : entry.value(QStringLiteral("tags")).toArray()) hay += QLatin1Char(' ') + tag.toString();
            bool matched = true;
            for (const QString& word : words) if (!hay.contains(word, Qt::CaseInsensitive)) { matched = false; break; }
            if (!matched) continue;
            const QString id = entry.value(QStringLiteral("id")).toString();
            // QIcon reads the prebuilt PNG on demand. No renderer/Python is invoked on startup or scrolling.
            auto* item = new QListWidgetItem(QIcon(entry.value(QStringLiteral("preview")).toString()), name, list);
            item->setData(Qt::UserRole, id);
            item->setData(Qt::UserRole+4,kind);
            const bool user=entry.value("user").toBool();
            item->setData(Qt::UserRole+1,user);
            if(user&&kind=="image")item->setData(Qt::UserRole+2,entry.value("file").toString());
            item->setData(Qt::UserRole+3,!user);
            item->setToolTip(category + QStringLiteral(" / ") + name);
            if (id == selected) list->setCurrentItem(item);
        }
        timer->start(0);
    };
    QObject::connect(search, &QLineEdit::textChanged, widget, [fill](const QString&) { fill(); });
    QObject::connect(folder, &QComboBox::currentIndexChanged, widget, [fill](int) { fill(); });
    fill();
    return widget;
}

}  // namespace genko::app
