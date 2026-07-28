#include "permissions.h"

#include <QCoreApplication>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#  include <QPermissions>
#endif

namespace app {

Permissions::Permissions(QObject* parent)
    : QObject(parent)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
    // Pas d'API de permissions : rien à demander (Linux desktop).
    m_cameraGranted = true;
#endif
}

void Permissions::setCameraGranted(bool granted)
{
    if (m_cameraGranted == granted)
        return;
    m_cameraGranted = granted;
    emit cameraGrantedChanged();
}

void Permissions::requestCamera()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const QCameraPermission permission;

    switch (qApp->checkPermission(permission)) {
    case Qt::PermissionStatus::Granted:
        setCameraGranted(true);
        return;
    case Qt::PermissionStatus::Denied:
        // Refus définitif : c'est à l'utilisateur de rouvrir les réglages système.
        setCameraGranted(false);
        return;
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(permission, this, [this](const QPermission& result) {
            setCameraGranted(result.status() == Qt::PermissionStatus::Granted);
        });
        return;
    }
#else
    setCameraGranted(true);
#endif
}

} // namespace app
