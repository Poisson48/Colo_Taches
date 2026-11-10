#pragma once

#include <QObject>
#include <QAbstractListModel>
#include <QString>
#include <QVariant>
#include <QUrl>
#include <vector>
#include <string>
#include "../store/database.h"
#include "../core/types.h"
#include "../core/pairing.h"
#include "../net/hybridtransport.h"
#include "itemmodel.h"
#include "syncengine.h"

namespace app {

// ListsModel: exposes the list-of-lists to QML.
// Roles: listId, name, count (unchecked items).
class ListsModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        ListIdRole = Qt::UserRole + 1,
        NameRole,
        CountRole,
        TotalRole,
        // Groupe local : identifiant, et nom affiché en en-tête de section.
        GroupIdRole,
        GroupNameRole,
        // Avec qui la liste est partagée : noms joints (« Marie, Léo »), et leur nombre.
        MembersRole,
        MemberCountRole,
    };

    explicit ListsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Reload from DB. deviceId sert à s'exclure soi-même de la liste des participants.
    void reload(store::Database &db, const std::string &deviceId);

    // Remove a row by listId (no-op if absent).
    void remove(const QString &listId);

    // Change a row's displayed name (no-op if absent).
    void rename(const QString &listId, const QString &name);

    // Déplacer une liste (réordonnancement manuel). `from`/`to` sont des index du
    // modèle. Franchir une frontière de groupe range aussi la liste dans ce groupe —
    // même geste que pour les articles et les rayons. Purement local.
    void moveRow(store::Database &db, int from, int to);

private:
    struct Row {
        QString listId;
        QString name;
        int     count = 0;  // articles restant à acheter
        int     total = 0;  // articles visibles (tombstones exclus)
        QString groupId;
        QString groupName;  // "" = non rangé, affiché en dernier
        int64_t groupOrder = 0;
        int64_t listOrder = 0;  // position manuelle dans le groupe
        QString members;    // noms des autres participants, joints
        int     memberCount = 0;
    };

    // Ré-espace les positions des listes d'un groupe quand l'intervalle est épuisé.
    void renumberGroup(store::Database &db, const QString &groupId);

    std::vector<Row> m_rows;
};

// AppController: singleton QObject exposed to QML as a context property.
class AppController : public QObject {
    Q_OBJECT

    Q_PROPERTY(QAbstractListModel* lists READ lists CONSTANT)
    // Tâches de la liste ouverte. Chargé par openList(), branché au SyncEngine.
    Q_PROPERTY(app::ItemModel* items READ items CONSTANT)
    // Étapes de la tâche ouverte (page de détail). Chargé par openTask().
    Q_PROPERTY(app::ItemModel* steps READ steps CONSTANT)
    // Compteur d'invalidation des vignettes : incrémenté quand le blob d'une photo
    // arrive d'un relais. Les vues l'ajoutent à l'URL du provider ("?r=N") pour
    // forcer un rechargement — sans lui, une vignette d'abord absente resterait vide.
    Q_PROPERTY(int imageRevision READ imageRevision NOTIFY imageRevisionChanged)
    Q_PROPERTY(bool online READ online NOTIFY onlineChanged)
    // Modifications écrites localement mais pas encore accusées par un relais. Zéro =
    // tout le monde a reçu. Sans ça, rien ne dit à l'utilisateur si ses ajouts sont
    // partis, et il n'a aucun moyen de le savoir avant de croiser l'autre personne.
    Q_PROPERTY(int pendingChanges READ pendingChanges NOTIFY pendingChangesChanged)
    // Articles fréquents, appris à l'usage : proposés en un tap sous la barre d'ajout.
    // [{ name, qty, aisle, pinned }, …], les plus utiles d'abord.
    Q_PROPERTY(QVariantList favorites READ favorites NOTIFY favoritesChanged)
    // Rayons personnalisés (créés par l'utilisateur, hors rayons d'origine), pour l'écran
    // de gestion. Triés. Un rayon d'origine ne peut être ni renommé ni supprimé.
    Q_PROPERTY(QStringList customAisles READ customAisles NOTIFY customAislesChanged)
    // Nom affiché aux autres participants ("3 articles ajoutés par Marie").
    Q_PROPERTY(QString displayName READ displayName WRITE setDisplayName NOTIFY displayNameChanged)
    // false tant que l'utilisateur n'a pas choisi son nom : l'écran d'accueil le
    // demande. Sans ça, tout le monde s'appelle « Moi » et les notifications
    // deviennent illisibles (« 2 articles modifiés par Moi »).
    Q_PROPERTY(bool hasDisplayName READ hasDisplayName NOTIFY displayNameChanged)
    // Synchronisation : relais Nostr et serveur P2P sont deux réglages distincts.
    Q_PROPERTY(int syncMode READ syncMode NOTIFY syncModeChanged)
    Q_PROPERTY(QString p2pServerUrl READ p2pServerUrl NOTIFY syncModeChanged)
    Q_PROPERTY(QString signalingUrl READ signalingUrl NOTIFY syncModeChanged)
    Q_PROPERTY(QString iceServers READ iceServers NOTIFY syncModeChanged)

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    // Arrêt ordonné (transport puis modèles).
    void shutdown();

