#include "itemmodel.h"

#include <QUuid>
#include <QDateTime>
#include <algorithm>
#include <atomic>
#include <set>

namespace app {

// Monotonically increasing created timestamp: ensures items added within the same
// millisecond get distinct, ordered values (critical for deterministic sort).
static int64_t nextCreated() {
    static std::atomic<int64_t> s_last{ 0 };
    const int64_t now = QDateTime::currentMSecsSinceEpoch();
    int64_t prev = s_last.load(std::memory_order_relaxed);
    int64_t next;
    do {
        next = (now > prev) ? now : prev + 1;
    } while (!s_last.compare_exchange_weak(prev, next,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed));
    return next;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

const QStringList &ItemModel::defaultAisles() {
    // Catégories de tâches d'une maisonnée : du quotidien vers l'occasionnel, pas
    // l'alphabet. C'est cet ordre qui trie les sections de la liste.
    static const QStringList kAisles = {
        QStringLiteral("Ménage"),
        QStringLiteral("Cuisine"),
        QStringLiteral("Linge"),
        QStringLiteral("Courses"),
        QStringLiteral("Jardin"),
        QStringLiteral("Bricolage"),
        QStringLiteral("Travaux"),
        QStringLiteral("Administratif"),
    };
    return kAisles;
}

int ItemModel::aisleRank(const std::string &aisle) {
    const int known = defaultAisles().size();

    // Non classé : tout à la fin. Une tâche sans catégorie se traite quand le
    // reste est passé en revue.
    if (aisle.empty())
        return known + 1;

    const int idx = defaultAisles().indexOf(QString::fromStdString(aisle));
    // Catégorie inventée : après celles d'origine, avant les non classées. Entre
    // elles, c'est le libellé qui départage (rowLessThan) — donc le même ordre
    // partout, sans avoir à synchroniser un classement des catégories.
    return idx >= 0 ? idx : known;
}

QStringList ItemModel::aisleNames() const {
    QStringList names = defaultAisles();

    // Les catégories inventées se déduisent des tâches : elles voyagent déjà avec
    // elles (le champ `aisle` porte le libellé), donc celle que l'un crée apparaît
    // chez l'autre sans rien ajouter au protocole.
    QStringList custom;
    for (const auto &item : m_items) {
        if (item.del || item.aisle.empty())
            continue;
        const QString aisle = QString::fromStdString(item.aisle);
        if (!names.contains(aisle) && !custom.contains(aisle))
            custom << aisle;
    }

    // Ordre alphabétique (sensible aux accents) : le même sur tous les appareils.
    std::sort(custom.begin(), custom.end(), [](const QString &a, const QString &b) {
        return QString::localeAwareCompare(a, b) < 0;
    });

    return names + custom;
}

bool ItemModel::rowLessThan(const Row &a, const Row &b, bool manual, bool steps) {
    // Mode étapes : position manuelle seule. Une étape cochée garde sa place — la
    // séquence (« démonter, poncer, repeindre ») doit rester lisible telle quelle.
    if (steps) {
        if (a.item.order != b.item.order)
            return a.item.order < b.item.order;
        return a.item.itemId < b.item.itemId;
    }

    // Mode catégorie : on regroupe d'abord par catégorie. En mode manuel, la
    // catégorie ne compte pas — la liste est une seule séquence ordonnée à la main.
    if (!manual) {
        const int rankA = aisleRank(a.item.aisle);
        const int rankB = aisleRank(b.item.aisle);
        if (rankA != rankB)
            return rankA < rankB;
        // Deux catégories de même rang mais de libellés différents (deux inconnues) :
        // les départager, sinon les sections s'entremêleraient.
        if (a.item.aisle != b.item.aisle)
            return a.item.aisle < b.item.aisle;
    }

    // L'état fait/à faire n'entre PAS dans le tri : cocher ne déplace rien, ni chez
    // soi ni chez les autres. À plusieurs sur le chantier, une liste qui se
    // réorganise à chaque cochage est illisible ; une tâche faite s'estompe, elle
    // ne bouge pas.
    if (a.item.order != b.item.order)
        return a.item.order < b.item.order;
    return a.item.itemId < b.item.itemId;   // départage stable
}

// ---------------------------------------------------------------------------
// ItemModel
// ---------------------------------------------------------------------------

ItemModel::ItemModel(QObject *parent)
    : QAbstractListModel(parent)
{}

void ItemModel::load(store::Database &db, const std::string &listId, const std::string &deviceId,
                     const std::string &parentId) {
    m_db       = &db;
    m_listId   = listId;
    m_deviceId = deviceId;
    m_parentId = parentId;

    m_memberNames.clear();
    for (const auto &[devId, name] : db.getMembers(listId))
        m_memberNames[devId] = QString::fromStdString(name);

    // Mode de classement de la liste (répliqué). "manual" = manuel seul, sinon rayon.
    const bool manual = [&]{
        auto meta = db.getList(listId);
        return meta && meta->sortMode == "manual";
    }();
    if (manual != m_manualSort) {
        m_manualSort = manual;
        emit manualSortChanged();
    }

    m_items = db.getItems(listId);
    rebuildRows();
}

void ItemModel::setManualSort(bool manual) {
    if (!m_db || manual == m_manualSort) return;

    // Écriture locale = tick du Lamport de la liste : la nouvelle version bat celle
    // qu'on connaissait et gagne le merge LWW chez les autres participants. Repasser
    // en mode rayon écrit "aisle" (et non "") pour battre un "manual" distant.
    const int64_t lamport = m_db->bumpLamport(m_listId);
    const core::Ver ver{ lamport, m_deviceId };
    m_db->updateListSortMode(m_listId, manual ? "manual" : "aisle", ver);

    m_manualSort = manual;
    emit manualSortChanged();
    rebuildRows();
    // Publier le changement de mode (comme un renommage : c'est un champ de liste).
    emit localChanged(m_listId);
}

// Filtre d'affichage : nom, quantité ou description. Insensible à la casse — on tape
// « lait », pas « Lait ».
bool ItemModel::matchesFilter(const core::Item &item) const {
    if (m_filter.isEmpty())
        return true;

    const auto has = [&](const std::string &field) {
        return QString::fromStdString(field).contains(m_filter, Qt::CaseInsensitive);
    };
    return has(item.name) || has(item.qty) || has(item.note);
}

void ItemModel::setFilter(const QString &filter) {
    if (m_filter == filter)
        return;
    m_filter = filter;
    emit filterChanged();
    rebuildRows();
}

void ItemModel::rebuildStepCounts() {
    m_stepCounts.clear();
    if (stepsMode())
        return;
    // m_items contient tout (tâches ET étapes) : la progression se lit d'une passe.
    for (const auto &item : m_items) {
        if (item.del || item.parentId.empty())
            continue;
        auto &[total, done] = m_stepCounts[item.parentId];
        ++total;
        if (item.done) ++done;
    }
}

void ItemModel::rebuildRows() {
    beginResetModel();
    m_rows.clear();

    std::vector<Row> tasks;
    for (const auto &item : m_items) {
        // La vue ne montre qu'un niveau : les tâches (parent vide), ou les étapes de
        // la tâche ouverte. m_items garde tout, y compris pour la progression.
        if (!item.del && item.parentId == m_parentId && matchesFilter(item)) {
            tasks.push_back(Row{ item });
        }
    }
    const bool manual = m_manualSort, steps = stepsMode();
    std::stable_sort(tasks.begin(), tasks.end(),
                     [=](const Row &a, const Row &b){ return rowLessThan(a, b, manual, steps); });

    if (m_showSteps && !stepsMode()) {
        // Mode « Au boulot » : chaque tâche est suivie de ses étapes, dans leur
        // ordre manuel, cochables en place. Une étape porte le rayon de section de
        // sa tâche pour rester dans la même section à l'écran.
        for (auto &t : tasks) {
            t.sectionAisle = t.item.aisle;
            const std::string taskId = t.item.itemId;
            const std::string aisle  = t.item.aisle;
            m_rows.push_back(std::move(t));

            std::vector<Row> kids;
            for (const auto &item : m_items) {
                if (!item.del && item.parentId == taskId)
                    kids.push_back(Row{ item, true, aisle });
            }
            std::stable_sort(kids.begin(), kids.end(), [](const Row &a, const Row &b) {
                if (a.item.order != b.item.order) return a.item.order < b.item.order;
                return a.item.itemId < b.item.itemId;
            });
            for (auto &k : kids)
                m_rows.push_back(std::move(k));
        }
    } else {
        m_rows = std::move(tasks);
    }

    rebuildStepCounts();
    endResetModel();
    emit countChanged();
    emit doneCountChanged();
    // Un merge distant peut apporter une catégorie qu'on ne connaissait pas.
    emit aisleNamesChanged();
    emit refreshed();
}

void ItemModel::setShowSteps(bool show) {
    if (m_showSteps == show)
        return;
    m_showSteps = show;
    emit showStepsChanged();
    rebuildRows();
}

int ItemModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_rows.size());
}

