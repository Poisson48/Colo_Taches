#include "appcontroller.h"
#include "platform.h"

#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QTimer>
#include <QImage>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QRegularExpression>
#include <optional>
#include <algorithm>
#include <map>
#include <set>
#include <limits>

#include "../core/types.h"
#include "../core/crdt.h"
#include "../core/pairing.h"
#include "../core/csv.h"
#include "../core/zip.h"
#include "../net/pushclient.h"
#include "../net/crypto.h"
#include "../net/relaypool.h"

namespace app {

// ---------------------------------------------------------------------------
// ListsModel
// ---------------------------------------------------------------------------

ListsModel::ListsModel(QObject *parent)
    : QAbstractListModel(parent)
{}

int ListsModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_rows.size());
}

QVariant ListsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= (int)m_rows.size())
        return {};
    const auto &row = m_rows[static_cast<size_t>(index.row())];
    switch (role) {
    case ListIdRole:     return row.listId;
    case NameRole:       return row.name;
    case CountRole:      return row.count;
    case TotalRole:      return row.total;
    case GroupIdRole:    return row.groupId;
    case GroupNameRole:  return row.groupName;
    case MembersRole:    return row.members;
    case MemberCountRole:return row.memberCount;
    default:             return {};
    }
}

QHash<int, QByteArray> ListsModel::roleNames() const {
    return {
        { ListIdRole,     "listId"      },
        { NameRole,       "name"        },
        { CountRole,      "count"       },
        { TotalRole,      "total"       },
        { GroupIdRole,    "groupId"     },
        { GroupNameRole,  "groupName"   },
        { MembersRole,    "members"     },
        { MemberCountRole,"memberCount" },
    };
}

void ListsModel::reload(store::Database &db, const std::string &deviceId) {
    beginResetModel();
    m_rows.clear();

    // Table des groupes : id → (nom, ordre). Un rang par défaut très grand range les
    // listes non rangées après tous les groupes.
    std::map<std::string, std::pair<QString, int64_t>> groups;
    for (const auto &g : db.getGroups())
        groups[g.groupId] = { QString::fromStdString(g.name), g.sortOrder };

    for (const auto &meta : db.getLists()) {
        int unchecked = 0;
        int total     = 0;
        for (const auto &item : db.getItems(meta.listId)) {
            // Les étapes ne comptent pas : « 2 à faire sur 7 » parle de tâches.
            if (item.del || !item.parentId.empty()) continue;
            ++total;
            if (!item.done) ++unchecked;
        }

        // Avec qui c'est partagé : les membres connus, soi-même exclu. Un membre =
        // quelqu'un dont on a reçu au moins un événement, donc un vrai participant.
        QStringList names;
        for (const auto &[devId, name] : db.getMembers(meta.listId)) {
            if (devId == deviceId || name.empty()) continue;
            names << QString::fromStdString(name);
        }
        names.removeDuplicates();

        Row row;
        row.listId      = QString::fromStdString(meta.listId);
        row.name        = QString::fromStdString(meta.title);
        row.count       = unchecked;
        row.total       = total;
        row.listOrder   = meta.listOrder;
        row.members     = names.join(QStringLiteral(", "));
        row.memberCount = static_cast<int>(names.size());

        const auto git = groups.find(meta.groupId);
        if (!meta.groupId.empty() && git != groups.end()) {
            row.groupId    = QString::fromStdString(meta.groupId);
            row.groupName  = git->second.first;
            row.groupOrder = git->second.second;
        } else {
            // Non rangée (ou groupe supprimé) : après tous les groupes.
            row.groupOrder = std::numeric_limits<int64_t>::max();
        }
        m_rows.push_back(std::move(row));
    }

    // Trier par groupe pour que les sections soient contiguës ; l'ordre d'origine
    // (création) est préservé à l'intérieur d'un groupe par le tri stable.
    std::stable_sort(m_rows.begin(), m_rows.end(), [](const Row &a, const Row &b) {
        if (a.groupOrder != b.groupOrder) return a.groupOrder < b.groupOrder;
        return a.groupName < b.groupName;
    });

    endResetModel();
}

void ListsModel::rename(const QString &listId, const QString &name) {
    const auto it = std::find_if(m_rows.begin(), m_rows.end(),
                                 [&](const Row &r){ return r.listId == listId; });
    if (it == m_rows.end() || it->name == name) return;

    it->name = name;
    const int row = static_cast<int>(std::distance(m_rows.begin(), it));
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { NameRole });
}

void ListsModel::remove(const QString &listId) {
    const auto it = std::find_if(m_rows.begin(), m_rows.end(),
                                 [&](const Row &r){ return r.listId == listId; });
    if (it == m_rows.end()) return;

    const int row = static_cast<int>(std::distance(m_rows.begin(), it));
    beginRemoveRows({}, row, row);
    m_rows.erase(it);
    endRemoveRows();
}

