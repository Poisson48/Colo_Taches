#pragma once

#include <QObject>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QSet>
#include <cstdint>
#include <vector>
#include <map>
#include <optional>

#include "../core/types.h"
#include "../net/relaypool.h"
#include "../net/nostr.h"
#include "../store/database.h"

namespace app {

class ItemModel;

// SyncEngine: orchestrates the full end-to-end sync loop (§3, §4).
//
// Outgoing (SORTANT):
//   Local modification → build CRDT delta payload → encrypt (XChaCha20) →
//   NostrEvent kind 4545 tag ["t",channelTag] → sign → publish.
//   Debounce 300 ms to batch bursts.
//   If offline → push to outbox. Also push to outbox until OK ack received.
//
// Incoming (ENTRANT):
//   RelayPool::eventReceived → dedup → decrypt → parsePayload → mergeItems →
//   upsert DB → refresh ItemModel → emit remoteChanges().
//
// Reconnection:
//   RelayPool::onlineChanged(true) → flush outbox → resubscribe kind 4545.
//
// Snapshots:
//   Counter of emitted deltas per list (stored in settings).
//   If >= 100 or last snap > 7 days → publish full snap.
//
// Join:
//   joinList path → subscribe without since (limit 500) to catch up history.
class SyncEngine : public QObject
{
    Q_OBJECT

public:
    explicit SyncEngine(QObject* parent = nullptr);
    ~SyncEngine() override;

    // Initialize with DB, RelayPool, device identity.
    // Does NOT take ownership of db or pool (caller owns).
    void init(store::Database* db,
              net::RelayPool*  pool,
              const QString&   deviceId,
              const QString&   displayName);

    // Nom affiché embarqué dans les payloads (members). Changeable à chaud.
    void setDisplayName(const QString& displayName) { m_displayName = displayName; }

    // Register an ItemModel for a given listId so incoming events can refresh it
    // directly (weak reference: model must outlive usage). Plusieurs modèles par
    // liste : l'écran des tâches ET la page d'étapes ouverte sur la même liste.
    void registerItemModel(const std::string& listId, ItemModel* model);
    void unregisterItemModel(const std::string& listId, ItemModel* model = nullptr);

    // Publie le blob d'une photo (événement « img » chiffré, à part des deltas).
    // Appelé à l'ajout d'une photo, et au snapshot pour re-semer les blobs
    // référencés (rétention limitée des relais).
    void publishImage(const std::string& listId, const std::string& sha);

    // Called by AppController/ItemModel after a local modification.
    // Schedules a debounced publish for listId.
    void onLocalChange(const std::string& listId);

    // Called when a new list is joined (subscribe without since).
    void onListJoined(const std::string& listId);

    // Subscribe all known lists (called after init, or after reconnect).
    void subscribeAllLists(int64_t since = 0);

signals:
    // N items changed in listId by authorName (from a remote event).
    void remoteChanges(const QString& listId, int count, const QString& authorName);

    // Le titre de listId a été renommé par un autre appareil (merge LWW gagné).
    void listTitleChanged(const QString& listId, const QString& title);

    // Le blob d'une photo vient d'arriver d'un relais : l'UI peut maintenant
    // l'afficher (les vignettes se rechargent via le compteur de révision).
    void imageArrived(const QString& listId, const QString& sha);

    // La file d'attente d'envoi a bougé (ajout, ou accusé de réception d'un relais).
    void outboxChanged();

    // Forwarded online state (so AppController can bind its property).
    void onlineChanged(bool online);

private slots:
    void onRelayEvent(const net::NostrEvent& ev);
    void onRelayOnline(bool online);
    void onPublishAck(const QString& eventId, bool accepted, const QString& msg);
    void onDebounceTimer();

private:
    // Build and publish a delta for listId. Returns the event id (empty on failure).
    void publishDelta(const std::string& listId);

    // Publish a full snapshot for listId.
    void publishSnap(const std::string& listId);

    // Encrypt, sign, and publish a payload string for listId.
    // If sign fails, do not publish. Stores in outbox.
    // Returns event id string (empty on error).
    std::string buildAndPublish(const std::string& listId, const std::string& payloadJson);

    // Flush the outbox for all known lists (called on reconnect).
    void flushOutbox();

    // Flush the outbox for a single list.
    void flushOutboxForList(const std::string& listId);

    // Track pending outbox events (eventId -> listId) waiting for OK ack.
    // Once we get at least one OK, remove from outbox.
    void trackPendingAck(const QString& eventId, const std::string& listId);

    // Returns the channel tag for a list (derived from its key).
    std::optional<std::string> channelTagForList(const std::string& listId);

    // Increment delta counter for a list; return new value.
    int64_t incrementDeltaCounter(const std::string& listId);

    // Returns the number of emitted deltas since last snap.
    int64_t getDeltaCounter(const std::string& listId);

    // Reset delta counter and update last-snap timestamp.
    void resetDeltaCounter(const std::string& listId);

    // Show a system notification (tray if available, otherwise qDebug).
    // whenMs > 0 → horodatage = heure de la modification (pas de la réception).
    void showNotification(const QString& title, const QString& body, qint64 whenMs = 0);

    store::Database* m_db     = nullptr;
    net::RelayPool*  m_pool   = nullptr;
    QString          m_deviceId;
    QString          m_displayName;

    // Registered item models (non-owning). Plusieurs par liste (tâches + étapes).
    std::map<std::string, std::vector<ItemModel*>> m_models;

    // Debounce: per-list timer fired 300 ms after last onLocalChange.
    QTimer m_debounceTimer;
    QSet<QString> m_pendingLists; // lists awaiting debounce publish

    // Per-event outbox tracking: eventId -> listId.
    std::map<QString, std::string> m_pendingAcks;

    // Active subscription channel tags (to avoid duplicate subscriptions).
    QSet<QString> m_subscribedChannels;

    QObject* m_tray = nullptr; // QSystemTrayIcon* when QtWidgets available
};

} // namespace app
