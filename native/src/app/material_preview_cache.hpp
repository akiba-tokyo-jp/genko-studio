#pragma once
#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <functional>

namespace genko::app {
struct MaterialPreviewSpec {
    QSize pixels{56, 56};
    int dpi = 110;
    QRgb background = qRgb(255, 255, 255);
    QByteArray render_settings;
    QString renderer_version = QStringLiteral("genko-native-material/1");
};

// Disposable preview data only. Never stores or deletes original material files.
// Call on demand on a worker; constructing the cache does not scan or render materials.
class MaterialPreviewCache {
public:
    explicit MaterialPreviewCache(QString root, int max_files = 1000, qint64 max_bytes = 64ll << 20);
    static QString key(const QByteArray& content_sha256, const MaterialPreviewSpec& spec);
    QImage obtain(const QByteArray& content_sha256, const MaterialPreviewSpec& spec,
                  const std::function<QImage()>& generate) const;
private:
    QString root_;
    int max_files_;
    qint64 max_bytes_;
};
} // namespace genko::app
