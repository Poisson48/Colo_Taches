#include "syncengine.h"
#include "itemmodel.h"
#include "platform.h"

#include "../core/crdt.h"
#include "../core/payload.h"
#include "../net/crypto.h"
#include "../net/nostr.h"
#include "../net/pushclient.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>

#if QT_FEATURE_systemtrayicon == 1 && defined(QT_WIDGETS_LIB)
#  include <QSystemTrayIcon>
#endif

namespace app {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int     kDebounceMs        = 300;
static constexpr int64_t kSnapDeltaThresh   = 100;    // deltas before forcing a snap
static constexpr int64_t kSnapAgeMs         = 7LL * 24 * 3600 * 1000; // 7 days
static constexpr int     kPushWakeDebounceMs = 60'000; // 1 min après la dernière modif

// Settings key prefixes
static constexpr const char* kDeltaCountKey  = "sync.delta_count.";
static constexpr const char* kLastSnapKey    = "sync.last_snap.";

// ---------------------------------------------------------------------------
// Construction / init
// ---------------------------------------------------------------------------

SyncEngine::SyncEngine(QObject* parent)
    : QObject(parent)
{
    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(kDebounceMs);
    connect(&m_debounceTimer, &QTimer::timeout, this, &SyncEngine::onDebounceTimer);
}

SyncEngine::~SyncEngine()
{
    // Disconnect from pool explicitly to prevent callbacks on half-destroyed state.
    if (m_pool) {
        m_pool->disconnect(this);
    }
    m_debounceTimer.stop();
    m_models.clear();
}

void SyncEngine::init(store::Database* db,
                      net::SyncTransport* pool,
                      const QString&   deviceId,
                      const QString&   displayName)
{
    m_db          = db;
    m_pool        = pool;
    m_deviceId    = deviceId;
    m_displayName = displayName;

    // Wire relay signals.
    connect(m_pool, &net::SyncTransport::eventReceived,
            this,   &SyncEngine::onRelayEvent);
    connect(m_pool, &net::SyncTransport::onlineChanged,
            this,   &SyncEngine::onRelayOnline);
    connect(m_pool, &net::SyncTransport::publishAckKind,
            this,   &SyncEngine::onPublishAckKind);
    connect(m_pool, &net::SyncTransport::imageRequested,
            this,   &SyncEngine::onImageRequested);
    connect(m_pool, &net::SyncTransport::catchUpRequested,
            this,   &SyncEngine::onCatchUpRequested);

    // Forward online state.
    connect(m_pool, &net::SyncTransport::onlineChanged,
            this,   &SyncEngine::onlineChanged);

#if QT_FEATURE_systemtrayicon == 1 && defined(QT_WIDGETS_LIB)
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        auto* tray = new QSystemTrayIcon(this);
        tray->show();
        m_tray = tray;
    }
#endif
}

// ---------------------------------------------------------------------------
// Model registration
// ---------------------------------------------------------------------------

void SyncEngine::registerItemModel(const std::string& listId, ItemModel* model)
{
    auto& vec = m_models[listId];
    if (std::find(vec.begin(), vec.end(), model) == vec.end())
        vec.push_back(model);
}

void SyncEngine::unregisterItemModel(const std::string& listId, ItemModel* model)
{
    auto it = m_models.find(listId);
    if (it == m_models.end()) return;
    if (!model) {
        m_models.erase(it);
        return;
    }
    auto& vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), model), vec.end());
    if (vec.empty())
        m_models.erase(it);
}

// ---------------------------------------------------------------------------
// Local change → debounced publish
// ---------------------------------------------------------------------------

void SyncEngine::onLocalChange(const std::string& listId)
{
    m_pendingLists.insert(QString::fromStdString(listId));
    schedulePushWake(listId);
    if (!m_debounceTimer.isActive())
        m_debounceTimer.start();
}

void SyncEngine::onDebounceTimer()
{
    const QSet<QString> lists = m_pendingLists;
    m_pendingLists.clear();

    for (const QString& listIdQ : lists) {
        const std::string listId = listIdQ.toStdString();

        // Check if snap threshold is met.
        int64_t deltaCount = getDeltaCounter(listId);
        int64_t lastSnapMs = 0;
        auto lastSnapOpt = m_db->getSetting(std::string(kLastSnapKey) + listId);
        if (lastSnapOpt) {
            try { lastSnapMs = std::stoll(*lastSnapOpt); } catch (...) {}
        }
        const int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool snapDue  = (deltaCount >= kSnapDeltaThresh)
                           || (lastSnapMs > 0 && (nowMs - lastSnapMs) >= kSnapAgeMs);

        if (snapDue) {
            publishSnap(listId);
        } else {
            publishDelta(listId);
            incrementDeltaCounter(listId);
        }
    }
}

