#include "nostr.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <optional>

namespace net {

// ── NostrEvent ─────────────────────────────────────────────────────────────

QJsonObject NostrEvent::toJson() const
{
    QJsonObject obj;
    obj["id"]         = id;
    obj["pubkey"]     = pubkey;
    obj["created_at"] = static_cast<qint64>(created_at);
    obj["kind"]       = kind;
    obj["tags"]       = tags;
    obj["content"]    = content;
    obj["sig"]        = sig;
    return obj;
}

std::optional<NostrEvent> NostrEvent::fromJson(const QJsonObject& obj)
{
    // Mandatory fields (we tolerate empty id/sig for stub-relay tests).
    if (!obj.contains("kind") || !obj.contains("content"))
        return std::nullopt;

    NostrEvent ev;
    ev.id         = obj.value("id").toString();
    ev.pubkey     = obj.value("pubkey").toString();
    ev.created_at = static_cast<int64_t>(obj.value("created_at").toDouble(0));
    ev.kind       = obj.value("kind").toInt(0);
    ev.tags       = obj.value("tags").toArray();
    ev.content    = obj.value("content").toString();
    ev.sig        = obj.value("sig").toString();
    return ev;
}

// ── Client → Relay ─────────────────────────────────────────────────────────

QJsonArray makeEventMsg(const NostrEvent& ev)
{
    QJsonArray msg;
    msg.append("EVENT");
    msg.append(ev.toJson());
    return msg;
}

QJsonArray makeReqMsg(const QString& subId,
                      const QStringList& kinds,
                      const QStringList& tValues,
                      int64_t since)
{
    QJsonObject filter;

    QJsonArray kindsArr;
    for (const QString& k : kinds)
        kindsArr.append(k.toInt());
    filter["kinds"] = kindsArr;

    QJsonArray tArr;
    for (const QString& t : tValues)
        tArr.append(t);
    filter["#t"] = tArr;

    filter["since"] = static_cast<qint64>(since);

    QJsonArray msg;
    msg.append("REQ");
    msg.append(subId);
    msg.append(filter);
    return msg;
}

QJsonArray makeCloseMsg(const QString& subId)
{
    QJsonArray msg;
    msg.append("CLOSE");
    msg.append(subId);
    return msg;
}

// ── Relay → Client ─────────────────────────────────────────────────────────

RelayMsg parseRelayMsg(const QString& raw)
{
    RelayMsg result;
    result.type = RelayMsgType::Unknown;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return result;

    const QJsonArray arr = doc.array();
    if (arr.isEmpty())
        return result;

    const QString msgType = arr[0].toString();

    if (msgType == "EVENT" && arr.size() >= 3) {
        result.type  = RelayMsgType::Event;
        result.subId = arr[1].toString();
        const auto ev = NostrEvent::fromJson(arr[2].toObject());
        if (!ev)
            return result;
        result.event = *ev;
    } else if (msgType == "EOSE" && arr.size() >= 2) {
        result.type  = RelayMsgType::Eose;
        result.subId = arr[1].toString();
    } else if (msgType == "OK" && arr.size() >= 4) {
        result.type        = RelayMsgType::Ok;
        result.okEventId   = arr[1].toString();
        result.okAccepted  = arr[2].toBool(false);
        result.okMessage   = arr[3].toString();
    } else if (msgType == "NOTICE" && arr.size() >= 2) {
        result.type   = RelayMsgType::Notice;
        result.notice = arr[1].toString();
    }

    return result;
}

} // namespace net