QVariant ItemModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= (int)m_rows.size())
        return {};
    const auto &row = m_rows[static_cast<size_t>(index.row())];
    switch (role) {
    case ItemIdRole:  return QString::fromStdString(row.item.itemId);
    case NameRole:    return QString::fromStdString(row.item.name);
    case QtyRole:     return QString::fromStdString(row.item.qty);
    case NoteRole:    return QString::fromStdString(row.item.note);
    case DoneRole:    return row.item.done;
    case DoneAtRole:  return static_cast<qlonglong>(row.item.doneAt);
    case CreatedRole: return static_cast<qlonglong>(row.item.created);
    // Une étape dépliée s'affiche dans la section de sa tâche, pas dans « Sans
    // catégorie » (elle n'a pas de catégorie propre).
    case AisleRole:   return QString::fromStdString(row.isStep ? row.sectionAisle
                                                               : row.item.aisle);
    case ImageRole:   return QString::fromStdString(row.item.image);
    case DueRole:     return static_cast<qlonglong>(row.item.due);
    case DueNoteRole: return QString::fromStdString(row.item.dueNote);
    case IsStepRole:  return row.isStep;
    case StepsTotalRole: {
        const auto it = m_stepCounts.find(row.item.itemId);
        return it != m_stepCounts.end() ? it->second.first : 0;
    }
    case StepsDoneRole: {
        const auto it = m_stepCounts.find(row.item.itemId);
        return it != m_stepCounts.end() ? it->second.second : 0;
    }
    case ByNameRole: {
        if (row.item.by.empty())
            return QString{};
        if (row.item.by == m_deviceId)
            return QStringLiteral("vous");
        const auto it = m_memberNames.find(row.item.by);
        // Participant jamais vu passer d'événement : mieux vaut ne rien dire que
        // d'afficher un identifiant d'appareil.
        return it != m_memberNames.end() ? it->second : QString{};
    }
    default:          return {};
    }
}

