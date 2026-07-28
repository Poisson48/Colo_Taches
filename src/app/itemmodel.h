#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QStringList>
#include <map>
#include <vector>
#include "../store/database.h"
#include "../core/types.h"

namespace app {

// ItemModel: QAbstractListModel for the tasks of a single list — or, en mode
// étapes (parentId non vide), pour les étapes d'une seule tâche.
// Sorting (§7 SPEC): unchecked first, then by created ascending.
// Deleted items (del=true) are hidden (tombstone filter).
class ItemModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Articles déjà dans le panier : c'est la progression qu'affiche le mode Courses.
    Q_PROPERTY(int doneCount READ doneCount NOTIFY doneCountChanged)
    // Filtre de recherche (nom, quantité, description). Vide = tout est visible.
    // Filtre d'affichage seulement : rien n'est supprimé, rien n'est synchronisé.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    // Nombre de rayons distincts affichés. À un seul, les en-têtes de section ne
    // servent à rien : qui ne classe pas ses articles ne voit aucun changement.
    Q_PROPERTY(int aisleCount READ aisleCount NOTIFY countChanged)
    // Mode de classement de la liste (répliqué). false = par rayon puis position
    // manuelle (défaut) ; true = position manuelle seule, sans sections, glisser
    // réordonne librement sans reclasser. Le changer le publie aux autres participants.
    Q_PROPERTY(bool manualSort READ manualSort WRITE setManualSort NOTIFY manualSortChanged)
    // Les rayons proposés à la saisie : ceux d'origine, puis ceux que les participants
    // ont inventés dans cette liste. NOTIFY et pas CONSTANT : un rayon créé doit
    // apparaître aussitôt dans le choix, ici comme chez les autres.
    Q_PROPERTY(QStringList aisleNames READ aisleNames NOTIFY aisleNamesChanged)

public:
    enum Roles {
        ItemIdRole = Qt::UserRole + 1,
        NameRole,
        QtyRole,
        NoteRole,
        DoneRole,
        DoneAtRole,
        CreatedRole,
        // Nom du participant qui a ajouté la tâche ("vous" pour soi, vide si inconnu).
        ByNameRole,
        AisleRole,
        // Photos de la tâche : shas256 hex des blobs, séparés par des espaces
        // ("" = pas d'image). L'ordre est celui d'ajout (« avant » puis « après »).
        ImageRole,
        // Progression des étapes de la tâche (0/0 si elle n'en a pas).
        StepsTotalRole,
        StepsDoneRole,
    };

    // Rayons d'origine, dans l'ordre où on traverse un magasin. C'est cet ordre qui trie
    // les sections : la liste se lit comme un parcours, pas comme un alphabet.
    // Un rayon vide (« ») veut dire « non classé » et passe en dernier.
    static const QStringList &defaultAisles();

    explicit ItemModel(QObject *parent = nullptr);

    // Load items from DB for a given list. `parentId` non vide = mode étapes : le
    // modèle n'affiche que les étapes de cette tâche, dans leur ordre manuel (les
    // étapes cochées gardent leur place : la séquence reste lisible).
    void load(store::Database &db, const std::string &listId, const std::string &deviceId,
              const std::string &parentId = {});

    // Tâche parente du mode étapes ("" = mode liste).
    std::string parentId() const { return m_parentId; }

    // Snapshot d'une tâche pour la page de détail : { itemId, name, note, aisle,
    // image, done, doneAt, created, byName }. Map vide si la tâche n'existe plus.
    Q_INVOKABLE QVariantMap get(const QString &itemId) const;

    // ---- QAbstractListModel ----
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;
    int doneCount() const;
    int aisleCount() const;
    // Rayons d'origine + rayons inventés dans cette liste (déduits des articles :
    // aucun champ de plus à synchroniser, et un rayon dont plus rien ne dépend
    // disparaît de lui-même).
    QStringList aisleNames() const;

    QString filter() const { return m_filter; }
    void    setFilter(const QString &filter);

    bool manualSort() const { return m_manualSort; }
    // Choisir le mode : écrit le champ répliqué (LWW), retrie, et publie aux autres.
    // Un merge distant, lui, arrive par un rechargement complet du modèle (load).
    void setManualSort(bool manual);

    // Nom d'un article déjà présent (comparaison insensible à la casse et aux
    // espaces), ou chaîne vide. Sert à prévenir avant d'ajouter un doublon.
    Q_INVOKABLE QString existingName(const QString &name) const;

public slots:
    void addItem(const QString &name, const QString &qty,
                 const QString &note = {}, const QString &aisle = {});

