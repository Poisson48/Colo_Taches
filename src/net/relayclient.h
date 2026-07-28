#pragma once

#include "nostr.h"

#include <QObject>
#include <QUrl>
#include <QTimer>
#include <QWebSocket>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>

namespace net {

// Represents a single Nostr relay connection.
//
// Reconnection: exponential backoff starting at 1 s, doubling each attempt,
// capped at 60 s. Resets to 1 s on a successful connection.
//
// Subscription: kind 4545, tag #t = channelTag, since = lastSync - 3600.
// Re-subscribes automatically after every reconnection.
//
// Publish: emits the event to the relay; tracks the matching ["OK"] reply and
// emits publishAck() when received.
class RelayClient : public QObject
{
    Q_OBJECT

public:
    explicit RelayClient(const QUrl& url, QObject* parent = nullptr);
    ~RelayClient() override = default;

    // Connect to the relay. Starts the reconnect loop.
    void connectToRelay();

    // Disconnect and stop the reconnect loop.
    void disconnectFromRelay();

    // Publish a NostrEvent to this relay.
    void publish(const NostrEvent& ev);

    // Subscribe using kind 4545, the given channel tag, and a since timestamp.
    // Calling again replaces the current subscription.
    void subscribe(const QString& channelTag, int64_t since);

    // Close the current subscription (sends CLOSE).
    void closeSubscription();

    bool isConnected() const;
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
    void sendJson(const QJsonArray& msg);
    void scheduleReconnect();
    void resetBackoff();
    void resubscribe();

    QUrl        m_url;
    QWebSocket  m_socket;
    QTimer      m_reconnectTimer;

    int     m_backoffMs    = 1000;   // current reconnect delay
    bool    m_intentionalDisconnect = false;

    // Active subscription parameters (re-applied after reconnect).
    std::optional<QString>  m_channelTag;
    int64_t                 m_since = 0;
    QString                 m_subId;

    static constexpr int kMaxBackoffMs = 60'000;
    static constexpr int kInitBackoffMs = 1'000;
};

} // namespace net
