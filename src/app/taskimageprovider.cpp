#include "taskimageprovider.h"

#include "../store/database.h"

#include <QImage>

namespace app {

TaskImageProvider::TaskImageProvider(const QString& dbPath)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_dbPath(dbPath)
{}

QImage TaskImageProvider::requestImage(const QString& id, QSize* size,
                                       const QSize& requestedSize)
{
    // Une connexion par thread de chargement, ouverte au premier besoin et gardée
    // pour la suite (l'ouverture SQLite n'est pas gratuite).
    thread_local store::Database db;
    if (!db.isOpen() && !db.open(m_dbPath))
        return {};

    const QString sha = id.section(QLatin1Char('?'), 0, 0);
    const QByteArray blob = db.getImage(sha.toStdString());

    QImage img;
    if (!blob.isEmpty())
        img.loadFromData(blob);
    if (img.isNull()) {
        // Blob pas encore reçu du relais : image vide, la vue réessaiera à la
        // prochaine révision (AppController.imageRevision).
        if (size) *size = QSize();
        return {};
    }

    if (size) *size = img.size();
    if (requestedSize.isValid() && !requestedSize.isEmpty())
        return img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img;
}

} // namespace app