void ListsModel::moveRow(store::Database &db, int from, int to) {
    const int n = static_cast<int>(m_rows.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;

    Row moved = m_rows[static_cast<size_t>(from)];

    // Voisines à l'arrivée, une fois la ligne retirée de sa place actuelle.
    std::vector<Row> without = m_rows;
    without.erase(without.begin() + from);

    const Row *before = (to > 0) ? &without[static_cast<size_t>(to - 1)] : nullptr;
    const Row *after  = (to < static_cast<int>(without.size()))
                        ? &without[static_cast<size_t>(to)] : nullptr;

    // La liste prend le groupe de la ligne survolée (même raisonnement que le rayon
    // d'un article) : en descendant on se pose APRÈS elle (`before`), en montant on
    // prend sa place (`after`).
    const Row *hovered = (from < to) ? before : after;
    const QString targetGroupId    = hovered ? hovered->groupId    : moved.groupId;
    const QString targetGroupName  = hovered ? hovered->groupName  : moved.groupName;
    const int64_t targetGroupOrder = hovered ? hovered->groupOrder : moved.groupOrder;

    // Les voisines qui comptent pour la position sont celles du MÊME groupe.
    const auto sameGroup = [&](const Row *r) {
        return r && r->groupId == targetGroupId;
    };
    const int64_t lo = sameGroup(before) ? before->listOrder : 0;
    const int64_t hi = sameGroup(after)  ? after->listOrder  : 0;

    int64_t order;
    if (lo && hi)      order = lo + (hi - lo) / 2;   // entre les deux
    else if (lo)       order = lo + 1000;            // en fin de groupe
    else if (hi)       order = hi - 1000;            // en tête de groupe
    else               order = moved.listOrder;      // seule de son groupe

    // Intervalle épuisé : renuméroter le groupe à grands pas, puis rejouer le geste.
    if (lo && hi && (order == lo || order == hi)) {
        renumberGroup(db, targetGroupId);
        moveRow(db, from, to);
        return;
    }

    const bool groupChanged = (moved.groupId != targetGroupId);
    if (groupChanged)
        db.setListGroup(moved.listId.toStdString(), targetGroupId.toStdString());
    db.setListOrder(moved.listId.toStdString(), order);

    moved.listOrder  = order;
    moved.groupId    = targetGroupId;
    moved.groupName  = targetGroupName;
    moved.groupOrder = targetGroupOrder;

    // Déplacement de ligne (pas de reset : la ligne glissée ne doit pas être détruite).
    const int dest = (from < to) ? to + 1 : to;   // Qt insère AVANT `dest`
    beginMoveRows({}, from, from, {}, dest);
    m_rows.erase(m_rows.begin() + from);
    m_rows.insert(m_rows.begin() + to, moved);
    endMoveRows();

    if (groupChanged) {
        const QModelIndex idx = index(to);
        emit dataChanged(idx, idx, { GroupIdRole, GroupNameRole });
    }
}

void ListsModel::renumberGroup(store::Database &db, const QString &groupId) {
    int64_t next = 1000;
    for (auto &row : m_rows) {
        if (row.groupId != groupId) continue;
        row.listOrder = next;
        db.setListOrder(row.listId.toStdString(), next);
        next += 1000;
    }
}

// ---------------------------------------------------------------------------
// AppController
// ---------------------------------------------------------------------------

AppController::AppController(QObject *parent)
    : QObject(parent)
    , m_listsModel(new ListsModel(this))
    , m_transport()
    , m_syncEngine(this)
{}

AppController::~AppController()
{
    shutdown();
}

void AppController::shutdown()
{
    platformConfigureNotifications(QStringLiteral("off"), notifProvider(),
                                   notifInterval(), QString(), {}, m_deviceId);
    m_transport.shutdown();
}

QString AppController::databasePath() {
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    return dataDir + QStringLiteral("/colotaches.db");
}

bool AppController::init() {
    if (!m_db.open(databasePath())) {
        return false;
    }

    // Blobs de photos que plus aucune tâche ne référence (photo remplacée ou
    // retirée, tâche supprimée) : les purger au démarrage suffit.
    m_db.purgeOrphanImages();

    // --- deviceId / displayName ---
    QSettings settings;
    const auto kDeviceId    = QStringLiteral("deviceId");
    const auto kDisplayName = QStringLiteral("displayName");

    // Also persist in our own settings table so they survive DB recreation.
    auto devIdOpt = m_db.getSetting("deviceId");
    if (devIdOpt) {
        m_deviceId = QString::fromStdString(*devIdOpt);
    } else {
        // Try QSettings fallback
        if (settings.contains(kDeviceId)) {
            m_deviceId = settings.value(kDeviceId).toString();
        } else {
            m_deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        m_db.setSetting("deviceId", m_deviceId.toStdString());
        settings.setValue(kDeviceId, m_deviceId);
    }

    auto dispOpt = m_db.getSetting("displayName");
    if (dispOpt) {
        m_displayName = QString::fromStdString(*dispOpt);
    } else if (settings.contains(kDisplayName)) {
        m_displayName = settings.value(kDisplayName).toString();
    } else {
        // Repli tant que l'utilisateur n'a rien choisi. Non persisté : il sera redemandé.
        m_displayName = QStringLiteral("Moi");
    }

    // Drapeau explicite, et pas « displayName existe » : les installations d'avant
    // portent un « Moi » persisté que personne n'a choisi. Elles passent ici une fois,
    // l'écran d'accueil redemande le nom, et le drapeau se pose au premier choix.
    m_hasDisplayName = m_db.getSetting("displayNameSet").has_value();

    // --- Load lists ---
    m_listsModel->reload(m_db, m_deviceId.toStdString());

    // --- Setup relay pool ---
    // Load relay URLs from settings (or use defaults).
    auto relaysSetting = m_db.getSetting("relays");
    QList<QUrl> relayUrls;
    if (relaysSetting && !relaysSetting->empty()) {
        const QString relaysStr = QString::fromStdString(*relaysSetting);
        for (const QString& u : relaysStr.split(QLatin1Char(','), Qt::SkipEmptyParts))
            relayUrls.append(QUrl(u.trimmed()));
    }
    if (relayUrls.isEmpty()) {
        relayUrls = net::RelayPool::defaultRelays();
    } else {
        // Migration : colo-apps en tête, les anciens relais publics restent
        // pour les appareils qui n'ont pas encore la mise à jour.
        const QUrl colo(QStringLiteral("wss://colo-apps.les-crevettes-cevenoles.fr"));
        bool hasColo = false;
        for (const QUrl& u : relayUrls)
            if (u == colo) hasColo = true;
        if (!hasColo)
            relayUrls.prepend(colo);
    }
    QStringList relayParts;
    for (const QUrl& u : relayUrls) relayParts.append(u.toString());
    m_db.setSetting("relays", relayParts.join(',').toStdString());
    m_transport.setRelays(relayUrls);

    // Synchronisation serveur uniquement ; migration unique des anciens modes.
    m_syncMode = 0;
    if (!m_db.getSetting("syncModeServerOnlyMigrated")) {
        m_db.setSetting("syncMode", "0");
        m_db.setSetting("syncModeServerOnlyMigrated", "1");
    }
    m_transport.setMode(net::HybridTransport::Mode::RelayOnly);

    // --- Wire SyncEngine ---
    m_syncEngine.init(&m_db, &m_transport, m_deviceId, m_displayName);
    connect(&m_syncEngine, &SyncEngine::onlineChanged,
            this,          &AppController::onSyncOnlineChanged);
    connect(&m_syncEngine, &SyncEngine::remoteChanges,
            this,          &AppController::onRemoteChanges);
    connect(&m_syncEngine, &SyncEngine::listTitleChanged,
            this,          &AppController::onRemoteTitleChanged);
    connect(&m_syncEngine, &SyncEngine::outboxChanged,
            this,          &AppController::onOutboxChanged);

    // Des modifications peuvent dormir dans l'outbox depuis la session précédente
    // (app fermée hors ligne) : l'état de départ n'est pas forcément « à jour ».
    m_pendingChanges = m_db.outboxCount();

    // Toute écriture locale (ajout, cochage, suppression) doit partir au relais.
    // Sans cette connexion, l'app modifie sa base et ne synchronise jamais rien.
    connect(&m_itemModel, &ItemModel::localChanged,
            this,         &AppController::onLocalItemChange);
    // Un ajout manuel enrichit les favoris fréquents : rafraîchir la barre de suggestions.
    connect(&m_itemModel, &ItemModel::itemAdded,
            this,         &AppController::favoritesChanged);

    // Écritures dans la page d'étapes : publier, et recharger le modèle des tâches —
    // la progression « 2/5 » affichée sur la ligne de la tâche vient de lui, et
    // cocher la dernière étape vient de cocher la tâche elle-même.
    connect(&m_stepModel, &ItemModel::localChanged,
            this,         [this](const std::string& listId) {
        onLocalItemChange(listId);
        if (m_openListId == listId)
            m_itemModel.load(m_db, listId, m_deviceId.toStdString());
    });

    // Réciproque : cocher la tâche (depuis la liste ou sa page) coche ses étapes —
    // la page d'étapes ouverte doit les voir se cocher.
    connect(&m_itemModel, &ItemModel::localChanged,
            this,         [this](const std::string& listId) {
        if (!m_stepModel.parentId().empty() && m_openListId == listId)
            m_stepModel.load(m_db, listId, m_deviceId.toStdString(),
                             m_stepModel.parentId());
    });

    // Un blob de photo vient d'arriver : invalider le cache des vignettes.
    connect(&m_syncEngine, &SyncEngine::imageArrived,
            this,          [this](const QString&, const QString&) {
        ++m_imageRevision;
        emit imageRevisionChanged();
    });

    // Aucun job de fond tant que l'utilisateur n'a pas choisi.
    platformSetNotificationPreferences(notifAlert(), notifSignal());
    if (notifMode() == "immediate" && notifProvider() == "unifiedpush") {
        platformRegisterUnifiedPush();
        QStringList topics;
        for (const auto &meta : m_db.getLists())
            topics.append(net::pushTopicForChannel(
                QString::fromStdString(net::deriveChannelTag(meta.key))));
        platformConfigureNotifications(QStringLiteral("off"),
                                       QStringLiteral("unifiedpush"),
                                       notifInterval(), pushBaseUrl(), topics, m_deviceId);
    }
    platformConfigureNotifications(QStringLiteral("off"), notifProvider(),
                                   notifInterval(), QString(), {}, m_deviceId);

    // --- Connect and subscribe ---
    m_transport.connectAll();
    m_syncEngine.subscribeAllLists();

    return true;
}

QAbstractListModel *AppController::lists() const {
    return m_listsModel;
}

ItemModel *AppController::items() {
    return &m_itemModel;
}

ItemModel *AppController::steps() {
    return &m_stepModel;
}

bool AppController::online() const {
    return m_online;
}

int AppController::pendingChanges() const {
    return m_pendingChanges;
}

void AppController::onOutboxChanged() {
    const int pending = m_db.outboxCount();
    if (pending == m_pendingChanges) return;

    const int previous = m_pendingChanges;
    m_pendingChanges = pending;
    emit pendingChangesChanged();

    // On avait des modifications en attente, et un relais vient d'accuser réception de
    // la dernière : le confirmer explicitement. C'est le « synchro réussie » demandé —
    // le bandeau discret ne suffit pas à rassurer sur un envoi ponctuel.
    if (previous > 0 && pending == 0 && m_online)
        emit toast(QStringLiteral("Modifications synchronisées"));
}

void AppController::onLocalItemChange(const std::string& listId) {
    m_syncEngine.onLocalChange(listId);
    // Les compteurs de l'écran des listes ("2 sur 7") dépendent des items.
    m_listsModel->reload(m_db, m_deviceId.toStdString());
}

void AppController::onSyncOnlineChanged(bool online) {
    if (m_online == online) return;
    m_online = online;
    emit onlineChanged();
}

void AppController::onRemoteChanges(const QString& /*listId*/, int /*count*/, const QString& /*authorName*/) {
    // Refresh the lists model so counts update.
    m_listsModel->reload(m_db, m_deviceId.toStdString());
}

void AppController::onRemoteTitleChanged(const QString& listId, const QString& title) {
    m_listsModel->rename(listId, title);
    emit listRenamed(listId, title);
}

QString AppController::deviceId() const {
    return m_deviceId;
}

QString AppController::displayName() const {
    return m_displayName;
}

bool AppController::hasDisplayName() const {
    return m_hasDisplayName;
}

void AppController::createList(const QString &title) {
    core::ListMeta meta;
    // listId: 16 random bytes → base64url 22 chars  (§1)
    meta.listId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    // Clé E2E de la liste (§3.1). Sans elle : canal et chiffrement identiques pour
    // tout le monde, et URI d'appairage rejetée par parseJoinUri (32 octets exigés).
    meta.key      = net::generateListKey();
    meta.title    = title.toStdString();
    meta.titleVer = core::Ver{ 1, m_deviceId.toStdString() };
    meta.lamport  = 1;
    meta.created  = QDateTime::currentMSecsSinceEpoch();

    if (meta.key.size() != 32) {
        emit toast(QStringLiteral("Échec de la génération de la clé de chiffrement"));
        return;
    }

    if (m_db.createList(meta)) {
        m_listsModel->reload(m_db, m_deviceId.toStdString());
        // Souscrire tout de suite : sans ça, la liste n'est écoutée qu'au prochain
        // lancement et les modifications des autres n'arrivent jamais.
        m_syncEngine.onListJoined(meta.listId);
    }
}

void AppController::renameList(const QString &listId, const QString &title) {
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty()) return;

    const std::string id = listId.toStdString();
    auto metaOpt = m_db.getList(id);
    if (!metaOpt || metaOpt->title == trimmed.toStdString()) return;

    // Écriture locale = tick du Lamport de la liste : la nouvelle version bat celle
    // qu'on connaissait, et gagne le merge LWW chez les autres participants.
    const int64_t lamport = m_db.bumpLamport(id);
    const core::Ver ver{ lamport, m_deviceId.toStdString() };

    if (!m_db.updateListTitle(id, trimmed.toStdString(), ver)) return;

    m_listsModel->rename(listId, trimmed);
    emit listRenamed(listId, trimmed);
    m_syncEngine.onLocalChange(id);
}

void AppController::duplicateList(const QString &listId, const QString &title) {
    auto srcOpt = m_db.getList(listId.toStdString());
    if (!srcOpt) return;

    const QString trimmed = title.trimmed();

    core::ListMeta copy;
    copy.listId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    // Clé neuve : la copie est une liste à part, pas une seconde vue de l'originale.
    // Réutiliser la clé source la ferait écrire dans le canal de l'originale.
    copy.key      = net::generateListKey();
    copy.title    = (trimmed.isEmpty()
                        ? QString::fromStdString(srcOpt->title) + QStringLiteral(" (copie)")
                        : trimmed).toStdString();
    copy.titleVer = core::Ver{ 1, m_deviceId.toStdString() };
    copy.lamport  = 1;
    copy.created  = QDateTime::currentMSecsSinceEpoch();

    if (copy.key.size() != 32) {
        emit toast(QStringLiteral("Échec de la génération de la clé de chiffrement"));
        return;
    }
    if (!m_db.createList(copy)) return;

    // Les tâches sont recopiées « à faire » (done=false) : on duplique une liste pour
    // la refaire. Les tombstones de l'originale ne sont pas repris. Multi-passe :
    // un item n'est copié que lorsque son parent (s'il en a un) l'est déjà.
    int copied = 0;
    std::map<std::string, std::string> idMap;   // itemId source → itemId copie
    const auto srcItems = m_db.getItems(srcOpt->listId);
    std::vector<const core::Item *> pending;
    for (const auto &src : srcItems) {
        if (!src.del) pending.push_back(&src);
    }
    while (!pending.empty()) {
        std::vector<const core::Item *> deferred;
        bool progress = false;
        for (const core::Item *psrc : pending) {
            const auto &src = *psrc;
            if (!src.parentId.empty() && !idMap.count(src.parentId)) {
                deferred.push_back(psrc);
                continue;
            }

            const int64_t lamport = m_db.bumpLamport(copy.listId);
            const core::Ver ver{ lamport, m_deviceId.toStdString() };

            core::Item item;
            item.listId  = copy.listId;
            // itemId neuf : garder celui de la source ferait entrer en collision les
            // deux listes si l'une des deux était un jour fusionnée avec l'autre.
            item.itemId  = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            item.created = copy.created + copied; // conserve l'ordre d'affichage de la source
            item.by      = m_deviceId.toStdString();
            item.parentId = src.parentId.empty() ? std::string() : idMap[src.parentId];
            item.name    = src.name;
            item.nameVer = ver;
            item.qty     = src.qty;
            item.qtyVer  = ver;
            item.note    = src.note;
            item.noteVer = ver;
            item.aisle    = src.aisle;
            item.aisleVer = ver;
            // La photo suit : le blob est adressé par contenu, il est déjà en base.
            item.image    = src.image;
            item.imageVer = ver;
            item.due      = src.due;   // l'échéance aussi
            item.dueVer   = ver;
            item.order    = src.order;   // la copie garde le classement de l'originale
            item.orderVer = ver;
            item.done    = false;
            item.doneVer = ver;
            item.doneAt  = 0;
            item.del     = false;
            item.delVer  = ver;
            item.touched = copy.created;

            if (m_db.upsertItem(item)) {
                idMap[src.itemId] = item.itemId;
                ++copied;
                progress = true;
            }
        }
        if (!progress) break;  // orphelins (parent tombstoné) : on les laisse
        pending = std::move(deferred);
    }

    m_listsModel->reload(m_db, m_deviceId.toStdString());
    m_syncEngine.onListJoined(copy.listId);
    // Publier le contenu : sans ça, quelqu'un qui rejoindrait la copie par lien
    // trouverait un canal vide tant que personne n'y touche.
    m_syncEngine.onLocalChange(copy.listId);

    emit toast(copied > 0
        ? QStringLiteral("Liste dupliquée — %1 tâche(s) à faire").arg(copied)
        : QStringLiteral("Liste dupliquée"));
}

void AppController::moveList(int from, int to) {
    m_listsModel->moveRow(m_db, from, to);
}

void AppController::importListInto(const QString &destListId, const QString &sourceListId) {
    if (destListId == sourceListId) return;  // s'importer soi-même n'a pas de sens

    auto destOpt = m_db.getList(destListId.toStdString());
    auto srcOpt  = m_db.getList(sourceListId.toStdString());
    if (!destOpt || !srcOpt) return;

    // Les tâches importées arrivent après celles déjà présentes : leur `order` part
    // de maintenant (ms epoch), forcément au-delà des `order` existants issus d'un
    // `created` passé. Tout est ajouté tel quel, sans fusion (choix : simple et littéral).
    const int64_t now = QDateTime::currentMSecsSinceEpoch();

    int copied = 0;
    std::map<std::string, std::string> idMap;   // itemId source → itemId copie
    const auto srcItems = m_db.getItems(srcOpt->listId);
    // Multi-passe : parent avant enfant (sous-étapes imbriquées comprises).
    std::vector<const core::Item *> pending;
    for (const auto &src : srcItems) {
        if (!src.del) pending.push_back(&src);  // les tombstones de la source ne sont pas repris
    }
    while (!pending.empty()) {
        std::vector<const core::Item *> deferred;
        bool progress = false;
        for (const core::Item *psrc : pending) {
            const auto &src = *psrc;
            if (!src.parentId.empty() && !idMap.count(src.parentId)) {
                deferred.push_back(psrc);
                continue;
            }

            const int64_t lamport = m_db.bumpLamport(destOpt->listId);
            const core::Ver ver{ lamport, m_deviceId.toStdString() };

            core::Item item;
            item.listId  = destOpt->listId;
            // itemId neuf : garder celui de la source ferait entrer en collision les
            // deux listes si l'une était un jour fusionnée avec l'autre.
            item.itemId  = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            item.created = now + copied;   // préserve l'ordre relatif des tâches importées
            item.by      = m_deviceId.toStdString();
            item.parentId = src.parentId.empty() ? std::string() : idMap[src.parentId];
            item.name    = src.name;
            item.nameVer = ver;
            item.qty     = src.qty;
            item.qtyVer  = ver;
            item.note    = src.note;
            item.noteVer = ver;
            item.aisle    = src.aisle;
            item.aisleVer = ver;
            item.image    = src.image;   // blob adressé par contenu, déjà en base
            item.imageVer = ver;
            item.due      = src.due;
            item.dueVer   = ver;
            item.order    = now + copied;  // à la suite des tâches déjà dans la destination
            item.orderVer = ver;
            item.done    = false;          // recopiées « à faire »
            item.doneVer = ver;
            item.doneAt  = 0;
            item.del     = false;
            item.delVer  = ver;
            item.touched = now;

            if (m_db.upsertItem(item)) {
                idMap[src.itemId] = item.itemId;
                ++copied;
                progress = true;
            }
        }
        if (!progress) break;
        pending = std::move(deferred);
    }

    // Si la destination est la liste actuellement ouverte, rafraîchir son écran.
    if (m_openListId == destOpt->listId)
        m_itemModel.load(m_db, destOpt->listId, m_deviceId.toStdString());

    m_listsModel->reload(m_db, m_deviceId.toStdString());
    m_syncEngine.onLocalChange(destOpt->listId);  // publier les ajouts aux autres pairs

    emit toast(copied > 0
        ? QStringLiteral("%1 tâche(s) importée(s) depuis « %2 »")
              .arg(copied).arg(QString::fromStdString(srcOpt->title))
        : QStringLiteral("Rien à importer (liste vide)"));
}

QVariantList AppController::otherLists(const QString &exceptListId) {
    QVariantList out;
    if (!m_db.isOpen()) return out;
    const std::string except = exceptListId.toStdString();
    for (const auto &meta : m_db.getLists()) {
        if (meta.listId == except) continue;
        QVariantMap m;
        m.insert(QStringLiteral("id"),   QString::fromStdString(meta.listId));
        m.insert(QStringLiteral("name"), QString::fromStdString(meta.title));
        out.append(m);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Export / import
// ---------------------------------------------------------------------------

namespace {

// En-tête CSV. L'import reconnaît une ligne d'en-tête à sa première cellule, et se
// passe d'en-tête si le fichier n'en a pas (une simple colonne de noms marche).
// « Etape de » porte le nom de la tâche parente pour les lignes d'étapes.
const std::vector<std::string> kCsvHeader =
    { "Tache", "Quantite", "Description", "Categorie", "Fait", "Etape de" };

bool looksLikeHeader(const std::vector<std::string>& row) {
    if (row.empty()) return false;
    QString first = QString::fromStdString(row[0]).trimmed().toLower();
    return first == "tache" || first == "tâche" || first == "article"
        || first == "nom" || first == "name" || first == "item" || first == "task";
}

bool truthy(const std::string& s) {
    const QString v = QString::fromStdString(s).trimmed().toLower();
    return v == "oui" || v == "1" || v == "true" || v == "x" || v == "vrai";
}

// Nom de fichier sûr : pas de séparateur de chemin ni de caractère interdit.
QString sanitizeFileName(const QString& title) {
    QString out;
    for (QChar c : title) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c < ' ')
            out += ' ';
        else
            out += c;
    }
    out = out.trimmed();
    return out.isEmpty() ? QStringLiteral("liste") : out;
}

} // namespace

QString AppController::listCsv(const QString &listId) {
    auto metaOpt = m_db.getList(listId.toStdString());
    if (!metaOpt) return {};

    std::vector<std::vector<std::string>> rows;
    rows.push_back(kCsvHeader);
    const auto items = m_db.getItems(listId.toStdString());

    // Nom de chaque item (tâches et étapes) pour la colonne « Etape de ».
    std::map<std::string, std::string> namesById;
    for (const auto &it : items)
        if (!it.del) namesById[it.itemId] = it.name;

    // Parent avant enfant (multi-passe) : l'import retrouve toujours le parent.
    std::set<std::string> written;
    bool progress = true;
    while (progress) {
        progress = false;
        for (const auto &it : items) {
            if (it.del || written.count(it.itemId)) continue;
            if (!it.parentId.empty() && !written.count(it.parentId)) continue;

            std::string parentName;
            if (!it.parentId.empty()) {
                const auto pit = namesById.find(it.parentId);
                if (pit == namesById.end()) continue;  // parent disparu : orphelin
                parentName = pit->second;
            }
            rows.push_back({ it.name, it.qty, it.note, it.aisle,
                             it.done ? "oui" : "", parentName });
            written.insert(it.itemId);
            progress = true;
        }
    }
    return QString::fromStdString(core::csvWrite(rows));
}

QString AppController::suggestedFileName(const QString &listId) {
    auto metaOpt = m_db.getList(listId.toStdString());
    const QString title = metaOpt ? QString::fromStdString(metaOpt->title)
                                  : QStringLiteral("liste");
    return sanitizeFileName(title) + QStringLiteral(".csv");
}

// Écrit des octets dans l'URL choisie. Sur Android, le sélecteur renvoie une URI
// content:// que QFile sait ouvrir ; ailleurs, un chemin fichier classique.
static bool writeUrl(const QUrl &url, const QByteArray &bytes) {
    const QString target = url.isLocalFile() ? url.toLocalFile() : url.toString();
    QFile f(target);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "writeUrl: open failed" << target << f.errorString();
        return false;
    }
    const bool ok = f.write(bytes) == bytes.size();
    f.close();
    return ok;
}