// ---------------------------------------------------------------------------
// Publish delta / snap
// ---------------------------------------------------------------------------

void SyncEngine::publishDelta(const std::string& listId)
{
    if (!m_db) return;

    auto metaOpt = m_db->getList(listId);
    if (!metaOpt) return;

    const auto items = m_db->getItems(listId);
    const auto members = m_db->getMembers(listId);

    core::Payload p;
    p.type   = core::Payload::Type::delta;
    p.listId = listId;
    p.items  = items;

    // Le titre est versionné (LWW) comme les articles : sans lui dans le delta, un
    // renommage n'atteindrait les autres qu'au prochain snap (jusqu'à 100 deltas
    // ou 7 jours plus tard). Le renvoyer à chaque fois est idempotent et sans coût.
    p.title    = metaOpt->title;
    p.titleVer = metaOpt->titleVer;

    // Mode de classement, versionné comme le titre. Omis tant que personne ne l'a
    // choisi (ver lamport 0) : inutile d'imposer un défaut aux versions antérieures.
    if (metaOpt->sortModeVer.lamport > 0) {
        p.sortMode    = metaOpt->sortMode;
        p.sortModeVer = metaOpt->sortModeVer;
    }

    // Include our own member entry so remote peers know our display name.
    const std::string devId = m_deviceId.toStdString();
    p.by = devId;
    p.members[devId] = {m_displayName.toStdString(),
                        core::Ver{metaOpt->lamport, devId}};

    const std::string json = core::serializePayload(p);
    buildAndPublish(listId, json);
}

std::string SyncEngine::snapPayloadJson(const std::string& listId)
{
    if (!m_db) return {};
    auto metaOpt = m_db->getList(listId);
    if (!metaOpt) return {};
    const auto items = m_db->getItems(listId);
    const auto rawMembers = m_db->getMembers(listId);

    core::Payload p;
    p.type      = core::Payload::Type::snap;
    p.listId    = listId;
    p.items     = items;
    p.title     = metaOpt->title;
    p.titleVer  = metaOpt->titleVer;
    if (metaOpt->sortModeVer.lamport > 0) {
        p.sortMode    = metaOpt->sortMode;
        p.sortModeVer = metaOpt->sortModeVer;
    }
    const std::string devId = m_deviceId.toStdString();
    p.by = devId;
    for (const auto& [did, name] : rawMembers)
        p.members[did] = {name, core::Ver{metaOpt->lamport, did}};
    p.members[devId] = {m_displayName.toStdString(),
                        core::Ver{metaOpt->lamport, devId}};
    return core::serializePayload(p);
}

void SyncEngine::publishSnap(const std::string& listId)
{
    const std::string json = snapPayloadJson(listId);
    if (json.empty()) return;
    buildAndPublish(listId, json);
    for (const std::string& sha : m_db->imagesReferenced(listId))
        publishImage(listId, sha);
    resetDeltaCounter(listId);
}

void SyncEngine::publishSnapToPeer(const std::string& listId, const QString& peerId)
{
    if (!m_pool || peerId.isEmpty()) return;
    const std::string json = snapPayloadJson(listId);
    if (json.empty()) return;
    if (auto ev = buildEvent(listId, json))
        m_pool->publishToPeer(peerId, *ev);
    for (const std::string& sha : m_db->imagesReferenced(listId))
        publishImageToPeer(listId, sha, peerId);
}

void SyncEngine::publishImage(const std::string& listId, const std::string& sha)
{
    if (!m_db || sha.empty()) return;

    const QByteArray blob = m_db->getImage(sha);
    if (blob.isEmpty()) return;   // blob jamais reçu : rien à re-semer

    std::vector<uint8_t> data(blob.constData(), blob.constData() + blob.size());
    const std::string json = core::serializeImagePayload(
        listId, m_deviceId.toStdString(), sha, data);
    buildAndPublish(listId, json);
}

