#include "database.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QDebug>
#include <QUuid>

namespace store {

namespace {
// Helpers to convert between std::string and QString.
inline QString qs(const std::string& s) { return QString::fromStdString(s); }
inline std::string ss(const QString& s) { return s.toStdString(); }

// Cast int64_t to qlonglong to resolve QVariant overload ambiguity.
inline qlonglong ll(int64_t v) { return static_cast<qlonglong>(v); }

core::Ver verFromCols(int64_t l, const QString& d)
{
    return {l, d.toStdString()};
}
} // anonymous namespace

Database::~Database()
{
    close();
}

bool Database::open(const QString& path)
{
    // Each instance gets a unique connection name so tests can open multiple DBs.
    m_connectionName = QStringLiteral("colotaches_") +
                       QUuid::createUuid().toString(QUuid::WithoutBraces);

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);

    if (!m_db.open()) {
        qWarning() << "Database::open failed:" << m_db.lastError().text();
        return false;
    }

    // Enable WAL mode for better concurrency (§6).
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("PRAGMA foreign_keys=ON"));

    return createSchema();
}

void Database::close()
{
    if (m_db.isOpen())
        m_db.close();
    if (!m_connectionName.isEmpty()) {
        QSqlDatabase::removeDatabase(m_connectionName);
        m_connectionName.clear();
    }
}