static std::optional<QByteArray> readUrl(const QUrl &url) {
    const QString target = url.isLocalFile() ? url.toLocalFile() : url.toString();
    QFile f(target);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "readUrl: open failed" << target << f.errorString();
        return std::nullopt;
    }
    return f.readAll();
}

bool AppController::exportListCsv(const QUrl &fileUrl, const QString &listId) {
    const QString csv = listCsv(listId);
    if (csv.isEmpty() && !m_db.getList(listId.toStdString())) {
        emit toast(QStringLiteral("Liste introuvable"));
        return false;
    }
    if (!writeUrl(fileUrl, csv.toUtf8())) {
        emit toast(QStringLiteral("Échec de l'enregistrement"));
        return false;
    }
    emit toast(QStringLiteral("Liste exportée"));
    return true;
}

bool AppController::exportAllZip(const QUrl &fileUrl) {
    std::vector<core::ZipEntry> entries;
    QStringList usedNames;

    for (const auto &meta : m_db.getLists()) {
        const QString listId = QString::fromStdString(meta.listId);
        // Nom de fichier = titre nettoyé, rendu unique (deux listes peuvent partager
        // un titre) pour ne pas écraser une entrée par une autre dans l'archive.
        QString base = sanitizeFileName(QString::fromStdString(meta.title));
        QString name = base + QStringLiteral(".csv");
        int n = 2;
        while (usedNames.contains(name))
            name = base + QStringLiteral(" (%1).csv").arg(n++);
        usedNames << name;

        entries.push_back({ name.toStdString(), listCsv(listId).toStdString() });
    }

    if (entries.empty()) {
        emit toast(QStringLiteral("Aucune liste à exporter"));
        return false;
    }
    if (!writeUrl(fileUrl, QByteArray::fromStdString(core::zipWrite(entries)))) {
        emit toast(QStringLiteral("Échec de l'enregistrement"));
        return false;
    }
    emit toast(QStringLiteral("%1 liste(s) exportée(s)").arg(entries.size()));
    return true;
}

