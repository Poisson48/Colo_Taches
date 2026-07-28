#include "qrdecode.h"

// zxing-cpp récupéré par FetchContent expose ses en-têtes à plat (core/src) ;
// une installation système les range sous ZXing/. Les deux doivent marcher.
#if __has_include(<ZXing/ReadBarcode.h>)
#  include <ZXing/ReadBarcode.h>
#else
#  include <ReadBarcode.h>
#endif

namespace app {

namespace {

// Au-delà, l'image est réduite : un QR reste lisible bien en dessous de la
// résolution d'une caméra moderne, et le décodage coûte en O(pixels).
constexpr int kMaxWidth = 720;

} // namespace

QString decodeQr(const QImage& source)
{
    if (source.isNull())
        return {};

    QImage image = source;
    if (image.width() > kMaxWidth)
        image = image.scaledToWidth(kMaxWidth, Qt::SmoothTransformation);

    // ZXing travaille en luminance : la conversion explicite rend le résultat
    // indépendant du format que livre la caméra.
    image = image.convertToFormat(QImage::Format_Grayscale8);

    const ZXing::ImageView view(image.constBits(),
                                image.width(),
                                image.height(),
                                ZXing::ImageFormat::Lum,
                                static_cast<int>(image.bytesPerLine()));

    ZXing::ReaderOptions options;
    options.setFormats(ZXing::BarcodeFormat::QRCode);
    options.setTryHarder(true);
    options.setTryRotate(true);

    const auto result = ZXing::ReadBarcode(view, options);
    if (!result.isValid())
        return {};

    return QString::fromStdString(result.text());
}

} // namespace app