void SyncEngine::publishImageHd(const std::string& listId, const std::string& sha)
{
    if (!m_pool || !m_db || sha.empty()) return;
    const QByteArray blob = m_db->getImageHd(sha);
    if (blob.isEmpty()) return;
    std::vector<uint8_t> data(blob.constData(), blob.constData() + blob.size());
    const std::string json = core::serializeImagePayload(
        listId, m_deviceId.toStdString(), sha, data, "hd");
    if (auto ev = buildEvent(listId, json))
        m_pool->publishToPeers(*ev);
}

void SyncEngine::publishImageToPeer(const std::string& listId, const std::string& sha,
                                    const QString& peerId)
{
    if (!m_pool || !m_db || sha.empty()) return;
    const QByteArray blob = m_db->getImage(sha);
    if (blob.isEmpty()) return;
    std::vector<uint8_t> data(blob.constData(), blob.constData() + blob.size());
    const std::string json = core::serializeImagePayload(
        listId, m_deviceId.toStdString(), sha, data);
    if (auto ev = buildEvent(listId, json))
        m_pool->publishToPeer(peerId, *ev);
}

void SyncEngine::publishImageHdToPeer(const std::string& listId, const std::string& sha,
                                      const QString& peerId)
{
    if (!m_pool || !m_db || sha.empty()) return;
    const QByteArray blob = m_db->getImageHd(sha);
    if (blob.isEmpty()) return;
    std::vector<uint8_t> data(blob.constData(), blob.constData() + blob.size());
    const std::string json = core::serializeImagePayload(
        listId, m_deviceId.toStdString(), sha, data, "hd");
    if (auto ev = buildEvent(listId, json))
        m_pool->publishToPeer(peerId, *ev);
}

std::optional<net::NostrEvent> SyncEngine::buildEvent(const std::string& listId,
                                                       const std::string& payloadJson)
{
    if (!m_db) return std::nullopt;
    auto metaOpt = m_db->getList(listId);
    if (!metaOpt) return std::nullopt;
    const std::string channelTag = net::deriveChannelTag(metaOpt->key);
    const auto seed = net::deriveNostrSeed(metaOpt->key);
    const std::string cipherBase64 = net::encryptPayload(metaOpt->key, channelTag, payloadJson);
    if (cipherBase64.empty()) return std::nullopt;

    net::NostrEvent ev;
    ev.created_at = QDateTime::currentSecsSinceEpoch();
    ev.kind = 4545;
    ev.content = QString::fromStdString(cipherBase64);
    QJsonArray tagT;
    tagT.append(QString("t"));
    tagT.append(QString::fromStdString(channelTag));
    ev.tags.append(tagT);
    if (!net::signEvent(ev, seed)) return std::nullopt;
    return ev;
}

std::string SyncEngine::buildAndPublish(const std::string& listId,
                                        const std::string& payloadJson)
{
    const auto evOpt = buildEvent(listId, payloadJson);
    if (!evOpt) return {};
    const net::NostrEvent& ev = *evOpt;
    const QJsonDocument doc(ev.toJson());
    const std::string eventJson = doc.toJson(QJsonDocument::Compact).toStdString();
    m_db->outboxPush(listId, eventJson);
    emit outboxChanged();
    if (m_pool && m_pool->isOnline()) {
        m_pool->publishToAll(ev);
        trackPendingAck(ev.id, listId);
    }
    return ev.id.toStdString();
}


// ---------------------------------------------------------------------------
// Incoming events
// ---------------------------------------------------------------------------