// Crée une liste locale (clé neuve, non partagée tant qu'on n'a pas diffusé le lien),
// importe les articles décrits par `rows`, et retourne le nombre d'articles ajoutés.
int AppController::importRowsAsList(const QString &title,
                                    const std::vector<std::vector<std::string>> &rows) {
    core::ListMeta meta;
    meta.listId   = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    meta.key      = net::generateListKey();
    meta.title    = title.trimmed().isEmpty() ? std::string("Liste importée")
                                              : title.trimmed().toStdString();
    meta.titleVer = core::Ver{ 1, m_deviceId.toStdString() };
    meta.lamport  = 1;
    meta.created  = QDateTime::currentMSecsSinceEpoch();
    if (meta.key.size() != 32 || !m_db.createList(meta))
        return 0;

    int added = 0;
    // Items créés par nom (dernière gagnante en cas d'homonymes), pour rattacher
    // les lignes « Etape de » — y compris sous-étapes imbriquées.
    std::map<std::string, std::string> idsByName;

    struct PendingRow {
        size_t index = 0;
        std::string name;
        std::string parentName;
        const std::vector<std::string> *row = nullptr;
    };
    std::vector<PendingRow> pending;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        if (looksLikeHeader(row)) continue;
        if (row.empty()) continue;
        const std::string name = QString::fromStdString(row[0]).trimmed().toStdString();
        if (name.empty()) continue;
        const std::string parentName = row.size() > 5
            ? QString::fromStdString(row[5]).trimmed().toStdString() : std::string();
        pending.push_back({ i, name, parentName, &row });
    }

    // Tant que le parent est connu (ou vide), on importe ; sinon on attend.
    while (!pending.empty()) {
        std::vector<PendingRow> deferred;
        bool progress = false;
        for (const auto &pr : pending) {
            std::string parentId;
            if (!pr.parentName.empty()) {
                const auto pit = idsByName.find(pr.parentName);
                if (pit == idsByName.end()) {
                    deferred.push_back(pr);
                    continue;
                }
                parentId = pit->second;
            }

            const int64_t lamport = m_db.bumpLamport(meta.listId);
            const core::Ver ver{ lamport, m_deviceId.toStdString() };
            const auto &row = *pr.row;

            core::Item item;
            item.listId   = meta.listId;
            item.itemId   = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            item.created  = meta.created + added;
            item.by       = m_deviceId.toStdString();
            item.parentId = parentId;
            item.name     = pr.name;                                    item.nameVer  = ver;
            item.qty      = row.size() > 1 ? row[1] : std::string();    item.qtyVer   = ver;
            item.note     = row.size() > 2 ? row[2] : std::string();    item.noteVer  = ver;
            item.aisle    = row.size() > 3 ? row[3] : std::string();    item.aisleVer = ver;
            item.order    = item.created;                               item.orderVer = ver;
            item.done     = row.size() > 4 && truthy(row[4]);           item.doneVer  = ver;
            item.doneAt   = item.done ? meta.created : 0;
            item.delVer   = ver;
            item.touched  = meta.created;

            if (m_db.upsertItem(item)) {
                idsByName[pr.name] = item.itemId;
                ++added;
                progress = true;
            }
        }
        if (!progress) {
            // Parents introuvables : importer le reste en tâches de premier niveau
            // plutôt que de les perdre (comportement historique).
            for (const auto &pr : deferred) {
                const int64_t lamport = m_db.bumpLamport(meta.listId);
                const core::Ver ver{ lamport, m_deviceId.toStdString() };
                const auto &row = *pr.row;

                core::Item item;
                item.listId  = meta.listId;
                item.itemId  = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                item.created = meta.created + added;
                item.by      = m_deviceId.toStdString();
                item.name    = pr.name;                                    item.nameVer  = ver;
                item.qty     = row.size() > 1 ? row[1] : std::string();    item.qtyVer   = ver;
                item.note    = row.size() > 2 ? row[2] : std::string();    item.noteVer  = ver;
                item.aisle   = row.size() > 3 ? row[3] : std::string();    item.aisleVer = ver;
                item.order   = item.created;                               item.orderVer = ver;
                item.done    = row.size() > 4 && truthy(row[4]);           item.doneVer  = ver;
                item.doneAt  = item.done ? meta.created : 0;
                item.delVer  = ver;
                item.touched = meta.created;

                if (m_db.upsertItem(item)) {
                    idsByName[pr.name] = item.itemId;
                    ++added;
                }
            }
            break;
        }
        pending = std::move(deferred);
    }

    m_syncEngine.onListJoined(meta.listId);
    m_syncEngine.onLocalChange(meta.listId);   // publier le contenu importé
    return added;
}

