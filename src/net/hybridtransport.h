#pragma once

#include "synctransport.h"
#include "relaypool.h"
#include "webrtctransport.h"

#include <QList>
#include <QStringList>
#include <QUrl>

namespace net {

// Transport hybride : relais Nostr durable + P2P WebRTC rapide.
//
//  - Hybrid (défaut) : publie sur les deux ; l'outbox n'est vidée que sur
//    AckKind::RelayDurable (voir SyncEngine).
//  - RelayOnly      : dépannage, comportement de l'ancienne version.
//  - P2POnly        : expérimental, aucune durabilité relais.
//
// Les deux backends restent des SyncTransport autonomes : HybridTransport ne
// fait que les composer et agréger leurs signaux.
class HybridTransport : public SyncTransport
{
    Q_OBJECT

public:
    enum class Mode {
        RelayOnly = 0,
        P2POnly = 1,
        Hybrid = 2,
    };
    Q_ENUM(Mode)

    explicit HybridTransport(QObject* parent = nullptr);
    ~HybridTransport() override;

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    void setRelays(const QList<QUrl>& urls);
    void setSignalingUrl(const QUrl& url);
    void setP2pServerUrl(const QUrl& url);
    void setAppId(const QString& appId);
    void setIceServers(const QStringList& urls);
    void setDeviceId(const QString& deviceId);

    RelayPool& relay() { return m_relay; }
    WebRTCTransport& p2p() { return m_p2p; }

    // SyncTransport
    void connectAll() override;
    void disconnectAll() override;
    void shutdown() override;
    void publishToAll(const NostrEvent& ev) override;
    void publishToPeer(const QString& peerId, const NostrEvent& ev) override;
    void publishToPeers(const NostrEvent& ev) override;
    void requestImage(const QString& channelTag, const QString& sha,
                      const QString& variant) override;
    void subscribeAll(const QString& channelTag, int64_t since) override;
    bool isOnline() const override { return m_online; }
    int peerCount(const QString& channelTag = QString()) const override;

    bool relayOnline() const { return m_relay.isOnline(); }
    bool p2pOnline() const { return m_p2p.isOnline(); }

signals:
    void modeChanged(net::HybridTransport::Mode mode);
    void relayOnlineChanged(bool online);
    void p2pOnlineChanged(bool online);

private:
    void updateOnlineState();

    RelayPool      m_relay;
    WebRTCTransport m_p2p;
    Mode           m_mode = Mode::RelayOnly;
    bool           m_online = false;
    bool           m_shuttingDown = false;
};

} // namespace net