void SyncEngine::onRelayEvent(const net::NostrEvent& ev)
{
    if (!m_db) return;

    const std::string eventId = ev.id.toStdString();

    // Dedup (DB-level persistence, survives restarts).
    if (m_db->isEventSeen(eventId)) return;
    m_db->markEventSeen(eventId);

    // Identify which list this belongs to (find list whose channelTag matches).
    // The channelTag is in the first "t" tag.
    QString channelTagQ;
    for (const auto& tagVal : ev.tags) {
        QJsonArray tag = tagVal.toArray();
        if (tag.size() >= 2 && tag[0].toString() == "t") {
            channelTagQ = tag[1].toString();
            break;
        }
    }
    if (channelTagQ.isEmpty()) return;

    const std::string channelTag = channelTagQ.toStdString();

    // Find list with matching channel tag.
    const auto lists = m_db->getLists();
    std::optional<core::ListMeta> foundMeta;
    for (const auto& meta : lists) {
        if (net::deriveChannelTag(meta.key) == channelTag) {
            foundMeta = meta;
            break;
        }
    }
    if (!foundMeta) return; // Not our list.

    const std::string listId = foundMeta->listId;

    // Decrypt payload.
    const std::string cipherBase64 = ev.content.toStdString();
    auto plainOpt = net::decryptPayload(foundMeta->key, channelTag, cipherBase64);
    if (!plainOpt) {
        // Silently ignore (wrong key, corrupted, or unrelated event).
        return;
    }

    // Parse payload.
    auto payloadOpt = core::parsePayload(*plainOpt);
    if (!payloadOpt) return;

    const core::Payload& payload = *payloadOpt;
    if (payload.listId != listId) return; // Mismatch — ignore.

    // Blob d'une photo : le ranger, prévenir l'UI, c'est tout. Aucun item à merger —
    // le champ `image` des tâches arrive par les deltas, dans n'importe quel ordre.
    if (payload.type == core::Payload::Type::image) {
        if (payload.imageSha.empty() || payload.imageData.empty()) return;
        const QByteArray blob(reinterpret_cast<const char*>(payload.imageData.data()),
                              static_cast<int>(payload.imageData.size()));
        if (payload.imageVariant == "hd")
            m_db->putImageHd(payload.imageSha, blob);
        else
            m_db->putImage(payload.imageSha, blob);
        m_db->updateLastSync(listId, ev.created_at * 1000);
        emit imageArrived(QString::fromStdString(listId),
                          QString::fromStdString(payload.imageSha));
        return;
    }

    // Advance Lamport clock to observe remote values (§1, §2.3).
    // Find max lamport in received items.
    int64_t maxLamport = 0;
    for (const auto& item : payload.items) {
        maxLamport = std::max({maxLamport,
                               item.nameVer.lamport,
                               item.qtyVer.lamport,
                               item.noteVer.lamport,
                               item.aisleVer.lamport,
                               item.imageVer.lamport,
                               item.dueVer.lamport,
                               item.dueNoteVer.lamport,
                               item.orderVer.lamport,
                               item.doneVer.lamport,
                               item.delVer.lamport});
    }
    if (payload.titleVer) maxLamport = std::max(maxLamport, payload.titleVer->lamport);
    if (payload.sortModeVer) maxLamport = std::max(maxLamport, payload.sortModeVer->lamport);
    for (const auto& [did, entry] : payload.members)
        maxLamport = std::max(maxLamport, entry.second.lamport);

    m_db->bumpLamport(listId, maxLamport);

    // Merge items via CRDT.
    auto localItems = m_db->getItems(listId);
    std::map<std::string, core::Item> localMap;
    for (auto& it : localItems) localMap[it.itemId] = it;

    // État « fait » d'avant merge : c'est la transition (pas l'état) qui alimente
    // l'historique — une tâche déjà connue faite ne doit pas s'y consigner deux fois.
    std::map<std::string, bool> wasDone;
    for (const auto& [iid, it] : localMap) wasDone[iid] = it.done;

    const auto changedIds = core::mergeItems(localMap, payload.items);

    // Upsert changed items in DB. touched = heure de publication de l'événement
    // (≈ heure de la modif chez l'auteur), pas l'heure locale de réception — sinon
    // la notification afficherait « à l'instant » pour une modif d'il y a une heure.
    const int64_t eventMs = ev.created_at * 1000;
    for (const auto& iid : changedIds) {
        auto mit = localMap.find(iid);
        if (mit != localMap.end()) {
            mit->second.touched = eventMs;
            m_db->upsertItem(mit->second);
        }
    }

    // Merge title if present.
    // Un renommage distant ne change aucun article : sans signal dédié, l'écran des
    // listes garderait l'ancien nom jusqu'au prochain lancement.
    if (payload.title && payload.titleVer) {
        auto metaOpt2 = m_db->getList(listId);
        if (metaOpt2) {
            bool titleChanged = core::mergeTitle(*metaOpt2, *payload.title, *payload.titleVer);
            if (titleChanged) {
                m_db->updateListTitle(listId, metaOpt2->title, metaOpt2->titleVer);
                emit listTitleChanged(QString::fromStdString(listId),
                                      QString::fromStdString(metaOpt2->title));
            }
        }
    }

    // Merge sort mode if present. Change silencieuse (aucun article touché) : l'écran
    // ouvert la reflète via le rechargement du modèle plus bas ; rien à notifier.
    if (payload.sortMode && payload.sortModeVer) {
        auto metaS = m_db->getList(listId);
        if (metaS && core::mergeSortMode(*metaS, *payload.sortMode, *payload.sortModeVer))
            m_db->updateListSortMode(listId, metaS->sortMode, metaS->sortModeVer);
    }

    // Merge members.
    for (const auto& [did, entry] : payload.members) {
        m_db->upsertMember(listId, did, entry.first, entry.second);
    }

    // Advance last_sync so the next subscribe uses since = lastSync - 1 h (§3.2).
    m_db->updateLastSync(listId, ev.created_at * 1000);

    // Refresh registered ItemModels (écran des tâches, et page d'étapes ouverte).
    auto modelIt = m_models.find(listId);
    if (modelIt != m_models.end()) {
        for (ItemModel* model : modelIt->second) {
            if (model)
                model->load(*m_db, listId, m_deviceId.toStdString(), model->parentId());
        }
    }

    // Nom de l'auteur. Le payload porte son deviceId (champ "by") : on lit SON entrée
    // dans members. Sans ce champ (ancienne version), members.begin() est le seul repli
    // possible — juste pour un delta, qui ne porte que l'auteur ; dans un snap, qui les
    // porte tous, la première entrée est un deviceId arbitraire, éventuellement le nôtre
    // (d'où les « modifié par Moi »). Dans ce cas on préfère taire le nom.
    const std::string ourDevId = m_deviceId.toStdString();
    // Notre propre événement nous revient du relais : les données ont déjà été fusionnées
    // ci-dessus (sans effet), mais il ne faut pas se notifier soi-même.
    const bool ownEvent = (payload.by == ourDevId);

    QString authorName;
    if (!payload.by.empty()) {
        auto authorIt = payload.members.find(payload.by);
        if (authorIt != payload.members.end())
            authorName = QString::fromStdString(authorIt->second.first);
    } else if (payload.type == core::Payload::Type::delta && payload.members.size() == 1
               && payload.members.begin()->first != ourDevId) {
        authorName = QString::fromStdString(payload.members.begin()->second.first);
    }
    if (authorName.isEmpty()) authorName = QStringLiteral("Quelqu'un");

    // Historique : les tâches (pas les étapes) passées à « fait » par ce merge.
    // Notre propre événement revenu du relais est ignoré (déjà consigné au cochage
    // local — et la clé (item, date) dédoublonne de toute façon).
    if (!ownEvent) {
        for (const auto& iid : changedIds) {
            const auto mit = localMap.find(iid);
            if (mit == localMap.end()) continue;
            const core::Item& it = mit->second;
            if (!it.done || it.del || !it.parentId.empty() || it.doneAt <= 0) continue;
            const auto old = wasDone.find(iid);
            if (old != wasDone.end() && old->second) continue;   // déjà faite avant
            m_db->appendHistory(listId,
                { it.itemId, it.name, it.aisle, it.doneAt, authorName.toStdString() });
        }
    }

    const int count = static_cast<int>(changedIds.size());
    if (count > 0)
        emit remoteChanges(QString::fromStdString(listId), count, authorName);

    if (count > 0 && !ownEvent) {
        // System notification — horodatée sur la dernière modif (touched), pas
        // sur l'heure de réception.
        auto metaOpt3 = m_db->getList(listId);
        const QString listTitle = metaOpt3
            ? QString::fromStdString(metaOpt3->title)
            : QString::fromStdString(listId);

        const QString body = QString("%1 tâche(s) modifiée(s) par %2")
                                 .arg(count).arg(authorName);
        qint64 whenMs = 0;
        for (const auto& iid : changedIds) {
            const auto mit = localMap.find(iid);
            if (mit == localMap.end()) continue;
            whenMs = std::max(whenMs, static_cast<qint64>(mit->second.touched));
        }
        showNotification(listTitle, body, whenMs);
    }
}