bool Database::createSchema()
{
    QSqlQuery q(m_db);

    // §6 exact schema.
    const QStringList ddl = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS lists ("
            "  list_id TEXT PRIMARY KEY,"
            "  key BLOB,"
            "  title TEXT,"
            "  title_ver_l INT,"
            "  title_ver_d TEXT,"
            "  lamport INT,"
            "  last_sync INT,"
            "  created INT,"
            "  group_id TEXT,"         // groupe local, '' = non rangé
            "  sort_mode TEXT,"        // mode de classement répliqué, '' = par rayon
            "  sort_mode_l INT,"
            "  sort_mode_d TEXT,"
            "  list_order INT"         // position manuelle locale dans le groupe
            ")"),
        QStringLiteral(
            // Groupes locaux : jamais synchronisés, propres à l'appareil.
            "CREATE TABLE IF NOT EXISTS groups ("
            "  group_id TEXT PRIMARY KEY,"
            "  name TEXT,"
            "  sort_order INT,"
            "  created INT"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS items ("
            "  list_id TEXT,"
            "  item_id TEXT,"
            "  created INT,"
            "  by TEXT,"
            "  parent_id TEXT,"        // étape : tâche parente, '' = premier niveau
            "  name TEXT,  name_l INT,  name_d TEXT,"
            "  qty  TEXT,  qty_l  INT,  qty_d  TEXT,"
            "  note TEXT,  note_l INT,  note_d TEXT,"
            "  aisle TEXT, aisle_l INT, aisle_d TEXT,"
            "  image TEXT, image_l INT, image_d TEXT,"
            "  due INT,   due_l INT,   due_d TEXT,"
            "  due_note TEXT, due_note_l INT, due_note_d TEXT,"
            // « order » est un mot réservé SQL : la colonne s'appelle sort_order.
            "  sort_order INT, sort_order_l INT, sort_order_d TEXT,"
            "  done INT,   done_l INT,  done_d TEXT,"
            "  done_at INT,"
            "  del  INT,   del_l  INT,  del_d  TEXT,"
            "  touched INT,"
            "  PRIMARY KEY(list_id, item_id)"
            ")"),
        QStringLiteral(
            // Blobs des photos de tâches, adressés par contenu (sha256 hex). Partagés
            // entre listes et entre tâches : le champ image des items s'y réfère.
            "CREATE TABLE IF NOT EXISTS images ("
            "  sha TEXT PRIMARY KEY,"
            "  data BLOB,"
            "  created INT"
            ")"),
        QStringLiteral(
            // Historique local des tâches faites. Survit au retrait de la tâche et au
            // GC des tombstones ; jamais synchronisé (chaque appareil garde sa trace).
            "CREATE TABLE IF NOT EXISTS history ("
            "  list_id TEXT,"
            "  item_id TEXT,"
            "  name TEXT,"
            "  aisle TEXT,"
            "  done_at INT,"
            "  by_name TEXT,"
            "  PRIMARY KEY(list_id, item_id, done_at)"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS members ("
            "  list_id TEXT,"
            "  device_id TEXT,"
            "  name TEXT,"
            "  ver_l INT,"
            "  ver_d TEXT,"
            "  PRIMARY KEY(list_id, device_id)"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS outbox ("
            "  rowid INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  list_id TEXT,"
            "  event_json TEXT,"
            "  created INT"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS seen_events ("
            "  event_id TEXT PRIMARY KEY,"
            "  seen INT"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS settings ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT"
            ")"),
        QStringLiteral(
            // Favoris locaux : articles fréquemment ajoutés, appris à l'usage. Purement
            // local (habitude propre à l'appareil), jamais synchronisé. La clé est le
            // nom, insensible à la casse (« Lait » et « lait » = même favori).
            "CREATE TABLE IF NOT EXISTS favorites ("
            "  name TEXT PRIMARY KEY COLLATE NOCASE,"
            "  qty TEXT,"
            "  aisle TEXT,"
            "  uses INT,"
            "  last_used INT,"
            "  pinned INT"
            ")"),
        QStringLiteral(
            // Mémoire des rayons : à quel rayon appartient un mot d'article. Clé = premier
            // mot du nom, en minuscules (« pain », « PAIN », « pain de mie » → « pain »),
            // pour qu'un nouvel article retombe dans le rayon déjà utilisé. Local.
            "CREATE TABLE IF NOT EXISTS aisle_memory ("
            "  word TEXT PRIMARY KEY,"
            "  aisle TEXT,"
            "  last_used INT"
            ")"),
    };

    for (const QString& stmt : ddl) {
        if (!q.exec(stmt)) {
            qWarning() << "createSchema error:" << q.lastError().text() << stmt;
            return false;
        }
    }
    return migrateSchema();
}

// Les bases déjà installées ont été créées par une version antérieure : CREATE TABLE
// IF NOT EXISTS ne les touche plus, il faut ajouter les colonnes manquantes à la main.
// SQLite n'a pas d'ADD COLUMN IF NOT EXISTS → on lit d'abord les colonnes existantes.
bool Database::migrateSchema()
{
    QSqlQuery q(m_db);

    QStringList existing;
    if (!q.exec(QStringLiteral("PRAGMA table_info(items)"))) {
        qWarning() << "migrateSchema: PRAGMA failed:" << q.lastError().text();
        return false;
    }
    while (q.next())
        existing << q.value(1).toString();

    // (colonne, définition). Une note absente vaut "" en version {0,""} : toute note
    // réelle la bat au merge LWW, donc la migration ne perd rien.
    const QList<QPair<QString, QString>> columns = {
        { QStringLiteral("note"),    QStringLiteral("note TEXT DEFAULT ''") },
        { QStringLiteral("note_l"),  QStringLiteral("note_l INT DEFAULT 0") },
        { QStringLiteral("note_d"),  QStringLiteral("note_d TEXT DEFAULT ''") },
        // 0 = date de cochage inconnue (article coché avant cette version) : l'UI
        // affiche « coché » sans date plutôt qu'une date inventée.
        { QStringLiteral("done_at"), QStringLiteral("done_at INT DEFAULT 0") },
        { QStringLiteral("aisle"),   QStringLiteral("aisle TEXT DEFAULT ''") },
        { QStringLiteral("aisle_l"), QStringLiteral("aisle_l INT DEFAULT 0") },
        { QStringLiteral("aisle_d"), QStringLiteral("aisle_d TEXT DEFAULT ''") },
        { QStringLiteral("sort_order"),   QStringLiteral("sort_order INT DEFAULT 0") },
        { QStringLiteral("sort_order_l"), QStringLiteral("sort_order_l INT DEFAULT 0") },
        { QStringLiteral("sort_order_d"), QStringLiteral("sort_order_d TEXT DEFAULT ''") },
        // Étapes et photos : absents des bases antérieures. '' = pas d'étape parente /
        // pas d'image, en version {0,''} — toute valeur réelle les bat au merge.
        { QStringLiteral("parent_id"), QStringLiteral("parent_id TEXT DEFAULT ''") },
        { QStringLiteral("image"),   QStringLiteral("image TEXT DEFAULT ''") },
        { QStringLiteral("image_l"), QStringLiteral("image_l INT DEFAULT 0") },
        { QStringLiteral("image_d"), QStringLiteral("image_d TEXT DEFAULT ''") },
        // Échéance facultative : 0 = « pas de date », jamais une date inventée.
        { QStringLiteral("due"),   QStringLiteral("due INT DEFAULT 0") },
        { QStringLiteral("due_l"), QStringLiteral("due_l INT DEFAULT 0") },
        { QStringLiteral("due_d"), QStringLiteral("due_d TEXT DEFAULT ''") },
        // Échéance texte : absente des bases antérieures → "" en {0,""}.
        { QStringLiteral("due_note"),   QStringLiteral("due_note TEXT DEFAULT ''") },
        { QStringLiteral("due_note_l"), QStringLiteral("due_note_l INT DEFAULT 0") },
        { QStringLiteral("due_note_d"), QStringLiteral("due_note_d TEXT DEFAULT ''") },
    };

    const bool hadOrder = existing.contains(QStringLiteral("sort_order"));

    for (const auto& [name, def] : columns) {
        if (existing.contains(name)) continue;
        if (!q.exec(QStringLiteral("ALTER TABLE items ADD COLUMN ") + def)) {
            qWarning() << "migrateSchema: ADD COLUMN" << name
                       << "failed:" << q.lastError().text();
            return false;
        }
    }

    // Position initiale = date de création : l'ordre affiché ne change pas à la mise à
    // jour, et les valeurs sont assez espacées (des millisecondes) pour qu'on puisse
    // toujours se glisser entre deux voisins sans renuméroter la liste.
    if (!hadOrder) {
        if (!q.exec(QStringLiteral("UPDATE items SET sort_order = created"
                                   " WHERE sort_order IS NULL OR sort_order = 0"))) {
            qWarning() << "migrateSchema: backfill sort_order failed:"
                       << q.lastError().text();
            return false;
        }
    }

    // La colonne group_id des listes : ajoutée aux bases d'avant les groupes.
    QStringList listCols;
    if (!q.exec(QStringLiteral("PRAGMA table_info(lists)"))) {
        qWarning() << "migrateSchema: PRAGMA lists failed:" << q.lastError().text();
        return false;
    }
    while (q.next())
        listCols << q.value(1).toString();
    if (!listCols.contains(QStringLiteral("group_id"))) {
        if (!q.exec(QStringLiteral("ALTER TABLE lists ADD COLUMN group_id TEXT DEFAULT ''"))) {
            qWarning() << "migrateSchema: ADD COLUMN group_id failed:"
                       << q.lastError().text();
            return false;
        }
    }

    // Colonnes du mode de classement répliqué : ajoutées aux bases d'avant cette version.
    // Absent = '' en version {0,''} : tout choix réel le bat au merge LWW, rien n'est perdu.
    const QList<QPair<QString, QString>> listSortCols = {
        { QStringLiteral("sort_mode"),   QStringLiteral("sort_mode TEXT DEFAULT ''") },
        { QStringLiteral("sort_mode_l"), QStringLiteral("sort_mode_l INT DEFAULT 0") },
        { QStringLiteral("sort_mode_d"), QStringLiteral("sort_mode_d TEXT DEFAULT ''") },
    };
    for (const auto& [name, def] : listSortCols) {
        if (listCols.contains(name)) continue;
        if (!q.exec(QStringLiteral("ALTER TABLE lists ADD COLUMN ") + def)) {
            qWarning() << "migrateSchema: ADD COLUMN" << name
                       << "failed:" << q.lastError().text();
            return false;
        }
    }

    // Position manuelle des listes : ajoutée aux bases antérieures. Initialisée sur
    // `created` pour que l'ordre affiché ne bouge pas à la mise à jour.
    if (!listCols.contains(QStringLiteral("list_order"))) {
        if (!q.exec(QStringLiteral("ALTER TABLE lists ADD COLUMN list_order INT DEFAULT 0"))) {
            qWarning() << "migrateSchema: ADD COLUMN list_order failed:"
                       << q.lastError().text();
            return false;
        }
        if (!q.exec(QStringLiteral("UPDATE lists SET list_order = created"
                                   " WHERE list_order IS NULL OR list_order = 0"))) {
            qWarning() << "migrateSchema: backfill list_order failed:"
                       << q.lastError().text();
            return false;
        }
    }
    return true;
}

// --- Lists ---

bool Database::createList(const core::ListMeta& meta)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO lists"
        " (list_id, key, title, title_ver_l, title_ver_d, lamport, last_sync, created,"
        "  sort_mode, sort_mode_l, sort_mode_d, list_order)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(qs(meta.listId));
    q.addBindValue(QByteArray(reinterpret_cast<const char*>(meta.key.data()),
                              static_cast<int>(meta.key.size())));
    q.addBindValue(qs(meta.title));
    q.addBindValue(ll(meta.titleVer.lamport));
    q.addBindValue(qs(meta.titleVer.deviceId));
    q.addBindValue(ll(meta.lamport));
    q.addBindValue(ll(meta.lastSync));
    q.addBindValue(ll(meta.created));
    q.addBindValue(qs(meta.sortMode));
    q.addBindValue(ll(meta.sortModeVer.lamport));
    q.addBindValue(qs(meta.sortModeVer.deviceId));
    // Position par défaut = date de création : une liste neuve se range en fin de son
    // groupe (created > tous les list_order rétro-remplis sur des créations passées).
    q.addBindValue(ll(meta.listOrder != 0 ? meta.listOrder : meta.created));

    if (!q.exec()) {
        qWarning() << "createList error:" << q.lastError().text();
        return false;
    }
    return true;
}

bool Database::updateListTitle(const std::string& listId,
                               const std::string& title,
                               const core::Ver& ver)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE lists SET title = ?, title_ver_l = ?, title_ver_d = ?"
        " WHERE list_id = ?"));
    q.addBindValue(qs(title));
    q.addBindValue(ll(ver.lamport));
    q.addBindValue(qs(ver.deviceId));
    q.addBindValue(qs(listId));

    if (!q.exec()) {
        qWarning() << "updateListTitle error:" << q.lastError().text();
        return false;
    }
    return true;
}

