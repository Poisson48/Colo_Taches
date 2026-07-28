#pragma once

#include <QImage>
#include <QString>

namespace app {

// Décode le premier QR code trouvé dans l'image. Chaîne vide si aucun.
// Isolé de QrScanner (et de Qt Multimedia) pour être testable sans caméra.
QString decodeQr(const QImage& image);

} // namespace app
