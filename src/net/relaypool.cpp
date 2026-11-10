#include "relaypool.h"

#include <QDebug>

namespace net {

RelayPool::RelayPool(QObject* parent)
    : SyncTransport(parent)
{}

RelayPool::~RelayPool()
{
    shutdown();
}

// static
QList<QUrl> RelayPool::defaultRelays()
{
    // Colo-apps devient le relais principal ; les anciens relais publics
    // restent 90 jours pour les appareils qui n'ont pas encore été mis à jour.
    return {
        QUrl(QStringLiteral("wss://colo-apps.les-crevettes-cevenoles.fr")),
        QUrl(QStringLiteral("wss://relay.damus.io")),
        QUrl(QStringLiteral("wss://nos.lol")),
        QUrl(QStringLiteral("wss://relay.nostr.band")),
        QUrl(QStringLiteral("wss://offchain.pub")),
    };
}

void RelayPool::setRelays(const QList<QUrl>& urls)
{
    if (m_shuttingDown)
        return;

    // Disconnect old clients.
    for (auto& c : m_clients)
        c->disconnectFromRelay();
    m_clients.clear();

    for (const QUrl& url : urls) {
        // Pas de parent QObject : propriété exclusive via unique_ptr.
        auto client = std::make_unique<RelayClient>(url);

        connect(client.get(), &RelayClient::connected,
                this, &RelayPool::onClientConnected);
        connect(client.get(), &RelayClient::disconnected,
                this, &RelayPool::onClientDisconnected);
        connect(client.get(), &RelayClient::eventReceived,
                this, &RelayPool::onClientEvent);
        connect(client.get(), &RelayClient::eose,
                this, &RelayPool::onClientEose);
        connect(client.get(), &RelayClient::publishAck,
                this, &RelayPool::onClientAck);

        m_clients.push_back(std::move(client));
    }

    // Ré-appliquer toutes les souscriptions actives (multi-listes).
    for (auto it = m_subscriptions.constBegin(); it != m_subscriptions.constEnd(); ++it) {
        for (auto& c : m_clients)
            c->subscribe(it.key(), it.value());
    }
}

void RelayPool::connectAll()
{
    if (m_shuttingDown)
        return;
    for (auto& c : m_clients)
        c->connectToRelay();
}

void RelayPool::disconnectAll()
{
    for (auto& c : m_clients)
        c->disconnectFromRelay();
}

void RelayPool::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    qDebug() << "[shutdown] RelayPool::shutdown";

    disconnect();

    for (auto& c : m_clients) {
        c->disconnect(this);
        c->shutdown();
    }
    m_clients.clear();
    m_online = false;
}

void RelayPool::publishToAll(const NostrEvent& ev)
{
    if (m_shuttingDown)
        return;
    for (auto& c : m_clients) {
        if (c->isConnected())
            c->publish(ev);
    }
}

void RelayPool::subscribeAll(const QString& channelTag, int64_t since)
{
    if (m_shuttingDown)
        return;
    m_subscriptions[channelTag] = since;
    for (auto& c : m_clients)
        c->subscribe(channelTag, since);
}

// ── Private slots ──────────────────────────────────────────────────────────

void RelayPool::onClientConnected()
{
    if (m_shuttingDown)
        return;
    updateOnlineState();
}

void RelayPool::onClientDisconnected()
{
    if (m_shuttingDown)
        return;
    updateOnlineState();
}

void RelayPool::onClientEvent(const NostrEvent& ev)
{
    if (m_shuttingDown)
        return;

    // Dedup: skip events we've already forwarded.
    if (ev.id.isEmpty() || m_seenIds.contains(ev.id))
        return;

    m_seenIds.insert(ev.id);
    emit eventReceived(ev);
}

void RelayPool::onClientEose()
{
    if (m_shuttingDown)
        return;
    emit eose();
}

void RelayPool::onClientAck(const QString& eventId, bool accepted, const QString& msg)
{
    if (m_shuttingDown)
        return;
    emit publishAck(eventId, accepted, msg);

    // Un OK explicite, ou un « duplicate/already/stored », signifie que le
    // relais possède déjà l'événement. C'est le seul ack qui autorise à vider
    // l'outbox. Tout le reste est un refus.
    const QString lower = msg.toLower();
    const bool durable = accepted
        || lower.contains(QStringLiteral("duplicate"))
        || lower.contains(QStringLiteral("already"))
        || lower.contains(QStringLiteral("stored"));
    emit publishAckKind(eventId,
                        durable ? AckKind::RelayDurable : AckKind::Rejected,
                        msg);
}

// ── Private helpers ────────────────────────────────────────────────────────

void RelayPool::updateOnlineState()
{
    if (m_shuttingDown)
        return;

    bool anyConnected = false;
    for (const auto& c : m_clients) {
        if (c->isShuttingDown())
            continue;
        if (c->isConnected()) {
            anyConnected = true;
            break;
        }
    }

    if (anyConnected != m_online) {
        m_online = anyConnected;
        // QML lit le signal sans argument (Q_PROPERTY NOTIFY). SyncEngine écoute
        // le signal de base avec son booléen : les deux doivent partir.
        emit onlineChanged();
        emit SyncTransport::onlineChanged(m_online);
    }
}

} // namespace net