bool Database::updateListSortMode(const std::string& listId,
                                  const std::string& sortMode,
                                  const core::Ver& ver)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE lists SET sort_mode = ?, sort_mode_l = ?, sort_mode_d = ?"
        " WHERE list_id = ?"));
    q.addBindValue(qs(sortMode));
    q.addBindValue(ll(ver.lamport));
    q.addBindValue(qs(ver.deviceId));
    q.addBindValue(qs(listId));

    if (!q.exec()) {
        qWarning() << "updateListSortMode error:" << q.lastError().text();
        return false;
    }
    return true;
}

std::vector<core::ListMeta> Database::getLists()
{
    std::vector<core::ListMeta> result;
    QSqlQuery q(m_db);
    // Trié par position manuelle : c'est l'ordre que le tri (stable) par groupe, côté
    // modèle, conserve à l'intérieur de chaque groupe. `created` départage les ex æquo.
    q.exec(QStringLiteral(
        "SELECT list_id, key, title, title_ver_l, title_ver_d, lamport, last_sync, created,"
        " group_id, sort_mode, sort_mode_l, sort_mode_d, list_order"
        " FROM lists ORDER BY list_order ASC, created ASC"));
    while (q.next()) {
        core::ListMeta m;
        m.listId        = ss(q.value(0).toString());
        QByteArray blob = q.value(1).toByteArray();
        m.key.assign(reinterpret_cast<const uint8_t*>(blob.constData()),
                     reinterpret_cast<const uint8_t*>(blob.constData()) + blob.size());
        m.title         = ss(q.value(2).toString());
        m.titleVer      = verFromCols(q.value(3).toLongLong(), q.value(4).toString());
        m.lamport       = q.value(5).toLongLong();
        m.lastSync      = q.value(6).toLongLong();
        m.created       = q.value(7).toLongLong();
        m.groupId       = ss(q.value(8).toString());
        m.sortMode      = ss(q.value(9).toString());
        m.sortModeVer   = verFromCols(q.value(10).toLongLong(), q.value(11).toString());
        m.listOrder     = q.value(12).toLongLong();
        result.push_back(std::move(m));
    }
    return result;
}

