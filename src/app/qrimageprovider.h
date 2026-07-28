#pragma once
#include <QQuickImageProvider>
#include <QString>

namespace app {

// Provides QR code images for URIs.
// Usage in QML: Image { source: "image://qr/" + encodeURIComponent(uri) }
// The id passed is the URI string (URL-decoded by Qt).
class QrImageProvider : public QQuickImageProvider {
public:
    QrImageProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

} // namespace app