// ---------------------------------------------------------------------------
// Online / reconnect
// ---------------------------------------------------------------------------

void SyncEngine::onRelayOnline(bool online)
{
    if (!online) return;

    // Flush outbox for all known lists.
    flushOutbox();

    // (Re)subscribe all known lists.
    subscribeAllLists();

    // Migration : s'assurer que chaque liste existe sur les relais actuels
    // (colo-apps), même si elle n'a connu que les anciens relais publics.
    rehostListsOnce();
}

void SyncEngine::rehostListsOnce()
{
    if (!m_db || !m_pool) return;
    for (const auto& meta : m_db->getLists()) {
        const std::string key = "rehostV2." + meta.listId;
        if (m_db->getSetting(key).has_value())
            continue;
        const std::string json = snapPayloadJson(meta.listId);
        if (json.empty())
            continue;
        const std::string eventId = buildAndPublish(meta.listId, json);
        if (eventId.empty())
            continue;
        for (const std::string& sha : m_db->imagesReferenced(meta.listId))
            publishImage(meta.listId, sha);
        resetDeltaCounter(meta.listId);
        m_db->setSetting(key, "1");
        qInfo() << "[SyncEngine] rehosted list" << QString::fromStdString(meta.listId);
    }
}

void SyncEngine::subscribeAllLists(int64_t since)
{
    if (!m_db || !m_pool) return;

    const auto lists = m_db->getLists();
    for (const auto& meta : lists) {
        const std::string channelTag = net::deriveChannelTag(meta.key);
        const QString channelTagQ = QString::fromStdString(channelTag);

        // since = lastSync - 3600 s (1 h overlap, per SPEC §3.2).
        const int64_t sub_since = (since > 0) ? since
            : std::max(int64_t(0), meta.lastSync / 1000 - 3600);

        m_pool->subscribeAll(channelTagQ, sub_since);
        m_subscribedChannels.insert(channelTagQ);
    }
}

