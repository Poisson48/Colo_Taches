#include "hybridtransport.h"

#include <QDebug>

namespace net {

HybridTransport::HybridTransport(QObject* parent)
    : SyncTransport(parent)
{
    // Les events entrants des deux backends remontent tels quels.
    connect(&m_relay, &SyncTransport::eventReceived,
            this, &SyncTransport::eventReceived);
    connect(&m_p2p, &SyncTransport::eventReceived,
            this, &SyncTransport::eventReceived);

    // Les acks sont typés : SyncEngine ne vide l'outbox que sur RelayDurable.
    connect(&m_relay, &SyncTransport::publishAck,
            this, &SyncTransport::publishAck);
    connect(&m_p2p, &SyncTransport::publishAck,
            this, &SyncTransport::publishAck);
    connect(&m_relay, &SyncTransport::publishAckKind,
            this, &SyncTransport::publishAckKind);
    connect(&m_p2p, &SyncTransport::publishAckKind,
            this, &SyncTransport::publishAckKind);

    connect(&m_p2p, &SyncTransport::peerStateChanged,
            this, &SyncTransport::peerStateChanged);
    connect(&m_p2p, &SyncTransport::catchUpRequested,
            this, &SyncTransport::catchUpRequested);
    connect(&m_p2p, &SyncTransport::imageRequested,
            this, &SyncTransport::imageRequested);

    connect(&m_relay, &SyncTransport::onlineChanged, this, [this](bool online) {
        emit relayOnlineChanged(online);
        updateOnlineState();
    });
    connect(&m_p2p, &SyncTransport::onlineChanged, this, [this](bool online) {
        emit p2pOnlineChanged(online);
        updateOnlineState();
    });
}

HybridTransport::~HybridTransport()
{
    shutdown();
}

void HybridTransport::setMode(Mode mode)
{
    if (m_mode == mode)
        return;

    m_mode = mode;

    // Fermer le backend qui n'est plus utilisé, connecter celui qui le devient.
    if (m_mode == Mode::RelayOnly) {
        m_p2p.disconnectAll();
        m_relay.connectAll();
    } else if (m_mode == Mode::P2POnly) {
        m_relay.disconnectAll();
        m_p2p.connectAll();
    } else {
        m_relay.connectAll();
        m_p2p.connectAll();
    }

    updateOnlineState();
    emit modeChanged(m_mode);
}

void HybridTransport::setRelays(const QList<QUrl>& urls)
{
    m_relay.setRelays(urls);
}

void HybridTransport::setSignalingUrl(const QUrl& url)
{
    m_p2p.setSignalingUrl(url);
}

void HybridTransport::setP2pServerUrl(const QUrl& url)
{
    m_p2p.setP2pServerUrl(url);
}

void HybridTransport::setAppId(const QString& appId)
{
    m_p2p.setAppId(appId);
}

void HybridTransport::setIceServers(const QStringList& urls)
{
    m_p2p.setIceServers(urls);
}

void HybridTransport::setDeviceId(const QString& deviceId)
{
    m_p2p.setDeviceId(deviceId);
}

void HybridTransport::connectAll()
{
    if (m_shuttingDown)
        return;
    if (m_mode != Mode::P2POnly)
        m_relay.connectAll();
    if (m_mode != Mode::RelayOnly)
        m_p2p.connectAll();
    updateOnlineState();
}

void HybridTransport::disconnectAll()
{
    m_relay.disconnectAll();
    m_p2p.disconnectAll();
    if (m_online) {
        m_online = false;
        emit onlineChanged(false);
    }
}

void HybridTransport::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_p2p.shutdown();
    m_relay.shutdown();
    if (m_online) {
        m_online = false;
        emit onlineChanged(false);
    }
}

void HybridTransport::publishToAll(const NostrEvent& ev)
{
    if (m_mode != Mode::P2POnly)
        m_relay.publishToAll(ev);
    if (m_mode != Mode::RelayOnly)
        m_p2p.publishToAll(ev);
}

void HybridTransport::publishToPeer(const QString& peerId, const NostrEvent& ev)
{
    if (m_mode == Mode::RelayOnly)
        return;
    m_p2p.publishToPeer(peerId, ev);
}

void HybridTransport::publishToPeers(const NostrEvent& ev)
{
    if (m_mode == Mode::RelayOnly)
        return;
    m_p2p.publishToAll(ev);
}

void HybridTransport::requestImage(const QString& channelTag, const QString& sha,
                                   const QString& variant)
{
    if (m_mode == Mode::RelayOnly)
        return;
    m_p2p.requestImage(channelTag, sha, variant);
}

void HybridTransport::subscribeAll(const QString& channelTag, int64_t since)
{
    if (m_mode != Mode::P2POnly)
        m_relay.subscribeAll(channelTag, since);
    if (m_mode != Mode::RelayOnly)
        m_p2p.subscribeAll(channelTag, since);
}

int HybridTransport::peerCount(const QString& channelTag) const
{
    return m_p2p.peerCount(channelTag);
}

void HybridTransport::updateOnlineState()
{
    const bool online =
        (m_mode != Mode::RelayOnly && m_p2p.isOnline())
        || (m_mode != Mode::P2POnly && m_relay.isOnline());

    if (online != m_online) {
        m_online = online;
        emit onlineChanged(m_online);
    }
}

} // namespace net
