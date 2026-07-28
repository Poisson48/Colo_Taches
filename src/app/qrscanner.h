#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVideoSink>

namespace app {

// Décodeur de QR code branché sur le flux de la caméra (QVideoSink de QML).
//
// Analyse au plus une image toutes les kThrottleMs : décoder chaque trame d'un flux
// 30 fps saturerait le thread UI sur mobile pour rien — un QR reste dans le champ
// bien plus longtemps.
//
// Émet codeDetected une seule fois par scan (le flux enverrait sinon le même code
// des dizaines de fois avant que la page ne se ferme).
class QrScanner : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVideoSink* videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    explicit QrScanner(QObject* parent = nullptr);

    QVideoSink* videoSink() const { return m_sink; }
    void setVideoSink(QVideoSink* sink);

    bool active() const { return m_active; }
    void setActive(bool active);

signals:
    void videoSinkChanged();
    void activeChanged();
    // Contenu textuel du QR (ici : une URI colotaches://join/…).
    void codeDetected(const QString& text);

private slots:
    void onFrame(const QVideoFrame& frame);

private:
    QVideoSink*   m_sink = nullptr;
    bool          m_active = true;
    bool          m_found = false;   // un code a déjà été émis pour ce scan
    QElapsedTimer m_since;
};

} // namespace app