void SyncEngine::onListJoined(const std::string& listId)
{
    if (!m_db || !m_pool) return;

    auto metaOpt = m_db->getList(listId);
    if (!metaOpt) return;

    const std::string channelTag = net::deriveChannelTag(metaOpt->key);
    const QString channelTagQ = QString::fromStdString(channelTag);

    // Subscribe without since, limit 500 to catch up full history (SPEC §3.4).
    // since=0 means from the beginning.
    m_pool->subscribeAll(channelTagQ, 0);
    m_subscribedChannels.insert(channelTagQ);
}

// ---------------------------------------------------------------------------
// Outbox
// ---------------------------------------------------------------------------

void SyncEngine::flushOutbox()
{
    if (!m_db) return;

    const auto lists = m_db->getLists();
    for (const auto& meta : lists)
        flushOutboxForList(meta.listId);
}

void SyncEngine::flushOutboxForList(const std::string& listId)
{
    if (!m_db || !m_pool || !m_pool->isOnline()) return;

    // Publish without removing: entries leave the outbox only on ack
    // (onPublishAck), so a drop mid-flush cannot lose events. Re-publishing
    // an already-delivered event is harmless (relay + receiver dedup by id).
    for (const auto& [rowid, eventJson] : m_db->outboxPeekAll(listId)) {
        const QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray::fromStdString(eventJson));
        if (!doc.isObject()) {
            m_db->outboxRemove(rowid); // malformed, never publishable
            emit outboxChanged();
            continue;
        }
        auto evOpt = net::NostrEvent::fromJson(doc.object());
        if (!evOpt) {
            m_db->outboxRemove(rowid);
            emit outboxChanged();
            continue;
        }
        m_pool->publishToAll(*evOpt);
        trackPendingAck(evOpt->id, listId);
    }
}

void SyncEngine::trackPendingAck(const QString& eventId, const std::string& listId)
{
    if (!eventId.isEmpty())
        m_pendingAcks[eventId] = listId;
}

void SyncEngine::onImageRequested(const QString& channelTag, const QString& peerId,
                                  const QString& sha, const QString& variant)
{
    if (!m_db || !m_pool || sha.isEmpty() || peerId.isEmpty())
        return;
    for (const auto& meta : m_db->getLists()) {
        if (net::deriveChannelTag(meta.key) != channelTag.toStdString())
            continue;
        if (variant == "hd")
            publishImageHdToPeer(meta.listId, sha.toStdString(), peerId);
        else
            publishImageToPeer(meta.listId, sha.toStdString(), peerId);
        return;
    }
}

void SyncEngine::onCatchUpRequested(const QString& channelTag, const QString& peerId,
                                    int64_t since)
{
    Q_UNUSED(since);
    if (!m_db || !m_pool || channelTag.isEmpty() || peerId.isEmpty())
        return;
    for (const auto& meta : m_db->getLists()) {
        if (net::deriveChannelTag(meta.key) == channelTag.toStdString()) {
            publishSnapToPeer(meta.listId, peerId);
            return;
        }
    }
}

