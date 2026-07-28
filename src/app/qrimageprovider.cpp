#include "qrimageprovider.h"
#include "qrcodegen.hpp"

#include <QImage>
#include <QColor>

namespace app {

QrImageProvider::QrImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{}

QImage QrImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    Q_UNUSED(requestedSize)

    std::string uri = id.toStdString();
    if (uri.empty()) {
        QImage img(1, 1, QImage::Format_RGB32);
        img.fill(Qt::white);
        if (size) *size = img.size();
        return img;
    }

    qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
        uri.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);

    const int scale  = 8;
    const int border = 4;
    const int qrSize = qr.getSize();
    const int totalPx = (qrSize + 2 * border) * scale;

    QImage img(totalPx, totalPx, QImage::Format_RGB32);
    img.fill(Qt::white);

    for (int y = 0; y < qrSize; ++y) {
        for (int x = 0; x < qrSize; ++x) {
            if (qr.getModule(x, y)) {
                const int px = (x + border) * scale;
                const int py = (y + border) * scale;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        img.setPixel(px + dx, py + dy, qRgb(0, 0, 0));
                    }
                }
            }
        }
    }

    if (size) *size = img.size();
    return img;
}

} // namespace app