QString AppController::importFile(const QUrl &fileUrl) {
    auto bytes = readUrl(fileUrl);
    if (!bytes) {
        const QString msg = QStringLiteral("Fichier illisible");
        emit toast(msg);
        return msg;
    }

    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
    const std::string raw = bytes->toStdString();

    int lists = 0, items = 0;

    // ZIP (plusieurs listes) reconnu à sa signature, quel que soit le nom du fichier.
    const bool isZip = raw.size() > 4 && raw.compare(0, 4, "PK\x03\x04", 4) == 0;
    if (isZip || path.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        auto archive = core::zipRead(raw);
        if (!archive) {
            const QString msg = QStringLiteral("Archive illisible");
            emit toast(msg);
            return msg;
        }
        for (const auto &entry : *archive) {
            QString title = QString::fromStdString(entry.name);
            if (title.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive))
                title.chop(4);
            const int n = importRowsAsList(title, core::csvParse(entry.data));
            if (n >= 0) { ++lists; items += n; }
        }
    } else {
        // CSV : une seule liste. Le titre vient du nom de fichier.
        QFileInfo info(path);
        QString title = info.completeBaseName();
        if (title.isEmpty()) title = QStringLiteral("Liste importée");
        const int n = importRowsAsList(title, core::csvParse(raw));
        lists = 1; items = n;
    }

    m_listsModel->reload(m_db, m_deviceId.toStdString());

    const QString msg = (lists > 1)
        ? QStringLiteral("%1 listes importées (%2 tâches)").arg(lists).arg(items)
        : QStringLiteral("Liste importée — %1 tâche(s)").arg(items);
    emit toast(msg);
    return msg;
}

QString AppController::createGroup(const QString &name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return {};

    const QString groupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Ordre = date de création : les groupes apparaissent dans l'ordre où on les crée.
    if (!m_db.createGroup(groupId.toStdString(), trimmed.toStdString(),
                          QDateTime::currentMSecsSinceEpoch()))
        return {};

    m_listsModel->reload(m_db, m_deviceId.toStdString());
    return groupId;
}

void AppController::renameGroup(const QString &groupId, const QString &name) {
    const QString trimmed = name.trimmed();
    if (groupId.isEmpty() || trimmed.isEmpty()) return;
    if (m_db.renameGroup(groupId.toStdString(), trimmed.toStdString()))
        m_listsModel->reload(m_db, m_deviceId.toStdString());
}

void AppController::deleteGroup(const QString &groupId) {
    if (groupId.isEmpty()) return;
    if (m_db.deleteGroup(groupId.toStdString())) {
        m_listsModel->reload(m_db, m_deviceId.toStdString());
        emit toast(QStringLiteral("Groupe supprimé — les listes sont conservées"));
    }
}

void AppController::setListGroup(const QString &listId, const QString &groupId) {
    if (m_db.setListGroup(listId.toStdString(), groupId.toStdString()))
        m_listsModel->reload(m_db, m_deviceId.toStdString());
}

QVariantList AppController::groups() {
    QVariantList out;
    if (!m_db.isOpen()) return out;   // appelée avant init() (ou en test) : rien à lire
    for (const auto &g : m_db.getGroups()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"),   QString::fromStdString(g.groupId));
        m.insert(QStringLiteral("name"), QString::fromStdString(g.name));
        out.append(m);
    }
    return out;
}

QString AppController::suggestAisle(const QString &name) {
    if (!m_db.isOpen()) return {};
    return QString::fromStdString(m_db.suggestAisleForName(name.toStdString()));
}

QStringList AppController::customAisles() {
    if (!m_db.isOpen()) return {};

    // Un rayon personnalisé = utilisé par un article, ou mémorisé pour une suggestion,
    // mais absent des rayons d'origine (ceux-là ne se gèrent pas).
    const QStringList defaults = app::ItemModel::defaultAisles();
    QStringList out;
    const auto add = [&](const std::vector<std::string> &src) {
        for (const auto &a : src) {
            const QString aisle = QString::fromStdString(a);
            if (!defaults.contains(aisle) && !out.contains(aisle))
                out << aisle;
        }
    };
    add(m_db.distinctItemAisles());
    add(m_db.distinctMemoryAisles());

    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        return QString::localeAwareCompare(a, b) < 0;
    });
    return out;
}

int AppController::countItemsInAisle(const QString &aisle) {
    if (!m_db.isOpen() || aisle.isEmpty()) return 0;
    const std::string a = aisle.toStdString();
    int n = 0;
    for (const auto &meta : m_db.getLists())
        for (const auto &it : m_db.getItems(meta.listId))
            if (!it.del && it.aisle == a) ++n;
    return n;
}

void AppController::reassignAisle(const QString &oldAisle, const QString &newAisle) {
    if (!m_db.isOpen() || oldAisle.isEmpty() || oldAisle == newAisle) return;

    const std::string from = oldAisle.toStdString();
    const std::string to   = newAisle.toStdString();
    const std::string dev  = m_deviceId.toStdString();
    const int64_t now = QDateTime::currentMSecsSinceEpoch();

    // Réaffecter les articles dans toutes les listes. Le rayon est un champ CRDT
    // répliqué : chaque changement bumpe le Lamport de sa liste, et un onLocalChange
    // par liste touchée publie la mise à jour aux autres participants.
    for (const auto &meta : m_db.getLists()) {
        bool listChanged = false;
        for (auto &it : m_db.getItems(meta.listId)) {
            if (it.del || it.aisle != from) continue;
            const int64_t lamport = m_db.bumpLamport(meta.listId);
            it.aisle    = to;
            it.aisleVer = core::Ver{ lamport, dev };
            it.touched  = now;
            if (m_db.upsertItem(it)) listChanged = true;
        }
        if (listChanged)
            m_syncEngine.onLocalChange(meta.listId);
    }

    // Mémoire des suggestions : renommer, ou oublier si le rayon disparaît.
    if (to.empty())
        m_db.forgetAisleInMemory(from);
    else
        m_db.renameAisleInMemory(from, to);

    // Rafraîchir la liste ouverte (ses articles ont pu changer de section).
    if (!m_openListId.empty())
        m_itemModel.load(m_db, m_openListId, dev);

    emit customAislesChanged();
}

void AppController::renameAisle(const QString &oldAisle, const QString &newAisle) {
    const QString trimmed = newAisle.trimmed();
    if (trimmed.isEmpty()) return;
    reassignAisle(oldAisle, trimmed);
}

void AppController::deleteAisle(const QString &aisle) {
    reassignAisle(aisle, QString());   // vers « sans catégorie »
    emit toast(QStringLiteral("Catégorie supprimée"));
}

QVariantList AppController::favorites() {
    QVariantList out;
    if (!m_db.isOpen()) return out;
    // Une douzaine suffit pour une barre qui se parcourt d'un pouce.
    for (const auto &f : m_db.getFavorites(12)) {
        QVariantMap m;
        m.insert(QStringLiteral("name"),   QString::fromStdString(f.name));
        m.insert(QStringLiteral("qty"),    QString::fromStdString(f.qty));
        m.insert(QStringLiteral("aisle"),  QString::fromStdString(f.aisle));
        m.insert(QStringLiteral("pinned"), f.pinned);
        out.append(m);
    }
    return out;
}