    // Déplacer une ligne visible : `from` et `to` sont des index de la vue. Franchir
    // une frontière de section change aussi le rayon de l'article — c'est le geste
    // naturel pour classer (« ce truc va à la crèmerie »), et il n'y a pas d'autre
    // interprétation raisonnable d'un dépôt sous un autre en-tête.
    void moveItem(int from, int to);

    // Ranger un article dans un rayon (chaîne vide = non classé).
    void setAisle(const QString &itemId, const QString &aisle);
    void toggleDone(const QString &itemId);
    void removeItem(const QString &itemId);
    // Suppression groupée (mode sélection) : un seul lot publié, pas N événements.
    void removeItems(const QStringList &itemIds);
    // Édition LWW du nom, de la quantité et de la description. Les champs inchangés
    // gardent leur version : réécrire une valeur identique ferait gagner ce champ
    // contre une modification distante concurrente qu'on n'a pas encore reçue.
    void editItem(const QString &itemId, const QString &name,
                  const QString &qty, const QString &note, const QString &aisle);

    // Photos de la tâche : le champ LWW `image` porte la liste des shas (séparés
    // par des espaces), dans l'ordre d'ajout — « avant » puis « après ». Les blobs
    // sont déjà en base (l'appelant s'en charge). Chaque écriture publie.
    void addItemImage(const QString &itemId, const QString &sha);
    void removeItemImage(const QString &itemId, const QString &sha);

    // Tout remettre à faire (la liste se refait), ou retirer ce qui est fait. Sans
    // ces deux-là, une tâche cochée reste barrée à l'écran pour toujours, et il faut
    // la traiter une par une.
    void uncheckAll();
    void removeDone();

signals:
    void countChanged();
    void doneCountChanged();
    void filterChanged();
    void aisleNamesChanged();
    void manualSortChanged();
    // Un article a été ajouté par l'utilisateur (pas un merge ni un import) : les
    // favoris fréquents ont changé.
    void itemAdded();
    // Emitted after any local write (addItem, toggleDone, removeItem, editItem).
    void localChanged(const std::string& listId);
    // Le contenu a été rechargé ou modifié : la page de détail relit sa tâche (get()).
    void refreshed();

private:
    // Sorted visible rows (del=false only, sorted as §7).
    struct Row {
        core::Item item;
    };

    // Find visible row index by itemId (-1 if not found).
    int findRow(const QString &itemId) const;

    // Tri d'affichage. En mode catégorie : catégorie, puis à faire avant fait, puis
    // position manuelle — une tâche cochée descend au bas de SA catégorie. En mode
    // manuel : la catégorie est ignorée, tri par à-faire/fait puis position manuelle.
    // En mode étapes : position manuelle seule — une étape cochée garde sa place,
    // la séquence des étapes reste lisible.
    static bool rowLessThan(const Row &a, const Row &b, bool manual, bool steps);

    // Rang du rayon dans le parcours. Rayon inconnu ou vide → après tous les autres.
    static int aisleRank(const std::string &aisle);

    // Full reload from m_items into m_rows (sorted, filtre appliqué).
    void rebuildRows();

    bool matchesFilter(const core::Item &item) const;

    // Ré-espace les positions d'un groupe quand l'intervalle entre deux voisins est
    // épuisé. Le groupe = même état (pris / à acheter) et, en mode rayon, même rayon ;
    // en mode manuel, tous rayons confondus.
    void renumber(const std::string &aisle, bool done);

    // Tombstone d'une tâche + cascade sur ses étapes (une étape sans tâche n'a plus
    // de sens, et resurgirait au merge si on ne la supprimait que localement).
    void tombstone(const std::string &itemId, const core::Ver &ver, int64_t nowMs);

    // Liste des photos d'une tâche, et écriture de la liste complète (champ LWW).
    QStringList imageList(const QString &itemId) const;
    void writeImageList(const QString &itemId, const QStringList &shas);

    // Recalcule la progression des étapes par tâche (mode liste uniquement).
    void rebuildStepCounts();

    bool stepsMode() const { return !m_parentId.empty(); }

    store::Database *m_db     = nullptr;
    std::string      m_listId;
    std::string      m_deviceId;
    // Mode étapes : itemId de la tâche dont on affiche les étapes ("" = mode liste).
    std::string      m_parentId;
    QString          m_filter;
    bool             m_manualSort = false;  // mode de classement de la liste chargée

    // Progression des étapes par tâche (mode liste) : itemId → {total, faites}.
    std::map<std::string, std::pair<int, int>> m_stepCounts;

    // deviceId → nom affiché, pour dire qui a ajouté quoi. Chargé avec la liste.
    std::map<std::string, QString> m_memberNames;

    // Full set including tombstones (authoritative).
    std::vector<core::Item> m_items;

    // Visible, sorted view (no del=true).
    std::vector<Row> m_rows;
};

} // namespace app