    // Initialize: open DB, generate/load deviceId + displayName.
    bool init();

    QAbstractListModel *lists() const;
    ItemModel *items();
    ItemModel *steps();
    bool online() const;
    int  pendingChanges() const;
    int  imageRevision() const { return m_imageRevision; }

    // Chemin de la base (partagé avec le provider d'images, qui ouvre sa propre
    // connexion : les requêtes d'images arrivent d'un thread de rendu).
    static QString databasePath();

    QString deviceId() const;
    QString displayName() const;
    bool    hasDisplayName() const;
    int     syncMode() const { return m_syncMode; }
    QString p2pServerUrl();
    QString signalingUrl();
    QString iceServers();

    store::Database &db() { return m_db; }

public slots:
    void createList(const QString &title);
    // Renommer une liste. Le titre est un champ CRDT (LWW) : le renommage part au
    // relais comme une modification d'article.
    void renameList(const QString &listId, const QString &title);
    // Dupliquer une liste : nouvelle liste, nouvelle clé, articles recopiés « à
    // acheter ». Purement local — c'est une liste distincte, pas un partage.
    void duplicateList(const QString &listId, const QString &title);
    // Importer le contenu d'une liste (source) dans une autre (destination), sans
    // toucher à la source : ses articles sont recopiés « à acheter » à la fin de la
    // destination. Permet de garder des listes-modèles réutilisables (« courants »).
    // Tout est ajouté tel quel — pas de fusion : un article déjà présent fait un doublon.
    void importListInto(const QString &destListId, const QString &sourceListId);
    // Autres listes que `exceptListId` : [{ id, name }, …], pour le sélecteur d'import.
    QVariantList otherLists(const QString &exceptListId);
    // openList is called from QML to open a list; emits listOpened.
    void openList(const QString &listId);
    // Charger les étapes d'une tâche de la liste ouverte dans le modèle `steps`
    // (page de détail). closeTask() débranche le modèle quand la page se ferme.
    void openTask(const QString &itemId);
    void closeTask();

    // --- Photos d'une tâche (plusieurs : avant, après…) ---
    // Ajoute une image : lecture, réduction (JPEG ≤ ~30 Ko pour passer les relais),
    // stockage local, ajout à la liste du champ CRDT et publication du blob. false
    // si le fichier est illisible.
    bool setTaskImage(const QString &itemId, const QUrl &fileUrl);
    // Retire UNE photo de la tâche (les autres restent).
    void removeTaskImage(const QString &itemId, const QString &sha);
    // Chemin temporaire pour une capture caméra (fichier local, prêt pour setTaskImage).
    Q_INVOKABLE QString tempPhotoPath() const;
    // Demande la HD d'une photo à un pair P2P (best-effort).
    Q_INVOKABLE void requestHd(const QString &sha);

    // --- Synchronisation ---
    Q_INVOKABLE void setSyncMode(int mode);
    Q_INVOKABLE void setP2pServerUrl(const QString &url);
    Q_INVOKABLE QString defaultP2pServerUrl() const;
    Q_INVOKABLE void setSignalingUrl(const QString &url);
    Q_INVOKABLE QString defaultSignalingUrl() const;
    Q_INVOKABLE void setIceServers(const QString &text);
    Q_INVOKABLE QString defaultIceServers() const;
    Q_INVOKABLE void setRelayUrls(const QString &text);
    Q_INVOKABLE QString relayUrls();
    Q_INVOKABLE QString defaultRelayUrls() const;

    // --- Échéance / agenda ---
    // Propose l'événement à l'agenda : Android ouvre l'app d'agenda pré-remplie
    // (journée entière) ; ailleurs, un fichier .ics est ouvert par l'app par défaut.
    void addToCalendar(const QString &name, const QString &note, double dueMs);

    // --- Historique (local) : ce qui a déjà été fait dans une liste ---
    // [{ name, aisle, doneAt, byName }, …], le plus récent d'abord.
    QVariantList history(const QString &listId);
    void clearHistory(const QString &listId);
    // Parse URI → create list with provided key → true on success
    bool joinList(const QString &uri);
    // Build pairing URI for an existing list
    QString joinUri(const QString &listId);
    // Quitter une liste : effacement local uniquement (les autres la gardent).
    void leaveList(const QString &listId);

    // Réordonner les listes à la main (index du modèle). Purement local.
    void moveList(int from, int to);

    // --- Groupes (organisation locale des listes) ---
    // Crée un groupe et retourne son identifiant (pour y ranger la liste dans la foulée).
    QString createGroup(const QString &name);
    void    renameGroup(const QString &groupId, const QString &name);
    // Supprime le groupe ; ses listes redeviennent « non rangées », rien n'est effacé.
    void    deleteGroup(const QString &groupId);
    // Ranger une liste dans un groupe existant, ou la sortir de tout groupe ("").
    void    setListGroup(const QString &listId, const QString &groupId);
    // Groupes existants, pour le menu « Ranger dans… » : [{ id, name }, …].
    QVariantList groups();