void AppController::pinFavorite(const QString &name, bool pinned) {
    if (m_db.setFavoritePinned(name.toStdString(), pinned))
        emit favoritesChanged();
}

void AppController::removeFavorite(const QString &name) {
    if (m_db.removeFavorite(name.toStdString()))
        emit favoritesChanged();
}

void AppController::leaveList(const QString &listId) {
    const std::string id = listId.toStdString();
    m_syncEngine.unregisterItemModel(id);
    if (m_openListId == id)
        m_openListId.clear();
    if (m_db.deleteList(id)) {
        m_listsModel->remove(listId);
        emit toast(QStringLiteral("Liste supprimée de cet appareil"));
    }
}

void AppController::handleJoinUrl(const QUrl &url) {
    if (joinList(url.toString()))
        emit toast(QStringLiteral("Liste rejointe — synchronisation en cours"));
    else
        emit toast(QStringLiteral("Lien d'invitation invalide"));
}

void AppController::setDisplayName(const QString &name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return;

    // Même nom mais premier choix explicite : il reste à poser le drapeau, sinon
    // l'écran d'accueil redemanderait le nom à chaque lancement.
    const bool firstChoice = !m_hasDisplayName;
    if (trimmed == m_displayName && !firstChoice) return;

    if (firstChoice) {
        m_hasDisplayName = true;
        m_db.setSetting("displayNameSet", "1");
    }

    m_displayName = trimmed;
    m_db.setSetting("displayName", m_displayName.toStdString());
    QSettings().setValue(QStringLiteral("displayName"), m_displayName);
    // Le SyncEngine embarque le nom dans les payloads : le lui repasser (surtout pas
    // via init(), qui rebrancherait les signaux du pool une deuxième fois).
    m_syncEngine.setDisplayName(m_displayName);
    emit displayNameChanged();
}

void AppController::copyToClipboard(const QString &text) {
    if (auto *cb = QGuiApplication::clipboard())
        cb->setText(text);
    emit toast(QStringLiteral("Lien copié"));
}

void AppController::vibrate(int ms) {
    app::platformVibrate(ms);
}

void AppController::setKeepScreenOn(bool on) {
    app::platformKeepScreenOn(on);
}

bool AppController::shareText(const QString &text) {
    if (app::platformShare(text))
        return true;
    // Pas de feuille de partage (desktop) : le presse-papiers fait le travail.
    copyToClipboard(text);
    return false;
}

void AppController::openList(const QString &listId) {
    const std::string id = listId.toStdString();
    auto metaOpt = m_db.getList(id);
    if (!metaOpt) return;

    // Un seul ItemModel pour toutes les listes : le rebrancher sur celle-ci, sinon
    // les événements distants d'une autre liste rafraîchiraient le mauvais écran.
    if (!m_openListId.empty() && m_openListId != id) {
        m_syncEngine.unregisterItemModel(m_openListId, &m_itemModel);
        closeTask();   // la page d'étapes appartenait à l'ancienne liste
    }

    m_itemModel.load(m_db, id, m_deviceId.toStdString());
    m_syncEngine.registerItemModel(id, &m_itemModel);
    m_openListId = id;

    emit listOpened(listId, QString::fromStdString(metaOpt->title));
}

void AppController::openTask(const QString &itemId) {
    if (m_openListId.empty()) return;
    m_stepModel.load(m_db, m_openListId, m_deviceId.toStdString(), itemId.toStdString());
    // Enregistré aussi : un merge distant doit rafraîchir la page d'étapes ouverte.
    m_syncEngine.registerItemModel(m_openListId, &m_stepModel);
}

void AppController::closeTask() {
    if (m_stepModel.parentId().empty()) return;
    m_syncEngine.unregisterItemModel(m_openListId, &m_stepModel);
}

// ---------------------------------------------------------------------------
// Photo d'une tâche
// ---------------------------------------------------------------------------

namespace {
// Réduit une image en JPEG ≤ ~30 Ko. Le blob subit deux couches de base64 (payload
// puis contenu chiffré de l'événement), soit ×1,8 : au-delà, les relais publics
// (limite courante ~64 Ko par événement) refuseraient la photo.
QByteArray compressTaskImage(const QString &path) {
    // QImage(path) rate les URI Android content:// : lire les octets d'abord.
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray raw = f.readAll();
    f.close();
    const QImage img = QImage::fromData(raw);
    if (img.isNull()) return {};
    constexpr int kMaxBytes = 30000;

    int side = 1024;
    for (;;) {
        const QImage scaled = (img.width() > side || img.height() > side)
            ? img.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            : img;
        for (const int quality : { 80, 70, 60, 50, 40 }) {
            QByteArray out;
            QBuffer buf(&out);
            buf.open(QIODevice::WriteOnly);
            scaled.save(&buf, "JPEG", quality);
            if (out.size() > 0 && out.size() <= kMaxBytes)
                return out;
        }
        if (side <= 320)
            return {};   // image irréductible (pathologique) : on renonce proprement
        side = side * 2 / 3;
    }
}

// HD P2P : 2048 px max, JPEG 85, cap 2 Mo. Pas d'upscale.
QByteArray compressTaskImageHd(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray raw = f.readAll();
    f.close();
    const QImage img = QImage::fromData(raw);
    if (img.isNull()) return {};

    constexpr int kMaxHdBytes = 2000000;
    int side = 2048;
    for (;;) {
        const QImage scaled = (img.width() > side || img.height() > side)
            ? img.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            : img;
        for (const int quality : { 90, 85, 80, 75, 70, 60 }) {
            QByteArray out;
            QBuffer buf(&out);
            if (!buf.open(QIODevice::WriteOnly))
                continue;
            if (!scaled.save(&buf, "JPEG", quality))
                continue;
            buf.close();
            if (out.size() > 0 && out.size() <= kMaxHdBytes)
                return out;
        }
        if (side <= 640)
            return {};
        side = side * 3 / 4;
    }
}
} // namespace

QString AppController::tempPhotoPath() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(dir);
    return QDir(dir).filePath(
        QStringLiteral("colotaches-%1.jpg").arg(QDateTime::currentMSecsSinceEpoch()));
}

bool AppController::setTaskImage(const QString &itemId, const QUrl &fileUrl) {
    if (m_openListId.empty()) return false;

    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
    const QByteArray jpeg = compressTaskImage(path);
    if (jpeg.isEmpty()) {
        emit toast(QStringLiteral("Impossible de lire cette image"));
        return false;
    }

    const QString sha = QString::fromLatin1(
        QCryptographicHash::hash(jpeg, QCryptographicHash::Sha256).toHex());

    if (!m_db.putImage(sha.toStdString(), jpeg)) {
        emit toast(QStringLiteral("Échec de l'enregistrement de l'image"));
        return false;
    }

    const QByteArray jpegHd = compressTaskImageHd(path);
    if (!jpegHd.isEmpty())
        m_db.putImageHd(sha.toStdString(), jpegHd);

    // Champ CRDT d'abord (delta débouncé), blob ensuite (événement img direct).
    // L'ordre d'arrivée chez les pairs est indifférent : la vignette s'affiche dès
    // que le blob ET le champ sont là.
    m_itemModel.addItemImage(itemId, sha);
    m_syncEngine.publishImage(m_openListId, sha.toStdString());
    if (!jpegHd.isEmpty())
        m_syncEngine.publishImageHd(m_openListId, sha.toStdString());

    ++m_imageRevision;
    emit imageRevisionChanged();
    return true;
}

void AppController::removeTaskImage(const QString &itemId, const QString &sha) {
    // La photo sort de la liste (LWW) ; le blob orphelin sera purgé au démarrage.
    m_itemModel.removeItemImage(itemId, sha);
}

// ---------------------------------------------------------------------------
// Agenda
// ---------------------------------------------------------------------------

