#pragma once

#include "nostr.h"

#include <QObject>
#include <QUrl>
#include <QTimer>
#include <QWebSocket>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <vector>

namespace net {

// Represents a single Nostr relay connection.
//
// Reconnection: exponential backoff starting at 1 s, doubling each attempt,
// capped at 60 s. Resets to 1 s on a successful connection.
//
// Subscriptions: plusieurs canaux (#t) en parallèle sur une même socket —
// nécessaire pour suivre toutes les listes partagées d'un appareil.
// Re-subscribes automatically after every reconnection.
//
// Publish: emits the event to the relay; tracks the matching ["OK"] reply and
// emits publishAck() when received.
class RelayClient : public QObject
{
    Q_OBJECT

public:
    explicit RelayClient(const QUrl& url, QObject* parent = nullptr);
    ~RelayClient() override;

    // Connect to the relay. Starts the reconnect loop.
    void connectToRelay();

    // Disconnect and stop the reconnect loop.
    void disconnectFromRelay();

    // Arrêt définitif : coupe signaux + socket avant destruction.
    void shutdown();

    // Publish a NostrEvent to this relay.
    void publish(const NostrEvent& ev);

    // Subscribe using kind 4545, the given channel tag, and a since timestamp.
    // Appeler pour chaque liste : les souscriptions coexistent.
    void subscribe(const QString& channelTag, int64_t since);

    // Close all subscriptions (sends CLOSE pour chaque subId actif).
    void closeSubscription();

    bool isConnected() const;
    bool isShuttingDown() const { return m_shuttingDown; }
    QUrl url() const { return m_url; }

signals:
    void connected();
    void disconnected();
    void eventReceived(const NostrEvent& ev);
    void eose();
    // Emitted when the relay replies ["OK", id, accepted, msg].
    void publishAck(const QString& eventId, bool accepted, const QString& msg);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString& msg);
    void onReconnectTimer();

private:
    struct ActiveSub {
        QString channelTag;
        int64_t since = 0;
        QString subId;
    };

    QString makeSubId();
    void sendReq(const ActiveSub& sub);
    void sendJson(const QJsonArray& msg);
    void scheduleReconnect();
    void resetBackoff();
    void resubscribe();
    bool hasLiveSocket() const;

    QUrl        m_url;
    std::unique_ptr<QWebSocket> m_socket;
    QTimer      m_reconnectTimer;

    int     m_backoffMs    = 1000;   // current reconnect delay
    bool    m_intentionalDisconnect = false;
    bool    m_shuttingDown = false;

    std::vector<ActiveSub> m_subscriptions;

    static constexpr int kMaxBackoffMs = 60'000;
    static constexpr int kInitBackoffMs = 1'000;
};

} // namespace net