bool Database::setListGroup(const std::string& listId, const std::string& groupId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE lists SET group_id = ? WHERE list_id = ?"));
    q.addBindValue(qs(groupId));
    q.addBindValue(qs(listId));
    if (!q.exec()) {
        qWarning() << "setListGroup error:" << q.lastError().text();
        return false;
    }
    return true;
}

bool Database::setListOrder(const std::string& listId, int64_t order)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE lists SET list_order = ? WHERE list_id = ?"));
    q.addBindValue(ll(order));
    q.addBindValue(qs(listId));
    if (!q.exec()) {
        qWarning() << "setListOrder error:" << q.lastError().text();
        return false;
    }
    return true;
}

// --- Groups ---

bool Database::createGroup(const std::string& groupId, const std::string& name, int64_t sortOrder)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO groups (group_id, name, sort_order, created)"
        " VALUES (?, ?, ?, ?)"));
    q.addBindValue(qs(groupId));
    q.addBindValue(qs(name));
    q.addBindValue(ll(sortOrder));
    q.addBindValue(ll(QDateTime::currentMSecsSinceEpoch()));
    if (!q.exec()) {
        qWarning() << "createGroup error:" << q.lastError().text();
        return false;
    }
    return true;
}

bool Database::renameGroup(const std::string& groupId, const std::string& name)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE groups SET name = ? WHERE group_id = ?"));
    q.addBindValue(qs(name));
    q.addBindValue(qs(groupId));
    if (!q.exec()) {
        qWarning() << "renameGroup error:" << q.lastError().text();
        return false;
    }
    return true;
}

bool Database::deleteGroup(const std::string& groupId)
{
    if (!m_db.transaction()) return false;
    QSqlQuery q(m_db);

    // Les listes du groupe ne sont PAS supprimées : elles reviennent simplement
    // « non rangées ». Supprimer un dossier ne doit pas emporter son contenu.
    q.prepare(QStringLiteral("UPDATE lists SET group_id = '' WHERE group_id = ?"));
    q.addBindValue(qs(groupId));
    if (!q.exec()) { m_db.rollback(); return false; }

    q.prepare(QStringLiteral("DELETE FROM groups WHERE group_id = ?"));
    q.addBindValue(qs(groupId));
    if (!q.exec()) { m_db.rollback(); return false; }

    return m_db.commit();
}

std::vector<Database::Group> Database::getGroups()
{
    std::vector<Group> result;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "SELECT group_id, name, sort_order FROM groups ORDER BY sort_order ASC, created ASC"));
    while (q.next())
        result.push_back({ ss(q.value(0).toString()),
                           ss(q.value(1).toString()),
                           q.value(2).toLongLong() });
    return result;
}

// --- Favorites ---

bool Database::recordFavoriteUse(const std::string& name, const std::string& qty,
                                 const std::string& aisle, int64_t nowMs)
{
    if (name.empty()) return false;

    QSqlQuery q(m_db);
    // Compteur incrémenté ; on garde la DERNIÈRE quantité / le dernier rayon non vides
    // comme valeurs par défaut à la prochaine ré-utilisation.
    q.prepare(QStringLiteral(
        "INSERT INTO favorites (name, qty, aisle, uses, last_used, pinned)"
        " VALUES (?, ?, ?, 1, ?, 0)"
        " ON CONFLICT(name) DO UPDATE SET"
        "  uses = uses + 1,"
        "  last_used = excluded.last_used,"
        "  qty   = CASE WHEN excluded.qty   <> '' THEN excluded.qty   ELSE favorites.qty   END,"
        "  aisle = CASE WHEN excluded.aisle <> '' THEN excluded.aisle ELSE favorites.aisle END"));
    q.addBindValue(qs(name));
    q.addBindValue(qs(qty));
    q.addBindValue(qs(aisle));
    q.addBindValue(ll(nowMs));
    if (!q.exec()) {
        qWarning() << "recordFavoriteUse error:" << q.lastError().text();
        return false;
    }
    return true;
}

std::vector<Database::Favorite> Database::getFavorites(int limit)
{
    std::vector<Favorite> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT name, qty, aisle, uses, pinned FROM favorites"
        " ORDER BY pinned DESC, uses DESC, last_used DESC LIMIT ?"));
    q.addBindValue(limit);
    if (!q.exec()) {
        qWarning() << "getFavorites error:" << q.lastError().text();
        return result;
    }
    while (q.next())
        result.push_back({ ss(q.value(0).toString()),
                           ss(q.value(1).toString()),
                           ss(q.value(2).toString()),
                           q.value(3).toLongLong(),
                           q.value(4).toInt() != 0 });
    return result;
}

bool Database::setFavoritePinned(const std::string& name, bool pinned)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE favorites SET pinned = ? WHERE name = ?"));
    q.addBindValue(pinned ? 1 : 0);
    q.addBindValue(qs(name));
    if (!q.exec()) {
        qWarning() << "setFavoritePinned error:" << q.lastError().text();
        return false;
    }
    return true;
}

bool Database::removeFavorite(const std::string& name)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM favorites WHERE name = ?"));
    q.addBindValue(qs(name));
    if (!q.exec()) {
        qWarning() << "removeFavorite error:" << q.lastError().text();
        return false;
    }
    return true;
}

// --- Aisle memory ---

namespace {
// Premier mot du nom, en minuscules (Unicode) : clé de la mémoire des rayons.
// « pain aux noix » et « PAIN » partagent le mot « pain ».
QString firstWordLower(const std::string& name)
{
    const QString s = QString::fromStdString(name).trimmed();
    const int sp = s.indexOf(QLatin1Char(' '));
    return (sp < 0 ? s : s.left(sp)).toLower();
}
} // namespace