void AppController::addToCalendar(const QString &name, const QString &note, double dueMs) {
    const qint64 start = static_cast<qint64>(dueMs);
    if (name.trimmed().isEmpty() || start <= 0) return;

    const QString description = note.trimmed().isEmpty()
        ? QStringLiteral("Colo Tâches")
        : note.trimmed() + QStringLiteral("\n(Colo Tâches)");

    // Android : l'app d'agenda s'ouvre pré-remplie, l'utilisateur confirme.
    if (platformAddCalendarEvent(name, description, start))
        return;

    // Desktop : un .ics « journée entière » ouvert par l'application par défaut
    // (agenda, Thunderbird…). Format minimal, suffisant pour tous les clients.
    const QDate day = QDateTime::fromMSecsSinceEpoch(start).date();
    const QString ics = QStringLiteral(
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "PRODID:-//ColoTaches//FR\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:%1@colotaches\r\n"
        "DTSTAMP:%2\r\n"
        "DTSTART;VALUE=DATE:%3\r\n"
        "SUMMARY:%4\r\n"
        "DESCRIPTION:%5\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces),
             QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")),
             day.toString(QStringLiteral("yyyyMMdd")),
             QString(name).replace(QLatin1Char('\n'), QLatin1Char(' ')),
             QString(description).replace(QStringLiteral("\n"), QStringLiteral("\\n")));

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString path = dir + QStringLiteral("/colotaches-echeance.ics");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit toast(QStringLiteral("Impossible de préparer l'événement d'agenda"));
        return;
    }
    f.write(ics.toUtf8());
    f.close();

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        emit toast(QStringLiteral("Aucune application d'agenda trouvée"));
}

// ---------------------------------------------------------------------------
// Historique
// ---------------------------------------------------------------------------

QVariantList AppController::history(const QString &listId) {
    QVariantList out;
    if (!m_db.isOpen()) return out;
    // 500 : bien au-delà d'un an de corvées quotidiennes, sans jamais peser à l'écran.
    for (const auto &e : m_db.getHistory(listId.toStdString(), 500)) {
        QVariantMap m;
        m.insert(QStringLiteral("name"),   QString::fromStdString(e.name));
        m.insert(QStringLiteral("aisle"),  QString::fromStdString(e.aisle));
        m.insert(QStringLiteral("doneAt"), static_cast<qlonglong>(e.doneAt));
        m.insert(QStringLiteral("byName"), QString::fromStdString(e.byName));
        out.append(m);
    }
    return out;
}

void AppController::clearHistory(const QString &listId) {
    if (m_db.clearHistory(listId.toStdString()))
        emit toast(QStringLiteral("Historique effacé"));
}

bool AppController::joinList(const QString &uri)
{
    auto infoOpt = core::parseJoinUri(uri.toStdString());
    if (!infoOpt) return false;

    const core::JoinInfo& info = *infoOpt;

    core::ListMeta meta;
    meta.listId   = info.listId;
    meta.key      = info.key;
    meta.title    = info.title;
    meta.titleVer = core::Ver{ 1, m_deviceId.toStdString() };
    meta.lamport  = 1;
    meta.created  = QDateTime::currentMSecsSinceEpoch();

    bool created = m_db.createList(meta);
    if (created) {
        m_listsModel->reload(m_db, m_deviceId.toStdString());
    }
    // Subscribe to catch up full history (SPEC §3.4).
    m_syncEngine.onListJoined(info.listId);
    // Return true even if already exists (already member)
    return true;
}

QString AppController::joinUri(const QString &listId)
{
    auto metaOpt = m_db.getList(listId.toStdString());
    if (!metaOpt) return {};
    return QString::fromStdString(
        core::buildJoinUri(metaOpt->listId, metaOpt->key, metaOpt->title));
}

// ---------------------------------------------------------------------------
// Synchronisation : relais Nostr + serveur P2P indépendants
// ---------------------------------------------------------------------------

static QString deriveSignalingUrl(const QString& base)
{
    QUrl u(base.trimmed());
    if (!u.isValid() || u.host().isEmpty())
        return {};
    QString scheme = u.scheme().toLower();
    if (scheme == QLatin1String("https")) scheme = QStringLiteral("wss");
    else if (scheme == QLatin1String("http")) scheme = QStringLiteral("ws");
    else if (scheme != QLatin1String("wss") && scheme != QLatin1String("ws"))
        return {};
    QString path = u.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    path += QStringLiteral("/signal");
    QUrl out;
    out.setScheme(scheme);
    out.setHost(u.host());
    out.setPort(u.port());
    out.setPath(path);
    return out.toString();
}

QString AppController::p2pServerUrl()
{
    const auto v = m_db.getSetting("p2pServerUrl");
    if (v && !v->empty())
        return QString::fromStdString(*v);
    return defaultP2pServerUrl();
}

QString AppController::defaultP2pServerUrl() const
{
    return QStringLiteral("https://colo-apps.les-crevettes-cevenoles.fr");
}

QString AppController::signalingUrl()
{
    const auto v = m_db.getSetting("signalingUrl");
    if (v && !v->empty())
        return QString::fromStdString(*v);
    return defaultSignalingUrl();
}

QString AppController::defaultSignalingUrl() const
{
    return deriveSignalingUrl(defaultP2pServerUrl());
}

QString AppController::iceServers()
{
    const auto v = m_db.getSetting("iceServers");
    if (v && !v->empty())
        return QString::fromStdString(*v).replace(QLatin1Char(','), QLatin1Char('\n'));
    return defaultIceServers();
}

QString AppController::defaultIceServers() const
{
    return QStringLiteral("stun:stun.l.google.com:19302\n"
                          "stun:colo-apps.les-crevettes-cevenoles.fr:3479\n"
                          "turn:colo-apps.les-crevettes-cevenoles.fr:3479");
}

QString AppController::relayUrls()
{
    const auto v = m_db.getSetting("relays");
    if (v && !v->empty())
        return QString::fromStdString(*v).replace(QLatin1Char(','), QLatin1Char('\n'));
    return defaultRelayUrls();
}

QString AppController::defaultRelayUrls() const
{
    QStringList parts;
    for (const QUrl& u : net::RelayPool::defaultRelays())
        parts.append(u.toString());
    return parts.join(QLatin1Char('\n'));
}

void AppController::setRelayUrls(const QString &text)
{
    if (!m_db.isOpen())
        return;
    QList<QUrl> urls;
    for (const QString& line : text.split(QRegularExpression(QStringLiteral("[\\s,]+")),
                                          Qt::SkipEmptyParts)) {
        const QUrl url(line);
        if (!url.isValid() || url.host().isEmpty()
            || (url.scheme() != QStringLiteral("wss") && url.scheme() != QStringLiteral("ws"))) {
            emit toast(QStringLiteral("URL de relais invalide : %1").arg(line));
            return;
        }
        urls.append(url);
    }
    if (urls.isEmpty()) {
        emit toast(QStringLiteral("Indiquez au moins un relais wss://"));
        return;
    }
    QStringList stored;
    for (const QUrl& u : urls) stored.append(u.toString());
    m_db.setSetting("relays", stored.join(QLatin1Char(',')).toStdString());
    m_transport.setRelays(urls);
    if (m_syncMode != 1) {
        m_transport.relay().connectAll();
        m_syncEngine.subscribeAllLists();
    }
    emit syncModeChanged();
}

void AppController::setP2pServerUrl(const QString &url)
{
    if (!m_db.isOpen())
        return;
    const QUrl base(url.trimmed());
    if (!base.isValid() || base.host().isEmpty()
        || (base.scheme() != QStringLiteral("http") && base.scheme() != QStringLiteral("https"))) {
        emit toast(QStringLiteral("URL de serveur P2P invalide (http/https attendu)"));
        return;
    }
    const QString derived = deriveSignalingUrl(url);
    if (derived.isEmpty()) {
        emit toast(QStringLiteral("URL de serveur P2P invalide"));
        return;
    }
    m_db.setSetting("p2pServerUrl", url.trimmed().toStdString());
    m_db.setSetting("signalingUrl", derived.toStdString());
    m_transport.setSignalingUrl(QUrl(derived));
    m_transport.setP2pServerUrl(base);
    if (m_syncMode != 0) {
        m_transport.p2p().disconnectAll();
        m_transport.p2p().connectAll();
    }
    emit syncModeChanged();
}

void AppController::setSignalingUrl(const QString &url)
{
    if (!m_db.isOpen())
        return;
    const QUrl u(url.trimmed());
    if (!u.isValid() || u.scheme().isEmpty()) {
        emit toast(QStringLiteral("URL de signalisation invalide"));
        return;
    }
    m_db.setSetting("signalingUrl", url.trimmed().toStdString());
    m_transport.setSignalingUrl(u);
    if (m_syncMode != 0) {
        m_transport.p2p().disconnectAll();
        m_transport.p2p().connectAll();
    }
    emit syncModeChanged();
}

void AppController::setIceServers(const QString &text)
{
    if (!m_db.isOpen())
        return;
    const QString stored = text.trimmed().replace(QLatin1Char('\n'), QLatin1Char(','));
    m_db.setSetting("iceServers", stored.toStdString());
    m_transport.setIceServers(stored.split(QLatin1Char(','), Qt::SkipEmptyParts));
    if (m_syncMode != 0) {
        m_transport.p2p().disconnectAll();
        m_transport.p2p().connectAll();
    }
    emit syncModeChanged();
}

