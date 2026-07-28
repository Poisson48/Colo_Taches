#pragma once

#include "../core/types.h"

#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <vector>
#include <optional>
#include <string>

namespace store {

// Persistent storage for Colo_Taches. One SQLite file, WAL mode.
// All merge operations + lamport/last_sync updates are transactional (§6).
class Database
{
public:
    Database() = default;
    ~Database();

    // Open (or create) the database at the given file path.
    // Returns true on success.
    bool open(const QString& path);

    void close();

    // La base est-elle ouverte ? Permet aux appelants d'éviter une requête (et son
    // avertissement) quand l'app n'a pas encore initialisé le stockage.
    bool isOpen() const { return m_db.isOpen(); }

    // --- Lists ---
    bool createList(const core::ListMeta& meta);
    std::vector<core::ListMeta> getLists();
    std::optional<core::ListMeta> getList(const std::string& listId);
    // Persist an LWW-merged title (caller does the mergeTitle comparison).
    bool updateListTitle(const std::string& listId,
                         const std::string& title,
                         const core::Ver& ver);
    // Persist an LWW-merged sort mode (caller does the mergeSortMode comparison).
    bool updateListSortMode(const std::string& listId,
                            const std::string& sortMode,
                            const core::Ver& ver);
    // Advance last_sync (ms epoch); only moves forward.
    bool updateLastSync(const std::string& listId, int64_t ms);
    // Quitter une liste : efface toute trace locale (liste, items, membres, outbox).
    // Purement local — aucune opération CRDT n'est émise, les autres participants
    // gardent la liste (§2.2 : pas de suppression de liste répliquée).
    bool deleteList(const std::string& listId);
    // Ranger une liste dans un groupe (groupId vide = la sortir de tout groupe).
    bool setListGroup(const std::string& listId, const std::string& groupId);
    // Position manuelle locale de la liste dans son groupe (réordonnancement).
    bool setListOrder(const std::string& listId, int64_t order);

    // --- Groups (local uniquement : organisation propre à l'appareil) ---
    struct Group { std::string groupId; std::string name; int64_t sortOrder; };
    bool createGroup(const std::string& groupId, const std::string& name, int64_t sortOrder);
    bool renameGroup(const std::string& groupId, const std::string& name);
    // Supprime le groupe ; ses listes ne sont pas effacées, juste sorties du groupe.
    bool deleteGroup(const std::string& groupId);
    std::vector<Group> getGroups();

    // --- Favoris (local : articles fréquents, appris à l'usage) ---
    struct Favorite { std::string name; std::string qty; std::string aisle;
                      int64_t uses = 0; bool pinned = false; };
    // Enregistre l'ajout d'un article : crée le favori ou incrémente son compteur, et
    // mémorise sa dernière quantité / son dernier rayon non vides comme valeurs par défaut.
    bool recordFavoriteUse(const std::string& name, const std::string& qty,
                           const std::string& aisle, int64_t nowMs);
    // Favoris les plus utiles d'abord (épinglés, puis fréquence, puis récence).
    std::vector<Favorite> getFavorites(int limit);
    bool setFavoritePinned(const std::string& name, bool pinned);
    bool removeFavorite(const std::string& name);

    // --- Mémoire des rayons (local : où retombe un article par son nom) ---
    // Mémorise que les articles commençant par le premier mot de `name` vont dans
    // `aisle`. Sans effet si le nom ou le rayon est vide.
    bool recordAisleForName(const std::string& name, const std::string& aisle, int64_t nowMs);
    // Rayon suggéré pour un nom (via son premier mot), ou "" si inconnu.
    std::string suggestAisleForName(const std::string& name);
    // Rayons distincts réellement utilisés par des articles (toutes listes, hors
    // tombstones et hors rayon vide), et ceux mémorisés pour les suggestions.
    std::vector<std::string> distinctItemAisles();
    std::vector<std::string> distinctMemoryAisles();
    // Renommer / oublier un rayon dans la mémoire des suggestions.
    bool renameAisleInMemory(const std::string& oldAisle, const std::string& newAisle);
    bool forgetAisleInMemory(const std::string& aisle);

    // --- Items ---
    // Insert or update an item. Transactional (updates lamport if needed).
    bool upsertItem(const core::Item& item);
    std::vector<core::Item> getItems(const std::string& listId);

    // --- Images (blobs des photos de tâches, adressés par contenu) ---
    // Stocke un blob JPEG sous son sha256 hex. Idempotent (le contenu ne change pas
    // pour un sha donné) : ré-insérer le même blob est sans effet.
    bool putImage(const std::string& sha, const QByteArray& data);
    // Blob d'une image, ou tableau vide si absente (pas encore reçue du relais).
    QByteArray getImage(const std::string& sha);
    bool hasImage(const std::string& sha);
    // Empreintes des images encore référencées par un item vivant de la liste —
    // celles qu'un snapshot doit republier (rétention limitée des relais).
    std::vector<std::string> imagesReferenced(const std::string& listId);
    // Purge les blobs que plus aucun item (toutes listes) ne référence.
    bool purgeOrphanImages();

    // --- Members ---
    // member: (listId, deviceId, name, ver)
    bool upsertMember(const std::string& listId,
                      const std::string& deviceId,
                      const std::string& name,
                      const core::Ver&   ver);
    // Returns pairs of (deviceId, name).
    std::vector<std::pair<std::string, std::string>> getMembers(const std::string& listId);

    // --- Outbox (FIFO queue of encrypted events) ---
    bool        outboxPush(const std::string& listId, const std::string& eventJson);
    // Remove one specific entry (targeted ack) — acks may arrive out of order,
    // so FIFO pop must never be used to acknowledge.
    bool        outboxRemove(int64_t rowid);
    // Returns the oldest entry's (rowid, eventJson); removes it from the queue.
    std::optional<std::pair<int64_t, std::string>> outboxPop(const std::string& listId);
    // Peek all entries for a list in FIFO order (rowid, eventJson).
    std::vector<std::pair<int64_t, std::string>> outboxPeekAll(const std::string& listId);
    // Nombre d'événements en attente d'envoi, toutes listes confondues. C'est ce que
    // l'UI traduit par « modifications en attente » / « à jour ».
    int outboxCount();

    // --- Seen-events dedup ---
    bool markEventSeen(const std::string& eventId);
    bool isEventSeen(const std::string& eventId);
    // Remove seen_events entries older than cutoffMs (ms epoch).
    bool purgeSeenBefore(int64_t cutoffMs);

    // --- Settings (key/value) ---
    std::optional<std::string> getSetting(const std::string& key);
    bool setSetting(const std::string& key, const std::string& value);

    // --- Lamport clock ---
    // Transactionally: new_clock = max(current_clock + 1, atLeast).
    // Returns the new value, or -1 on error.
    int64_t bumpLamport(const std::string& listId, int64_t atLeast = 0);

private:
    bool createSchema();
    // Ajoute les colonnes absentes des bases créées par une version antérieure.
    bool migrateSchema();

    QSqlDatabase m_db;
    QString      m_connectionName;
};

} // namespace store