bool Database::recordAisleForName(const std::string& name, const std::string& aisle, int64_t nowMs)
{
    if (aisle.empty()) return false;
    const QString word = firstWordLower(name);
    if (word.isEmpty()) return false;

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO aisle_memory (word, aisle, last_used) VALUES (?, ?, ?)"
        " ON CONFLICT(word) DO UPDATE SET aisle = excluded.aisle, last_used = excluded.last_used"));
    q.addBindValue(word);
    q.addBindValue(qs(aisle));
    q.addBindValue(ll(nowMs));
    if (!q.exec()) {
        qWarning() << "recordAisleForName error:" << q.lastError().text();
        return false;
    }
    return true;
}

std::string Database::suggestAisleForName(const std::string& name)
{
    const QString word = firstWordLower(name);
    if (word.isEmpty()) return {};

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT aisle FROM aisle_memory WHERE word = ?"));
    q.addBindValue(word);
    if (q.exec() && q.next())
        return ss(q.value(0).toString());
    return {};
}

std::vector<std::string> Database::distinctItemAisles()
{
    std::vector<std::string> out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral(
            "SELECT DISTINCT aisle FROM items WHERE del = 0 AND aisle <> ''")))
        while (q.next())
            out.push_back(ss(q.value(0).toString()));
    return out;
}

std::vector<std::string> Database::distinctMemoryAisles()
{
    std::vector<std::string> out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT DISTINCT aisle FROM aisle_memory WHERE aisle <> ''")))
        while (q.next())
            out.push_back(ss(q.value(0).toString()));
    return out;
}

bool Database::renameAisleInMemory(const std::string& oldAisle, const std::string& newAisle)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE aisle_memory SET aisle = ? WHERE aisle = ?"));
    q.addBindValue(qs(newAisle));
    q.addBindValue(qs(oldAisle));
    return q.exec();
}

bool Database::forgetAisleInMemory(const std::string& aisle)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM aisle_memory WHERE aisle = ?"));
    q.addBindValue(qs(aisle));
    return q.exec();
}

std::optional<core::ListMeta> Database::getList(const std::string& listId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT list_id, key, title, title_ver_l, title_ver_d, lamport, last_sync, created,"
        " group_id, sort_mode, sort_mode_l, sort_mode_d, list_order"
        " FROM lists WHERE list_id = ?"));
    q.addBindValue(qs(listId));
    if (!q.exec() || !q.next())
        return std::nullopt;

    core::ListMeta m;
    m.listId        = ss(q.value(0).toString());
    QByteArray blob = q.value(1).toByteArray();
    m.key.assign(reinterpret_cast<const uint8_t*>(blob.constData()),
                 reinterpret_cast<const uint8_t*>(blob.constData()) + blob.size());
    m.title         = ss(q.value(2).toString());
    m.titleVer      = verFromCols(q.value(3).toLongLong(), q.value(4).toString());
    m.lamport       = q.value(5).toLongLong();
    m.lastSync      = q.value(6).toLongLong();
    m.created       = q.value(7).toLongLong();
    m.groupId       = ss(q.value(8).toString());
    m.sortMode      = ss(q.value(9).toString());
    m.sortModeVer   = verFromCols(q.value(10).toLongLong(), q.value(11).toString());
    m.listOrder     = q.value(12).toLongLong();
    return m;
}

// --- Items ---