void AppController::setSyncMode(int mode)
{
    mode = 0; // P2P retiré du produit : seul le relais est exposé.
    if (mode == m_syncMode)
        return;
    m_syncMode = mode;
    m_db.setSetting("syncMode", std::to_string(mode));
    m_transport.setMode(mode == 0 ? net::HybridTransport::Mode::RelayOnly
                       : mode == 1 ? net::HybridTransport::Mode::P2POnly
                                   : net::HybridTransport::Mode::Hybrid);
    m_syncEngine.init(&m_db, &m_transport, m_deviceId, m_displayName);
    m_transport.connectAll();
    m_syncEngine.subscribeAllLists();
    const bool online = m_transport.isOnline();
    if (online != m_online) {
        m_online = online;
        emit onlineChanged();
    }
    emit syncModeChanged();
    emit toast(mode == 2 ? QStringLiteral("Mode hybride activé")
               : mode == 1 ? QStringLiteral("Mode P2P seul activé")
                           : QStringLiteral("Mode relais seul activé"));
}

void AppController::requestHd(const QString &sha)
{
    if (m_openListId.empty() || sha.isEmpty())
        return;
    auto metaOpt = m_db.getList(m_openListId);
    if (!metaOpt)
        return;
    if (m_db.hasImageHd(sha.toStdString()))
        return;
    m_transport.requestImage(
        QString::fromStdString(net::deriveChannelTag(metaOpt->key)),
        sha, QStringLiteral("hd"));
}

QString AppController::notifMode()
{
    const auto v = m_db.getSetting("notif.mode");
    if (v && (*v == "off" || *v == "deferred" || *v == "immediate"))
        return QString::fromStdString(*v);
    return QStringLiteral("unset");
}

QString AppController::notifProvider()
{
    const auto v = m_db.getSetting("notif.provider");
    return v ? QString::fromStdString(*v) : QStringLiteral("auto");
}

int AppController::notifInterval()
{
    const auto v = m_db.getSetting("notif.interval");
    if (!v) return 15;
    const int minutes = std::atoi(v->c_str());
    return (minutes == 30 || minutes == 60) ? minutes : 15;
}

QString AppController::notifAlert()
{
    const auto v = m_db.getSetting("notif.alert");
    return v && *v == "silent" ? QStringLiteral("silent") : QStringLiteral("notify");
}

QString AppController::notifSignal()
{
    const auto v = m_db.getSetting("notif.signal");
    if (v && (*v == "vibrate" || *v == "visual")) return QString::fromStdString(*v);
    return QStringLiteral("sound");
}

bool AppController::notifOnboardingSeen()
{
    const auto v = m_db.getSetting("notif.onboardingV1");
    return v && *v == "1";
}

void AppController::setNotificationPreferences(const QString &alert, const QString &signal)
{
    if (!m_db.isOpen()) return;
    const QString a = alert == "silent" ? "silent" : "notify";
    QString sig = signal;
    if (sig != "vibrate" && sig != "visual") sig = "sound";
    m_db.setSetting("notif.alert", a.toStdString());
    m_db.setSetting("notif.signal", sig.toStdString());
    platformSetNotificationPreferences(a, sig);
    emit notifSettingsChanged();
}

void AppController::resetNotificationOnboarding()
{
    if (!m_db.isOpen()) return;
    m_db.setSetting("notif.onboardingV1", "0");
    emit notifSettingsChanged();
}

bool AppController::notifUnifiedPushAvailable()
{
    return platformUnifiedPushAvailable();
}

bool AppController::notifImmediateAvailable()
{
    return platformFcmAvailable() || platformUnifiedPushAvailable();
}

QString AppController::pushBaseUrl()
{
    const auto v = m_db.getSetting("pushBaseUrl");
    if (v && !v->empty())
        return QString::fromStdString(*v);
    return defaultPushBaseUrl();
}

QString AppController::defaultPushBaseUrl() const
{
    return QStringLiteral("https://colo-apps.les-crevettes-cevenoles.fr/ntfy");
}

void AppController::markNotificationOnboardingSeen()
{
    if (!m_db.isOpen()) return;
    m_db.setSetting("notif.onboardingV1", "1");
    emit notifSettingsChanged();
}

void AppController::setNotificationMode(const QString &mode, const QString &provider,
                                        int intervalMinutes)
{
    if (!m_db.isOpen()) return;
    if (mode != "off" && mode != "deferred" && mode != "immediate") return;
    if (mode == "immediate" && !notifImmediateAvailable()) {
        emit toast(QStringLiteral("Aucun moteur de notification immédiate disponible"));
        return;
    }
    if (mode == "immediate" && provider == "fcm" && !platformFcmAvailable()) {
        emit toast(QStringLiteral("Google FCM est indisponible sur cet appareil"));
        return;
    }
    if (mode == "immediate" && provider == "unifiedpush"
        && !platformUnifiedPushAvailable()) {
        emit toast(QStringLiteral("Installe un distributeur UnifiedPush (par ex. ntfy)"));
        return;
    }
    if (intervalMinutes != 30 && intervalMinutes != 60) intervalMinutes = 15;
    QString p = provider;
    if (p != "fcm" && p != "unifiedpush" && p != "auto") p = "auto";
    m_db.setSetting("notif.mode", mode.toStdString());
    m_db.setSetting("notif.provider", p.toStdString());
    m_db.setSetting("notif.interval", std::to_string(intervalMinutes));
    if (mode == "immediate" && p == "unifiedpush") {
        platformRegisterUnifiedPush();
        QStringList topics;
        for (const auto &meta : m_db.getLists())
            topics.append(net::pushTopicForChannel(
                QString::fromStdString(net::deriveChannelTag(meta.key))));
        platformConfigureNotifications(QStringLiteral("off"),
                                       QStringLiteral("unifiedpush"),
                                       notifInterval(), pushBaseUrl(), topics, m_deviceId);
    }
    m_db.setSetting("notif.onboardingV1", "1");
    refreshPushTopics();
    emit notifSettingsChanged();
    emit toast(mode == "off" ? QStringLiteral("Notifications désactivées")
              : mode == "immediate" ? QStringLiteral("Notifications immédiates activées")
                                    : QStringLiteral("Notifications différées activées"));
}

void AppController::setNotificationInterval(int minutes)
{
    if (minutes != 30 && minutes != 60) minutes = 15;
    if (notifInterval() == minutes) return;
    m_db.setSetting("notif.interval", std::to_string(minutes));
    refreshPushTopics();
    emit notifSettingsChanged();
}

void AppController::testNotification()
{
    const bool ok = platformNotify(QStringLiteral("Colo Tâches"),
                                   QStringLiteral("Les notifications fonctionnent."));
    qInfo() << "[Notification] test demandé, ok=" << ok;
    emit toast(ok ? QStringLiteral("Notification de test envoyée")
                  : QStringLiteral("Notifications système indisponibles sur cet appareil"));
}

void AppController::refreshPushTopics()
{
    const QString mode = notifMode();
    const bool on = (mode == "deferred" || mode == "immediate");
    auto currentTopics = [this]() {
        QStringList topics;
        for (const auto &meta : m_db.getLists())
            topics.append(net::pushTopicForChannel(
                QString::fromStdString(net::deriveChannelTag(meta.key))));
        return topics;
    };
    if (!on) {
        platformConfigureNotifications(QStringLiteral("off"), notifProvider(),
                                       notifInterval(), pushBaseUrl(), currentTopics(),
                                       m_deviceId);
        return;
    }
    if (QGuiApplication::applicationState() == Qt::ApplicationActive) {
        platformConfigureNotifications(QStringLiteral("off"), notifProvider(),
                                       notifInterval(), pushBaseUrl(), currentTopics(),
                                       m_deviceId);
        return;
    }
    if (mode == "immediate") {
        if (notifProvider() == QStringLiteral("unifiedpush")) {
            platformRegisterUnifiedPush();
            platformConfigureNotifications(QStringLiteral("off"),
                                           QStringLiteral("unifiedpush"),
                                           notifInterval(), pushBaseUrl(),
                                           currentTopics(), m_deviceId);
            return;
        }
        platformConfigureNotifications(QStringLiteral("immediate"),
                                       QStringLiteral("fcm"),
                                       notifInterval(), pushBaseUrl(),
                                       currentTopics(), m_deviceId);
        return;
    }
    platformConfigureNotifications(QStringLiteral("deferred"), notifProvider(),
                                   notifInterval(), pushBaseUrl(), currentTopics(),
                                   m_deviceId);
}

void AppController::backgroundSyncNow()
{
    m_transport.connectAll();
    m_syncEngine.subscribeAllLists();
    QTimer::singleShot(15'000, this, [this]() {
        if (QGuiApplication::applicationState() != Qt::ApplicationActive)
            m_transport.disconnectAll();
    });
}

void AppController::onApplicationStateChanged(bool active)
{
    if (active) {
        platformConfigureNotifications(QStringLiteral("off"), notifProvider(),
                                       notifInterval(), QString(), {}, m_deviceId);
    } else {
        refreshPushTopics();
    }
}

} // namespace app
