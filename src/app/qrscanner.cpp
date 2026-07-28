#include "qrscanner.h"
#include "qrdecode.h"

#include <QImage>
#include <QVideoFrame>

namespace app {

namespace {

// Intervalle minimal entre deux décodages (ms).
constexpr qint64 kThrottleMs = 150;

} // namespace

QrScanner::QrScanner(QObject* parent)
    : QObject(parent)
{
    m_since.start();
}

void QrScanner::setVideoSink(QVideoSink* sink)
{
    if (m_sink == sink)
        return;

    if (m_sink)
        disconnect(m_sink, nullptr, this, nullptr);

    m_sink = sink;

    if (m_sink) {
        connect(m_sink, &QVideoSink::videoFrameChanged,
                this,   &QrScanner::onFrame);
    }

    emit videoSinkChanged();
}

void QrScanner::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (m_active)
        m_found = false;   // nouveau scan : réarmer
    emit activeChanged();
}

void QrScanner::onFrame(const QVideoFrame& frame)
{
    if (!m_active || m_found || !frame.isValid())
        return;

    // Décoder chaque trame d'un flux 30 fps saturerait le thread UI pour rien :
    // un QR reste dans le champ bien plus longtemps que 150 ms.
    if (m_since.elapsed() < kThrottleMs)
        return;
    m_since.restart();

    const QString text = decodeQr(frame.toImage());
    if (text.isEmpty())
        return;

    m_found = true;
    emit codeDetected(text);
}

} // namespace app