bool Database::upsertItem(const core::Item& item)
{
    if (!m_db.transaction()) {
        qWarning() << "upsertItem: cannot start transaction";
        return false;
    }

    QSqlQuery q(m_db);
    const int64_t nowMs = QDateTime::currentMSecsSinceEpoch();
    q.prepare(QStringLiteral(
        "INSERT INTO items"
        " (list_id, item_id, created, by, parent_id,"
        "  name, name_l, name_d,"
        "  qty,  qty_l,  qty_d,"
        "  note, note_l, note_d,"
        "  aisle, aisle_l, aisle_d,"
        "  image, image_l, image_d,"
        "  due,   due_l,   due_d,"
        "  due_note, due_note_l, due_note_d,"
        "  sort_order, sort_order_l, sort_order_d,"
        "  done, done_l, done_d, done_at,"
        "  del,  del_l,  del_d,"
        "  touched)"
        " VALUES (?,?,?,?,?, ?,?,?, ?,?,?, ?,?,?, ?,?,?, ?,?,?, ?,?,?, ?,?,?, ?,?,?, ?,?,?,?, ?,?,?, ?)"
        " ON CONFLICT(list_id, item_id) DO UPDATE SET"
        "  name   = excluded.name,   name_l = excluded.name_l,   name_d = excluded.name_d,"
        "  qty    = excluded.qty,    qty_l  = excluded.qty_l,    qty_d  = excluded.qty_d,"
        "  note   = excluded.note,   note_l = excluded.note_l,   note_d = excluded.note_d,"
        "  aisle  = excluded.aisle,  aisle_l = excluded.aisle_l, aisle_d = excluded.aisle_d,"
        "  image  = excluded.image,  image_l = excluded.image_l, image_d = excluded.image_d,"
        "  due    = excluded.due,    due_l  = excluded.due_l,    due_d  = excluded.due_d,"
        "  due_note   = excluded.due_note,"
        "  due_note_l = excluded.due_note_l,"
        "  due_note_d = excluded.due_note_d,"
        "  sort_order   = excluded.sort_order,"
        "  sort_order_l = excluded.sort_order_l,"
        "  sort_order_d = excluded.sort_order_d,"
        "  done   = excluded.done,   done_l = excluded.done_l,   done_d = excluded.done_d,"
        "  done_at = excluded.done_at,"
        "  del    = excluded.del,    del_l  = excluded.del_l,    del_d  = excluded.del_d,"
        "  touched = excluded.touched"));
    q.addBindValue(qs(item.listId));
    q.addBindValue(qs(item.itemId));
    q.addBindValue(ll(item.created));
    q.addBindValue(qs(item.by));
    q.addBindValue(qs(item.parentId));
    q.addBindValue(qs(item.name));
    q.addBindValue(ll(item.nameVer.lamport));
    q.addBindValue(qs(item.nameVer.deviceId));
    q.addBindValue(qs(item.qty));
    q.addBindValue(ll(item.qtyVer.lamport));
    q.addBindValue(qs(item.qtyVer.deviceId));
    q.addBindValue(qs(item.note));
    q.addBindValue(ll(item.noteVer.lamport));
    q.addBindValue(qs(item.noteVer.deviceId));
    q.addBindValue(qs(item.aisle));
    q.addBindValue(ll(item.aisleVer.lamport));
    q.addBindValue(qs(item.aisleVer.deviceId));
    q.addBindValue(qs(item.image));
    q.addBindValue(ll(item.imageVer.lamport));
    q.addBindValue(qs(item.imageVer.deviceId));
    q.addBindValue(ll(item.due));
    q.addBindValue(ll(item.dueVer.lamport));
    q.addBindValue(qs(item.dueVer.deviceId));
    q.addBindValue(qs(item.dueNote));
    q.addBindValue(ll(item.dueNoteVer.lamport));
    q.addBindValue(qs(item.dueNoteVer.deviceId));
    q.addBindValue(ll(item.order));
    q.addBindValue(ll(item.orderVer.lamport));
    q.addBindValue(qs(item.orderVer.deviceId));
    q.addBindValue(item.done ? 1 : 0);
    q.addBindValue(ll(item.doneVer.lamport));
    q.addBindValue(qs(item.doneVer.deviceId));
    q.addBindValue(ll(item.doneAt));
    q.addBindValue(item.del ? 1 : 0);
    q.addBindValue(ll(item.delVer.lamport));
    q.addBindValue(qs(item.delVer.deviceId));
    q.addBindValue(ll(nowMs));

    if (!q.exec()) {
        qWarning() << "upsertItem error:" << q.lastError().text();
        m_db.rollback();
        return false;
    }

    if (!m_db.commit()) {
        qWarning() << "upsertItem commit error:" << m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

std::vector<core::Item> Database::getItems(const std::string& listId)
{
    std::vector<core::Item> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT item_id, created, by, parent_id,"
        "  name, name_l, name_d,"
        "  qty,  qty_l,  qty_d,"
        "  note, note_l, note_d,"
        "  aisle, aisle_l, aisle_d,"
        "  image, image_l, image_d,"
        "  due,   due_l,   due_d,"
        "  due_note, due_note_l, due_note_d,"
        "  sort_order, sort_order_l, sort_order_d,"
        "  done, done_l, done_d, done_at,"
        "  del,  del_l,  del_d,"
        "  touched"
        " FROM items WHERE list_id = ? ORDER BY created ASC"));
    q.addBindValue(qs(listId));
    if (!q.exec()) {
        qWarning() << "getItems error:" << q.lastError().text();
        return result;
    }
    while (q.next()) {
        core::Item it;
        it.listId      = listId;
        it.itemId      = ss(q.value(0).toString());
        it.created     = q.value(1).toLongLong();
        it.by          = ss(q.value(2).toString());
        it.parentId    = ss(q.value(3).toString());
        it.name        = ss(q.value(4).toString());
        it.nameVer     = verFromCols(q.value(5).toLongLong(), q.value(6).toString());
        it.qty         = ss(q.value(7).toString());
        it.qtyVer      = verFromCols(q.value(8).toLongLong(), q.value(9).toString());
        it.note        = ss(q.value(10).toString());
        it.noteVer     = verFromCols(q.value(11).toLongLong(), q.value(12).toString());
        it.aisle       = ss(q.value(13).toString());
        it.aisleVer    = verFromCols(q.value(14).toLongLong(), q.value(15).toString());
        it.image       = ss(q.value(16).toString());
        it.imageVer    = verFromCols(q.value(17).toLongLong(), q.value(18).toString());
        it.due         = q.value(19).toLongLong();
        it.dueVer      = verFromCols(q.value(20).toLongLong(), q.value(21).toString());
        it.dueNote     = ss(q.value(22).toString());
        it.dueNoteVer  = verFromCols(q.value(23).toLongLong(), q.value(24).toString());
        it.order       = q.value(25).toLongLong();
        it.orderVer    = verFromCols(q.value(26).toLongLong(), q.value(27).toString());
        it.done        = q.value(28).toInt() != 0;
        it.doneVer     = verFromCols(q.value(29).toLongLong(), q.value(30).toString());
        it.doneAt      = q.value(31).toLongLong();
        it.del         = q.value(32).toInt() != 0;
        it.delVer      = verFromCols(q.value(33).toLongLong(), q.value(34).toString());
        it.touched     = q.value(35).toLongLong();

        // Base d'avant la position manuelle, ou article créé par un pair qui l'ignore :
        // la date de création fait une position de départ cohérente avec l'affichage.
        if (it.order == 0)
            it.order = it.created;
        result.push_back(std::move(it));
    }
    return result;
}

// --- Historique ---