QHash<int, QByteArray> ItemModel::roleNames() const {
    return {
        { ItemIdRole,  "itemId"  },
        { NameRole,    "name"    },
        { QtyRole,     "qty"     },
        { NoteRole,    "note"    },
        { DoneRole,    "done"    },
        { DoneAtRole,  "doneAt"  },
        { CreatedRole, "created" },
        { ByNameRole,  "byName"  },
        { AisleRole,   "aisle"   },
        { ImageRole,   "image"   },
        { StepsTotalRole, "stepsTotal" },
        { StepsDoneRole,  "stepsDone"  },
        { DueRole,     "due"     },
        { DueNoteRole, "dueNote" },
        { IsStepRole,  "isStep"  },
    };
}

QVariantMap ItemModel::get(const QString &itemId) const {
    const std::string id = itemId.toStdString();
    const auto it = std::find_if(m_items.begin(), m_items.end(),
                                 [&](const core::Item &i){ return i.itemId == id; });
    if (it == m_items.end() || it->del)
        return {};

    QVariantMap m;
    m.insert(QStringLiteral("itemId"),  itemId);
    m.insert(QStringLiteral("name"),    QString::fromStdString(it->name));
    m.insert(QStringLiteral("note"),    QString::fromStdString(it->note));
    m.insert(QStringLiteral("aisle"),   QString::fromStdString(it->aisle));
    m.insert(QStringLiteral("image"),   QString::fromStdString(it->image));
    m.insert(QStringLiteral("due"),     static_cast<qlonglong>(it->due));
    m.insert(QStringLiteral("dueNote"), QString::fromStdString(it->dueNote));
    m.insert(QStringLiteral("done"),    it->done);
    m.insert(QStringLiteral("doneAt"),  static_cast<qlonglong>(it->doneAt));
    m.insert(QStringLiteral("created"), static_cast<qlonglong>(it->created));
    QString byName;
    if (it->by == m_deviceId) {
        byName = QStringLiteral("vous");
    } else if (!it->by.empty()) {
        const auto mit = m_memberNames.find(it->by);
        if (mit != m_memberNames.end()) byName = mit->second;
    }
    m.insert(QStringLiteral("byName"), byName);
    return m;
}

int ItemModel::aisleCount() const {
    // En mode manuel (et pour les étapes) pas de sections : une seule « catégorie »
    // du point de vue de la vue, pour que les en-têtes disparaissent.
    if (m_manualSort || stepsMode()) return 1;
    std::set<std::string> distinct;
    for (const auto &row : m_rows)
        distinct.insert(row.item.aisle);
    return static_cast<int>(distinct.size());
}

QString ItemModel::existingName(const QString &name) const {
    const QString needle = name.trimmed();
    if (needle.isEmpty())
        return {};

    // On cherche dans m_items et pas dans m_rows : un doublon reste un doublon même
    // si un filtre de recherche le cache à l'écran. Au même niveau seulement — une
    // étape peut porter le nom d'une tâche, et inversement.
    for (const auto &item : m_items) {
        if (item.del || item.parentId != m_parentId)
            continue;
        const QString existing = QString::fromStdString(item.name).trimmed();
        if (existing.compare(needle, Qt::CaseInsensitive) == 0)
            return existing;
    }
    return {};
}

int ItemModel::count() const {
    return static_cast<int>(m_rows.size());
}

int ItemModel::doneCount() const {
    return static_cast<int>(std::count_if(m_rows.begin(), m_rows.end(),
                                          [](const Row &r){ return r.item.done; }));
}

