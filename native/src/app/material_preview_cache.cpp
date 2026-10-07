#include "app/material_preview_cache.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace genko::app {
namespace {
const QString kKey=QStringLiteral("genko-material-cache-key");
const QString kOwner=QStringLiteral("genko-material-cache-owned");
constexpr qint64 kEncodedLimit=2ll<<20;
QString usable_root(const QString& root,bool create) {
    if (root.isEmpty() || !QDir::isAbsolutePath(root)) return {};
    const QString absolute=QDir::cleanPath(root);
    // Reject root/ancestor symlinks: disposable cache writes must not follow them into originals.
    QFileInfo info(absolute);
    if (!info.exists()) {
        if (!create) return {};
        QFileInfo ancestor(QFileInfo(absolute).absolutePath());
        while (!ancestor.exists() && ancestor.absoluteFilePath()!=QStringLiteral("/"))
            ancestor=QFileInfo(ancestor.absolutePath());
        if (ancestor.isSymLink() || !ancestor.isDir() || ancestor.canonicalFilePath()!=ancestor.absoluteFilePath()) return {};
        if (!QDir().mkpath(absolute)) return {};
        info=QFileInfo(absolute);
    }
    if (info.isSymLink() || !info.isDir() || info.canonicalFilePath()!=absolute || absolute==QStringLiteral("/")) return {};
    return absolute;
}
QImage load(const QString& name,const QString& key,const QSize& size=QSize()) {
    const QFileInfo info(name);
    if (!info.isFile() || info.isSymLink() || info.size()<=0 || info.size()>kEncodedLimit) return {};
    QFile file(name);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&file,"PNG");
    const QSize dimensions=reader.size();
    if (dimensions.width()<1 || dimensions.height()<1 || dimensions.width()>512 || dimensions.height()>512 ||
        (size.isValid() && size!=dimensions)) return {};
    QImage image=reader.read();
    if (image.isNull() || image.text(kKey)!=key || image.text(kOwner)!=QStringLiteral("1")) return {};
    image.setText(kKey,{});image.setText(kOwner,{});
    return image.convertToFormat(QImage::Format_ARGB32);
}
void prune(const QString& root,int max_files,qint64 max_bytes) {
    if (usable_root(root,false)!=root) return;
    const QRegularExpression owned(QStringLiteral("^[0-9a-f]{64}\\.png$"));
    std::vector<QFileInfo> entries;
    qint64 bytes=0;
    for (const auto& info : QDir(root).entryInfoList({QStringLiteral("*.png")},QDir::Files|QDir::NoSymLinks,QDir::Name)) {
        if (!owned.match(info.fileName()).hasMatch() || load(info.absoluteFilePath(),info.baseName()).isNull()) continue;
        entries.push_back(info);bytes+=info.size();
    }
    std::sort(entries.begin(),entries.end(),[](const QFileInfo& a,const QFileInfo& b) {
        return a.lastModified()!=b.lastModified() ? a.lastModified()<b.lastModified() : a.fileName()<b.fileName();
    });
    auto count=entries.size();
    for (const auto& info : entries) {
        if (count<=static_cast<std::size_t>(max_files) && bytes<=max_bytes) break;
        // Recheck binding immediately before deleting only a recognized disposable preview.
        if (usable_root(root,false)!=root) return;
        if (!load(info.absoluteFilePath(),info.baseName()).isNull() && QFile::remove(info.absoluteFilePath())) {
            --count;bytes-=info.size();
        }
    }
}
}
MaterialPreviewCache::MaterialPreviewCache(QString root,int max_files,qint64 max_bytes)
    : root_(std::move(root)),max_files_(max_files),max_bytes_(max_bytes) {
    if (max_files<0 || max_files>1000 || max_bytes<0 || max_bytes>(64ll<<20))
        throw std::invalid_argument("material preview cache budget");
}
QString MaterialPreviewCache::key(const QByteArray& digest,const MaterialPreviewSpec& spec) {
    if (digest.size()!=32 || spec.pixels.width()<1 || spec.pixels.height()<1 ||
        spec.pixels.width()>512 || spec.pixels.height()>512 || spec.dpi<1 || spec.dpi>9600 ||
        spec.renderer_version.isEmpty() || spec.renderer_version.size()>4096 || spec.render_settings.size()>(1<<20))
        throw std::invalid_argument("material preview key input");
    QByteArray data;
    QDataStream stream(&data,QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << digest << spec.pixels << spec.dpi << spec.background << spec.render_settings << spec.renderer_version;
    return QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex());
}
QImage MaterialPreviewCache::obtain(const QByteArray& digest,const MaterialPreviewSpec& spec,
                                  const std::function<QImage()>& generate) const {
    const QString id=key(digest,spec);
    const bool enabled=max_files_>0 && max_bytes_>0;
    const QString root=enabled?usable_root(root_,false):QString();
    const QString name=root+QLatin1Char('/')+id+QStringLiteral(".png");
    if (!root.isEmpty()) {
        QImage cached=load(name,id,spec.pixels);
        if (!cached.isNull()) {
            QFile touch(name);
            if (touch.open(QIODevice::ReadOnly)) touch.setFileTime(QDateTime::currentDateTimeUtc(),QFileDevice::FileModificationTime);
            return cached;
        }
    }
    QImage generated=generate();
    if (generated.isNull() || generated.size()!=spec.pixels)
        throw std::invalid_argument("material preview renderer dimensions");
    generated=generated.convertToFormat(QImage::Format_ARGB32);
    if (!enabled || root_.isEmpty()) return generated;
    QImage encoded=generated;
    encoded.setText(kKey,id);encoded.setText(kOwner,QStringLiteral("1"));
    QByteArray bytes;QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !encoded.save(&buffer,"PNG") || bytes.size()>max_bytes_ || bytes.size()>kEncodedLimit)
        return generated;
    const QString writable=usable_root(root_,true);
    if (writable.isEmpty()) return generated;
    const QString target=writable+QLatin1Char('/')+id+QStringLiteral(".png");
    const QFileInfo existing(target);
    if (existing.isSymLink() || (existing.exists() && !existing.isFile())) return generated;
    // An unbound/corrupt collision could be an original; regenerate in memory, never replace it.
    if (existing.exists() && load(target,id).isNull()) return generated;
    QSaveFile file(target);file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return generated;
    if (file.write(bytes)!=bytes.size()) {file.cancelWriting();return generated;}
    if (!file.commit()) return generated;
    prune(writable,max_files_,max_bytes_);
    return generated;
}
} // namespace genko::app
