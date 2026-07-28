#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <cstdint>

namespace net {

// NIP-01 Nostr event.
// NOTE: id (sha256 of canonical serialisation) and sig (Schnorr) are computed
// in task 4.2 (crypto layer). Until then, use setIdAndSig() to inject them once
// available. The rest of the struct is ready for serialisation.
struct NostrEvent {
    QString  id;         // 64 hex chars (sha256 of canonical form)
    QString  pubkey;     // 64 hex chars (secp256k1 x-only public key)
    int64_t  created_at = 0; // UNIX timestamp seconds
    int      kind       = 0;
    // tags: list of tag arrays, e.g. [["t","channelTag"]]
    QJsonArray tags;
    QString  content;    // opaque base64 blob (encrypted in 4.2)
    QString  sig;        // 128 hex chars (Schnorr signature)

    // Called by the crypto layer (task 4.2) once id and sig are computed.
    // Before 4.2, leave id/sig empty for outgoing events (relays that validate
    // will reject them; used in tests with a stub relay that ignores signatures).
    void setIdAndSig(const QString& newId, const QString& newSig) {
        id  = newId;
        sig = newSig;
    }

    // Serialise to the JSON object the relay expects inside messages.
    QJsonObject toJson() const;

    // Parse a relay-supplied JSON object.  Returns nullopt on error.
    static std::optional<NostrEvent> fromJson(const QJsonObject& obj);
};

// ── Client → Relay messages ────────────────────────────────────────────────

// ["EVENT", event]
QJsonArray makeEventMsg(const NostrEvent& ev);

// ["REQ", subscriptionId, filter]
// filter keys populated: kinds, #t (tag), since.
QJsonArray makeReqMsg(const QString& subId,
                      const QStringList& kinds,
                      const QStringList& tValues,
                      int64_t since);

// ["CLOSE", subscriptionId]
QJsonArray makeCloseMsg(const QString& subId);

// ── Relay → Client message parsing ────────────────────────────────────────

enum class RelayMsgType {
    Event,
    Eose,
    Ok,
    Notice,
    Unknown
};

struct RelayMsg {
    RelayMsgType type = RelayMsgType::Unknown;

    // EVENT
    QString     subId;
    NostrEvent  event;

    // EOSE
    // subId (same field)

    // OK
    QString     okEventId;
    bool        okAccepted = false;
    QString     okMessage;

    // NOTICE
    QString     notice;
};

RelayMsg parseRelayMsg(const QString& raw);

} // namespace net

Q_DECLARE_METATYPE(net::NostrEvent)
