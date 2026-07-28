#include <QtTest>
#include <QImage>
#include <QPainter>

#include "../src/app/qrdecode.h"
#include "../src/core/pairing.h"

#include "qrcodegen.hpp"

using qrcodegen::QrCode;

// Rend un QR code en QImage, comme l'affiche la feuille de partage (modules noirs
// sur fond blanc, marge de 4 modules).
static QImage renderQr(const std::string& text, int scale = 8, int quiet = 4)
{
    const QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
    const int size  = qr.getSize();
    const int side  = (size + 2 * quiet) * scale;

    QImage img(side, side, QImage::Format_RGB32);
    img.fill(Qt::white);

    QPainter p(&img);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (qr.getModule(x, y))
                p.drawRect((x + quiet) * scale, (y + quiet) * scale, scale, scale);
    p.end();

    return img;
}

class TstQrDecode : public QObject
{
    Q_OBJECT

private slots:
    // Le QR affiché par un appareil doit être relu par l'autre : c'est tout
    // l'appairage. On teste la chaîne complète encode → décode → parseJoinUri.
    void pairingQrRoundTrip()
    {
        const std::vector<uint8_t> key(32, 0x5A);
        const std::string uri = core::buildJoinUri("liste-42", key, "Courses maison");

        const QString decoded = app::decodeQr(renderQr(uri));
        QCOMPARE(decoded.toStdString(), uri);

        const auto info = core::parseJoinUri(decoded.toStdString());
        QVERIFY(info.has_value());
        QCOMPARE(info->listId, std::string("liste-42"));
        QCOMPARE(info->key, key);
        QCOMPARE(info->title, std::string("Courses maison"));
    }

    // La caméra ne cadre jamais parfaitement : marges, échelle, et image renversée
    // (l'autre téléphone est en face). Le décodage doit encaisser tout ça.
    void decodesRotatedAndScaled()
    {
        const std::vector<uint8_t> key(32, 0x11);
        const std::string uri = core::buildJoinUri("l1", key, "Coloc");

        QImage img = renderQr(uri, 4);
        img = img.scaled(img.width() * 3, img.height() * 3, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
        QTransform rot;
        rot.rotate(180);
        img = img.transformed(rot);

        QCOMPARE(app::decodeQr(img).toStdString(), uri);
    }

    // Une image sans QR ne doit rien renvoyer (et surtout pas planter) : le scanner
    // appelle decodeQr sur chaque trame, la plupart ne contiennent rien.
    void emptyImageYieldsNothing()
    {
        QImage blank(320, 240, QImage::Format_RGB32);
        blank.fill(Qt::gray);

        QVERIFY(app::decodeQr(blank).isEmpty());
        QVERIFY(app::decodeQr(QImage()).isEmpty());
    }
};

QTEST_MAIN(TstQrDecode)
#include "tst_qrdecode.moc"
