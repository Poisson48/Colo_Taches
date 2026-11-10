#pragma once

#include "relayclient.h"
#include "synctransport.h"
#include "nostr.h"

#include <QObject>
#include <QUrl>
#include <QSet>
#include <QHash>
#include <QStringList>
#include <vector>
#include <memory>
#include <cstdint>

namespace net {

// Manages a pool of RelayClient connections (one per relay URL).
//
// - publishToAll: sends to every connected relay.
// - subscribeAll: subscribes all relays (and re-subscribes on reconnect via
//   RelayClient's built-in mechanism).
// - Deduplication: each event id is tracked; eventReceived is emitted at most once.
// - online property: true iff at least one relay is connected; emits onlineChanged.
class RelayPool : public SyncTransport
{
    Q_OBJECT
    Q_PROPERTY(bool online READ isOnline NOTIFY onlineChanged)

public:
    explicit RelayPool(QObject* parent = nullptr);
    ~RelayPool() override;

    // Replace the relay list and reconnect everything.
    void setRelays(const QList<QUrl>& urls);

    // Default relay set from SPEC §3.1.
    static QList<QUrl> defaultRelays();

    // SyncTransport interface
    void connectAll() override;
    void disconnectAll() override;
    void shutdown() override;
    void publishToAll(const NostrEvent& ev) override;
    void subscribeAll(const QString& channelTag, int64_t since) override;
    bool isOnline() const override { return m_online; }

signals:
    // RelayPool-specific signal (not in SyncTransport).
    void eose();

    // Redeclare onlineChanged without the bool parameter for Q_PROPERTY NOTIFY.
    // updateOnlineState() émet AUSSI SyncTransport::onlineChanged(bool), que
    // SyncEngine utilise pour flush/resubscribe. Les deux signaux sont nécessaires.
    void onlineChanged();

private slots:
    void onClientConnected();
    void onClientDisconnected();
    void onClientEvent(const NostrEvent& ev);
    void onClientEose();
    void onClientAck(const QString& eventId, bool accepted, const QString& msg);

private:
    void updateOnlineState();

    std::vector<std::unique_ptr<RelayClient>> m_clients;
    QSet<QString> m_seenIds;   // in-memory dedup by event id

    // Souscriptions actives par canal (#t) — une entrée par liste partagée.
    QHash<QString, int64_t> m_subscriptions;

    bool m_online = false;
    bool m_shuttingDown = false;
};

} // namespace net