    // Rayon suggéré pour un nom d'article (d'après les articles déjà classés), "" si
    // inconnu. Sert UNIQUEMENT à pré-remplir le sélecteur : rien n'est assigné en douce.
    QString suggestAisle(const QString &name);

    // --- Gestion des rayons personnalisés (écran « Mes listes ») ---
    QStringList customAisles();
    // Nombre d'articles (toutes listes) rangés dans ce rayon — pour le message de confirmation.
    int countItemsInAisle(const QString &aisle);
    // Renommer un rayon partout : articles de toutes les listes + mémoire des suggestions.
    // C'est une modification synchronisée (le rayon d'un article est un champ répliqué).
    void renameAisle(const QString &oldAisle, const QString &newAisle);
    // Supprimer un rayon : les articles qui l'utilisaient repassent « sans rayon ».
    void deleteAisle(const QString &aisle);

    // --- Favoris (articles fréquents) ---
    QVariantList favorites();
    // Épingler un favori en tête (ou le désépingler), ou le retirer des suggestions.
    void pinFavorite(const QString &name, bool pinned);
    void removeFavorite(const QString &name);

    // --- Export / import (CSV, et ZIP pour tout d'un coup) ---
    // CSV d'une liste, en texte : pour le partage direct et le presse-papiers.
    QString listCsv(const QString &listId);
    // Écrit le CSV d'une liste dans le fichier choisi (fileUrl). false si l'écriture échoue.
    bool exportListCsv(const QUrl &fileUrl, const QString &listId);
    // Écrit toutes les listes dans un ZIP (un CSV par liste).
    bool exportAllZip(const QUrl &fileUrl);
    // Importe un fichier .csv (une liste) ou .zip (plusieurs). Crée de nouvelles listes,
    // sans toucher aux existantes. Retourne un message prêt pour le snackbar.
    QString importFile(const QUrl &fileUrl);
    // Nom de fichier suggéré pour l'export d'une liste (titre nettoyé + .csv).
    QString suggestedFileName(const QString &listId);

    // Deep link colotaches://join/... (lien tapé dans WhatsApp, ou QR scanné).
    void handleJoinUrl(const QUrl &url);

    void setDisplayName(const QString &name);

    // Presse-papiers, et partage natif (feuille de partage Android ; ailleurs :
    // copie dans le presse-papiers). Retourne false si le partage a échoué.
    void copyToClipboard(const QString &text);
    bool shareText(const QString &text);

    // Confort natif, sans effet hors Android : vibration courte au cochage, et écran
    // maintenu allumé pendant le mode Courses.
    void vibrate(int ms = 18);
    void setKeepScreenOn(bool on);

    // Access SyncEngine (for ItemModel integration).
    SyncEngine *syncEngine() { return &m_syncEngine; }

signals:
    void onlineChanged();
    void pendingChangesChanged();
    void imageRevisionChanged();
    void favoritesChanged();
    void customAislesChanged();
    void displayNameChanged();
    void syncModeChanged();
    // Emitted when QML should push the item page.
    void listOpened(const QString &listId, const QString &title);
    // Titre changé (ici ou par un autre appareil) : l'en-tête de la liste ouverte suit.
    void listRenamed(const QString &listId, const QString &title);
    // Message court à afficher en bas de l'écran (snackbar).
    void toast(const QString &message);

private slots:
    void onSyncOnlineChanged(bool online);
    void onRemoteChanges(const QString& listId, int count, const QString& authorName);
    void onRemoteTitleChanged(const QString& listId, const QString& title);
    void onOutboxChanged();
    // Écriture locale dans la liste ouverte → publier + rafraîchir les compteurs.
    void onLocalItemChange(const std::string& listId);

private:
    // Crée une liste locale et y importe les articles décrits par `rows`. Retourne le
    // nombre d'articles ajoutés. Ne recharge pas le modèle (l'appelant groupe l'import).
    int importRowsAsList(const QString &title,
                         const std::vector<std::vector<std::string>> &rows);

    // Réaffecte partout les articles du rayon `oldAisle` vers `newAisle` (vide = « sans
    // rayon »), met à jour la mémoire, resynchronise les listes touchées.
    void reassignAisle(const QString &oldAisle, const QString &newAisle);

    store::Database  m_db;
    ListsModel      *m_listsModel;
    ItemModel        m_itemModel;
    // Étapes de la tâche ouverte : un second modèle sur la même liste, filtré par
    // tâche parente. Enregistré auprès du SyncEngine le temps que la page est ouverte.
    ItemModel        m_stepModel;
    net::HybridTransport m_transport;
    SyncEngine       m_syncEngine;
    bool             m_online = false;
    int              m_pendingChanges = 0;
    int              m_imageRevision = 0;
    QString          m_deviceId;
    QString          m_displayName;
    bool             m_hasDisplayName = false;
    std::string      m_openListId;   // liste actuellement chargée dans m_itemModel
    int              m_syncMode = 2; // 0 relais seul, 1 P2P seul, 2 hybride
};

} // namespace app