bool Database::appendHistory(const std::string& listId, const HistoryEntry& e)
{
    if (e.name.empty() || e.doneAt <= 0) return false;
    QSqlQuery q(m_db);
    // OR IGNORE : le même cochage revenu d'un second relais ne fait pas de doublon
    // (clé = liste + item + date de cochage).
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO history (list_id, item_id, name, aisle, done_at, by_name)"
        " VALUES (?, ?, ?, ?, ?, ?)"));
    q.addBindValue(qs(listId));
    q.addBindValue(qs(e.itemId));
    q.addBindValue(qs(e.name));
    q.addBindValue(qs(e.aisle));
    q.addBindValue(ll(e.doneAt));
    q.addBindValue(qs(e.byName));
    if (!q.exec()) {
        qWarning() << "appendHistory error:" << q.lastError().text();
        return false;
    }
    return true;
}

std::vector<Database::HistoryEntry> Database::getHistory(const std::string& listId, int limit)
{
    std::vector<HistoryEntry> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT item_id, name, aisle, done_at, by_name FROM history"
        " WHERE list_id = ? ORDER BY done_at DESC LIMIT ?"));
    q.addBindValue(qs(listId));
    q.addBindValue(limit);
    if (!q.exec()) {
        qWarning() << "getHistory error:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        HistoryEntry e;
        e.itemId = ss(q.value(0).toString());
        e.name   = ss(q.value(1).toString());
        e.aisle  = ss(q.value(2).toString());
        e.doneAt = q.value(3).toLongLong();
        e.byName = ss(q.value(4).toString());
        out.push_back(std::move(e));
    }
    return out;
}

bool Database::clearHistory(const std::string& listId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM history WHERE list_id = ?"));
    q.addBindValue(qs(listId));
    if (!q.exec()) {
        qWarning() << "clearHistory error:" << q.lastError().text();
        return false;
    }
    return true;
}

// --- Images ---

bool Database::putImage(const std::string& sha, const QByteArray& data)
{
    if (sha.empty() || data.isEmpty()) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO images (sha, data, created) VALUES (?, ?, ?)"));
    q.addBindValue(qs(sha));
    q.addBindValue(data);
    q.addBindValue(ll(QDateTime::currentMSecsSinceEpoch()));
    if (!q.exec()) {
        qWarning() << "putImage error:" << q.lastError().text();
        return false;
    }
    return true;
}

QByteArray Database::getImage(const std::string& sha)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT data FROM images WHERE sha = ?"));
    q.addBindValue(qs(sha));
    if (q.exec() && q.next())
        return q.value(0).toByteArray();
    return {};
}

bool Database::hasImage(const std::string& sha)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT 1 FROM images WHERE sha = ?"));
    q.addBindValue(qs(sha));
    return q.exec() && q.next();
}

// Le champ `image` d'un item est une liste de shas séparés par des espaces (une
// photo « avant », une « après »…) : l'appartenance se teste sur la liste bordée
// d'espaces, pas par égalité.
std::vector<std::string> Database::imagesReferenced(const std::string& listId)
{
    std::vector<std::string> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT b.sha FROM images b WHERE EXISTS ("
        "  SELECT 1 FROM items i WHERE i.list_id = ? AND i.del = 0 AND i.image <> ''"
        "  AND instr(' ' || i.image || ' ', ' ' || b.sha || ' ') > 0)"));
    q.addBindValue(qs(listId));
    if (q.exec())
        while (q.next())
            out.push_back(ss(q.value(0).toString()));
    return out;
}

bool Database::purgeOrphanImages()
{
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "DELETE FROM images WHERE NOT EXISTS ("
            "  SELECT 1 FROM items i WHERE i.del = 0 AND i.image <> ''"
            "  AND instr(' ' || i.image || ' ', ' ' || images.sha || ' ') > 0)"))) {
        qWarning() << "purgeOrphanImages error:" << q.lastError().text();
        return false;
    }
    return true;
}

// --- Members ---

bool Database::upsertMember(const std::string& listId,
                             const std::string& deviceId,
                             const std::string& name,
                             const core::Ver&   ver)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO members (list_id, device_id, name, ver_l, ver_d)"
        " VALUES (?, ?, ?, ?, ?)"
        " ON CONFLICT(list_id, device_id) DO UPDATE SET"
        "  name = excluded.name, ver_l = excluded.ver_l, ver_d = excluded.ver_d"));
    q.addBindValue(qs(listId));
    q.addBindValue(qs(deviceId));
    q.addBindValue(qs(name));
    q.addBindValue(ll(ver.lamport));
    q.addBindValue(qs(ver.deviceId));
    if (!q.exec()) {
        qWarning() << "upsertMember error:" << q.lastError().text();
        return false;
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> Database::getMembers(const std::string& listId)
{
    std::vector<std::pair<std::string, std::string>> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT device_id, name FROM members WHERE list_id = ?"));
    q.addBindValue(qs(listId));
    if (!q.exec()) return result;
    while (q.next())
        result.emplace_back(ss(q.value(0).toString()), ss(q.value(1).toString()));
    return result;
}

// --- Outbox ---

bool Database::outboxRemove(int64_t rowid)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM outbox WHERE rowid = ?"));
    q.addBindValue(ll(rowid));
    if (!q.exec()) {
        qWarning() << "outboxRemove error:" << q.lastError().text();
        return false;
    }
    return true;
}

bool Database::updateLastSync(const std::string& listId, int64_t ms)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE lists SET last_sync = ? WHERE list_id = ? AND last_sync < ?"));
    q.addBindValue(ll(ms));
    q.addBindValue(qs(listId));
    q.addBindValue(ll(ms));
    if (!q.exec()) {
        qWarning() << "updateLastSync error:" << q.lastError().text();
        return false;
    }
    return true;
}