int ItemModel::findRow(const QString &itemId) const {
    const std::string id = itemId.toStdString();
    for (int i = 0; i < (int)m_rows.size(); ++i) {
        if (m_rows[static_cast<size_t>(i)].item.itemId == id) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Mutation slots
// ---------------------------------------------------------------------------

void ItemModel::addItem(const QString &name, const QString &qty,
                        const QString &note, const QString &aisle) {
    if (!m_db) return;

    const int64_t lamport = m_db->bumpLamport(m_listId);
    const core::Ver ver{ lamport, m_deviceId };
    const int64_t ts = nextCreated();

    core::Item item;
    item.listId  = m_listId;
    item.itemId  = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    item.created = ts;
    item.by      = m_deviceId;
    // En mode étapes, le nouvel item est une étape de la tâche ouverte.
    item.parentId = m_parentId;
    item.name    = name.toStdString();
    item.nameVer = ver;
    item.qty     = qty.toStdString();
    item.qtyVer  = ver;
    item.note    = note.trimmed().toStdString();
    item.noteVer = ver;
    // Le rayon est celui fourni par l'appelant, tel quel (la barre d'ajout le pré-remplit
    // côté UI, mais rien n'est assigné en douce : un rayon vide reste vide).
    item.aisle   = aisle.trimmed().toStdString();
    item.aisleVer = ver;
    // Position initiale = date de création : l'article se pose en bas de son groupe,
    // et l'écart avec le précédent (des millisecondes) laisse la place d'en insérer
    // d'autres entre les deux sans renuméroter.
    item.order    = ts;
    item.orderVer = ver;
    item.done    = false;
    item.doneVer = ver;
    item.doneAt  = 0;
    item.del     = false;
    item.delVer  = ver;
    item.touched = ts;

    if (!m_db->upsertItem(item)) return;

    // Apprendre l'habitude : cette tâche rejoint (ou renforce) les favoris fréquents.
    // Seulement sur un ajout manuel de tâche — ni les imports/duplications (qui
    // écrivent en base directement), ni les étapes (propres à leur tâche).
    if (!stepsMode()) {
        m_db->recordFavoriteUse(item.name, item.qty, item.aisle, ts);
        // Et mémoriser où va cette tâche, pour que les suivantes du même nom suivent.
        m_db->recordAisleForName(item.name, item.aisle, ts);
    }

    emit localChanged(m_listId);
    emit aisleNamesChanged();
    emit itemAdded();

    // Insert into m_items (full set).
    m_items.push_back(item);

    // Un filtre de recherche est actif et la nouvelle tâche n'y répond pas : elle
    // existe, mais n'a rien à faire dans la vue courante.
    if (!matchesFilter(item)) {
        emit refreshed();
        return;
    }

    // Find sorted insertion position (upper_bound to append after equals).
    Row newRow{ item };
    const bool manual = m_manualSort, steps = stepsMode();
    const auto less = [=](const Row &a, const Row &b){ return rowLessThan(a, b, manual, steps); };
    auto it = std::upper_bound(m_rows.begin(), m_rows.end(), newRow, less);
    const int pos = static_cast<int>(it - m_rows.begin());

    beginInsertRows({}, pos, pos);
    m_rows.insert(it, newRow);
    endInsertRows();
    emit countChanged();
    emit doneCountChanged();
    emit refreshed();
}

void ItemModel::toggleDone(const QString &itemId) {
    if (!m_db) return;

    const std::string id = itemId.toStdString();

    // Find in authoritative set.
    auto it = std::find_if(m_items.begin(), m_items.end(),
                           [&](const core::Item &i){ return i.itemId == id; });
    if (it == m_items.end()) return;

    const int64_t lamport = m_db->bumpLamport(m_listId);
    const core::Ver ver{ lamport, m_deviceId };
    const int64_t now = QDateTime::currentMSecsSinceEpoch();

    const bool newDone = !it->done;
    it->done    = newDone;
    it->doneVer = ver;
    it->doneAt  = newDone ? now : 0;  // décoché → plus de date de cochage
    it->touched = now;

    if (!m_db->upsertItem(*it)) return;

    // Lignes dont l'apparence peut changer (sans changer de place).
    std::vector<std::string> dirtyIds{ id };
    // Tâche dont la progression d'étapes doit être rafraîchie à l'écran.
    std::string stepsTaskId;

    // Historique : chaque tâche (pas les étapes) cochée ici laisse une trace locale
    // durable — elle survivra à « Retirer les tâches faites ». « vous » : c'est ce
    // que l'UI affiche pour soi-même partout ailleurs.
    if (newDone && it->parentId.empty()) {
        m_db->appendHistory(m_listId,
                            { it->itemId, it->name, it->aisle, now, "vous" });
    }

    if (it->parentId.empty()) {
        // Cocher (ou décocher) une tâche emporte ses étapes : l'état de la tâche
        // EST celui de l'ensemble — une tâche « faite » aux étapes ouvertes ne
        // voudrait rien dire.
        stepsTaskId = id;
        for (auto &s : m_items) {
            if (s.del || s.parentId != id || s.done == newDone) continue;
            s.done    = newDone;
            s.doneVer = ver;
            s.doneAt  = newDone ? now : 0;
            s.touched = now;
            m_db->upsertItem(s);
            dirtyIds.push_back(s.itemId);
        }
    } else {
        // Dernière étape cochée → la tâche se coche toute seule ; une étape
        // rouverte rouvre la tâche. La tâche reflète toujours ses étapes.
        stepsTaskId = it->parentId;
        auto p = std::find_if(m_items.begin(), m_items.end(),
                              [&](const core::Item &i){ return i.itemId == it->parentId; });
        if (p != m_items.end() && !p->del) {
            bool any = false, all = true;
            for (const auto &s : m_items) {
                if (s.del || s.parentId != p->itemId) continue;
                any = true;
                if (!s.done) { all = false; break; }
            }
            const bool target = any && all;
            if (any && p->done != target) {
                p->done    = target;
                p->doneVer = ver;
                p->doneAt  = target ? now : 0;
                p->touched = now;
                m_db->upsertItem(*p);
                dirtyIds.push_back(p->itemId);
                if (target && p->parentId.empty())
                    m_db->appendHistory(m_listId,
                                        { p->itemId, p->name, p->aisle, now, "vous" });
            }
        }
    }

    emit localChanged(m_listId);
    emit aisleNamesChanged();

    // Cocher ne déplace pas la ligne : l'état fait/à faire ne fait pas partie du
    // tri (rowLessThan). La ligne change d'apparence, pas de place — même quand
    // la cascade touche aussi les étapes ou la tâche parente.
    rebuildStepCounts();
    bool stepsRolesEmitted = false;
    for (const auto &dirtyId : dirtyIds) {
        const int pos = findRow(QString::fromStdString(dirtyId));
        if (pos < 0) continue;
        auto src = std::find_if(m_items.begin(), m_items.end(),
                                [&](const core::Item &i){ return i.itemId == dirtyId; });
        if (src == m_items.end()) continue;
        m_rows[static_cast<size_t>(pos)].item = *src;
        QList<int> roles{ DoneRole, DoneAtRole };
        if (dirtyId == stepsTaskId) {
            roles << StepsTotalRole << StepsDoneRole;
            stepsRolesEmitted = true;
        }
        const QModelIndex idx = index(pos);
        emit dataChanged(idx, idx, roles);
    }
    // Progression des étapes sur la tâche, même si son `done` n'a pas bougé
    // (ex. 1ʳᵉ étape sur 3 cochée).
    if (!stepsTaskId.empty() && !stepsRolesEmitted) {
        const int pos = findRow(QString::fromStdString(stepsTaskId));
        if (pos >= 0) {
            const QModelIndex idx = index(pos);
            emit dataChanged(idx, idx, { StepsTotalRole, StepsDoneRole });
        }
    }

    // Le nombre de lignes n'a pas bougé, mais la progression (mode Chantier /
    // barre d'étapes) oui.
    emit doneCountChanged();
    emit refreshed();
}

void ItemModel::editItem(const QString &itemId, const QString &name,
                         const QString &qty, const QString &note, const QString &aisle) {
    if (!m_db) return;

    const std::string id       = itemId.toStdString();
    const std::string newName  = name.trimmed().toStdString();
    const std::string newQty   = qty.trimmed().toStdString();
    const std::string newNote  = note.trimmed().toStdString();
    const std::string newAisle = aisle.trimmed().toStdString();

    if (newName.empty()) return; // un article sans nom n'a rien à afficher

    auto it = std::find_if(m_items.begin(), m_items.end(),
                           [&](const core::Item &i){ return i.itemId == id; });
    if (it == m_items.end()) return;

    const bool nameChanged  = (it->name  != newName);
    const bool qtyChanged   = (it->qty   != newQty);
    const bool noteChanged  = (it->note  != newNote);
    const bool aisleChanged = (it->aisle != newAisle);
    if (!nameChanged && !qtyChanged && !noteChanged && !aisleChanged) return;

    const int64_t lamport = m_db->bumpLamport(m_listId);
    const core::Ver ver{ lamport, m_deviceId };

    if (nameChanged)  { it->name  = newName;  it->nameVer  = ver; }
    if (qtyChanged)   { it->qty   = newQty;   it->qtyVer   = ver; }
    if (noteChanged)  { it->note  = newNote;  it->noteVer  = ver; }
    if (aisleChanged) { it->aisle = newAisle; it->aisleVer = ver; }
    it->touched = QDateTime::currentMSecsSinceEpoch();

    if (!m_db->upsertItem(*it)) return;

    // Ranger un article enseigne où va son nom, pour pré-remplir les prochains.
    if (aisleChanged && !newAisle.empty())
        m_db->recordAisleForName(it->name, newAisle, it->touched);

    emit localChanged(m_listId);
    emit aisleNamesChanged();

    // Le rayon est la première clé de tri : l'article change de section, la ligne se
    // déplace. De même sous filtre, où l'édition peut la faire sortir de la recherche.
    if (aisleChanged || !m_filter.isEmpty()) {
        rebuildRows();
        return;
    }

    // Aucun de ces champs n'entre dans le tri (done, created) : la ligne ne bouge
    // pas, seul son contenu change.
    const int pos = findRow(itemId);
    if (pos >= 0) {
        m_rows[static_cast<size_t>(pos)].item = *it;
        const QModelIndex idx = index(pos);
        emit dataChanged(idx, idx, { NameRole, QtyRole, NoteRole });
    }
    emit refreshed();
}

void ItemModel::setDue(const QString &itemId, double dueMs) {
    if (!m_db) return;

    const std::string id  = itemId.toStdString();
    const int64_t     due = static_cast<int64_t>(dueMs);

    auto it = std::find_if(m_items.begin(), m_items.end(),
                           [&](const core::Item &i){ return i.itemId == id; });
    if (it == m_items.end()) return;

    // Poser une date efface l'échéance texte ; retirer la date (0) laisse dueNote.
    const bool clearNote = (due > 0);
    if (it->due == due && (!clearNote || it->dueNote.empty())) return;

    const int64_t lamport = m_db->bumpLamport(m_listId);
    const core::Ver ver{ lamport, m_deviceId };
    it->due     = due;
    it->dueVer  = ver;
    if (clearNote) {
        it->dueNote    = {};
        it->dueNoteVer = ver;
    }
    it->touched = QDateTime::currentMSecsSinceEpoch();

    if (!m_db->upsertItem(*it)) return;

    emit localChanged(m_listId);

    // L'échéance n'entre pas dans le tri : la ligne ne bouge pas.
    const int pos = findRow(itemId);
    if (pos >= 0) {
        m_rows[static_cast<size_t>(pos)].item = *it;
        const QModelIndex idx = index(pos);
        emit dataChanged(idx, idx, { DueRole, DueNoteRole });
    }
    emit refreshed();
}

void ItemModel::setDueNote(const QString &itemId, const QString &note) {
    if (!m_db) return;

    const std::string id      = itemId.toStdString();
    const std::string newNote = note.trimmed().toStdString();

    auto it = std::find_if(m_items.begin(), m_items.end(),
                           [&](const core::Item &i){ return i.itemId == id; });
    if (it == m_items.end()) return;

    // Texte non vide → effacer la date ; texte vide → seulement vider dueNote.
    const bool clearDue = !newNote.empty() && it->due != 0;
    if (it->dueNote == newNote && !clearDue) return;

    const int64_t lamport = m_db->bumpLamport(m_listId);
    const core::Ver ver{ lamport, m_deviceId };
    it->dueNote    = newNote;
    it->dueNoteVer = ver;
    if (clearDue) {
        it->due    = 0;
        it->dueVer = ver;
    }
    it->touched = QDateTime::currentMSecsSinceEpoch();

    if (!m_db->upsertItem(*it)) return;

    emit localChanged(m_listId);

    const int pos = findRow(itemId);
    if (pos >= 0) {
        m_rows[static_cast<size_t>(pos)].item = *it;
        const QModelIndex idx = index(pos);
        emit dataChanged(idx, idx, { DueNoteRole, DueRole });
    }
    emit refreshed();
}

// Écrit la nouvelle liste de photos (LWW sur le champ entier : deux ajouts
// simultanés sur deux appareils → le dernier gagne, comme pour tout champ).
void ItemModel::writeImageList(const QString &itemId, const QStringList &shas) {
    const std::string id     = itemId.toStdString();
    const std::string joined = shas.join(QLatin1Char(' ')).toStdString();

    auto it = std::find_if(m_items.begin(), m_items.end(),
                           [&](const core::Item &i){ return i.itemId == id; });
    if (it == m_items.end() || it->image == joined) return;

    const int64_t lamport = m_db->bumpLamport(m_listId);
    it->image    = joined;
    it->imageVer = { lamport, m_deviceId };
    it->touched  = QDateTime::currentMSecsSinceEpoch();

    if (!m_db->upsertItem(*it)) return;

    emit localChanged(m_listId);

    // Les photos n'entrent pas dans le tri : la ligne ne bouge pas.
    const int pos = findRow(itemId);
    if (pos >= 0) {
        m_rows[static_cast<size_t>(pos)].item = *it;
        const QModelIndex idx = index(pos);
        emit dataChanged(idx, idx, { ImageRole });
    }
    emit refreshed();
}

QStringList ItemModel::imageList(const QString &itemId) const {
    const std::string id = itemId.toStdString();
    const auto it = std::find_if(m_items.begin(), m_items.end(),
                                 [&](const core::Item &i){ return i.itemId == id; });
    if (it == m_items.end()) return {};
    return QString::fromStdString(it->image)
        .split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

void ItemModel::addItemImage(const QString &itemId, const QString &sha) {
    if (!m_db || sha.trimmed().isEmpty()) return;
    QStringList shas = imageList(itemId);
    if (shas.contains(sha)) return;   // même contenu → même photo, rien à faire
    shas.append(sha);
    writeImageList(itemId, shas);
}

void ItemModel::removeItemImage(const QString &itemId, const QString &sha) {
    if (!m_db) return;
    QStringList shas = imageList(itemId);
    if (!shas.removeAll(sha)) return;
    writeImageList(itemId, shas);
}

void ItemModel::removeItems(const QStringList &itemIds) {
    for (const QString &id : itemIds)
        removeItem(id);
}

void ItemModel::setAisle(const QString &itemId, const QString &aisle) {
    if (!m_db) return;

    const std::string id       = itemId.toStdString();
    const std::string newAisle = aisle.trimmed().toStdString();

    auto it = std::find_if(m_items.begin(), m_items.end(),
                           [&](const core::Item &i){ return i.itemId == id; });
    if (it == m_items.end() || it->aisle == newAisle) return;

    const int64_t lamport = m_db->bumpLamport(m_listId);
    it->aisle    = newAisle;
    it->aisleVer = { lamport, m_deviceId };
    it->touched  = QDateTime::currentMSecsSinceEpoch();

    if (!m_db->upsertItem(*it)) return;

    if (!newAisle.empty())
        m_db->recordAisleForName(it->name, newAisle, it->touched);

    emit localChanged(m_listId);
    emit aisleNamesChanged();
    // Le rayon est la première clé de tri : l'article change de section.
    rebuildRows();
}

// Déplacement manuel. La nouvelle position se glisse entre les deux voisins de
// destination ; c'est un simple champ LWW de plus, donc deux personnes qui réordonnent
// en même temps convergent (le dernier gagne) au lieu de se marcher dessus.
void ItemModel::moveItem(int from, int to) {
    if (!m_db) return;
    const int n = static_cast<int>(m_rows.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;

    core::Item moved = m_rows[static_cast<size_t>(from)].item;

    // Voisins à l'arrivée, une fois la ligne retirée de sa place actuelle.
    std::vector<Row> without = m_rows;
    without.erase(without.begin() + from);

    const Row *before = (to > 0) ? &without[static_cast<size_t>(to - 1)] : nullptr;
    const Row *after  = (to < static_cast<int>(without.size()))
                        ? &without[static_cast<size_t>(to)] : nullptr;

    // En mode rayon, l'article prend le rayon de la ligne qu'on a survolée pour le
    // déposer — c'est elle que le doigt désigne. Selon le sens du geste, cette ligne se
    // retrouve au-dessus ou en dessous de la place visée : en descendant, on vient se
    // poser APRÈS elle (elle est donc `before`) ; en montant, on vient prendre sa place
    // (elle est donc `after`). En mode manuel, glisser NE reclasse pas : le rayon ne
    // bouge pas, on ne fait que déplacer dans la séquence unique.
    const Row *hovered = (from < to) ? before : after;
    const std::string targetAisle = (m_manualSort || stepsMode())
        ? moved.aisle
        : (hovered ? hovered->item.aisle : moved.aisle);

    // Les voisins qui comptent pour la position sont ceux du même groupe : même
    // catégorie en mode catégorie, toute la liste en mode manuel / étapes. Le tri
    // les sépare avant de regarder la position. L'état fait/à faire ne compte pas :
    // il n'est pas trié.
    const auto sameGroup = [&](const Row *r) {
        if (stepsMode()) return r != nullptr;
        return r && (m_manualSort || r->item.aisle == targetAisle);
    };
    const int64_t lo = sameGroup(before) ? before->item.order : 0;
    const int64_t hi = sameGroup(after)  ? after->item.order  : 0;

    int64_t order;
    if (lo && hi)
        order = lo + (hi - lo) / 2;          // entre les deux
    else if (lo)
        order = lo + 1000;                   // en fin de groupe
    else if (hi)
        order = hi - 1000;                   // en tête de groupe
    else
        order = moved.created;               // seul de son groupe

    // Intervalle épuisé (des milliers de déplacements au même endroit) : renuméroter
    // le groupe à grands pas plutôt que d'écraser silencieusement l'ordre voulu.
    if (lo && hi && (order == lo || order == hi)) {
        renumber(targetAisle);
        moveItem(from, to);   // les positions sont ré-espacées : rejouer le geste
        return;
    }

    const int64_t lamport = m_db->bumpLamport(m_listId);
    const core::Ver ver{ lamport, m_deviceId };
    const int64_t now = QDateTime::currentMSecsSinceEpoch();

    auto it = std::find_if(m_items.begin(), m_items.end(),
                           [&](const core::Item &i){ return i.itemId == moved.itemId; });
    if (it == m_items.end()) return;

    it->order    = order;
    it->orderVer = ver;
    const bool aisleChanged = (it->aisle != targetAisle);
    if (aisleChanged) {
        it->aisle    = targetAisle;
        it->aisleVer = ver;
    }
    it->touched = now;

    if (!m_db->upsertItem(*it)) return;

    // Glisser une tâche dans une catégorie enseigne aussi où va son nom.
    if (aisleChanged && !targetAisle.empty())
        m_db->recordAisleForName(it->name, targetAisle, now);

    emit localChanged(m_listId);
    emit aisleNamesChanged();

    // Déplacement de ligne, surtout pas beginResetModel : une reconstruction détruirait
    // tous les délégués — dont celui que le doigt est en train de glisser.
    // La position calculée place l'article exactement à `to` selon le tri : on peut
    // donc déplacer la ligne directement, sans retrier.
    const int dest = (from < to) ? to + 1 : to;   // Qt insère AVANT `dest`
    beginMoveRows({}, from, from, {}, dest);
    Row row = m_rows[static_cast<size_t>(from)];
    row.item = *it;
    m_rows.erase(m_rows.begin() + from);
    m_rows.insert(m_rows.begin() + to, row);
    endMoveRows();

    const QModelIndex idx = index(to);
    emit dataChanged(idx, idx, { AisleRole });
}

// Ré-espace les positions d'un groupe (même catégorie ; toute la liste en mode
// manuel / étapes) par pas de 1000.
void ItemModel::renumber(const std::string &aisle) {
    if (!m_db) return;

    const int64_t lamport = m_db->bumpLamport(m_listId);
    const core::Ver ver{ lamport, m_deviceId };

    int64_t next = 1000;
    for (auto &row : m_rows) {
        // En mode manuel / étapes la catégorie ne délimite pas le groupe.
        // Le statut `done` n'entre pas dans la définition du groupe.
        if (!stepsMode() && !m_manualSort && row.item.aisle != aisle)
            continue;
        auto it = std::find_if(m_items.begin(), m_items.end(),
                               [&](const core::Item &i){ return i.itemId == row.item.itemId; });
        if (it == m_items.end()) continue;
        it->order    = next;
        it->orderVer = ver;
        m_db->upsertItem(*it);
        next += 1000;
    }
    rebuildRows();
}

void ItemModel::uncheckAll() {
    if (!m_db) return;

    // Un seul tick de Lamport pour tout le lot : c'est une seule intention (« on
    // recommence la liste »), et le SyncEngine n'en publiera qu'un delta.
    const int64_t lamport = m_db->bumpLamport(m_listId);
    const core::Ver ver{ lamport, m_deviceId };
    const int64_t now = QDateTime::currentMSecsSinceEpoch();

    bool any = false;
    for (auto &item : m_items) {
        if (item.del || !item.done)
            continue;
        // En mode étapes, ne toucher qu'aux étapes de la tâche ouverte (m_items
        // contient toute la liste). En mode liste : tout, étapes comprises — « on
        // recommence » vaut pour les tâches ET leurs étapes.
        if (stepsMode() && item.parentId != m_parentId)
            continue;
        item.done    = false;
        item.doneVer = ver;
        item.doneAt  = 0;
        item.touched = now;
        if (m_db->upsertItem(item))
            any = true;
    }

    if (!any) return;

    emit localChanged(m_listId);
    emit aisleNamesChanged();
    // Tout a changé de camp et donc de place dans le tri : reconstruire est plus
    // simple, et plus sûr, que d'orchestrer N déplacements de lignes.
    rebuildRows();
}

// Tombstone d'un item + cascade sur ses étapes. Ne publie ni ne retrie : l'appelant
// regroupe (un seul localChanged / rebuild pour tout le lot).
void ItemModel::tombstone(const std::string &itemId, const core::Ver &ver, int64_t nowMs) {
    for (auto &item : m_items) {
        if (item.del)
            continue;
        if (item.itemId != itemId && item.parentId != itemId)
            continue;
        // Une étape sans tâche n'a plus de sens — et sans tombstone répliqué, la
        // copie des autres la ferait réapparaître au prochain merge.
        item.del     = true;
        item.delVer  = ver;
        item.touched = nowMs;
        m_db->upsertItem(item);
    }
}

void ItemModel::removeDone() {
    if (!m_db) return;

    const int64_t lamport = m_db->bumpLamport(m_listId);
    const core::Ver ver{ lamport, m_deviceId };
    const int64_t now = QDateTime::currentMSecsSinceEpoch();

    // En mode liste : les tâches faites partent avec toutes leurs étapes (faites ou
    // non). Les étapes cochées d'une tâche encore à faire restent — c'est de la
    // progression, pas du travail terminé. En mode étapes : les étapes faites de la
    // tâche ouverte seulement.
    bool any = false;
    for (auto &item : m_items) {
        if (item.del || !item.done || item.parentId != m_parentId)
            continue;
        tombstone(item.itemId, ver, now);
        any = true;
    }

    if (!any) return;

    emit localChanged(m_listId);
    emit aisleNamesChanged();
    rebuildRows();
}

void ItemModel::removeItem(const QString &itemId) {
    if (!m_db) return;

    const std::string id = itemId.toStdString();

    // Find in authoritative set.
    auto it = std::find_if(m_items.begin(), m_items.end(),
                           [&](const core::Item &i){ return i.itemId == id; });
    if (it == m_items.end()) return;

    const int64_t lamport = m_db->bumpLamport(m_listId);
    const core::Ver ver{ lamport, m_deviceId };
    const int64_t now = QDateTime::currentMSecsSinceEpoch();

    // Cascade : supprimer une tâche emporte ses étapes.
    tombstone(id, ver, now);

    emit localChanged(m_listId);
    emit aisleNamesChanged();

    // Remove from visible rows.
    const int pos = findRow(itemId);
    if (pos >= 0) {
        beginRemoveRows({}, pos, pos);
        m_rows.erase(m_rows.begin() + static_cast<size_t>(pos));
        endRemoveRows();
        emit countChanged();
        emit doneCountChanged();
    }
    rebuildStepCounts();
    emit refreshed();
}

} // namespace app
