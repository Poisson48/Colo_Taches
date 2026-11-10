#include "backgroundsync.h"
#include "appcontroller.h"
#include "syncengine.h"

#include "../net/relaypool.h"
#include "../store/database.h"

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <QUrl>

namespace app {

BackgroundSync::BackgroundSync(QObject* parent) : QObject(parent) {}

bool BackgroundSync::start()
{
    store::Database db;
    if (!db.open(AppController::databasePath())) return false;
    const auto mode = db.getSetting("notif.mode");
    if (!mode || *mode != "deferred") return false;

    const QString deviceId = QString::fromStdString(
        db.getSetting("deviceId").value_or("background"));
    const QString displayName = QString::fromStdString(
        db.getSetting("displayName").value_or("Moi"));

    QList<QUrl> relays;
    const auto relaysSetting = db.getSetting("relays");
    if (relaysSetting) {
        for (const QString& part : QString::fromStdString(*relaysSetting)
                                       .split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QUrl u(part.trimmed());
            if (u.isValid() && !u.host().isEmpty()) relays.append(u);
        }
    }
    if (relays.isEmpty()) relays = net::RelayPool::defaultRelays();

    net::RelayPool relay;
    relay.setRelays(relays);
    SyncEngine engine;
    engine.init(&db, &relay, deviceId, displayName);
    engine.setSuppressNotifications(true);
    engine.subscribeAllLists();
    relay.connectAll();
    QTimer::singleShot(20'000, QCoreApplication::instance(), &QCoreApplication::quit);
    qInfo() << "[BackgroundSync] Taches listes=" << db.getLists().size();
    return true;
}

} // namespace app