bool Database::deleteList(const std::string& listId)
{
    if (!m_db.transaction()) {
        qWarning() << "deleteList: transaction failed:" << m_db.lastError().text();
        return false;
    }

    // Une seule transaction : une liste à moitié effacée laisserait des items
    // orphelins que getItems() ressusciterait à la prochaine ouverture.
    const QStringList stmts = {
        QStringLiteral("DELETE FROM items   WHERE list_id = ?"),
        QStringLiteral("DELETE FROM members WHERE list_id = ?"),
        QStringLiteral("DELETE FROM outbox  WHERE list_id = ?"),
        QStringLiteral("DELETE FROM history WHERE list_id = ?"),
        QStringLiteral("DELETE FROM lists   WHERE list_id = ?"),
    };

    for (const QString& sql : stmts) {
        QSqlQuery q(m_db);
        q.prepare(sql);
        q.addBindValue(qs(listId));
        if (!q.exec()) {
            qWarning() << "deleteList error:" << q.lastError().text();
            m_db.rollback();
            return false;
        }
    }

    if (!m_db.commit()) {
        qWarning() << "deleteList: commit failed:" << m_db.lastError().text();
        m_db.rollback();
        return false;
    }
    return true;
}

bool Database::outboxPush(const std::string& listId, const std::string& eventJson)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO outbox (list_id, event_json, created) VALUES (?, ?, ?)"));
    q.addBindValue(qs(listId));
    q.addBindValue(qs(eventJson));
    q.addBindValue(ll(QDateTime::currentMSecsSinceEpoch()));
    if (!q.exec()) {
        qWarning() << "outboxPush error:" << q.lastError().text();
        return false;
    }
    return true;
}

std::optional<std::pair<int64_t, std::string>> Database::outboxPop(const std::string& listId)
{
    // Find oldest entry.
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT rowid, event_json FROM outbox WHERE list_id = ? ORDER BY rowid ASC LIMIT 1"));
    q.addBindValue(qs(listId));
    if (!q.exec() || !q.next())
        return std::nullopt;

    int64_t rowid       = q.value(0).toLongLong();
    std::string payload = ss(q.value(1).toString());

    QSqlQuery del(m_db);
    del.prepare(QStringLiteral("DELETE FROM outbox WHERE rowid = ?"));
    del.addBindValue(ll(rowid));
    del.exec();

    return std::make_pair(rowid, payload);
}

std::vector<std::pair<int64_t, std::string>> Database::outboxPeekAll(const std::string& listId)
{
    std::vector<std::pair<int64_t, std::string>> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT rowid, event_json FROM outbox WHERE list_id = ? ORDER BY rowid ASC"));
    q.addBindValue(qs(listId));
    if (!q.exec()) return result;
    while (q.next())
        result.emplace_back(q.value(0).toLongLong(), ss(q.value(1).toString()));
    return result;
}

int Database::outboxCount()
{
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM outbox")) || !q.next()) {
        qWarning() << "outboxCount error:" << q.lastError().text();
        return 0;
    }
    return q.value(0).toInt();
}

// --- Seen events ---

bool Database::markEventSeen(const std::string& eventId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO seen_events (event_id, seen) VALUES (?, ?)"));
    q.addBindValue(qs(eventId));
    q.addBindValue(ll(QDateTime::currentMSecsSinceEpoch()));
    if (!q.exec()) {
        qWarning() << "markEventSeen error:" << q.lastError().text();
        return false;
    }
    return true;
}

bool Database::isEventSeen(const std::string& eventId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT 1 FROM seen_events WHERE event_id = ?"));
    q.addBindValue(qs(eventId));
    return q.exec() && q.next();
}

bool Database::purgeSeenBefore(int64_t cutoffMs)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM seen_events WHERE seen < ?"));
    q.addBindValue(ll(cutoffMs));
    if (!q.exec()) {
        qWarning() << "purgeSeenBefore error:" << q.lastError().text();
        return false;
    }
    return true;
}

// --- Settings ---

std::optional<std::string> Database::getSetting(const std::string& key)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    q.addBindValue(qs(key));
    if (!q.exec() || !q.next())
        return std::nullopt;
    return ss(q.value(0).toString());
}

bool Database::setSetting(const std::string& key, const std::string& value)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO settings (key, value) VALUES (?, ?)"
        " ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(qs(key));
    q.addBindValue(qs(value));
    if (!q.exec()) {
        qWarning() << "setSetting error:" << q.lastError().text();
        return false;
    }
    return true;
}

// --- Lamport clock ---

int64_t Database::bumpLamport(const std::string& listId, int64_t atLeast)
{
    if (!m_db.transaction()) {
        qWarning() << "bumpLamport: cannot start transaction";
        return -1;
    }

    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT lamport FROM lists WHERE list_id = ?"));
    q.addBindValue(qs(listId));
    if (!q.exec() || !q.next()) {
        m_db.rollback();
        return -1;
    }

    int64_t current = q.value(0).toLongLong();
    int64_t next    = std::max(current + 1, atLeast);

    QSqlQuery u(m_db);
    u.prepare(QStringLiteral("UPDATE lists SET lamport = ? WHERE list_id = ?"));
    u.addBindValue(ll(next));
    u.addBindValue(qs(listId));
    if (!u.exec()) {
        qWarning() << "bumpLamport update error:" << u.lastError().text();
        m_db.rollback();
        return -1;
    }

    if (!m_db.commit()) {
        m_db.rollback();
        return -1;
    }
    return next;
}

} // namespace store
