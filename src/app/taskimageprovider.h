#pragma once

#include <QQuickImageProvider>
#include <QString>

namespace app {

// Sert les photos de tâches à QML : image://taskimg/<sha256hex>[?r=N].
// Le suffixe ?r=N (révision) ne sert qu'à invalider le cache de la vue quand un
// blob arrive d'un relais après coup.
//
// Les requêtes arrivent d'un thread de chargement de Qt Quick, pas du thread
// principal : chaque thread ouvre SA connexion SQLite (une QSqlDatabase ne se
// partage pas entre threads), sur le même fichier — WAL rend la lecture sûre.
class TaskImageProvider : public QQuickImageProvider
{
public:
    explicit TaskImageProvider(const QString& dbPath);

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

private:
    QString m_dbPath;
};

} // namespace app
