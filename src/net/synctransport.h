#pragma once

#include "nostr.h"

#include <QObject>
#include <QString>
#include <cstdint>

namespace net {

// How a published event was acknowledged.
//  - RelayDurable : stocké par le relais (seul ack qui autorise à vider l'outbox)
//  - PeerDelivered: remis à un pair P2P (utile pour l'UI, pas encore durable)
//  - Rejected     : refus explicite (relais ou pair)
enum class AckKind {
    RelayDurable,
    PeerDelivered,
    Rejected,
};

// Abstract transport interface for sync events.
//
// Both RelayPool (Nostr relay) and WebRTCTransport (P2P data channels)
// implement this interface so SyncEngine can work with either backend.
//
// The event envelope is always net::NostrEvent — the transport is agnostic
// to the payload format (encrypted JSON blobs).
class SyncTransport : public QObject
{
    Q_OBJECT

public:
    explicit SyncTransport(QObject* parent = nullptr) : QObject(parent) {}
    ~SyncTransport() override = default;

    // Connect to the transport network (relays, signaling server, etc.).
    virtual void connectAll() = 0;

    // Disconnect gracefully.
    virtual void disconnectAll() = 0;

    // Ordered shutdown (cut connections, release resources).
    virtual void shutdown() = 0;

    // Publish an event to all connected peers/relays.
    virtual void publishToAll(const NostrEvent& ev) = 0;

    // Publish to one P2P peer only (snap/catch-up, HD image). Defaults to
    // broadcast so transports without peer addressing keep working.
    virtual void publishToPeer(const QString& /*peerId*/, const NostrEvent& ev)
    {
        publishToAll(ev);
    }

    // Publish to P2P peers only, never to the relay (HD images).
    virtual void publishToPeers(const NostrEvent& ev) { publishToAll(ev); }

    // Demande une variante d'image aux pairs P2P (no-op si non supporté).
    virtual void requestImage(const QString& /*channelTag*/, const QString& /*sha*/,
                              const QString& /*variant*/) {}

    // Subscribe to a channel (identified by channelTag).
    // For Nostr: REQ subscription on all relays.
    // For WebRTC: join the signaling room for this channel.
    virtual void subscribeAll(const QString& channelTag, int64_t since) = 0;

    // True if at least one relay/peer is reachable.
    virtual bool isOnline() const = 0;

    // Nombre de pairs P2P ouverts sur un canal (0 si non pertinent).
    virtual int peerCount(const QString& /*channelTag*/ = QString()) const { return 0; }

signals:
    // A sync event was received from a remote peer.
    void eventReceived(const net::NostrEvent& ev);

    // Online state changed (at least one relay/peer connected or disconnected).
    void onlineChanged(bool online);

    // A publish was acknowledged (relay OK, or peer confirmed receipt).
    // Conservé pour compatibilité/QML ; la sémantique fine est publishAckKind.
    void publishAck(const QString& eventId, bool accepted, const QString& msg);

    // Ack typé : distingue un stockage durable relais d'une remise P2P.
    void publishAckKind(const QString& eventId, net::AckKind kind, const QString& msg);

    // Un pair P2P vient de se connecter/déconnecter sur un canal.
    void peerStateChanged(const QString& channelTag, int peerCount);

    // Un pair demande le rattrapage d'un canal depuis `since` (0 = tout).
    void catchUpRequested(const QString& channelTag, const QString& peerId, int64_t since);

    // Un pair demande une image (typiquement la HD) pour un sha donné.
    void imageRequested(const QString& channelTag, const QString& peerId,
                        const QString& sha, const QString& variant);
};

} // namespace net

Q_DECLARE_METATYPE(net::AckKind)