void SyncEngine::onPublishAckKind(const QString& eventId, net::AckKind kind,
                                  const QString& msg)
{
    if (eventId.isEmpty())
        return;
    if (kind == net::AckKind::Rejected) {
        qWarning() << "[SyncEngine] publish rejected" << eventId.left(12) << msg;
        emit publishRejected(msg);
        return;
    }
    if (kind == net::AckKind::PeerDelivered) {
        // Remise P2P : informative, l'outbox attend l'ack durable du relais.
        qInfo() << "[SyncEngine] peer delivered" << eventId.left(12);
        return;
    }

    auto it = m_pendingAcks.find(eventId);
    if (it == m_pendingAcks.end()) return;

    const std::string& listId = it->second;
    for (const auto& [rowid, json] : m_db->outboxPeekAll(listId)) {
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
        if (!doc.isObject()) continue;
        auto evOpt = net::NostrEvent::fromJson(doc.object());
        if (evOpt && evOpt->id == eventId) {
            m_db->outboxRemove(rowid);
            emit outboxChanged();
            break;
        }
    }
    m_pendingAcks.erase(it);
}

void SyncEngine::onPublishAck(const QString& eventId, bool accepted, const QString& msg)
{
    if (!accepted) return;
    onPublishAckKind(eventId, net::AckKind::RelayDurable, msg);
}
// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::optional<std::string> SyncEngine::channelTagForList(const std::string& listId)
{
    if (!m_db) return std::nullopt;
    auto metaOpt = m_db->getList(listId);
    if (!metaOpt) return std::nullopt;
    return net::deriveChannelTag(metaOpt->key);
}

int64_t SyncEngine::getDeltaCounter(const std::string& listId)
{
    auto opt = m_db->getSetting(std::string(kDeltaCountKey) + listId);
    if (!opt) return 0;
    try { return std::stoll(*opt); } catch (...) { return 0; }
}

int64_t SyncEngine::incrementDeltaCounter(const std::string& listId)
{
    const int64_t n = getDeltaCounter(listId) + 1;
    m_db->setSetting(std::string(kDeltaCountKey) + listId, std::to_string(n));
    return n;
}

void SyncEngine::resetDeltaCounter(const std::string& listId)
{
    m_db->setSetting(std::string(kDeltaCountKey) + listId, "0");
    const int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
    m_db->setSetting(std::string(kLastSnapKey) + listId, std::to_string(nowMs));
}

void SyncEngine::showNotification(const QString& title, const QString& body, qint64 whenMs)
{
    if (m_suppressNotifications) return;
    // Android : notification système via JNI. Ailleurs : no-op, on retombe sur le tray.
    if (platformNotify(title, body, whenMs))
        return;

#if QT_FEATURE_systemtrayicon == 1 && defined(QT_WIDGETS_LIB)
    if (m_tray) {
        auto* tray = qobject_cast<QSystemTrayIcon*>(m_tray);
        if (tray) {
            tray->showMessage(title, body, QSystemTrayIcon::Information, 4000);
            return;
        }
    }
#endif
    qInfo() << "[Notification]" << title << "—" << body;
}

void SyncEngine::schedulePushWake(const std::string &listId)
{
    const QString key = QString::fromStdString(listId);
    QTimer *&timer = m_pushWakeTimers[key];
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(kPushWakeDebounceMs);
        connect(timer, &QTimer::timeout, this, [this, listId]() {
            maybeSendPushWake(listId);
        });
    }
    timer->start();
}

void SyncEngine::maybeSendPushWake(const std::string &listId)
{
    if (!m_db) return;
    // Le réveil n'existe que si l'utilisateur a choisi le mode différé.
    const auto mode = m_db->getSetting("notif.mode");
    if (!mode || *mode != "deferred") return;

    const auto urlOpt = m_db->getSetting("pushBaseUrl");
    const QString base = urlOpt && !urlOpt->empty()
        ? QString::fromStdString(*urlOpt)
        : QStringLiteral("https://colo-apps.les-crevettes-cevenoles.fr/ntfy");
    const auto tagOpt = channelTagForList(listId);
    if (!tagOpt) return;

    net::sendPushWake(base,
                      net::pushTopicForChannel(QString::fromStdString(*tagOpt)),
                      QStringLiteral("Liste mise à jour"),
                      m_deviceId);
}

} // namespace app
