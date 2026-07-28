#pragma once

#include <QObject>

namespace app {

// Permissions runtime, exposé à QML comme singleton `Permissions`.
//
// L'API QPermission n'existe qu'à partir de Qt 6.5 (Android est en 6.8, le desktop
// Linux packagé est en 6.4) : sous 6.5, et sur les plateformes sans permissions
// runtime, on considère l'accès accordé — c'est le cas de Linux.
class Permissions : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool cameraGranted READ cameraGranted NOTIFY cameraGrantedChanged)

public:
    explicit Permissions(QObject* parent = nullptr);

    bool cameraGranted() const { return m_cameraGranted; }

    // Demande la permission caméra si elle n'est pas encore tranchée.
    Q_INVOKABLE void requestCamera();

signals:
    void cameraGrantedChanged();

private:
    void setCameraGranted(bool granted);

    bool m_cameraGranted = false;
};

} // namespace app
