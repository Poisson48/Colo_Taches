#include <QtTest>
#include <QTemporaryDir>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "../src/store/database.h"
#include "../src/core/types.h"

using namespace store;
using namespace core;

class DatabaseTest : public QObject
{
    Q_OBJECT

private:
    // Build a minimal ListMeta for tests.
    ListMeta makeList(const std::string& id = "list-A")
    {
        ListMeta m;
        m.listId   = id;
        m.key      = std::vector<uint8_t>(32, 0xAB);
        m.title    = "Courses";
        m.titleVer = {1, "dev-A"};
        m.lamport  = 1;
        m.lastSync = 0;
        m.created  = 1000;
        return m;
    }

    // Build a complete Item with all fields versioned.
    Item makeItem(const std::string& listId   = "list-A",
                  const std::string& itemId   = "item-1",
                  const std::string& name     = "Lait",
                  int64_t            nameLamp = 2)
    {
        Item it;
        it.listId  = listId;
        it.itemId  = itemId;
        it.created = 2000;
        it.by      = "dev-A";
        it.name    = name;
        it.nameVer = {nameLamp, "dev-A"};
        it.qty     = "1L";
        it.qtyVer  = {3, "dev-B"};
        it.done    = false;
        it.doneVer = {4, "dev-C"};
        it.del     = false;
        it.delVer  = {5, "dev-D"};
        return it;
    }

private slots:

