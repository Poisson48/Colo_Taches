#pragma once

#include "relayclient.h"
#include "nostr.h"

#include <QObject>
#include <QUrl>
#include <QSet>
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
class RelayPool : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool online READ isOnline NOTIFY onlineChanged)

public:
    explicit RelayPool(QObject* parent = nullptr);
    ~RelayPool() override = default;

    // Replace the relay list and reconnect everything.
    void setRelays(const QList<QUrl>& urls);

    // Default relay set from SPEC §3.1.
    static QList<QUrl> defaultRelays();

    // Connect all relays.
    void connectAll();

    // Disconnect all relays.
    void disconnectAll();

    // Publish to every connected relay.
    void publishToAll(const NostrEvent& ev);

    // Subscribe on every relay. Subscription is re-applied on reconnect automatically.
    void subscribeAll(const QString& channelTag, int64_t since);

    bool isOnline() const { return m_online; }

signals:
    // Emitted once per unique event id.
    void eventReceived(const NostrEvent& ev);
    void eose();
    void onlineChanged(bool online);
    void publishAck(const QString& eventId, bool accepted, const QString& msg);

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

    // Active subscription (stored for new relays added after subscribeAll).
    std::optional<QString> m_channelTag;
    int64_t                m_since = 0;

    bool m_online = false;
};

} // namespace net