    // 1. Schema creation — open must succeed and all tables must exist.
    void testSchemaCreation()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));

        // Create a list: if schema is wrong this will fail.
        QVERIFY(db.createList(makeList()));
        auto lists = db.getLists();
        QCOMPARE(lists.size(), size_t(1));
    }

    void testOutboxTargetedRemove()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));

        QVERIFY(db.outboxPush("l1", "evA"));
        QVERIFY(db.outboxPush("l1", "evB"));
        QVERIFY(db.outboxPush("l1", "evC"));

        // Remove the middle entry (out-of-order ack): neighbours must survive.
        auto entries = db.outboxPeekAll("l1");
        QCOMPARE(entries.size(), size_t(3));
        QVERIFY(db.outboxRemove(entries[1].first));

        entries = db.outboxPeekAll("l1");
        QCOMPARE(entries.size(), size_t(2));
        QCOMPARE(entries[0].second, std::string("evA"));
        QCOMPARE(entries[1].second, std::string("evC"));
    }

    void testUpdateLastSync()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));

        core::ListMeta meta = makeList();
        QVERIFY(db.createList(meta));

        QVERIFY(db.updateLastSync(meta.listId, 5000));
        QCOMPARE(db.getList(meta.listId)->lastSync, int64_t(5000));
        // Only moves forward.
        QVERIFY(db.updateLastSync(meta.listId, 4000));
        QCOMPARE(db.getList(meta.listId)->lastSync, int64_t(5000));
    }

    void testUpdateListTitle()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));

        core::ListMeta meta = makeList();
        QVERIFY(db.createList(meta));

        core::Ver newVer{meta.titleVer.lamport + 5, "dev-remote"};
        QVERIFY(db.updateListTitle(meta.listId, "Titre mergé", newVer));

        auto got = db.getList(meta.listId);
        QVERIFY(got.has_value());
        QCOMPARE(got->title, std::string("Titre mergé"));
        QCOMPARE(got->titleVer.lamport, newVer.lamport);
        QCOMPARE(got->titleVer.deviceId, newVer.deviceId);
    }

    // 2. Full round-trip: every field of an Item survives write→read.
    void testItemRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));
        QVERIFY(db.createList(makeList()));

        Item original = makeItem();
        original.done    = true;
        original.doneVer = {10, "dev-X"};
        original.del     = true;
        original.delVer  = {11, "dev-Y"};
        QVERIFY(db.upsertItem(original));

        auto items = db.getItems("list-A");
        QCOMPARE(items.size(), size_t(1));
        const Item& got = items[0];

        QCOMPARE(got.listId,         original.listId);
        QCOMPARE(got.itemId,         original.itemId);
        QCOMPARE(got.created,        original.created);
        QCOMPARE(got.by,             original.by);
        QCOMPARE(got.name,           original.name);
        QCOMPARE(got.nameVer.lamport, original.nameVer.lamport);
        QCOMPARE(got.nameVer.deviceId, original.nameVer.deviceId);
        QCOMPARE(got.qty,            original.qty);
        QCOMPARE(got.qtyVer.lamport,  original.qtyVer.lamport);
        QCOMPARE(got.qtyVer.deviceId, original.qtyVer.deviceId);
        QCOMPARE(got.done,           original.done);
        QCOMPARE(got.doneVer.lamport, original.doneVer.lamport);
        QCOMPARE(got.doneVer.deviceId, original.doneVer.deviceId);
        QCOMPARE(got.del,            original.del);
        QCOMPARE(got.delVer.lamport,  original.delVer.lamport);
        QCOMPARE(got.delVer.deviceId, original.delVer.deviceId);
    }

    // 3. Upsert is idempotent — inserting the same item twice yields one row.
    void testUpsertIdempotent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));
        QVERIFY(db.createList(makeList()));

        Item it = makeItem();
        QVERIFY(db.upsertItem(it));
        QVERIFY(db.upsertItem(it)); // second time must not fail
        QCOMPARE(db.getItems("list-A").size(), size_t(1));
    }

    // 4. Outbox is FIFO.
    void testOutboxFIFO()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));
        QVERIFY(db.createList(makeList()));

        QVERIFY(db.outboxPush("list-A", "event-1"));
        QVERIFY(db.outboxPush("list-A", "event-2"));
        QVERIFY(db.outboxPush("list-A", "event-3"));

        // peekAll sees all three in insertion order.
        auto all = db.outboxPeekAll("list-A");
        QCOMPARE(all.size(), size_t(3));
        QCOMPARE(all[0].second, std::string("event-1"));
        QCOMPARE(all[1].second, std::string("event-2"));
        QCOMPARE(all[2].second, std::string("event-3"));

        // pop removes oldest first.
        auto first = db.outboxPop("list-A");
        QVERIFY(first.has_value());
        QCOMPARE(first->second, std::string("event-1"));

        auto second = db.outboxPop("list-A");
        QVERIFY(second.has_value());
        QCOMPARE(second->second, std::string("event-2"));

        QCOMPARE(db.outboxPeekAll("list-A").size(), size_t(1));

        // Pop the last one.
        auto third = db.outboxPop("list-A");
        QVERIFY(third.has_value());
        QCOMPARE(third->second, std::string("event-3"));

        // Queue is now empty.
        QVERIFY(!db.outboxPop("list-A").has_value());
    }

    // 5. seen_events: mark, query, purge.
    void testSeenEvents()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));

        QVERIFY(!db.isEventSeen("evt-X"));
        QVERIFY(db.markEventSeen("evt-X"));
        QVERIFY(db.isEventSeen("evt-X"));

        // markEventSeen is idempotent (INSERT OR IGNORE).
        QVERIFY(db.markEventSeen("evt-X"));
        QVERIFY(db.isEventSeen("evt-X"));

        // Purge with a very old cutoff — keeps evt-X (seen ~now).
        QVERIFY(db.purgeSeenBefore(1000));
        QVERIFY(db.isEventSeen("evt-X"));

        // Purge with far-future cutoff — removes evt-X.
        QVERIFY(db.purgeSeenBefore(
            QDateTime::currentMSecsSinceEpoch() + 1000000LL));
        QVERIFY(!db.isEventSeen("evt-X"));
    }

    // 6. bumpLamport: increments and respects the atLeast constraint.
    void testBumpLamport()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));
        ListMeta m = makeList();
        m.lamport  = 5;
        QVERIFY(db.createList(m));

        // Normal increment: 5 → 6.
        int64_t v = db.bumpLamport("list-A");
        QCOMPARE(v, int64_t(6));

        // atLeast larger than current+1: adopts atLeast.
        v = db.bumpLamport("list-A", 20);
        QCOMPARE(v, int64_t(20));

        // atLeast smaller than current+1: normal increment.
        v = db.bumpLamport("list-A", 1);
        QCOMPARE(v, int64_t(21));

        // Value persisted in the row.
        auto meta = db.getList("list-A");
        QVERIFY(meta.has_value());
        QCOMPARE(meta->lamport, int64_t(21));
    }

    // 7. Persistence: data survives close + reopen.
    void testPersistenceAfterReopen()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString dbPath = dir.filePath("persist.db");

        {
            Database db;
            QVERIFY(db.open(dbPath));
            QVERIFY(db.createList(makeList()));
            QVERIFY(db.upsertItem(makeItem()));
            QVERIFY(db.setSetting("deviceId", "dev-A"));
            // db destructor closes the connection.
        }

        {
            Database db2;
            QVERIFY(db2.open(dbPath));
            auto lists = db2.getLists();
            QCOMPARE(lists.size(), size_t(1));
            QCOMPARE(lists[0].listId, std::string("list-A"));

            auto items = db2.getItems("list-A");
            QCOMPARE(items.size(), size_t(1));
            QCOMPARE(items[0].name, std::string("Lait"));

            auto dev = db2.getSetting("deviceId");
            QVERIFY(dev.has_value());
            QCOMPARE(*dev, std::string("dev-A"));
        }
    }

    // 8. touched: set on insert, updated on subsequent upsert.
    void testTouchedTimestamp()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));
        QVERIFY(db.createList(makeList()));

        int64_t before = QDateTime::currentMSecsSinceEpoch();
        Item it = makeItem();
        QVERIFY(db.upsertItem(it));
        int64_t after = QDateTime::currentMSecsSinceEpoch();

        auto items = db.getItems("list-A");
        QCOMPARE(items.size(), size_t(1));
        int64_t t1 = items[0].touched;
        QVERIFY(t1 >= before);
        QVERIFY(t1 <= after);

        // Second upsert: touched must be >= t1 (wall clock advances or stays same).
        it.name    = "Lait entier";
        it.nameVer = {10, "dev-B"};
        int64_t before2 = QDateTime::currentMSecsSinceEpoch();
        QVERIFY(db.upsertItem(it));
        int64_t after2 = QDateTime::currentMSecsSinceEpoch();

        auto items2 = db.getItems("list-A");
        QCOMPARE(items2.size(), size_t(1));
        int64_t t2 = items2[0].touched;
        QVERIFY(t2 >= before2);
        QVERIFY(t2 <= after2);
        QVERIFY(t2 >= t1);
    }

    // 9. Members round-trip.
    void testMembers()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));
        QVERIFY(db.createList(makeList()));

        Ver v{1, "dev-A"};
        QVERIFY(db.upsertMember("list-A", "dev-A", "Leo", v));
        QVERIFY(db.upsertMember("list-A", "dev-B", "Marie", {2, "dev-B"}));

        auto members = db.getMembers("list-A");
        QCOMPARE(members.size(), size_t(2));

        // Update dev-A name.
        QVERIFY(db.upsertMember("list-A", "dev-A", "Leonard", {3, "dev-A"}));
        members = db.getMembers("list-A");
        QCOMPARE(members.size(), size_t(2));

        bool found = false;
        for (auto& [did, name] : members)
            if (did == "dev-A") { QCOMPARE(name, std::string("Leonard")); found = true; }
        QVERIFY(found);
    }

    // 10. deleteList: efface la liste et tout ce qui lui est rattaché, et rien d'autre.
    void testDeleteList()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));

        QVERIFY(db.createList(makeList("list-A")));
        QVERIFY(db.createList(makeList("list-B")));

        Item a = makeItem();          // rattaché à list-A
        QVERIFY(db.upsertItem(a));
        Item b = makeItem();
        b.listId = "list-B";
        b.itemId = "item-B";
        QVERIFY(db.upsertItem(b));

        QVERIFY(db.upsertMember("list-A", "dev-A", "Leo", {1, "dev-A"}));
        QVERIFY(db.upsertMember("list-B", "dev-A", "Leo", {1, "dev-A"}));
        QVERIFY(db.outboxPush("list-A", "{\"ev\":\"a\"}"));
        QVERIFY(db.outboxPush("list-B", "{\"ev\":\"b\"}"));

        QVERIFY(db.deleteList("list-A"));

        // list-A a disparu, items/membres/outbox compris.
        QVERIFY(!db.getList("list-A").has_value());
        QVERIFY(db.getItems("list-A").empty());
        QVERIFY(db.getMembers("list-A").empty());
        QVERIFY(db.outboxPeekAll("list-A").empty());

        // list-B est intacte : la suppression ne doit pas déborder.
        QVERIFY(db.getList("list-B").has_value());
        QCOMPARE(db.getItems("list-B").size(), size_t(1));
        QCOMPARE(db.getMembers("list-B").size(), size_t(1));
        QCOMPARE(db.outboxPeekAll("list-B").size(), size_t(1));

        // Une liste effacée puis recréée repart vierge (pas d'items ressuscités).
        QVERIFY(db.createList(makeList("list-A")));
        QVERIFY(db.getItems("list-A").empty());
    }

    // Mémoire des rayons : un nom retombe dans le rayon d'un article similaire déjà classé.
    void test_aisleMemory() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("am.db")));

        // Rien de connu au départ.
        QVERIFY(db.suggestAisleForName("Pain").empty());

        // « Pain » classé une fois en boulangerie.
        QVERIFY(db.recordAisleForName("Pain", "Boulangerie", 1000));

        // Toutes ces variantes partagent le premier mot « pain » → Boulangerie.
        QCOMPARE(db.suggestAisleForName("pain"),         std::string("Boulangerie")); // casse
        QCOMPARE(db.suggestAisleForName("PAIN"),         std::string("Boulangerie"));
        QCOMPARE(db.suggestAisleForName("pain de mie"),  std::string("Boulangerie")); // suffixe
        QCOMPARE(db.suggestAisleForName("Pain aux noix"),std::string("Boulangerie"));

        // Un nom sans rapport reste inconnu.
        QVERIFY(db.suggestAisleForName("Lait").empty());
        // Le second mot ne suffit pas (on ne classe que par le premier).
        QVERIFY(db.suggestAisleForName("brioche mie").empty());

        // Reclasser met à jour : « pain » suit désormais l'épicerie.
        QVERIFY(db.recordAisleForName("pain de mie", "Épicerie sucrée", 2000));
        QCOMPARE(db.suggestAisleForName("Pain"), std::string("Épicerie sucrée"));

        // Un rayon vide n'enseigne rien (pas d'oubli du classement précédent).
        QVERIFY(!db.recordAisleForName("Pain", "", 3000));
        QCOMPARE(db.suggestAisleForName("Pain"), std::string("Épicerie sucrée"));
    }

    // Favoris : appris à l'usage, classés par fréquence, épinglables, insensibles à la casse.
    void test_favorites() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("f.db")));

        // « Lait » ajouté trois fois (dont deux avec quantité), « Pain » une fois.
        QVERIFY(db.recordFavoriteUse("Lait", "",    "",         1000));
        QVERIFY(db.recordFavoriteUse("lait", "1 L", "Crèmerie", 2000)); // même favori (casse)
        QVERIFY(db.recordFavoriteUse("Lait", "",    "",         3000)); // qty vide → on garde "1 L"
        QVERIFY(db.recordFavoriteUse("Pain", "",    "",         4000));

        auto favs = db.getFavorites(10);
        QCOMPARE(favs.size(), size_t(2));           // Lait fusionné, pas dédoublé

        // Lait en tête (3 usages > 1), avec la dernière quantité/rayon non vides retenus.
        QCOMPARE(favs[0].name,  std::string("Lait"));
        QCOMPARE(favs[0].uses,  int64_t(3));
        QCOMPARE(favs[0].qty,   std::string("1 L"));
        QCOMPARE(favs[0].aisle, std::string("Crèmerie"));
        QCOMPARE(favs[1].name,  std::string("Pain"));

        // Épingler « Pain » le fait passer devant, malgré moins d'usages.
        QVERIFY(db.setFavoritePinned("Pain", true));
        favs = db.getFavorites(10);
        QCOMPARE(favs[0].name,   std::string("Pain"));
        QVERIFY(favs[0].pinned);

        // Retirer un favori le fait disparaître des suggestions.
        QVERIFY(db.removeFavorite("Lait"));
        favs = db.getFavorites(10);
        QCOMPARE(favs.size(), size_t(1));
        QCOMPARE(favs[0].name, std::string("Pain"));

        // La limite est respectée.
        for (int i = 0; i < 20; ++i)
            db.recordFavoriteUse(QStringLiteral("Article %1").arg(i).toStdString(), "", "", 5000 + i);
        QCOMPARE(db.getFavorites(5).size(), size_t(5));
    }

    // Groupes locaux : ranger des listes, renommer, supprimer sans perdre les listes.
    void test_groups() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("g.db")));

        QVERIFY(db.createList(makeList("list-A")));
        QVERIFY(db.createList(makeList("list-B")));

        QVERIFY(db.createGroup("grp-1", "Maison", 1000));
        QVERIFY(db.createGroup("grp-2", "Boulot", 2000));
        QCOMPARE(db.getGroups().size(), size_t(2));
        QCOMPARE(db.getGroups().front().name, std::string("Maison")); // trié par sort_order

        // Ranger list-A dans Maison ; getLists remonte le group_id.
        QVERIFY(db.setListGroup("list-A", "grp-1"));
        std::string groupOfA, groupOfB = "sentinel";
        for (const auto &m : db.getLists()) {
            if (m.listId == "list-A") groupOfA = m.groupId;
            if (m.listId == "list-B") groupOfB = m.groupId;
        }
        QCOMPARE(groupOfA, std::string("grp-1"));
        QCOMPARE(groupOfB, std::string(""));   // list-B reste non rangée

        QVERIFY(db.renameGroup("grp-1", "Maison & jardin"));
        QCOMPARE(db.getGroups().front().name, std::string("Maison & jardin"));

        // Supprimer un groupe ne supprime pas ses listes : elles redeviennent non rangées.
        QVERIFY(db.deleteGroup("grp-1"));
        QCOMPARE(db.getGroups().size(), size_t(1));
        QVERIFY(db.getList("list-A").has_value());
        for (const auto &m : db.getLists())
            if (m.listId == "list-A")
                QCOMPARE(m.groupId, std::string(""));
    }

    // Une base créée par une version antérieure n'a pas la colonne group_id : la
    // migration doit l'ajouter, et les listes rester intactes.
    void test_migration_addsGroupId() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("legacy-lists.db");
        {
            QSqlDatabase old = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                         QStringLiteral("legacyLists"));
            old.setDatabaseName(path);
            QVERIFY(old.open());
            QSqlQuery q(old);
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE lists (list_id TEXT PRIMARY KEY, key BLOB, title TEXT,"
                " title_ver_l INT, title_ver_d TEXT, lamport INT, last_sync INT, created INT)")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO lists VALUES ('list-A', X'', 'Courses', 1, 'dev-A', 1, 0, 1000)")));
            old.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("legacyLists"));

        Database db;
        QVERIFY(db.open(path));

        const auto lists = db.getLists();
        QCOMPARE(lists.size(), size_t(1));
        QCOMPARE(QString::fromStdString(lists.front().title), QStringLiteral("Courses"));
        QCOMPARE(lists.front().groupId, std::string(""));   // colonne ajoutée, vide par défaut

        // La colonne est écrivable : on peut ranger la liste tout de suite.
        QVERIFY(db.createGroup("grp-1", "Maison", 1000));
        QVERIFY(db.setListGroup("list-A", "grp-1"));
        QCOMPARE(db.getLists().front().groupId, std::string("grp-1"));
    }

    // Une base créée par une version antérieure n'a ni colonnes note*, ni done_at.
    // CREATE TABLE IF NOT EXISTS ne les ajoute pas : sans migration, toute lecture
    // d'article échoue et l'app est vide après mise à jour.
    void test_migration_fromSchemaWithoutNoteAndDoneAt() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("legacy.db");

        // --- Base « ancienne version », écrite à la main ---
        {
            QSqlDatabase old = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                         QStringLiteral("legacy"));
            old.setDatabaseName(path);
            QVERIFY(old.open());

            QSqlQuery q(old);
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE items ("
                "  list_id TEXT, item_id TEXT, created INT, by TEXT,"
                "  name TEXT, name_l INT, name_d TEXT,"
                "  qty  TEXT, qty_l  INT, qty_d  TEXT,"
                "  done INT,  done_l INT, done_d TEXT,"
                "  del  INT,  del_l  INT, del_d  TEXT,"
                "  touched INT,"
                "  PRIMARY KEY(list_id, item_id))")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO items VALUES ("
                "  'list-A', 'item-1', 1000, 'dev-A',"
                "  'Lait', 3, 'dev-A',"
                "  '1L',   3, 'dev-A',"
                "  1,      4, 'dev-A',"
                "  0,      3, 'dev-A',"
                "  5000)")));
            old.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("legacy"));

        // --- Ouverture par la version courante : la migration ajoute les colonnes ---
        Database db;
        QVERIFY(db.open(path));

        const auto items = db.getItems("list-A");
        QCOMPARE(items.size(), size_t(1));

        // Les données d'avant sont intactes…
        const core::Item &it = items.front();
        QCOMPARE(QString::fromStdString(it.name), QStringLiteral("Lait"));
        QCOMPARE(QString::fromStdString(it.qty),  QStringLiteral("1L"));
        QVERIFY(it.done);
        QCOMPARE(it.nameVer.lamport, int64_t(3));

        // …et les nouveaux champs valent « rien », pas une valeur inventée. Une note
        // en version {0,""} perd contre n'importe quelle note réelle au merge.
        QVERIFY(it.note.empty());
        QCOMPARE(it.noteVer.lamport, int64_t(0));
        QCOMPARE(it.doneAt, int64_t(0));  // coché avant la migration : date inconnue
        QVERIFY(it.aisle.empty());        // non classé, pas rangé au hasard
        QCOMPARE(it.aisleVer.lamport, int64_t(0));

        // La position est rétro-remplie sur la date de création : l'ordre affiché ne
        // change pas à la mise à jour, et les valeurs restent assez espacées pour
        // qu'on puisse toujours insérer entre deux voisins.
        QCOMPARE(it.order, it.created);

        // Les nouvelles colonnes sont bien écrivables.
        core::Item edited = it;
        edited.note    = "6 couches épaisses";
        edited.noteVer = {5, "dev-A"};
        edited.doneAt  = 1'700'000'000'000;
        QVERIFY(db.upsertItem(edited));

        const auto reread = db.getItems("list-A").front();
        QCOMPARE(QString::fromStdString(reread.note), QStringLiteral("6 couches épaisses"));
        QCOMPARE(reread.doneAt, int64_t(1'700'000'000'000));

        // Rouvrir une base déjà migrée ne doit pas retenter l'ALTER TABLE.
        db.close();
        Database again;
        QVERIFY(again.open(path));
        QCOMPARE(again.getItems("list-A").size(), size_t(1));

        // La migration a aussi ajouté étape et photo : écrivables tout de suite.
        core::Item withNew = again.getItems("list-A").front();
        withNew.image    = "sha-1";
        withNew.imageVer = {6, "dev-A"};
        QVERIFY(again.upsertItem(withNew));
        QCOMPARE(again.getItems("list-A").front().image, std::string("sha-1"));
        QCOMPARE(again.getItems("list-A").front().parentId, std::string(""));
    }

    // parent_id et image font l'aller-retour ; les blobs de photos sont adressés
    // par contenu et purgeables quand plus rien ne les référence.
    void test_stepsAndImages() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Database db;
        QVERIFY(db.open(dir.filePath("img.db")));
        QVERIFY(db.createList(makeList()));

        core::Item task = makeItem("list-A", "task-1", "Repeindre");
        task.image    = "sha-abc";
        task.imageVer = {3, "dev-A"};
        task.due      = 1722000000000LL;
        task.dueVer   = {4, "dev-A"};
        QVERIFY(db.upsertItem(task));

        core::Item step = makeItem("list-A", "step-1", "Poncer");
        step.parentId = "task-1";
        QVERIFY(db.upsertItem(step));

        const auto items = db.getItems("list-A");
        QCOMPARE(items.size(), size_t(2));
        for (const auto &it : items) {
            if (it.itemId == "task-1") {
                QCOMPARE(it.image, std::string("sha-abc"));
                QCOMPARE(it.imageVer.lamport, int64_t(3));
                QCOMPARE(it.due, int64_t(1722000000000LL));
                QCOMPARE(it.dueVer.lamport, int64_t(4));
                QCOMPARE(it.parentId, std::string(""));
            } else {
                QCOMPARE(it.parentId, std::string("task-1"));
            }
        }

        // Blob : aller-retour octet pour octet, insertion idempotente.
        const QByteArray blob("\xFF\xD8\xFF fake jpeg data", 18);
        QVERIFY(!db.hasImage("sha-abc"));
        QVERIFY(db.putImage("sha-abc", blob));
        QVERIFY(db.putImage("sha-abc", blob));   // ré-insérer : sans effet
        QVERIFY(db.hasImage("sha-abc"));
        QCOMPARE(db.getImage("sha-abc"), blob);

        // Référencée par un item vivant : le snapshot doit la republier.
        const auto refs = db.imagesReferenced("list-A");
        QCOMPARE(refs.size(), size_t(1));
        QCOMPARE(refs.front(), std::string("sha-abc"));

        // Un blob orphelin (aucun item vivant ne le référence) est purgé ; le
        // référencé survit.
        QVERIFY(db.putImage("sha-orpheline", QByteArray("x")));
        QVERIFY(db.purgeOrphanImages());
        QVERIFY(db.hasImage("sha-abc"));
        QVERIFY(!db.hasImage("sha-orpheline"));

        // La tâche supprimée (tombstone) libère son blob à la purge suivante.
        core::Item gone = task;
        gone.del    = true;
        gone.delVer = {9, "dev-A"};
        QVERIFY(db.upsertItem(gone));
        QVERIFY(db.purgeOrphanImages());
        QVERIFY(!db.hasImage("sha-abc"));
    }
};

QTEST_MAIN(DatabaseTest)
#include "tst_database.moc"
